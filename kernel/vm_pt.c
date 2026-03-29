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
#include "slab.h"
#include "kalloc.h"
#include "mm.h"
#include "vm_internal.h"
#include "vm.h"
#include "colors.h"
#include "utils.h"


//To support multihart interrupt
//A streamlined struct mmu_gather for deterministic scenarios. 
// Recommended when a global TLB shootdown is certain and no range probing is required
slab_cache_t *mmu_free_batch_listcache=NULL;
slab_cache_t *mmu_gather_cache=NULL;

void init_mmu_free_batch_cache(void){
    if(mmu_free_batch_listcache==NULL){
        mmu_free_batch_listcache=create_slab_cache("mmu_free_batch_pool", 
            sizeof(struct mmu_free_batch_listnode), 8, NULL, NULL);
        if(mmu_free_batch_listcache==NULL)
            panic("unable create mmu_free_batch pool");
    }
}

void init_mmu_gather_cache(void){
    if(mmu_gather_cache==NULL){
        mmu_gather_cache=create_slab_cache("mmu_free_batch_pool", 
            sizeof(struct mmu_gather), 8, NULL, NULL);
        if(mmu_gather_cache==NULL)
            panic("unable create mmu_free_batch pool");
    }
}

//The relevant operations are as follows.
__attribute__((warn_unused_result))
struct mmu_free_batch_listnode *mmu_fbl_create(void){
    if(mmu_free_batch_listcache==NULL)     panic("have not init corresponding pool.");
    struct mmu_free_batch_listnode *new_node=
        (struct mmu_free_batch_listnode *)slab_alloc(mmu_free_batch_listcache);
    if(new_node==NULL)      panic("");
    new_node->count=0;
    new_node->next=NULL;
    memset(new_node->node, 0, sizeof(struct mmu_free_batch_entry)*8);
    return new_node;
}

__attribute__((warn_unused_result))
struct mmu_gather *mmu_gather_create(void){
    if(mmu_gather_cache==NULL)      panic("have not init corresponding pool.");
    struct mmu_gather *new_node=(struct mmu_gather *)slab_alloc(mmu_gather_cache);
    if(new_node==NULL)  panic("");
    memset(new_node, 0, sizeof(struct mmu_gather));
    return new_node;
}

void mmu_fbl_add_element(struct mmu_free_batch_listnode **head, uint64 pa, uint64 len){
    if(head==NULL || len==0 || pa==0)   panic("Invalid parameter.");
    if(*head==NULL)
        *head=mmu_fbl_create();     //The return value is guaranteed to be non-null.
    if((*head)->count >= 8){
        struct mmu_free_batch_listnode *tmp_node=mmu_fbl_create();
        tmp_node->next=*head;
        *head=tmp_node;
    }
    (*head)->node[(*head)->count].delete_pa=pa;
    (*head)->node[(*head)->count++].len=len;
}

void mmu_fbl_commit(pagetable_t pagetable, struct mmu_free_batch_listnode **head){
    if(head==NULL || pagetable==NULL)      panic("Invalid parameter.");
    if(*head!=NULL){
        tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
        struct mmu_free_batch_listnode *cur=*head, *next;
        while(cur !=NULL){
            for(int i=0;i<cur->count;i++)
                free_pages((void *)cur->node[i].delete_pa, cur->node[i].len);
            next=cur->next;
            slab_free((void *)cur);
            cur=next;
        }
    }
    *head=NULL;
}

void mmu_gather_reclaim(struct mmu_gather * mg1){
    if(mg1==NULL)   panic("NULL when reclaim resources.");
    slab_free((void *)mg1);
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
/*
 * Scan for physically contiguous memory across page tables.
 * * Strict Buddy System rule: 
 * Stops immediately if page size (level) changes across boundaries.
 *
 * @pagetable: Root page table.
 * @src_va:    Start virtual address.
 * @max:       Max bytes to scan (-1 for unlimited).
 * @return:    Total contiguous bytes.
 */
//Note: Use (uint64)-1 to represent an unbounded range.
// don't limit in the same pagtable, so input pagetable should be root_pg
uint64 cross_scan_cont_map(pagetable_t pagetable, uint64 src_va, uint64 max){
    // Traverse the page table entries to determine the maximum length 
    // of physically contiguous memory mapped by the current PTE range
    int old_level=0, new_level=0, pl_pte_idx=0;
    pte_t *l_pte=NULL, *pl_pte=NULL;        //Loop pte, and prev Loop pte.
    uint64 cur_chunk_len=0, cur_va=src_va, offset_inpage, stride;
    pl_pte=walk_internal(pagetable, src_va, 0, 0, &old_level);
    if(pl_pte==NULL || (*pl_pte & PTE_V)==0 || PTE_LEAF(*pl_pte)==0)
        return 0;
    offset_inpage=cur_va & (get_step_size(old_level)-1);
    cur_chunk_len=(get_step_size(old_level)-offset_inpage);
    //Prepare the first pte.
    if(src_va % PGSIZE){
        pr_warn("Src_va isn't page-aligned, have aligned first.");
        cur_va=PGROUNDDOWN(src_va);
    }
    while(cur_chunk_len < max){       //start with one valid pte(pl_pte, and its length has added)
        //first calculate the stride, move to the next pte.Verfiy then.
        offset_inpage=cur_va & (get_step_size(old_level)-1);
        stride=(get_step_size(old_level)-offset_inpage);
        //update
        cur_va+=stride;
        pl_pte_idx=PX(old_level, cur_va);
        if(pl_pte_idx==0){       //Cross the pagetable, walk again.
            l_pte=walk_internal(pagetable, cur_va, 0, 0, &new_level);
            if(old_level != new_level)  return cur_chunk_len;
        }
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

/*
 * Scan for physically contiguous memory within a SINGLE page table.
 * Fast path. Hard stops at the 512th entry to prevent out-of-bounds.
 *
 * @pagetable: Localized sub-level page table array.
 * @src_va:    Start virtual address.
 * @max:       Max bytes to scan.
 * @level:     Current PT level (0-2).
 * @return:    Total contiguous bytes.
 */
// Scanning within the immediate child table, input_pg is localized to the current sub_level.
uint64 same_scan_cont_map(pagetable_t pagetable, uint64 src_va, uint64 max, int level){
    int pl_pte_idx=PX(level, src_va);
    pte_t *l_pte=NULL, *pl_pte=(pte_t *)&pagetable[pl_pte_idx];
    uint64 cur_chunk_len=0;
    uint64 basic_step=get_step_size(level), stride;
    if(pagetable==NULL || max==0 || !(level >=0 && level <=2)){
        pr_err("Invalid parameter.");
        return 0;
    }
    cur_chunk_len = basic_step - (src_va & (basic_step -1));
    if(src_va % PGSIZE){
        pr_warn("Src_va isn't page-aligned, have aligned first.");
        src_va=PGROUNDDOWN(src_va);
    }
    while(cur_chunk_len < max){ //start with one valid pte(pl_pte, and its length has added)
        //first calculate the stride, move to the next pte.Verfiy then.
        stride=basic_step - (src_va & (basic_step - 1));
        //update
        src_va+=stride;
        pl_pte_idx++;
        if(pl_pte_idx==512) return cur_chunk_len;
        l_pte=pl_pte+1;
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
            // acquire(&rmap_lock);
            *pte = PA2PTE(pagetable) | PTE_V;
            //sync_rmap(pagetable, PGSIZE, pte);
            // release(&rmap_lock)
            //Don't set R/W/X, so this is directory entry,not a superpage
        }
    }
    if(found_level!=NULL)   *found_level=target_level;
    return &pagetable[PX(target_level, va)];
}
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, int target_level){
    return walk_internal(pagetable, va, alloc, target_level, NULL);//wrapper
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

// add a mapping to the kernel page table only used when booting.
// does not flush TLB or enable paging.
//NOTE: Paired with alloc(Buddy-system), it can be guarantee pa follows Buddy-system's prescribed size;
// When pa is suffiencently large and va meets the alignment requirements, we prefer mappage into larger pages.
int mappages(struct map_context *ctx1){
#ifdef DEBUG_VM
    pr_info("va=%p size=0x%llx pa=%p perm=0x%x\n", (void *)va, size, (void *)pa, perm);
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
        if(PTE_LEAF(*pte)!=0)   panic("Destoried pagetable.");
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

//Split the hugepage into smaller part(cur_level -1 ) and ignore the pte with range:[start_vpn, end_vpn]
//Usually followed by free_page, reclaim these unmapped pte.
//Not intended for stack usage, targeted at non-heap regions.
pagetable_t split_and_prune(pte_t pte, uint64 start_vpn, uint64 end_vpn, uint64 cur_level){
    if(PTE_LEAF(pte)==0)  panic("split into block: non-leaf!");
    if(!(cur_level >0 && cur_level < MAX_LEVEL))    panic("split into block:invalid level");
    if(end_vpn>512 || start_vpn>end_vpn)
        panic("split_and_prune:invalid vpn_range");
    //Align start and size to ensure alignment!
    uint64 cur_pa=PTE2PA(pte), basic_stride=get_step_size(cur_level-1);
    #ifdef DEBUG_VM
        pr_info("alloc new pagetable to replace leaf-pte\n");
    #endif
    pagetable_t new_pagetable=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
    if(new_pagetable==0){
        panic("out-of-memory");
    }
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

//NOTE: TLB is designated for MMU which can bypass lock to scan the pagetable directly.
// (Bus master: DMA, hardware prefetcher, GPU)
//Return the deleted size and caller will compare with expected value.
//The Whole process is unrecoverable.
uint64 vmunmap_helper(struct map_context *ctx1, struct mmu_gather *ctx2){
    //Unmap and then free physical resource(if necessary)
#ifdef DEBUG_VM
    pr_info("va=%p size=0x%llx do_free=%d, cur_level is %d\n", (void *)va, size, do_free, cur_level);
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
                //It's safe as current implementation doesn't invoke a free operations on these area.
                //pr_warn("Uvmunmap dangerous area(outside the RAM region)");
                pte[0]=0;
                //But check the unmap size is equal to its size.
                if(ctx1->size - del_size < basic_stride){
                    pr_err("Going to unmap region outside RAM partially.Strictly forbidded!");
                    goto exit;
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
                if(ctx1->cur_level<=0){
                    pr_err("Try to Split the minimun Unit");
                    goto exit;
                }
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
            vmunmap_helper(&(struct map_context){
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
exit:
    if(locked_by_me==1)
        release(ctx1->pt_lock);
    return del_size;
}

//Iterative veriosn, use less stack.
uint64 vmunmap_helper_iter(struct map_context *ctx1, struct mmu_gather *ctx2){
    //Unmap and then free physical resource(if necessary)
#ifdef DEBUG_VM
    pr_info("va=%p size=0x%llx do_free=%d, cur_level is %d\n", (void *)va, size, do_free, cur_level);
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
                pr_info("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
                pr_info("size=0x%llx, cur_level=%d, end_vpn=0x%llx\n", 
                    ctx1->size, ctx1->cur_level, end_vpn);
            #endif
            pr_err("uvmunmap_helper!");
            goto exit;
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
                    pr_info("Uvmunmap dangerous area(outside the RAM region)\n");
                    pte[0]=0;
                    //But check the unmap size is equal to its size.
                    if(ctx1->size - del_size < basic_stride){
                        pr_err("Going to unmap region outside RAM partially.Strictly forbidded!");
                        goto exit;
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
exit:
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

uint64 Simp_vmunmap(pagetable_t pagetable, uint64 va, uint64 size, int cur_level){
    //Dedicated to reclaim some area outside RAM kernel pagetable
    //Out of the buddy-system control, perform simple allocate logic without "order"
    uint64 basic_stride=get_step_size(cur_level);
    uint64 cur_vpn=PX(cur_level, va), start_page_offset=va & (basic_stride-1);
    pte_t *pte;
    uint64 end_vpn=cur_vpn+(start_page_offset+size+basic_stride-1)/basic_stride;    //exclude end_vpn
    if(end_vpn>512){
        #ifdef DEBUG_VM
            pr_info("Out-of-bounds deletion caused by the upper's failure to split the data:\n");
            pr_info("size=0x%llx, cur_level=%d, end_vpn=0x%llx\n", size, cur_level, end_vpn);
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
            Simp_vmunmap((pagetable_t)PTE2PA(*pte), va, pending, cur_level-1);
        }
        cur_vpn++;
        va+=pending;
        del_size+=pending;
    }
    sfence_vma(0, 0);
    return del_size;
}

void vmunmap(struct map_context *ctx1){
#ifdef DEBUG_VM
    pr_info("va=%p size=0x%llx do_free=%d\n", (void *)va, size, do_free);
#endif
    if(ctx1==NULL || ctx1->pagetable==NULL)
        panic("Invalid argument.");
    if(ctx1->size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (ctx1->size == 0)    panic("mappages: size");
    ctx1->cur_level=2;      //Start from highest level.
    struct mmu_gather *ctx2=mmu_gather_create();
    ctx2->batch_start_va=ctx1->start_va;
    ctx2->fullmm=0;
    ctx2->root_pg=ctx1->pagetable;
    uint64 del_size=vmunmap_helper(ctx1, ctx2);
    if(del_size!=ctx1->size)
        panic("uvmunmap: cannot unmap required size!");
    mmu_gather_reclaim(ctx2);
}

// Recursively free page-table pages totally 
// fixme: consider tlb_shootdown_issue_nolock,
void freewalk(pagetable_t pagetable, int do_free, int level, struct mmu_gather *ctx2) {
#ifdef DEBUG_VM
    if (level == 2)
        pr_info("Freewalk Start: pt=%p\n", pagetable);
#endif
    pte_t *pte;// there are 2^9 = 512 PTEs in a page table.
    uint64 pa;
    uint64 va_step, cur_order, basic_stride=get_step_size(level);
    int cur_vpn=0;
    while(cur_vpn<512){
        pte = &pagetable[cur_vpn];
        pa = PTE2PA(*pte);    //child pagetable or physical 
        if ((*pte & PTE_V) && PTE_LEAF(*pte) == 0) {
            // this PTE points to a lower-level page table.
            #ifdef DEBUG_VM
                pr_info("%sDir: idx=%d -> next_pa=%p\n", INDENT_STR(level), idx, (void*)pa);
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
            uint64 cmp_perm=(*pte & CMP_PXMASK);
            //Remove the total pagetable, so va is unnecessary.(Can't invoke "same_scan_cont_map")
            for(int k=0;k<512;k++){
                if(cur_vpn+k>=512 || cont_len >= max_phy_size)  break;
                tmp_pte=pagetable[k+cur_vpn];
                if(tmp_pte==0 || (tmp_pte & PTE_V)==0 || PTE_LEAF(tmp_pte)==0)
                    break;
                if(cmp_perm != (tmp_pte & CMP_PXMASK))    break;
                if(PTE2PA(tmp_pte) != pa + k*basic_stride)    break;
                cont_len+=basic_stride;
            }
            va_step=max_phy_size;
            while(va_step > cont_len && va_step > basic_stride)
                va_step /=2;
            for(int j=0;j<va_step / basic_stride;j++){
                if(cur_vpn+j==512){
                    panic("freewalk: alignment error");
                    break;
                }
                pagetable[cur_vpn+j]=0; //Clear the relevant PTEs
            }
            cur_vpn += va_step / basic_stride;
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
        pr_info("Freewalk Start: pt=%p\n", pagetable);
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
    uint64 cur_vpn;
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
                    pr_info("%sDir: idx=%d -> next_pa=%p\n", INDENT_STR(level), idx, (void*)pa);
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
                uint64 cmp_perm=(*pte & CMP_PXMASK);
                for(int k=0;k<512;k++){
                    if(cur_vpn+k>=512 || cont_len >= max_phy_size)  break;
                    tmp_pte=cur_pg[k+cur_vpn];
                    if(tmp_pte==0 || (tmp_pte & PTE_V)==0 || PTE_LEAF(tmp_pte)==0)
                        break;
                    if(cmp_perm != (tmp_pte & CMP_PXMASK))    break;
                    if(PTE2PA(tmp_pte) != pa + k*basic_stride)    break;
                    cont_len+=basic_stride;
                }
                va_step=max_phy_size;
                while(va_step > cont_len && va_step > basic_stride)
                    va_step /=2;
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
                for(int j=0;j<va_step/basic_stride;j++){
                    if(cur_vpn+j==512){
                        panic("freewalk: alignment error");
                        break;
                    }
                    cur_pg[cur_vpn+j]=0; //Clear the relevant PTEs
                }
                cur_vpn+=va_step/basic_stride;
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
    ctx2->dir_pa_batch[ctx2->dir_idx++]=(uint64)pagetable;
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


//No need to store intermidate data, 'Casue it was designed as NO-Fail(Commit Point)
//update pte and reclaim resouces at the same.
// (form the srattered physical page to hugepages, but pte still scattered.)
void safe_update_and_free(res_block *rblocks, pagetable_t pagetable, uint64 va, uint64 alloced_pa){
    uint64 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, rblocks[idx].va, 0, 0);
    uint64 bp_idx=0, tmp_flags, tmp_pa, step;
    if(mmu_free_batch_listcache==NULL)
        panic("have not init corresponding pool.");
    struct mmu_free_batch_listnode *head=NULL;
    while(bp_idx<512){
        if(rblocks[idx].bitmap[bp_idx]!=0){
            tmp_pa=PTE2PA(old_pte[bp_idx]);
            step=1ull<<(ORDER_BASE+get_order(tmp_pa));
            for(int j=0;j<step/PGSIZE;j++){
                tmp_flags=PTE_FLAGS(old_pte[bp_idx+j]);
                old_pte[bp_idx+j]=PA2PTE(alloced_pa+j*PGSIZE) | tmp_flags;
            }
            mmu_fbl_add_element(&head, tmp_pa, step);
            //Precise tracking of the virtual start address is unnecessary;
            //given the involvement of huge pages, a global flush is the most cost-effective.
        }
        else    step=PGSIZE;
        alloced_pa+=step;
        bp_idx+=step/PGSIZE;
    }
    mmu_fbl_commit(pagetable, &head);
}
