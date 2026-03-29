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

// res_block rblocks[MAX_RES_BLOCK] __attribute__((unused)) ={0};  //static Global variable

int in_res_area(uint64 va, res_block *rblocks){
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

//Used in heap's hugepages only.'Casue calculate the idx from va directly.
void split_into_blocks(res_block * rblocks, pagetable_t pagetable, uint64 va, uint64 cur_level){
    if(rblocks==NULL || pagetable==NULL)
        panic("Invalid argument");
    uint16 idx=(va-rblocks[0].va)/SUPERPGSIZE;
    pte_t *old_pte=walk(pagetable, va, 0, cur_level);
    if(old_pte==NULL || *old_pte==0)
        panic("Incomplete or missing page table structure for the given address");
    pagetable_t new_pagetable=alloc_memory(PGSIZE, GFP_ZERO);
    if(new_pagetable==NULL) panic("Split-into-blocks:OOM");
    uint64 cur_pa=PTE2PA(*old_pte), basic_stride=get_step_size(cur_level-1), delete_pa=cur_pa;
    void *new_block_pa;
    if(new_pagetable==NULL){
        panic("Out-of-memory!");
    }
    for(int i=0;i<512;i++){
        if(rblocks[idx].bitmap[i]!=0){
            //Inherits RXW permissions from the original huge page, becoming a new small leaf.
            new_block_pa=alloc_memory(basic_stride, 0);
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
    tlb_shootdown_issue_nolock(pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    free_pages((void *)delete_pa, get_step_size(cur_level));
}


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

uint64 uvmdealloc_thp_region(struct alloc_context *ctx1){
    //Eager Demotion!
    ctx1->seg_start=PGROUNDDOWN(ctx1->seg_start);   //
    ctx1->seg_end=PGROUNDUP(ctx1->seg_end);
    uint64 req_size=ctx1->seg_end - ctx1->seg_start, cur_va, end_va;
    //dealloc backward,maintain consistent handling logic
    uint16 idx=(ctx1->seg_start - ctx1->rblocks[0].va)/SUPERPGSIZE;
    uint64 start_vpn, vpn_step;
    int split_dealloc;      //1 means need to split, 0 means dealloc the whole block.
    cur_va=ctx1->seg_start;
    while(idx<MAX_RES_BLOCK && req_size>0){
        start_vpn=(cur_va-ctx1->rblocks[idx].va)/PGSIZE;
        if(ctx1->seg_end - cur_va >=SUPERPGSIZE){
            end_va=(idx+1==MAX_RES_BLOCK)?
                (ctx1->rblocks[idx].va+SUPERPGSIZE):ctx1->rblocks[idx+1].va;
        }
        else    end_va=ctx1->seg_end;
        split_dealloc=((end_va - cur_va) < SUPERPGSIZE)?1:0;
        vpn_step=(end_va-cur_va)/PGSIZE;
        if(ctx1->rblocks[idx].promoted==1 && split_dealloc){
            //Demotion is merely a preliminary step for reclamation.
            //Split regradless of release size due to the lack of intermidate state.
            split_into_blocks(ctx1->rblocks, ctx1->pagetable, cur_va, 1);
            // Demotion for partial uvmunmap
        }
        if(ctx1->rblocks[idx].promoted==1)
            ctx1->rblocks[idx].promoted=0;
        for(uint64 bp_idx=start_vpn;bp_idx<start_vpn+vpn_step;bp_idx++){
            if(ctx1->rblocks[idx].bitmap[bp_idx]!=0)
                ctx1->rblocks[idx].pop_count--;
        }
        memset(ctx1->rblocks[idx].bitmap+start_vpn, 0, vpn_step);
        if(ctx1->rblocks[idx].is_scattered==0)
            ctx1->rblocks[idx].is_scattered=1;
        vmunmap(&(struct map_context){
            .pagetable=ctx1->pagetable,
            .do_free=ctx1->do_free,
            .pt_lock=ctx1->pt_lock,
            .start_va=cur_va,
            .size=end_va - cur_va,
        });   //Finalize the deallocation process.
        if(split_dealloc==0){
            ctx1->rblocks[idx].pa=0;
            ctx1->rblocks[idx].alloc_attempts=0;
        }
        req_size-=(end_va-cur_va);
        cur_va=end_va;
        idx++;
    }
    return ctx1->seg_end;
}
#ifndef IN_PLACE_PROMOTE
uint64 uvmalloc_thp_region(struct alloc_context *ctx1){    //Consider reserved area version.
    if(ctx1==NULL || ctx1->rblocks==NULL || ctx1->pagetable==NULL 
        || ctx1->seg_start>=ctx1->seg_end || ctx1->seg_start % PGSIZE!=0
        || ctx1->seg_end % PGSIZE !=0){
        pr_err("Invalid argument.");
        return 0;
    }
    int ret1=in_res_area(ctx1->seg_start, ctx1->rblocks);
    int ret2=in_res_area(ctx1->seg_end, ctx1->rblocks);
    //Record the first allocated block pa.
    if(ret1!=1 || ret2!=1)
        panic("Specific region does not meet THP requirements.");
    uint16 idx=(ctx1->seg_start - ctx1->rblocks[0].va)/SUPERPGSIZE;
    uint64 cur_vpn, new_heap_block, cur_size, cur_va=ctx1->seg_start;
    uint64 ret_pa=ctx1->rblocks[idx].pa;
    while(cur_va < ctx1->seg_end){
        //handle one rblock each time.
        if(ctx1->rblocks[idx].promoted==1){
            idx++;
            if(idx < MAX_RES_BLOCK)
                cur_va=ctx1->rblocks[idx].va;
            else    break;
            continue;   //Exist the leaf_pte already.(superpage)
        }
        cur_vpn=(cur_va-ctx1->rblocks[idx].va)/PGSIZE;
        //enter reservable region, evaluate reservation strategy
        if(idx==MAX_RES_BLOCK-1)
            cur_size=MIN(ctx1->seg_end, ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE);
        else
            cur_size=MIN(ctx1->seg_end, ctx1->rblocks[idx+1].va);
        cur_size=cur_size-cur_va;
        if(ctx1->rblocks[idx].is_scattered==1){
            if(ctx1->rblocks[idx].alloc_attempts<MAX_ALLOWED_ALLOCATIONS){
                new_heap_block=(uint64)alloc_memory(SUPERPGSIZE, GFP_ZERO);
                if(new_heap_block!=0){
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
                        // Transit the data into temporary buffer, then update mapping and free original pte.
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
                    //Synchronize data
                    memset(&ctx1->rblocks[idx].bitmap[cur_vpn], 1, sizeof(uint8)*cur_size/PGSIZE);
                    ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);
                    ctx1->rblocks[idx].is_scattered=0;   //update
                    ctx1->rblocks[idx].pa=new_heap_block;
                    if(THRESHLOD==0 || ctx1->rblocks[idx].pop_count >= THRESHLOD)
                        ctx1->rblocks[idx].promoted=1;
                    cur_va+=cur_size;
                    idx++;
                    continue;
                }
                ctx1->rblocks[idx].alloc_attempts++;
            }
discrete_alloc:
            if(buddy_alloc_backend(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .seg_start=cur_va,
                .seg_end=cur_va+cur_size,
                .rblocks=ctx1->rblocks,
                .xperm=ctx1->xperm,
                .pt_lock=ctx1->pt_lock
            }, uvmdealloc, mappages) !=0 ){
                pr_err("Handling discrete page alloction in res_area fail.");
                uvmdealloc_thp_region(&(struct alloc_context){
                    .pagetable=ctx1->pagetable,
                    .pt_lock=ctx1->pt_lock,
                    .seg_start=ctx1->seg_start,
                    .seg_end=cur_va,
                    .rblocks=ctx1->rblocks
                });
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
                    .seg_start=ctx1->seg_start,
                    .seg_end=cur_va,
                    .rblocks=ctx1->rblocks
                });
                return 0;
            }
            ctx1->rblocks[idx].pop_count+=(cur_size/PGSIZE);    //Synchronisz data
            if(THRESHLOD==0 || ctx1->rblocks[idx].pop_count>=THRESHLOD){
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
        idx++;
    }
    return ret_pa;
}
#else
uint64 uvmalloc_thp_region(struct alloc_context *ctx1){    //Consider reserved area version.
    //out-of-place promotion, alloc memory in addition.
    if(ctx1==NULL || ctx1->rblocks==NULL || ctx1->pagetable==NULL 
        || ctx1->seg_start>=ctx1->seg_end || ctx1->seg_start % PGSIZE!=0
        || ctx1->seg_end % PGSIZE !=0){
        pr_err("Invalid argument.");
        return 0;
    }
    int ret1=in_res_area(ctx1->seg_start, ctx1->rblocks);
    int ret2=in_res_area(ctx1->seg_end, ctx1->rblocks);
    uint64 ret_pa=0;
    //Record the first allocated block pa.
    if(ret1!=1 || ret2!=1)
        panic("Specific region does not meet THP requirements.");
    uint16 idx=(ctx1->seg_start - ctx1->rblocks[0].va)/SUPERPGSIZE;
    uint64 cur_vpn, new_heap_block, cur_size, cur_va=ctx1->seg_start;
    //Record the initial start as rollback termunation point.
    while(cur_va < ctx1->seg_end){
        //handle one rblock each time.
        cur_vpn=(cur_va-ctx1->rblocks[idx].va)/PGSIZE;
        //enter reservable region, evaluate reservation strategy
        if(idx==MAX_RES_BLOCK-1)
            cur_size=MIN(ctx1->seg_end, ctx1->rblocks[MAX_RES_BLOCK-1].va+SUPERPGSIZE);
        else
            cur_size=MIN(ctx1->seg_end, ctx1->rblocks[idx+1].va);
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
                    //treat as oridinary discrete pages.
                }
            }
        }
        if(buddy_alloc_backend(&(struct alloc_context){
            .pagetable=ctx1->pagetable,
            .seg_start=cur_va,
            .seg_end=cur_va+cur_size,
            .xperm=ctx1->xperm,
            .pt_lock=ctx1->pt_lock}, buddy_dealloc_backend, mappages) !=0 )
        {
            pr_err("Handling discrete page alloction in res_area fail.");
            uvmdealloc_thp_region(&(struct alloc_context){
                .pagetable=ctx1->pagetable,
                .seg_start=ctx1->seg_start,
                .seg_end=cur_va,
                .pt_lock=ctx1->pt_lock,
                .rblocks=ctx1->rblocks
            });
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
    return ret_pa;
}
#endif