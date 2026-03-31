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
#include "vm.h"
#include "vm_internal.h"
#include "colors.h"
#include "utils.h"
#define INDENT_STR(lvl) ((lvl)==2 ? "" : ((lvl)==1 ? "  |-- " : "  |    |-- "))

struct spinlock rmap_lock;  //All operations(reading, writing ...) of the pte.
struct spinlock kheap_mem;  // protect one memory that designed only for kernel.
slab_cache_t *kheap_node_cache=NULL;
struct kheap_node *kheap_freelist_head=NULL;
/*
 * the kernel's page table. In xv6, all process share this pagetable while trap into
 * kernel, the switch from user's pagetable into this will clear all user mapping.
 * To get user's data, we should depend on software pagetable walk to retrive pa.
 * But for linux, each process's kernel pg is seperate. A combination of its mapping and
 * shared kernel mappging.
 */
pagetable_t kernel_pagetable;

//NOTE: Compliation precedes linking.the symbol table does not yet exist during complier.
// so the compiler can only parse code according to fixed, rigid rules.
//We must "deceive" the compiler or explicitly declare that it should not look up the value 
// at that location and then return the data to us.
// Difference is: la a0, _init_start ; the other: la a0, _init_start, lb a1, 0(a0)
// Two method to solve it: & ,or  using array name deacy into address automatically.
extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char _init_start;  // kernel.ld sets this to start of init.text. region
extern char _init_end; 

void *usyscall_pa=NULL;
extern char trampoline[];  // trampoline.S

//Involved lock: kvm_lock(protect kernel pagetable); mm->mm_lock(can be global_mm),
//protect the rb_tree + vma_list
//Sequence(rank): mm_lock/proc_lock, fs_lock, kvm_lock/pagetable_lock, kmem_lock/alloc_lock
struct spinlock kvm_lock;   //protect the kernel pagetable in multi-cpu

static inline uint8 in_kernel_heap(uint64 va){
    if(va>=KHEAP_START && va<KHEAP_END) return 1;
    else    return 0;
}
// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
__init_code void kvmmap_boot_only(struct map_context *ctx) {
    //Only this function can bypass the mappage kernel range limit
    // !!!!DON'T USE when not initialize kernel.
#ifdef DEBUG_KVM
    KVM_TRACE("Kpagetable=%p start_va=0x%llx pa=0x%llx sz=0x%llx perm=%d\n", 
        (void *)ctx->pagetable, ctx->start_va, ctx->pa, ctx->size, ctx->perm);
#endif
    //Current, only the local CPU holds the kernle page address, eliminating the need for locking.
    if(ctx->pt_lock==NULL)
        ctx->pt_lock=&kvm_lock;     //Equipped with existing lock.
    if (mappages(ctx) != 0) panic("kvmmap_init");
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
__init_code void kvminithart() {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    //NOTE:Use the global kernel_pagetbale instead of allocate one for each process.
    //Pros:Access user memory directly by mirroring page table entries.
    //        eliminating the overhead of address translation by copyin or copyout.
    //Cons:Memory overhead, Synchronization Complexity.Performance Hit on fork/exit.
    // wait for any previous writes to the page table memory to finish.
    acquire(&kvm_lock);
    sfence_vma(0, 0);

    w_satp(MAKE_SATP(kernel_pagetable));
    // flush stale entries from the TLB.
    sfence_vma(0, 0);
    release(&kvm_lock);
}

void free_initmem(){        //NOTE: Discard one-time boot routines. 
    uint64 start=(uint64)&_init_start;
    uint64 end=(uint64)&_init_end;
    if((start & (PGSIZE-1)) || (end & (PGSIZE-1))){//check aligned
        pr_warn("unmap some core function fail,address disalign(Don't reclaim).");
        return;
    }
    uint64 size=end-start;
    pr_info("Freeing unused kernel memory: %llu KB", (end-start)/1024);
    acquire(&kvm_lock);
    //A simple deallocator dedicated for destory some core and dangerous code mapping.
    //The previous mapping follow the logic of mappage() defined in vm.c
    //So we just reverse this progress.(And perform minimal checks)
    uint64 Simp_vmunmap(pagetable_t pagetable, uint64 va, uint64 size, int cur_level);
    Simp_vmunmap(kernel_pagetable, start, size, 2);    //scan from the highest level
    sfence_vma(0, 0);
    release(&kvm_lock);
}

// Make a direct-map page table for the kernel.
// Record the relavant info into global_mm
__init_code pagetable_t kvmmake(void) {
    //map kernel data and the physical RAM we'll use of.
    //NOTE: This create a direct mapping(Identity Map) for all physical RAM.When we later allocate pages
    //for Kernel_Heap, those page will be mapped again at a high virtual address.This create an
    //"Alias",it's international and safe because kalloc() ensures exclusive ownership.
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    pagetable_t kpgtbl;

    kpgtbl = (pagetable_t)kalloc_page();
    if(kpgtbl==0)   panic("kvmmake: alloc fail!");
    memset(kpgtbl, 0, PGSIZE);

    usyscall_pa=kalloc_page();
    if(usyscall_pa==0)  panic("kvmmake: alloc fail!");
    memset(usyscall_pa, 0, PGSIZE);
    //Allocated once during system initialization, not per-process;
    //thus, it is immune to memory leak.

    // ------------------IO device Mapping -----------------------
    // ACLINT SSWI(Supervisor Software Interrupt) register mapping
    // Required for S-mode to send IPIs across cores without SBI
    kvmmap_boot_only(&(struct map_context){
        .pagetable=kpgtbl,
        .start_va=SSWI_BASE,
        .pa=SSWI_BASE,
        .size=SSWI_SIZE,
        .xperm =PTE_W | PTE_R | PTE_IO
    });

    // uart registers
    kvmmap_boot_only(&(struct map_context){
        .pagetable = kpgtbl,
        .start_va = UART0,
        .pa = UART0,
        .size = PGSIZE,
        .xperm = PTE_R | PTE_W | PTE_IO
    });

    // virtio mmio disk interface
    kvmmap_boot_only(&(struct map_context){
        .pagetable = kpgtbl,
        .start_va = VIRTIO0,
        .pa = VIRTIO0,
        .size = PGSIZE,
        .xperm = PTE_R | PTE_W | PTE_IO
    });

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap_nolock(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W | PTE_IO);

  // pci.c maps the e1000's registers here.
  kvmmap_nolock(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W | PTE_IO);
#endif  

    // PLIC
    kvmmap_boot_only(&(struct map_context){
        .pagetable = kpgtbl,
        .start_va = PLIC,
        .pa = PLIC,
        .size = 0x4000000,
        .xperm = PTE_R | PTE_W | PTE_IO
    });

    //--------------------Normal RAM Mapping -----------------------
    // map kernel text executable and read-only.
    kvmmap_boot_only(&(struct map_context){
        .pagetable = kpgtbl,
        .start_va = KERNBASE,
        .pa = KERNBASE,
        .size = (uint64)etext - KERNBASE,
        .xperm = PTE_R | PTE_X
    });

    // map kernel data and the physical RAM we'll make use of.
    kvmmap_boot_only(&(struct map_context){
        .pagetable = kpgtbl,
        .start_va = (uint64)etext,
        .pa = (uint64)etext,
        .size = PHYSTOP - (uint64)etext,
        .xperm = PTE_R | PTE_W
    });

    //NOTE: Contigugous virtual range backed by non-contiguous physical memory via MMU.
    // (range: KRESERVED_START ~ KRESERVED_END)

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap_boot_only(&(struct map_context){
        .pagetable = kpgtbl,
        .start_va = TRAMPOLINE,
        .pa = (uint64)trampoline,
        .size = PGSIZE,
        .xperm = PTE_R | PTE_X
    });

    // allocate and map a kernel stack for each process.
    proc_mapstacks(kpgtbl);
    // reserve one area for kernel heap,support kernel alloc memory with any size.
    return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
// This function will be called by bootstraping,process has not yet been 
// associated with the concept of a CPU, so myproc() will raise fault. 
__init_code void kvminit(void) { 
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    init_mm();
    init_tlb_data_cache();
    init_mmu_free_batch_cache();
    init_mmu_gather_cache();
    init_kheap_node_cache();
    initlock(&kvm_lock, "kvm_pagetable lock");
    initlock(&kheap_mem, "kernel heap free memory lock");
    // initlock(&rmap_lock, )
    kernel_pagetable = kvmmake();
    // init timer payload lock and reserve one area in kernel pagetable.
    init_timer_payload_cache();
    //Init lock
}

uint64 scan_cont_extent(res_block *rblocks, uint64 start_va, uint64 end_va, int *type){
    //Find the end address of a contiguous extent of uniform type.
    if(rblocks==NULL || rblocks[0].va % SUPERPGSIZE!=0)
        return end_va;
    uint64 res_start=rblocks[0].va, res_end=rblocks[MAX_RES_BLOCK-1].va + SUPERPGSIZE;
    for(int i=0;i<MAX_RES_BLOCK;i++){
        if(rblocks[i].va !=res_start + SUPERPGSIZE * i){
            pr_warn("Corrpution rblocks structure(0x%llx in NO.%d blocks).",
                (uint64)rblocks, i);
            return end_va;
        }
    }
    //pre-check
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

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(struct alloc_context * actx){
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
    actx->xperm |= PTE_U;       //Defensive re-assignment
    //Partition into non-overlapping sub-blocks
    while(cur_start < actx->seg_end){
        cur_end=scan_cont_extent(actx->rblocks, cur_start, actx->seg_end, &cur_type);
        switch(cur_type){
            case ZONE_REGION:
                if(buddy_alloc_backend(&(struct alloc_context){
                    .pagetable=actx->pagetable,
                    .do_free=actx->do_free,
                    .pt_lock=actx->pt_lock,
                    .seg_start=cur_start,
                    .seg_end=cur_end,
                    .xperm=actx->xperm
                }, uvmdealloc, mappages)!=0)  
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
                    pr_err("alloc 0x%llx to 0x%llx fail(belong to reserved-region)", 
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

__init_code void init_kheap_node_cache(){
    kheap_node_cache=create_slab_cache("kheap_node_pool",
        sizeof(struct kheap_node), 8, NULL, NULL);
    if(kheap_node_cache==NULL)
        panic("Unable create kheap_node pool.");
    //Also initialize the list.
    if(kheap_freelist_head!=NULL)   panic("reinitialize kernel heap_list.");
    kheap_freelist_head=slab_alloc(kheap_node_cache);
    if(kheap_freelist_head==NULL)  panic("OOM while allocating kernel node.");
    memset(kheap_freelist_head, 0, sizeof(struct kheap_node));
    kheap_freelist_head->start_va=KHEAP_START;
    kheap_freelist_head->size = KHEAP_END - KHEAP_START;
}

static void remove_kheap_node(struct kheap_node *node){
    if(node==NULL){
        pr_err("NULL pointer passed, invalid argument.");
        return;
    }
    if(!holding(&kheap_mem))    panic("No lock operations.");
    if(node->prev!=NULL)     node->prev->next=node->next;
    else    kheap_freelist_head=node->next;  //remove the headnode.
    if(node->next!=NULL)     node->next->prev=node->prev;
    node->prev=NULL;
    node->next=NULL;
}

static void coalesce_kheap_node(struct kheap_node *node){
    if(node==NULL){
        pr_err("NULL pointer passed, invalid argument.");
        return;
    }
    if(!holding(&kheap_mem))    panic("No lock operations.");
    //Attempt to coalesce with adjacent node upon every insertion 
    // to mitigate memory fragmentation.(Always keep the fronter node)
    if(node->prev && node->prev->start_va + node->prev->size == node->start_va){
        //preceding coalesce
        node->prev->size += node->size;
        remove_kheap_node(node);
    }
    if(node->next && node->start_va + node->size == node->next->start_va){
        //succeeding coalesce
        node->size += node->next->size;
        remove_kheap_node(node->next);
    }
}

static void insert_kheap_node(struct kheap_node *node){
    if(!holding(&kheap_mem))    panic("No lock operations.");
    struct kheap_node *insert_pos=kheap_freelist_head;
    while(insert_pos!=NULL){
        if(insert_pos->next==NULL)  break;
        if(insert_pos->prev==NULL){
            if(node->start_va + node->size < insert_pos->start_va)  break;
            else    panic("Corrupted kernel heap freelist.");
        }
        else{
            if(node->size + node->start_va <=insert_pos->start_va &&
                node->prev->start_va + node->prev->size <= node->start_va)
                break;
        }
        insert_pos=insert_pos->next;
    }
    if(insert_pos!=NULL){
        if(insert_pos->prev!=NULL){
            insert_pos->prev->next=node;
            node->prev=insert_pos->prev;
        }
        else    kheap_freelist_head=node;
        insert_pos->prev=node;
        node->next=insert_pos;
    }
    else    kheap_freelist_head=node;
    coalesce_kheap_node(node);
}

__attribute__((warn_unused_result)) static uint64 find_suit_hole(uint64 req_sz){
    //return the available start_va within the designated range.(address ordering)
    if(kheap_freelist_head==NULL){
        pr_err("have not init kernel heap freelist.");
        return -1;
    }
    if(!holding(&kheap_mem)){
        pr_err("No lock operations.");
        return -1;
    }
    struct kheap_node *cur=kheap_freelist_head;
    uint64 ret_va=-1;
    while(cur!=NULL){
        if(cur->size >= req_sz)     break;
        cur=cur->next;
    }
    if(cur==NULL){
        pr_err("Unable find suitable hole in kernel heap freelist.");
        return -1;
    }
    ret_va=cur->start_va;
    if(cur->size == req_sz){
        slab_free((void *)cur);
        remove_kheap_node(cur);
    }
    else{   //Modify the found node.
        cur->size -= req_sz;
        cur->start_va += req_sz;
    }
    return ret_va;
}

//Lightweight, a simplifying version compared to user_vmalloc
uint64 kvmalloc(pagetable_t Kpagetable, uint64 req_sz, int xperm){
    if(req_sz==0){
        printf("kernel_vmalloc:Invalid size : 0\n");
        return 0;    //Prior the acquiration of lock,just return
    }
    req_sz=PGROUNDUP(req_sz);
    //Given fixed range:[KHEAP_START, KHEAP_END)
    acquire(&kheap_mem);
    uint64 start_va=find_suit_hole(req_sz);
    if(start_va==(uint64)-1){
        pr_info("Cannot find the avail_space!\n");
        release(&kheap_mem);
        goto error;
    }
    release(&kheap_mem);
    if(buddy_alloc_backend(&(struct alloc_context){
        .pagetable=Kpagetable,
        .pt_lock=&kvm_lock,     //Use the default pt_lock
        .seg_start=start_va,
        .seg_end=start_va + req_sz,
        .xperm=xperm,
    } , kvmdealloc, mappages) !=0 ){
        struct kheap_node *restore_node=(struct kheap_node *)slab_alloc(kheap_node_cache);
        if(restore_node==NULL){
            pr_err("Restore the allocated heap_freenode fail.Unable to create new_node.");
            goto error;
        }
        memset((void *)restore_node, 0, sizeof(struct kheap_node));
        restore_node->start_va=start_va;
        restore_node->size=req_sz;
        acquire(&kheap_mem);
        insert_kheap_node(restore_node);
        release(&kheap_mem);
        goto error;
    }
    return start_va;
error:
    pr_info("kvmalloc: fail!\n");
    return 0;
    //Address 0 is a unique error indicator because it's guaranteed to be invalid.
}

int buddy_alloc_backend(struct alloc_context *actx, uint64(*free_fn)(struct alloc_context *), 
                         int(*map_fn)(struct map_context*)){
    //return 0 when Success , and return -1 when it fail!
    //A shared utility for user and kernel space.
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
    int req_cnt=0, caller_was_holding=0;
    //before entering loop, release this lock firstly.
    if(actx->pt_lock!=NULL && holding(actx->pt_lock)){
        caller_was_holding=1;
        release(actx->pt_lock);
    }
    while(size>0){
        alloc_size=cur_max_size;
        while(size < alloc_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size, GFP_ZERO);
        if (mem == 0) {
            pr_info("uvmalloc failed to allocate cur_ctx1->seg_start=0x%llx size=0x%llx\n", 
                actx->seg_start, alloc_size);
            for(int i=0;i<req_cnt;i++)
                free_pages((void *)map_request[i].pa, map_request[i].size);
            if(actx->seg_start < cur_va)
                free_fn(&(struct alloc_context){
                    .pagetable=actx->pagetable,
                    .pt_lock=actx->pt_lock,
                    .seg_start=actx->seg_start,
                    .seg_end=cur_va,
                    .do_free=1
                });
            goto error_quit;
        }
        memset(mem, 0, alloc_size);
        map_request[req_cnt].pa=(uint64)mem;
        map_request[req_cnt].va=cur_va;
        map_request[req_cnt].size=alloc_size;
        map_request[req_cnt].xperm=actx->xperm;
        req_cnt++;
        if(req_cnt>=8){
            //Process accumulated mapping requests.(full batch, commit.)
            if(actx->pt_lock!=NULL){
                if(holding(actx->pt_lock))
                    pr_warn("Still holding lock after handling last mapping request.");
                else    acquire(actx->pt_lock);
            }
            for(int i=0;i<req_cnt;i++){
                if(map_fn(&(struct map_context){
                    .pagetable=actx->pagetable,
                    .start_va=map_request[i].va,
                    .pt_lock=actx->pt_lock,
                    .size=map_request[i].size,
                    .pa=map_request[i].pa,
                    .xperm=map_request[i].xperm
                })!=0){
                    if(actx->pt_lock!=NULL)
                        release(actx->pt_lock);
                    for(int j=i;j<req_cnt;j++)
                        free_pages((void *)map_request[j].pa, map_request[j].size);
                    free_fn(&(struct alloc_context){
                        .pagetable=actx->pagetable,
                        .pt_lock=actx->pt_lock,
                        .seg_start=actx->seg_start,
                        .seg_end=map_request[i].va,
                        .do_free=1      //Set explicitly
                    });
                    goto error_quit;
                }
            }
            req_cnt=0;
            memset(map_request, 0, sizeof(struct batch_map_entry));
            if(actx->pt_lock!=NULL) 
                release(actx->pt_lock);     //NOTE: Critical statement!
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
    if(req_cnt!=0){
        //process the remaining request
        if(actx->pt_lock!=NULL){
            if(holding(actx->pt_lock))
                pr_warn("Still holding lock after last mapping request handle.");
            else    acquire(actx->pt_lock);
        }
        for(int i=0;i<req_cnt;i++){
            if(map_fn(&(struct map_context){
                .pagetable=actx->pagetable,
                .pt_lock=actx->pt_lock,
                .start_va=map_request[i].va,
                .size=map_request[i].size,
                .pa=map_request[i].pa,
                .xperm=map_request[i].xperm
            })!=0){     //Map fail, rollback to the initial statement.
                if(actx->pt_lock!=NULL) 
                    release(actx->pt_lock);
                for(int j=i;j<req_cnt;j++)
                    free_pages((void *)map_request[j].pa, map_request[j].size);
                free_fn(&(struct alloc_context){
                    .pagetable=actx->pagetable,
                    .pt_lock=actx->pt_lock,
                    .seg_start=actx->seg_start,
                    .seg_end=map_request[i].va,
                    .do_free=1
                });
                goto error_quit;
            }
        }
        if(actx->pt_lock!=NULL)
            release(actx->pt_lock);
    }
    if(caller_was_holding)    acquire(actx->pt_lock);
    return 0;
error_quit:
    if(caller_was_holding)  acquire(actx->pt_lock);
    return -1;
}

uint64 uvmdealloc(struct alloc_context *actx){
    if(actx==NULL){
        pr_err("Pass in nullptr.");
        return 0;
    }
    if(actx->seg_start % PGSIZE !=0 || actx->seg_end % PGSIZE !=0 
        || actx->seg_start >= actx->seg_end){
            //The caller is responsible for ensuring the validity of the supplied address.
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
                vmunmap(&(struct map_context){
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
                }) !=cur_end){
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

//Allocate required size on kernel-heap, so the start-va depends on current memory usage.
uint64 kvmdealloc(struct alloc_context *actx){
    if(actx==NULL || actx->pagetable==NULL || actx->seg_start >=actx->seg_end
        || actx->seg_start % PGSIZE !=0 || actx->seg_end % PGSIZE !=0){
        pr_err("Invalid argument.");
        return 0;
    }
    if(!in_kernel_heap(actx->seg_start) || !in_kernel_heap(actx->seg_end -1)){
        pr_err("Invalid address range.");
        return 0;
    }
    if(actx->pt_lock==NULL){
        if(actx->pagetable == kernel_pagetable)     actx->pt_lock=&kvm_lock;
        else    panic("Conflict with default lock in the absence of an explicit one.");
    }
    vmunmap(&(struct map_context){
        .pagetable=actx->pagetable,
        .pt_lock=actx->pt_lock,
        .do_free=actx->do_free,
        .start_va=actx->seg_start,
        .size=actx->seg_end - actx->seg_start,
    });
    if(actx->do_free==0)    return actx->seg_end;       //Just unmap
    //if Free the physical resources, insert into kernel heap freelist.
    struct kheap_node *new_node=slab_alloc(kheap_node_cache);
    if(new_node==NULL){
        pr_err("OOM while creating kheap_node");
        return 0;
    }
    memset(new_node, 0, sizeof(struct kheap_node));
    new_node->start_va=actx->seg_start;
    new_node->size = actx->seg_end - actx->seg_start;
    acquire(&kheap_mem);
    insert_kheap_node(new_node);
    release(&kheap_mem);
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
