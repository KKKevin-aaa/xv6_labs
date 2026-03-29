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
#include "utils.h"
#include "colors.h"

void *alloc_memory_assert(uint64 size){
    if(size==0) panic("Invalid argument.");
    void *ret=alloc_memory(size, GFP_ZERO);
    if(ret==NULL)   panic("OOM");
    return ret;
}

//Input dst_pg is newly allocated, as lower-level of src_pg.
//Deal with partial copy in leaf pte.!!!
int copy_partial_leaf_private(struct vm_sub_copy_ctx *p1){   //Reuse ret_va as src
    if(p1==NULL || p1->src_pt==NULL || p1->dst_pt==NULL || p1->size==0){
        pr_err("NULL pointer passed, invalid argument.");
        return -1;
    }
    uint64 basic_stride=get_step_size(p1->dst_level), commit_len;
    int i= p1->base_va >> PXSHIFT(p1->dst_level) & PXMASK;
    uint64 src_flags=PTE_FLAGS(p1->src_pt[i]), src_pa;
    int src_locked_by_me=0, dst_locked_by_me=0, retval=0;   //Initially default to a normal exit.
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
            uint64 max_phy_size=1ull<<(get_order(src_pa) + ORDER_BASE);
            commit_len=max_phy_size;
            uint64 scan_len=same_scan_cont_map(p1->src_pt, p1->base_va, max_phy_size, p1->dst_level);
            while(commit_len > scan_len)
                commit_len /= 2;
            void *mem=alloc_memory(commit_len, 0);
            if(mem==NULL){
                pr_err("OOM.");
                retval=-1;
                goto exit;
            }
            memmove(mem, (void *)src_pa, commit_len);
            for(int j = 0; j < commit_len / basic_stride; j++){
                // Reuse flags from original entry
                uint64 inner_pa = (uint64)mem + j * basic_stride;
                p1->dst_pt[i+j] = PA2PTE(inner_pa) | src_flags;
            }
            i+=commit_len / basic_stride;
            p1->size-=commit_len;
            p1->base_va+=commit_len;
        }
        else{   //Partial/Split(p1->max_sz < basic_stride)
            if(p1->dst_level!=0){    //Current copy is insuffient for filling current level stride
                //Or it is an intermidate node.(alloc one pagetable and enter sub-level recursively.)
                pte_t *new_dst_pt=(pte_t *)alloc_memory(PGSIZE, GFP_ZERO);
                if(new_dst_pt==NULL){
                    pr_err("alloc failed,");
                    retval=-1;
                    goto exit;
                }
                p1->dst_pt[i]=PA2PTE((uint64)new_dst_pt) | PTE_V;   //As a directroy PTE
                retval=copy_partial_leaf_private(&(struct vm_sub_copy_ctx){
                    .base_va=p1->base_va,
                    .dst_pt=new_dst_pt,
                    .dst_level=p1->dst_level-1,
                    .size=p1->size,
                    .src_pt=p1->src_pt
                    //Current src_pte is leaf_pte, lack of lower level pagetable.
                }); //Deep copy
                goto exit;
            }
            else{
                //The lowest level, minimun size is PGSIZE.
                void *dst_mem=alloc_memory(PGSIZE, 0);
                memmove(dst_mem, (void *)src_pa, PGSIZE);
                p1->dst_pt[i]=PA2PTE((uint64)dst_mem) | src_flags;
                goto exit;
            }
        }
    }
exit:
    if(src_locked_by_me)
        release(p1->src_pt_lock);
    if(dst_locked_by_me)
        release(p1->dst_pt_lock);
    return retval;
}

//Absolutely deal with the leaf_node(shared memory),So all pte must be assigned with pte_COW.
// Targeted for regular(non-reserved) regions, metadata synchronization is omitted.
int copy_partial_leaf_shared(struct vm_sub_copy_ctx *p1){
    if(p1==NULL || p1->size==0 || p1->src_pt==NULL || p1->dst_pt==NULL){
        pr_err("NULL pointer passed, invalid argument.");
        return -1;
    }
    uint64 basic_stride=get_step_size(p1->dst_level);
    int i= p1->base_va >> PXSHIFT(p1->dst_level) & PXMASK, retval=0;
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
            pagetable_t src_child_pt=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
            if(src_child_pt==0){
                retval=-1;
                goto exit;
            }
            pagetable_t dst_child_pt=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
            if(dst_child_pt==0){
                retval=-1;
                goto exit;
            }
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
            if(p1->dst_level!=1){
                retval=copy_partial_leaf_shared(&(struct vm_sub_copy_ctx){
                    .base_va=p1->base_va,
                    .dst_pt=dst_child_pt,      //update pagetable to directory pte
                    .src_pt=src_child_pt,
                    .dst_level=p1->dst_level-1,
                    .size=p1->size,
                });  //Undertake the actual work.
                goto exit;
            }
            else{
                inc_ref_range((void *)(src_pa+start_pfn*PGSIZE), p1->size);
                goto exit;
            }
        }
        if(p1->size >= basic_stride){
            //Once the COW triggered, the default assumption of contiguous virtual and physical
            //addresses no longer holds, therefore, a scan is required.
            uint64 max_phy_size=1ull<<(get_order(src_pa) + ORDER_BASE);
            uint64 commit_len=max_phy_size;
            uint64 scan_len=same_scan_cont_map(p1->src_pt, p1->base_va, max_phy_size, p1->dst_level);
            while(commit_len > scan_len)
                commit_len/=2;
            for(int j = 0; j < commit_len / basic_stride; j++){
                // Reuse flags from original entry
                p1->src_pt[i+j] = (p1->src_pt[i+j] & ~PTE_W) | PTE_COW;
                p1->dst_pt[i+j] = p1->src_pt[i+j];
            }
            inc_ref_range((void *)src_pa, commit_len);
            i+=commit_len / basic_stride;
            p1->size-=commit_len;
            src_pa+=commit_len;
            p1->base_va+=commit_len;
        }
        else{
            //The lowest level, minimun size is PGSIZE.
            p1->src_pt[i]=(p1->src_pt[i] & ~PTE_W) | PTE_COW;
            p1->dst_pt[i]= p1->src_pt[i];
            inc_ref_range((void *)PTE2PA(p1->src_pt[i]), PGSIZE);
            goto exit;
        }
    }
exit:
    if(dst_locked_by_me)
        release(p1->dst_pt_lock);
    if(src_locked_by_me)
        release(p1->src_pt_lock);
    return retval;
}

int copy_heap_private(struct vm_dupl_ctx *v1){
    if(v1==NULL || v1->dst_pg==NULL || v1->src_pg==NULL
        || v1->new_rblocks==NULL || v1->old_rblocks==NULL){
        pr_err("NULL pointer passed, invalid argument.");
        return -1;
    }
    if(v1->dst_level!=1 || !in_res_area(v1->base_va, v1->new_rblocks)){
        panic("Interface Conflict");
        return -1;
    }
    uint64 cur_va=v1->base_va, rb_idx, src_pa;
    pte_t src_pte;
    int idx=0, retval=0;
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
        idx= cur_va >> PXSHIFT(v1->dst_level) & PXMASK;
        src_pte = v1->src_pg[idx];
        rb_idx=(cur_va-v1->new_rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
        uint64 bp_idx=(cur_va - v1->new_rblocks[0].va) % SUPERPGSIZE / PGSIZE;
        uint64 bp_step=MIN(512-bp_idx, (v1->end_va - cur_va) / PGSIZE);
        if(v1->new_rblocks[rb_idx].pa!=0){
            src_pa = PTE2PA(src_pte);    // child pagetable or physical address.
            //have allocated hugepage
            uint64 new_superpage=(uint64)alloc_memory(SUPERPGSIZE, 0);
            if(new_superpage==0){
                retval=-1;
                goto exit;
            }
            memmove((void *)new_superpage, (void *)(v1->new_rblocks[rb_idx].pa), SUPERPGSIZE);
            //migrate data
            uint64 cur_pa=new_superpage+PGSIZE*bp_idx, cur_flags;
            memset(v1->new_rblocks[rb_idx].bitmap, 0, bp_idx);
            memset(&v1->new_rblocks->bitmap[bp_idx+bp_step], 0, 512-(bp_idx+bp_step));
            if(v1->new_rblocks[rb_idx].promoted==0 || v1->base_va + SUPERPGSIZE > v1->end_va){
                uint64 mem=(uint64)alloc_memory(PGSIZE, GFP_ZERO);
                if(mem==0){
                    retval=-1;
                    goto exit;
                }
                pte_t *src_leaf_pte=(pte_t *)src_pa, *dst_leaf_pte=(pte_t *)mem;
                for(int j=0;j<bp_step;j++){
                    if(bp_idx+j>=512){
                        pr_err("Out-of-range.");
                        retval=-1;
                        goto exit;
                    }
                    if(v1->new_rblocks[rb_idx].promoted==0)
                        cur_flags=PTE_FLAGS(src_leaf_pte[bp_idx+j]);
                    else{   //already promoted into hugepage.
                        if(PTE_LEAF(src_pte)==0){
                            pr_err("Should be leaf.");
                            retval=-1;
                            goto exit;
                        }
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
                if(PTE_LEAF(src_pte)==0){
                    pr_err("Should be leaf.");
                    retval=-1;
                    goto exit;
                }
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
            if(PTE_LEAF(src_pte)==1){
                retval=-1;
                pr_err("Should not be leaf pte.");
                goto exit;
            }
            pagetable_t cur_src_pg= (pagetable_t)PTE2PA(v1->src_pg[idx]), cur_dst_pg=0;
            for(int i=0;i<bp_step;i++){
                if((cur_src_pg[i+bp_idx] & PTE_V) !=0){
                    if(cur_dst_pg==0){
                        cur_dst_pg=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
                        if(cur_dst_pg==0){
                            retval=-1;
                            pr_err("OOM.");
                            goto exit;
                        }
                        //hangle to the directory tree.
                        v1->dst_pg[idx]=PA2PTE(cur_dst_pg) | PTE_V;
                    }
                    cur_dst_pg[i+bp_idx]=cur_src_pg[i+bp_idx];
                }
            }
            cur_va+=bp_step*PGSIZE;
        }
    }
exit:
    *(uint64 *)(v1->ret_va)=cur_va;
    if(src_locked_by_me==1)
        release(v1->src_pt_lock);
    if(dst_locked_by_me==1)
        release(v1->dst_pt_lock);
    return retval;
}

//Designated for shared memory and non-COW case, 
// so we just copying PTE and increment reference count, return the 
int copy_heap_shared(struct vm_dupl_ctx *v1){
    if(v1==NULL || v1->dst_pg==NULL || v1->src_pg==NULL ||
        v1->old_rblocks==NULL || v1->old_rblocks==NULL){
        pr_err("NULL pointer passed, invalid argument.");
        return -1;
    }
    if(v1->dst_level!=1 || !in_res_area(v1->base_va, v1->new_rblocks)){
        pr_err("Interface Conflict");
        return -1;
    }
    //In this case, copying the whole valid heap reservation area.
    uint64 cur_va=v1->base_va, rb_idx;
    pte_t src_pte;
    uint64 src_pa;
    int idx=0, retval=0;
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
        idx= cur_va >> PXSHIFT(v1->dst_level) & PXMASK;
        src_pte = v1->src_pg[idx];
        rb_idx=(cur_va-v1->new_rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
        uint64 bp_idx=(cur_va - v1->new_rblocks[0].va) % SUPERPGSIZE / PGSIZE;
        uint64 bp_step=MIN(512-bp_idx, (v1->end_va - cur_va) / PGSIZE);
        if(v1->new_rblocks[rb_idx].pa!=0){
            src_pa = PTE2PA(src_pte);    // child pagetable or physical address.
            uint64 cur_pa=src_pa, sub_basic_stride=get_step_size(v1->dst_level-1);
            pagetable_t dst_new_dire=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
            if(dst_new_dire==0){
                retval=-1;
                goto exit;
            }
            pagetable_t src_dire=(pagetable_t)PTE2PA(v1->src_pg[idx]);
            for(int j=0;j<512;j++){
                //Both the parent and child processes have their corresponding PTEs
                //set to read-only and marked for COW.
                if(j>=bp_idx && j < bp_idx+bp_step){
                    if(src_dire[j]==0 || (src_dire[j] & PTE_V)==0)  continue;
                    dst_new_dire[j]=src_dire[j];
                    if((src_dire[j] & PTE_IO)==0)
                        inc_ref_range((void *)PTE2PA(src_dire[j]), PGSIZE);
                    else{
                        pr_err("Detect an io_mapped PTE.");
                        retval=-1;
                        goto exit;
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
                retval=-1;
                goto exit;
            }
            pagetable_t cur_src_pg= (pagetable_t)PTE2PA(v1->src_pg[idx]), cur_dst_pg=0;
            for(int i=0;i<bp_step;i++){
                if((cur_src_pg[i+bp_idx] & PTE_V) !=0){
                    if(cur_dst_pg==0){
                        cur_dst_pg=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
                        if(cur_dst_pg==0){
                            retval=-1;
                            pr_err("OOM.");
                            goto exit;
                        }
                        //hangle to the directory tree.
                        v1->dst_pg[idx]=PA2PTE(cur_dst_pg) | PTE_V;
                    }
                    cur_dst_pg[i+bp_idx]=cur_src_pg[i+bp_idx];
                    if((cur_dst_pg[i+idx] & PTE_IO)==0)
                        inc_ref_range((void *)PTE2PA(cur_dst_pg[i+bp_idx]), PGSIZE);
                    else{
                        pr_err("Detect an io_mapped PTE.");
                        retval=-1;
                        goto exit;
                    }
                }
            }
            cur_va+=bp_step*PGSIZE;
        }
    }
exit:
    *(uint64 *)(v1->ret_va)=cur_va;
    if(src_locked_by_me)
        release(v1->src_pt_lock);
    if(dst_locked_by_me)
        release(v1->dst_pt_lock);
    return retval;
}

int copy_heap_cow(struct vm_dupl_ctx *v1){
    if(v1==NULL || v1->dst_pg==NULL || v1->src_pg==NULL || v1->new_rblocks==NULL 
        || v1->old_rblocks==NULL){
        pr_err("NULL pointer passed, invalid argument.");
        return -1;
    }
    if(v1->dst_level!=1 || !in_res_area(v1->base_va, v1->new_rblocks)){
        pr_err("Interface Conflict");
        return -1;
    }
    uint64 cur_va=v1->base_va, rb_idx;
    pte_t src_pte;
    uint64 src_pa;
    int idx=0, src_locked_by_me=0, dst_locked_by_me=0, retval=0;
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
        idx= cur_va >> PXSHIFT(v1->dst_level) & PXMASK;
        src_pte = v1->src_pg[idx];
        rb_idx=(cur_va-v1->new_rblocks[0].va)/SUPERPGSIZE;   //Alignment satisfied
        uint64 bp_idx=(cur_va - v1->new_rblocks[0].va) % SUPERPGSIZE / PGSIZE;
        uint64 bp_step=MIN(512-bp_idx, (v1->end_va - cur_va) / PGSIZE);
        if(v1->new_rblocks[rb_idx].pa!=0){
            src_pa = PTE2PA(src_pte);    // child pagetable or physical address.
            uint64 cur_pa=src_pa, sub_basic_stride=get_step_size(v1->dst_level-1);
            //have allocated hugepage.
            if(!(src_pte & PTE_V) || (src_pte & PTE_IO)){
                pr_err("Detect an io_mapped PTE while dealing heap copy.");
                retval=-1;
                goto exit;
            }
            if(v1->new_rblocks[rb_idx].promoted==1){       //split it first.
                pagetable_t src_new_dire=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
                if(src_new_dire==0){
                    pr_err("OOM.");
                    retval=-1;
                    goto exit;
                }
                uint64 src_flags=PTE_FLAGS(src_pte);    //as leaf, flags is meaningful.
                for(int j=0;j<512;j++){
                    src_new_dire[j]=PA2PTE(cur_pa) | src_flags;
                    cur_pa+=PGSIZE;
                }
                //Mount the new directory pte to replace the previous leaf pte.
                v1->src_pg[idx]=PA2PTE(src_new_dire) | PTE_V;
                v1->old_rblocks[rb_idx].promoted=0;
            }
            pagetable_t dst_new_dire=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
            if(dst_new_dire==0){
                pr_err("OOM.");
                retval=-1;
                goto exit;
            }
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
                        retval=-1;
                        goto exit;
                    }
                }
                cur_pa+=sub_basic_stride;
            }
            v1->dst_pg[idx]=PA2PTE(dst_new_dire) | PTE_V;   //mount
            //update old_rblock's metadata.
            v1->new_rblocks[rb_idx]=v1->old_rblocks[rb_idx];
            memset(v1->new_rblocks[rb_idx].bitmap, 0, bp_idx);
            memset(&v1->new_rblocks->bitmap[bp_idx+bp_step], 0, 512-(bp_idx+bp_step));
            //update new_rblock's metadata.
        }
        else{   //Standard pte,however considering it's located within the heap region,
                //It still needs to be processed here accordingly.
            if(src_pte==0 || (src_pte & PTE_V) ==0){
                cur_va+=SUPERPGSIZE;
                continue;
            }
            if(PTE_LEAF(src_pte)==1){
                pr_err("Should not be leaf pte.");
                retval=-1;
                goto exit;
            }
            pagetable_t cur_src_pg= (pagetable_t)PTE2PA(v1->src_pg[idx]), cur_dst_pg=0;
            for(int i=0;i<bp_step;i++){
                if((cur_src_pg[i+bp_idx] & PTE_V) !=0){
                    if(cur_dst_pg==0){
                        cur_dst_pg=(pagetable_t)alloc_memory(PGSIZE, GFP_ZERO);
                        if(cur_dst_pg==0){
                            pr_err("OOM.");
                            retval=-1;
                            goto exit;
                        }
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
                        retval=-1;
                        goto exit;
                    }

                }
            }
        }
        cur_va+=bp_step*PGSIZE;

    }
exit:
    *(uint64 *)(v1->ret_va)=cur_va;
    if(src_locked_by_me)
        release(v1->src_pt_lock);
    if(dst_locked_by_me)
        release(v1->dst_pt_lock);
    return retval;
}

// Recursively copy page-table pages. Quit recursively When the superpage Error occurs, 
// Directly assign the known PTE to bypass the mappage overhead, significantly improving efficiency
int copywalk_private(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0, retval=0;
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
        if(heap_managed==0 && v1->dst_level==1 && in_res_area(cur_va, v1->new_rblocks)){
            heap_managed=1;
            uint64 new_ret_va=0;
            retval=copy_heap_private(&(struct vm_dupl_ctx){
                .base_va=cur_va,
                .end_va=v1->end_va,
                .dst_level=1,
                .dst_pg=v1->dst_pg,
                .src_pg=v1->src_pg, //Level-2 paegtable,able to get the correct sub_pg
                .ret_va=&new_ret_va,
                .new_rblocks=v1->new_rblocks,
                .old_rblocks=v1->old_rblocks,
            });
            if(retval!=0)   goto exit;
            cur_va = new_ret_va;
            idx=PX(v1->dst_level, cur_va);
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
                mem = alloc_memory(PGSIZE, GFP_ZERO);
                if(mem == NULL){
                    retval=-1;
                    goto exit;
                }
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->dst_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    retval=-1;
                    goto exit;
                }
            }
            if(copywalk_private(&(struct vm_dupl_ctx){
                    .src_pg=(pagetable_t)pa,
                    .dst_pg=(pagetable_t)mem,
                    .base_va=cur_va,
                    .ret_va=v1->ret_va,
                    .end_va=v1->end_va,
                    .dst_level=v1->dst_level-1,
                    .old_rblocks=v1->old_rblocks,
                    .new_rblocks=v1->new_rblocks
                })<0){
                retval=-1;
                goto exit;
            }
            va_step = standard_stride;
        } 
        else{   // Case 3: Leaf Node(and can be upgraded to hugepage,remember check!)
            uint64 remain_size=v1->end_va - cur_va;
            if(standard_stride > remain_size){
                if(copy_partial_leaf_private(&(struct vm_sub_copy_ctx)
                        {.dst_pt=(pte_t *)v1->dst_pg, 
                        .src_pt=(pte_t *)v1->src_pg, 
                        .base_va=cur_va, 
                        .size=remain_size, 
                        .dst_level=v1->dst_level
                    })!=0){
                    retval=-1;
                    goto exit;
                }
            }
            else{
                void *comp_leaf=alloc_memory(standard_stride, 0);
                if(comp_leaf==NULL)     panic("OOM.");
                memmove(comp_leaf, (void *)pa, standard_stride);
                v1->dst_pg[idx]=PA2PTE((uint64)comp_leaf) | PTE_FLAGS(v1->src_pg[idx]);
            }
            va_step=MIN(standard_stride, remain_size);
        }
        idx++;
        cur_va += va_step;
        *(uint64 *)v1->ret_va = cur_va;
    }
exit:
    if(src_locked_by_me)        release(v1->src_pt_lock);
    if(dst_locked_by_me)        release(v1->dst_pt_lock);
    return retval;
}

//Designed for direct mapping(shared), which just copying the pte from src pagetable
// And don't increment the reference count.
int copywalk_shared_direct(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0, retval=0;
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
            retval=-1;
            goto exit;
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
                mem = alloc_memory(PGSIZE, GFP_ZERO);
                if(mem == NULL){
                    retval=-1;
                    goto exit;
                }
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->dst_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    retval=-1;
                    goto exit;
                }
            }
            if(copywalk_shared_direct(&(struct vm_dupl_ctx){
                    .src_pg=(pagetable_t)pa,
                    .dst_pg=(pagetable_t)mem,
                    .base_va=cur_va,
                    .ret_va=v1->ret_va,
                    .end_va=v1->end_va,
                    .dst_level=v1->dst_level-1,
                    .old_rblocks=v1->old_rblocks,
                    .new_rblocks=v1->new_rblocks
                })<0){
                retval=-1;
                goto exit;
            }
            va_step = standard_stride;
        } 
        else{
            uint64 remain_size=v1->end_va - cur_va;
            if(standard_stride > remain_size){
                pr_err("Partial leaf copy, shouldn't exist.");
                retval=-1;
                goto exit;
            }
            else    v1->dst_pg[idx]= v1->src_pg[idx];
            va_step=MIN(standard_stride, remain_size);
        }
        idx++;
        cur_va += va_step;
        *(uint64 *)v1->ret_va = cur_va;
    }
exit:
    if(src_locked_by_me)        release(v1->src_pt_lock);
    if(dst_locked_by_me)        release(v1->dst_pt_lock);
    return retval;
}

//Designed for COW, which copy the pagetable verbatim without allocating
// additional physical memory.(shared physical page.)
// Return 0 on success, and return -1 on failure.
int copywalk_shared_cow(struct vm_dupl_ctx *v1){
    pte_t pte, new_pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_resolved=0;
    int src_locked_by_me=0, dst_locked_by_me=0, retval=0;
    //Manage lock acquistion and release exclusively at the top level
    // (Don't propagate this parameters in recursive calls.)
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
        if(heap_resolved==0 && v1->dst_level==1 
            && in_res_area(cur_va, v1->new_rblocks)==1){
            heap_resolved=1;
            uint64 new_ret_va=0;
            retval=copy_heap_cow(&(struct vm_dupl_ctx){
                .base_va=cur_va,
                .end_va=v1->end_va,
                .dst_level=1,
                .dst_pg=v1->dst_pg,
                .src_pg=v1->src_pg, //Level-2 paegtable,able to get the correct sub_pg
                .ret_va=&new_ret_va,
                .new_rblocks=v1->new_rblocks,
                .old_rblocks=v1->old_rblocks,
            });
            if(retval!=0)   goto exit;
            cur_va = new_ret_va;
            idx=PX(v1->dst_level, cur_va);
            *(uint64 *)v1->ret_va=cur_va;
            continue;
        }
        pte = v1->src_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if((pte & PTE_V)==0 && (pte & PTE_IO)){
            pr_err("detect an io_mapped pte.");
            retval=-1;
            goto exit;
        }
        if(pte==0 || (pte & PTE_V) ==0){
            cur_va += standard_stride;  //case 1: Invalid or don't exist pte at all.
            *(uint64 *)v1->ret_va = cur_va;
        }
        else if (PTE_LEAF(pte)==0) {    // Case 2: Directory
            new_pte=v1->dst_pg[idx];    // The corresponding pte of dst_pg.
            void *mem=NULL;
            if((new_pte & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE, GFP_ZERO);
                if(mem == NULL){
                    retval=-1;
                    goto exit;
                }
                new_pte = PA2PTE((uint64)mem) | flags | PTE_V;
                v1->dst_pg[idx] = new_pte;
            }
            else{
                mem=(char *)PTE2PA(new_pte);    //Extract the physical address from the pte.
                if(PTE_LEAF(new_pte)){ //PTE is valid, make sure it's a directory(not huge page.)
                    pr_err("copy to pagetable failed, already exist one, and dismatch with the dest.");
                    retval=-1;
                    goto exit;
                }
            }
            if(copywalk_shared_cow(&(struct vm_dupl_ctx){
                    .src_pg=(pagetable_t)pa,
                    .dst_pg=(pagetable_t)mem,
                    .base_va=cur_va,
                    .ret_va=v1->ret_va,
                    .end_va=v1->end_va,
                    .dst_level=v1->dst_level-1,
                    .new_rblocks=v1->new_rblocks,
                    .old_rblocks=v1->old_rblocks
                    })<0){
                retval=-1;
                goto exit;
            }
            cur_va = *(uint64 *)v1->ret_va;
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
                    })!=0){
                    retval=-1;
                    goto exit;
                }
            }
            else{
                v1->src_pg[idx]= (v1->src_pg[idx] & ~PTE_W) | PTE_COW;
                v1->dst_pg[idx]= v1->src_pg[idx];
                inc_ref_range((void *)PTE2PA(v1->src_pg[idx]), standard_stride);
            }
            cur_va += MIN(standard_stride, remain_size);
            *(uint64 *)v1->ret_va = cur_va;
        }
        idx++;
    }
exit:
    if(src_locked_by_me)        release(v1->src_pt_lock);
    if(dst_locked_by_me)        release(v1->dst_pt_lock);
    return retval;
}

//Designed for shared but non-COW, copying the original pte, incrementing the physical ref_count.
//Return 0 on success and -1 on error.
int copywalk_shared(struct vm_dupl_ctx *v1){
    pte_t pte;
    uint64 pa, standard_stride=get_step_size(v1->dst_level), cur_va=v1->base_va, va_step;
    uint64 flags;
    int idx= v1->base_va >> PXSHIFT(v1->dst_level) & PXMASK, heap_managed=0;
    int src_locked_by_me=0, dst_locked_by_me=0, retval=0;
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
        if(heap_managed==0 && v1->dst_level==1 && in_res_area(cur_va, v1->new_rblocks)){
            heap_managed=1;
            uint64 new_ret_va=0;
            retval=copy_heap_shared(&(struct vm_dupl_ctx){
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
            if(retval!=0)   goto exit;
            cur_va = new_ret_va;
            idx=PX(v1->dst_level, cur_va);
            *(uint64 *)v1->ret_va=cur_va;
            continue;
        }
        pte = v1->src_pg[idx];
        pa = PTE2PA(pte);    // child pagetable or physical address.
        flags = PTE_FLAGS(pte);
        if((pte & PTE_V) && (pte & PTE_IO)){
            pr_err("detect io_mapped pte.");
            retval=-1;
            goto exit;
        }
        if(pte==0 || (pte & PTE_V) ==0)
            va_step=standard_stride;    //case 1: Invalid or don't exist pte at all.
        else if (PTE_LEAF(pte)==0) {    // Case 2: Directory
            void *mem=NULL;
            if((v1->dst_pg[idx] & PTE_V)==0){       //Create only when if don't exist.
                mem = alloc_memory(PGSIZE, GFP_ZERO);
                if(mem == NULL){
                    retval=-1;
                    goto exit;
                }
                v1->dst_pg[idx] = PA2PTE((uint64)mem) | flags | PTE_V;
            }
            else{
                mem=(char *)PTE2PA(v1->dst_pg[idx]);
                //Extract the physical address from the pte.
            }
            if(copywalk_shared(&(struct vm_dupl_ctx){
                    .src_pg=(pagetable_t)pa,
                    .dst_pg=(pagetable_t)mem,
                    .base_va=cur_va,
                    .ret_va=v1->ret_va,
                    .end_va=v1->end_va,
                    .dst_level=v1->dst_level-1,
                    .new_rblocks=v1->new_rblocks
                })<0){
                retval=-1;
                goto exit;
            }
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
exit:
    if(src_locked_by_me)
        release(v1->src_pt_lock);
    if(dst_locked_by_me)
        release(v1->dst_pt_lock);
    return retval;
}

// Given a parent process's page table, copy its memory into a child's page table.
// Copies both the page table and the physical memory.
// returns 0 on success, -1 on failure and also frees any allocated pages
//NOTE: make sure dst_pg is clean, otherwise some memory corruption will occur.
int uvmcopy_range_private(struct vm_dupl_ctx *ctx1) {
#ifdef DEBUG_VM
    pr_err("src_pg=%p dst_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->src_pg, (void *)ctx1->dst_pg, ctx1->base_va, ctx1->end_va);
#endif
    if(ctx1==NULL || ctx1->new_rblocks==NULL || ctx1->old_rblocks==NULL 
        || ctx1->src_pg==NULL || ctx1->dst_pg==NULL){
        pr_err("Invalid paramter.");
        return -1;
    }
    uint64 local_fallback_va=0;
    ctx1->dst_level=2;
    //Once detecting a null pointer, initialize and bind a local variable to the new instance.
    if(ctx1->ret_va==NULL)  ctx1->ret_va=&local_fallback_va;
    if(copywalk_private(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmdealloc(&(struct alloc_context){
            .do_free=1,
            .pagetable=ctx1->dst_pg,
            .pt_lock=ctx1->dst_pt_lock,
            .rblocks=ctx1->new_rblocks,
            .seg_start=ctx1->base_va,
            .seg_end=local_fallback_va,
        });
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_private failed.");
        return -1;
    }
    return 0;
}

int uvmcopy_range_shared(struct vm_dupl_ctx *ctx1) {
#ifdef DEBUG_VM
    pr_err("src_pg=%p dst_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->src_pg, (void *)ctx1->dst_pg, ctx1->base_va, ctx1->end_va);
#endif
    ctx1->dst_level=2;
    uint64 local_fallback_va=ctx1->base_va;
    if(ctx1->ret_va==NULL)
        ctx1->ret_va=&local_fallback_va;
    if(copywalk_shared(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmdealloc(&(struct alloc_context){
            .do_free=1,
            .pagetable=ctx1->dst_pg,
            .pt_lock=ctx1->dst_pt_lock,
            .rblocks=ctx1->new_rblocks,
            .seg_start=ctx1->base_va,
            .seg_end=local_fallback_va
        });
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_shared failed.");
        return -1;
    }
    return 0;
}

int uvmcopy_range_cow(struct vm_dupl_ctx *ctx1) {
#ifdef DEBUG_VM
    pr_err("src_pg=%p dst_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->src_pg, (void *)ctx1->dst_pg, ctx1->base_va, ctx1->end_va);
#endif
    ctx1->dst_level=2;
    uint64 local_fallback_va=ctx1->base_va;
    if(ctx1->ret_va==NULL)
        ctx1->ret_va=&local_fallback_va;
    if(copywalk_shared_cow(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmdealloc(&(struct alloc_context){
            .do_free=1,
            .pagetable=ctx1->dst_pg,
            .pt_lock=ctx1->dst_pt_lock,
            .rblocks=ctx1->new_rblocks,
            .seg_start=ctx1->base_va,
            .seg_end=local_fallback_va
        });
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_cow failed.");
        return -1;
    }
    return 0;
}

int uvmcopy_range_direct_shared(struct vm_dupl_ctx *ctx1){
    #ifdef DEBUG_VM
    pr_err("src_pg=%p dst_pg=%p start=0x%llx, end=0x%llx\n", 
        (void *)ctx1->src_pg, (void *)ctx1->dst_pg, ctx1->base_va, ctx1->end_va);
#endif
    ctx1->dst_level=2;
    uint64 local_fallback_va=ctx1->base_va;
    if(ctx1->ret_va==NULL)
        ctx1->ret_va=&local_fallback_va;
    if(copywalk_shared_direct(ctx1)<0)
    {    //prevent resources leaks and waste
        uvmdealloc(&(struct alloc_context){
            .do_free=1,
            .pagetable=ctx1->dst_pg,
            .pt_lock=ctx1->dst_pt_lock,
            .rblocks=ctx1->new_rblocks,
            .seg_start=ctx1->base_va,
            .seg_end=local_fallback_va
        });
        //Error occurs, size are guaranteed not to exceed the original size
        pr_err("copywalk_direct_shared failed.");
        return -1;
    }
    return 0;
} 