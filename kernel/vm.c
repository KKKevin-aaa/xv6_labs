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
#include "vm_internal.h"
#include "colors.h"
#include "utils.h"
// #define DEBUG_VM
#define INDENT_STR(lvl) ((lvl)==2 ? "" : ((lvl)==1 ? "  |-- " : "  |    |-- "))

struct spinlock rmap_lock;  //All operations(reading, writing ...) of the pte.
struct spinlock only_kmem;  // protect one memory that designed only for kernel.

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 vmalloc(struct alloc_context * actx){
    if(actx==NULL){
        pr_err("Pass in nullptr.");
        return 0;
    }
    if(actx->seg_start % PGSIZE !=0 || actx->seg_end % PGSIZE !=0 
        || actx->seg_start >= actx->seg_end){
        pr_err("Invalid argument.");
        return 0;
    }
    uint64 cur_start=actx->seg_start, cur_end;
    int cur_type;
    //Partition into non-overlapping sub-blocks
    while(cur_start < actx->seg_end){
        cur_end=scan_cont_extent(actx->rblocks, cur_start, actx->seg_end, &cur_type);
        switch(cur_type){
            case ZONE_REGION:
                if(buddy_alloc_backend(actx, buddy_dealloc_backend, mappages)!=0)  
                    return 0;
                break;
            case RESERVED_REGION:
                if(uvmalloc_thp_region(&(struct alloc_context){
                    .pagetable=actx->pagetable,
                    .do_free=actx->do_free,
                    .pt_lock=actx->pt_lock,
                    .rblocks=actx->rblocks,
                    .seg_start=cur_start,
                    .seg_end=cur_end,
                }) ==0){
                    pr_err("dealloc 0x%llx to 0x%llx fail(belong to reserved-region)", 
                        cur_start, cur_end);
                    return cur_start;
                }
                break;
            default:
                pr_warn("0x%llo to 0x%llx belongs to Unknown region_type.", cur_start, cur_end);
                return cur_end;
        }
        cur_start=cur_end;
    }
    return actx->seg_end;
}

//Lightweight, a simplifying version compared to user_vmalloc
uint64 kvmalloc(pagetable_t Kpagetable, uint64 req_sz, int xperm){
    if(req_sz==0){
        printf("kernel_vmalloc:Invalid size : 0\n");
        return NULL;    //Prior the acquiration of lock,just return
    }
    req_sz=PGROUNDUP(req_sz);
    //Given fixed range:[KHEAP_START, KHEAP_END)
    acquire(&only_kmem);
    uint64 start_va=find_suit_hole();   //fixme: 
    if(start_va==(uint64)-1){
        printf("Cannot find the avail_space!\n");
        goto error;
    }
    void *ret=kvmalloc_range_locked(&(struct alloc_context){
        .pagetable = Kpagetable,
        .pt_lock = &kvm_lock,
        .seg_start = start_va,
        .seg_end = start_va + req_sz,
        .xperm = xperm
    }, &cont);
    if(ret!=(void *)start_va)   goto error;
    release(&kvm_lock); //must be held
    releasesleep(&global_mm->mm_lock);
    return (void *)start_va;
error:
    if(holding(&kvm_lock)) release(&kvm_lock);
    printf("kvmalloc: fail!\n");
    releasesleep(&global_mm->mm_lock);
    return NULL;
    //Address 0 is a unique error indicator because it's guaranteed to be invalid.
}

int buddy_alloc_backend(struct alloc_context *actx, uint64(*free_fn)(struct alloc_context *), 
                         int(*map_fn)(struct map_context*)){
    //return 0 when Success , and return -1 when it fail!
    //A shared utility for use and kernel space.
    if(actx==NULL || actx->seg_start >actx->seg_end){
        pr_err("Invalid parameter.");
        return -1;
    }
    if(actx->seg_end - actx->seg_start > 0x50 * PGSIZE){
        void *caller_addr=__builtin_return_address(0);
        pr_info("0x%llx allocate a massive contingous block of memory, totaling 0x%llx",
            (uint64)caller_addr, actx->seg_end - actx->seg_start);
    }
    if(actx->seg_start%PGSIZE!=0)    panic("Split_alloc: unaligned address!");
    uint8 cur_order=i_log2(actx->seg_start & -actx->seg_start);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)?
        (MAX_ORDER+ORDER_BASE):cur_order;
    uint64 cur_max_size=1ull<<cur_order ,alloc_size;
    uint64 cur_va=actx->seg_start, size=actx->seg_end-actx->seg_start;
    void *mem;
    struct batch_map_entry map_request[8];
    memset(map_request, 0, sizeof(struct batch_map_entry));
    int req_cnt=0, release_by_me=0;
    //before entering loop, release this lock firstly.
    if(holding(actx->pt_lock)){
        release_by_me=1;
        release(actx->pt_lock);
    }
    while(size>0){
        alloc_size=cur_max_size;
        while(size < alloc_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size);
        if (mem == 0) {
            pr_info("uvmalloc failed to allocate cur_ctx1->seg_start=0x%llx size=0x%llx\n", 
                actx->seg_start, alloc_size);
            for(int i=0;i<req_cnt;i++)
                free_pages((void *)map_request[i].pa, map_request[i].size);
            free_fn(&(struct alloc_context){
                .pagetable=actx->pagetable,
                .pt_lock=actx->pt_lock,
                .seg_start=cur_va,
                .seg_end=actx->seg_start,
            });
            return -1;
        }
        memset(mem, 0, alloc_size);
        map_request[req_cnt].pa=(uint64)mem;
        map_request[req_cnt].va=cur_va;
        map_request[req_cnt].size=alloc_size;
        map_request[req_cnt].xperm=actx->xperm;
        req_cnt++;
        if(req_cnt>=8){
            //Process accumulated mapping requests.(full batch, commit.)
            if(holding(actx->pt_lock))
                pr_warn("Still holding lock after last mapping request handle.");
            else    acquire(actx->pt_lock);
            for(int i=0;i<req_cnt;i++){
                if(map_fn(&(struct map_context){
                    .pagetable=actx->pagetable,
                    .start_va=map_request[i].va,
                    .size=map_request[i].size,
                    .pa=map_request[i].pa,
                    .xperm=map_request[i].xperm
                })!=0){
                    for(int j=i;j<req_cnt;j++)
                        free_pages((void *)map_request[j].pa, map_request[j].size);
                    free_fn(&(struct alloc_context){
                        .pagetable=actx->pagetable,
                        .pt_lock=actx->pt_lock,
                        .seg_start=cur_va,
                        .seg_end=actx->seg_start
                    });
                    return -1;
                }
            }
            req_cnt=0;
            memset(map_request, 0, sizeof(struct batch_map_entry));
            release(actx->pt_lock);
        }
        pr_info("alloc -> map cur_ctx1->seg_start=0x%llx alloc_size=0x%llx\n", 
            cur_va, alloc_size);
        if(size>alloc_size)  size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        cur_order=i_log2(cur_va & -cur_va);
        cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull << cur_order;
    }
    //process the remaining request
    if(holding(actx->pt_lock))
        pr_warn("Still holding lock after last mapping request handle.");
    else    acquire(actx->pt_lock);
    for(int i=0;i<req_cnt;i++){
        if(map_fn(&(struct map_context){
            .pagetable=actx->pagetable,
            .start_va=map_request[i].va,
            .size=map_request[i].size,
            .pa=map_request[i].pa,
            .xperm=map_request[i].xperm
        })!=0){
            for(int j=i;j<req_cnt;j++)
                free_pages((void *)map_request[j].pa, map_request[j].size);
            free_fn(&(struct alloc_context){
                .pagetable=actx->pagetable,
                .pt_lock=actx->pt_lock,
                .seg_start=map_request[i].va,
                .seg_end=actx->seg_start,
            });
            return -1;
        }
    }
    if(release_by_me==1)    acquire(actx->pt_lock);
    return 0;
}

uint64 scan_cont_extent(res_block *rblocks, uint64 start_va, uint64 end_va, int *type){
    //Find the end address of a contiguous extent of uniform type.
    uint64 res_start=rblocks[0].va;
    if(rblocks==NULL || res_start % SUPERPGSIZE!=0)
        return end_va;
    uint64 res_start=res_start, res_end=rblocks[MAX_RES_BLOCK-1].va + SUPERPGSIZE;
    for(int i=0;i<MAX_RES_BLOCK;i++){
        if(rblocks[i].va !=res_start + SUPERPGSIZE * i){
            pr_warn("Corrpution rblocks structure(0x%llx in NO.%d blocks).",
                (uint64)rblocks, i);
            return end_va;
        }
    }
    //pre-check
    uint64 res_end=rblocks[MAX_RES_BLOCK-1].va + SUPERPGSIZE;
    if(start_va < res_start){
        *type=ZONE_REGION;
        return (end_va < res_start) ? end_va : res_start;
    }
    else if(start_va < res_end){
        *type=RESERVED_REGION;
        return (end_va < res_end) ? end_va : res_end;
    }
    *type = ZONE_REGION;
    return end_va;
    //Only support one kind region, but logic can earily extended.
}

uint64 uvmdealloc(struct alloc_context *actx){
    if(actx==NULL){
        pr_err("Pass in nullptr.");
        return 0;
    }
    if(actx->seg_start % PGSIZE !=0 || actx->seg_end % PGSIZE !=0 
        || actx->seg_start >= actx->seg_end){
        pr_err("Invalid argument.");
        return 0;
    }
    uint64 cur_start=actx->seg_start, cur_end;
    int cur_type;
    //Partition into non-overlapping sub-blocks
    while(cur_start < actx->seg_end){
        cur_end=scan_cont_extent(actx->rblocks, cur_start, actx->seg_end, &cur_type);
        switch(cur_type){
            case ZONE_REGION:
                uvmunmap(&(struct map_context){
                    .pagetable = actx->pagetable,
                    .pt_lock = actx->pt_lock,
                    .start_va = cur_start,
                    .size = cur_end - cur_start,
                    .do_free = actx->do_free
                });
                break;
            case RESERVED_REGION:
                if(uvmdealloc_thp_region(&(struct alloc_context){
                    .pagetable=actx->pagetable,
                    .do_free=actx->do_free,
                    .pt_lock=actx->pt_lock,
                    .rblocks=actx->rblocks,
                    .seg_start=cur_start,
                    .seg_end=cur_end,
                }) ==0){
                    pr_err("dealloc 0x%llx to 0x%llx fail(belong to reserved-region)", 
                        cur_start, cur_end);
                    return cur_start;
                }
                break;
            default:
                pr_warn("0x%llo to 0x%llx belongs to Unknown region_type.", cur_start, cur_end);
                return cur_end;
        }
        cur_start=cur_end;
    }
    return actx->seg_end;
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

// #ifdef LAB_PGTBL
pte_t *pgpte(pagetable_t pagetable, uint64 va) { return walk(pagetable, va, 0, 0); }
// #endif

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc_page();
    if (pagetable == 0) return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

void uvmremove(pagetable_t pagetable){
    if(pagetable==NULL){
        pr_warn("Input an empty pagetable.");
        return;
    }
    struct mmu_gather *ctx1=mmu_gather_create();
    ctx1->root_pg=pagetable;
    freewalk(pagetable, 1, 2, ctx1);
    mmu_gather_reclaim(ctx1);
}
