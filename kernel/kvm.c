#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "rbtree.h"
#include "kvm.h"

#define DEBUG_KVM // 默认开启调试
#ifdef DEBUG_KVM
#define KVM_TRACE(fmt, ...) \
    do { \
        printf("[KVM:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define KVM_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;
extern char etext[];  // kernel.ld sets this to end of kernel code.
void *usyscall_pa=NULL;
extern char trampoline[];  // trampoline.S
//Maintain global state.
cpu_vma_pool_t my_cpu_vma_pool[NCPU];
mm_struct_t global_mm;
struct spinlock kvm_lock;   //protect the kernel pagetable in multi-cpu

uint64 gene_page_prot(uint64 vm_flags){
#ifdef DEBUG_KVM
    KVM_TRACE("vm_flags=%llu\n", vm_flags);
#endif
    //As a hardware-agnostic kernel sturcture, it implements the translation
    //from logical abstraction to physical hareware via the following functions.
    uint64 page_prot=0;
    if(vm_flags & PROT_READ)    page_prot |= PTE_R;
    if(vm_flags & PROT_EXEC)    page_prot |= PTE_X;
    if(vm_flags & PROT_WRITE)   page_prot |= PTE_W;
    if(vm_flags & PROT_USER)    page_prot |= PTE_U;
    return page_prot;
}
uint64 gene_flags(uint64 vm_page_prot){
#ifdef DEBUG_KVM
    KVM_TRACE("vm_page_prot=%llu\n", vm_page_prot);
#endif
    uint64 vm_flags=0;
    if(vm_page_prot & PTE_R)    vm_flags |= PROT_READ;
    if(vm_page_prot & PTE_X)    vm_flags |= PROT_EXEC;
    if(vm_page_prot & PTE_W)    vm_flags |= PROT_WRITE;
    if(vm_page_prot & PTE_U)    vm_flags |= PROT_USER;
    return vm_flags;
}
static inline uint8 in_kernel_heap(uint64 va){
#ifdef DEBUG_KVM
    KVM_TRACE("va=%llu\n", va);
#endif
    if(va>=KHEAP_START && va<KHEAP_END) return 1;
    else    return 0;
}
vm_area_struct_t *insert_vma_helper(uint64 va, uint64 sz, int perm){
#ifdef DEBUG_KVM
    KVM_TRACE("va=%llu sz=%llu perm=%d\n", va, sz, perm);
#endif
    vm_area_struct_t *tmp_vma=NULL;
    if(sizeof(vm_area_struct_t)>=PGSIZE){
        uint64 nr_pages=PGROUNDUP(sizeof(vm_area_struct_t));
        tmp_vma=(vm_area_struct_t *)alloc_memory(nr_pages*PGSIZE);
        memset((void *)tmp_vma, 0, nr_pages*PGSIZE);
    }
    else    tmp_vma=alloc_vma_node();
    if(tmp_vma==NULL)   return NULL;
    tmp_vma->vm_start=va;
    tmp_vma->vm_end=va+sz;
    tmp_vma->vm_page_prot=perm;
    tmp_vma->vm_flags=gene_flags(tmp_vma->vm_page_prot);
    tmp_vma->vm_mm=&global_mm;
    tmp_vma->ref_count=1;   //Holded by mm_struct
    //Omit values for unused arguments.
    insert_vma(&global_mm, tmp_vma);
    return tmp_vma;
}
//Pre-allocate static memory pool to bootstrap the Kernel virtual memory management.
void vma_pool_init(){
#ifdef DEBUG_KVM
    KVM_TRACE("vma_pool_init\n");
#endif
    for(int i=0;i<NCPU;i++){
        initlock(&my_cpu_vma_pool[i].pool_lock, "vm_struct_pool");
        my_cpu_vma_pool[i].partial=NULL;
        my_cpu_vma_pool[i].full=NULL;
        my_cpu_vma_pool[i].empty=NULL;
    }
}
void vma_get(vm_area_struct_t *vma){    //Grab
    if(vma==NULL)   return;
    __sync_fetch_and_add(&vma->ref_count, 1);
    #ifdef DEBUG_REF
        printf("VMA %p get:ref=%d\n", vma, vma.ref_count);
    #endif
}
int vma_put(vm_area_struct_t *vma){    //Release
    if(vma==NULL)   return;
    int new_ref=__sync_sub_and_fetch(&vma->ref_count, 1);
    #ifdef DEBUG_REF
        printf("VMA %p put:ref=%d\n", vma, new_ref);
    #endif
    if(new_ref==0){
        if(vma->vm_ops && vma->vm_ops->close)
            vma->vm_ops->close(vma);
        return reclaim_vma_node(vma);
    }
    else if(new_ref<0){
        panic("VMA Ref-count underflow!Double free deteched!");
    }
    return 0;
}


// Make a direct-map page table for the kernel.
// Record the relavant info into global_mm
pagetable_t kvmmake(void) {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
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
    kvmmap_init(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio mmio disk interface
    kvmmap_init(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

    // PLIC
    kvmmap_init(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    kvmmap_init(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
    global_mm.start_code=KERNBASE;
    global_mm.end_code=(uint64)etext;

    // map kernel data and the physical RAM we'll make use of.
    kvmmap_init(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);
    global_mm.start_data=(uint64)etext;
    global_mm.end_data=PHYSTOP;

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap_init(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // allocate and map a kernel stack for each process.
    proc_mapstacks(kpgtbl);
    // reserve one area for kernel heap,support kernel alloc memory with any size.
    global_mm.heap_vma=NULL;
    return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void) { 
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    initlock(&kvm_lock, "kvm_pagetable lock");
    initlock(&global_mm.mm_lock, "mm_lock");
    acquire(&global_mm.mm_lock);
    kernel_pagetable = kvmmake();
    //Init lock
    memset(&global_mm, 0, sizeof(global_mm));
    vma_pool_init();
    global_mm.rb_root=(rb_root_t *)alloc_vma_node();    //multiplex this slab_allocator(size unequal)
    global_mm.rb_root->rb_parent=NULL;
    insert_vma_helper(UART0, PGSIZE, PTE_R | PTE_W);
    insert_vma_helper(VIRTIO0, PGSIZE, PTE_R | PTE_W);
    insert_vma_helper(PLIC, 0x4000000, PTE_R | PTE_W);
    insert_vma_helper(KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);
    insert_vma_helper((uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);
    insert_vma_helper(TRAMPOLINE, PGSIZE, PTE_R | PTE_X);
    release(&global_mm.mm_lock);
}
// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap_init(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
#ifdef DEBUG_KVM
    KVM_TRACE("kpgtbl=%p va=%llu pa=%llu sz=%llu perm=%d\n", (void *)kpgtbl, va, pa, sz, perm);
#endif
    //Current, only the local CPU holds the kernle page address, eliminating the need for locking.
    if (mappages(kpgtbl, va, sz, pa, perm) != 0) panic("kvmmap_init");
}
int kvmmap_safe(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm){
    //Since there is only a single global kernel pagetable, we default to using unique lock.
    //And it can be extended to multi kernel pagetable, so use different lock at that time.
    if(!holding(&kvm_lock))
        acquire(&kvm_lock);
    int ret=mappages(kpgtbl, va, sz, pa, perm);
    if (ret!= 0) panic("kvmmap_safe");
    release(&kvm_lock);
    return ret;
}
// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart() {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    //NOTE:Use the global kernel_pagetbale instead of allocate one for each process.
    //Pros:Access user memory directly by mirroring page table entries.
    //        eliminating the overhead of address translation by copyin or copyout.
    //Cons:Memory overhead, Synchronization Complexity.Performance Hit on fork/exit.
    // wait for any previous writes to the page table memory to finish.
    acquire(&kvm_lock);
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));
    // flush stale entries from the TLB.
    sfence_vma();
    release(&kvm_lock);
}
//external function from my_project:nemu
//A reduced set of page table functions suffices, as kernel mapping are shared.
static int relink_locked(page_slab_header_t *slab, 
                page_slab_header_t **old_list, page_slab_header_t **new_list){
#ifdef DEBUG_KVM
    KVM_TRACE("slab=%p old_list=%p new_list=%p\n", 
        (void *)slab, (void *)old_list, (void *)new_list);
#endif
    if(old_list==NULL || *old_list==NULL || new_list==NULL ||
           old_list==new_list || slab==NULL){
        printf("relink: Unexpected agrument!, slab=%p, new_list=%p, old_list=%p\n",
            slab, new_list, old_list);
        return -1;
    }
    //Invariant Association, lock id stored in object and will not change in object lifecycle
    int slab_id=slab->cpu_id, old_list_id=(*old_list)->cpu_id, new_list_id=(*new_list)->cpu_id;
    if(slab_id !=old_list_id || old_list_id != new_list_id){
        printf("relink: operator different cpu's list,Dangerous\n");
        return -1;
    }
    if(!holding(&my_cpu_vma_pool[slab_id].pool_lock)){
        panic("Race Conditions: Accessing list without lock!\n");
    }
    uint64 cnt1=(*old_list)->inuse_count, cnt2=slab->inuse_count;
    if(!((cnt1==cnt2) || (cnt1>0 && cnt2>0 && cnt1<VMA_SLAB_LIMIT && cnt2<VMA_SLAB_LIMIT))){
        printf("remove an unexisted entry from dismatch list\n");
        return -1;
    }
    page_slab_header_t *prev=slab->prev_page;
    page_slab_header_t *next=slab->next_page;
    if(prev!=NULL)  prev->next_page=next;
    else    *old_list=NULL; //old_list now is empty
    if(next!=NULL)  next->prev_page=prev;
    //hang up to the new_list
    slab->next_page=*new_list;
    slab->prev_page=NULL;
    if(*new_list!=NULL) (*new_list)->prev_page=slab;
    *new_list=slab;
    return 0;
}
static int add_to_list_locked(page_slab_header_t *slab, page_slab_header_t **list){
#ifdef DEBUG_KVM
    KVM_TRACE("slab=%p list=%p\n", (void *)slab, (void *)list);
#endif
    if(slab==NULL || list==NULL){
        printf("add_to_list:Unexpected argument!\n");
        return -1;
    }
    int slab_id=slab->cpu_id, list_id=(*list)->cpu_id;
    if(slab_id!=list_id){
        printf("add_to_list: operator different cpu's list.Dangerous\n");
        return -1;
    }
    if(!holding(&my_cpu_vma_pool[slab_id].pool_lock)){  //Lock Onwership Verification
        panic("Race Conditions: Accessing list without lock!\n");
    }
    slab->next_page=*list;
    slab->prev_page=NULL;
    if(*list!=NULL) (*list)->prev_page=slab;
    *list=slab;
    return 0;
}
vm_area_struct_t *alloc_vma_node(){
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    int id=cpuid();
    if(!holding(&global_mm.mm_lock))
        panic("[Alloc_vma_node]Race Conditions: access mm_struct without lock\n");
    acquire(&my_cpu_vma_pool[id].pool_lock);
    page_slab_header_t *tmp=my_cpu_vma_pool[id].partial;//Extract space from partial list
    printf("current No.%d cpu try to alloc_vma_node!\n", id);
    while(tmp!=NULL){
        if(tmp->inuse_count==VMA_SLAB_LIMIT){
            tmp=tmp->next_page;
        }
        else{
            if(tmp->inuse_count==VMA_SLAB_LIMIT-1){
                //transit into the full_list(Defer the inuse_count update while in critical state.)
                if(tmp->freelist_head==NULL || tmp->freelist_head->vm_next!=NULL)
                    panic("alloc_vma_node: slab_header metadata inconsisent!\n");
                #ifdef DEBUG_KVM
                    KVM_TRACE("now relink page(%p) from partial_list to full_list\n", tmp);
                #endif
                if(relink_locked(tmp, &my_cpu_vma_pool[id].partial, &my_cpu_vma_pool[id].full)==-1)
                    goto error;
            }
            vm_area_struct_t *ret_vma=tmp->freelist_head;
            tmp->freelist_head=tmp->freelist_head->next_free;
            memset(ret_vma, 0, sizeof(vm_area_struct_t));   //clear again
            tmp->inuse_count++; //update at the end.
            release(&my_cpu_vma_pool[id].pool_lock);
            return ret_vma;
        }
    }
    //the current pool is empty, considering alloc new page    
    void *new_pool_mem=alloc_memory(PGSIZE);
    if(new_pool_mem==NULL){
        printf("alloc_vma_node: OOM!\n");
        goto error;
    }
    memset(new_pool_mem, 0, PGSIZE);
    //Embed the management structure at the start of the page.
    page_slab_header_t *new_header=(page_slab_header_t *)new_pool_mem;
    new_header->magic=PAGE_SLAB_HEADER_MAGIC;
    new_header->inuse_count=1;
    new_header->cpu_id=id;  //Explicitly states "executing" to avoid ambiguity.
    vm_area_struct_t *cur=(vm_area_struct_t *)((uint64)new_pool_mem+sizeof(page_slab_header_t));
    new_header->freelist_head=cur;
    for(int i=0;i<VMA_SLAB_LIMIT-1;i++){
        cur->next_free=cur+1;
        cur->ref_count=1;
        cur++;
    }
    cur->ref_count=1;
    cur->next_free=NULL;    //end
    add_to_list_locked(new_header, &my_cpu_vma_pool[id].partial);
    vm_area_struct_t *ret=new_header->freelist_head;
    new_header->freelist_head=new_header->freelist_head->next_free;
    memset(ret, 0, sizeof(vm_area_struct_t));   //clear again
    release(&my_cpu_vma_pool[id].pool_lock);
    return ret;
error:
    printf("alloc_vma_node : fail!\n");
    release(&my_cpu_vma_pool[id].pool_lock);
    return NULL;
}
int reclaim_vma_node(vm_area_struct_t *node){
#ifdef DEBUG_KVM
    KVM_TRACE("node=%p\n", (void *)node);
#endif
    if(node==NULL)  return -1;
    page_slab_header_t *cur_page=(page_slab_header_t *)((uint64)node & ~(PGSIZE -1));
    if(cur_page->magic!=PAGE_SLAB_HEADER_MAGIC){
        printf("Reclaim an invalid vma_node, that allocator recognized\n");
        goto cleanup;
    }
    int id=cur_page->cpu_id;
    acquire(&my_cpu_vma_pool[id].pool_lock);
    if(cur_page->inuse_count==VMA_SLAB_LIMIT){
        #ifdef DEBUG_KVM
            KVM_TRACE("now relink page(%p) from full_list to partial_list\n", cur_page);
        #endif
        if(relink_locked(cur_page, &my_cpu_vma_pool[id].full, &my_cpu_vma_pool[id].partial)==-1)
            goto cleanup;
        memset(node, 0, sizeof(vm_area_struct_t));
        cur_page->inuse_count-=1;
        node->next_free=cur_page->freelist_head;
        cur_page->freelist_head=node;
    }
    else if(cur_page->inuse_count==1){
        #ifdef DEBUG_KVM
            KVM_TRACE("now relink page(%p) from partial_list to empty_list\n", cur_page);
        #endif
        if(relink_locked(cur_page, &my_cpu_vma_pool[id].partial, &my_cpu_vma_pool[id].empty)==-1)
            goto cleanup;
        cur_page->inuse_count=0;
        memset(node, 0, sizeof(vm_area_struct_t));
        node->next_free=cur_page->freelist_head;
        cur_page->freelist_head=node;
        uint64 count=0;
        page_slab_header_t *tmp=my_cpu_vma_pool[id].empty;
        while(tmp!=NULL){
            count++;
            if(count>=VMA_POOL_LIMIT)   break;
            tmp=tmp->next_page;
        }
        if(count>=VMA_POOL_LIMIT){  //shrink the pool if breaches the limit!
            #ifdef DEBUG_KVM
                KVM_TRACE("reclaim_vma_node: shrink the pool!\n");
            #endif
            tmp=my_cpu_vma_pool[id].empty;
            page_slab_header_t *next=tmp;
            count=count/2;
            while(count>0){
                if(tmp==NULL){
                    printf("reclaim_vma_node: statement dismatch!");
                    goto cleanup;
                }
                next=tmp->next_page;
                memset((void *)tmp, 0, PGSIZE);
                free_pages(tmp, PGSIZE);
                count--;
                tmp=next;
            }
            my_cpu_vma_pool[id].empty=tmp;
            if(tmp) tmp->prev_page=NULL;
        }
    }
    else{   //No list transition.
        cur_page->inuse_count-=1;
        memset(node, 0, sizeof(vm_area_struct_t));
        node->next_free=cur_page->freelist_head;
        cur_page->freelist_head=node;
    }
    release(&my_cpu_vma_pool[id].pool_lock);
    return 0;
cleanup:
    release(&my_cpu_vma_pool[id].pool_lock);
    return -1;
}
vm_area_struct_t *find_vma(mm_struct_t *mm, uint64 vaddr){
    //Assuming hold mm->mm_lock(Lock-Prected Borrowing)
    //No refcount update is needed since the object isn't leaked out of the critical sections.
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vaddr=%llu\n", (void *)mm, vaddr);
#endif
    if(!holding(&mm->mm_lock))
        panic("[find_vma]Race Conditions: Accessing mm without lock");
    //Fast lookup using a cache-first,tree fallback strategy to find the vma
    //containg a specific address.
    vm_area_struct_t *found=NULL;
    if(mm==NULL)    panic("find_vma:pass an invalid argument!\n");
    found=mm->mmap_cache;
    if(found && vaddr >= found->vm_start && vaddr < found->vm_end)    return found; //cache hit
    rb_node_t *iter=mm->rb_root->rb_parent;//Cache miss, search the RB tree
    found=NULL;
    while(iter){
        vm_area_struct_t *candidate=rb_entry(iter, vm_area_struct_t, vm_rb_node);
        // printf("searching found iter in rb_tree, current found's range is [%p, %p)\n", 
        //     (void *)candidate->vm_start, (void *)candidate->vm_end);
        if(vaddr < candidate->vm_start)   iter=iter->rb_left;
        else if(vaddr >= candidate->vm_end)   iter=iter->rb_right;
        else{
            //found!address is within [vm_start, vm_end)
            found=candidate;break;
        }
    }
    if(found){
        mm->mmap_cache=found; //if found, update the cache
    }
    return found; 
}
vm_area_struct_t *find_vma_and_get(mm_struct_t *mm, uint64 addr){
    if(!holding(&mm->mm_lock))
        panic("[find_vma_and_get]Race Conditions: Accessing mm without lock");
    vm_area_struct_t *vma=find_vma(mm, addr);
    if(vma!=NULL)   vma_get(vma);
    return vma;
}
static vm_area_struct_t *find_upper_vma(mm_struct_t *mm, uint64 vaddr){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vaddr=%llu\n", (void *)mm, vaddr);
#endif
    //Find the first vma statifying vma->vm_end > addr
    //Differ from the find_vma, should exist one vma meet requirement unless empty vma_list
    if(!holding(&mm->mm_lock))
        panic("[find_upper_vma]Race Conditions: access mm without lock!\n");
    vm_area_struct_t *found=NULL;
    if(mm==NULL)    panic("find_vma:pass an invalid argument!\n");
    found=mm->mmap_cache;
    if(found && vaddr >= found->vm_start && vaddr < found->vm_end)
        return found; //cache hit
    rb_node_t *iter=mm->rb_root->rb_parent; //Cache miss, search the RB tree
    rb_node_t *best_fit=iter;
    found=NULL;
    while(iter){
        vm_area_struct_t *candidate=rb_entry(iter, vm_area_struct_t, vm_rb_node);
        if(vaddr < candidate->vm_start){    //Left side, a potential valid "Upper bound"
            best_fit=iter;
            iter=iter->rb_left;
        }
        else if(vaddr >= candidate->vm_end){    //Right side, cannot be "Upper bound"
            iter=iter->rb_right;
        }
        else{   //Exact match!
            //found!address is within [vm_start, vm_end)
            found=candidate;break;
        }
    }
    if(found){
        mm->mmap_cache=found; //if found, update the cache
        return found;
    }
    if(found==NULL)     //No exact match(Hole), return the upper bound.
        return rb_entry(best_fit, vm_area_struct_t, vm_rb_node);
    return NULL; 
}
vm_area_struct_t *find_upper_vma_and_get(mm_struct_t *mm, uint64 addr){
    if(!holding(&mm->mm_lock))
        panic("[find_vma_and_get]Race Conditions: Accessing mm without lock");
    vm_area_struct_t *vma=find_upper_vma(mm, addr);
    if(vma!=NULL)   vma_get(vma);
    return vma;
}
static rb_node_t *rb_search(rb_node_t *node, vm_area_struct_t **predecessor, 
        vm_area_struct_t **successor, const rb_root_t *root){
#ifdef DEBUG_KVM
    KVM_TRACE("node=%p predecessor=%p successor=%p root=%p\n", (void *)node, (void *)predecessor, (void *)successor, (void *)root);
#endif
    //A helper functions that locates the insertion parent and idenitifies
    //the linear list neighbors(prev/next) in a signle traversal.
    if(node==NULL){
        *predecessor=NULL;
        *successor=NULL;
        return NULL;
    }
    rb_node_t *find_used=NULL;
    uint8 found_pred=0, found_succ=0;
    if(node->rb_left!=NULL){
        find_used=node->rb_left;
        while(find_used->rb_right!=NULL) find_used=find_used->rb_right;
        *predecessor=rb_entry(find_used, vm_area_struct_t, vm_rb_node);
        found_pred=1;
    }
    //Successor
    if(node->rb_right!=NULL){
        find_used=node->rb_right;
        while(find_used->rb_left!=NULL) find_used=find_used->rb_left;
        *successor=rb_entry(find_used, vm_area_struct_t, vm_rb_node);
        found_succ=1;
    }
    //search upwards
    find_used=root->rb_parent;
    if(!found_pred) *predecessor=NULL;
    if(!found_succ) *successor=NULL;
    vm_area_struct_t *node_vma=rb_entry(node, vm_area_struct_t, vm_rb_node);
    rb_node_t *parent=root->rb_parent;
    while(find_used){
        vm_area_struct_t *find_used_vma=rb_entry(find_used, vm_area_struct_t, vm_rb_node);
        if(find_used_vma->vm_start >= node_vma->vm_end){
            if(found_succ==0)   *successor=rb_entry(find_used, vm_area_struct_t, vm_rb_node);   
            //Update whne it's not found!
            parent=find_used;
            find_used=find_used->rb_left;//don't record
        }
        else if(node_vma->vm_start >= find_used_vma->vm_end){
            if(found_pred==0)  *predecessor=rb_entry(find_used, vm_area_struct_t, vm_rb_node);  
            //Update when it's not found!
            parent=find_used;
            find_used=find_used->rb_right;
        }
        else if(node_vma==find_used_vma)    break;
        else    panic("rb_search unexpected situations!\n");
        //means overlap in area range, very dangerous and shouldn't happend!
    }
    return parent;
}
int insert_vma_fast(mm_struct_t *mm, vm_area_struct_t *vma, vma_context_t *cont){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vma=%p cont=%p\n", (void *)mm, (void *)vma, (void *)cont);
#endif
    //Check if cont qualifies for the fast path,
    //if not;fallback to the generic insetions.
    if(vma==NULL){
        printf("Insert an empty entry into vma_struct!\n");
        return -1;
    }
    if(vma->vm_start%PGSIZE!=0 || vma->vm_end%PGSIZE!=0){
        printf("insert_vma: pass an invalid argument!\n");
        return -1;
    }
    if(cont==NULL || (cont->prev==NULL && cont->next==NULL))
        return insert_vma(mm, vma);
    if((cont->prev && cont->prev->vm_end>vma->vm_start) ||
        (cont->next && cont->next->vm_start < vma->vm_end))
        return insert_vma(mm, vma);
    if((cont->prev && cont->prev->vm_next != cont->next) ||
        (cont->next && cont->next->vm_prev !=cont->prev))
        return insert_vma(mm, vma);
    //Insert into list according the tree hierarchy
    vma->vm_prev=cont->prev;
    vma->vm_next=cont->next;
    if(cont->prev)  cont->prev->vm_next=vma;
    else    mm->mmap=vma;
    if(cont->next)  cont->next->vm_prev=vma;
    //Insert into the RB-tree
    rb_node_t **link=NULL, *parent_node=NULL;
    if(cont->prev && cont->prev->vm_rb_node.rb_right==NULL){
        parent_node=&cont->prev->vm_rb_node;
        link=&cont->prev->vm_rb_node.rb_right;
    }
    else if(cont->next && cont->next->vm_rb_node.rb_left==NULL){
        parent_node=&cont->next->vm_rb_node;
        link=&cont->next->vm_rb_node.rb_left;
    }
    else    return insert_vma(mm, vma);
    rb_link_node(&vma->vm_rb_node, parent_node, link);
    rb_insert_color(&vma->vm_rb_node, mm->rb_root);
    return 0;
}
int insert_vma(mm_struct_t *mm, vm_area_struct_t *vma){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vma=%p\n", (void *)mm, (void *)vma);
#endif
    //Builds the structure by linking the new VMA into both 
    //the RB-Tree and the linked list after checking for overlaps
    if(mm==NULL || vma==NULL || vma->vm_start%PGSIZE!=0 || vma->vm_end%PGSIZE!=0)
        panic("insert_vma: pass an invalid argument!\n");
    if(!holding(&mm->mm_lock))
        panic("[Insert_vma]Race Condtions: access mm_struct without lock\n");
    rb_node_t *vma_node=&vma->vm_rb_node;
    rb_root_t *rb_root=mm->rb_root;
    rb_node_t **link=NULL;
    vm_area_struct_t *vm_prev, *vm_next;
    rb_node_t *parent_node=rb_search(vma_node, &vm_prev, &vm_next, rb_root);
    if(vm_prev && vm_prev->vm_end > vma->vm_start)  return -1;
    if(vm_next && vm_next->vm_start < vma->vm_end)  return -1;
    //Insert to the RB tree
    if(parent_node==NULL){
        link=&(rb_root->rb_parent);
        mm->mmap=vma;
        mm->rb_root->rb_parent=vma_node;
    }
    else{
        vm_area_struct_t *parent_vma=rb_entry(parent_node, vm_area_struct_t, vm_rb_node);
        if(parent_vma->vm_start >= vma->vm_end)  link=&parent_node->rb_left;
        else if(vma->vm_start >= parent_vma->vm_end)    link=&parent_node->rb_right;
    }
    rb_link_node(vma_node, parent_node, link);
    rb_insert_color(vma_node, mm->rb_root);
    //Insert into the list according to tree hierarchy
    //vma should be between the vm_prev and vm_next
    vma->vm_prev=vm_prev;
    vma->vm_next=vm_next;
    if(vm_prev) vm_prev->vm_next=vma;
    else    mm->mmap=vma;
    if(vm_next) vm_next->vm_prev=vma;
    return 0;
}
int remove_vma(mm_struct_t *mm, vm_area_struct_t *vma){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vma=%p\n", (void *)mm, (void *)vma);
#endif
    if(!holding(&mm->mm_lock))
        panic("[remove_vma]Race Conditions: access mm without lock!");
    //Remove from the list,maintain mmap,also need to free the associated resource
    rb_node_t *vma_node=&vma->vm_rb_node;
    vm_area_struct_t *vm_prev=vma->vm_prev, *vm_next=vma->vm_next;
    if(vm_prev) vm_prev->vm_next=vm_next;
    else    mm->mmap=vm_next;
    if(vm_next) vm_next->vm_prev=vm_prev;
    //tree-operation, Only here can edit rb_root
    rb_erase(vma_node, mm->rb_root);//remove form the tree
    //Update the cache to prevent Use-After-Free(UAF)
    if(mm->mmap_cache==vma) mm->mmap_cache=NULL;
    vma->vm_prev=NULL;vma->vm_next=NULL;
    if(vma && vma->vm_ops->close)    vma->vm_ops->close(vma);
    return vma_put(vma);
}
static uint64 get_unmapped_area(mm_struct_t *mm, uint64 len, 
        uint64 low_limit, uint64 high_limit, vma_context_t *cont){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p len=%llu low_limit=%llu high_limit=%llu cont=%p\n", (void *)mm, len, low_limit, high_limit, (void *)cont);
#endif
    if(mm==NULL || high_limit<=low_limit || len==0 || len%PGSIZE!=0){
        printf("get_unmapped_area:get invalid para!\n");
        return -1;
    }
    if(!holding(&mm->mm_lock))
        panic("[get_unmapped_area]Race Conditions: access mm without lock!");
    uint64 avail_len=high_limit-low_limit;
    if(avail_len<len){
        printf("Required size even larger than given range!\n");
        return -1;
    }
    //Size requirements met.
    //find the first vma->end > low_limit
    vm_area_struct_t *cur_vma=find_upper_vma_and_get(mm, low_limit);
    if(cur_vma==NULL){  //Non-found the upper bound.
        cont->prev=cur_vma;
        cont->next=NULL;
        return low_limit;
    }
    if(cur_vma->vm_start>=high_limit){  //Completely disjoint from this range
        cont->prev=cur_vma->vm_prev;
        cont->next=cur_vma;
        return low_limit;
    }
    else if(cur_vma->vm_start>low_limit){   //Fully contained within this range
        if(cur_vma->vm_start - low_limit >=len){
            cont->prev=cur_vma->vm_prev;
            cont->next=cur_vma;
            return low_limit;
        }
    }
    while(cur_vma!=NULL){   //partial overlaps
        if(cur_vma->vm_end>=high_limit)
            break;  //out-of-range!
        vm_area_struct_t *next_vma=cur_vma->vm_next;
        uint64 gap;
        if(next_vma!=NULL && next_vma->vm_start<high_limit)  
            gap=next_vma->vm_start-cur_vma->vm_end;
        else
            gap=high_limit-cur_vma->vm_end;

        if(gap < len)
            cur_vma=next_vma;
        else{
            cont->prev=cur_vma;
            cont->next=next_vma;
            return cur_vma->vm_end;//enough gap to cover len
        }
    }
    return -1;
}
static uint64 helper_kvmdealloc(pagetable_t Kpagetable, uint64 oldsz, uint64 new_sz){
#ifdef DEBUG_KVM
    KVM_TRACE("Kpagetable=%p oldsz=%llu new_sz=%llu\n", (void *)Kpagetable, oldsz, new_sz);
#endif
    if(!holding(&kvm_lock))
        panic("[helper_kvmdealloc]Race Conditions: access kernel pagetable without lock!");
    // release resources, exclude vma operations.
    return uvmdealloc(Kpagetable, oldsz, new_sz);
}
int Kernel_buddy_alloc(pagetable_t Kpagetable, uint64 va, uint64 size, int xperm){
#ifdef DEBUG_KVM
    KVM_TRACE("Kpagetable=%p va=%llu size=%llu xperm=%d\n", (void *)Kpagetable, va, size, xperm);
#endif
    //return 0 when Success , and return -1 when it fail!
    //A shared utility for use and kernel space.
    if(va%PGSIZE!=0){
        printf("Split_alloc: unaligned address!");
        return -1;
    }
    if(!in_kernel_heap(va) || !in_kernel_heap(va+size)){
        printf("Kernel_buddy_alloc: out-of-kernel-heap\n");
        return -1;
    }
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
            // KVM_TRACE("kvmalloc failed to allocate cur_va=0x%llx size=0x%llx\n", cur_va, alloc_size);
            acquire(&kvm_lock);
            helper_kvmdealloc(Kpagetable, cur_va, va);  //have not inserted into vma_list
            release(&kvm_lock);
            return -1;
        }
        memset(mem, 0, alloc_size);
        acquire(&kvm_lock);
        // KVM_TRACE("kvmalloc -> mappages cur_va=0x%llx alloc_size=0x%llx\n", cur_va, alloc_size);
        if(kvmmap_safe(Kpagetable, cur_va, alloc_size, (uint64)mem, xperm)!=0){
            free_pages(mem, alloc_size);
            // KVM_TRACE("kvmalloc mappages failed at cur_va=0x%llx size=0x%llx\n", cur_va, alloc_size);
            helper_kvmdealloc(Kpagetable, cur_va, va);  //have not inserted into vma_list
            return -1;
        }
        if(size>alloc_size)  size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        va_order=i_log2(cur_va & -cur_va);
        va_order=(va_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):va_order;
        cur_max_size=1ull << va_order;
    }
    release(&kvm_lock);
    return 0;
}
void *kvmalloc(pagetable_t Kpagetable, uint64 req_sz, int xperm){
#ifdef DEBUG_KVM
    KVM_TRACE("Kpagetable=%p req_sz=%llu xperm=%d\n", (void *)Kpagetable, req_sz, xperm);
#endif
    if(req_sz==0){
        printf("kernel_vmalloc:Invalid size : 0\n");
        goto error;
    }
    req_sz=PGROUNDUP(req_sz);
    //Given fixed range:[KHEAP_START, KHEAP_END)
    vma_context_t cont;
    memset(&cont, 0, sizeof(vma_context_t));
    acquire(&global_mm.mm_lock);
    uint64 start_va=get_unmapped_area(&global_mm, req_sz, KHEAP_START, KHEAP_END, &cont);
    if(start_va==(uint64)-1){
        printf("Cannot find the avail_space!\n");
        goto error;
    }
    //Alloc reserves metadata then physical resources.
    vm_area_struct_t *new_vma=NULL;
    if(sizeof(vm_area_struct_t)>=PGSIZE){   //Maybe the prelogue size larger than one page.
        uint64 nr_pages=PGROUNDUP(sizeof(vm_area_struct_t));
        new_vma=(vm_area_struct_t*)alloc_memory(nr_pages*PGSIZE);
        memset(new_vma, 0, nr_pages*PGSIZE);
    }
    else new_vma=alloc_vma_node();
    if(new_vma==NULL)   goto error;
    new_vma->vm_start=start_va;
    new_vma->vm_end=start_va+req_sz;
    new_vma->vm_page_prot=PTE_R | PTE_W;
    new_vma->vm_flags=gene_flags(new_vma->vm_page_prot);
    new_vma->vm_mm=&global_mm;
    //Omit values for unused arguments.
    insert_vma_fast(&global_mm, new_vma, &cont);

    //allocate the corresponding size
    int alloc_ret=Kernel_buddy_alloc(kernel_pagetable, start_va, req_sz, xperm);
    if(alloc_ret==-1){
        remove_vma(&global_mm, new_vma);    //clear the metadata
        printf("Kernel_buddy_alloc fail!\n");
        goto error;
    }
    release(&global_mm.mm_lock);
    return (void *)start_va;
error:
    printf("kvmalloc: fail!\n");
    release(&global_mm.mm_lock);
    return NULL;
    //Address 0 acts as a unique error indicator bacause it's guaranteed to be invalid.
}
uint64 kvmdealloc_range(pagetable_t Kpagetable, uint64 oldsz, uint64 newsz){
#ifdef DEBUG_KVM
    KVM_TRACE("Kpagetable=%p oldsz=%llu newsz=%llu\n", (void *)Kpagetable, oldsz, newsz);
#endif
    //Range Deallocation
    if(newsz>=oldsz || !in_kernel_heap(oldsz) || !in_kernel_heap(newsz)){
        printf("free invalid area!\n");
        goto error;
    }
    uint64 aligned_newsz=PGROUNDUP(newsz);
    uint64 aligned_oldsz=PGROUNDDOWN(oldsz);
    if(aligned_newsz==aligned_oldsz)    return newsz;
    acquire(&global_mm.mm_lock);
    vm_area_struct_t *find_ret=find_vma_and_get(&global_mm, aligned_newsz);
    if(find_ret==NULL){
        printf("Kvmdealloc:cannot find vma sastify requiment!\n");
        goto error;
    }
    else if(find_ret->vm_start!=aligned_newsz){
        printf("kvmdealloc: try to dealloc from the existing vma's inner");
        goto error;
    }
    else if(find_ret->vm_end!=aligned_oldsz){
        printf("kvmdealloc: try to dealloc more than one vma one time!\n");
        goto error;
    }
    //Pass the range test, current find_ret is located at [aligned_newsz, aligned_oldsz) exactly
    //Free must release physical resources then metadata.
    acquire(&kvm_lock);
    uint64 ret=helper_kvmdealloc(Kpagetable, aligned_oldsz, aligned_newsz);
    release(&kvm_lock);
    if(remove_vma(&global_mm, find_ret)==-1){
        printf("kvmdealloc_range: remove_vma fail\n");
        goto error;
    }
    release(&global_mm.mm_lock);
    return ret;
error:
    printf("kvmdealloc_range fail!\n");
    release(&global_mm.mm_lock);
    return -1;
}
uint64 kvmdealloc(pagetable_t Kpagetable, uint64 start_va, uint64 sz){
#ifdef DEBUG_KVM
    KVM_TRACE("Kpagetable=%p start_va=%llu sz=%llu\n", (void *)Kpagetable, start_va, sz);
#endif
    //Sized Deallocation
    return kvmdealloc_range(Kpagetable, start_va+sz, start_va);
}
//test-only code co-located to access status functions.
void test_vma_slab_allocator() {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    printf("=== Starting VMA Slab Allocator Test ===\n");
    // Definition: Create an array to hold many VMA pointers
    #define TEST_BATCH 200
    vm_area_struct_t *vma_ptrs[TEST_BATCH];
    // Step 1: Alloc until slab needs to expand (Stress Test)
    for (int i = 0; i < TEST_BATCH; i++) {
        vma_ptrs[i] = alloc_vma_node();
        if (vma_ptrs[i] == NULL) panic("Slab alloc failed prematurely!");
        // Verify: Ensure memory is zeroed out
        if (vma_ptrs[i]->vm_start != 0) panic("Slab node not zeroed!");
    }
    printf("  [Success] Allocated %d VMAs.\n", TEST_BATCH);
    // Step 2: Free half of them to trigger 'full' -> 'partial' transition
    for (int i = 0; i < TEST_BATCH; i += 2) {
        remove_vma(&global_mm, vma_ptrs[i]);
        vma_ptrs[i] = NULL;
    }
    printf("  [Success] Freed half VMAs.\n");
    // Step 3: Re-alloc to verify reuse mechanism
    for (int i = 0; i < TEST_BATCH; i += 2) {
        vma_ptrs[i] = alloc_vma_node();
        if (vma_ptrs[i] == NULL) panic("Slab reuse failed!");
    }
    printf("  [Success] re-alloc VMAs. \n");
    // Cleanup: Free all
    for (int i = 0; i < TEST_BATCH; i++) {
        if (vma_ptrs[i]) remove_vma(&global_mm, vma_ptrs[i]);
    }
    printf("=== VMA Slab Test Passed ===\n");
}

void test_vma_rbtree_and_list() {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    printf("=== Starting RB-Tree & List Consistency Test ===\n");
    // Mock MM struct specifically for testing
    mm_struct_t test_mm;
    memset(&test_mm, 0, sizeof(mm_struct_t));
    initlock(&test_mm.mm_lock, "test_mm_lock");
    test_mm.rb_root = (rb_root_t *)alloc_vma_node(); // Bootstrap root

    // Scenario: Insert 3 VMAs: A[0x1000-0x2000], C[0x3000-0x4000], B[0x2000-0x3000]
    // Order matters: Insert non-sequentially to test sorting logic
    // 1. Create VMA A
    vm_area_struct_t *vma_a = alloc_vma_node();
    vma_a->vm_start = 0x1000; vma_a->vm_end = 0x2000;
    insert_vma(&test_mm, vma_a);
    // 2. Create VMA C (Leave a gap for B)
    vm_area_struct_t *vma_c = alloc_vma_node();
    vma_c->vm_start = 0x3000; vma_c->vm_end = 0x4000;
    insert_vma(&test_mm, vma_c);
    // 3. Create VMA B (Fills the gap)
    vm_area_struct_t *vma_b = alloc_vma_node();
    vma_b->vm_start = 0x2000; vma_b->vm_end = 0x3000;
    insert_vma(&test_mm, vma_b);
    // Check 1: Lookup (RB-Tree functionality)
    vm_area_struct_t *found = find_vma_and_get(&test_mm, 0x1050);
    if (found != vma_a) panic("RB-Tree lookup failed for VMA A");
    
    found = find_vma_and_get(&test_mm, 0x2050);
    if (found != vma_b) panic("RB-Tree lookup failed for VMA B");
    // Check 2: Linked List Continuity
    if (test_mm.mmap != vma_a) panic("Head of list is wrong");
    if (vma_a->vm_next != vma_b) panic("List link A->B broken");
    if (vma_b->vm_next != vma_c) panic("List link B->C broken");
    if (vma_b->vm_prev != vma_a) panic("List link B<-A broken");
    // Check 3: Upper Bound Search (Gap finding)
    vm_area_struct_t *upper = find_upper_vma_and_get(&test_mm, 0x1500);
    // Should return A because A ends at 0x2000 > 0x1500? 
    // Wait, logic check: find_upper_vma finds first vma where vm_end > addr.
    // 0x1500 is inside A. A->end=0x2000 > 0x1500. Correct.
    if (upper != vma_a) panic("Upper bound check failed");
    // Cleanup
    remove_vma(&test_mm, vma_a);
    remove_vma(&test_mm, vma_b);
    remove_vma(&test_mm, vma_c);
    printf("=== RB-Tree & List Test Passed ===\n");
}

void test_kvmalloc_integrity() {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    printf("=== Starting kvmalloc Integration Test ===\n");

    // Define allocation size: 2.5 Pages (Testing alignment handling)
    uint64 alloc_sz = PGSIZE * 2 + PGSIZE / 2;
    
    // Step 1: Allocate memory
    char *ptr = (char *)kvmalloc(kernel_pagetable, alloc_sz, PTE_R | PTE_W);
    if (ptr == NULL) panic("kvmalloc failed");

    // Check alignment
    if ((uint64)ptr % PGSIZE != 0) panic("kvmalloc returned unaligned address");

    // Step 2: Write/Read Test (Verify Page Table Mapping)
    // Verify boundaries: Write to start, middle, and end
    ptr[0] = 'A';
    ptr[PGSIZE] = 'B'; 
    ptr[alloc_sz - 1] = 'C'; // Should be accessible due to PGROUNDUP

    if (ptr[0] != 'A' || ptr[PGSIZE] != 'B' || ptr[alloc_sz - 1] != 'C') {
        panic("Memory read/write verification failed");
    }
    printf("  [Success] Memory Read/Write OK.\n");

    // Step 3: Verify Metadata (VMA existence)
    acquire(&global_mm.mm_lock);
    vm_area_struct_t *vma = find_vma_and_get(&global_mm, (uint64)ptr);
    if (!vma || vma->vm_start != (uint64)ptr) panic("VMA not created for kvmalloc");
    release(&global_mm.mm_lock);

    // Step 4: Deallocation
    uint64 ret = kvmdealloc(kernel_pagetable, (uint64)ptr, alloc_sz);
    if (ret == -1) panic("kvmdealloc failed");

    // Verify VMA is gone
    acquire(&global_mm.mm_lock);
    vma = find_vma_and_get(&global_mm, (uint64)ptr);
    if (vma != NULL) panic("VMA still exists after free");
    release(&global_mm.mm_lock);

    printf("=== kvmalloc Test Passed ===\n");
}

void test_unmapped_area_search() {
#ifdef DEBUG_KVM
    KVM_TRACE("void\n");
#endif
    printf("=== Starting Unmapped Area Search Test ===\n");

    // 1. Create a sandwich structure: [Block A] [Block B] [Block C]
    // Block A: The base barrier (1 Page)
    void *ptr_a = kvmalloc(kernel_pagetable, PGSIZE, PTE_R | PTE_W);
    if (ptr_a == NULL) panic("Setup failed: ptr_a");

    // Block B: This will become the gap (2 Pages)
    void *ptr_b = kvmalloc(kernel_pagetable, 2 * PGSIZE, PTE_R | PTE_W);
    if (ptr_b == NULL) panic("Setup failed: ptr_b");

    // Block C: The upper barrier (1 Page)
    void *ptr_c = kvmalloc(kernel_pagetable, PGSIZE, PTE_R | PTE_W);
    if (ptr_c == NULL) panic("Setup failed: ptr_c");

    // Verify adjacency (First-fit logic check)
    // Ensures that the allocator originally packed them tightly
    if ((uint64)ptr_b != (uint64)ptr_a + PGSIZE) 
        panic("Allocator did not pack A and B tightly");
    if ((uint64)ptr_c != (uint64)ptr_b + 2 * PGSIZE) 
        panic("Allocator did not pack B and C tightly");

    // 2. Create the GAP by freeing Block B
    // Current layout: [A: Used] [HOLE: 2pg] [C: Used]
    kvmdealloc(kernel_pagetable, (uint64)ptr_b, 2 * PGSIZE);

    printf("  [Info] Created a 2-page gap at %p\n", ptr_b);

    // 3. Alloc Block D: Request 2 Pages
    // Based on First-Fit strategy, it MUST fit into the hole left by B
    void *ptr_d = kvmalloc(kernel_pagetable, 2 * PGSIZE, PTE_R | PTE_W);
    
    if (ptr_d == NULL) panic("Failed to allocate in the gap");

    // 4. Verify Strategy
    // The new pointer must strictly equal the old B pointer
    if (ptr_d != ptr_b) {
        printf("  [Error] Expected address %p but got %p\n", ptr_b, ptr_d);
        panic("Allocator failed to utilize the existing gap (Fragmentation issue)");
    }

    printf("  [Success] Allocator correctly filled the memory hole.\n");

    // Cleanup remaining blocks
    kvmdealloc(kernel_pagetable, (uint64)ptr_a, PGSIZE);
    kvmdealloc(kernel_pagetable, (uint64)ptr_d, 2 * PGSIZE);
    kvmdealloc(kernel_pagetable, (uint64)ptr_c, PGSIZE);

    printf("=== Unmapped Area Test Passed ===\n");
}
//DEBUG_only functions
void identify_list_nolock(page_slab_header_t *page){
    if(page==NULL || page->magic!=PAGE_SLAB_HEADER_MAGIC) 
        panic("identify_list_nolock!\n");
    int id=page->cpu_id;
    page_slab_header_t *partial=my_cpu_vma_pool[id].partial;
    while(partial!=NULL){
        if(page==partial){
            printf("In partial list!\n");
            return;
        }
        partial=partial->next_page;
    }
    page_slab_header_t *empty=my_cpu_vma_pool[id].empty;
    while(empty!=NULL){
        if(page==empty){
            printf("In empty list!\n");
            return;
        }
        empty=empty->next_page;
    }
    page_slab_header_t *full=my_cpu_vma_pool[id].full;
    while(full!=NULL){
        if(page==full){
            printf("In full list!\n");
            return;
        }
        full=full->next_page;
    }
    printf("Detached node\n");
}
void identify_list(page_slab_header_t *page){
    int id=page->cpu_id;
    acquire(&my_cpu_vma_pool[id].pool_lock);
    identify_list_nolock(page);
    release(&my_cpu_vma_pool[id].pool_lock);
}