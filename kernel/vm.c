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
#include "rbtree.h"
#include "kvm.h"
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

//To support multihart interrupt
struct mmu_free_batch_listnode{
    struct mmu_free_batch_entry node;
    struct mmu_free_batch_entry *next;
};
slab_cache_t *mmu_free_batch_listcache=NULL;

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
    if (va >= MAXVA)    panic("Walk exceed virtual address boundary");
    if(pagetable==NULL)
        panic("Pass in an unvalid pagetable(Nullptr)");
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
static inline int in_res_area(uint64 va, res_block *rblocks){
    if(rblocks==NULL){
        pr_err("Invalid argument:rblocks require a non-NULL pointer.");
        return -1;
    }
    if(va >= rblocks[0].va && va < rblocks[MAX_RES_BLOCK-1].va +SUPERPGSIZE)
        return 1;
    return 0;
}


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
int mappages(struct map_context *ctx1){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx pa=%p perm=0x%x\n", (void *)va, size, (void *)pa, perm);
#endif
    // if(pa != (1ull<<i_log2(pa)))   panic("Bypass the buddy system, passing invalid pa!\n");
    if(ctx1==NULL || ctx1->size==0 || ctx1->pagetable==NULL){
        pr_err("Invalid parameter.");
        return -1;
    }
    pte_t *pte=NULL;
    uint64 basic_size=PGSIZE, target_level=0, prev_level=3;   //determine rewalk necessity
    if(ctx1->size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (ctx1->size == 0) panic("mappages: size: 0");
    int locked_by_me=0;
    if(ctx1->pt_lock!=NULL && !holding(ctx1->pt_lock)){
        acquire(ctx1->pt_lock);
        locked_by_me=1;
    }
    while(ctx1->size>0){
        if(ctx1->start_va%GIGAPGSIZE==0 && ctx1->size>=GIGAPGSIZE){
            target_level=2;
            basic_size=GIGAPGSIZE;
        } 
        else if(ctx1->start_va%MEGAPGSIZE==0 && ctx1->size>=MEGAPGSIZE){
            target_level=1;
            basic_size=MEGAPGSIZE;
        }
        else{
            target_level=0;
            basic_size=PGSIZE;
        }
        if ((ctx1->pa % basic_size) != 0) panic("mappages: pa not aligned");
        if(prev_level!=target_level || PX(target_level, ctx1->start_va)==0){
            //check if need jump into other level or arrive the boundary
            pte=walk(ctx1->pagetable, ctx1->start_va, 1, target_level);//walk again
            prev_level=target_level;
        }
        else    pte++;  //update the pte quickly,no need to walk agin
        if(*pte & PTE_V)    panic("mappages: remap");
        //check if it's the last vpns to 
        *pte= PA2PTE(ctx1->pa) | PTE_V | ctx1->xperm; //Assign
        ctx1->size-=basic_size;
        ctx1->start_va+=basic_size;
        ctx1->pa+=basic_size;
        //update basic_size(and target level) based on the current maximum alignment value.
    }
    sfence_vma(0, 0);   //tlb flush once prevent overhead of TLB shootdown.
    if(locked_by_me==1)
        release(ctx1->pt_lock);
    return 0;
}

void split_into_blocks(res_block * rblocks, pagetable_t pagetable, uint64 va, uint64 cur_level){
    if(rblocks==NULL || pagetable==NULL)
        panic("Invalid argument");
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, va, 0, cur_level);
    if(old_pte==NULL || *old_pte==0)
        panic("Incomplete or missing page table structure for the given address");
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
    sfence_vma(0, 0);
    free_pages((void *)delete_pa, get_step_size(cur_level));
    sfence_vma(0, 0);
}

//Split the hugepage into smaller part(cur_level -1 ) and ignore the pte with range:[start_vpn, end_vpn]
//Usually followed by free_page, reclaim these unmapped pte.
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
    cur_pa+=(end_vpn-start_vpn) * get_step_size(cur_level-1);
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

//NOTE: TLB is designated for MMU which can bypass lock to scan the pagetable directly.
// (Bus master: DMA, hardware prefetcher, GPU)
uint64 uvmunmap_helper(struct map_context *ctx1, struct mmu_gather *ctx2){
    //Unmap and then free physical resource(if necessary)
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx do_free=%d, cur_level is %d\n", (void *)va, size, do_free, cur_level);
#endif
    if(ctx1==NULL || ctx1->pagetable==NULL || ctx1->size==0){
        pr_err("Invalid argument: map_context");
        return -1;
    }
    uint64 basic_stride=get_step_size(ctx1->cur_level);
    uint64 cur_vpn=PX(ctx1->cur_level, ctx1->start_va);
    uint64 start_page_offset=ctx1->start_va & (basic_stride-1);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(start_page_offset+ctx1->size+basic_stride-1)/basic_stride;
    uint64 pa, del_size=0, pending;
    if(ctx2==NULL || ctx2->root_pg==NULL){
        pr_err("Invalid argument: mmu_gather.");
        return -1;
    }
    if(ctx1->pagetable == ctx2->root_pg){
        if(ctx2->batch_start_va != ctx1->start_va || 
            ctx2->batch_flush_len!=0 || ctx2->data_idx!=0 || ctx2->dir_idx !=0){
            pr_warn("The input argument(mmu_gather) were not reset to zero upon entry.");
            return -1;
        }
    }
    //store pa to be deleted(also with its size) and process them in a batch,
    //followed by a TLB flush to improve efficiency.
    int locked_by_me=0;
    if(ctx1->pt_lock!=NULL && !holding(ctx1->pt_lock)){
        acquire(ctx1->pt_lock);
        locked_by_me=1;
    }
    while(cur_vpn<end_vpn){
        pte=&ctx1->pagetable[cur_vpn];
        uint64 page_offset=ctx1->start_va & (basic_stride-1);
        if((*pte & PTE_V)==0){
            cur_vpn++;
            pending=MIN(basic_stride-page_offset, ctx1->size-del_size);
            del_size+=pending;
            ctx1->start_va+=pending;
        }
        else if(PTE_LEAF(*pte)){
            pa=PTE2PA(*pte);
            if(is_managed_memory(pa)==0){   //Outside RAM region, maybe trampoline or trapframe
                //It's safe as current implementation does not invoke a free operations on these area.
                VM_TRACE("Uvmunmap dangerous area(outside the RAM region)\n");
                pte[0]=0;
                //But check the unmap size is equal to its size.
                if(ctx1->size - del_size < basic_stride){
                    pr_err("Going to unmap region outside RAM partially.Strictly forbidded!");
                    return -1;
                }
                ctx1->start_va+=basic_stride;
                del_size+=basic_stride;
                cur_vpn++;
            }
            else if(page_offset !=0 || ctx1->size-del_size<basic_stride){
                //Partial Unmap and use Pruned Spltting Strategy
                pending=MIN(ctx1->size-del_size, basic_stride-page_offset);
                pte_t tmp_pte=*pte; //Old bigger page pte
                *pte=0; //Unmap to prevent remap error!
                if(ctx1->cur_level<=0)    panic("Try to Split the minimun Unit");
                uint64 child_start_vpn=PX(ctx1->cur_level-1, ctx1->start_va);
                uint64 child_end_vpn=PX(ctx1->cur_level-1, ctx1->start_va+pending);
                child_end_vpn=(child_end_vpn==0)?512:child_end_vpn;
                *pte=PA2PTE((uint64)split_and_prune(tmp_pte, child_start_vpn, 
                child_end_vpn, ctx1->cur_level)) | PTE_V;
                //Calculate the free_start_pa(the Only place still remember the original physical address)
                uint64 free_start_pa=PTE2PA(tmp_pte)+child_start_vpn*get_step_size(ctx1->cur_level-1);
                ctx2->data_page_batch[ctx2->data_idx].delete_pa=free_start_pa;
                ctx2->data_page_batch[ctx2->data_idx].len=pending;
                ctx2->data_page_batch[ctx2->data_idx++].start_va=ctx1->start_va;
                ctx2->batch_flush_len+=pending;
                if(ctx2->data_idx>=MMU_BATCH_SIZE){
                    //Start processing each batch, must begin with sfence_vam
                    //'Casue MMU will bypass lock and read RAM's pagetable.So tlb will accumulate always.
                    tlb_shootdown_issue_nolock(ctx2->root_pg, ctx2->batch_start_va, ctx2->batch_flush_len);
                    ctx2->batch_start_va=ctx1->start_va + pending;
                    ctx2->batch_flush_len=0;   //update
                    //(Unmap with tlb flush, so reocrd, but free isn't necessary.)
                    if(ctx1->do_free==1){
                        for(int i=0;i<MMU_BATCH_SIZE;i++)
                            free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                                ctx2->data_page_batch[i].len);
                    }
                    ctx2->data_idx=0;
                }
                // free_pages((void *)free_start_pa, pending);
                if(pending==ctx1->size-del_size)  return ctx1->size;
                del_size+=pending;
                ctx1->start_va+=pending;
                cur_vpn++;    //across two pages, continue handling(at current level)
            }
            else{   //full Unmap(basic_stride < size-del_size)
                uint64 max_cont_len=basic_stride, cmp_perm=PTE_FLAGS(*pte);
                pte_t tmp_pte=0;
                int inc_vpn=1;
                while(cur_vpn+inc_vpn < end_vpn && max_cont_len < (ctx1->size - del_size)){
                    tmp_pte=ctx1->pagetable[cur_vpn+inc_vpn];
                    if((tmp_pte & PTE_V) == 0)  break;
                    if(PTE2PA(tmp_pte) != pa + inc_vpn * basic_stride)
                        break;
                    if(PTE_FLAGS(tmp_pte) != cmp_perm)
                        break;
                    inc_vpn++;
                    max_cont_len+=basic_stride;
                }
                for(int i=0;i<inc_vpn;i++)
                if(cur_vpn+i<end_vpn) pte[i]=0;
                ctx2->data_page_batch[ctx2->data_idx].delete_pa=pa;
                ctx2->data_page_batch[ctx2->data_idx].len=max_cont_len;
                ctx2->data_page_batch[ctx2->data_idx++].start_va=ctx1->start_va;
                ctx2->batch_flush_len+=max_cont_len;
                if(ctx2->data_idx>=MMU_BATCH_SIZE){     //Amortization
                    tlb_shootdown_issue_nolock(ctx2->root_pg, ctx2->batch_start_va, ctx2->batch_flush_len);
                    ctx2->batch_start_va=ctx1->start_va + max_cont_len;
                    ctx2->batch_flush_len=0;   //update
                    if(ctx1->do_free==1){
                        for(int i=0;i<MMU_BATCH_SIZE;i++)
                            free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                                ctx2->data_page_batch[i].len);
                    }
                    ctx2->data_idx=0;
                }
                del_size+=max_cont_len;
                ctx1->start_va+=max_cont_len;
                cur_vpn+=inc_vpn;
            }
        }
        else{   //directory page:Split and delegate the task to the next-level page table! 
            pending=MIN(basic_stride-page_offset, ctx1->size-del_size);
            uvmunmap_helper(&(struct map_context){
                .pagetable=(pagetable_t)PTE2PA(*pte),
                .start_va=ctx1->start_va,
                .cur_level=ctx1->cur_level-1,
                .pt_lock=ctx1->pt_lock,
                .size=pending,
                .do_free=ctx1->do_free
            } , ctx2);
            if(is_directory_empty((pagetable_t)PTE2PA(*pte))){
                ctx2->dir_pa_batch[ctx2->dir_idx]=PTE2PA(*pte);
                *pte=0;     //Invalid
                if(ctx2->dir_idx >= MMU_BATCH_SIZE){
                    tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
                    // start_va and len doesn't matter ,just force to flush global pte.
                    for(int i=0;i<MMU_BATCH_SIZE;i++)
                        free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
                    ctx2->dir_idx=0;
                }
            }

            //NOTE: for directory pte, while Adopt Lazy reclaim mechanism: 
            // avoid thrashing, the cost of verification,and Concurrency hell. 
            // Only two specific moment take earge reclaim:
            // exit/freeproc(use freewalk instead), memory pressure(implemented by backprocess)

            // if(is_directory_empty((pagetable_t)PTE2PA(*pte))){
            //     uint64 child_pa=PTE2PA(*pte);
            //     free_pages((void *)child_pa, PGSIZE);
            //     *pte=0; //clear the pte
            // }
            ctx1->start_va+=pending;
            del_size+=pending;
            cur_vpn++;
        }
    }
    if(ctx1->pagetable == ctx2->root_pg){
        if(ctx2->dir_idx==0){
            if(ctx2->data_idx!=0 && ctx2->batch_flush_len!=0)
                tlb_shootdown_issue_nolock(ctx2->root_pg, ctx2->batch_start_va, ctx2->batch_flush_len);
        }
        else    tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
        if(ctx1->do_free==1){
            for(int i=0;i<ctx2->data_idx;i++)
                free_pages((void *)ctx2->data_page_batch[i].delete_pa, ctx2->data_page_batch[i].len);
        }
        //directory pte
        for(int i=0;i<ctx2->dir_idx;i++)
            free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
        ctx2->data_idx=0;
        ctx2->dir_idx=0;
    }
    if(locked_by_me==1)
        release(ctx1->pt_lock);
    return del_size;
}

//Iterative veriosn, use less stack.
uint64 uvmunmap_helper_iter(struct map_context *ctx1, struct mmu_gather *ctx2){
    //Unmap and then free physical resource(if necessary)
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx do_free=%d, cur_level is %d\n", (void *)va, size, do_free, cur_level);
#endif
    if(ctx1==NULL || ctx1->pagetable==NULL || ctx1->size==0){
        pr_err("Invalid argument: map_context");
        return -1;
    }
    if(ctx2==NULL || ctx2->root_pg==NULL){
        pr_err("Invalid argument: mmu_gather.");
        return -1;
    }
    uint64 basic_stride=get_step_size(ctx1->cur_level);
    uint64 cur_vpn=PX(ctx1->cur_level, ctx1->start_va);
    uint64 start_page_offset=ctx1->start_va & (basic_stride-1);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(start_page_offset+ctx1->size+basic_stride-1)/basic_stride;
    int cur_level=2;    //Initialize as the highest.
    pagetable_t cur_pg=ctx1->pagetable;
    struct map_iter_context cur_state_array[3];
    memset(cur_state_array, 0, sizeof(cur_state_array));
    cur_state_array[2].cur_level=2;
    cur_state_array[2].cur_vpn=cur_vpn;
    cur_state_array[2].end_vpn=end_vpn;
    cur_state_array[2].pagetable=cur_pg;
    uint64 pa, del_size=0, pending;
    //followed by a TLB flush to improve efficiency.
    int locked_by_me=0;
    if(ctx1->pt_lock!=NULL && !holding(ctx1->pt_lock)){
        acquire(ctx1->pt_lock);
        locked_by_me=1;
    }
    //Validate input parameter also.
    if(ctx2->batch_start_va != ctx1->start_va || 
        ctx2->batch_flush_len!=0 || ctx2->data_idx!=0 || ctx2->dir_idx !=0){
        pr_warn("The input argument(mmu_gather) were not reset to zero upon entry.");
        return -1;
    }
    while(cur_level < 3){
        //update the cur_pg and other metadata
        cur_pg=cur_state_array[cur_level].pagetable;
        basic_stride=get_step_size(cur_level);
        cur_vpn=cur_state_array[cur_level].cur_vpn;
        end_vpn=cur_state_array[cur_level].end_vpn;
        if(end_vpn>512){
            #ifdef DEBUG_VM
                VM_TRACE("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
                VM_TRACE("size=0x%llx, cur_level=%d, end_vpn=0x%llx\n", 
                    ctx1->size, ctx1->cur_level, end_vpn);
            #endif
            panic("uvmunmap_helper!");
            return -1;
        }
        while(cur_vpn < end_vpn){   //exclude end_vpn
            pte=&cur_pg[cur_vpn];
            uint64 page_offset=ctx1->start_va & (basic_stride-1);
            if((*pte & PTE_V)==0){
                cur_vpn++;
                pending=MIN(basic_stride-page_offset, ctx1->size-del_size);
                del_size+=pending;
                ctx1->start_va+=pending;
            }
            else if(PTE_LEAF(*pte)){
                pa=PTE2PA(*pte);
                if(is_managed_memory(pa)==0){   //Outside RAM region, maybe trampoline or trapframe
                    //It's safe as current implementation does not invoke a free operations on these area.
                    VM_TRACE("Uvmunmap dangerous area(outside the RAM region)\n");
                    pte[0]=0;
                    //But check the unmap size is equal to its size.
                    if(ctx1->size - del_size < basic_stride){
                        pr_err("Going to unmap region outside RAM partially.Strictly forbidded!");
                        return -1;
                    }
                    ctx1->start_va+=basic_stride;
                    del_size+=basic_stride;
                    cur_vpn++;
                }
                else if(page_offset !=0 || ctx1->size-del_size<basic_stride){
                    //Partial Unmap and use Pruned Spltting Strategy
                    pending=MIN(ctx1->size-del_size, basic_stride-page_offset);
                    pte_t tmp_pte=*pte; //Old bigger page pte
                    *pte=0; //Unmap to prevent remap error!
                    if(cur_level<=0)    panic("Try to Split the minimun Unit");
                    uint64 child_start_vpn=PX(cur_level-1, ctx1->start_va);
                    uint64 child_end_vpn=PX(cur_level-1, ctx1->start_va+pending);
                    child_end_vpn=(child_end_vpn==0)?512:child_end_vpn;
                    *pte=PA2PTE((uint64)split_and_prune(tmp_pte, child_start_vpn, child_end_vpn, cur_level)) | PTE_V;
                    //Calculate the free_start_pa(the Only place still remember the original physical address)
                    uint64 free_start_pa=PTE2PA(tmp_pte)+child_start_vpn*get_step_size(cur_level-1);
                    ctx2->data_page_batch[ctx2->data_idx].delete_pa=free_start_pa;
                    ctx2->data_page_batch[ctx2->data_idx].len=pending;
                    ctx2->data_page_batch[ctx2->data_idx++].start_va=ctx1->start_va;
                    ctx2->batch_flush_len+=pending;
                    if(ctx2->data_idx>=MMU_BATCH_SIZE){     //Start processing each batch, must begin with sfence_vam
                        //'Casue MMU will bypass lock and read RAM's pagetable.So tlb will accumulate always.
                        tlb_shootdown_issue_nolock(ctx2->root_pg, ctx2->batch_start_va, ctx2->batch_flush_len);
                        ctx2->batch_start_va=ctx1->start_va + pending;
                        ctx2->batch_flush_len=0;   //update
                        if(ctx1->do_free==1){
                            for(int i=0;i<MMU_BATCH_SIZE;i++)
                                free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                                    ctx2->data_page_batch[i].len);
                        }
                        ctx2->data_idx=0;
                    }
                    // free_pages((void *)free_start_pa, pending);
                    del_size+=pending;
                    ctx1->start_va+=pending;
                    cur_vpn++;    //across two pages, continue handling(at current level)
                }
                else{   //full Unmap(basic_stride < size-del_size)
                    uint64 max_cont_len=basic_stride, cmp_perm=PTE_FLAGS(*pte);
                    pte_t tmp_pte=0;
                    int inc_vpn=1;
                    while(cur_vpn+inc_vpn < end_vpn && max_cont_len < (ctx1->size - del_size)){
                        tmp_pte=cur_pg[cur_vpn+inc_vpn];
                        if((tmp_pte & PTE_V) == 0)  break;
                        if(PTE2PA(tmp_pte) != pa + inc_vpn * basic_stride)
                            break;
                        if(PTE_FLAGS(tmp_pte) != cmp_perm)
                            break;
                        inc_vpn++;
                        max_cont_len+=basic_stride;
                    }
                    for(int i=0;i<inc_vpn;i++)
                    if(cur_vpn+i<end_vpn) pte[i]=0;
                    ctx2->data_page_batch[ctx2->data_idx].delete_pa=pa;
                    ctx2->data_page_batch[ctx2->data_idx].len=max_cont_len;
                    ctx2->data_page_batch[ctx2->data_idx++].start_va=ctx1->start_va;
                    ctx2->batch_flush_len+=max_cont_len;
                    if(ctx2->data_idx>=MMU_BATCH_SIZE){
                        tlb_shootdown_issue_nolock(ctx2->root_pg, ctx2->batch_start_va, ctx2->batch_flush_len);
                        ctx2->batch_start_va=ctx1->start_va + max_cont_len;
                        ctx2->batch_flush_len=0;   //update
                        if(ctx1->do_free==1){
                            for(int i=0;i<MMU_BATCH_SIZE;i++)
                                free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                                    ctx2->data_page_batch[i].len);
                        }
                        ctx2->data_idx=0;
                    }
                    del_size+=max_cont_len;
                    ctx1->start_va+=max_cont_len;
                    cur_vpn+=inc_vpn;
                }
            }
            else{   //directory page:
                pending=MIN(basic_stride-page_offset, ctx1->size-del_size);
                //save the current statement.
                cur_state_array[cur_level].pagetable=cur_pg;
                cur_state_array[cur_level].cur_level=cur_level;
                cur_state_array[cur_level].cur_vpn=cur_vpn+1;   //adjust
                cur_state_array[cur_level].end_vpn=end_vpn;
                cur_pg=(pagetable_t)PTE2PA(*pte);
                cur_level--;
                basic_stride=get_step_size(cur_level);
                cur_vpn=PX(cur_level, ctx1->start_va);
                start_page_offset=ctx1->start_va & (basic_stride-1);
                end_vpn=cur_vpn+(start_page_offset+pending+basic_stride-1)/basic_stride;
                //Then Re-initialize paramter for the new cycle.
                //NOTE: Adopt Lazy reclaim mechanism: avoid thrashing, the cost of verification,
                // and Concurrency hell. Only two specific moment take earge reclaim:
                // exit/freeproc(use freewalk instead), memory pressure(implemented by backprocess)
                // if(is_directory_empty((pagetable_t)PTE2PA(*pte))){
                //     uint64 child_pa=PTE2PA(*pte);
                //     free_pages((void *)child_pa, PGSIZE);
                //     *pte=0; //clear the pte
                // }
            }
        }
        //while finish the scan for current hierarchy, elevate the mapping level.
        cur_level++;
        //Eager reclaim.
        if(cur_level< 3 && is_directory_empty(cur_pg)){
            ctx2->dir_pa_batch[ctx2->dir_idx]=(uint64)cur_pg;
            //Use cur_state_array to find parent pagetable.
            cur_state_array[cur_level].pagetable[cur_state_array[cur_level].cur_vpn-1]=0;
            if(ctx2->dir_idx >= MMU_BATCH_SIZE){
                tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
                // start_va and len doesn't matter(just trigger an enforced global TLB invalidation)
                for(int i=0;i<MMU_BATCH_SIZE;i++)
                    free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
                ctx2->dir_idx=0;
            }
        }
    }
    if(ctx2->dir_idx==0){
        if(ctx2->data_idx!=0 && ctx2->batch_flush_len!=0)
            tlb_shootdown_issue_nolock(ctx2->root_pg, ctx2->batch_start_va, ctx2->batch_flush_len);
    }
    else    tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    if(ctx1->do_free==1){
        for(int i=0;i<ctx2->data_idx;i++)
            free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                ctx2->data_page_batch[i].len);
    }
    //directory pte
    for(int i=0;i<ctx2->dir_idx;i++)
        free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
    ctx2->data_idx=0;
    ctx2->dir_idx=0;
    if(locked_by_me==1)
        release(ctx1->pt_lock);
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
    sfence_vma(0, 0);
    return del_size;
}

void uvmunmap(struct map_context *ctx1){
#ifdef DEBUG_VM
    VM_TRACE("va=%p size=0x%llx do_free=%d\n", (void *)va, size, do_free);
#endif
    if(ctx1==NULL || ctx1->pagetable==NULL)
        panic("Invalid argument.");
    if(ctx1->size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (ctx1->size == 0)    panic("mappages: size");
    ctx1->cur_level=2;      //Start from highest level.
    struct mmu_gather ctx2;
    memset(&ctx2, 0, sizeof(struct mmu_gather));
    ctx2.batch_start_va=ctx1->start_va;
    ctx2.fullmm=0;
    ctx2.root_pg=ctx1->pagetable;
    uint64 del_size=uvmunmap_helper(ctx1, &ctx2);
    if(del_size!=ctx1->size)
        panic("uvmunmap: cannot unmap required size!");
}

//--------------------ALLOC--------------------------
int buddy_alloc(struct alloc_context *ctx1){
    //return 0 when Success , and return -1 when it fail!
    //A shared utility for use and kernel space.
    if(ctx1==NULL || ctx1->seg_start >ctx1->seg_end){
        pr_err("Invalid parameter.");
        return -1;
    }
    if(ctx1->seg_start%PGSIZE!=0)    panic("Split_alloc: unaligned address!");
    uint8 cur_order=i_log2(ctx1->seg_start & -ctx1->seg_start);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)?
        (MAX_ORDER+ORDER_BASE):cur_order;
    uint64 cur_max_size=1ull<<cur_order ,alloc_size;
    uint64 cur_va=ctx1->seg_start, size=ctx1->seg_end-ctx1->seg_start;
    void *mem;
    struct batch_map_entry map_request[8];
    memset(map_request, 0, sizeof(struct batch_map_entry));
    int req_cnt=0;
    //before entering loop, release this lock firstly.
    while(size>0){
        alloc_size=cur_max_size;
        while(size < alloc_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size);
        if (mem == 0) {
            pr_info("uvmalloc failed to allocate cur_ctx1->seg_start=0x%llx size=0x%llx\n", 
                ctx1->seg_start, alloc_size);
            for(int i=0;i<req_cnt;i++)
                free_pages((void *)map_request[i].pa, map_request[i].size);
            uvmdealloc(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .pt_lock=ctx1->pt_lock,
                .seg_start=cur_va,
                .seg_end=ctx1->seg_start,
            });
            return -1;
        }
        memset(mem, 0, alloc_size);
        map_request[req_cnt].pa=(uint64)mem;
        map_request[req_cnt].va=cur_va;
        map_request[req_cnt].size=alloc_size;
        map_request[req_cnt].xperm=ctx1->xperm;
        req_cnt++;
        if(req_cnt>=8){
            //Process accumulated mapping requests.(full batch, commit.)
            for(int i=0;i<req_cnt;i++){
                if(mappages(&(struct map_context){
                    .pagetable=ctx1->pagetable,
                    .start_va=map_request[i].va,
                    .size=map_request[i].size,
                    .pa=map_request[i].pa,
                    .xperm=map_request[i].xperm
                })!=0){
                    for(int j=i;j<req_cnt;j++)
                        free_pages((void *)map_request[j].pa, map_request[j].size);
                    uvmdealloc(&(struct alloc_context){
                        .pagetable=ctx1->pagetable,
                        .pt_lock=ctx1->pt_lock,
                        .seg_start=cur_va,
                        .seg_end=ctx1->seg_start
                    });
                    return -1;
                }
            }
            req_cnt=0;
            memset(map_request, 0, sizeof(struct batch_map_entry));
            release(ctx1->pt_lock);
        }
        pr_info("uvmalloc -> mappages cur_ctx1->seg_start=0x%llx alloc_size=0x%llx\n", 
            cur_va, alloc_size);
        if(size>alloc_size)  size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        cur_order=i_log2(cur_va & -cur_va);
        cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull << cur_order;
    }
    for(int i=0;i<req_cnt;i++){
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=map_request[i].va,
            .size=map_request[i].size,
            .pa=map_request[i].pa,
            .xperm=map_request[i].xperm
        })!=0){
            for(int j=i;j<req_cnt;j++)
                free_pages((void *)map_request[i].pa, map_request[i].size);
            uvmdealloc(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .pt_lock=ctx1->pt_lock,
                .seg_start=map_request[i].va,
                .seg_end=ctx1->seg_start,
            });
        }
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

//Input dst_pg is newly allocated, as lower-level of src_pg.
//Deal with partial copy in leaf pte.!!!
int copy_partial_leaf_private(struct vm_sub_copy_ctx *p1){   //Reuse ret_va as src
    uint64 basic_stride=get_step_size(p1->dst_level), chunk_size;
    int i= p1->base_va >> PXSHIFT(p1->dst_level) & PXMASK;
    uint64 src_flags=PTE_FLAGS(p1->src_pt[i]), src_pa;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(p1->src_pt_lock!=NULL && !holding(p1->src_pt_lock)){
        acquire(p1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(p1->dst_pt_lock!=NULL && !holding(p1->dst_pt_lock)){
        acquire(p1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(i<512 && p1->size>0){ //start with zero(always)
        //Pseduo-splitting of the heap reservation area, while maintaining physical continuity.
        src_pa=PTE2PA(p1->src_pt[i]);
        //in heap region but scratted and out-of-heap.
        if(p1->size >= basic_stride){   //Perfect fit(considering buddy_system)
            //Use specific physical properties(buddy system)
            uint64 max_cont_len=basic_stride, cmp_perm=PTE_FLAGS(p1->src_pt[i]);
            uint64 step=1;
            while(i+step < 512 && max_cont_len < p1->size){
                pte_t next_pte=p1->src_pt[i+step];
                if((next_pte & PTE_V)==0)   break;
                if(PTE2PA(next_pte) != src_pa + step * basic_stride)
                    break;
                if((next_pte & CMP_PXMASK) != cmp_perm)
                    break;
                max_cont_len+=basic_stride;
                step++;
            }
            uint64 max_phy_size=1ull<<(get_order(src_pa) + ORDER_BASE);
            chunk_size=max_phy_size;
            while(chunk_size > max_cont_len){
                chunk_size /= 2;
                step /=2;
            }
            void *mem=alloc_memory(chunk_size);
            if(mem==NULL){
                VM_TRACE("");
                return -1;
            }
            memmove(mem, (void *)src_pa, chunk_size);
            for(int j = 0; j < step; j++){
                // Reuse flags from original entry
                uint64 inner_pa = (uint64)mem + j * basic_stride;
                p1->dst_pt[i+j] = PA2PTE(inner_pa) | src_flags;
            }
            i+=step;
            p1->size-=chunk_size;
            p1->base_va+=chunk_size;
        }
        else{   //Partial/Split(p1->max_sz < basic_stride)
            if(p1->dst_level!=0){    //Current copy is insuffient for filling current level stride
                //Or it is an intermidate node.(alloc one pagetable and enter sub-level recursively.)
                pte_t *new_dst_pt=(pte_t *)alloc_memory(PGSIZE);
                if(new_dst_pt==NULL){
                    pr_err("alloc failed,");
                    return -1;
                }
                memset((void *)new_dst_pt, 0, PGSIZE);
                p1->dst_pt[i]=PA2PTE((uint64)new_dst_pt) | PTE_V;   //As a directroy PTE
                return copy_partial_leaf_private(&(struct vm_sub_copy_ctx){
                    .base_va=p1->base_va,
                    .dst_pt=new_dst_pt,
                    .dst_level=p1->dst_level-1,
                    .new_rblocks=p1->new_rblocks,
                    .size=p1->size,
                    .src_pt=p1->src_pt
                    //Current src_pte is leaf_pte, lack of lower level pagetable.
                }); //Deep copy
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
    if(src_locked_by_me)
        release(p1->src_pt_lock);
    if(dst_locked_by_me)
        release(p1->dst_pt_lock);
    return 0;
}

//Absolutely deal with the leaf_node(shared memory),So all pte must be assigned with pte_COW.
//
int copy_partial_leaf_shared(struct vm_sub_copy_ctx *p1){
    uint64 basic_stride=get_step_size(p1->dst_level);
    int i= p1->base_va >> PXSHIFT(p1->dst_level) & PXMASK;
    uint64 src_flags=PTE_FLAGS(p1->src_pt[i]), src_pa=PTE2PA(p1->src_pt[i]);
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(p1->src_pt_lock!=NULL && !holding(p1->src_pt_lock)){
        acquire(p1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(p1->dst_pt_lock!=NULL && !holding(p1->dst_pt_lock)){
        acquire(p1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(i<512 && p1->size>0){
        src_pa=PTE2PA(p1->src_pt[i]);
        //get the PA each time, instead of accumulate.(Loss of Continuity.)
        if(p1->dst_level!=0){
            //Split the hugepage
            uint64 cur_pa=PTE2PA(p1->src_pt[i]), sub_basic_stride=get_step_size(p1->dst_level-1);
            pagetable_t src_child_pt=(pagetable_t)alloc_memory(PGSIZE);
            if(src_child_pt==0)    panic("Out-of-memory.");
            memset(src_child_pt, 0, PGSIZE);
            pagetable_t dst_child_pt=(pagetable_t)alloc_memory(PGSIZE);
            if(dst_child_pt==0)    panic("Out-of-memory.");
            memset(dst_child_pt, 0, PGSIZE);
            //Determine the permission based on the current level
            uint64 start_pfn=p1->base_va % SUPERPGSIZE / PGSIZE;
            uint64 end_pfn=(p1->base_va + p1->size)% SUPERPGSIZE / PGSIZE;
            for(int j=0;j<512;j++){
                if(p1->dst_level==1){
                    //Both the parent and child processes have their corresponding PTEs
                    //set to read-only and marked for COW.
                    src_child_pt[j]=PA2PTE(cur_pa) | (src_flags & ~PTE_W) | PTE_COW;
                    if(j>=start_pfn && j < end_pfn)
                        dst_child_pt[j]=src_child_pt[j];
                }
                else{
                    src_child_pt[j]=(PA2PTE(cur_pa) | PTE_V);    //not the lowest level
                    dst_child_pt[j]=src_child_pt[j];
                }
                cur_pa+=sub_basic_stride;
            }
            p1->src_pt[i]=PA2PTE(src_child_pt) | PTE_V;
            p1->dst_pt[i]=PA2PTE(dst_child_pt) | PTE_V;
            if(in_res_area(p1->base_va, p1->new_rblocks)==1){
                //update old_rblock's metadata.
                uint64 rb_idx=(p1->base_va - p1->new_rblocks[0].va) / SUPERPGSIZE;
                if(p1->old_rblocks[rb_idx].pa!=0){
                    p1->old_rblocks[rb_idx].pa=0;
                    p1->old_rblocks[rb_idx].promoted=0;
                    p1->old_rblocks[rb_idx].is_scattered=0;
                    p1->old_rblocks[rb_idx].pop_count=0;
                    p1->new_rblocks[rb_idx]=p1->old_rblocks[rb_idx];
                    //udpate new_rblock's metadata.
                }
            }
            if(p1->dst_level!=1){
                return copy_partial_leaf_shared(&(struct vm_sub_copy_ctx){
                    .base_va=p1->base_va,
                    .dst_pt=dst_child_pt,      //update pagetable to directory pte
                    .src_pt=src_child_pt,
                    .dst_level=p1->dst_level-1,
                    .new_rblocks=p1->new_rblocks,
                    .old_rblocks=p1->old_rblocks,
                    .size=p1->size,
                });  //Undertake the actual work.
            }
            else{
                inc_ref_range((void *)(src_pa+start_pfn*PGSIZE), p1->size);
                return 0;   //already finish copy.
            }
        }
        if(p1->size >= basic_stride){
            //Once the COW triggered, the default assumption of contiguous virtual and physical
            //addresses no longer holds, therefore, a scan is required.
            uint64 max_cont_len=basic_stride;
            int step=1;
            while(i+step < 512 && max_cont_len < p1->size){
                pte_t next_pte=p1->src_pt[i+step];
                if((next_pte & PTE_V)==0)   break;
                if(PTE2PA(next_pte) != src_pa + step * basic_stride)
                    break;
                if((next_pte & CMP_PXMASK) != (p1->src_pt[i] & CMP_PXMASK))
                    break;
                max_cont_len+=basic_stride;
                step++;
            }
            uint64 max_phy_size=1ull<<(get_order(src_pa) + ORDER_BASE);
            uint64 chunk_size=max_phy_size;
            while(chunk_size > max_cont_len){
                chunk_size/=2;
                step/=2;
            }
            for(int j = 0; j < step; j++){
                // Reuse flags from original entry
                p1->src_pt[i+j] = (p1->src_pt[i+j] & ~PTE_W) | PTE_COW;
                p1->dst_pt[i+j] = p1->src_pt[i+j];
            }
            inc_ref_range((void *)src_pa, chunk_size);
            i+=step;
            p1->size-=chunk_size;
            src_pa+=chunk_size;
            p1->base_va+=chunk_size;
        }
        else{
            //The lowest level, minimun size is PGSIZE.
            p1->src_pt[i]=(p1->src_pt[i] & ~PTE_W) | PTE_COW;
            p1->dst_pt[i]= p1->src_pt[i];
            inc_ref_range((void *)PTE2PA(p1->src_pt[i]), PGSIZE);
            return 0;
        }
    }
    if(dst_locked_by_me)
        release(p1->dst_pt_lock);
    if(src_locked_by_me)
        release(p1->src_pt_lock);
    return 0;
}



// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
uint64 uvmalloc(struct alloc_context * ctx1){
    if(ctx1==NULL)  return 0;
    if(ctx1->seg_start % PGSIZE !=0 || ctx1->seg_end % PGSIZE !=0 
        || ctx1->seg_start >= ctx1->seg_end)
        return 0;
    //Verify that xperm is configured with PTE_R and PTE_U
    if((ctx1->xperm & (PTE_R | PTE_U))==0)
        ctx1->xperm |= (PTE_R | PTE_U);
    if(buddy_alloc(ctx1)!=0)
        return 0;
    VM_TRACE("uvmalloc exiting success newsz=0x%llx\n", ctx1->seg_end);
    return ctx1->seg_end;
}

//-------------------DEALLOC------------------------------
//Keep the same logic as uvmalloc(Binary Buddy Decomposition!)
//A User/Kernel agnostic function.
//TIPS: for dealloc, passed-in seg_end < seg_start.
uint64 uvmdealloc(struct alloc_context *ctx1){
#ifdef DEBUG_VM
    VM_TRACE("oldsz=0x%llx newsz=0x%llx\n", oldsz, newsz);
#endif
    if(ctx1==NULL || ctx1->pagetable==NULL){
        pr_err("Invalid parameter.");
        return -1;
    }
    if(ctx1->seg_end >= ctx1->seg_start){
        VM_TRACE("uvmdealloc no-op new>=old, returning oldsz=0x%llx\n", oldsz);
        return ctx1->seg_start;
    }
    uint64 aligned_newsz=PGROUNDUP(ctx1->seg_end), aligned_oldsz=PGROUNDUP(ctx1->seg_start);
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
            uvmunmap(&(struct map_context){
                .pagetable=ctx1->pagetable,
                .start_va=cur_va,
                .size=free_size,
                .do_free=1,
                .pt_lock=ctx1->pt_lock
            });
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
    return ctx1->seg_end;
}

//----------------------END dealloc------------------------------------------------------

// Recursively free page-table pages totally 
// fixme: consider tlb_shootdown_issue_nolock,
void freewalk(pagetable_t pagetable, int do_free, int level, struct mmu_gather *ctx2) {
#ifdef DEBUG_VM
    if (level == 2)
        VM_TRACE("Freewalk Start: pt=%p\n", pagetable);
#endif
    pte_t *pte;// there are 2^9 = 512 PTEs in a page table.
    uint64 pa;
    uint64 va_step, cur_order, standard_stride=get_step_size(level);
    int cur_vpn=0;
    while(cur_vpn<512){
        pte = &pagetable[cur_vpn];
        pa = PTE2PA(*pte);    //child pagetable or physical
        if ((*pte & PTE_V) && PTE_LEAF(*pte) == 0) {
            // this PTE points to a lower-level page table.
            #ifdef DEBUG_VM
                VM_TRACE("%sDir: idx=%d -> next_pa=%p\n", INDENT_STR(level), idx, (void*)pa);
            #endif
            freewalk((pagetable_t)pa, do_free, level-1, ctx2);
            ctx2->dir_pa_batch[ctx2->dir_idx++]=PTE2PA(*pte);
            *pte=0;
            if(ctx2->dir_idx >= MMU_BATCH_SIZE){
                tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
                for(int i=0;i<ctx2->dir_idx;i++)
                    free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
                ctx2->dir_idx=0;
            }
            cur_vpn++;
        } else if (*pte & PTE_V) {   //leaf-node,release the physical page
            //check if next page is associated with current page(Non-contiguity of physical addresses.)
            cur_order=get_order(pa);    //Specific area(Cannot be freed),so skip additional checks.
            uint64 max_phy_size=1ull<<(cur_order+ORDER_BASE), cont_len=0;
            //Not limit by max_sz(free the entire pagetable), so unnecessary to adjust the max_phy_size
            pte_t tmp_pte=0;
            uint64 cmp_perm=(*pte & CMP_PXMASK), vpn_inc=0;
            int k=0;
            for(;k<512;k++){
                if(cur_vpn+k>=512 || cont_len >= max_phy_size)  break;
                tmp_pte=pagetable[k+cur_vpn];
                if(tmp_pte==0 || (tmp_pte & PTE_V)==0 || PTE_LEAF(tmp_pte)==0)
                    break;
                if(cmp_perm != (tmp_pte & CMP_PXMASK))    break;
                if(PTE2PA(tmp_pte) != pa + k*standard_stride)    break;
                cont_len+=standard_stride;
            }
            va_step=max_phy_size;
            vpn_inc=k;
            while(va_step > cont_len && va_step > standard_stride){
                va_step /=2;
                vpn_inc /=2;
            }
            for(int j=0;j<vpn_inc;j++){
                if(cur_vpn+j==512){
                    panic("freewalk: alignment error");
                    break;
                }
                pagetable[cur_vpn+j]=0; //Clear the relevant PTEs
            }
            cur_vpn += vpn_inc;
            ctx2->data_page_batch[ctx2->data_idx].delete_pa=pa;
            ctx2->data_page_batch[ctx2->data_idx++].len=va_step;
            // va_start doesn't matter(just trigger an enforced global TLB invalidation.)
            if(ctx2->data_idx >= MMU_BATCH_SIZE){
                tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
                if(do_free==1){
                    for(int i=0;i<MMU_BATCH_SIZE;i++)
                        free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                            ctx2->data_page_batch[i].len);
                }
                ctx2->data_idx=0;
            }
        }
        else    pagetable[cur_vpn++]=0;
    }

    if(pagetable == ctx2->root_pg){
        ctx2->dir_pa_batch[ctx2->dir_idx++]=(uint64)pagetable;  //deal with 
        tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
        if(do_free==1){
            for(int i=0;i<ctx2->data_idx;i++)
                free_pages((void *)ctx2->data_page_batch[i].delete_pa, 
                    ctx2->data_page_batch[i].len);
        }
        //directory pte
        for(int i=0;i<ctx2->dir_idx;i++)
            free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
        ctx2->data_idx=0;
        ctx2->dir_idx=0;
    }
}

//Iterative version
void freewalk_iter(pagetable_t pagetable, int do_free, struct mmu_gather *ctx2){
#ifdef DEBUG_VM
    if (level == 2)
        VM_TRACE("Freewalk Start: pt=%p\n", pagetable);
#endif
    if(pagetable==NULL || ctx2==NULL || ctx2->root_pg==NULL)
        panic("Invalid argument.");
    if(ctx2->data_idx!=0 || ctx2->dir_idx!=0)
        panic("The input argument(mmu_gather) were not reset to zero upon entry.");
    int cur_level=2;
    pte_t *pte;// there are 2^9 = 512 PTEs in a page table.
    uint64 pa, va_step, cur_order, basic_stride=get_step_size(cur_level);
    struct map_iter_context cur_state_array[3];
    memset(cur_state_array, 0, sizeof(cur_state_array));
    cur_state_array[cur_level].cur_level=2;
    cur_state_array[cur_level].cur_vpn=0;
    cur_state_array[cur_level].pagetable=pagetable;
    pagetable_t cur_pg;
    uint64 cur_vpn, end_vpn;
    while(cur_level < 3){
        cur_pg=cur_state_array[cur_level].pagetable;
        cur_vpn=cur_state_array[cur_level].cur_vpn;
        basic_stride=get_step_size(cur_level);
        while(cur_vpn<512){     //Hardcoded to 512 for wholescale deletion.
            pte = &cur_pg[cur_vpn];
            pa = PTE2PA(*pte);    //child cur_pg or physical
            if ((*pte & PTE_V) && PTE_LEAF(*pte) == 0) {
                // this PTE points to a lower-level page table.
                #ifdef DEBUG_VM
                    VM_TRACE("%sDir: idx=%d -> next_pa=%p\n", INDENT_STR(level), idx, (void*)pa);
                #endif
                cur_state_array[cur_level].cur_level=cur_level;
                cur_state_array[cur_level].cur_vpn=cur_vpn+1;
                cur_state_array[cur_level].pagetable=cur_pg;
                cur_pg=(pagetable_t)PTE2PA(*pte);
                cur_level--;
                basic_stride=get_step_size(cur_level);
                cur_vpn=0;
            } else if (*pte & PTE_V) {   //leaf-node,release the physical page
                //check if next page is associated with current page(Non-contiguity of physical addresses.)
                cur_order=get_order(pa);    //Specific area(Cannot be freed),so skip additional checks.
                uint64 max_phy_size=1ull<<(cur_order+ORDER_BASE), cont_len=0;
                //Not limit by max_sz(free the entire cur_pg), so unnecessary to adjust the max_phy_size
                pte_t tmp_pte=0;
                uint64 cmp_perm=(*pte & CMP_PXMASK), vpn_inc=0;
                int k=0;
                for(;k<512;k++){
                    if(cur_vpn+k>=512 || cont_len >= max_phy_size)  break;
                    tmp_pte=cur_pg[k+cur_vpn];
                    if(tmp_pte==0 || (tmp_pte & PTE_V)==0 || PTE_LEAF(tmp_pte)==0)
                        break;
                    if(cmp_perm != (tmp_pte & CMP_PXMASK))    break;
                    if(PTE2PA(tmp_pte) != pa + k*basic_stride)    break;
                    cont_len+=basic_stride;
                }
                va_step=max_phy_size;
                vpn_inc=k;
                while(va_step > cont_len && va_step > basic_stride){
                    va_step /=2;
                    vpn_inc /=2;
                }
                ctx2->data_page_batch[ctx2->data_idx].delete_pa=pa;
                ctx2->data_page_batch[ctx2->data_idx++].len=va_step;
                // va_start doesn't matter(just trigger an enforced global TLB invalidation.)
                if(ctx2->data_idx >= MMU_BATCH_SIZE){
                    tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
                    if(do_free==1){
                        for(int i=0;i<MMU_BATCH_SIZE;i++)
                            free_pages((void *)ctx2->data_page_batch[i].delete_pa, ctx2->data_page_batch[i].len);
                    }
                    ctx2->data_idx=0;
                }
                for(int j=0;j<vpn_inc;j++){
                    if(cur_vpn+j==512){
                        panic("freewalk: alignment error");
                        break;
                    }
                    cur_pg[cur_vpn+j]=0; //Clear the relevant PTEs
                }
                cur_vpn+=vpn_inc;
            }
            else    cur_pg[cur_vpn++]=0; //Invalid pte
        }
        cur_level++;
        if(cur_level < 3){      //Global reclaim(include directory page.)
            ctx2->dir_pa_batch[ctx2->dir_idx++]=(uint64)cur_pg;
            //start_va doesn't matteer
            cur_state_array[cur_level].pagetable[cur_state_array[cur_level].cur_vpn-1]=0;
            if(ctx2->dir_idx >= MMU_BATCH_SIZE){
                tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
                for(int i=0;i<MMU_BATCH_SIZE;i++)
                    free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
                ctx2->dir_idx=0;
            }
        }
    }
    // global reclaim(also include root pagetable)
    ctx2->dir_pa_batch[ctx2->dir_idx++]=pagetable;
    tlb_shootdown_issue_nolock(ctx2->root_pg, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    if(do_free==1){
        for(int i=0;i<ctx2->data_idx;i++)
            free_pages((void *)ctx2->data_page_batch[i].delete_pa, ctx2->data_page_batch[i].len);
    }
    //directory pte
    for(int i=0;i<ctx2->dir_idx;i++)
        free_pages((void *)ctx2->dir_pa_batch[i], PGSIZE);
    ctx2->dir_idx=0;
    ctx2->data_idx=0;
}


int copy_heap_private(struct vm_dupl_ctx *v1){
    if(v1->dst_level!=1 || in_res_area(v1->base_va, v1->new_rblocks)!=1){
        panic("Interface Conflict");
        return -1;
    }
    uint64 cur_va=v1->base_va, rb_idx, src_pa;
    pte_t src_pte;
    int idx=0, inc_idx=0;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(in_res_area(cur_va, v1->new_rblocks)==1 && cur_va<v1->end_va){
        inc_idx++;
        idx= cur_va >> PXSHIFT(v1->dst_level) & PXMASK;
        src_pte = v1->src_pg[idx];
        rb_idx=(cur_va-v1->new_rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
        uint64 bp_idx=(cur_va - v1->new_rblocks[0].va) % SUPERPGSIZE / PGSIZE;
        uint64 bp_step=MIN(512-bp_idx, (v1->end_va - cur_va) / PGSIZE);
        if(v1->new_rblocks[rb_idx].pa!=0){
            src_pa = PTE2PA(src_pte);    // child pagetable or physical address.
            //have allocated hugepage
            uint64 new_superpage=(uint64)alloc_memory(SUPERPGSIZE);
            if(new_superpage==0)    return -1;
            memmove((void *)new_superpage, (void *)(v1->new_rblocks[rb_idx].pa), SUPERPGSIZE);
            //migrate data
            uint64 cur_pa=new_superpage+PGSIZE*bp_idx, cur_flags;
            memset(v1->new_rblocks[rb_idx].bitmap, 0, bp_idx);
            memset(&v1->new_rblocks->bitmap[bp_idx+bp_step], 0, 512-(bp_idx+bp_step));
            if(v1->new_rblocks[rb_idx].promoted==0 || v1->base_va + SUPERPGSIZE > v1->end_va){
                uint64 mem=(uint64)alloc_memory(PGSIZE);
                if(mem==0)      panic("OOM.");
                memset((void *)mem, 0, PGSIZE);
                pte_t *src_leaf_pte=(pte_t *)src_pa, *dst_leaf_pte=(pte_t *)mem;
                for(int j=0;j<bp_step;j++){
                    if(bp_idx+j>=512)    panic("out-of-range!");
                    if(v1->new_rblocks[rb_idx].promoted==0)
                        cur_flags=PTE_FLAGS(src_leaf_pte[bp_idx+j]);
                    else{   //already promoted into hugepage.
                        if(PTE_LEAF(src_pte)==0)    panic("Should be leaf.");
                        cur_flags=PTE_FLAGS(src_pte);
                    }
                    //Partial of the aggregated hugepage, use the leaf_pte permission.
                    if(v1->new_rblocks[rb_idx].bitmap[bp_idx]!=0)
                        dst_leaf_pte[bp_idx+j]=cur_flags | PA2PTE(cur_pa);
                    cur_pa+=PGSIZE;
                }
                v1->dst_pg[idx]=PA2PTE(mem) | PTE_V;
                cur_va+=bp_step*PGSIZE;
            }
            else{
                if(PTE_LEAF(src_pte)==0)    panic("Shoule be leaf pte.");
                v1->dst_pg[idx]=PA2PTE(new_superpage) | PTE_FLAGS(src_pte);
                cur_va+=SUPERPGSIZE;
                inc_ref_range((void *)PTE2PA(src_pte), SUPERPGSIZE);
            }
            v1->new_rblocks[rb_idx].pa=new_superpage;//Update PA last;preserve remaining parameters.
        }
        else{   //Standard pte,however considering it's located within the heap region,
                //It still needs to be processed here accordingly.
            if(src_pte==0 || (src_pte & PTE_V) ==0){
                cur_va+=SUPERPGSIZE;
                continue;
            }
            if(PTE_LEAF(src_pte)==1)    panic("Should not be leaf pte.");
            pagetable_t cur_src_pg= (pagetable_t)PTE2PA(v1->src_pg[idx]), cur_dst_pg=0;
            for(int i=0;i<bp_step;i++){
                if((cur_src_pg[i+bp_idx] & PTE_V) !=0){
                    if(cur_dst_pg==0){
                        cur_dst_pg=(pagetable_t)alloc_memory(PGSIZE);
                        if(cur_dst_pg==0)   panic("OOM.");
                        memset(cur_dst_pg, 0, PGSIZE);
                        //hangle to the directory tree.
                        v1->dst_pg[idx]=PA2PTE(cur_dst_pg) | PTE_V;
                    }
                    cur_dst_pg[i+bp_idx]=cur_src_pg[i+bp_idx];
                }
            }
            cur_va+=bp_step*PGSIZE;
        }
    }
    *(uint64 *)(v1->ret_va)=cur_va;
    if(src_locked_by_me==1)
        release(v1->src_pt_lock);
    if(dst_locked_by_me==1)
        release(v1->dst_pt_lock);
    return inc_idx;
}

//Designated for shared memory and non-COW case, 
// so we just copying PTE and increment reference count.
int copy_heap_shared(struct vm_dupl_ctx *v1){
    if(v1->dst_level!=1 || in_res_area(v1->base_va, v1->new_rblocks)!=1){
        pr_err("Interface Conflict");
        return -1;
    }
    //In this case, copying the whole valid heap reservation area.
    uint64 cur_va=v1->base_va, rb_idx;
    pte_t src_pte;
    uint64 src_pa;
    int idx=0, inc_idx=0;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(in_res_area(cur_va, v1->new_rblocks)==1 && cur_va<v1->end_va){
        inc_idx++;
        idx= cur_va >> PXSHIFT(v1->dst_level) & PXMASK;
        src_pte = v1->src_pg[idx];
        rb_idx=(cur_va-v1->new_rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
        uint64 bp_idx=(cur_va - v1->new_rblocks[0].va) % SUPERPGSIZE / PGSIZE;
        uint64 bp_step=MIN(512-bp_idx, (v1->end_va - cur_va) / PGSIZE);
        if(v1->new_rblocks[rb_idx].pa!=0){
            src_pa = PTE2PA(src_pte);    // child pagetable or physical address.
            uint64 cur_pa=src_pa, sub_basic_stride=get_step_size(v1->dst_level-1);
            pagetable_t dst_new_dire=(pagetable_t)alloc_memory(PGSIZE);
            if(dst_new_dire==0)     panic("OOM.");
            memset((void *)dst_new_dire, 0, PGSIZE);
            pagetable_t src_dire=(pagetable_t)PTE2PA(v1->src_pg[idx]);
            for(int j=0;j<512;j++){
                //Both the parent and child processes have their corresponding PTEs
                //set to read-only and marked for COW.
                if(j>=bp_idx && j < bp_idx+bp_step){
                    if(src_dire[j]==0 || (src_dire[j] & PTE_V)==0)  continue;
                    dst_new_dire[j]=src_dire[j];
                    if((src_dire[j] & PTE_IO)==0)
                        inc_ref_range((void *)PTE2PA(src_dire[j]), PGSIZE);
                    else    panic("Detect an io_mapped PTE.");
                }
                cur_pa+=sub_basic_stride;
            }
            v1->dst_pg[idx]=PA2PTE(dst_new_dire) | PTE_V;   //mount
            //update old_rblock's metadata.
            v1->new_rblocks[rb_idx]=v1->old_rblocks[rb_idx];
            memset(v1->new_rblocks[rb_idx].bitmap, 0, bp_idx);
            memset(&v1->new_rblocks->bitmap[bp_idx+bp_step], 0, 512-(bp_idx+bp_step));
            //udpate new_rblock's metadata.
        }
        else{   //Standard pte,however considering it's located within the heap region,
                //It still needs to be processed here accordingly.
            if(src_pte==0 || (src_pte & PTE_V) ==0){
                cur_va+=SUPERPGSIZE;
                continue;
            }
            if(PTE_LEAF(src_pte)==1)    panic("Should not be leaf pte.");
            pagetable_t cur_src_pg= (pagetable_t)PTE2PA(v1->src_pg[idx]), cur_dst_pg=0;
            for(int i=0;i<bp_step;i++){
                if((cur_src_pg[i+bp_idx] & PTE_V) !=0){
                    if(cur_dst_pg==0){
                        cur_dst_pg=(pagetable_t)alloc_memory(PGSIZE);
                        if(cur_dst_pg==0)   panic("OOM.");
                        memset(cur_dst_pg, 0, PGSIZE);
                        //hangle to the directory tree.
                        v1->dst_pg[idx]=PA2PTE(cur_dst_pg) | PTE_V;
                    }
                    cur_dst_pg[i+bp_idx]=cur_src_pg[i+bp_idx];
                    if((cur_dst_pg[i+idx] & PTE_IO)==0)
                        inc_ref_range((void *)PTE2PA(cur_dst_pg[i+bp_idx]), PGSIZE);
                    else    panic("Detect an io_mapped PTE.");
                }
            }
            cur_va+=bp_step*PGSIZE;
        }
    }
    *(uint64 *)(v1->ret_va)=cur_va;
    if(src_locked_by_me)
        release(v1->src_pt_lock);
    if(dst_locked_by_me)
        release(v1->dst_pt_lock);
    return inc_idx;
}

int copy_heap_cow(struct vm_dupl_ctx *v1){
    if(v1->dst_level!=1 || in_res_area(v1->base_va, v1->new_rblocks)!=1){
        pr_err("Interface Conflict");
        return -1;
    }
    uint64 cur_va=v1->base_va, rb_idx;
    pte_t src_pte;
    uint64 src_pa;
    int idx=0, inc_idx=0, src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(cur_va < v1->new_rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE 
        && cur_va < v1->end_va){
        inc_idx++;
        idx= cur_va >> PXSHIFT(v1->dst_level) & PXMASK;
        src_pte = v1->src_pg[idx];
        rb_idx=(cur_va-v1->new_rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
        uint64 bp_idx=(cur_va - v1->new_rblocks[0].va) % SUPERPGSIZE / PGSIZE;
        uint64 bp_step=MIN(512-bp_idx, (v1->end_va - cur_va) / PGSIZE);
        if(v1->new_rblocks[rb_idx].pa!=0){
            src_pa = PTE2PA(src_pte);    // child pagetable or physical address.
            uint64 cur_pa=src_pa, sub_basic_stride=get_step_size(v1->dst_level-1);
            //have allocated hugepage.
            if((src_pte & PTE_V)==0 || !(src_pte & PTE_IO))
                panic("Detech an io_mapped PTE while dealing heap copy.");
            if(v1->new_rblocks[rb_idx].promoted==1){       //split it first.
                pagetable_t src_new_dire=(pagetable_t)alloc_memory(PGSIZE);
                if(src_new_dire==0)     panic("OOM.");
                memset((void *)src_new_dire, 0, PGSIZE);
                uint64 src_flags=PTE_FLAGS(src_pte);    //as leaf, flags is meaningful.
                for(int j=0;j<512;j++){
                    src_new_dire[j]=PA2PTE(cur_pa) | src_flags;
                    cur_pa+=PGSIZE;
                }
                //Mount the new directory pte to replace the previous leaf pte.
                v1->src_pg[idx]=PA2PTE(src_new_dire) | PTE_V;
                v1->old_rblocks[rb_idx].promoted=0;
            }
            pagetable_t dst_new_dire=(pagetable_t)alloc_memory(PGSIZE);
            if(dst_new_dire==0)     panic("OOM.");
            memset((void *)dst_new_dire, 0, PGSIZE);
            pagetable_t src_dire=(pagetable_t)PTE2PA(v1->src_pg[idx]);
            for(int j=0;j<512;j++){
                //Both the parent and child processes have their corresponding PTEs
                //set to read-only and marked for COW.
                if(j>=bp_idx && j < bp_idx+bp_step){
                    if(src_dire[j]==0 || (src_dire[j] & PTE_V)==0)  continue;
                    src_dire[j]=(src_dire[j] & ~PTE_W) | PTE_COW;
                    dst_new_dire[j]=src_dire[j];
                    if((src_dire[j] & PTE_IO)==0)
                        inc_ref_range((void *)PTE2PA(src_dire[j]), PGSIZE);
                    else{
                        pr_err("Detach an io_mapped PTE while dealing heap copy.");
                        return -1;
                    }
                }
                cur_pa+=sub_basic_stride;
            }
            v1->dst_pg[idx]=PA2PTE(dst_new_dire) | PTE_V;   //mount
            //update old_rblock's metadata.
            v1->new_rblocks[rb_idx]=v1->old_rblocks[rb_idx];
            memset(v1->new_rblocks[rb_idx].bitmap, 0, bp_idx);
            memset(&v1->new_rblocks->bitmap[bp_idx+bp_step], 0, 512-(bp_idx+bp_step));
            //udpate new_rblock's metadata.
        }
        else{   //Standard pte,however considering it's located within the heap region,
                //It still needs to be processed here accordingly.
            if(src_pte==0 || (src_pte & PTE_V) ==0){
                cur_va+=SUPERPGSIZE;
                continue;
            }
            if(PTE_LEAF(src_pte)==1){
                pr_err("Should not be leaf pte.");
                return -1;
            }
            pagetable_t cur_src_pg= (pagetable_t)PTE2PA(v1->src_pg[idx]), cur_dst_pg=0;
            for(int i=0;i<bp_step;i++){
                if((cur_src_pg[i+bp_idx] & PTE_V) !=0){
                    if(cur_dst_pg==0){
                        cur_dst_pg=(pagetable_t)alloc_memory(PGSIZE);
                        if(cur_dst_pg==0){
                            pr_err("OOM.");
                            return -1;
                        }
                        memset(cur_dst_pg, 0, PGSIZE);
                        //hangle to the directory tree.
                        v1->dst_pg[idx]=PA2PTE(cur_dst_pg) | PTE_V;
                    }
                    if((cur_src_pg[i+bp_idx] & PTE_IO)==0){
                        cur_src_pg[i+bp_idx] = (cur_src_pg[i+bp_idx] & ~PTE_W) | PTE_COW;
                        cur_dst_pg[i+bp_idx]=cur_src_pg[i+bp_idx];
                        inc_ref_range((void *)PTE2PA(cur_dst_pg[i+bp_idx]), PGSIZE);
                    }
                    else{
                        pr_err("Detech an io_mapped PTE while dealing heap copy.");
                        return -1;
                    }

                }
            }
            cur_va+=bp_step*PGSIZE;
        }
    }
    *(uint64 *)(v1->ret_va)=cur_va;
    if(src_locked_by_me)
        release(v1->src_pt_lock);
    if(dst_locked_by_me)
        release(v1->dst_pt_lock);
    return inc_idx;
}

// Recursively copy page-table pages. Quit recursively When the superpage Error occurs, 
// Directly assign the known PTE to bypass the mappage overhead, significantly improving efficiency
int copywalk_private(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(idx < 512) {
        if(cur_va >= v1->end_va) break;
        if(heap_managed==0 && v1->dst_level==1 
            && in_res_area(cur_va, v1->new_rblocks)==1){
            heap_managed=1;
            uint64 new_ret_va=0;
            int inc_idx=copy_heap_private(&(struct vm_dupl_ctx){
                .base_va=cur_va,
                .end_va=v1->end_va,
                .dst_level=1,
                .dst_pg=v1->dst_pg,
                .src_pg=v1->src_pg, //Level-2 paegtable,able to get the correct sub_pg
                .ret_va=&new_ret_va,
                .new_rblocks=v1->new_rblocks,
                .old_rblocks=v1->old_rblocks,
            }
            );
            cur_va = new_ret_va;
            idx+=inc_idx;
            *(uint64 *)v1->ret_va=cur_va;
            continue;
        }
        pte = v1->src_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if(pte==0 || (pte & PTE_V) ==0)
            va_step=standard_stride;    //case 1: Invalid or don't exist pte at all.
            //NOTE: future extension, use part of PTE to SWAP id to support Swap Entry.
        else if (PTE_LEAF(pte)==0) {    // Case 2: Directory
            new_pte=v1->dst_pg[idx];
            void *mem=NULL;
            if((new_pte & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE);
                if(mem == NULL) return -1;
                memset(mem, 0, PGSIZE);
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->dst_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    return -1;
                }
            }
            if(copywalk_private(&(struct vm_dupl_ctx)
                        {.src_pg=(pagetable_t)pa,
                        .dst_pg=(pagetable_t)mem,
                        .base_va=cur_va,
                        .ret_va=v1->ret_va,
                        .end_va=v1->end_va,
                        .dst_level=v1->dst_level-1,
                        .new_rblocks=v1->new_rblocks})<0)
                return -1;
            va_step = standard_stride;
        } 
        else{
            // Case 3: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 remain_size=v1->end_va - cur_va;
            if(standard_stride > remain_size){
                if(copy_partial_leaf_private(&(struct vm_sub_copy_ctx)
                        {.dst_pt=(pte_t *)v1->dst_pg, 
                        .src_pt=(pte_t *)v1->src_pg, 
                        .base_va=cur_va, 
                        .size=remain_size, 
                        .dst_level=v1->dst_level,
                        .new_rblocks=v1->new_rblocks})!=0)
                    return -1;
            }
            else{
                void *comp_leaf=alloc_memory(standard_stride);
                if(comp_leaf==NULL)     panic("OOM.");
                memset(comp_leaf, 0, standard_stride);
                memmove(comp_leaf, (void *)pa, standard_stride);
                v1->dst_pg[idx]=PA2PTE((uint64)comp_leaf) | PTE_FLAGS(v1->src_pg[idx]);
            }
            va_step=MIN(standard_stride, remain_size);
        }
        idx++;
        cur_va += va_step;
        *(uint64 *)v1->ret_va = cur_va;
    }
    if(src_locked_by_me)        release(v1->src_pt_lock);
    if(dst_locked_by_me)        release(v1->dst_pt_lock);
    return 0;
}

//Designed for direct mapping(shared), which just copying the pte from src pagetable
// And don't increment the reference count.
int copywalk_shared_direct(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(idx < 512) {
        if(cur_va >= v1->end_va) break;
        if(heap_managed==0 && v1->dst_level==1 && 
            in_res_area(cur_va, v1->new_rblocks)==1){
            pr_err("This code path is unreachable under normal conditions.");
            return -1;
        }
        pte = v1->src_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if(pte==0 || (pte & PTE_V) ==0)
            va_step=standard_stride;    //case 1: Invalid or don't exist pte at all.
        else if (PTE_LEAF(pte)==0) {    // Case 2: Directory
            new_pte=v1->dst_pg[idx];
            void *mem=NULL;
            if((new_pte & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE);
                if(mem == NULL) return -1;
                memset(mem, 0, PGSIZE);
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->dst_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    return -1;
                }
            }
            if(copywalk_shared_direct(&(struct vm_dupl_ctx)
                        {.src_pg=(pagetable_t)pa,
                        .dst_pg=(pagetable_t)mem,
                        .base_va=cur_va,
                        .ret_va=v1->ret_va,
                        .end_va=v1->end_va,
                        .dst_level=v1->dst_level-1,
                        .new_rblocks=v1->new_rblocks})<0)
                return -1;
            va_step = standard_stride;
        } 
        else{
            // Case 3: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 remain_size=v1->end_va - cur_va;
            if(standard_stride > remain_size){
                pr_err("Partial leaf copy, shouldn't exist.");
                return -1;
            }
            else    v1->dst_pg[idx]= v1->src_pg[idx];
            va_step=MIN(standard_stride, remain_size);
        }
        idx++;
        cur_va += va_step;
        *(uint64 *)v1->ret_va = cur_va;
    }
    if(src_locked_by_me)        release(v1->src_pt_lock);
    if(dst_locked_by_me)        release(v1->dst_pt_lock);
    return 0;
}

//Designed for COW, which copy the pagetable verbatim without allocating
// additional physical memory.(shared physical page.)
int copywalk_shared_cow(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(idx < 512) {
        if(cur_va >= v1->end_va) break;
        if(heap_managed==0 && v1->dst_level==1 
            && in_res_area(cur_va, v1->new_rblocks)==1){
            heap_managed=1;
            uint64 new_ret_va=0;
            int inc_idx=copy_heap_cow(&(struct vm_dupl_ctx){
                .base_va=cur_va,
                .end_va=v1->end_va,
                .dst_level=1,
                .dst_pg=v1->dst_pg,
                .src_pg=v1->src_pg, //Level-2 paegtable,able to get the correct sub_pg
                .ret_va=&new_ret_va,
                .new_rblocks=v1->new_rblocks,
                .old_rblocks=v1->old_rblocks,
            }
            );
            cur_va = new_ret_va;
            idx+=inc_idx;
            *(uint64 *)v1->ret_va=cur_va;
            continue;
        }
        pte = v1->src_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if((pte & PTE_V)==0 && (pte & PTE_IO)){
            pr_err("detect an io_mapped pte.");
            return -1;
        }
        if(pte==0 || (pte & PTE_V) ==0)
            va_step=standard_stride;    //case 1: Invalid or don't exist pte at all.
        else if (PTE_LEAF(pte)==0) {    // Case 2: Directory
            new_pte=v1->dst_pg[idx];
            void *mem=NULL;
            if((new_pte & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE);
                if(mem == NULL) return -1;
                memset(mem, 0, PGSIZE);
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->dst_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    return -1;
                }
            }
            if(copywalk_shared_cow(&(struct vm_dupl_ctx)
                        {.src_pg=(pagetable_t)pa,
                        .dst_pg=(pagetable_t)mem,
                        .base_va=cur_va,
                        .ret_va=v1->ret_va,
                        .end_va=v1->end_va,
                        .dst_level=v1->dst_level-1,
                        .new_rblocks=v1->new_rblocks})<0)
                return -1;
            va_step = standard_stride;
        } 
        else{
            // Case 3: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 remain_size=v1->end_va - cur_va;
            if(standard_stride > remain_size){
                if(copy_partial_leaf_shared(&(struct vm_sub_copy_ctx){
                        .dst_pt=(pte_t *)v1->dst_pg, 
                        .src_pt=(pte_t *)v1->src_pg, 
                        .base_va=cur_va, 
                        .size=remain_size, 
                        .dst_level=v1->dst_level,
                        .new_rblocks=v1->new_rblocks})!=0)
                    return -1;
            }
            else{
                v1->src_pg[idx]= (v1->src_pg[idx] & ~PTE_W) | PTE_COW;
                v1->dst_pg[idx]= v1->src_pg[idx];
                inc_ref_range((void *)PTE2PA(v1->src_pg[idx]), standard_stride);
            }
            va_step=MIN(standard_stride, remain_size);
        }
        idx++;
        cur_va += va_step;
        *(uint64 *)v1->ret_va = cur_va;
    }
    if(src_locked_by_me)        release(v1->src_pt_lock);
    if(dst_locked_by_me)        release(v1->dst_pt_lock);
    return 0;
}

//Designed for shared but non-COW, copying the original pte, incrementing the physical ref_count.
int copywalk_shared(struct vm_dupl_ctx *v1){
    pte_t pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0;
    if(v1->src_pt_lock!=NULL && !holding(v1->src_pt_lock)){
        acquire(v1->src_pt_lock);
        src_locked_by_me=1;
    }
    if(v1->dst_pt_lock!=NULL && !holding(v1->dst_pt_lock)){
        acquire(v1->dst_pt_lock);
        dst_locked_by_me=1;
    }
    while(idx < 512) {
        if(cur_va >= v1->end_va) break;
        if(heap_managed==0 && v1->dst_level==1 &&
            in_res_area(cur_va, v1->new_rblocks)==1){
            heap_managed=1;
            uint64 new_ret_va=0;
            int inc_idx=copy_heap_shared(&(struct vm_dupl_ctx){
                .base_va=cur_va,
                .end_va=v1->end_va,
                .dst_level=1,
                .dst_pg=v1->dst_pg,
                .src_pg=v1->src_pg, //Level-2 paegtable,able to get the correct sub_pg
                .ret_va=&new_ret_va,
                .new_rblocks=v1->new_rblocks,
                .old_rblocks=v1->old_rblocks,
            }
            );
            cur_va = new_ret_va;
            idx+=inc_idx;
            *(uint64 *)v1->ret_va=cur_va;
            continue;
        }
        pte = v1->src_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if((pte & PTE_V) && (pte & PTE_IO)){
            pr_err("detect io_mapped pte.");
            return -1;
        }
        if(pte==0 || (pte & PTE_V) ==0)
            va_step=standard_stride;    //case 1: Invalid or don't exist pte at all.
        else if (PTE_LEAF(pte)==0) {    // Case 2: Directory
            void *mem=NULL;
            if((v1->dst_pg[idx] & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE);
                if(mem == NULL) return -1;
                memset(mem, 0, PGSIZE);
                v1->dst_pg[idx] = PA2PTE((uint64)mem) | flags | PTE_V;
            }
            else{
                mem=(char *)PTE2PA(v1->dst_pg[idx]);
                //Extract the physical address from the pte.
            }
            if(copywalk_shared(&(struct vm_dupl_ctx)
                        {.src_pg=(pagetable_t)pa,
                        .dst_pg=(pagetable_t)mem,
                        .base_va=cur_va,
                        .ret_va=v1->ret_va,
                        .end_va=v1->end_va,
                        .dst_level=v1->dst_level-1,
                        .new_rblocks=v1->new_rblocks})<0)
                return -1;
            va_step = standard_stride;
        } 
        else{
            // Case 3: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 remain_size=v1->end_va - cur_va;
            if(standard_stride > remain_size && standard_stride!=PGSIZE)
                panic("");
            v1->dst_pg[idx]= v1->src_pg[idx];
            inc_ref_range((void *)PTE2PA(v1->src_pg[idx]), standard_stride);
            va_step=MIN(standard_stride, remain_size);
        }
        idx++;
        cur_va += va_step;
        *(uint64 *)v1->ret_va = cur_va;
    }
    if(src_locked_by_me)
        release(v1->src_pt_lock);
    if(dst_locked_by_me)
        release(v1->dst_pt_lock);
    return 0;
}

// Free user memory pages,(maintain the interface consistency, 
// an non-zero sz implies the need to release the corresponding page)
// Even though the release process itself does not requires the sz parameter.
// Must free page-table pages.
void uvmfree_range(res_block *rblocks, pagetable_t pagetable, uint64 start_va, uint64 sz) {
    uvmdealloc_thp_region_range(rblocks, pagetable, 0, sz);
    if(sz > 0)  uvmunmap(&(struct map_context){
        .pagetable=pagetable,
        .do_free=1,
        .pt_lock=NULL,
        .size=sz,
        .start_va=start_va,
    });
    else    uvmunmap(&(struct map_context){
        .pagetable=pagetable,
        .pt_lock=NULL,
        .do_free=0,
        .size=sz,
        .start_va=start_va
    });
}

// Given a parent process's page table, copy its memory into a child's page table.
// Copies both the page table and the physical memory.
// returns 0 on success, -1 on failure and also frees any allocated pages
//NOTE: make sure new_pg is clean, otherwise some memory corruption will occur.
int uvmcopy_range_private(struct vm_dupl_ctx *ctx1) {
#ifdef DEBUG_VM
    pr_err("old_pg=%p new_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->old_pg, (void *)ctx1->new_pg, ctx1->base_va, ctx1->end_va);
#endif
    if(ctx1==NULL || ctx1->new_rblocks==NULL || ctx1->old_rblocks==NULL 
        || ctx1->src_pg==NULL || ctx1->dst_pg==NULL){
        pr_err("Invalid paramter.");
        return -1;
    }
    uint64 ret_va=0;
    ctx1->dst_level=2;
    if(copywalk_private(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmfree_range(ctx1->new_rblocks, ctx1->dst_pg, ctx1->base_va ,ret_va-ctx1->base_va);
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_private failed.");
        return -1;
    }
    return 0;
}

int uvmcopy_range_shared(struct vm_dupl_ctx *ctx1) {
#ifdef DEBUG_VM
    pr_err("old_pg=%p new_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->old_pg, (void *)ctx1->new_pg, ctx1->base_va, ctx1->end_va);
#endif
    ctx1->dst_level=2;
    uint64 local_fallback_va=ctx1->base_va;
    if(ctx1->ret_va==NULL)
        ctx1->ret_va=&local_fallback_va;
    if(copywalk_shared(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmfree_range(ctx1->new_rblocks, ctx1->dst_pg, 
            ctx1->base_va , (uint64)ctx1->ret_va-ctx1->base_va);
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_shared failed.");
        return -1;
    }
    return 0;
}

int uvmcopy_range_cow(struct vm_dupl_ctx *ctx1) {
#ifdef DEBUG_VM
    pr_err("old_pg=%p new_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->old_pg, (void *)ctx1->new_pg, ctx1->base_va, ctx1->end_va);
#endif
    ctx1->dst_level=2;
    uint64 local_fallback_va=ctx1->base_va;
    if(ctx1->ret_va==NULL)
        ctx1->ret_va=&local_fallback_va;
    if(copywalk_shared_cow(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmfree_range(ctx1->new_rblocks, ctx1->dst_pg, 
            ctx1->base_va , (uint64)ctx1->ret_va-ctx1->base_va);
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_cow failed.");
        return -1;
    }
    return 0;
}

int uvmcopy_range_direct_shared(struct vm_dupl_ctx *ctx1){
    #ifdef DEBUG_VM
    pr_err("old_pg=%p new_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->old_pg, (void *)ctx1->new_pg, ctx1->base_va, ctx1->end_va);
#endif
    ctx1->dst_level=2;
    uint64 local_fallback_va=ctx1->base_va;
    if(ctx1->ret_va==NULL)
        ctx1->ret_va=&local_fallback_va;
    if(copywalk_shared_direct(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmfree_range(ctx1->new_rblocks, ctx1->dst_pg, 
            ctx1->base_va , (uint64)ctx1->ret_va-ctx1->base_va);
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_direct_shared failed.");
        return -1;
    }
    return 0;
} 

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
    pte_t *pte;

    pte = walk(pagetable, va, 0, 0);
    if (pte == NULL)
        panic("Imcomplete page table structure while clear 0x%llx", va);
    if(*pte==0)
        pr_err("The detected pte is empty.");
    *pte &= ~PTE_U;
    tlb_shootdown_issue_nolock(pagetable, va, PGSIZE);
}

//-----NOTE:the following three function are build upon kernel page, 
//          user pa is treated as kernel va.--------
#define PROACTIVE_CHECK

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

uint64 scan_contigous_map(pagetable_t pagetable, uint64 src_va){
    // Traverse the page table entries to determine the maximum length 
    // of physically contiguous memory mapped by the current PTE range
    int level=0, pl_pte_idx=0;
    pte_t *l_pte=NULL, *pl_pte=NULL;        //Loop pte, and prev Loop pte.
    uint64 cur_chunk_len=0, cur_va=src_va, offset_inpage, stride;
    pl_pte=walk_internal(pagetable, src_va, 0, 0, &level);
    if(pl_pte==NULL || (*pl_pte & PTE_V)==0 || PTE_LEAF(*pl_pte)==0)
        return 0;
    offset_inpage=cur_va & (get_step_size(level)-1);
    cur_chunk_len=(get_step_size(level)-offset_inpage);
    //Prepare the first pte.
    if(src_va % PGSIZE){
        pr_warn("Src_va isn't page-aligned, have aligned first.offset ingore");
        cur_va=PGROUNDDOWN(src_va);
    }
    while(1){       //start with one valid pte(pl_pte, and its length has added)
        //first calculate the stride, move to the next pte.Verfiy then.
        offset_inpage=cur_va & (get_step_size(level)-1);
        stride=(get_step_size(level)-offset_inpage);
        //update
        cur_va+=stride;
        pl_pte_idx=PX(level, cur_va);
        if(pl_pte_idx==0)       //Cross the pagetable, walk again.
            l_pte=walk_internal(pagetable, cur_va, 0, 0, &level);
        else    l_pte=pl_pte+1;     //In the same pagetable.
        if(l_pte==NULL || (*l_pte & PTE_V)==0 || PTE_LEAF(*l_pte)==0)
            return cur_chunk_len;
        if(PTE2PA(*l_pte)!=stride+PTE2PA(*pl_pte))
            return cur_chunk_len;
        if((*l_pte & CMP_PXMASK)!=(*pl_pte & CMP_PXMASK))
            return cur_chunk_len;
        cur_chunk_len+=stride;
        pl_pte=l_pte;
    }
    return cur_chunk_len;
}

//Used for process invalid pte met in vmfault.
int process_empty_pte(struct proc *cur_proc, uint64 basepage_va, uint64 cur_va, uint64 len){
    uint64 cur_order, block_size;
    void *mem;
    int perm;
    cur_order=i_log2(basepage_va & -basepage_va);
    cur_order=(cur_order>MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
    block_size=1ull<<cur_order;
    while(len < block_size && block_size>PGSIZE)
        block_size/=2;
    mem=alloc_memory(block_size);
    if(mem==0)  return -1;
    memset(mem, 0, block_size);
    int locked_by_me=0;     //Variable on stack(Process's private stack, visable for CPUs)
    //check if belong to file-backend and initiate a batch disk read request.
    if(cur_proc->mm==NULL)
        pr_warn("lack necessary mm_struct, can't get more info.Treat as anonymous VMA.");
    else{
        if(!holdingsleep(&cur_proc->mm->mm_lock)){
            acquiresleep(&cur_proc->mm->mm_lock);
            locked_by_me=1;
        }
        vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, cur_va);
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

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    //Proactive check, instead of raise page fault frequently.
    uint64 aligned_dstva=PGROUNDDOWN(dstva);
    uint8 cur_order=i_log2(aligned_dstva & -aligned_dstva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):cur_order;
    uint64 block_size, copy_size, cur_va=dstva, basepage_va=aligned_dstva, cur_pa;
    void *mem;
    int perm=0, locked_by_me=0;
    struct proc *cur_proc=myproc();
    pte_t *pte=NULL;
    while(len>0){
        if(basepage_va>=MAXVA)      goto error;
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(*pte==0){
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about this uncontroled region.");
                goto error;
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, cur_va);
            if(cur_vma==NULL){
                pr_err("%llx isn't recorded at current mm_struct.", cur_va);
                goto error;
            }
            else if((cur_vma->vm_page_prot & PTE_W)==0){
                pr_err("Corrputed file, unable to continue.");
                goto error;
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
            mem=alloc_memory(block_size);
            if(mem==0)  goto error;
            memset(mem, 0, block_size);
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_vma->vm_file!=NULL && vmfile_load(cur_vma, basepage_va, (uint64)mem, block_size)!=0){
                pr_err("Corrputed file, unable to continue.");
                free_pages(mem, block_size);
                goto error;
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
                goto error;
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
        else if ((*pte & PTE_U) == 0)    goto error;
        else if ((*pte & PTE_W)==0){        //Valid but Unwriteable.
            //get the VMA firstly.
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about this uncontroled region.");
                goto error;
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, cur_va);
            if(cur_vma==NULL){
                pr_err("%llx isn't recorded at current mm_struct.", cur_va);
                goto error;
            }
            if((*pte & PTE_COW) && (cur_vma->vm_flags & VM_WRITE) && (*pte & PTE_W)==0){
                uint64 old_pa=PTE2PA(*pte), src_flag=PTE_FLAGS(*pte);
                if(get_page_ref_count((void *)old_pa)!=1){
                    cur_pa=(uint64)alloc_memory(PGSIZE);
                    if(cur_pa==0){
                        pr_warn("COW:alloc memory fail.\n");
                        goto error;
                    }
                    memmove((void *)cur_pa, (void *)old_pa, PGSIZE);
                    *pte=PTE2PA(cur_pa) | (src_flag & ~PTE_COW) | PTE_W;    //Overwrite
                    free_pages((void *)old_pa, PGSIZE);
                }
                else{   //Current ref_count is already one, exclusively owned this page.
                    *pte = (*pte & ~PTE_COW) | PTE_W;
                    cur_pa=PTE2PA(*pte);
                }
            }
            else{
                pr_err("%llx exist valid pte, but it's not writable.", cur_va);
                goto error;
            }
            //COW.
        }
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
            //find the max contigious pte len instead of get from physical order
            // (A special case:COW may cause discontinuity)
            block_size=scan_contigous_map(pagetable, basepage_va);
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
        if(basepage_va>=MAXVA)      goto error;
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(*pte==0){
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about this uncontroled region.");
                goto error;
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, cur_va);
            if(cur_vma==NULL){
                pr_err("%llx isn't record at current mm_struct.", cur_va);
                goto error;
            }
            else if((cur_vma->vm_page_prot & PTE_R)==0){
                pr_err("%llx isn't readable.", cur_va);
                goto error;
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
            mem=alloc_memory(block_size);
            if(mem==0)  goto error;
            memset(mem, 0, block_size);
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_vma->vm_file!=NULL && vmfile_load(cur_vma, basepage_va, (uint64)mem, block_size)!=0){
                pr_err("Corrputed file, unable to continue.");
                free_pages(mem, block_size);
                goto error;
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
                goto error;
            }
            release(&cur_proc->uvm_lock);
            cur_pa=walkaddr(pagetable, basepage_va);    //update
        }
        else if ((*pte & PTE_V) == 0){   //Swap in fault.
            //extrace info(Swap_id) from the pte.
            //FIXME: 
            panic("swap pending implementation.");
        }
        else if ((*pte & PTE_U) == 0)    goto error;
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
            //find the max contigious pte len instead of get from physical order
            // (A special case:COW may cause discontinuity)
            block_size=scan_contigous_map(pagetable, basepage_va);
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
        if(basepage_va>=MAXVA)      goto error;
        int found_level=0;  //Consider hugeleaf
        pte = walk_internal(pagetable, basepage_va, 0, 0, &found_level);
        if(*pte==0){
            //check if belong to file-backend and initiate a batch disk read request.
            if(cur_proc->mm==NULL){
                pr_warn("lack necessary mm_struct, can't get more info about uncontroled region.");
                goto error;
            }
            if(!holdingsleep(&cur_proc->mm->mm_lock)){
                acquiresleep(&cur_proc->mm->mm_lock);
                locked_by_me=1;
            }
            vm_area_struct_t *cur_vma=find_vma(cur_proc->mm, srcva);
            if(cur_vma==NULL)       goto error;
            if(!(cur_vma->vm_page_prot & PTE_R)){
                pr_err("Invalid page prot, while trying to write to %llx", srcva);
                goto error;
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
            mem=(uint64)alloc_memory(block_size);
            if(mem==0)  goto error;
            memset((void *)mem, 0, block_size);
            if(cur_vma->vm_file!=NULL && vmfile_load(cur_vma, basepage_va, mem, block_size)!=0){
                //handle the file-backend seperately.
                pr_err("Corrputed file, unable to continue.");
                free_pages((void *)mem, block_size);
                goto error;
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
                goto error;
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
        else if ((*pte & PTE_U) == 0)    goto error;
        else{   //pte is valid now.
            cur_pa = PTE2PA(*pte);
            if(found_level!=0)  cur_pa+=(basepage_va % get_step_size(found_level));
            block_size=scan_contigous_map(pagetable, basepage_va);
        }
        if(pte==0 || (*pte & PTE_R)==0) goto error;
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
    if(locked_by_me==1){
        releasesleep(&cur_proc->mm->mm_lock);
        locked_by_me=0;
    }
    return -1;
}

// allocate and map user memory if process is referencing a page 
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if out of physical memory, 
// and return physical address if successful.
// NOTE: for argument "read" means Load page fault while 1, and means store error whne 0.
uint64 vmfault(res_block *rblocks, pagetable_t pagetable, uint64 va, int read_error) {
    struct proc *p = myproc();
    uint64 ret_pa=0;
    int locked_by_me=0;
    //Pass arugment rblocks etc explicitly instead of implicitly retriving them via myproc()
    if(p->mm==NULL){
        pr_err("Fatal error: proc's mm is NULL.\n");
        return 0;
    }
    acquiresleep(&p->mm->mm_lock);  //acquire sleeplock firstly
    va = PGROUNDDOWN(va);
    vm_area_struct_t *vma=find_vma_and_get(p->mm, va);
    releasesleep(&p->mm->mm_lock);
    if(vma==NULL){
        pr_err("Cannot find the corresponding vma.\n");
        goto release_and_ret;
    }
    if((read_error==1 && !(vma->vm_flags & VM_READ)) || (read_error==0 && !(vma->vm_flags & VM_WRITE))){
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
        if((read_error==0 && (*pte & PTE_W)) || read_error==1)
            goto release_and_ret;
        //Page Fault have already handled by other thread prior to this, just return.
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
                VM_TRACE("alloc new page fail.\n");
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
            ret_pa = (uint64)alloc_memory(PGSIZE);
            if (ret_pa == 0){
                VM_TRACE("alloc new page fail.\n");
                goto release_and_ret;
            }
            memset((void *)ret_pa, 0, PGSIZE);
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
                    VM_TRACE("Mappage %llx fail", va);
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
            uint64 old_pa=PTE2PA(*pte), src_flag=PTE_FLAGS(*pte);
            if(get_page_ref_count((void *)old_pa)!=1){
                ret_pa=(uint64)alloc_memory(PGSIZE);
                if(ret_pa==0){
                    pr_warn("COW:alloc memory fail.\n");
                    goto release_and_ret;
                }
                memmove((void *)ret_pa, (void *)old_pa, PGSIZE);
                *pte=PA2PTE(ret_pa) | (src_flag & ~PTE_COW) | PTE_W;    //Overwrite
                tlb_shootdown_issue_nolock_nolock(pagetable, va, PGSIZE);
                free_pages((void *)old_pa, PGSIZE);
            }
            else{   //Current ref_count is already one, exclusively owned this page.
                *pte = (*pte & ~PTE_COW) | PTE_W;
                tlb_shootdown_issue_nolock_nolock(pagetable, va, PGSIZE);
                ret_pa=PTE2PA(*pte);
            }
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


int merge_into_hugepages_Out(res_block *rblocks, pagetable_t pagetable, 
    uint64 va, uint64 alloced_pa, int expe_perm){
    //for out-of-place promoted.Expensive copy or move cost.
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE, cur_order;
    if(idx>=MAX_RES_BLOCK)  return -1;
    pte_t *parent_pte=walk(pagetable, va, 1, 1);
    pte_t *old_pte=walk(pagetable, rblocks[idx].va, 1, 0);
    if(parent_pte==NULL || old_pte==NULL)   return -1;
    //mapping trigger va using stale xperm(keep same processing flow)
    //Attribute Consistency Check and Accumulation of A/D bits
    uint16 check_perm_mask= PTE_W | PTE_R | PTE_X | PTE_U;
    uint64 accu_ad=0, bp_idx=0, delete_pa=PTE2PA(*parent_pte);
    if(delete_pa==0)    return -1;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
            uint64 flags=PTE_FLAGS(old_pte[bp_idx]);
            if(!(flags & PTE_V))    panic("Bitmap valid but hardware PTE invalid!");
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
    tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    //keep the correct sequence, avoid reusing the freed memory(which record in tlb)
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
    tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    free_pages((void *)delete_pa, PGSIZE);
    return 0;
}

int move_and_aggregate(res_block *rblocks, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    //Prepare the shadow copy, may trigger faults;not absolute safe.
    if(in_res_area(va, rblocks)!=1){
        panic("aggregate_data failed:invalid va");
        return -1;
    }
    uint64 rb_idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, rblocks[rb_idx].va, 0, 0);
    if(*old_pte==0)   return -1;
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
    uint64 bp_idx=0, tmp_flags, tmp_pa, step;
    if(mmu_free_batch_listcache==NULL){
        mmu_free_batch_listcache=create_slab_cache("mmu_free_batch_pool", 
            sizeof(struct mmu_free_batch_listnode), 8, NULL, NULL);
        if(mmu_free_batch_listcache==NULL)
            panic("unable create mmu_free_batch pool");
    }
    struct mmu_free_batch_listnode *head=NULL, *cur=NULL;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
            tmp_pa=PTE2PA(old_pte[bp_idx]);
            step=1ull<<(ORDER_BASE+get_order(tmp_pa));
            for(int j=0;j<step/PGSIZE;j++){
                tmp_flags=PTE_FLAGS(old_pte[bp_idx+j]);
                old_pte[bp_idx+j]=PA2PTE(alloced_pa+j*PGSIZE) | tmp_flags;
            }
            struct mmu_free_batch_listnode *new_node=slab_alloc(mmu_free_batch_listcache);
            if(new_node==NULL)      panic("");
            new_node->node.delete_pa=tmp_pa;
            new_node->node.len=step;
            new_node->next=head;
            head=new_node;      //insert.
            //Precise tracking of the virtual start address is unnecessary;
            //given the involvement of huge pages, a global flush is the most cost-effective.
        }
        else    step=PGSIZE;
        alloced_pa+=step;
        bp_idx+=step/PGSIZE;
    }
    if(head != NULL ){
        tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
        struct mmu_free_batch_listnode *next;
        while(head !=NULL){
            free_pages(head->node.delete_pa, head->node.len);
            next=head->next;
            slab_free((void *)head);
            head=next;
        }
        fixme: ?????????????
    }
}

void uvmdealloc_thp_region_range(res_block *rblocks, pagetable_t pagetable, 
        uint64 start_va, uint64 end_va){
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
            sfence_vma(0, 0);
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
uint64 uvmdealloc_thp_region(struct alloc_context *ctx1){
    //Eager Demotion!
    if(ctx1==NULL || ctx1->pagetable==NULL || ctx1->rblocks==NULL 
        || ctx1->seg_start <= ctx1->seg_end){
        pr_err("Invalid argument.");
        return 0;
    }
    if(ctx1->rblocks[0].va % SUPERPGSIZE !=0 )
        panic("free_res_memory:pass the unaligned address!");
    ctx1->seg_start=PGROUNDUP(ctx1->seg_start);
    ctx1->seg_end=PGROUNDUP(ctx1->seg_end);
    // uint64 align_oldsz=PGROUNDUP(oldsz), align_newsz=PGROUNDUP(newsz);
    uint64 req_size=ctx1->seg_start - ctx1->seg_end, start_va, end_va;
    //dealloc backward,maintain consistent handling logic
    if(ctx1->seg_end < ctx1->rblocks[0].va){
        if(ctx1->seg_start < ctx1->rblocks[0].va)
            return uvmdealloc(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .pt_lock=ctx1->pt_lock,
                .rblocks=ctx1->rblocks,
                .seg_start=ctx1->seg_start,
                .seg_end=ctx1->seg_end,
            });
        uint64 tmp_ret=uvmdealloc(&(struct alloc_context){
            .pagetable=ctx1->pagetable,
            .pt_lock=ctx1->pt_lock,
            .rblocks=ctx1->rblocks,
            .seg_start=ctx1->rblocks[0].va,
            .seg_end=ctx1->seg_end
        });
        if(tmp_ret!=ctx1->seg_end)    return 0;   //error
        req_size-=(ctx1->rblocks[0].va - ctx1->seg_end);
        ctx1->seg_end=ctx1->rblocks[0].va;
    }
    uint16 idx=(ctx1->seg_end - ctx1->rblocks[0].va)/SUPERPGSIZE;
    uint64 start_vpn, vpn_step;
    int split_dealloc;
    start_va=ctx1->seg_end;
    while(idx<MAX_RES_BLOCK && req_size>0){
        start_vpn=(start_va-ctx1->rblocks[idx].va)/PGSIZE;
        split_dealloc=(SUPERPGROUNDDOWN(start_va)==SUPERPGROUNDDOWN(ctx1->seg_start))?0:1;
        if(split_dealloc){
            end_va=(idx+1==MAX_RES_BLOCK)?
                (ctx1->rblocks[idx].va+SUPERPGSIZE):ctx1->rblocks[idx+1].va;
        }
        else    end_va=ctx1->seg_start;
        vpn_step=(end_va-start_va)/PGSIZE;
        if(ctx1->rblocks[idx].promoted==1){  //Demotion is merely a preliminary step for reclamation.
            //Split regradless of release size due to the lack of intermidate state.
            split_into_blocks(ctx1->rblocks, ctx1->pagetable, start_va, 1);  // Demotion for partial uvmunmap
            ctx1->rblocks[idx].promoted=0;
        }
        for(uint64 bp_idx=start_vpn;bp_idx<start_vpn+vpn_step;bp_idx++){
            if(ctx1->rblocks[idx].bitmap[bp_idx]!=0)
                ctx1->rblocks[idx].pop_count--;
        }
        uvmdealloc(&(struct alloc_context){
            .pagetable=ctx1->pagetable,
            .seg_start=end_va,
            .seg_end=start_va,
            .pt_lock=ctx1->pt_lock,
            .rblocks=ctx1->rblocks
        });     //Finalize the deallocation process.
        memset(ctx1->rblocks[idx].bitmap+start_vpn, 0, vpn_step);
        req_size-=(end_va-start_va);
        start_va=end_va;
        idx++;
    }
    return ctx1->seg_end;
}
#ifdef IN_PLACE_PROMOTE
uint64 uvmalloc_thp_region(struct alloc_context *ctx1){    //Consider reserved area version.
    ctx1->seg_start=PGROUNDDOWN(ctx1->seg_start);
    if(ctx1==NULL || ctx1->rblocks==NULL || ctx1->pagetable==NULL 
        || ctx1->seg_start>=ctx1->seg_end){
        pr_err("Invalid argument.");
        return 0;
    }
    int ret1=in_res_area(ctx1->seg_start, ctx1->rblocks);
    int ret2=in_res_area(ctx1->seg_end, ctx1->rblocks);
    uint64 res_start=0, res_end=0, non_res_start=0, non_res_end=0;
    uint64 block1, block1_size, ret_pa=0;//Record the first allocated block pa.
    if(ret1==-1 && ret2==-1){
        pr_err("Invalid argument.Unable to determine if currently within res_area.");
        return 0;
    }
    if(ret1==0 && ret2==1){
        //Splice into two ranges, [va, rblocks[0].va] and [rblocks[0].va, va+size]
        block1_size=ctx1->rblocks[0].va - ctx1->seg_start;
        block1=(uint64)alloc_memory(block1_size);
        if(block1==0)   return 0;
        memset((void *)block1, 0, block1_size);
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=ctx1->seg_start,
            .pt_lock=ctx1->pt_lock,
            .pa=block1,
            .size=block1_size,
            .xperm=ctx1->xperm
        }) != 0 ){
            free_pages((void *)block1, block1_size);
            return 0;
        }
        res_start=ctx1->rblocks[0].va;
        res_end=ctx1->seg_end;
        non_res_start=ctx1->seg_start;
        non_res_end=ctx1->rblocks[0].va;
        ret_pa=block1;
    }
    else if(ret1==1 && ret2==0){
        //Exist one range exceeds heap_res_area.mappage it firstly
        block1_size=ctx1->seg_end - ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE;
        block1=(uint64)alloc_memory(block1_size);
        if(block1==0)   return 0;
        memset((void *)block1, 0, block1_size);
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE,
            .pt_lock=ctx1->pt_lock,
            .pa=block1,
            .size=block1_size,
            .xperm=ctx1->xperm
        }) != 0 ){
            free_pages((void *)block1, block1_size);
            return 0;
        }
        res_start=ctx1->seg_start;
        res_end=ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE;
        non_res_start=ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE;
        non_res_end=ctx1->seg_end;
    }
    else if(ret1==1 && ret2==1){
        res_start=ctx1->seg_start;
        res_end=ctx1->seg_end;
    }
    else{   //ret1==0 && ret2==0, just treat as ordinary area and map.
        block1_size=ctx1->seg_end - ctx1->seg_start;
        block1=(uint64)alloc_memory(block1_size);
        if(block1==0)   return 0;
        memset((void *)block1, 0, block1_size);
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=ctx1->seg_start,
            .pt_lock=ctx1->pt_lock,
            .pa=block1,
            .size=block1_size,
            .xperm=ctx1->xperm
        }) != 0 ){
            free_pages((void *)block1, block1_size);
            return 0;
        }
        return block1;
    }
    uint16 idx=(res_start - ctx1->rblocks[0].va)/SUPERPGSIZE;
    uint64 cur_vpn, new_heap_block, cur_size, cur_va=res_start;
    while(cur_va < res_end){
        //handle one rblock each time.
        if(ctx1->rblocks[idx].promoted==1)  continue;   //Exist the leaf_pte already.(superpage)
        cur_vpn=(cur_va-ctx1->rblocks[idx].va)/PGSIZE;
        //enter reservable region, evaluate reservation strategy
        if(idx==MAX_RES_BLOCK-1)
            cur_size=MIN(res_end, ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE);
        else
            cur_size=MIN(res_end, ctx1->rblocks[idx+1].va);
        cur_size=cur_size-cur_va;
        if(ctx1->rblocks[idx].is_scattered==1){
            if(ctx1->rblocks[idx].alloc_attempts<MAX_ALLOWED_ALLOCATIONS){
                new_heap_block=(uint64)alloc_memory(SUPERPGSIZE);
                if(new_heap_block!=0){
                    memset((void *)new_heap_block, 0, SUPERPGSIZE);
                    // update the pte Only when new memory is ready 
                    // to avoid handling intermediate states.
                    if(mappages(&(struct map_context){
                        .pagetable=ctx1->pagetable,
                        .start_va=cur_va,
                        .size=cur_size,
                        .pa=new_heap_block+cur_vpn*PGSIZE,
                        .xperm=ctx1->xperm
                    }) !=0 ) {
                        free_pages((void *)new_heap_block, SUPERPGSIZE);
                        ctx1->rblocks[idx].alloc_attempts++;
                        goto discrete_alloc;
                    }
                    if(ctx1->rblocks[idx].pop_count>0){
                        if(move_and_aggregate(ctx1->rblocks, ctx1->pagetable, cur_va, new_heap_block)!=0){
                            ctx1->rblocks[idx].alloc_attempts++;
                            free_pages((void *)new_heap_block, SUPERPGSIZE);//rollback
                            pte_t *flight_pte=walk(ctx1->pagetable, cur_va, 0, 0);
                            if(flight_pte==NULL)    panic("map successfully, but detect invalid pte.");
                            memset(flight_pte, 0, sizeof(pte_t)*cur_size/PGSIZE);
                            goto discrete_alloc;
                        }
                        //update the mapping(safe), then move the data.
                        safe_update_and_free(ctx1->rblocks, ctx1->pagetable, cur_va, new_heap_block);
                    }
                    if(ret_pa==0)   ret_pa=new_heap_block+cur_vpn*PGSIZE;
                    //Synchronisz data
                    memset(&ctx1->rblocks[idx].bitmap[cur_vpn], 1, sizeof(uint8)*cur_size/PGSIZE);
                    ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);
                    ctx1->rblocks[idx].is_scattered=0;   //update
                    ctx1->rblocks[idx].pa=new_heap_block;
                    cur_va+=cur_size;
                    continue;
                }
                ctx1->rblocks[idx].alloc_attempts++;
            }
discrete_alloc:
            if(buddy_alloc(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .seg_start=cur_va,
                .seg_end=cur_va+cur_size,
                .rblocks=ctx1->rblocks,
                .xperm=ctx1->xperm,
                .pt_lock=ctx1->pt_lock
            }) !=0 ){
                pr_err("Handling discrete page alloction in res_area fail.");
                uvmdealloc_thp_region(&(struct alloc_context){
                    .pagetable=ctx1->pagetable,
                    .pt_lock=ctx1->pt_lock,
                    .seg_start=cur_va,
                    .seg_end=res_start,
                    .rblocks=ctx1->rblocks
                });
                if(non_res_end > non_res_start){
                    //In certain case, unmapping this region is not strictly required.
                    uvmdealloc(&(struct alloc_context){
                        .pagetable=ctx1->pagetable,
                        .seg_start=non_res_end,
                        .seg_end=non_res_start,
                        .pt_lock=ctx1->pt_lock,
                        .rblocks=ctx1->rblocks
                    });
                }
                return 0;
            }
            if(ret_pa==0){      //Perform a page table walk to resolve the pa.
                pte_t *pte=walk(ctx1->pagetable, cur_va, 0, 0);
                if(pte==NULL)   panic("Existing pte have cleared within critical area.");
                ret_pa=PTE2PA(*pte);
            }
            memset(&ctx1->rblocks[idx].bitmap[cur_vpn], 1, sizeof(uint8)*cur_size/PGSIZE);
            ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);
        }
        else{   // is_scrattered==0
            if(mappages(&(struct map_context){
                .pagetable=ctx1->pagetable,
                .start_va=cur_va,
                .size=cur_size,
                .pa=ctx1->rblocks[idx].pa+cur_vpn*PGSIZE,
                .xperm=ctx1->xperm
            }) !=0 ) {
                panic("Mapping to existing page fail!");
                uvmdealloc_thp_region(&(struct alloc_context){
                    .pagetable=ctx1->pagetable,
                    .pt_lock=ctx1->pt_lock,
                    .seg_start=cur_va,
                    .seg_end=res_start,
                    .rblocks=ctx1->rblocks
                });
                if(non_res_end > non_res_start){
                    //In certain case, unmapping this region is not strictly required.
                    uvmdealloc(&(struct alloc_context){
                        .pagetable=ctx1->pagetable,
                        .seg_start=non_res_end,
                        .seg_end=non_res_start,
                        .pt_lock=ctx1->pt_lock,
                        .rblocks=ctx1->rblocks
                    });
                }
                return 0;
            }
            ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);    //Synchronisz data
            if(THRESHLOD==0 || ctx1->rblocks[idx].pop_count>=THRESHLOD-1){
                //Supposing exist the huge_page already,Upgrade page table
                if(merge_into_hugepages_In(ctx1->rblocks, ctx1->pagetable, ctx1->seg_start)==0){
                    memset(&ctx1->rblocks[idx].bitmap[cur_vpn], 1, sizeof(uint8)*cur_size/PGSIZE);
                    ctx1->rblocks[idx].promoted=1;
                }
                else{
                    pr_warn("Exceed the threshold, but merge fail.");
                    //Not considered a mandatory rollback error;it simply means the benefits of 
                    // hugepage are no longer exist.
                }
            }
            if(ret_pa==0)   ret_pa=ctx1->rblocks[idx].pa+cur_vpn*PGSIZE;
        }
        cur_va+=cur_size;
    }
    if(!holding(ctx1->pt_lock))     sfence_vma(0, 0);
    return ret_pa;
}
#else
uint64 uvmalloc_thp_region(struct alloc_context *ctx1){    //Consider reserved area version.
    //out-of-place promotion, alloc memory in addition.
    ctx1->seg_start=PGROUNDDOWN(ctx1->seg_start);
    if(ctx1==NULL || ctx1->rblocks==NULL || ctx1->pagetable==NULL 
        || ctx1->seg_start>=ctx1->seg_end){
        pr_err("Invalid argument.");
        return 0;
    }
    int ret1=in_res_area(ctx1->seg_start, ctx1->rblocks);
    int ret2=in_res_area(ctx1->seg_end, ctx1->rblocks);
    uint64 res_start=0, res_end=0, non_res_start=0, non_res_end=0;
    uint64 block1, block1_size, ret_pa=0;//Record the first allocated block pa.
    if(ret1==-1 && ret2==-1){
        pr_err("Invalid argument.Unable to determine if currently within res_area.");
        return 0;
    }
    if(ret1==0 && ret2==1){
        //Splice into two ranges, [va, rblocks[0].va] and [rblocks[0].va, va+size]
        block1_size=ctx1->rblocks[0].va - ctx1->seg_start;
        block1=(uint64)alloc_memory(block1_size);
        if(block1==0)   return 0;
        memset((void *)block1, 0, block1_size);
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=ctx1->seg_start,
            .pt_lock=ctx1->pt_lock,
            .pa=block1,
            .size=block1_size,
            .xperm=ctx1->xperm
        }) != 0 ){
            free_pages((void *)block1, block1_size);
            return 0;
        }
        res_start=ctx1->rblocks[0].va;
        res_end=ctx1->seg_end;
        non_res_start=ctx1->seg_start;
        non_res_end=non_res_start + block1_size;
        ret_pa=block1;
    }
    else if(ret1==1 && ret2==0){
        //Exist one range exceeds heap_res_area.mappage it firstly
        block1_size=ctx1->seg_end - ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE;
        block1=(uint64)alloc_memory(block1_size);
        if(block1==0)   return 0;
        memset((void *)block1, 0, block1_size);
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE,
            .pt_lock=ctx1->pt_lock,
            .pa=block1,
            .size=block1_size,
            .xperm=ctx1->xperm
        }) != 0 ){
            free_pages((void *)block1, block1_size);
            return 0;
        }
        res_start=ctx1->seg_start;
        res_end=ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE;
        non_res_start=ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE;
        non_res_end=non_res_start + block1_size;
    }
    else if(ret1==1 && ret2==1){
        res_start=ctx1->seg_start;
        res_end=ctx1->seg_end;
    }
    else{   //ret1==0 && ret2==0, just treat as ordinary area and map.
        block1_size=ctx1->seg_end - ctx1->seg_start;
        block1=(uint64)alloc_memory(block1_size);
        if(block1==0)   return 0;
        memset((void *)block1, 0, block1_size);
        if(mappages(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .start_va=ctx1->seg_start,
            .pt_lock=ctx1->pt_lock,
            .pa=block1,
            .size=block1_size,
            .xperm=ctx1->xperm
        }) != 0 ){
            free_pages((void *)block1, block1_size);
            return 0;
        }
        return block1;
    }
    uint16 idx=(res_start - ctx1->rblocks[0].va)/SUPERPGSIZE;
    uint64 cur_vpn, new_heap_block, cur_size, cur_va=res_start;
    //Record the initial start as rollback termunation point.
    while(cur_va < res_end){
        //handle one rblock each time.
        cur_vpn=(cur_va-ctx1->rblocks[idx].va)/PGSIZE;
        //enter reservable region, evaluate reservation strategy
        if(idx==MAX_RES_BLOCK-1)
            cur_size=MIN(res_end, ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE);
        else
            cur_size=MIN(res_end, ctx1->rblocks[idx+1].va);
        cur_size=cur_size-cur_va;
        if(ctx1->rblocks[idx].promoted==1){
            cur_va+=cur_size;
            idx++;
            continue;   //Exist the leaf_pte already.
        }
        if(THRESHLOD==0 || ctx1->rblocks[idx].pop_count>=THRESHLOD-cur_size/PGSIZE){
            new_heap_block=(uint64)alloc_memory(SUPERPGSIZE);
            if(new_heap_block!=0){
                memset((void *)new_heap_block, 0, SUPERPGSIZE);
                if(merge_into_hugepages_Out(ctx1->rblocks, ctx1->pagetable, 
                        cur_va, new_heap_block, ctx1->xperm)==0){
                    ctx1->rblocks[idx].pa=new_heap_block;
                    ctx1->rblocks[idx].promoted=1;
                    ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);
                    memset(&ctx1->rblocks[idx].bitmap[cur_vpn], 1, sizeof(uint8)*cur_size/PGSIZE);
                    if(ret_pa==0)   ret_pa=new_heap_block+cur_vpn*PGSIZE;
                    cur_va+=cur_size;
                    idx++;
                    continue;
                }
                else{
                    free_pages((void *)new_heap_block, SUPERPGSIZE);
                    pr_warn("for out-of-place promotion, merge into new memory fail.");
                }
            }
        }
        if(buddy_alloc(&(struct alloc_context){
            .pagetable=ctx1->pagetable,
            .seg_start=cur_va,
            .seg_end=cur_va+cur_size,
            .rblocks=ctx1->rblocks,
            .xperm=ctx1->xperm,
            .pt_lock=ctx1->pt_lock}) !=0 ){
            pr_err("Handling discrete page alloction in res_area fail.");
            uvmdealloc_thp_region(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .seg_start=cur_va,
                .seg_end=res_start,
                .pt_lock=ctx1->pt_lock,
                .rblocks=ctx1->rblocks
            });
            if(non_res_end > non_res_start){
                //In certain case, unmapping this region is not strictly required.
                uvmdealloc(&(struct alloc_context){
                    .pagetable=ctx1->pagetable,
                    .seg_start=non_res_end,
                    .seg_end=non_res_start,
                    .pt_lock=ctx1->pt_lock,
                    .rblocks=ctx1->rblocks
                });
            }
            return 0;
        }
        if(ret_pa==0){      //Perform a page table walk to resolve the pa.
            pte_t *pte=walk(ctx1->pagetable, cur_va, 0, 0);
            if(pte==NULL)   panic("Existing pte have cleared within critical section.");
            ret_pa=PTE2PA(*pte);
        }
        memset(&ctx1->rblocks[idx].bitmap[cur_vpn], 1, sizeof(uint8)*cur_size/PGSIZE);
        ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);
        cur_va+=cur_size;
        idx++;
    }
    if(!holding(ctx1->pt_lock))     sfence_vma(0, 0);
    return ret_pa;
}
#endif