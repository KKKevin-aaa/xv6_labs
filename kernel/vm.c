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
// res_block rb_array[MAX_RES_BLOCK] __attribute__((unused)) ={0};  //static Global variable

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
void init_res_array(res_block *rb_array, uint64 init_heap_start){    //Assuming A fixed-size stack
    //Limited by the physical memory, cannot reach MAXVA
    uint64 align_2mb_hs=SUPERPGROUNDUP(init_heap_start);
    memset((void *)rb_array, 0, sizeof(res_block)*MAX_RES_BLOCK);
    for(int i=0;i<MAX_RES_BLOCK;i++){
        rb_array[i].va=align_2mb_hs;
        rb_array[i].is_scattered=1;
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
        memset((void *)mem, 0, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, (uint64)mem, PTE_FLAGS(pte))!=0){
            free_pages((void *)mem, alloc_size);
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
void split_into_blocks(res_block * rb_array, pagetable_t pagetable, uint64 va, uint64 cur_level){
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, va, 0, cur_level);
    pagetable_t new_pagetable=alloc_memory(PGSIZE);
    uint64 cur_pa=PTE2PA(*old_pte), basic_stride=get_step_size(cur_level-1), delete_pa=cur_pa;
    void *new_block_pa;
    if(new_pagetable==NULL) panic("Out-of-memory!");
    for(int i=0;i<512;i++){
        if(rb_array[idx].bitmap[i]!=0){
            //Inherits RXW permissions from the original huge page, becoming a new small leaf.
            new_block_pa=alloc_memory(basic_stride);
            if(new_block_pa==NULL)  panic("out-of-memory!");
            //Insufficient intermediate space.Update failed.
            memmove(new_block_pa, (void *)cur_pa, basic_stride);
            new_pagetable[i]=PA2PTE(new_block_pa) | PTE_FLAGS(*old_pte);
        }
        cur_pa+=basic_stride;
    }
    *old_pte=PA2PTE(new_pagetable) | PTE_V; //Directory pte
    sfence_vma();
    free_pages((void *)delete_pa, get_step_size(cur_level));
}
pagetable_t split_and_prune(pte_t pte, uint64 start_vpn, uint64 end_vpn, uint64 cur_level){
    if(PTE_LEAF(pte)==0)  panic("split into block: non-leaf!");
    if(!(cur_level >0 && cur_level < MAX_LEVEL))    panic("split into block:invalid level");
    if(end_vpn>512 || start_vpn>end_vpn)
        panic("split_and_prune:invalid vpn_range");
    //Align start and size to ensure alignment!
    uint64 cur_pa=PTE2PA(pte), basic_stride=get_step_size(cur_level-1);
    #ifdef DEBUG_VM
        VM_TRACE("alloc new pagetable to replace leaf-pte\n");
    #endif
    pagetable_t new_pagetable=(pagetable_t)alloc_memory(PGSIZE);
    if(new_pagetable==0)    panic("out-of-memory");
    memset((void *)new_pagetable, 0, PGSIZE);
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
    uint64 cur_vpn=PX(cur_level, va), start_page_offset=va & (basic_stride-1);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(start_page_offset+size+basic_stride-1)/basic_stride;    //exclude end_vpn
    if(end_vpn>512){
        #ifdef DEBUG_VM
            VM_TRACE("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
            VM_TRACE("size=0x%lx, cur_level=%d, end_vpn=0x%lx\n", size, cur_level, end_vpn);
        #endif
        panic("uvmunmap_helper!");
        return -1;
    }
    uint64 pa, vpn_incr, cur_order=0, del_size=0, intersection;
    while(cur_vpn<end_vpn){
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
    }
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
void buddy_alloc(pagetable_t pagetable, uint64 va, uint64 size, int xperm){
    if(va%PGSIZE!=0)    panic("Split_alloc: unaligned address!");
    uint8 va_order=i_log2(va & -va);
    va_order=(va_order==0 || va_order>MAX_ORDER+ORDER_BASE)?
        (MAX_ORDER+ORDER_BASE):va_order;
    uint64 cur_max_size=1ull<<va_order ,alloc_size, cur_va=va;
    void *mem;
    while(size>0){
        alloc_size=cur_max_size;
        while(size < alloc_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size);
        if (mem == 0) {
            VM_TRACE("uvmalloc failed to allocate cur_va=0x%lx size=0x%lx\n", cur_va, alloc_size);
            uvmdealloc(pagetable, cur_va, va);
            return 0;
        }
        memset(mem, 0, alloc_size);
        VM_TRACE("uvmalloc -> mappages cur_va=0x%lx alloc_size=0x%lx\n", cur_va, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, (uint64)mem, xperm)!=0){
            free_pages(mem, alloc_size);
            VM_TRACE("uvmalloc mappages failed at cur_va=0x%lx size=0x%lx\n", cur_va, alloc_size);
            uvmdealloc(pagetable, cur_va, va);
            return 0;
        }
        if(size>alloc_size)  size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        va_order=i_log2(cur_va & -cur_va);
        va_order=(va_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):va_order;
        cur_max_size=1ull << va_order;
    }
}
int recursive_copy_map(pte_t *dst_pg, pte_t *src_pg, uint64 size, int level){
    uint64 basic_stride=get_step_size(level);
    uint64 src_pa=PTE2PA(*src_pg);
    int i=0;
    while(i<512 && size>0){ //start with zero(always)
        pte_t src_entry=src_pg[i], src_pa=PTE2PA(src_entry);
        if(size >= basic_stride){   //Perfect fit(considering buddy_system)
            uint64 src_phy_size;
            if(level==0)    src_phy_size=1ull<<(get_order(src_pa) + ORDER_BASE);
            else    src_phy_size=basic_stride;
            uint64 num_4k_page=src_phy_size/PGSIZE, page_per_slot=1ull<<(9*level);
            uint64 step=num_4k_page/page_per_slot;
            void *mem=alloc_memory(src_phy_size);
            if(mem==NULL)   return -1;
            memmove(mem, (void *)src_pa, src_phy_size);
            if(step <= 0) step = 1;
            for(int j = 0; j < step; j++){
                // Reset flags from original entry
                uint64 inner_pa = (uint64)mem + j * page_per_slot * PGSIZE;
                dst_pg[j] = PA2PTE(inner_pa) | PTE_FLAGS(*src_pg);
            }
            i+=step;
            size-=src_phy_size;
        }
        else{   //Partial/Split(size < basic_stride)
            if(level!=0){
                pte_t *new_dst_pg=(pte_t *)alloc_memory(PGSIZE);
                if(new_dst_pg==NULL)    return -1;
                dst_pg[i]=PA2PTE((uint64)new_dst_pg) | PTE_V;
                pte_t *next_src_table=(pte_t *)PTE2PA(src_entry);
                return recursive_copy_map(new_dst_pg, next_src_table, size, level-1);
            }
            else{
                void *dst_mem=alloc_memory(PGSIZE);
                memmove(dst_mem, src_pa, PGSIZE);
                src_pg[i]=PA2PTE((uint64)dst_mem) | PTE_FLAGS(src_pg[i]);
                return 0;
            }
        }
    }
}
int copy_partial_block(pte_t *dst_pg, pte_t *src_pg, uint64 va, uint64 size, int level){
    uint64 basic_stride=get_step_size(level);
    uint64 src_pa=PTE2PA(*src_pg);
    int i=0;
    while(i<512 && size>0){ //start with zero(always)
        if(size >= basic_stride){   //Perfect fit(considering buddy_system)
            uint64 src_phy_size=1ull<<i_log2(va & -va);
            while(src_phy_size > size)
                src_phy_size/=2;
            uint64 num_4k_page=src_phy_size/PGSIZE, page_per_slot=1ull<<(9*level);
            uint64 step=num_4k_page/page_per_slot;
            void *mem=alloc_memory(src_phy_size);
            if(mem==NULL)   return -1;
            memmove(mem, (void *)src_pa, src_phy_size);
            if(step <= 0) step = 1;
            for(int j = 0; j < step; j++){
                // Reset flags from original entry
                uint64 inner_pa = (uint64)mem + j * page_per_slot * PGSIZE;
                dst_pg[j] = PA2PTE(inner_pa) | PTE_FLAGS(src_pg[i+j]);
            }
            i+=step;
            size-=src_phy_size;
            src_pa+=src_phy_size;va+=src_phy_size;
        }
        else{   //Partial/Split(size < basic_stride)
            if(level!=0){
                pte_t *new_dst_pg=(pte_t *)alloc_memory(PGSIZE);
                if(new_dst_pg==NULL)    return -1;
                dst_pg[i]=PA2PTE((uint64)new_dst_pg) | PTE_V;
                return recursive_copy_map(new_dst_pg, src_pg, size, level-1);
            }
            else{
                void *dst_mem=alloc_memory(PGSIZE);
                memmove(dst_mem, src_pa, PGSIZE);
                dst_pg[i]=PA2PTE((uint64)dst_mem) | PTE_FLAGS(src_pg[i]);
                return 0;
            }
        }
    }
}
int copy_whole_block(pte_t *dst_pg, pte_t *src_pg, uint64 size, int level){
    uint64 basic_stride=get_step_size(level);
    uint64 src_pa=PTE2PA(*src_pg);
    //Perfect fit(considering buddy_system)
    uint64 num_4k_page=size/PGSIZE, page_per_slot=1ull<<(9*level);
    uint64 step=num_4k_page/page_per_slot;
    void *mem=alloc_memory(size);
    if(mem==NULL)   return -1;
    memmove(mem, (void *)src_pa, size);
    if(step <= 0) step = 1;
    for(int j = 0; j < step; j++){
        // Reset flags from original entry
        uint64 inner_pa = (uint64)mem + j * page_per_slot * PGSIZE;
        dst_pg[j] = PA2PTE(inner_pa) | PTE_FLAGS(src_pg[j]);
    }

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
    uint64 remain_size=aligned_newsz-aligned_oldsz;
    buddy_alloc(pagetable, aligned_oldsz, remain_size, xperm | PTE_R | PTE_U);
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
        uint8 newsz_order=i_log2(aligned_newsz & -aligned_newsz);
        newsz_order=(newsz_order==0 || newsz_order>MAX_ORDER+ORDER_BASE)
            ?(MAX_ORDER+ORDER_BASE):newsz_order;
        uint64 cur_max_size=1ull << newsz_order;
        uint64 remain_size=aligned_oldsz-aligned_newsz, free_size=cur_max_size;
        uint64 cur_va=aligned_newsz, cur_order=0;
        while(remain_size>0){
            free_size=cur_max_size;
            while(remain_size < free_size && free_size>PGSIZE)
                free_size/=2;
            #ifdef DEBUG_VM
                VM_TRACE("uvmdealloc -> uvmunmap va=0x%lx size=0x%lx, remain size is 0x%lx\n", 
                    cur_va-free_size, free_size, remain_size);
            #endif
            uvmunmap(pagetable, cur_va, free_size, 1);
            if(remain_size>free_size)  remain_size-=free_size;
            else    break;
            cur_va+=free_size;
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
        if ((pte & PTE_V) && PTE_LEAF(pte) == 0) {
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
int copywalk(struct vm_dupl_ctx v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1.level), cur_va=v1.base_va, va_step;
    uint64 step, flags;
    char *mem;
    int idx=0;
    while(idx < 512) {
        if(cur_va >= v1.max_sz) break;
        pte = v1.old_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical
        flags = PTE_FLAGS(pte);
        if ((pte & PTE_V) && PTE_LEAF(pte)==0) {// Case 1: Directory
            mem = alloc_memory(PGSIZE);
            if(mem == 0) return -1;
            memset(mem, 0, PGSIZE);
            new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
            v1.new_pg[idx] = new_pte;
            #ifdef DEBUG_VM
                VM_TRACE("%s[DIR ] L%d idx=%d: va=0x%lx -> new_tbl=%p (recurse)\n", 
                     INDENT_STR(v1.level), v1.level, idx, cur_va, mem);
            #endif
            if(v1.level==1 && cur_va>=v1.rb_array[0].va){   //enter heap region(Recursion Short-circuiting)
                // promote==0 must be true upon entering this block.
                if(cur_va>=v1.rb_array[MAX_RES_BLOCK-1].va+SUPERPGSIZE) return -1;
                uint64 rb_idx=(cur_va-v1.rb_array[0].va)/SUPERPGSIZE;   //Alignment satisfied
                if(cur_va + SUPERPGSIZE < v1.max_sz
                        && v1.rb_array[rb_idx].is_scattered==0 && v1.rb_array[rb_idx].pa!=0){
                    uint64 new_superpage=(uint64)alloc_memory(SUPERPGSIZE);
                    if(new_superpage==0)    return -1;
                    memmove((void *)new_superpage, (void *)(v1.rb_array[rb_idx].pa), SUPERPGSIZE);    //migrate data
                    uint64 bp_idx=0, cur_order, bp_step;
                    pte_t *src_leaf_pte=(pte_t *)pa, *dst_leaf_pte=(pte_t *)mem;
                    while(bp_idx<512){
                        if(v1.rb_array[rb_idx].bitmap[bp_idx]!=0){
                            cur_order=get_order(PTE2PA(src_leaf_pte[bp_idx]));
                            bp_step=1ull<<cur_order;
                            uint64 cur_pa=new_superpage+PGSIZE*bp_idx, src_flags=PTE_FLAGS(src_leaf_pte[bp_idx]);
                            for(int j=0;j<bp_step;j++){
                                if(bp_idx+j>=512)    panic("out-of-range!");
                                dst_leaf_pte[bp_idx+j]=src_flags | PA2PTE(cur_pa);
                                cur_pa+=PGSIZE;
                            }
                        }
                        else    bp_step=1;
                        bp_idx+=bp_step;
                    }
                    v1.rb_array[rb_idx].pa=new_superpage;//Update PA last;preserve remaining parameters.
                }
                else{
                    v1.rb_array[rb_idx].pa=0;
                    v1.rb_array[rb_idx].is_scattered=1;
                    v1.rb_array[rb_idx].promoted=0;
                    if(cur_va + SUPERPGSIZE >=v1.max_sz){  //Synchronisz data within size limits
                        v1.rb_array[rb_idx].pop_count=0;
                        uint64 bp_idx=0, cur_order, bp_step;
                        pte_t *src_leaf_pte=(pte_t *)pa;
                        while(bp_idx<512 && cur_va+PGSIZE * bp_idx < v1.max_sz){
                            if(v1.rb_array[rb_idx].bitmap[bp_idx]!=0){
                                cur_order=get_order(PTE2PA(src_leaf_pte[bp_idx]));
                                bp_step=1ull<<cur_order;
                                if(cur_va+PGSIZE * bp_idx +(1ull<<(ORDER_BASE+cur_order))>=v1.max_sz)
                                    panic("copywalk: cannot handle partial copy!");
                                v1.rb_array[rb_idx].pop_count+=bp_step;
                            }
                            else    bp_step=1;
                            bp_idx+=bp_step;
                        }
                        memset((void *)(v1.rb_array[rb_idx].bitmap + bp_idx), 0, 512-bp_idx);
                    }
                    struct vm_dupl_ctx sub_v={.old_pg=(pagetable_t)pa, .new_pg=(pagetable_t)mem,
                        .base_va=cur_va, .ret_va=v1.ret_va, .max_sz=v1.max_sz, 
                        .level=v1.level-1, .rb_array=v1.rb_array};
                    if(copywalk(sub_v) < 0) return -1;
                }
            }
            else{
                struct vm_dupl_ctx sub_v={.old_pg=(pagetable_t)pa, .new_pg=(pagetable_t)mem,
                    .base_va=cur_va, .ret_va=v1.ret_va, .max_sz=v1.max_sz, 
                    .level=v1.level-1, .rb_array=v1.rb_array};
                if(copywalk(sub_v) < 0) return -1;
            }
            step = 1;
            va_step = standard_stride;
        } 
        else if (pte & PTE_V) {   // Case 2: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 src_phys_size=1ull<<(get_order(pa) + ORDER_BASE);
            uint64 remain_size=v1.max_sz - cur_va;
            int ret=0;
            if(src_phys_size > remain_size){
                ret=copy_partial_block(v1.new_pg, v1.old_pg, cur_va, remain_size, v1.level);
                va_step=remain_size;
            }
            else{
                ret=copy_whole_block(&v1.new_pg[idx], &v1.old_pg[idx], src_phys_size, v1.level);
                va_step=src_phys_size;
            }
            if(ret==0){
                uint64 page_per_slot=1ull<<(v1.level*9);
                uint64 num_4k_page=va_step/PGSIZE;
                step=num_4k_page/page_per_slot;
                if(step<0)  step=1;
            }
            else    return -1;
        }
        else {   
            // Case 3: Invalid / Gap
            step = 1;
            va_step = standard_stride;
        }
        cur_va += va_step;
        idx += step;
        *(uint64 *)v1.ret_va = cur_va;
    }
    return 0;
}

// Free user memory pages,(maintain the interface consistency, 
// an non-zero sz implies the need to release the corresponding page)
// Even though the release process itself does not requires the sz parameter.
// Must free page-table pages.
void uvmfree(res_block *rb_array, pagetable_t pagetable, uint64 sz) {
    reclaim_res_memory_range(rb_array, pagetable, 0, sz);
    if (sz > 0) freewalk(pagetable, 1, 0, sz, 2);
    else    freewalk(pagetable, 0, 0, sz, 2);
}
// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(res_block *rb_array, pagetable_t old_pg, pagetable_t new_pg, uint64 sz) {
#ifdef DEBUG_VM
    VM_TRACE("old_pg=%p new_pg=%p sz=0x%lx\n", (void *)old_pg, (void *)new_pg, sz);
#endif
    uint64 start_va=0;
    uint64 ret_va=0;
    sz=PGROUNDUP(sz);   //Aligned firstly
    struct vm_dupl_ctx v1={.old_pg=old_pg, new_pg=new_pg, .base_va=start_va, .ret_va=&ret_va,
        .max_sz=sz, .level=2, .rb_array=rb_array};
    if(copywalk(v1)<0){//prevent resources leaks and waste
        uvmfree(rb_array, new_pg, ret_va-start_va);
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
//-----NOTE:the following three function are build upon kernel page, user pa is treated as kernel va.--------
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
uint64 vmfault(res_block *rb_array, pagetable_t pagetable, uint64 va, int read) {
    struct proc *p = myproc();
    //Pass arugment rb_array etc explicitly instead of implicitly retriving them via myproc()
    if (va >= p->sz) return 0;
    va = PGROUNDDOWN(va);
    if (ismapped(pagetable, va)) {
        return 0;
    }
    #ifdef RESERVE
        return Simp_alloc_res_memory(rb_array, pagetable, va, PTE_W|PTE_R|PTE_U);
    #else
        uint64 mem = (uint64)kalloc();
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
void merge_into_hugepages_Out(res_block *rb_array, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    //for out-of-place promoted.Expensive copy or move cost.
    if(va% SUPERPGSIZE!=0)  panic("must be 2mb aligned!");
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE, cur_order;
    pte_t *parent_pte=walk(pagetable, va, 1, 1);
    pte_t *old_pte=walk(pagetable, rb_array[idx].va, 1, 0);
    //mapping trigger va using stale xperm(keep same processing flow)
    //Attribute Consistency Check and Accumulation of A/D bits
    uint16 check_perm_mask= PTE_W | PTE_R | PTE_X | PTE_U;
    uint64 expe_perm=0, accu_ad=0, bp_idx=0, delete_pa=PTE2PA(*parent_pte);
    uint8 is_expe_perm_fix=0;
    while(bp_idx<512){
        if(rb_array[idx].bitmap[bp_idx]!=0){
            uint64 flags=PTE_FLAGS(old_pte[bp_idx]);
            if(!(flags & PTE_V))    panic("Bitmap valid but hardware PTE invalid!");
            if(is_expe_perm_fix==0){    //Self-Adaptive Permission Capture
                expe_perm=(flags & check_perm_mask);
                is_expe_perm_fix=1; //Guarantee single initialization
            }
            else if((flags & check_perm_mask) != expe_perm){
                if((flags & PTE_U) != (expe_perm & PTE_U))
                    panic("Inconsisent User/Kernel Privilege!");
                panic("Inconsisent permission!");
            }
            accu_ad |= (flags & (PTE_A | PTE_D));
            //data transfer and release the unused resource
            uint64 prev_pa=PTE2PA(old_pte[bp_idx]);
            cur_order=get_order(prev_pa);
            memmove((void *)(alloced_pa+bp_idx*PGSIZE), (void *)prev_pa, 1ull<<(cur_order+ORDER_BASE));
            bp_idx+=(1ull<<cur_order);  //skip the used memory
        }
        else    bp_idx++;
    }
    expe_perm |= (accu_ad)?(PTE_A | PTE_D):0;
    *parent_pte=expe_perm | PA2PTE(alloced_pa) | PTE_V;
    sfence_vma();   //keep the proper sequence, avoid reusing the freed memory(which record in tlb)
    bp_idx=0;
    while(bp_idx<512){
        if(rb_array[idx].bitmap[bp_idx]!=0){
            uint64 prev_pa=PTE2PA(old_pte[bp_idx]);
            cur_order=get_order(prev_pa);
            free_pages((void *)prev_pa, 1ull<<(ORDER_BASE+cur_order));
            bp_idx+=(1ull<<cur_order);
        }
        else bp_idx++;
    }
    free_pages((void *)delete_pa, PGSIZE);    //free the dirty pagetable
    //data transfer and release the unused resource
}
void merge_into_hugepages_In(res_block *rb_array, pagetable_t pagetable, uint64 va){
    //for in-place promoted, clear the mapping,save the leaf-pte only
    pte_t *parent_pte=walk(pagetable, va, 0, 1);
    uint16 check_perm_mask=PTE_W | PTE_R | PTE_X | PTE_U, cur_order;
    uint64 expe_perm=0, accu_ad=0, bp_idx=0, delete_pa=PTE2PA(*parent_pte);
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE;
    uint8 is_expe_perm_fix=0;
    pte_t *old_pte=walk(pagetable, rb_array[idx].va, 0, 0);
    while(bp_idx<512){
        if(rb_array[idx].bitmap[bp_idx]!=0){
            uint64 flags=PTE_FLAGS(old_pte[bp_idx]);
            if(!(flags & PTE_V))    panic("Bitmap valid but hardware PTE invalid!");
            if(is_expe_perm_fix==0){    //Self-Adaptive Permission Capture
                expe_perm=(flags & check_perm_mask);
                is_expe_perm_fix=1; //Guarantee single initialization
            }
            else if((flags & check_perm_mask) != expe_perm){
                if((flags & PTE_U) != (expe_perm & PTE_U))
                    panic("Inconsisent User/Kernel Privilege!");
                panic("Inconsisent permission!");
            }
            accu_ad |= (flags & (PTE_A | PTE_D));
            //data transfer and release the unused resource
            uint64 prev_pa=PTE2PA(old_pte[bp_idx]);
            cur_order=get_order(prev_pa);
            bp_idx+=(1ull<<cur_order);
        }
        else    bp_idx++;
    }
    *parent_pte=PA2PTE(rb_array[idx].pa) | PTE_V | accu_ad | expe_perm;
    sfence_vma();
    free_pages((void *)delete_pa, PGSIZE);
}
void move_and_aggregate(res_block *rb_array, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    //Used in upgrade is_scattered to zero(aggregrate the seperate data together)
    if(va>rb_array[MAX_RES_BLOCK-1].va+SUPERPGSIZE || va<rb_array[0].va)
        panic("aggregate_data failed:invalid va");
    uint64 idx=(va-rb_array[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, rb_array[idx].va, 0, 0);
    uint64 bp_idx=0, tmp_flags, tmp_pa;
    uint16 cur_order, step;
    while(bp_idx<512){
        if(rb_array[idx].bitmap[bp_idx]!=0){
            tmp_flags=PTE_FLAGS(old_pte[bp_idx]);
            tmp_pa=PTE2PA(old_pte[bp_idx]);
            if(tmp_pa==0)   panic("store invalid ptr!");
            cur_order=get_order(tmp_pa);
            step=1ull<<(ORDER_BASE+cur_order);
            memmove((void *)alloced_pa, (void *)tmp_pa, step);
            old_pte[bp_idx]=PA2PTE(alloced_pa) | tmp_flags;
        }
        else    step=PGSIZE;
        alloced_pa+=step;
        bp_idx+=step/PGSIZE;
    }
    sfence_vma();
    bp_idx=0;   //reset and restart
    while(bp_idx<512){
        if(rb_array[idx].bitmap[bp_idx]!=0){
            tmp_pa=PTE2PA(old_pte[bp_idx]);
            cur_order=get_order(tmp_pa);
            step=1ull<<(ORDER_BASE+cur_order);
            free_pages((void *)tmp_pa, step);
        }
        else    step=PGSIZE;
        bp_idx+=step/PGSIZE;
    }
}
uint64 alloc_res_memory(res_block *rb_array, pagetable_t pagetable, uint64 init_heap_start, 
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
    uint64 start_vpn, vpn_step, new_block;
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
                new_block=(uint64)alloc_memory(SUPERPGSIZE);
                if(new_block==0){   //promoted fail!
                    new_block=(uint64)alloc_memory(vpn_step*PGSIZE);
                    if(new_block==0)    panic("out-of-memory");
                }
                else{
                    rb_array[idx].pa=new_block;
                    rb_array[idx].promoted=1;
                    merge_into_hugepages_Out(rb_array, pagetable, SUPERPGROUNDDOWN(start_va), new_block); 
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

uint64 free_res_memory(res_block *rb_array, pagetable_t pagetable, uint64 init_heap_start, 
        uint64 oldsz, uint64 newsz){
    //Eager Demotion!
    if(init_heap_start % SUPERPGSIZE !=0 )  panic("free_res_memory:pass the unaligned address!");
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
    uint16 idx=(align_newsz-init_heap_start)/SUPERPGSIZE;
    uint64 start_vpn, vpn_step;
    int split_dealloc;
    start_va=align_newsz;
    while(idx<MAX_RES_BLOCK && req_size>0){
        start_vpn=(start_va-rb_array[idx].va)/PGSIZE;
        split_dealloc=(SUPERPGROUNDDOWN(start_va)==SUPERPGROUNDDOWN(oldsz))?0:1;
        if(split_dealloc){
            end_va=(idx+1==MAX_RES_BLOCK)?(rb_array[idx].va+SUPERPGSIZE):rb_array[idx+1].va;
        }
        else    end_va=align_oldsz;
        vpn_step=(end_va-start_va)/PGSIZE;
        if(rb_array[idx].promoted==1){  //Demotion is merely a preliminary step for reclamation.
            //Split regradless of release size due to the lack of intermidate state.
            split_into_blocks(rb_array, pagetable, start_va, 1);  // Demotion for partial uvmunmap
            rb_array[idx].promoted=0;
        }
        uint64 bp_idx=start_vpn, bp_step;
        pte_t *pte=walk(pagetable, start_va, 0, 0);
        while(bp_idx<start_vpn+vpn_step){
            if(rb_array[idx].bitmap[bp_idx]!=0){
                uint64 cur_bp_pa=PTE2PA(*pte);
                uint64 cur_order=get_order(cur_bp_pa);
                bp_step=(1ull<<cur_order);
                rb_array[idx].pop_count-=bp_step;
            }
            else    bp_step=1;
            bp_idx+=bp_step;
            pte+=bp_step;
        }
        uvmdealloc(pagetable, end_va, start_va);    //Finalize the deallocation process.
        memset(rb_array[idx].bitmap+start_vpn, 0, vpn_step);
        req_size-=(end_va-start_va);
        start_va=end_va;
        idx++;
    }
    return newsz;
}
void reclaim_res_memory_range(res_block *rb_array, pagetable_t pagetable, uint64 start_va, uint64 end_va){
    //check if current rb_array is valid and initialize success
    uint64 block_start, block_end, intersect_start, intersect_end;
    uint64 base_addr=rb_array[0].va;  //Save the initial address to enable fast assignment.
    for(int i=0;i<MAX_RES_BLOCK;i++){
        if(rb_array[i].pa==0 || rb_array[i].promoted==1)    continue;
        block_start=rb_array[i].va;
        block_end=rb_array[i].va+SUPERPGSIZE;
        intersect_start=MAX(start_va, block_start);
        intersect_end=MIN(end_va, block_end);
        if(intersect_start>=intersect_end)  continue;   //No intersections, skip
        else if(intersect_end==intersect_start+SUPERPGSIZE){    //full overlap
            pte_t *parent_pte=walk(pagetable, rb_array[i].va, 0, 1);
            uint64 delete_pa=PTE2PA(*parent_pte);
            *parent_pte=0;
            sfence_vma();
            free_pages((void *)delete_pa, PGSIZE);
            free_pages((void *)rb_array[i].pa, SUPERPGSIZE);
            memset(&rb_array[i], 0, sizeof(res_block));
            rb_array[i].va=base_addr+i*SUPERPGSIZE;
        }
        else{   //partial Overlap
            uint64 start_vpn=(intersect_start-rb_array[i].va)/PGSIZE;
            uint64 end_vpn=(intersect_end-rb_array[i].va)/PGSIZE;
            pte_t *old_pte=walk(pagetable, block_start, 0, 0);
            for(int j=start_vpn;j<end_vpn;j++){
                if(old_pte[j]==0)   continue;
                if(rb_array[i].bitmap[j]==0)    panic("Dismatch!");
                old_pte[j]=0;
                rb_array[i].bitmap[j]=0;
                rb_array[i].pop_count--;
            }
        }
    }
}
#ifdef IN_PLACE_PROMOTE
uint64 Simp_alloc_res_memory(res_block *rb_array, pagetable_t pagetable, 
        uint64 va, int xperm){
    //Triggered by a single page fault and allocate exactly one pages.
    va=PGROUNDDOWN(va);
    uint64 new_block=0;
    if(va<rb_array[0].va || va>=rb_array[MAX_RES_BLOCK-1].va+SUPERPGSIZE){
        new_block=(uint64)alloc_memory(PGSIZE);
        if(new_block==0) return 0;
        memset((void *)new_block, 0, PGSIZE);
        if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
            free_pages((void *)new_block, PGSIZE);
            new_block=0;
        }
        return new_block;
    }
    uint16 idx=(va-rb_array[0].va)/SUPERPGSIZE; //now idx must be valid!
    uint64 cur_vpn=(va-rb_array[idx].va)/PGSIZE;
    rb_array[idx].bitmap[cur_vpn]=1;    //Synchronisz data
    rb_array[idx].pop_count++;
    if(rb_array[idx].promoted==0){  //enter reservable region, evaluate reservation strategy
        if(rb_array[idx].is_scattered==1){
            if(rb_array[idx].alloc_attempts<MAX_ALLOWED_ALLOCATIONS){
                new_block=(uint64)alloc_memory(SUPERPGSIZE);
                if(new_block!=0){
                    memset((void *)new_block, 0, SUPERPGSIZE);
                    rb_array[idx].is_scattered=0;   //update
                    rb_array[idx].pa=new_block;
                    if(rb_array[idx].pop_count>1)
                        move_and_aggregate(rb_array, pagetable, va, new_block);//move the data
                    //Only update the pte when new memory is ready to avoid handling intermediate states.
                    if(mappages(pagetable, va, PGSIZE, new_block+cur_vpn*PGSIZE, xperm)!=0){
                        free_pages((void *)new_block, SUPERPGSIZE);
                        rb_array[idx].alloc_attempts++;
                        new_block=0;
                    }
                    else return rb_array[idx].pa+cur_vpn*PGSIZE;
                }
                rb_array[idx].alloc_attempts++;
            }
            new_block=(uint64)alloc_memory(PGSIZE); //when alloc fail or map fail(free already)
            if(new_block==0)    panic("Simp_alloc_res_memory : out-of-memory");
            memset((void *)new_block, 0, PGSIZE);
            if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                free_pages((void *)new_block, PGSIZE);
                new_block=0;
            }
            return new_block;
        }
        else{   // is_scrattered==0
            if(mappages(pagetable, va, PGSIZE, rb_array[idx].pa+cur_vpn*PGSIZE, xperm)!=0)
                panic("Simp_alloc_res_memory : mapping to existing page fail!");
            if(rb_array[idx].pop_count>=THRESHLOD){
                //Supposing existing the huge already,Upgrade page table
                merge_into_hugepages_In(rb_array, pagetable, va);
                rb_array[idx].promoted=1;
            }
        }
    }
    //already promoted, exist mapping.
    return rb_array[idx].pa+cur_vpn*PGSIZE;
}
#else
uint64 Simp_alloc_res_memory(res_block *rb_array, pagetable_t pagetable, uint64 init_heap_start, 
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
            merge_into_hugepages(pagetable, va, new_block);
            rb_array[idx].bitmap[start_vpn]=1;
            return new_block+start_vpn*PGSIZE;
        }
        else{   
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
        }
    }
    //already promoted, exist mapping.
    return new_block;
}
#endif