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


//handle file-backend page fault, support arbitrary size
int vmfile_load(vm_area_struct_t *vma, uint64 va, uint64 dst_pa, uint64 size){
    uint64 offset=vma->vm_pgoff + (va-vma->vm_start);
    uint64 len=(va+size > vma->vm_end)?(vma->vm_end-va):size;
    begin_op();
    ilock(vma->vm_file->ip);
    uint64 file_offset=vma->vm_pgoff + offset;
    if(file_offset < vma->vm_file->ip->size){
        len=(vma->vm_file->ip->size - file_offset < size)?
                (vma->vm_file->ip->size-file_offset):size;
        if (readi(vma->vm_file->ip, 0, dst_pa, offset, len) != len)
            pr_err("load from %llx fail(Expected is %llx)", va, len);
    }
    iunlockput(vma->vm_file->ip);
    end_op();
    return 0;
}

//Input cow pte and return the allocated physical page address.
uint64 vm_cowfault_handler(pagetable_t pagetable, uint64 flush_va, uint64 flush_size, 
        pte_t * pte, int flush){
    //check again.
    if(!(*pte & PTE_COW) || (*pte & PTE_W)!=0){
        pr_err("Non-compliant PTE.");
        return 0;
    }
    if(flush && pagetable==NULL){
        pr_err("The input is a empty pagetable when needed flush.");
    }
    uint64 old_pa=PTE2PA(*pte), src_flag=PTE_FLAGS(*pte), ret_pa=0;
    if(page_get_ref_wrapper((void *)old_pa)!=1){
        ret_pa=(uint64)alloc_memory(PGSIZE, 0);
        if(ret_pa==0){
            pr_warn("COW:alloc memory fail.\n");
            return 0;
        }
        memmove((void *)ret_pa, (void *)old_pa, PGSIZE);
        *pte=PA2PTE(ret_pa) | (src_flag & ~PTE_COW) | PTE_W;    //Overwrite
        if(flush)   tlb_shootdown_issue_nolock(pagetable, flush_va, flush_size);
        free_pages((void *)old_pa, PGSIZE);
    }
    else{   //Current ref_count is already one, exclusively owned this page.
        *pte = (*pte & ~PTE_COW) | PTE_W;
        if(flush)   tlb_shootdown_issue_nolock(pagetable, flush_va, flush_size);
        ret_pa=PTE2PA(*pte);
    }
    return ret_pa;
}

// allocate and map user memory if process is referencing a page 
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if out of physical memory, 
// and return physical address if successful.
// NOTE: for argument "read" means Load page fault while 1, and means store error whne 0.
uint64 vmfault(res_block *rblocks, pagetable_t pagetable, uint64 va, uint64 scause) {
    struct proc *p = myproc();
    uint64 ret_pa=0;
    int locked_by_me=0;
    //Pass arugment rblocks etc explicitly instead of implicitly retriving them via myproc()
    if(p->mm==NULL){
        pr_err("Fatal error: proc's mm is NULL.\n");
        return 0;
    }
    if(scause!=12 && scause!=13 && scause!=15){
        pr_err("Unsupported trap casue(0x%llx); only page faults are handled here.", scause);
        return 0;
    }
    acquiresleep(&p->mm->mm_lock);  //acquire sleeplock firstly
    va = PGROUNDDOWN(va);
    vm_area_struct_t *vma=find_contain_vma_and_get(p->mm, va);
    releasesleep(&p->mm->mm_lock);
    if(vma==NULL){
        pr_err("Cannot find the corresponding vma.\n");
        goto release_and_ret;
    }
    if(((scause==12) && !(vma->vm_flags & VM_EXEC))
        || (scause==13 && !(vma->vm_flags & VM_READ)) 
        || (scause==15 && !(vma->vm_flags & VM_WRITE))){
        pr_warn("Inconsisent permission.\n");
        goto release_and_ret;
    }
    if(!holding(&p->uvm_lock)){
        acquire(&p->uvm_lock);      //Acquiring, for modifying PTE absolutely.
        locked_by_me=1;
    }
    //Other threads operating on the page table can proceed normally.
    //Current threads will spin-wait.
    pte_t *pte=walk(p->pagetable, va, 0, 0);
    if(pte!=NULL && (*pte & PTE_V)){
        //double check, for shared pagetable in multicore changed
        if((*pte & PTE_U)==0){  //Only if Sstatus.sum is 1, hardware will allow the access to userspace.
            pr_warn("Illegal access to kernel address.Permission denied.");
            goto release_and_ret;
        }
        //Some case : A/D bit wouldn't update atomatically, should via fault traps.
        //Set only if necessary(cache line boundcing, mesi)
        if(scause ==12){    //Instruction
            if((*pte & PTE_X)!=0){
                if((*pte & PTE_A)==0)   *pte |= PTE_A;
                ret_pa=PTE2PA(*pte);
                pr_warn("Instructions fault already resolved by another threads.");
                goto release_and_ret;
            }
        }
        else if(scause== 13){
            if((*pte & PTE_R)!=0){
                if((*pte & PTE_A)==0)   *pte |= PTE_A;
                ret_pa=PTE2PA(*pte);
                pr_warn("Load fault already resolved by another threads.");
                goto release_and_ret;
            }
        }
        else if(scause==15){
            if((*pte & PTE_W)!=0){
                if((*pte & PTE_D)==0)    *pte |= (PTE_D | PTE_A);
                //Set Access and dirty bit both.
                ret_pa=PTE2PA(*pte);
                pr_warn("Save fault already resolved by another threads.");
                goto release_and_ret;
            }
        }
        else    panic("???");
    }
    //Valid vma, categorization and dispatching.
    if(pte==NULL || *pte==0){
        //Lack basic directory pte or PTE is empty.do Lazy allocation.
        if(vma->vm_file==NULL){     //Anonymous Fault.(.bss or heap or stack), mayeb mmap
        #ifdef RESERVE
            ret_pa=uvmalloc_thp_region(&(struct alloc_context){
                .pagetable=pagetable,
                .seg_start=va,
                .seg_end=va+PGSIZE,
                .pt_lock=&p->uvm_lock,
                .xperm=vma->vm_page_prot,
                .rblocks=p->rb_array
            });
        #else
            ret_pa = (uint64)alloc_memory(PGSIZE);
            if (ret_pa == 0){
                pr_err("alloc new page fail.\n");
                goto release_and_ret;
            }
            memset((void *)ret_pa, 0, PGSIZE);
            if(mappages(&(struct map_context){
                .pagetable=p->pagetable,
                .start_va=va,
                .size=PGSIZE,
                .pa=ret_pa,
                .xperm=vma->vm_page_prot,
                .pt_lock=&p->uvm_lock,
            }) != 0){
                free_pages((void *)ret_pa, PGSIZE);
                ret_pa=0;
                goto release_and_ret;
            }
        #endif
        }
        else{       //File-backed Fault(.text or .data)
            ret_pa = (uint64)alloc_memory(PGSIZE, GFP_ZERO);
            if (ret_pa == 0){
                pr_err("alloc new page fail.\n");
                goto release_and_ret;
            }
            release(&p->uvm_lock);
            locked_by_me=0;
            if(vmfile_load(vma, va, ret_pa, PGSIZE)!=0){
                free_pages((void *)ret_pa, PGSIZE);
                pr_err("error when loading file.");
                ret_pa=0;
            }
            else{   //load file successfully. Post-sleep validation
                acquiresleep(&p->mm->mm_lock);
                acquire(&p->uvm_lock);
                locked_by_me=1;
                if(vma->vm_mm != p->mm){
                    pr_warn("handle page fault(0x%llx) occur error during reacquire lock"
                        "previous vma have unmapped.", va);
                    free_pages((void *)ret_pa, PGSIZE);
                    ret_pa=0;
                    goto release_and_ret;
                }
                pte=walk(pagetable, va, 0, 0);
                if(pte==NULL)   panic("Destory pagetable structure.");
                else if(*pte & PTE_V){
                    //Current page fault has already been handled just Release original resources.
                    free_pages((void *)ret_pa, PGSIZE);
                    pr_info("Other concurrent process have resolved.");
                    ret_pa=PTE2PA(*pte);
                }
                else if(mappages(&(struct map_context){
                    .pagetable=p->pagetable,
                    .start_va=va,
                    .size=PGSIZE,
                    .pa=ret_pa,
                    .xperm=vma->vm_page_prot
                }) !=0 ) {
                    pr_err("Mappage %llx fail", va);
                    free_pages((void *)ret_pa, PGSIZE);
                    ret_pa=0;
                }
            }
        }
    }
    else if(pte!=NULL && (*pte & PTE_V)==0){        //Swap in Fault
        //extrace info(Swap_id) from the pte.
        //FIXME: 
        panic("Swap fault have not handle.");
    }
    else{       //PTE is valid, but permission dismatch
        if(vma->vm_flags & VM_WRITE){   //COW(copy on write)
            if(!PTE_LEAF(*pte) || !(*pte & PTE_COW) || (*pte & PTE_W))
                panic("Dismatch between PTE amd vm_flag in COW.");
            ret_pa=vm_cowfault_handler(pagetable, va, PGSIZE, pte, 1);
            if(ret_pa==0)   goto release_and_ret;
            // panic("COW: pending implementation.");
        }
        else    pr_warn("Try to write constant area.\n");
    }
release_and_ret:
    if(vma!=NULL)   vma_put(vma);
    //About lock releasing, Obey first in last out.
    if(locked_by_me==1 && holding(&p->uvm_lock)){
        release(&p->uvm_lock);
        locked_by_me=0;
    }
    if(holdingsleep(&p->mm->mm_lock))
        releasesleep(&p->mm->mm_lock);
    return ret_pa;
}

//Used for process invalid pte met in vmfault.
int process_empty_pte(struct proc *cur_proc, uint64 basepage_va, uint64 cur_va, uint64 len){
    uint64 cur_order, block_size;
    void *mem;
    int perm=0;
    cur_order=i_log2(basepage_va & -basepage_va);
    cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
    block_size=1ull<<cur_order;
    while(len < block_size && block_size>PGSIZE)
        block_size/=2;
    mem=alloc_memory(block_size, GFP_ZERO);
    if(mem==0)  return -1;
    int locked_by_me=0;     //Variable on stack(Process's private stack, visable for CPUs)
    //check if belong to file-backend and initiate a batch disk read request.
    if(cur_proc->mm==NULL)
        pr_warn("lack necessary mm_struct, can't get more info.Treat as anonymous VMA.");
    else{
        if(!holdingsleep(&cur_proc->mm->mm_lock)){
            acquiresleep(&cur_proc->mm->mm_lock);
            locked_by_me=1;
        }
        vm_area_struct_t *cur_vma=find_contain_vma(cur_proc->mm, cur_va);
        if(cur_vma==NULL || cur_vma->vm_file==NULL){
            pr_info("Treat as anonymous VMA.");
            perm = PTE_R | PTE_U | PTE_W;
        }
        else if(vmfile_load(cur_vma, basepage_va, (uint64)mem, block_size)!=0){
            pr_err("Corrputed file, unable to continue.");
            free_pages(mem, block_size);
            if(locked_by_me)    releasesleep(&cur_proc->mm->mm_lock);
            //Whatever CPUs run this process, must release this sleeplock(CPU independent)
            //And "locked_by_me" can seen by any CPUS.
            return -1;
        }
        else    perm = cur_vma->vm_page_prot;
    }
    acquire(&cur_proc->uvm_lock);
    if(mappages(&(struct map_context){
        .pagetable=cur_proc->pagetable,
        .start_va=basepage_va,
        .size=block_size,
        .pa=(uint64)mem,
        .xperm=perm
    })!=0){
        release(&cur_proc->uvm_lock);
        free_pages(mem, block_size);
        if(locked_by_me)        releasesleep(&cur_proc->mm->mm_lock);
        return -1;
    }
    release(&cur_proc->uvm_lock);
    if(locked_by_me)        releasesleep(&cur_proc->mm->mm_lock);
    return 0;
}