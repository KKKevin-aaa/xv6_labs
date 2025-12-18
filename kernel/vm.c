#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

// #define DEBUG_VM
#ifdef DEBUG_VM
#define VM_TRACE(fmt, ...) \
    do { \
        printf("[VM:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define VM_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif
#define INDENT_STR(lvl) ((lvl)==2 ? "" : ((lvl)==1 ? "  |-- " : "  |    |-- "))
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;
void *usyscall_pa=NULL;
extern char etext[];  // kernel.ld sets this to end of kernel code.
res_block rb_array[MAX_RES_BLOCK] __attribute__((unused)) ={0};  //static Global variable

extern char trampoline[];  // trampoline.S
void walk_all_page(uint64 start_va, pagetable_t pagetable, int level);
// Make a direct-map page table for the kernel.
pagetable_t kvmmake(void) {
    pagetable_t kpgtbl;

    kpgtbl = (pagetable_t)kalloc();
    if(kpgtbl==0)   panic("kvmmake: alloc fail!");
    memset(kpgtbl, 0, PGSIZE);

    usyscall_pa=kalloc();
    if(usyscall_pa==0)  panic("kvmmake: alloc fail!");
    memset(usyscall_pa, 0, PGSIZE);
    //Allocated once during system initialization, not per-process;
    //thus, it is immune to memory leak.

    // uart registers
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

    // PLIC
    kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // map kernel data and the physical RAM we'll make use of.
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // allocate and map a kernel stack for each process.
    proc_mapstacks(kpgtbl);

    return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void) { kernel_pagetable = kvmmake(); }

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart() {
    // wait for any previous writes to the page table memory to finish.
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));

    // flush stale entries from the TLB.
    sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
// To support superpage, we introduce new paras: target_level
// and stop while level equals to target_level(0 for 4kB, 1 for 2MB, 2 for 1GB)
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, int target_level) {
// #ifdef DEBUG_VM
//     VM_TRACE("pagetable=%p va=%p alloc=%d target_level=%d\n", pagetable, (void *)va, alloc, target_level);
// #endif
    if (va >= MAXVA) panic("walk");
    for (int level = 2; level > target_level; level--) {    //Tips: start level can be changed!
        pte_t *pte = &pagetable[PX(level, va)]; //math
        if (*pte & PTE_V) { //Locate the next level page table using pte(create and init if necessary)
            pagetable = (pagetable_t)PTE2PA(*pte);
        #ifdef LAB_PGTBL    //Superpage support
            if (PTE_LEAF(*pte))
                return pte;
        #endif
        } else {
            if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;   
            //Don't set R/W/X, so this is directory entry,not a superpage
        }
    }
    return &pagetable[PX(target_level, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA) return 0;

    pte = walk(pagetable, va, 0, 0);
    if (pte == 0) return 0;
    if ((*pte & PTE_V) == 0) return 0;
    if ((*pte & PTE_U) == 0) return 0;
    pa = PTE2PA(*pte);
    return pa;
}
#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
void prefix_helper(char c, int count){
    int i=count;
    while(i-- > 0)  printf("%c", c);
}
uint64 get_step_size(int level){
    if(level==2)    return 1ull<<30;
    else if(level==1)   return 1ull<<21;
    else if(level==0)   return 1ull<<12;
    return 0;
}
void walk_all_page(uint64 start_va, pagetable_t pagetable, int level){
    int idx=0, step=1;
    uint64 va_step, standard_stride=get_step_size(level);
    uint64 num_4k_page, page_per_slot;
    while(idx<512){
        pte_t pte=pagetable[idx];
        uint64 pa=PTE2PA(pte);
        if(pte & PTE_V){
            prefix_helper('.', (3-level)*2);
            if((pte & (PTE_X | PTE_W | PTE_R))==0){
                //this PTE points to a lower-level page table.
                printf("(next-level)%p: pte %p pa %p\n", (void *)start_va, (void *)pte, (void *)pa);
                walk_all_page(start_va, (pagetable_t)(void *)pa, level-1);
                //have not handle current va
                step=1;
                va_step=standard_stride;
            }
            else{
                if(is_managed_memory(pa)==0){
                    step=1;
                    va_step=get_step_size(level);
                }
                else{
                    num_4k_page=1ull<<get_order(pa);
                    page_per_slot=1ull<<(level*9);
                    step=num_4k_page/page_per_slot;
                    if(step<=0) step=1;
                    va_step=num_4k_page*PGSIZE;
                }
                printf("(data)%p: pte %p pa %p, size is 0x%lx\n", (void *)start_va, (void *)pte, (void *)pa, va_step);
            }
        }
        else{
            step=1;
            va_step=standard_stride;
        }
        idx+=step;
        start_va+=va_step;
    }
}
void vmprint(pagetable_t pagetable) {
    // your code here'
    printf("page table %p\n", (void *)pagetable);
    walk_all_page(0, pagetable, 2);
    // dump memory map
    dump_memory_map();
}
#endif
void init_res_array(uint64 init_heap_start){    //Assuming A fixed-size stack
    //Limited by the physical memory, cannot reach MAXVA
    uint64 align_2mb_hs=SUPERPGROUNDUP(init_heap_start);
    memset(rb_array, 0, sizeof(res_block)*MAX_RES_BLOCK);
    for(int i=0;i<MAX_RES_BLOCK;i++){
        rb_array[i].va=align_2mb_hs;
        align_2mb_hs+=SUPERPGSIZE;
    }
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
    if (mappages(kpgtbl, va, sz, pa, perm) != 0) panic("kvmmap");
}
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%lx pa=%p perm=0x%x\n", (void *)va, size, (void *)pa, perm);
#endif
    pte_t *pte=NULL;
    uint64 basic_size=PGSIZE, target_level=0, prev_level=3;   //determine rewalk necessity
    if(size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (size == 0) panic("mappages: size");
    while(size>0){
        if(va%GIGAPGSIZE==0 && size>=GIGAPGSIZE){
            target_level=2;basic_size=GIGAPGSIZE;
        } 
        else if(va%MEGAPGSIZE==0 && size>=MEGAPGSIZE){
            target_level=1;basic_size=MEGAPGSIZE;
        }
        else{
            target_level=0;basic_size=PGSIZE;
        }
        if ((pa % basic_size) != 0) panic("mappages: pa not aligned");
        if(prev_level!=target_level || PX(target_level, va)==0)
            //check if arrive the boundary or need jump into other level
            pte=walk(pagetable, va, 1, target_level);//walk again
        else    pte++;  //update the pte quickly,no need to walk agin
        if(*pte & PTE_V)    panic("mappages: remap");
        *pte= PA2PTE(pa) | PTE_V | perm;//Assign
        size-=basic_size;
        va+=basic_size;
        pa+=basic_size;
        //update basic_size(and target level) based on the current maximum alignment value.
    }
    return 0;
}
void migrate_data(pagetable_t pagetable, pte_t pte, uint64 cur_level, uint64 start_va, uint64 total_size){
    uint64 copied_size=0, data_pa=PTE2PA(pte);
    uint64 cur_max_size=i_log2(start_va), alloc_size, cur_va=start_va;
    uint16 cur_order;
    char *mem;
    while(copied_size<total_size){
        alloc_size=cur_max_size;
        while(alloc_size+copied_size>total_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size);
        if (mem == 0) {
            VM_TRACE("migrate_data: failed to allocate cur_va=0x%lx size=0x%lx\n", cur_va, alloc_size);
            freewalk(pagetable, 1, start_va, copied_size, cur_level);
            panic("can't handle!");
        }
        memset(mem, 0, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, alloc_size, PTE_FLAGS(pte))!=0){
            free_pages(mem, alloc_size);
            freewalk(pagetable, 1, start_va, copied_size, cur_level);
            panic("cannot handle!");
        }
        //start migrating data
        memmove(mem, (void *)(data_pa+copied_size), alloc_size);
        if(copied_size+alloc_size<total_size)  copied_size+=alloc_size;
        else    break;
        cur_va+=alloc_size;
        cur_order=i_log2(cur_va);
        cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull<<cur_order;
    }
}
void split_into_blocks(pagetable_t pagetable, uint64 va, uint64 cur_level){
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE;
    pte_t *old_pte;
    old_pte=walk(pagetable, va, 0, cur_level);
    pagetable_t new_pagetable=alloc_memory(PGSIZE);
    uint64 cur_pa=PTE2PA(*old_pte), basic_stride=get_step_size(cur_level-1);
    void *new_block_pa;
    if(new_pagetable==NULL) panic("Out-of-memory!");
    for(int i=0;i<512;i++){
        if(rb_array[idx].bitmap[i]!=0){
            new_pagetable[i]=PA2PTE(cur_pa) | PTE_FLAGS(*old_pte);
            //Inherits RXW permissions from the original huge page, becoming a new small leaf.
            new_block_pa=alloc_memory(basic_stride);
            if(new_block_pa==NULL)  panic("out-of-memory!");
            //Insufficient intermediate space.Update failed.
            memmove(new_block_pa, cur_pa, PGSIZE);
        }
        cur_pa+=basic_stride;
    }
    free_pages((void *)PTE2PA(*old_pte), get_step_size(cur_level));
    *old_pte=PA2PTE(new_pagetable) | PTE_V;
}
pagetable_t split_and_prune(pte_t pte, uint64 start_vpn, uint64 end_vpn, uint64 cur_level){
    if(PTE_LEAF(pte)==0)  panic("split into block: non-leaf!");
    if(!(cur_level >0 && cur_level < MAX_LEVEL))    panic("split into block:invalid level");
    if(start_vpn < 0 || end_vpn>512 || start_vpn>end_vpn)
        panic("split_and_prune:invalid vpn_range");
    //Align start and size to ensure alignment!
    uint64 cur_pa=PTE2PA(pte), basic_stride=get_step_size(cur_level-1);
    #ifdef DEBUG_VM
        VM_TRACE("alloc new pagetable to replace leaf-pte\n");
    #endif
    pagetable_t new_pagetable=(pagetable_t)alloc_memory(PGSIZE);
    if(new_pagetable==0)    panic("out-of-memory");
    memset(new_pagetable, 0, PGSIZE);
    for(int i=0;i<start_vpn;i++){
        new_pagetable[i]=PA2PTE(cur_pa) | PTE_FLAGS(pte);
        cur_pa+=basic_stride;
    }
    cur_pa+=(end_vpn-start_vpn)*get_step_size(cur_level-1);
    for(int i=end_vpn;i<512;i++){
        new_pagetable[i]=PA2PTE(cur_pa) | PTE_FLAGS(pte);
        cur_pa+=basic_stride;
    }
    return new_pagetable;
}
// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == 0) return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}
uint64 uvmunmap_helper(pagetable_t pagetable, uint64 va, uint64 size, int cur_level, int do_free){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%lx do_free=%d, cur_level is %d\n", (void *)va, size, do_free, cur_level);
#endif
    uint64 basic_stride=get_step_size(cur_level);
    uint64 cur_vpn=PX(cur_level, va);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(size+basic_stride-1)/basic_stride;
    if(end_vpn>512){
        #ifdef DEBUG_VM
            VM_TRACE("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
            VM_TRACE("size=0x%lx, cur_level=%d, end_vpn=0x%lx\n", size, cur_level, end_vpn);
        #endif
        panic("uvmunmap_helper!");
        return -1;
    }
    uint64 pa, vpn_incr, cur_order=0, del_size=0, intersection;
    do{
        pte=&pagetable[cur_vpn];
        if((*pte & PTE_V)==0){
            cur_vpn++;
            intersection=(basic_stride > size-del_size) ? (size-del_size) :basic_stride;
            del_size+=intersection;
            va+=intersection;
        }
        else if(PTE_LEAF(*pte)){
            pa=PTE2PA(*pte);
            if(is_managed_memory(pa)==0){
                pte[0]=0;
                va+=basic_stride;
                del_size+=basic_stride;
                cur_vpn++;
            }
            else if(size-del_size<basic_stride){    //Partial Unmap and use Pruned Spltting Strategy
                pte_t tmp_pte=*pte; //Old bigger page pte
                *pte=0; //Unmap to prevent remap error!
                //*pte=PA2PTE((uint64)split_into_blocks(tmp_pte, cur_level)) | PTE_V;
                if(cur_level<=0)    panic("Try to Split the minimun Unit");
                uint64 start_vpn=PX(cur_level-1, va), end_vpn=PX(cur_level-1, va+size-del_size);
                end_vpn=(end_vpn==0)?512:end_vpn;
                *pte=PA2PTE((uint64)split_and_prune(tmp_pte, start_vpn, end_vpn, cur_level)) | PTE_V;
                if(do_free){    //Calculate the free_start_pa(the Only place still remember the original physical address)
                    uint64 free_start_pa=PTE2PA(tmp_pte)+start_vpn*get_step_size(cur_level-1);
                    free_pages((void *)free_start_pa, size-del_size);
                }
                return size;    //Must be end!Since no pte can unmap.
            }
            else{   //full Unmap(basic_stride < size-del_size)
                cur_order=get_order(pa);
                intersection=1ull << (cur_order +ORDER_BASE);
                intersection=(intersection>size-del_size)?(size-del_size):intersection;
                if(do_free!=0)  free_pages((void *)pa, intersection); //free 
                vpn_incr=1ull << (cur_order - cur_level*9);
                for(int i=0;i<vpn_incr;i++)
                    if(cur_vpn+i<end_vpn) pte[i]=0;
                del_size+=intersection;
                va+=intersection;
                cur_vpn+=vpn_incr;
            }
        }
        else{   //directory page:Split and delegate the task to the next-level page table! 
            intersection=(basic_stride > size-del_size) ? (size-del_size) :basic_stride;
            uvmunmap_helper((pagetable_t)PTE2PA(*pte), va, intersection, cur_level-1, do_free);
            if(is_pagetable_empty((pagetable_t)PTE2PA(*pte))){
                uint64 child_pa=PTE2PA(*pte);
                free_pages((void *)child_pa, PGSIZE);
                *pte=0; //clear the pte
            }
            va+=intersection;
            del_size+=intersection;
            cur_vpn++;
        }
    }while(cur_vpn<end_vpn);
    return del_size;
}

void uvmunmap(pagetable_t pagetable, uint64 va, uint64 size, int do_free){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%lx do_free=%d\n", (void *)va, size, do_free);
#endif
    if(size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (size == 0) panic("mappages: size");
    uint64 del_size=uvmunmap_helper(pagetable, va, size, 2, do_free);
    if(del_size!=size)
        panic("uvmunmap: cannot unmap required size!");
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
#ifdef DEBUG_VM
    VM_TRACE("oldsz=0x%lx newsz=0x%lx xperm=0x%x\n", oldsz, newsz, xperm);
#endif
    if(newsz<oldsz) return oldsz;
    uint64 aligned_oldsz=PGROUNDUP(oldsz), aligned_newsz=PGROUNDUP(newsz);
    if(aligned_oldsz==aligned_newsz)    return newsz;
    uint8 oldsz_order=i_log2(aligned_oldsz & -aligned_oldsz);
    oldsz_order=(oldsz_order==0 || oldsz_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):oldsz_order;
    uint64 cur_max_size=1ull << oldsz_order;
    uint64 remain_size=aligned_newsz-aligned_oldsz, alloc_size=cur_max_size;
    void *mem;
    uint64 cur_va=aligned_oldsz, cur_order=0;
    while(remain_size>0){
        alloc_size=cur_max_size;
        while(remain_size < alloc_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size);
        if (mem == 0) {
            VM_TRACE("uvmalloc failed to allocate cur_va=0x%lx size=0x%lx\n", cur_va, alloc_size);
            uvmdealloc(pagetable, cur_va, aligned_oldsz);
            return 0;
        }
        memset(mem, 0, alloc_size);
        VM_TRACE("uvmalloc -> mappages cur_va=0x%lx alloc_size=0x%lx\n", cur_va, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, (uint64)mem, PTE_R | PTE_U | xperm)!=0){
            free_pages(mem, alloc_size);
            VM_TRACE("uvmalloc mappages failed at cur_va=0x%lx size=0x%lx\n", cur_va, alloc_size);
            uvmdealloc(pagetable, cur_va, aligned_oldsz);
            return 0;
        }
        if(remain_size>alloc_size)  remain_size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        cur_order=i_log2(cur_va & -cur_va);
        cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull << cur_order;
    }
    VM_TRACE("uvmalloc exiting success newsz=0x%lx\n", newsz);
    return newsz;
}

//Keep the same logic as uvmalloc(Binary Buddy Decomposition!)
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
#ifdef DEBUG_VM
    VM_TRACE("oldsz=0x%lx newsz=0x%lx\n", oldsz, newsz);
#endif
    if (newsz >= oldsz) {
        VM_TRACE("uvmdealloc no-op new>=old, returning oldsz=0x%lx\n", oldsz);
        return oldsz;
    }
    int aligned_newsz=PGROUNDUP(newsz), aligned_oldsz=PGROUNDUP(oldsz);
    if (aligned_newsz < aligned_oldsz) {
        // Perform the release step-by-step, following the reverse logic of alloc.
        uint8 oldsz_order=i_log2(aligned_oldsz & -aligned_oldsz);
        oldsz_order=(oldsz_order==0 || oldsz_order>MAX_ORDER+ORDER_BASE)
            ?(MAX_ORDER+ORDER_BASE):oldsz_order;
        uint64 cur_max_size=1ull << oldsz_order;
        uint64 remain_size=aligned_oldsz-aligned_newsz, free_size=cur_max_size;
        uint64 cur_va=aligned_oldsz, cur_order=0;
        while(remain_size>0){
            free_size=cur_max_size;
            while(remain_size < free_size && free_size>PGSIZE)
                free_size/=2;
            #ifdef DEBUG_VM
                VM_TRACE("uvmdealloc -> uvmunmap va=0x%lx size=0x%lx, remain size is 0x%lx\n", 
                    cur_va-free_size, free_size, remain_size);
            #endif
            uvmunmap(pagetable, cur_va-free_size, free_size, 1);
            if(remain_size>free_size)  remain_size-=free_size;
            else    break;
            cur_va-=free_size;
            cur_order=i_log2(cur_va & -cur_va);
            cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            cur_max_size=1ull << cur_order;
        }
        // int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        // uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    VM_TRACE("uvmdealloc exiting newsz=0x%lx\n", newsz);
    return newsz;
}

// Recursively free page-table pages.(And consider one-to-many mappings, where
// a single allocation corresponds to N PTEs, and the first pte owing the control of whole
// physical memory, so just free the header and clear the next ptes)
void freewalk(pagetable_t pagetable, int do_free, uint64 base_va, uint64 max_sz, int level) {
#ifdef DEBUG_VM
    if (base_va == 0 && level == 2)
        VM_TRACE("Freewalk Start: pt=%p base_va=0x%lx max_sz=0x%lx\n", pagetable, base_va, max_sz);
#endif
    pte_t pte;// there are 2^9 = 512 PTEs in a page table.
    uint64 pa, num_4k_page, page_per_slot, step;
    uint64 standard_stride=get_step_size(level), cur_va=base_va, va_step;
    uint16 cur_order;
    int idx=0;
    while(idx<512){
        if(cur_va>=max_sz)   break;
        pte = pagetable[idx];
        pa = PTE2PA(pte);    //child pagetable or physical
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // this PTE points to a lower-level page table.
            #ifdef DEBUG_VM
                VM_TRACE("%sDir: idx=%d va=0x%lx -> next_pa=%p\n", INDENT_STR(level), idx, cur_va, (void*)pa);
            #endif
            freewalk((pagetable_t)pa, do_free, cur_va, max_sz, level-1);
            step=1;
            va_step=standard_stride;
        } else if ((pte & PTE_V) && do_free!=0) {   //leaf-node,release the physical page
            //check if next page is associated with current page
            cur_order=get_order(pa);    //Specific area(Cannot be freed),so skip additional checks.
            va_step=1ull<<(cur_order+ORDER_BASE);
            num_4k_page=1ull<<cur_order;
            page_per_slot=1ull<<(level*9);
            step=num_4k_page/page_per_slot;
            if(step<=0) step=1; //at least advance one
            #ifdef DEBUG_VM
            VM_TRACE("%sLeaf: idx=%d va=0x%lx pa=%p order=%d size=0x%lx (step=%ld)\n", 
                     INDENT_STR(level), idx, cur_va, (void*)pa, cur_order, va_step, step);
            #endif
            free_pages((void *)pa, va_step); //record statement before freeing, 
            // as the merging process potentially alter the metadata.
        }
        else{
            step=1; //Invalid pte
            va_step=standard_stride;
        }
        for(int j=0;j<step;j++){
            if(idx+j==512){
                panic("freewalk: alignment error");
                break;
            }
            pagetable[idx+j]=0; //Clear the relevant PTEs
        }
        cur_va+=va_step;
        idx+=step;
    }
    free_pages((void *)pagetable, PGSIZE);
}
// Recursively copy page-table pages.Consider the superpage Error occurs, quit recursively.
// Directly assign the known PTE to bypass the mappage overhead, 
// significantly improving efficiency
int copywalk(pagetable_t old_pg, pagetable_t new_pg, uint64 base_va, void *ret_va, uint64 max_sz, int level){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(level), cur_va=base_va, va_step;
    uint64 num_4k_page, page_per_slot, step, cur_order;
    char *mem;
    uint flags;
    int idx=0;
    while(idx < 512) {
        if(cur_va >= max_sz) break;
        pte = old_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical
        flags = PTE_FLAGS(pte);
        if ((pte & PTE_V) && PTE_LEAF(pte)==0) {
            // Case 1: Directory
            mem = alloc_memory(PGSIZE);
            if(mem == 0) return -1;
            memset(mem, 0, PGSIZE);
            new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
            new_pg[idx] = new_pte;
            #ifdef DEBUG_VM
                VM_TRACE("%s[DIR ] L%d idx=%d: va=0x%lx -> new_tbl=%p (recurse)\n", 
                     INDENT_STR(level), level, idx, cur_va, mem);
            #endif
            if(copywalk((pagetable_t)pa, (pagetable_t)mem, cur_va, ret_va, max_sz, level-1) < 0) return -1;
            step = 1;
            va_step = standard_stride;
        } 
        else if (pte & PTE_V) {   
            // Case 2: Leaf Node
            cur_order = get_order(pa);
            va_step = 1ull << (cur_order + ORDER_BASE);
            if(cur_va % va_step != 0) panic("copywalk: unaligned address!");
            mem = alloc_memory(va_step);
            if(mem == 0) return -1;
            memset(mem, 0, va_step);
            memmove(mem, (char *)pa, va_step);
            num_4k_page = 1ull << cur_order;
            page_per_slot = 1ull << (level * 9);
            step = num_4k_page / page_per_slot;
            if(step <= 0) step = 1;
            #ifdef DEBUG_VM
                VM_TRACE("%s[COPY] L%d idx=%d: va=0x%lx src_pa=0x%lx -> dst_pa=%p size=0x%lx (Order %lx)\n", 
                     INDENT_STR(level), level, idx, cur_va, pa, mem, va_step, cur_order);
            #endif
            for(int i = 0; i < step; i++){
                // Reset flags from original entry
                flags = PTE_FLAGS(old_pg[i+idx]); 
                uint64 inner_pa = (uint64)mem + i * page_per_slot * PGSIZE;
                new_pte = PA2PTE(inner_pa) | flags | PTE_V;
                new_pg[i+idx] = new_pte;
            }
        }
        else {   
            // Case 3: Invalid / Gap
            step = 1;
            va_step = standard_stride;
        }
        cur_va += va_step;
        idx += step;
        *(uint64 *)ret_va = cur_va;
    }
    return 0;
}

// Free user memory pages,(maintain the interface consistency, 
// an non-zero sz implies the need to release the corresponding page)
// Even though the release process itself does not requires the sz parameter.
// Must free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0) freewalk(pagetable, 1, 0, sz, 2);
    else    freewalk(pagetable, 0, 0, sz, 2);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old_pg, pagetable_t new_pg, uint64 sz) {
#ifdef DEBUG_VM
    VM_TRACE("old_pg=%p new_pg=%p sz=0x%lx\n", (void *)old_pg, (void *)new_pg, sz);
#endif
    uint64 start_va=0;
    uint64 ret_va=0;
    if(copywalk(old_pg, new_pg, start_va, &ret_va, sz, 2)<0){//prevent resources leaks and waste
        uvmfree(new_pg, ret_va-start_va);
        //Error occurs, size are guaranteed not to exceed the original size
        return -1;
    }
#ifdef DEBUG_VM
    VM_TRACE("after uvmcopy start_va is 0x%lx, while ret_va is 0x%lx\n", start_va, ret_va);
#endif
    return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
    pte_t *pte;

    pte = walk(pagetable, va, 0, 0);
    if (pte == 0) panic("uvmclear");
    *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
// #ifdef DEBUG_VM
//     VM_TRACE("dstva=%p len=0x%lx\n", (void *)dstva, len);
// #endif
    uint64 aligned_dstva=PGROUNDDOWN(dstva);
    uint8 cur_order=i_log2(aligned_dstva & -aligned_dstva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):cur_order;
    uint64 cur_max_size=1ull << cur_order;
    uint64 alloc_size=cur_max_size, copy_size;
    void *mem;
    uint64 cur_va=dstva, basepage_va=aligned_dstva, cur_pa;
    pte_t *pte;
    while(len>0){
        if(cur_va>=MAXVA)   return -1;
        cur_pa=walkaddr(pagetable, basepage_va);
        if(cur_pa==0){
            alloc_size=cur_max_size;
            while(len < alloc_size && alloc_size>PGSIZE)
                alloc_size/= 2;
            // VM_TRACE("copyout needs alloc for basepage_va=0x%lx size=0x%lx\n", basepage_va, alloc_size);
            mem=alloc_memory(alloc_size);
            if(mem==0){
                uvmdealloc(pagetable, cur_va, dstva);
                return -1;
            }
            memset(mem, 0, alloc_size);
            if(mappages(pagetable, basepage_va, alloc_size, (uint64)mem, PTE_U | PTE_W | PTE_R)!=0){
                free_pages(mem, alloc_size);
                uvmdealloc(pagetable, cur_va, dstva);
                return -1;
            }
            cur_pa=walkaddr(pagetable, basepage_va);
        }
        else{
            alloc_size=1ull<<(get_order(cur_pa)+ORDER_BASE);
            //exist the mapping, check the bitmap for more info.
            pte=walk(pagetable, basepage_va, 0, 0);
            if(pte==0 || (*pte & PTE_W)==0) return -1;  //exist and pte_w is valid.
        }
        copy_size=alloc_size-(cur_va-basepage_va);
        if(copy_size>len)   copy_size=len;
        memmove((void *)cur_pa+(cur_va-basepage_va), src, copy_size);
        len-=copy_size;
        src+=copy_size;
        cur_va=basepage_va+alloc_size;
        cur_order=i_log2(basepage_va & -basepage_va);
        cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull<<cur_order;
        basepage_va=PGROUNDDOWN(cur_va);
    }
    return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
// #ifdef DEBUG_VM
//     VM_TRACE("srcva=%p len=0x%lx\n", (void *)srcva, len);
// #endif
    uint64 aligned_srcva=PGROUNDDOWN(srcva);
    uint8 cur_order=i_log2(aligned_srcva & -aligned_srcva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):cur_order;
    uint64 alloc_size, copy_size;
    void *mem;
    uint64 cur_va=srcva, basepage_va=aligned_srcva, cur_pa;
    pte_t *pte;
    while(len>0){
        cur_pa=walkaddr(pagetable, basepage_va);
        if(cur_pa==0){  
            //Trigger a page fault due to the missing page and finish allocations.
            cur_order=i_log2(basepage_va & -basepage_va);
            cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            alloc_size=1ull<<cur_order;
            while(len < alloc_size && alloc_size>PGSIZE)
                alloc_size/=2;
            // VM_TRACE("copyin needs alloc for basepage_va=0x%lx size=0x%lx\n", basepage_va, alloc_size);
            mem=alloc_memory(alloc_size);
            if(mem==0){
                uvmdealloc(pagetable, basepage_va, srcva);
                return -1;
            }
            memset(mem, 0, alloc_size);
            if(mappages(pagetable, basepage_va, alloc_size, (uint64)mem, PTE_U | PTE_W | PTE_R)!=0){
                free_pages(mem, alloc_size);
                uvmdealloc(pagetable, basepage_va, srcva);
                return -1;
            }
            cur_pa=walkaddr(pagetable, basepage_va);    //update 
        }
        else{
            alloc_size=1ull<<(get_order(cur_pa)+ORDER_BASE); 
            //exist the mapping, check the bitmap for more info.
            pte=walk(pagetable, basepage_va, 0, 0);
            if(pte==0 || (*pte & PTE_R)==0) return -1;
        }
        copy_size=alloc_size-(cur_va-basepage_va);
        if(copy_size>len)   copy_size=len;  
        //modified the size to reflect the actual amount successfully copied!
        memmove(dst, (void *)(cur_pa+(cur_va-basepage_va)), copy_size);
        len-=copy_size;
        dst+=copy_size;
        cur_va=basepage_va+alloc_size;
        basepage_va=PGROUNDDOWN(cur_va);
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    uint64 cur_pa, basepage_va;
    uint64 support_size, copy_size;
    int got_null=0;
    pte_t *pte;
    while(got_null==0 && max>0){
        basepage_va=PGROUNDDOWN(srcva);
        cur_pa=walkaddr(pagetable, basepage_va);
        if(cur_pa==0)   return -1;  //Invalid page
        pte=walk(pagetable, basepage_va, 0, 0);
        if(pte==0 || (*pte & PTE_R)==0) return -1;
        support_size=1ull<<(get_order(cur_pa)+ORDER_BASE);
        copy_size=support_size-(srcva-basepage_va);
        if(copy_size>max)   copy_size=max;  
        //modified the size to reflect the actual amount successfully copied!
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
        srcva=basepage_va+support_size;
    }
    if (got_null)   return 0;
    else    return -1;
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64 vmfault(pagetable_t pagetable, uint64 va, int read) {
    uint64 mem;
    struct proc *p = myproc();

    if (va >= p->sz) return 0;
    va = PGROUNDDOWN(va);
    if (ismapped(pagetable, va)) {
        return 0;
    }
    #ifdef RESERVE
        alloc_res_memory(pagetable, p->init_heap_start, va, va+PGSIZE, PTE_W|PTE_R|PTE_U);
        pte_t *pte=walk(pagetable, va, 0, 0);
        return PTE2PA(*pte);
    #else
        mem = (uint64)kalloc();
        if (mem == 0) return 0;
        memset((void *)mem, 0, PGSIZE);
        if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
            kfree((void *)mem);
            return 0;
        }
        return mem;
    #endif
}

int ismapped(pagetable_t pagetable, uint64 va) {
    pte_t *pte = walk(pagetable, va, 0, 0);
    if (pte == 0) {
        return 0;
    }
    if (*pte & PTE_V) {
        return 1;
    }
    return 0;
}

#ifdef LAB_PGTBL
pte_t *pgpte(pagetable_t pagetable, uint64 va) { return walk(pagetable, va, 0, 0); }
#endif
void merge_into_hugepages_Out(pagetable_t pagetable, uint64 va, int xperm, uint64 alloced_pa){
    //for out-of-place promoted.Expensive copy or move cost.
    if(va% SUPERPGSIZE!=0)  panic("must be 2mb aligned!");
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE, cur_order;
    pte_t *old_pte;
    //mapping trigger va using stale xperm(keep same processing flow)
    old_pte=walk(pagetable, va, 1, 0);
    //Attribute Consistency Check and Accumulation of A/D bits
    uint16 check_perm_mask= PTE_W | PTE_R | PTE_X;
    uint64 expe_perm=xperm, accu_ad=0, bp_idx=0, step;
    while(bp_idx<512){
        if(rb_array[idx].bitmap[bp_idx]!=0){
            uint64 flags=PTE_FLAGS(old_pte[bp_idx]);
            if((flags & check_perm_mask) != expe_perm)  panic("Inconsisent permission!");
            accu_ad |= (flags | (PTE_A | PTE_D));
            //data transfer and release the unused resource
            uint64 prev_pa=PTE2PA(old_pte[bp_idx]);
            memmove(alloced_pa+bp_idx*PGSIZE, prev_pa, PGSIZE);
            cur_order=get_order(prev_pa);
            free_pages(prev_pa, 1ull<<(ORDER_BASE+cur_order));
            bp_idx+=(1ull<<cur_order);
        }
        else    bp_idx++;
    }
    pte_t *parent_pte=walk(pagetable, va, 0, 1);
    free_pages(PTE2PA(*parent_pte), PGSIZE);    //free the dirty pagetable
    expe_perm |= (accu_ad==1)?(PTE_A | PTE_D):0;
    *parent_pte=expe_perm | PA2PTE(alloced_pa) | PTE_V;
    //data transfer and release the unused resource
}
pte_t merge_into_hugepages_In(pagetable_t pagetable, uint64 va, int xperm){
    pte_t *old_pte=walk(pagetable, va, 0, 1);
    uint16 check_perm_mask=PTE_W | PTE_R | PTE_X, cur_order;
    uint64 expe_perm=xperm, accu_ad=0, bp_idx=0, head_pa=0;
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE, cur_order;
    while(idx<512){
        if(rb_array[idx].bitmap[idx]!=0){
            if(head_pa==0)
                head_pa=PTE2PA(old_pte[idx])-PGSIZE*idx;
            uint64 flags=PTE_FLAGS(old_pte[idx]);
            if((flags & check_perm_mask) != expe_perm)  panic("Inconsisent permission!");
            accu_ad |= (flags | (PTE_A | PTE_D));
            //data transfer and release the unused resource
            uint64 prev_pa=PTE2PA(old_pte[idx]);
            cur_order=get_order(prev_pa);
            idx+=(1ull<<cur_order);
        }
        else    idx++;
    }
    free_pages((void *)PTE2PA(*old_pte), PGSIZE);
    *old_pte=PA2PTE(head_pa) | PTE_V | accu_ad;
    return *old_pte;
}
uint64 alloc_res_memory(pagetable_t pagetable, uint64 init_heap_start, 
        uint64 oldsz, uint64 newsz, int xperm){ //sync---64(backward)
    //Lazy promotion.
    uint64 align_2mb_heap_start=SUPERPGROUNDUP(init_heap_start);
    uint64 align_oldsz=PGROUNDDOWN(oldsz), align_newsz=PGROUNDDOWN(newsz);
    uint64 req_size=align_newsz-align_oldsz, start_va, end_va;
    if(align_oldsz<align_2mb_heap_start){
        if(align_newsz<align_2mb_heap_start)
            return uvmalloc(pagetable, oldsz, newsz, xperm);
        uint64 tmp_ret=uvmalloc(pagetable, oldsz, align_2mb_heap_start, xperm);
        if(tmp_ret!=align_2mb_heap_start)   return 0;
        req_size-=(align_2mb_heap_start-align_oldsz);
        align_oldsz=align_2mb_heap_start;
    }
    uint16 idx=(align_oldsz-align_2mb_heap_start)/SUPERPGSIZE;
    uint64 start_vpn, vpn_step;
    void *new_block;
    int split_alloc;
    start_va=align_oldsz;
    while(idx<MAX_RES_BLOCK && req_size>0){
        split_alloc=(SUPERPGROUNDDOWN(start_va)==SUPERPGROUNDDOWN(newsz))?0:1;
        if(split_alloc){
            end_va=(idx+1==MAX_RES_BLOCK)?(rb_array[idx].va+SUPERPGSIZE):rb_array[idx+1].va;
            vpn_step=(end_va-start_va)/PGSIZE;
        }
        else{
            end_va=align_newsz;
            vpn_step=(align_newsz-start_va)/PGSIZE;
        }
        start_vpn=(start_va-rb_array[idx].va)/PGSIZE;
        if(rb_array[idx].promoted==0){
            rb_array[idx].pop_count+=vpn_step;
            if(rb_array[idx].pop_count>THRESHLOD){
                //use the huge page(2mb)
                new_block=alloc_memory(SUPERPGSIZE);
                if(new_block==0){   //promoted fail!
                    new_block=alloc_memory(vpn_step*PGSIZE);
                    if(new_block==0)    panic("out-of-memory");
                }
                else{
                    rb_array[idx].pa=new_block;
                    rb_array[idx].promoted=1;
                    merge_into_hugepages(pagetable, SUPERPGROUNDDOWN(start_va), xperm, new_block); 
                    //update the mapping_table
                    memset(rb_array[idx].bitmap+start_vpn, 1, vpn_step);    //update the bitmaps
                }
            }
            else{//use the simple allocator(split_alloc must be zero)
                memset(rb_array[idx].bitmap+start_vpn, 1, vpn_step);
                return uvmalloc(pagetable, oldsz, newsz, xperm);
            }
        }   //for promoted pages, do nothing!
        req_size-=(end_va-start_va);
        start_va=end_va;
        idx++;
    }
    //check if cross-page and need handle further
    return newsz;
}

uint64 free_res_memory(pagetable_t pagetable, uint64 init_heap_start, 
        uint64 oldsz, uint64 newsz){
    //Eager Demotion!
    init_heap_start=PGROUNDUP(init_heap_start);
    uint64 align_oldsz=PGROUNDUP(oldsz), align_newsz=PGROUNDUP(newsz);
    uint64 req_size=align_oldsz-align_newsz, start_va, end_va;
    //dealloc backward,maintain consistent handling logic
    if(align_newsz<init_heap_start){
        if(align_oldsz<init_heap_start)
            return uvmdealloc(pagetable, oldsz, newsz);
        uint64 tmp_ret=uvmdealloc(pagetable, init_heap_start, align_newsz);
        if(tmp_ret!=align_newsz)    return 0;   //error
        req_size-=(init_heap_start-align_newsz);
        align_newsz=init_heap_start;
    }
    uint16 idx=(align_newsz-init_heap_start)/PGSIZE;
    uint64 start_vpn, vpn_step;
    void *new_block;
    int split_dealloc;
    pte_t *pte;
    start_va=align_newsz;
    while(idx<MAX_RES_BLOCK && req_size>0){
        start_vpn=(start_va-rb_array[idx].va)/PGSIZE;
        split_dealloc=(SUPERPGROUNDDOWN(start_va)==SUPERPGROUNDDOWN(oldsz))?0:1;
        if(split_dealloc){
            end_va=(idx+1==MAX_RES_BLOCK)?(rb_array[idx].va+SUPERPGSIZE):rb_array[idx+1].va;
        }
        else    end_va=align_oldsz;
        vpn_step=(end_va-start_va)/PGSIZE;
        if(rb_array[idx].promoted==1){  
            //Split regradless of release size due to the lack of intermidate state.
            rb_array[idx].pop_count-=vpn_step;
            split_into_blocks(pagetable, start_va, 1);  //fixed as SUPERPAGE
            rb_array[idx].promoted=0;
            memset(rb_array[idx].bitmap+start_vpn, 0, vpn_step);
        }
        else{   //don't promotion
            uvmdealloc(pagetable, end_va, start_va);
        }
        req_size-=(end_va-start_va);
        start_va=end_va;
        idx++;
    }
    //check if cross-page and need handle further
    return newsz;
}

uint64 Simp_alloc_res_memory(pagetable_t pagetable, uint64 init_heap_start, 
        uint64 va, int xperm){
    //Triggered by a single page fault and allocate exactly one pages.
    init_heap_start=SUPERPGROUNDUP(init_heap_start); //make sure aligned
    va=PGROUNDDOWN(va);
    uint64 new_block=0;
    if(va<init_heap_start){
        new_block=(uint64)alloc_memory(PGSIZE);
        if(new_block==NULL) return 0;
        memset((void *)new_block, 0, PGSIZE);
        if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
            free_pages(new_block, PGSIZE);
            new_block=0;
        }
        return new_block;
    }
    uint16 idx=(va-init_heap_start)/SUPERPGSIZE;
    uint64 start_vpn=(va-rb_array[idx].va)/PGSIZE;
    if(rb_array[idx].promoted==0){  //enter reservable region, evaluate reservation strategy
        rb_array[idx].pop_count++;
        if(rb_array[idx].pop_count>THRESHLOD){
            #ifdef IN_PLACE_PROMOTE
                //Supposing existing the huge already,Upgrade page table 
                (void)start_vpn;
                pte_t new_pte=merge_into_hugepages_In(pagetable, va, xperm);
                rb_array[idx].promoted=1;
                return PTE2PA(new_pte)+va*PGSIZE;
            #else
                new_block=alloc_memory(SUPERPGSIZE);
                if(new_block==0){   //promoted fail!
                    new_block=alloc_memory(PGSIZE);
                    if(new_block==0)    panic("out-of-memory");
                    memset((void *)new_block, 0, PGSIZE);
                    if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                        free_pages(new_block, PGSIZE);
                        new_block=NULL;
                    }
                    rb_array[idx].bitmap[start_vpn]=1;
                    return new_block;
                }
                //promoted success
                memset(new_block, 0, SUPERPGSIZE);
                rb_array[idx].pa=new_block;
                rb_array[idx].promoted=1;
                merge_into_hugepages(pagetable, va, xperm, new_block);
                rb_array[idx].bitmap[start_vpn]=1;
                return new_block+start_vpn*PGSIZE;
            #endif
        }
        else{   
            #ifdef IN_PLACE_PROMOTE
                //check if superpage have been allocated
                rb_array[idx].bitmap[start_vpn]=1;
                if(rb_array[idx].pop_count==1){ //have not allocated superpage
                    new_block=(uint64)alloc_memory(SUPERPGSIZE);
                    if(new_block==0){   //Used but allocate hugepage fail(out-of-memory)
                        rb_array[idx].is_scattered=1;
                        new_block=alloc_memory(PGSIZE);
                        if(new_block==0)    panic("out-of-memory");
                        memset((void *)new_block, 0, PGSIZE);
                        if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                            free_pages(new_block, PGSIZE);
                            new_block=0;
                        }
                        return new_block;
                    }
                    //allocate success
                    memset(new_block, 0, SUPERPGSIZE);
                    rb_array[idx].pa=new_block;
                    rb_array[idx].is_scattered=0;
                    uint64 ret_pa=new_block+start_vpn*PGSIZE;
                    if(mappages(pagetable, va, PGSIZE, ret_pa, xperm)!=0){
                        free_pages(new_block, SUPERPGSIZE);
                        ret_pa=0;
                    }
                    return ret_pa;
                }
                else{
                    if(rb_array[idx].is_scattered==1){  //try to allocate again
                        new_block=alloc_memory(SUPERPGSIZE);
                        if(new_block!=0 && mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                            memset(new_block, 0, SUPERPGSIZE);
                            rb_array[idx].is_scattered=0;   //update
                            //move the data
                            merge_into_hugepages_Out(pagetable, va, xperm, new_block);
                        }
                        else{
                            if(new_block!=0)    free_pages(new_block, SUPERPGSIZE);
                            new_block=alloc_memory(PGSIZE);
                            if(new_block==0)    panic("out-of-memory");
                            memset((void *)new_block, 0, PGSIZE);
                            if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                                free_pages(new_block, PGSIZE);
                                new_block=0;
                            }
                            return new_block;
                        }
                    }
                    //is_scattered is 0(or upgrade successfully)
                    
                }
            #else
                //use the simple allocator
                new_block=alloc_memory(PGSIZE);
                if(new_block==0)    panic("out-of-memory");
                memset((void *)new_block, 0, PGSIZE);
                if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                    free_pages(new_block, PGSIZE);
                    new_block=NULL;
                }
                rb_array[idx].bitmap[start_vpn]=1;
                return new_block;
            #endif
        }
    }
    //already promoted, exist mapping.
    return new_block;
}
