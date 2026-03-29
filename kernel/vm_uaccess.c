#include "param.h"
#include "types.h"
#include "atomic.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "slab.h"
#include "kalloc.h"
#include "vm.h"
#include "rbtree.h"
#include "mm.h"
#include "vm_internal.h"
#include "colors.h"
#include "utils.h"

#define TRACKED_GOTO(label) \
do{ \
    goto_source_line = __LINE__; \
    goto label;   \
}while(0)
static int goto_source_line=0;
// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    //Proactive check, instead of raise page fault frequently.
    uint64 aligned_dstva=PGROUNDDOWN(dstva);
    uint8 cur_order=i_log2(aligned_dstva & -aligned_dstva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):cur_order;
    uint64 block_size=0, copy_size, cur_va=dstva, basepage_va=aligned_dstva, cur_pa;
    void *mem;
    int perm=0, locked_by_me=0;
    struct proc *cur_proc=myproc();
    pte_t *pte=NULL;
    while(len>0){
        if(basepage_va>=MAXVA)      TRACKED_GOTO(error);
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(*pte==0){
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about this uncontroled region.");
                TRACKED_GOTO(error);
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_contain_vma(cur_proc->mm, cur_va);
            if(cur_vma==NULL){
                pr_err("%llx isn't recorded at current mm_struct.", cur_va);
                TRACKED_GOTO(error);
            }
            else if((cur_vma->vm_page_prot & PTE_W)==0){
                pr_err("Corrputed file, unable to continue.");
                TRACKED_GOTO(error);
            }
            else if(cur_vma->vm_file==NULL)
                pr_info("%llx will treated as anonymous VMA.", cur_va);
            perm=cur_vma->vm_page_prot;

            cur_order=i_log2(basepage_va & -basepage_va);
            cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            block_size=1ull<<cur_order;
            uint64 empty_pte_len=0, valid_len=len;
            int cur_pte_idx=PX(found_level, basepage_va);
            for(int j=0;j<512;j++){
                if(cur_pte_idx +j >=512)    break;
                if(*(pte+j)!=0)     break;
                empty_pte_len++;
            }
            valid_len=MIN(empty_pte_len, valid_len);
            while(block_size > valid_len && block_size>PGSIZE)
                block_size/=2;
            mem=alloc_memory(block_size, GFP_ZERO);
            if(mem==0)  TRACKED_GOTO(error);
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_vma->vm_file!=NULL && vmfile_load(cur_vma, basepage_va, (uint64)mem, block_size)!=0){
                pr_err("Corrputed file, unable to continue.");
                free_pages(mem, block_size);
                TRACKED_GOTO(error);
            }

            acquire(&cur_proc->uvm_lock);
            if(mappages(&(struct map_context){
                .pagetable=pagetable,
                .start_va=basepage_va,
                .size=block_size,
                .pa=(uint64)mem,
                .xperm=perm
            })!=0){
                release(&cur_proc->uvm_lock);
                free_pages(mem, block_size);
                TRACKED_GOTO(error);
            }
            release(&cur_proc->uvm_lock);
            cur_pa=walkaddr(pagetable, basepage_va);    //update
            //A newly allocated memory region without cow, thus, physical dicountinutiy is not
            // a concern, block_size can be used directly for mapping.
        }
        else if ((*pte & PTE_V) == 0){   //Swap in fault.
            //extrace info(Swap_id) from the pte.
            //FIXME: 
            panic("swap pending implementation.");
        }
        else if ((*pte & PTE_U) == 0)    TRACKED_GOTO(error);
        else if ((*pte & PTE_W)==0){        //Valid but Unwriteable.
            //get the VMA firstly.
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about this uncontroled region.");
                TRACKED_GOTO(error);
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_contain_vma(cur_proc->mm, cur_va);
            if(cur_vma==NULL){
                pr_err("%llx isn't recorded at current mm_struct.", cur_va);
                TRACKED_GOTO(error);
            }
            if((*pte & PTE_COW) && (cur_vma->vm_flags & VM_WRITE) && (*pte & PTE_W)==0){
                cur_pa=vm_cowfault_handler(NULL, 0, 0, pte, 0);
                block_size=PGSIZE;
                if(cur_pa==0)   TRACKED_GOTO(error);
            }
            else{
                pr_err("%llx exist valid pte, but it's not writable.", cur_va);
                TRACKED_GOTO(error);
            }
            //COW.
        }
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
            //find the max contigious pte len instead of get from physical order
            // (A special case:COW may cause discontinuity)
            block_size=cross_scan_cont_map(pagetable, basepage_va, (uint64)-1);
        }
        copy_size=block_size-(cur_va-basepage_va);
        if(copy_size>len)   copy_size=len;
        memmove((void *)cur_pa+(cur_va-basepage_va), src, copy_size);
        len-=copy_size;
        src+=copy_size;
        cur_va=basepage_va+block_size;
        basepage_va=PGROUNDDOWN(cur_va);
    }
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    //Must call the TLB flush before reback to user's process,
    return 0;
error:
    pr_err("Jump from No.%d", goto_source_line);
    panic("1");
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    return -1;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success(TIPS: Not the success bytes), -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
    //Considering when user's page is missing.
    uint64 aligned_srcva=PGROUNDDOWN(srcva);
    uint8 cur_order=i_log2(aligned_srcva & -aligned_srcva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ? (MAX_ORDER+ORDER_BASE):cur_order;
    uint64 block_size, copy_size, cur_va=srcva, basepage_va=aligned_srcva, cur_pa;
    void *mem;
    int perm=0, locked_by_me=0;
    struct proc *cur_proc=myproc();
    pte_t *pte=NULL;
    if(cur_proc==NULL)  panic("Empty proc.");
    while(len>0){
        if(basepage_va>=MAXVA)      TRACKED_GOTO(error);
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(*pte==0){
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about this uncontroled region.");
                TRACKED_GOTO(error);
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_contain_vma(cur_proc->mm, cur_va);
            if(cur_vma==NULL){
                pr_err("%llx isn't record at current mm_struct.", cur_va);
                TRACKED_GOTO(error);
            }
            else if((cur_vma->vm_page_prot & PTE_R)==0){
                pr_err("%llx isn't readable.", cur_va);
                TRACKED_GOTO(error);
            }
            else if(cur_vma->vm_file==NULL)
                pr_info("%llx will Treated as anonymous VMA.",cur_va);
            perm = cur_vma->vm_page_prot;
            //Passed the pre-checks, so alloc physical(time-consuming)
            cur_order=i_log2(basepage_va & -basepage_va);
            cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            block_size=1ull<<cur_order;
            while(len < block_size && block_size>PGSIZE)
                block_size/=2;  //Before enter block is power of 2 already, divide 2 simply.
            // VM_TRACE("copyin needs alloc for basepage_va=0x%llx size=0x%llx\n", basepage_va, alloc_size);
            mem=alloc_memory(block_size, GFP_ZERO);
            if(mem==0)  TRACKED_GOTO(error);
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_vma->vm_file!=NULL && vmfile_load(cur_vma, basepage_va, (uint64)mem, block_size)!=0){
                pr_err("Corrputed file, unable to continue.");
                free_pages(mem, block_size);
                TRACKED_GOTO(error);
            }
            acquire(&cur_proc->uvm_lock);
            if(mappages(&(struct map_context){
                .pagetable=pagetable,
                .start_va=basepage_va,
                .pa=(uint64)mem,
                .size=block_size,
                .xperm=perm
            })!=0){
                release(&cur_proc->uvm_lock);
                free_pages(mem, block_size);
                TRACKED_GOTO(error);
            }
            release(&cur_proc->uvm_lock);
            cur_pa=walkaddr(pagetable, basepage_va);    //update
        }
        else if ((*pte & PTE_V) == 0){   //Swap in fault.
            //extrace info(Swap_id) from the pte.
            //FIXME: 
            panic("swap pending implementation.");
        }
        else if ((*pte & PTE_U) == 0)    TRACKED_GOTO(error);
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
            //find the max contigious pte len instead of get from physical order
            // (A special case:COW may cause discontinuity)
            block_size=cross_scan_cont_map(pagetable, basepage_va, (uint64)-1);
        }
        copy_size=block_size-(cur_va-basepage_va);
        if(copy_size>len)   copy_size=len;  
        //modified the size to reflect the actual amount successfully copied!
        memmove(dst, (void *)(cur_pa+(cur_va-basepage_va)), copy_size);
        len-=copy_size;
        dst+=copy_size;
        cur_va=basepage_va+block_size;
        basepage_va=PGROUNDDOWN(cur_va);
    }
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    return 0;
error:
    pr_err("Jump from No.%d", goto_source_line);
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    return -1;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error(for Already mapped, no rollback will be considered.
//just return -1 to alert the caller that we are no longer responsible for subsequent content.)
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    uint64 cur_pa, basepage_va, copy_size;
    uint64 cur_order, block_size, mem;
    int got_null=0, perm, locked_by_me=0;
    pte_t *pte;
    struct proc *cur_proc=myproc();
    while(got_null==0 && max>0){
        basepage_va=PGROUNDDOWN(srcva);
        if(basepage_va>=MAXVA)      TRACKED_GOTO(error);
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(*pte==0){
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about uncontroled region.");
                TRACKED_GOTO(error);
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_contain_vma(cur_proc->mm, srcva);
            if(cur_vma==NULL)       TRACKED_GOTO(error);
            if(!(cur_vma->vm_page_prot & PTE_R)){
                pr_err("Invalid page prot, while trying to write to %llx", srcva);
                TRACKED_GOTO(error);
            }
            else if(cur_vma->vm_file==NULL)
                pr_info("Treat as Anonymous VMA, so copyed string is empty at %llx.", srcva);
            //Case:cur_vma->vm_file==NULL, Can be heap or stack, due to Demand Paging, we shouldn't treat as error.
            perm=cur_vma->vm_page_prot;

            cur_order=i_log2(basepage_va & -basepage_va);
            cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            block_size=1ull<<cur_order;
            while(max < block_size && block_size>PGSIZE)
                block_size/=2;
            // VM_TRACE("copyin needs alloc for basepage_va=0x%llx size=0x%llx\n", basepage_va, alloc_size);
            mem=(uint64)alloc_memory(block_size, GFP_ZERO);
            if(mem==0)  TRACKED_GOTO(error);
            if(cur_vma->vm_file!=NULL && vmfile_load(cur_vma, basepage_va, mem, block_size)!=0){
                //handle the file-backend seperately.
                pr_err("Corrputed file, unable to continue.");
                free_pages((void *)mem, block_size);
                TRACKED_GOTO(error);
            }
            acquire(&cur_proc->uvm_lock);
            if(mappages(&(struct map_context){
                .pagetable=pagetable,
                .start_va=basepage_va,
                .size=block_size,
                .pa=(uint64)mem,
                .xperm=perm
            }) !=0 ) {
                release(&cur_proc->uvm_lock);
                free_pages((void *)mem, block_size);
                TRACKED_GOTO(error);
            }
            release(&cur_proc->uvm_lock);
            cur_pa=walkaddr(pagetable, basepage_va);    //update
            pte=walk(pagetable, basepage_va, 0, 0);
        }
        else if ((*pte & PTE_V) == 0){   //Swap in fault.
            //extrace info(Swap_id) from the pte.
            //FIXME: 
            panic("Swap pending implementation.");
            //remember update the pte and other variable.
        }
        else if ((*pte & PTE_U) == 0)    TRACKED_GOTO(error);
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
            block_size=cross_scan_cont_map(pagetable, basepage_va, (uint64)-1);
        }
        if(pte==0 || (*pte & PTE_R)==0) TRACKED_GOTO(error);
        copy_size=block_size-(srcva-basepage_va);
        if(copy_size>max)   copy_size=max;  
        //modified the size according to the actual amount successfully copied!

        char *ptr=(char *)(cur_pa + (srcva-basepage_va));
        while (copy_size > 0) {
            if (*ptr == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *ptr;
            }
            --copy_size;--max;
            ptr++;dst++;
        }
        srcva=basepage_va+block_size;
    }
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    if (got_null)   return 0;
error:
    pr_err("Jump from No.%d", goto_source_line);
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    return -1;
}
