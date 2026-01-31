#include "param.h"
#include "types.h"
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
#include "rbtree.h"
#include "mm.h"
#include "vm.h"
#include "colors.h"
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

struct spinlock rmap_lock;  //All operations(reading, writing ...) of the pte.

// res_block rblocks[MAX_RES_BLOCK] __attribute__((unused)) ={0};  //static Global variable
void walk_all_page(uint64 start_va, pagetable_t pagetable, int level);

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

pte_t *walk_internal(pagetable_t pagetable, uint64 va, int alloc, int target_level, int *found_level) {
    if (va >= MAXVA) panic("walk");
    for (int level = 2; level > target_level; level--) {    //Tips: start level can be changed!
        pte_t *pte = &pagetable[PX(level, va)]; //math
        if (*pte & PTE_V) { //Locate the next level pagetable using pte(create and init if necessary)
        //Superpage support
            if (PTE_LEAF(*pte)){    //early stop(record the current-level)
                if(found_level!=NULL)   *found_level=level;
                return pte;
            }
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc || (pagetable = (pde_t *)kalloc_page()) == 0) return 0;
            //create a new mappaing.
            memset(pagetable, 0, PGSIZE);
            acquire(&rmap_lock);
            *pte = PA2PTE(pagetable) | PTE_V;
            //sync_rmap(pagetable, PGSIZE, pte);
            release(&rmap_lock);
            //Don't set R/W/X, so this is directory entry,not a superpage
        }
    }
    if(found_level!=NULL)   *found_level=target_level;
    return &pagetable[PX(target_level, va)];
}
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, int target_level){
    return walk_internal(pagetable, va, alloc, target_level, NULL);//wrapper
}
static inline uint64 get_step_size(int level){
    if(level==2)    return 1ull<<30;
    else if(level==1)   return 1ull<<21;
    else if(level==0)   return 1ull<<12;
    return 0;
}

static inline int get_shift(int level){
    return (level *9 + 12);
}

// Look up a virtual address, return the physical address, or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA) return 0;
    int found_level=0;  //Consider hugeleaf
    pte = walk_internal(pagetable, va, 0, 0, &found_level);
    if (pte == 0) return 0;
    if ((*pte & PTE_V) == 0) return 0;
    if ((*pte & PTE_U) == 0) return 0;
    pa = PTE2PA(*pte);
    if(found_level!=0)  pa+=(va % get_step_size(found_level));
    return pa;
}
// #if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
void prefix_helper(char c, int count){
    int i=count;
    while(i-- > 0)  printf("%c", c);
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
                printf("(data)%p: pte %p pa %p, size is 0x%llx\n", 
                    (void *)start_va, (void *)pte, (void *)pa, va_step);
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
// #endif
void init_res_array(res_block *rblocks, uint64 init_heap_start){    //Assuming A fixed-size stack
    //Limited by the physical memory, cannot reach MAXVA
    uint64 align_2mb_hs=SUPERPGROUNDUP(init_heap_start);
    memset((void *)rblocks, 0, sizeof(res_block)*MAX_RES_BLOCK);
    for(int i=0;i<MAX_RES_BLOCK;i++){
        rblocks[i].va=align_2mb_hs;
        rblocks[i].is_scattered=1;
        align_2mb_hs+=SUPERPGSIZE;
    }
}

// add a mapping to the kernel page table only used when booting.
// does not flush TLB or enable paging.
//NOTE: Paired with alloc(Buddy-system), it can be guarantee pa follows Buddy-system's prescribed size;
// When pa is suffiencently large and va meets the alignment requirements, we prefer mappage into larger pages.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx pa=%p perm=0x%x\n", (void *)va, size, (void *)pa, perm);
#endif
    // if(pa != (1ull<<i_log2(pa)))   panic("Bypass the buddy system, passing invalid pa!\n");
    pte_t *pte=NULL;
    uint64 basic_size=PGSIZE, target_level=0, prev_level=3;   //determine rewalk necessity
    if(size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (size == 0) panic("mappages: size: 0");
    while(size>0){
        if(va%GIGAPGSIZE==0 && size>=GIGAPGSIZE){
            target_level=2;
            basic_size=GIGAPGSIZE;
        } 
        else if(va%MEGAPGSIZE==0 && size>=MEGAPGSIZE){
            target_level=1;
            basic_size=MEGAPGSIZE;
        }
        else{
            target_level=0;
            basic_size=PGSIZE;
        }
        if ((pa % basic_size) != 0) panic("mappages: pa not aligned");
        if(prev_level!=target_level || PX(target_level, va)==0){
            //check if need jump into other level or arrive the boundary
            pte=walk(pagetable, va, 1, target_level);//walk again
            prev_level=target_level;
        }
        else    pte++;  //update the pte quickly,no need to walk agin
        if(*pte & PTE_V)    panic("mappages: remap");
        //check if it's the last vpns to 
        *pte= PA2PTE(pa) | PTE_V | perm; //Assign
        size-=basic_size;
        va+=basic_size;
        pa+=basic_size;
        //update basic_size(and target level) based on the current maximum alignment value.
    }
    return 0;
}

void split_into_blocks(res_block * rblocks, pagetable_t pagetable, uint64 va, uint64 cur_level){
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, va, 0, cur_level);
    pagetable_t new_pagetable=alloc_memory(PGSIZE);
    if(new_pagetable==NULL) panic("Split-into-blocks:OOM");
    memset((void *)new_pagetable, 0, PGSIZE);
    uint64 cur_pa=PTE2PA(*old_pte), basic_stride=get_step_size(cur_level-1), delete_pa=cur_pa;
    void *new_block_pa;
    if(new_pagetable==NULL){
        panic("Out-of-memory!");
    }
    for(int i=0;i<512;i++){
        if(rblocks[idx].bitmap[i]!=0){
            //Inherits RXW permissions from the original huge page, becoming a new small leaf.
            new_block_pa=alloc_memory(basic_stride);
            if(new_block_pa==NULL){
                panic("out-of-memory!");
            }
            //Insufficient intermediate space.Update failed.
            memmove(new_block_pa, (void *)cur_pa, basic_stride);
            new_pagetable[i]=PA2PTE((uint64)new_block_pa) | PTE_FLAGS(*old_pte);
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
    if(new_pagetable==0){
        panic("out-of-memory");
    }
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
    pagetable = (pagetable_t)kalloc_page();
    if (pagetable == 0) return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

uint64 uvmunmap_helper(pagetable_t pagetable, uint64 va, uint64 size, int cur_level, int do_free){
    //Unmap and then free physical resource(if necessary)
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx do_free=%d, cur_level is %d\n", (void *)va, size, do_free, cur_level);
#endif
    uint64 basic_stride=get_step_size(cur_level);
    uint64 cur_vpn=PX(cur_level, va), start_page_offset=va & (basic_stride-1);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(start_page_offset+size+basic_stride-1)/basic_stride;    //exclude end_vpn
    if(end_vpn>512){
        #ifdef DEBUG_VM
            VM_TRACE("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
            VM_TRACE("size=0x%llx, cur_level=%d, end_vpn=0x%llx\n", size, cur_level, end_vpn);
        #endif
        panic("uvmunmap_helper!");
        return -1;
    }
    uint64 pa, vpn_incr, cur_order=0, del_size=0, pending;
    while(cur_vpn<end_vpn){
        pte=&pagetable[cur_vpn];
        uint64 page_offset=va & (basic_stride-1);
        if((*pte & PTE_V)==0){
            cur_vpn++;
            pending=MIN(basic_stride-page_offset, size-del_size);
            del_size+=pending;
            va+=pending;
        }
        else if(PTE_LEAF(*pte)){
            pa=PTE2PA(*pte);
            if(is_managed_memory(pa)==0){   //Outside RAM region, maybe trampoline or trapframe
                //This is safe as current implementation does not invoke a free operations on these area.
                VM_TRACE("Uvmunmap dangerout area(outside the RAM region)\n");
                pte[0]=0;
                va+=basic_stride;
                del_size+=basic_stride;
                cur_vpn++;
            }
            else if(page_offset !=0 || size-del_size<basic_stride){
                //Partial Unmap and use Pruned Spltting Strategy
                pending=MIN(size-del_size, basic_stride-page_offset);
                pte_t tmp_pte=*pte; //Old bigger page pte
                *pte=0; //Unmap to prevent remap error!
                if(cur_level<=0)    panic("Try to Split the minimun Unit");
                uint64 child_start_vpn=PX(cur_level-1, va), child_end_vpn=PX(cur_level-1, va+pending);
                child_end_vpn=(child_end_vpn==0)?512:child_end_vpn;
                *pte=PA2PTE((uint64)split_and_prune(tmp_pte, child_start_vpn, child_end_vpn, cur_level)) | PTE_V;
                if(do_free){    
                    //Calculate the free_start_pa(the Only place still remember the original physical address)
                    uint64 free_start_pa=PTE2PA(tmp_pte)+child_start_vpn*get_step_size(cur_level-1);
                    free_pages((void *)free_start_pa, pending);
                }
                if(pending==size-del_size)  return size;
                del_size+=pending;
                va+=pending;
                cur_vpn++;    //across two pages, continue handling(at current level)
            }
            else{   //full Unmap(basic_stride < size-del_size)
                cur_order=get_order(pa);    //Return 0 while pa located in a huge page
                pending=MIN(1ull<<(cur_order+ORDER_BASE), size-del_size);
                if(do_free!=0)  free_pages((void *)pa, pending); //free 
                if(cur_order < cur_level*9) panic("dismatch\n");    //Only allocte a single smaller page
                vpn_incr=1ull << (cur_order - cur_level*9);
                for(int i=0;i<vpn_incr;i++)
                    if(cur_vpn+i<end_vpn) pte[i]=0;
                del_size+=pending;
                va+=pending;
                cur_vpn+=vpn_incr;
            }
        }
        else{   //directory page:Split and delegate the task to the next-level page table! 
            pending=MIN(basic_stride-page_offset, size-del_size);
            uvmunmap_helper((pagetable_t)PTE2PA(*pte), va, pending, cur_level-1, do_free);
            //NOTE: Adopt Lazy reclaim mechanism: avoid thrashing, the cost of verification,
            // and Concurrency hell. Only two specific moment take earge reclaim:
            // exit/freeproc(use freewalk instead), memory pressure(implemented by backprocess)
            // if(is_directory_empty((pagetable_t)PTE2PA(*pte))){
            //     uint64 child_pa=PTE2PA(*pte);
            //     free_pages((void *)child_pa, PGSIZE);
            //     *pte=0; //clear the pte
            // }
            va+=pending;
            del_size+=pending;
            cur_vpn++;
        }
    }
    return del_size;
}

uint64 Simp_uvmunmap(pagetable_t pagetable, uint64 va, uint64 size, int cur_level){
    //Dedicated to reclaim some area outside RAM kernel pagetable
    //Out of the buddy-system control, perform simple allocate logic without "order"
    uint64 basic_stride=get_step_size(cur_level);
    uint64 cur_vpn=PX(cur_level, va), start_page_offset=va & (basic_stride-1);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(start_page_offset+size+basic_stride-1)/basic_stride;    //exclude end_vpn
    if(end_vpn>512){
        #ifdef DEBUG_VM
            VM_TRACE("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
            VM_TRACE("size=0x%llx, cur_level=%d, end_vpn=0x%llx\n", size, cur_level, end_vpn);
        #endif
        panic("uvmunmap");
    }
    uint64 del_size=0, pending;
    while(cur_vpn<end_vpn){
        pte=&pagetable[cur_vpn];
        uint64 page_offset=va & (basic_stride-1);
        if((*pte & PTE_V)==0){
            pending=MIN(basic_stride-page_offset, size-del_size);
        }
        else if(PTE_LEAF(*pte)){
            if(page_offset !=0 || size-del_size<basic_stride){
                //Partial Unmap and use Pruned Splitting Strategy
                pending=MIN(size-del_size, basic_stride-page_offset);
                pte_t tmp_pte=*pte; //Old bigger page pte
                *pte=0; //Unmap to prevent remap error!
                if(cur_level<=0)    panic("Try to Split the minimal Unit");
                uint64 child_start_vpn=PX(cur_level-1, va), child_end_vpn=PX(cur_level-1, va+pending);
                child_end_vpn=(child_end_vpn==0)?512:child_end_vpn;
                *pte=PA2PTE((uint64)split_and_prune(tmp_pte, child_start_vpn, child_end_vpn, cur_level)) | PTE_V;
                if(pending==size-del_size)  return size;
                //across two pages, continue handling(at current level)
            }
            else{   //full Unmap(basic_stride < size-del_size)
                *pte=0;
                pending=basic_stride;
            }
        }
        else{   //directory page:Split and delegate the task to the next-level page table! 
            pending=MIN(basic_stride-page_offset, size-del_size);
            Simp_uvmunmap((pagetable_t)PTE2PA(*pte), va, pending, cur_level-1);
        }
        cur_vpn++;
        va+=pending;
        del_size+=pending;
    }
    return del_size;
}

void uvmunmap(pagetable_t pagetable, uint64 va, uint64 size, int do_free){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx do_free=%d\n", (void *)va, size, do_free);
#endif
    if(size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (size == 0) panic("mappages: size");
    uint64 del_size=uvmunmap_helper(pagetable, va, size, 2, do_free);
    if(del_size!=size)
        panic("uvmunmap: cannot unmap required size!");
}

//--------------------ALLOC--------------------------
int buddy_alloc(pagetable_t pagetable, uint64 va, uint64 size, int xperm){
    //return 0 when Success , and return -1 when it fail!
    //A shared utility for use and kernel space.
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
            VM_TRACE("uvmalloc failed to allocate cur_va=0x%llx size=0x%llx\n", cur_va, alloc_size);
            uvmdealloc(pagetable, cur_va, va);
            return -1;
        }
        memset(mem, 0, alloc_size);
        VM_TRACE("uvmalloc -> mappages cur_va=0x%llx alloc_size=0x%llx\n", cur_va, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, (uint64)mem, xperm)!=0){
            free_pages(mem, alloc_size);
            VM_TRACE("uvmalloc mappages failed at cur_va=0x%llx size=0x%llx\n", cur_va, alloc_size);
            uvmdealloc(pagetable, cur_va, va);
            return -1;
        }
        if(size>alloc_size)  size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        va_order=i_log2(cur_va & -cur_va);
        va_order=(va_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):va_order;
        cur_max_size=1ull << va_order;
    }
    return 0;
}

//--------------------introduce heap reservation------------------------
int recursive_copy_map(pte_t *dst_pg, pte_t *src_pg, uint64 size, int level){
    //Raw version(mixed two difficult situation, cannot work)
    uint64 basic_stride=get_step_size(level);
    int i=0;
    while(i<512 && size>0){ //start with zero(always)
        pte_t src_entry=src_pg[i];
        uint64 src_pa=PTE2PA(src_entry);
        if(size >= basic_stride){   //Perfect fit(considering buddy_system)
            uint64 src_phy_size;
            if(level==0)    src_phy_size=1ull<<(get_order(src_pa) + ORDER_BASE);
            else    src_phy_size=basic_stride;
            uint64 num_4k_page=src_phy_size/PGSIZE, page_per_slot=1ull<<(9*level);
            uint64 step=num_4k_page/page_per_slot;
            void *mem=alloc_memory(src_phy_size);
            if(mem==NULL){
                VM_TRACE("");
                return -1;
            }
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
                if(new_dst_pg==NULL){
                    VM_TRACE("");
                    return -1;
                }
                dst_pg[i]=PA2PTE((uint64)new_dst_pg) | PTE_V;
                pte_t *next_src_table=(pte_t *)PTE2PA(src_entry);
                return recursive_copy_map(new_dst_pg, next_src_table, size, level-1);
            }
            else{
                void *dst_mem=alloc_memory(PGSIZE);
                memmove(dst_mem, (void *)src_pa, PGSIZE);
                src_pg[i]=PA2PTE((uint64)dst_mem) | PTE_FLAGS(src_pg[i]);
                return 0;
            }
        }
    }
    return 0;
}

//The Partial is proposed specifically for buddy_block
//make sure the input pagetable is complete and can start with index 0
int copy_partial_block(struct vm_sub_copy_ctx *p1){   //Reuse ret_va as src
    uint64 basic_stride=get_step_size(p1->level);
    int i= p1->base_va >> get_shift(p1->level) & 0x1ff;
    uint64 src_flags=PTE_FLAGS(p1->src_pt[i]), src_pa=PTE2PA(p1->src_pt[i]);
    while(i<512 && p1->size>0){ //start with zero(always)
        if(p1->size >= basic_stride){   //Perfect fit(considering buddy_system)
            if(p1->base_va >= p1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE)
                panic("copy_partial_block: out-of-range!");
            uint64 chunk_size;
            //Pseduo-splitting of the heap reservation area, while maintaining physical continuity.
            if(p1->level== 0 && p1->base_va >= p1->rblocks[0].va){ //heap reservation Interception
                if(i!=0)    panic("Error in partial copy reservation area!");
                chunk_size=p1->size; //fixed as superpgsize(for reservation mechanism)
                uint64 rb_idx=(p1->base_va - p1->rblocks[0].va)/SUPERPGSIZE;
                void *mem=alloc_memory(SUPERPGSIZE);
                if(mem==NULL){
                    VM_TRACE("");
                    return -1;
                }
                memset(mem, 0, SUPERPGSIZE);
                //partial copy,should initialize first
                memmove(mem, (void *)src_pa, chunk_size);
                p1->rblocks[rb_idx].pa=(uint64)mem;
                p1->rblocks[rb_idx].promoted=0;
                p1->rblocks[rb_idx].is_scattered=0;
                p1->rblocks[rb_idx].pop_count=0;
                //record metadata.
                uint64 valid_pte_size=p1->size/PGSIZE;
                if(valid_pte_size>=512) panic("");
                for(int j=0;j<valid_pte_size;j++){
                    uint64 inner_pa=(uint64)mem + j*PGSIZE;
                    p1->dst_pt[j]=PA2PTE(inner_pa) | src_flags;
                    p1->rblocks[rb_idx].pop_count++;
                }
                memset((void *)&p1->rblocks[rb_idx].bitmap[valid_pte_size], 0, 512-valid_pte_size);
                return 0;
            }else{
                //Out of the heap area, and copy size oversize current stride.
                //Use Greedy alignment, that is friendly for buddy-system.
                uint64 alignment_limit=(src_pa | p1->base_va);
                uint64 max_order=i_log2(alignment_limit & -alignment_limit);
                if(max_order==0)    max_order=(ORDER_BASE+MAX_ORDER);
                chunk_size=1ull<<max_order;
                while(chunk_size > p1->size)
                    chunk_size/=2;
                uint64 num_4k_page=chunk_size/PGSIZE, page_per_slot=1ull<<(9*p1->level);
                uint64 step=num_4k_page/page_per_slot;
                void *mem=alloc_memory(chunk_size);
                if(mem==NULL){
                    VM_TRACE("");
                    return -1;
                }
                memmove(mem, (void *)src_pa, chunk_size);
                if(step <= 0) step = 1;
                for(int j = 0; j < step; j++){
                    // Reset flags from original entry
                    uint64 inner_pa = (uint64)mem + j * page_per_slot * PGSIZE;
                    p1->dst_pt[i+j] = PA2PTE(inner_pa) | src_flags;
                }
                i+=step;
                p1->size-=chunk_size;
                src_pa+=chunk_size;
                p1->base_va+=chunk_size;
            }
        }
        else{   //Partial/Split(p1->max_sz < basic_stride)
            if(p1->level!=0){    //Current copy is insuffient for filling current level stride
                //Or it is an intermidate node.(alloc one pagetable and enter sub-level recursively.)
                pte_t *new_dst_pt=(pte_t *)alloc_memory(PGSIZE);
                if(new_dst_pt==NULL){
                    VM_TRACE("");
                    return -1;
                }
                memset((void *)new_dst_pt, 0, PGSIZE);
                p1->dst_pt[i]=PA2PTE((uint64)new_dst_pt) | PTE_V;
                p1->level--;
                p1->dst_pt=new_dst_pt;
                return copy_partial_block(p1);
            }
            else{
                //The lowest level, minimun size is PGSIZE.
                void *dst_mem=alloc_memory(PGSIZE);
                memmove(dst_mem, (void *)src_pa, PGSIZE);
                p1->dst_pt[i]=PA2PTE((uint64)dst_mem) | src_flags;
                return 0;
            }
        }
    }
    return 0;
}

//Copy the whole buddy_block quickly.
int copy_whole_block(struct vm_sub_copy_ctx *w1){
    int start_pfn = w1->base_va >> get_shift(w1->level) & 0x1ff;
    uint64 src_pa=PTE2PA(w1->src_pt[start_pfn]);  //Extract the start pa
    //Perfect fit(considering buddy_system)
    uint64 num_4k_page=w1->size/PGSIZE, page_per_slot=1ull<<(9*w1->level);
    uint64 step=num_4k_page/page_per_slot;
    void *mem=alloc_memory(w1->size);
    if(mem==NULL)   return -1;
    memmove(mem, (void *)src_pa, w1->size);
    if(step <= 0) step = 1;
    for(int j; j < step; j++){
        // Reset flags from original entry
        uint64 inner_pa = (uint64)mem + j * page_per_slot * PGSIZE;
        w1->dst_pt[j+start_pfn] = PA2PTE(inner_pa) | PTE_FLAGS(w1->src_pt[j+start_pfn]);
    }
    if(w1->base_va >= w1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE)    panic("out-of-range!");
    if(w1->level==1 && w1->base_va >= w1->rblocks[0].va){
        if(w1->size!=SUPERPGSIZE)    panic("Sequential huge pages detected in the heap!");
        uint64 rb_idx=(w1->base_va - w1->rblocks[0].va)/SUPERPGSIZE;
        w1->rblocks[rb_idx].pa=(uint64)mem;
    }
    return 0;
}
// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
#ifdef DEBUG_VM
    VM_TRACE("oldsz=0x%llx newsz=0x%llx xperm=0x%x\n", oldsz, newsz, xperm);
#endif
    if(newsz<oldsz) return oldsz;
    uint64 aligned_oldsz=PGROUNDUP(oldsz), aligned_newsz=PGROUNDUP(newsz);
    if(aligned_oldsz==aligned_newsz)    return newsz;
    uint64 remain_size=aligned_newsz-aligned_oldsz;
    if(buddy_alloc(pagetable, aligned_oldsz, remain_size, xperm | PTE_R | PTE_U)!=0)
        return 0;
    VM_TRACE("uvmalloc exiting success newsz=0x%llx\n", newsz);
    return newsz;
}

//-------------------DEALLOC------------------------------
//Keep the same logic as uvmalloc(Binary Buddy Decomposition!)
//A User/Kernel agnostic function.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
#ifdef DEBUG_VM
    VM_TRACE("oldsz=0x%llx newsz=0x%llx\n", oldsz, newsz);
#endif
    if (newsz >= oldsz) {
        VM_TRACE("uvmdealloc no-op new>=old, returning oldsz=0x%llx\n", oldsz);
        return oldsz;
    }
    uint64 aligned_newsz=PGROUNDUP(newsz), aligned_oldsz=PGROUNDUP(oldsz);
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
                VM_TRACE("uvmdealloc -> uvmunmap va=0x%llx size=0x%llx, remain size is 0x%llx\n", 
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
    VM_TRACE("uvmdealloc exiting newsz=0x%llx\n", newsz);
    return newsz;
}

//----------------------END dealloc------------------------------------------------------

// Recursively free page-table pages totally 
void freewalk(pagetable_t pagetable, int do_free, int level) {
#ifdef DEBUG_VM
    if (base_va == 0 && level == 2)
        VM_TRACE("Freewalk Start: pt=%p base_va=0x%llx max_sz=0x%llx\n", pagetable, base_va, max_sz);
#endif
    pte_t pte;// there are 2^9 = 512 PTEs in a page table.
    uint64 pa, num_4k_page, page_per_slot, step;
    uint64 va_step, cur_order;
    int idx=0;
    while(idx<512){
        pte = pagetable[idx];
        pa = PTE2PA(pte);    //child pagetable or physical
        if ((pte & PTE_V) && PTE_LEAF(pte) == 0) {
            // this PTE points to a lower-level page table.
            #ifdef DEBUG_VM
                VM_TRACE("%sDir: idx=%d va=0x%llx -> next_pa=%p\n", INDENT_STR(level), idx, cur_va, (void*)pa);
            #endif
            freewalk((pagetable_t)pa, do_free, level-1);
            step=1;
        } else if ((pte & PTE_V) && do_free!=0) {   //leaf-node,release the physical page
            //check if next page is associated with current page
            cur_order=get_order(pa);    //Specific area(Cannot be freed),so skip additional checks.
            va_step=1ull<<(cur_order+ORDER_BASE);
            num_4k_page=1ull<<cur_order;
            page_per_slot=1ull<<(level*9);
            step=num_4k_page/page_per_slot;
            if(step<=0) step=1; //at least advance one
            #ifdef DEBUG_VM
            VM_TRACE("%sLeaf: idx=%d va=0x%llx pa=%p order=%llu size=0x%llx (step=%lld)\n", 
                     INDENT_STR(level), idx, cur_va, (void*)pa, cur_order, va_step, step);
            #endif
            free_pages((void *)pa, va_step); //record statement before freeing, 
            // as the merging process potentially alter the metadata.
        }
        else    step=1; //Invalid pte
        for(int j=0;j<step;j++){
            if(idx+j==512){
                panic("freewalk: alignment error");
                break;
            }
            pagetable[idx+j]=0; //Clear the relevant PTEs
        }
        idx+=step;
    }
    free_pages((void *)pagetable, PGSIZE);
}

void freewalk_limit(pagetable_t pagetable, int do_free, uint64 base_va, uint64 max_sz, int level) {
    #ifdef DEBUG_VM
        if (base_va == 0 && level == 2)
            VM_TRACE("Freewalk Start: pt=%p base_va=0x%llx max_sz=0x%llx\n", pagetable, base_va, max_sz);
    #endif
        pte_t pte;// there are 2^9 = 512 PTEs in a page table.
        uint64 pa, num_4k_page, page_per_slot, step;
        uint64 standard_stride=get_step_size(level), cur_va=base_va, va_step;
        uint64 cur_order;
        int idx= base_va >> get_shift(level) & 0x1ff;
        while(idx<512){
            if(cur_va>=max_sz)   break;
            pte = pagetable[idx];
            pa = PTE2PA(pte);    //child pagetable or physical
            if ((pte & PTE_V) && PTE_LEAF(pte) == 0) {
                // this PTE points to a lower-level page table.
                #ifdef DEBUG_VM
                    VM_TRACE("%sDir: idx=%d va=0x%llx -> next_pa=%p\n", INDENT_STR(level), idx, cur_va, (void*)pa);
                #endif
                freewalk_limit((pagetable_t)pa, do_free, cur_va, max_sz, level-1);
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
                VM_TRACE("%sLeaf: idx=%d va=0x%llx pa=%p order=%llu size=0x%llx (step=%lld)\n", 
                         INDENT_STR(level), idx, cur_va, (void*)pa, cur_order, va_step, step);
                #endif
                free_pages((void *)pa, va_step); //record statement before freeing, 
                // as the merging process potentially alter the metadata.
            }
            else{
                step=1; //already Invalid pte,just skip.
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

// Recursively copy page-table pages. Quit recursively When the superpage Error occurs, 
// Directly assign the known PTE to bypass the mappage overhead, significantly improving efficiency
int copywalk(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->level), cur_va=v1->base_va, va_step;
    uint64 step, flags;
    char *mem;
    int idx= v1->base_va >> get_shift(v1->level) & 0x1ff;
    while(idx < 512) {
        if(cur_va >= v1->end_va) break;
        pte = v1->old_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if ((pte & PTE_V) && PTE_LEAF(pte)==0) {    // Case 1: Directory
            new_pte=v1->new_pg[idx];
            if((new_pte & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE);
                if(mem == 0) return -1;
                memset(mem, 0, PGSIZE);
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->new_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    return -1;
                }
            }
            if(v1->level==1 && cur_va>=v1->rblocks[0].va){
                 //enter heap region(Recursion Short-circuiting)
                // promote==0 must be true upon entering this block.
                // Use one huge page to manager all data, and for pte keep the same as parent.
                if(cur_va>=v1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE) return -1;
                uint64 rb_idx=(cur_va-v1->rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
                if(cur_va + SUPERPGSIZE < v1->end_va && v1->rblocks[rb_idx].is_scattered==0
                        && v1->rblocks[rb_idx].pa!=0){  //have allocated hugepage
                    uint64 new_superpage=(uint64)alloc_memory(SUPERPGSIZE);
                    if(new_superpage==0)    return -1;
                    memmove((void *)new_superpage, (void *)(v1->rblocks[rb_idx].pa), SUPERPGSIZE);    //migrate data
                    uint64 bp_idx=0, cur_order, bp_step;
                    pte_t *src_leaf_pte=(pte_t *)pa, *dst_leaf_pte=(pte_t *)mem;
                    while(bp_idx<512){
                        if(v1->rblocks[rb_idx].bitmap[bp_idx]!=0){
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
                    v1->rblocks[rb_idx].pa=new_superpage;//Update PA last;preserve remaining parameters.
                }
                else{   //Scrattered Situation
                    v1->rblocks[rb_idx].pa=0;
                    v1->rblocks[rb_idx].is_scattered=1;
                    v1->rblocks[rb_idx].promoted=0;
                    if(cur_va + SUPERPGSIZE >=v1->end_va){  //Synchronisz data within size limits
                        v1->rblocks[rb_idx].pop_count=0;
                        uint64 bp_idx=0, cur_order, bp_step;
                        pte_t *src_leaf_pte=(pte_t *)pa;
                        while(bp_idx<512 && cur_va+PGSIZE * bp_idx < v1->end_va){
                            if(v1->rblocks[rb_idx].bitmap[bp_idx]!=0){
                                cur_order=get_order(PTE2PA(src_leaf_pte[bp_idx]));
                                bp_step=1ull<<cur_order;
                                if(cur_va+PGSIZE * bp_idx +(1ull<<(ORDER_BASE+cur_order))>=v1->end_va)
                                    panic("copywalk: cannot handle partial copy!");
                                v1->rblocks[rb_idx].pop_count+=bp_step;
                            }
                            else    bp_step=1;
                            bp_idx+=bp_step;
                        }
                        memset((void *)(v1->rblocks[rb_idx].bitmap + bp_idx), 0, 512-bp_idx);
                    }
                    if(copywalk(&(struct vm_dupl_ctx)
                                {.old_pg=(pagetable_t)pa,
                                .new_pg=(pagetable_t)mem,
                                .base_va=cur_va,
                                .ret_va=v1->ret_va,
                                .end_va=v1->end_va,
                                .level=v1->level-1,
                                .rblocks=v1->rblocks})<0)
                        return -1;
                }
            }
            else{   //Normal area(treat as Stardard Page Directory)
                if(copywalk(&(struct vm_dupl_ctx)
                            {.old_pg=(pagetable_t)pa,
                            .new_pg=(pagetable_t)mem,
                            .base_va=cur_va,
                            .ret_va=v1->ret_va,
                            .end_va=v1->end_va,
                            .level=v1->level-1,
                            .rblocks=v1->rblocks})<0)
                    return -1;
            }
            step = 1;
            va_step = standard_stride;
        } 
        else if (pte & PTE_V) {   // Case 2: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 chunk_size=1ull<<(get_order(pa) + ORDER_BASE);
            uint64 remain_size=v1->end_va - cur_va;
            int ret=0;
            if(chunk_size > remain_size){
                ret=copy_partial_block(&(struct vm_sub_copy_ctx)
                        {.dst_pt=(pte_t *)v1->new_pg, 
                        .src_pt=(pte_t *)v1->old_pg, 
                        .base_va=cur_va, 
                        .size=remain_size, 
                        .level=v1->level, 
                        .rblocks=v1->rblocks});
                va_step=remain_size;
            }
            else{
                ret=copy_whole_block(&(struct vm_sub_copy_ctx)
                        {.dst_pt=(pte_t *)v1->new_pg, 
                        .src_pt=(pte_t *)v1->old_pg, 
                        .base_va=cur_va, 
                        .size=chunk_size, 
                        .level=v1->level,
                        .rblocks=v1->rblocks});
                va_step=chunk_size;
            }
            if(ret==0){
                uint64 page_per_slot=1ull<<(v1->level*9);
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
        *(uint64 *)v1->ret_va = cur_va;
    }
    return 0;
}

// Free user memory pages,(maintain the interface consistency, 
// an non-zero sz implies the need to release the corresponding page)
// Even though the release process itself does not requires the sz parameter.
// Must free page-table pages.
void uvmfree_range(res_block *rblocks, pagetable_t pagetable, uint64 start_va, uint64 sz) {
    reclaim_res_memory_range(rblocks, pagetable, 0, sz);
    if (sz > 0) freewalk_limit(pagetable, 1, start_va, sz, 2);
    else    freewalk_limit(pagetable, 0, start_va, sz, 2);
}

// Given a parent process's page table, copy its memory into a child's page table.
// Copies both the page table and the physical memory.
// returns 0 on success, -1 on failure and also frees any allocated pages
//NOTE: make sure new_pg is clean, otherwise some memory corruption will occur.
int uvmcopy_range(res_block *rblocks, pagetable_t old_pg, pagetable_t new_pg, 
                    uint64 start_va, uint64 sz) {
#ifdef DEBUG_VM
    VM_TRACE("old_pg=%p new_pg=%p sz=0x%llx\n", (void *)old_pg, (void *)new_pg, sz);
#endif
    pr_err("old_pg=%p new_pg=%p sz=0x%llx\n", (void *)old_pg, (void *)new_pg, sz);
    uint64 ret_va=0;
    sz=PGROUNDUP(sz);   //Aligned firstly
    if(copywalk(&(struct vm_dupl_ctx)
                {.old_pg=old_pg,
                .new_pg=new_pg,
                .base_va=start_va,
                .ret_va=&ret_va,
                .end_va=start_va+sz,
                .level=2,
                .rblocks=rblocks})<0)
    {    //prevent resources leaks and waste
        uvmfree_range(rblocks, new_pg, start_va ,ret_va-start_va);
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk failed.");
        return -1;
    }
#ifdef DEBUG_VM
    VM_TRACE("after uvmcopy start_va is 0x%llx, while ret_va is 0x%llx\n", start_va, ret_va);
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
#define PROACTIVE_CHECK

//handle file-backend page fault, support arbitrary size
int vmfile_load(vm_area_struct_t *vma, uint64 va, uint64 dst_pa, uint64 size){
    uint64 offset=vma->vm_pgoff + (va-vma->vm_start);
    uint len=(va+size > vma->vm_end)?(vma->vm_end-va):size;
    begin_op();
    ilock(vma->vm_file->ip);
    uint64 file_offset=vma->vm_pgoff + offset;
    if(file_offset < vma->vm_file->ip->size){
        len=(vma->vm_file->ip->size - file_offset < size)?
                (vma->vm_file->ip->size-file_offset):size;
        if (readi(vma->vm_file->ip, 0, dst_pa, offset, len) != len)
            pr_err("load from %llx fail(Expected is %lx)", va, len);
    }
    iunlockput(vma->vm_file->ip);
    end_op();
    return 0;
}

uint64 scan_contigous_map(pagetable_t pagetable, uint64 va, uint64 limit){
    // Traverse the page table entries to determine the maximum length 
    // of physically contiguous memory mapped by the current PTE range
    int level, src_level;
    if(limit >= 4096 && limit <512*4096)    level=0;
    else if(limit >=512*4096 && limit<512*512*4096) level=1;
    else    level=2;        //Highest level.
    pte_t *src_pte=walk_internal(pagetable, va, 0, 0, &src_level), *l_pte=*src_pte;
    if(src_pte==NULL || (*src_pte & PTE_V)==0)   return 0;
    if(src_level!=level){
        pr_warn("level dismatch.");
        return 0;
    }
    uint64 cur_chunk_len=0, next_va=va;
    while(cur_chunk_len < limit){
        next_va=va+cur_chunk_len;
        next_pte=
    }
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    //Proactive check, instead of raise page fault frequently.
    uint64 aligned_dstva=PGROUNDDOWN(dstva);
    uint8 cur_order=i_log2(aligned_dstva & -aligned_dstva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):cur_order;
    uint64 cur_max_size=1ull << cur_order;
    uint64 alloc_size=cur_max_size, copy_size, mem;
    uint64 cur_va=dstva, basepage_va=aligned_dstva, cur_pa;
    pte_t *pte;
    while(len>0){
        if(cur_va>=MAXVA)   return -1;
        cur_pa=walkaddr(pagetable, basepage_va);
        if(cur_pa==0){
            alloc_size=cur_max_size;
            while(len < alloc_size && alloc_size>PGSIZE)
                alloc_size/= 2;
            // VM_TRACE("copyout needs alloc for basepage_va=0x%llx size=0x%llx\n", basepage_va, alloc_size);
            mem=(uint64)alloc_memory(alloc_size);
            if(mem==0)  return -1;
            memset((void *)mem, 0, alloc_size);
            if(mappages(pagetable, basepage_va, alloc_size, (uint64)mem, PTE_U | PTE_W | PTE_R)!=0){
                free_pages(mem, alloc_size);
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
    //Considering when user's page is missing.
    uint64 aligned_srcva=PGROUNDDOWN(srcva);
    uint8 cur_order=i_log2(aligned_srcva & -aligned_srcva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ? (MAX_ORDER+ORDER_BASE):cur_order;
    uint64 block_size, copy_size, cur_va=srcva, basepage_va=aligned_srcva, cur_pa;
    void *mem;
    int perm=0;
    struct proc *cur_proc=myproc();
    pte_t *pte=NULL;
    if(cur_proc==NULL)  panic("Empty proc.");
    while(len>0){
        if(basepage_va>=MAXVA)      return -1;
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(pte==NULL){
            cur_order=i_log2(basepage_va & -basepage_va);
            cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            block_size=1ull<<cur_order;
            while(len < block_size && block_size>PGSIZE)
                block_size/=2;
            // VM_TRACE("copyin needs alloc for basepage_va=0x%llx size=0x%llx\n", basepage_va, alloc_size);
            mem=alloc_memory(block_size);
            if(mem==0)  return -1;
            memset(mem, 0, block_size);
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_proc->mm==NULL || !holdingsleep(&cur_proc->mm->mm_lock))
                pr_warn("lack necessary mm_struct, can't get more info.Treat as anonymous VMA.");
            else{
                vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, cur_va);
                if(cur_vma==NULL || cur_vma->vm_file==NULL){
                    pr_info("Treat as anonymous VMA.");
                    perm = PTE_R | PTE_U | PTE_W;
                }
                else if(vmfile_load(cur_vma, basepage_va, mem, block_size)!=0){
                    pr_err("Corrputed file, unable to continue.");
                    free_pages(mem, block_size);
                    return -1;
                }
                else    perm = cur_vma->vm_page_prot;
            }
            acquire(&cur_proc->uvm_lock);
            if(mappages(pagetable, basepage_va, block_size, (uint64)mem, perm)!=0){
                release(&cur_proc->uvm_lock);
                free_pages(mem, block_size);
                return -1;
            }
            release(&cur_proc->uvm_lock);
            cur_pa=walkaddr(pagetable, basepage_va);    //update
        }
        else if ((*pte & PTE_V) == 0){   //Swap in fault.
            //extrace info(Swap_id) from the pte.
            //FIXME: 
        }
        else if ((*pte & PTE_U) == 0)    return -1;
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));

        }
        //block_size=1ull<<(get_order(cur_pa)+ORDER_BASE); 
        copy_size=block_size-(cur_va-basepage_va);
        if(copy_size>len)   copy_size=len;  
        //modified the size to reflect the actual amount successfully copied!
        memmove(dst, (void *)(cur_pa+(cur_va-basepage_va)), copy_size);
        len-=copy_size;
        dst+=copy_size;
        cur_va=basepage_va+block_size;
        basepage_va=PGROUNDDOWN(cur_va);
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    uint64 cur_pa, basepage_va, copy_size;
    uint64 cur_order, block_size, mem;
    int got_null=0, perm;
    pte_t *pte;
    struct proc *cur_proc=myproc();
    while(got_null==0 && max>0){
        basepage_va=PGROUNDDOWN(srcva);
        if(basepage_va>=MAXVA)      return -1;
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(pte==NULL){
            cur_order=i_log2(basepage_va & -basepage_va);
            cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            block_size=1ull<<cur_order;
            while(max < block_size && block_size>PGSIZE)
                block_size/=2;
            // VM_TRACE("copyin needs alloc for basepage_va=0x%llx size=0x%llx\n", basepage_va, alloc_size);
            mem=(uint64)alloc_memory(block_size);
            if(mem==0)  return -1;
            memset(mem, 0, block_size);
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_proc->mm==NULL || !holdingsleep(&cur_proc->mm->mm_lock)){
                pr_warn("lack necessary mm_struct, can't get more info.Treat as anonymous VMA.");
                return -1;
            }
            vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, srcva);
            if(cur_vma==NULL || cur_vma->vm_file==NULL)
                return -1;
            if(vmfile_load(cur_vma, basepage_va, mem, block_size)!=0){
                pr_err("Corrputed file, unable to continue.");
                free_pages(mem, block_size);
                return -1;
            }
            perm = cur_vma->vm_page_prot;
            acquire(&cur_proc->uvm_lock);
            if(mappages(pagetable, basepage_va, block_size, (uint64)mem, perm)!=0){
                release(&cur_proc->uvm_lock);
                free_pages(mem, block_size);
                return -1;
            }
            release(&cur_proc->uvm_lock);
            cur_pa=walkaddr(pagetable, basepage_va);    //update
        }
        else if ((*pte & PTE_V) == 0){   //Swap in fault.
            //extrace info(Swap_id) from the pte.
            //FIXME: 
        }
        else if ((*pte & PTE_U) == 0)    return -1;
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
        }
        pte=walk(pagetable, basepage_va, 0, 0);
        if(pte==0 || (*pte & PTE_R)==0) return -1;
        block_size=1ull<<(get_order(cur_pa)+ORDER_BASE);
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
    if (got_null)   return 0;
    else    return -1;
}

// allocate and map user memory if process is referencing a page that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if out of physical memory, 
// and return physical address if successful.
uint64 vmfault(res_block *rblocks, pagetable_t pagetable, uint64 va, int read) {
    struct proc *p = myproc();
    uint64 tmp_ret=0;

    //Pass arugment rblocks etc explicitly instead of implicitly retriving them via myproc()
    if(p->mm==NULL){
        VM_TRACE("Fatal error: proc's mm is NULL.\n");
        return 0;
    }
    acquiresleep(&p->mm->mm_lock);  //acquire sleeplock firstly
    va = PGROUNDDOWN(va);
    vm_area_struct_t *vma=find_vma_and_get(p->mm, va);
    releasesleep(&p->mm->mm_lock);
    if(vma==NULL){
        VM_TRACE("Cannot find the corresponding vma.\n");
        goto release_and_ret;
    }
    if((read==1 && !(vma->vm_flags & VM_READ)) || (read==0 && !(vma->vm_flags & VM_WRITE))){
        VM_TRACE("Inconsisent permission.\n");
        goto release_and_ret;
    }
    acquire(&p->uvm_lock);      //Acquiring, for modifying PTE absolutely
    //Valid vma, categorization and dispatching.
    pte_t *pte=walk(p->pagetable, va, 0, 0);
    if(pte==NULL){      //Lazy allocation.
        if(vma->vm_file==NULL){     //Anonymous Fault.(.bss or heap or stack), mayeb mmap
        #ifdef RESERVE
            tmp_ret=Simp_alloc_res_memory(rblocks, pagetable, va, PTE_W|PTE_R|PTE_U);
        #else
            tmp_ret = (uint64)kalloc_page();
            if (tmp_ret == 0){
                VM_TRACE("alloc new page fail.\n");
                goto release_and_ret;
            }
            memset((void *)tmp_ret, 0, PGSIZE);
            if (mappages(p->pagetable, va, PGSIZE, tmp_ret, vma->vm_page_prot) != 0) {
                kfree_page((void *)tmp_ret);
                tmp_ret=0;
                goto release_and_ret;
            }
        #endif
        }
        else{       //File-backed Fault(.text or .data)
            uint64 offset=vma->vm_pgoff + (va-vma->vm_start);
            uint len=(va+PGSIZE > vma->vm_end)?(vma->vm_end-va):PGSIZE;
            tmp_ret = (uint64)kalloc_page();
            if (tmp_ret == 0){
                VM_TRACE("alloc new page fail.\n");
                goto release_and_ret;
            }
            memset((void *)tmp_ret, 0, PGSIZE);
            release(&p->uvm_lock);
            if(vmfile_load(vma, va, tmp_ret, PGSIZE)!=0){
                free_pages((void *)tmp_ret, PGSIZE);
                pr_err("error when loading file.");
                tmp_ret=0;
            }
            else{
                acquiresleep(&p->mm->mm_lock);
                acquire(&p->uvm_lock);
                pte=walk(pagetable, va, 0, 0);
                if((*pte & PTE_V) || vma->vm_mm!=p->mm){
                    //Current page fault has already been handled,Release original resources.
                    free_pages(tmp_ret, PGSIZE);
                    pr_info("Other concurrent process have resolved.");
                    tmp_ret=PTE2PA(*pte);
                }
                else if (mappages(p->pagetable, va, PGSIZE, tmp_ret, vma->vm_page_prot) != 0) {
                    VM_TRACE("Mappage %llx fail", va);
                    kfree_page((void *)tmp_ret);
                    tmp_ret=0;
                }
                releasesleep(&p->mm->mm_lock);
            }
        }
    }
    else if(pte!=NULL && (*pte & PTE_V)==0){        //Swap in Fault
        //extrace info(Swap_id) from the pte.
        //FIXME: 
    }
    else{       //PTE is valid, but permission dismatch
        if(vma->vm_flags & VM_WRITE){   //COW(copy on write)
            tmp_ret=(uint64)alloc_memory(PGSIZE);
            if(tmp_ret==0){
                pr_warn("COW:alloc memory fail.\n");
                goto release_and_ret;
            }
            //FIXME: 
        }
        else    pr_warn("Try to write constant area.\n");
    }
release_and_ret:
    if(vma!=NULL)   vma_put(vma);
    if(holding(&p->uvm_lock))    release(&p->uvm_lock);
    return tmp_ret;
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

// #ifdef LAB_PGTBL
pte_t *pgpte(pagetable_t pagetable, uint64 va) { return walk(pagetable, va, 0, 0); }
// #endif

int merge_into_hugepages_Out(res_block *rblocks, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    //for out-of-place promoted.Expensive copy or move cost.
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE, cur_order;
    if(idx>=MAX_RES_BLOCK)  return -1;
    pte_t *parent_pte=walk(pagetable, va, 1, 1);
    pte_t *old_pte=walk(pagetable, rblocks[idx].va, 1, 0);
    if(parent_pte==NULL || old_pte==NULL)   return -1;
    //mapping trigger va using stale xperm(keep same processing flow)
    //Attribute Consistency Check and Accumulation of A/D bits
    uint16 check_perm_mask= PTE_W | PTE_R | PTE_X | PTE_U;
    uint64 expe_perm=0, accu_ad=0, bp_idx=0, delete_pa=PTE2PA(*parent_pte);
    if(delete_pa==0)    return -1;
    uint8 is_expe_perm_fix=0;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
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
                return -1;
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
    //Commit Phase
    expe_perm |= (accu_ad)?(PTE_A | PTE_D):0;
    *parent_pte=expe_perm | PA2PTE(alloced_pa) | PTE_V;
    sfence_vma();   //keep the proper sequence, avoid reusing the freed memory(which record in tlb)
    bp_idx=0;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
            uint64 prev_pa=PTE2PA(old_pte[bp_idx]);
            cur_order=get_order(prev_pa);
            free_pages((void *)prev_pa, 1ull<<(ORDER_BASE+cur_order));
            bp_idx+=(1ull<<cur_order);
        }
        else bp_idx++;
    }
    free_pages((void *)delete_pa, PGSIZE);    //free the dirty and scrattered pagetable
    return 0;
}

int merge_into_hugepages_In(res_block *rblocks, pagetable_t pagetable, uint64 va){
    //for in-place promoted, clear the mapping,save the leaf-pte only
    pte_t *parent_pte=walk(pagetable, va, 0, 1);
    uint64 check_perm_mask=PTE_W | PTE_R | PTE_X | PTE_U, cur_order;
    uint64 expe_perm=0, accu_ad=0, bp_idx=0, delete_pa=PTE2PA(*parent_pte);
    if(parent_pte==NULL || delete_pa==0)    return -1;
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    if(idx>=512)    return -1;
    uint8 is_expe_perm_fix=0;
    pte_t *old_pte=walk(pagetable, rblocks[idx].va, 0, 0);
    if(old_pte==NULL)   return -1;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
            uint64 flags=PTE_FLAGS(old_pte[bp_idx]);
            if(!(flags & PTE_V)){
                panic("Bitmap valid but hardware PTE invalid!");
                return -1;
            }
            if(is_expe_perm_fix==0){    //Self-Adaptive Permission Capture
                expe_perm=(flags & check_perm_mask);
                is_expe_perm_fix=1; //Guarantee single initialization
            }
            else if((flags & check_perm_mask) != expe_perm){
                if((flags & PTE_U) != (expe_perm & PTE_U))
                    panic("Inconsisent User/Kernel Privilege!");
                panic("Inconsisent permission!");
                return -1;
            }
            accu_ad |= (flags & (PTE_A | PTE_D));
            //data transfer and release the unused resource
            uint64 prev_pa=PTE2PA(old_pte[bp_idx]);
            cur_order=get_order(prev_pa);
            bp_idx+=(1ull<<cur_order);
        }
        else    bp_idx++;
    }
    *parent_pte=PA2PTE(rblocks[idx].pa) | PTE_V | accu_ad | expe_perm;
    sfence_vma();
    free_pages((void *)delete_pa, PGSIZE);
    return 0;
}

int move_and_aggregate(res_block *rblocks, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    //Prepare the shadow copy, may trigger faults;not absolutely safe.
    if(va>rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE || va<rblocks[0].va){
        panic("aggregate_data failed:invalid va");
        return -1;
    }
    uint64 rb_idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, rblocks[rb_idx].va, 0, 0);
    if(old_pte==NULL)   return -1;
    uint64 bp_idx=0 ,tmp_pa, step=0;
    while(bp_idx<512){
        if(rblocks[rb_idx].bitmap[bp_idx]!=0){
            if(old_pte[bp_idx]==0)  return -1;
            if((PTE_FLAGS(old_pte[bp_idx]) & PTE_V)==0) return -1;
            tmp_pa=PTE2PA(old_pte[bp_idx]);
            if(tmp_pa==0)   return -1;
            step=1ull<<(get_order(tmp_pa)+ORDER_BASE);
            for(int j=0;j<step/PGSIZE;j++){
                if(j+bp_idx>=512)   return -1;
                if((PTE_FLAGS(old_pte[bp_idx+j]) & PTE_V)==0)  return -1;
            }
            memmove((void *)(alloced_pa+bp_idx*PGSIZE), (void *)tmp_pa, step);
        }
        else    step=PGSIZE;
        bp_idx+=step/PGSIZE;
    }
    return 0;
}

void safe_update_and_free(res_block *rblocks, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    //No need to store intermidate data, 'Casue it was designed as NO-Fail(Commit Point)
    //update pte and reclaim resouces at the same.
    uint64 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, rblocks[idx].va, 0, 0);
    uint64 bp_idx=0, tmp_flags, tmp_pa;
    uint64 step;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
            tmp_pa=PTE2PA(old_pte[bp_idx]);
            step=1ull<<(ORDER_BASE+get_order(tmp_pa));
            for(int j=0;j<step/PGSIZE;j++){
                tmp_flags=PTE_FLAGS(old_pte[bp_idx+j]);
                old_pte[bp_idx+j]=PA2PTE(alloced_pa+j*PGSIZE) | tmp_flags;
            }
            free_pages((void *)tmp_pa, step);
        }
        else    step=PGSIZE;
        alloced_pa+=step;
        bp_idx+=step/PGSIZE;
    }
    sfence_vma();
}

uint64 alloc_res_memory(res_block *rblocks, pagetable_t pagetable, uint64 init_heap_start, 
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
    if(idx>=MAX_RES_BLOCK)  return 0;
    while(idx<MAX_RES_BLOCK && req_size>0){
        split_alloc=(SUPERPGROUNDDOWN(start_va)==SUPERPGROUNDDOWN(newsz))?0:1;
        if(split_alloc){
            end_va=(idx+1==MAX_RES_BLOCK)?(rblocks[idx].va+SUPERPGSIZE):rblocks[idx+1].va;
            vpn_step=(end_va-start_va)/PGSIZE;
        }
        else{
            end_va=align_newsz;
            vpn_step=(align_newsz-start_va)/PGSIZE;
        }
        start_vpn=(start_va-rblocks[idx].va)/PGSIZE;
        if(rblocks[idx].promoted==0){
            if(rblocks[idx].pop_count>=THRESHLOD){
                //use the huge page(2mb)
                new_block=(uint64)alloc_memory(SUPERPGSIZE);
                if(new_block==0){   //promoted fail!
                    new_block=(uint64)alloc_memory(vpn_step*PGSIZE);
                    if(new_block==0)    panic("out-of-memory");
                }
                else{   //Hugepage allocate success
                    if(merge_into_hugepages_Out(rblocks, pagetable, SUPERPGROUNDDOWN(start_va), new_block)==0){
                        rblocks[idx].pa=new_block;
                        rblocks[idx].promoted=1;
                        rblocks[idx].pop_count+=vpn_step;
                        //update the mapping_table
                        memset(rblocks[idx].bitmap+start_vpn, 1, vpn_step);    //update the bitmaps
                    }
                    else{   //Merge fail, reclaim the allocated resource
                        free_pages((void *)new_block, SUPERPGSIZE);
                    }
                }
            }
            else{//use the simple allocator(split_alloc must be zero)
                int tmp_ret=uvmalloc(pagetable, oldsz, newsz, xperm);
                if(tmp_ret!=newsz)  return 0;
                memset(rblocks[idx].bitmap+start_vpn, 1, vpn_step);
                rblocks[idx].pop_count+=vpn_step;
            }
        }   //for promoted pages, do nothing!
        req_size-=(end_va-start_va);
        start_va=end_va;
        idx++;
    }
    //check if cross-page and need handle further
    return newsz;
}

uint64 free_res_memory(res_block *rblocks, pagetable_t pagetable, uint64 init_heap_start, 
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
        start_vpn=(start_va-rblocks[idx].va)/PGSIZE;
        split_dealloc=(SUPERPGROUNDDOWN(start_va)==SUPERPGROUNDDOWN(oldsz))?0:1;
        if(split_dealloc){
            end_va=(idx+1==MAX_RES_BLOCK)?(rblocks[idx].va+SUPERPGSIZE):rblocks[idx+1].va;
        }
        else    end_va=align_oldsz;
        vpn_step=(end_va-start_va)/PGSIZE;
        if(rblocks[idx].promoted==1){  //Demotion is merely a preliminary step for reclamation.
            //Split regradless of release size due to the lack of intermidate state.
            split_into_blocks(rblocks, pagetable, start_va, 1);  // Demotion for partial uvmunmap
            rblocks[idx].promoted=0;
        }
        uint64 bp_idx=start_vpn, bp_step;
        pte_t *pte=walk(pagetable, start_va, 0, 0);
        while(bp_idx<start_vpn+vpn_step){
            if(rblocks[idx].bitmap[bp_idx]!=0){
                uint64 cur_bp_pa=PTE2PA(*pte);
                uint64 cur_order=get_order(cur_bp_pa);
                bp_step=(1ull<<cur_order);
                rblocks[idx].pop_count-=bp_step;
            }
            else    bp_step=1;
            bp_idx+=bp_step;
            pte+=bp_step;
        }
        uvmdealloc(pagetable, end_va, start_va);    //Finalize the deallocation process.
        memset(rblocks[idx].bitmap+start_vpn, 0, vpn_step);
        req_size-=(end_va-start_va);
        start_va=end_va;
        idx++;
    }
    return newsz;
}

void reclaim_res_memory_range(res_block *rblocks, pagetable_t pagetable, uint64 start_va, uint64 end_va){
    //check if current rblocks is valid and initialize success
    uint64 block_start, block_end, intersect_start, intersect_end;
    uint64 base_addr=rblocks[0].va;  //Save the initial address to enable fast assignment.
    for(int i=0;i<MAX_RES_BLOCK;i++){
        if(rblocks[i].pa==0 || rblocks[i].promoted==1)    continue;
        block_start=rblocks[i].va;
        block_end=rblocks[i].va+SUPERPGSIZE;
        intersect_start=MAX(start_va, block_start);
        intersect_end=MIN(end_va, block_end);
        if(intersect_start>=intersect_end)  continue;   //No intersections, skip
        else if(intersect_end==intersect_start+SUPERPGSIZE){    //full overlap
            pte_t *parent_pte=walk(pagetable, rblocks[i].va, 0, 1);
            uint64 delete_pa=PTE2PA(*parent_pte);
            *parent_pte=0;
            sfence_vma();
            free_pages((void *)delete_pa, PGSIZE);
            free_pages((void *)rblocks[i].pa, SUPERPGSIZE);
            memset(&rblocks[i], 0, sizeof(res_block));
            rblocks[i].va=base_addr+i*SUPERPGSIZE;
        }
        else{   //partial Overlap(exist scrattered 4K)
            uint64 start_vpn=(intersect_start-rblocks[i].va)/PGSIZE;
            uint64 end_vpn=(intersect_end-rblocks[i].va)/PGSIZE;
            pte_t *old_pte=walk(pagetable, block_start, 0, 0);
            uint8 do_free=(rblocks[i].is_scattered==1);
            for(int j=start_vpn;j<end_vpn;j++){
                if(old_pte[j]==0)   continue;
                if(rblocks[i].bitmap[j]==0)    panic("Dismatch!");
                if(do_free) free_pages((void *)PTE2PA(old_pte[j]), PGSIZE);
                old_pte[j]=0;
                rblocks[i].bitmap[j]=0;
                rblocks[i].pop_count--;
            }
        }
    }
}
#ifdef IN_PLACE_PROMOTE
uint64 Simp_alloc_res_memory(res_block *rblocks, pagetable_t pagetable, 
        uint64 va, int xperm){
    //Triggered by a single page fault and allocate exactly one pages.
    va=PGROUNDDOWN(va);
    uint64 new_block=0;
    if(va<rblocks[0].va || va>=rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE){
        new_block=(uint64)alloc_memory(PGSIZE);
        if(new_block==0) return 0;
        memset((void *)new_block, 0, PGSIZE);
        if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
            free_pages((void *)new_block, PGSIZE);
            new_block=0;
        }
        return new_block;
    }
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE; //now idx must be valid!
    uint64 cur_vpn=(va-rblocks[idx].va)/PGSIZE;
    //By reserving huge page in the heap, only one allocation and multiple mapping is enough.
    if(rblocks[idx].promoted==0){  //enter reservable region, evaluate reservation strategy
        if(rblocks[idx].is_scattered==1){
            if(rblocks[idx].alloc_attempts<MAX_ALLOWED_ALLOCATIONS){
                new_block=(uint64)alloc_memory(SUPERPGSIZE);
                if(new_block!=0){
                    memset((void *)new_block, 0, SUPERPGSIZE);
                    // update the pte Only when new memory is ready to avoid handling intermediate states.
                    if(mappages(pagetable, va, PGSIZE, new_block+cur_vpn*PGSIZE, xperm)!=0){
                        free_pages((void *)new_block, SUPERPGSIZE);
                        rblocks[idx].alloc_attempts++;
                        goto signle_page;
                    }
                    if(rblocks[idx].pop_count>0){
                        if(move_and_aggregate(rblocks, pagetable, va, new_block)!=0){
                            rblocks[idx].alloc_attempts++;
                            free_pages((void *)new_block, SUPERPGSIZE);//rollback
                            pte_t *flight_pte=walk(pagetable, va, 0, 0);
                            if(flight_pte)  *flight_pte=0;
                            sfence_vma();
                            goto signle_page;
                        }
                        //update the mapping(safe)
                        safe_update_and_free(rblocks, pagetable, va, new_block);//move the data
                    }
                    rblocks[idx].bitmap[cur_vpn]=1;    //Synchronisz data
                    rblocks[idx].pop_count++;
                    rblocks[idx].is_scattered=0;   //update
                    rblocks[idx].pa=new_block;
                    return rblocks[idx].pa+cur_vpn*PGSIZE;
                }
                rblocks[idx].alloc_attempts++;
            }
signle_page:
            new_block=(uint64)alloc_memory(PGSIZE); //when alloc fail or map fail(free already)
            if(new_block==0){
                panic("Simp_alloc_res_memory : out-of-memory");
                return 0;
            }
            memset((void *)new_block, 0, PGSIZE);
            if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                free_pages((void *)new_block, PGSIZE);
                return 0;
            }
            else{
                rblocks[idx].bitmap[cur_vpn]=1;    //Synchronsize data
                rblocks[idx].pop_count++;
                return new_block;
            }
        }
        else{   // is_scrattered==0
            if(mappages(pagetable, va, PGSIZE, rblocks[idx].pa+cur_vpn*PGSIZE, xperm)!=0){
                panic("Simp_alloc_res_memory : mapping to existing page fail!");
                return 0;
            }
            rblocks[idx].bitmap[cur_vpn]=1;    //Synchronisz data
            rblocks[idx].pop_count++;
            if(THRESHLOD==0 || rblocks[idx].pop_count>=THRESHLOD-1){
                //Supposing existing the huge already,Upgrade page table
                if(merge_into_hugepages_In(rblocks, pagetable, va)==0){
                    rblocks[idx].bitmap[cur_vpn]=1;    //Synchronisz data
                    rblocks[idx].pop_count++;
                    rblocks[idx].promoted=1;
                }
                else{
                    pte_t *flight_pte=walk(pagetable, va, 0, 0);
                    if(*flight_pte) *flight_pte=0;
                    sfence_vma();
                    return 0;
                }
            }
        }
    }
    //already promoted, exist mapping.
    return rblocks[idx].pa+cur_vpn*PGSIZE;
}
#else
uint64 Simp_alloc_res_memory(res_block *rblocks, pagetable_t pagetable,
    uint64 va, int xperm){
    //Triggered by a single page fault and allocate exactly one pages.
    va=PGROUNDDOWN(va);
    uint64 new_block=0;
    if(va<rblocks[0].va){
        new_block=(uint64)alloc_memory(PGSIZE);
        if(new_block==0) return 0;
        memset((void *)new_block, 0, PGSIZE);
        if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
            free_pages((void *)new_block, PGSIZE);
            new_block=0;
        }
        return new_block;
    }
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    if(idx>MAX_RES_BLOCK)   return 0;
    uint64 start_vpn=(va-rblocks[idx].va)/PGSIZE;
    if(idx>=512)    return 0;
    if(rblocks[idx].promoted==0){  //enter reservable region, evaluate reservation strategy
        if(THRESHLOD==0 || rblocks[idx].pop_count>=THRESHLOD-1){
            new_block=(uint64)alloc_memory(SUPERPGSIZE);
            if(new_block==0){   //promoted fail!
                new_block=(uint64)alloc_memory(PGSIZE);
                if(new_block==0){
                    panic("out-of-memory");
                    return 0;
                }
                memset((void *)new_block, 0, PGSIZE);
                if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                    free_pages((void *)new_block, PGSIZE);
                    return 0;
                }
                rblocks[idx].bitmap[start_vpn]=1;
                rblocks[idx].pop_count++;
                return new_block;
            }
            //promoted success
            memset((void *)new_block, 0, SUPERPGSIZE);
            if(merge_into_hugepages_Out(rblocks, pagetable, va, new_block)==0){
                rblocks[idx].pa=new_block;
                rblocks[idx].promoted=1;
                rblocks[idx].bitmap[start_vpn]=1;
                rblocks[idx].pop_count++;
                return new_block+start_vpn*PGSIZE;
            }
            else{
                free_pages((void *)new_block, SUPERPGSIZE);
                return 0;
            }
        }
        else{   
            //use the simple allocator
            new_block=(uint64)alloc_memory(PGSIZE);
            if(new_block==0)    panic("out-of-memory");
            memset((void *)new_block, 0, PGSIZE);
            if(mappages(pagetable, va, PGSIZE, new_block, xperm)!=0){
                free_pages((void *)new_block, PGSIZE);
                return 0;
            }
            rblocks[idx].bitmap[start_vpn]=1;
            rblocks[idx].pop_count++;
            return new_block;
        }
    }
    rblocks[idx].pop_count++;
    //already promoted, exist mapping.
    return new_block;
}
#endif