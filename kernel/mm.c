//All operation about mm_struct_t and vm_area_struct_t
#include "param.h"
#include "types.h"
#include "atomic.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "vm.h"
#include "fcntl.h"
#include "mm.h"
#include "colors.h"
#define DEBUG_MM


//Compresssing pre-CPU data within caches eliminates the need of 
// create a seperate cache object for each CPU entry.
slab_cache_t *vma_cache=NULL;
slab_cache_t *mm_cache=NULL;
//Some basic function declerations.
int remove_mm(mm_struct_t *mm);

static inline int in_range_size(uint64 addr, uint64 start, uint64 size){
    if(start + size < start)    return 0;      //Wrapped back to zero.
    return (addr >= start) && (addr <start+size);
}
static inline int in_range(uint64 addr, uint64 start, uint64 end){
    if(end <= start){
        pr_err("Invalid range, end must bigger than start.");
        return 0;
    }
    return (addr >= start) && (addr < end);
}

//NOTE: Dangerous and discouraged practice that exposes internal interface.
// void vma_get(vm_area_struct_t *vma){    //Acquire one reference
//     if(vma==NULL)   return;
//     __sync_fetch_and_add(&vma->ref_count, 1);   //Atomic operations(lock-free)
//     #ifdef DEBUG_REF
//         printf("VMA %p get:ref=%d\n", vma, vma.ref_count);
//     #endif
// }

// int vma_put(vm_area_struct_t *vma){    //Drop one reference
//     //If I am the last one decrease ref_count return 1, otherwise return 0.
//     if(vma==NULL)   return 0;
//     int new_ref=__sync_sub_and_fetch(&vma->ref_count, 1);
//     #ifdef DEBUG_REF
//         printf("VMA %p put:ref=%d\n", vma, new_ref);
//     #endif
//     if(new_ref==0){
//         if(vma->vm_ops && vma->vm_ops->close)
//             vma->vm_ops->close(vma);
//         if(vma->vm_file!=NULL)
//             fileclose(vma->vm_file);
//         return slab_free((void *)vma);
//     }
//     else if(new_ref<0){
//         MM_TRACE("VMA Ref-count underflow!Double free deteched!");
//         pr_err("Extermely! dangerous in free %llx\n", (uint64)vma);
//         __sync_lock_test_and_set(&vma->ref_count, REF_SATURATION);
//         return 0;
//     }
//     return 0;
// }
void vma_get(vm_area_struct_t *vma){
    if(unlikely(vma==NULL))  return; 
    //error, use unlikely.While in hot path,use likely() but still need handle error.(less flat)
    if(atomic_inc_not_zero(&vma->ref_count)!=1)
        panic("Try to get uninitialized vma, or have already free.");
}
int vma_put(vm_area_struct_t *vma){
    if(unlikely(vma==NULL))     return -1;
    int new_ref=atomic_dec_and_ret(&vma->ref_count);
    if(new_ref==0){
        if(vma->vm_ops && vma->vm_ops->close)
            vma->vm_ops->close(vma);
        if(vma->vm_file!=NULL)
            fileclose(vma->vm_file);
        return slab_free((void *)vma);
    }
    else if(unlikely(new_ref<0)){
        MM_TRACE("VMA Ref-count underflow!Double free deteched!");
        panic("Extermely! dangerous in free %llx\n", (uint64)vma);
    }
    return 0;
}

static int vma_ctor(void *ptr){
    //treat as vma,initialize ref_count
    vm_area_struct_t *vma=(vm_area_struct_t *)ptr;
    if(unlikely(vma==NULL))   return -1;
    else{   //do some additional initialization.
        atomic_set(&vma->ref_count, 1);
        return 0;
    }
}

void mm_get(mm_struct_t *mm){    //Acquire one reference
    if(unlikely(mm==NULL))      return;
    if(atomic_inc_not_zero(&mm->ref_count)!=1)
        panic("Get a uninitialized mm, or have already free.");
}

int mm_put(mm_struct_t *mm){    //Drop one reference
    // return 1 If I am the last one decrease ref_count, otherwise return 0.
    if(unlikely(mm==NULL))      return -1;
    int new_ref=atomic_dec_and_ret(&mm->ref_count);
    if(new_ref==0){
        if(remove_mm(mm)!=0)
            pr_warn("remove_mm fail, Check the reclaim logic.\n");
        return 1;
    }
    else if(unlikely(new_ref<0)){
        MM_TRACE("MM Ref-count underflow!Double free deteched!");
        panic("Extermely! dangerous in free %llx\n", (uint64)mm);
    }
    return 0;
}

static int mm_ctor(void *ptr){
    //treat as vma,initialize ref_count
    mm_struct_t *mm=(mm_struct_t *)ptr;
    if(mm==NULL)
        return -1;
    else{   //do some additional initialization.
        atomic_set(&mm->ref_count, 1);
        initsleeplock(&mm->mm_lock, "mm_struct's lock.");
        return 0;
    }
}


void init_mm(void){ //Init the system(alloc prepare, )
    //Deferred reclaimation of slab object,typicall occuring long after their lifecycle.
    //And the slab destructor is predominatly a nop in standard configuration.
    vma_cache=create_slab_cache("vma_pool", sizeof(vm_area_struct_t), 8, vma_ctor, NULL);
    if(vma_cache==NULL){
        panic("init vma_cache fail.\n");
        return;
    }
    else    MM_TRACE("init vma_cache succeed.\n");
    mm_cache=create_slab_cache("mm_pool", sizeof(mm_struct_t), 8, mm_ctor, NULL);
    if(mm_cache==NULL){
        panic("init mm_cache fail.\n");
        return;
    }
    else    MM_TRACE("init mm_cache succeed.\n");
}

vm_area_struct_t *kernel_insert_vma_helper(mm_struct_t *mm, uint64 va, uint64 sz, int perm){
#ifdef DEBUG_KVM
    KVM_TRACE("va=%llx sz=%llx perm=%d\n", va, sz, perm);
#endif
    if(!holdingsleep(&mm->mm_lock))    //Kernel-specific VMA initialization
        panic("[Insert_vma_helper]Race Conditions: access global_mm without lock\n");
    vm_area_struct_t *tmp_vma=alloc_kernel_vma();
    if(tmp_vma==NULL)   return NULL;
    memset(tmp_vma, 0, sizeof(vm_area_struct_t));
    tmp_vma->vm_start=va;
    tmp_vma->vm_end=va+sz;
    tmp_vma->vm_page_prot=perm | PTE_V;     //get the hareware permission directly.
    uint64 flags = VM_KERN | VM_LOCKED | VM_DONTEXPAND | VM_DONTDUMP;
    if (perm & PTE_R) flags |= VM_READ;
    if (perm & PTE_W) flags |= VM_WRITE;
    if (perm & PTE_X) flags |= VM_EXEC;
    tmp_vma->vm_flags=flags;
    tmp_vma->vm_mm=mm;
    atomic_set(&tmp_vma->ref_count, 1);   //Held by mm_struct
    //Omit values for unused arguments.
    if(insert_vma(mm, tmp_vma)!=0){
        pr_warn("insert va=%llx, size=%llx fail.\n", va, sz);
        vma_put(tmp_vma);
        return NULL;
    }
    return tmp_vma;
}

//-----------------------VMA_OPERATIONS--------------------------------

__attribute__((warn_unused_result)) vm_area_struct_t *alloc_vma_node(){
    return (vm_area_struct_t *)slab_alloc(vma_cache);
}

vm_area_struct_t *vma_dup(const vm_area_struct_t *vma){       //Copy safely.
    if(vma==NULL)   return NULL;
    vm_area_struct_t *vma_clone=alloc_vma_node();
    if(vma_clone==NULL){
        pr_err("Vma_dup fail, unable create one new vma.");
        return NULL;
    }
    //Transitioning to the Commitment Phase;
    *vma_clone=*vma;
    atomic_set(&vma_clone->ref_count, 1);
    vma_clone->vm_next=NULL;
    vma_clone->vm_prev=NULL;
    memset(&vma_clone->vm_rb_node, 0, sizeof(rb_node_t));
    if(vma->vm_file!=NULL)  vma_clone->vm_file=filedup(vma->vm_file);
    if(vma->vm_ops!=NULL && vma->vm_ops->open!=NULL)
        vma->vm_ops->open(vma_clone);   //Notify the memory management and driver layers
    return vma_clone;
}

__attribute__((warn_unused_result)) mm_struct_t *mm_create() {
    return (mm_struct_t *)slab_alloc(mm_cache);
}

int remove_mm(mm_struct_t *mm){
    if(mm==NULL)    return 0;
    acquiresleep(&mm->mm_lock);
    vm_area_struct_t *clear_vma=mm->mmap, *tmp_next;
    while(clear_vma!=NULL){
        tmp_next=clear_vma->vm_next;
        remove_vma(mm, clear_vma);   //remove from the existing mm completely
        clear_vma=tmp_next;
    }
    releasesleep(&mm->mm_lock);
    //The cpu cannot unlock a memory area that has already been deallocated
    return slab_free((void *)mm);
}

vm_area_struct_t *find_vma(mm_struct_t *mm, uint64 vaddr){
    //Assuming hold mm->mm_lock(Lock-Prected Borrowing)
    //No refcount update is needed since the object isn't leaked out of the critical sections.
#ifdef DEBUG_KVM
    KVM_TRACE("While mm=%p, try to find vaddr %llx\n", (void *)mm, vaddr);
#endif
    if(!holdingsleep(&mm->mm_lock))
        panic("[find_vma]Race Conditions: Accessing mm without lock");
    //Fast lookup using a cache-first,tree fallback strategy to find the vma
    //containg a specific address.
    vm_area_struct_t *found=NULL;
    if(mm==NULL)    panic("find_vma:pass an invalid argument!\n");
    found=mm->mmap_cache;
    if(found && vaddr >= found->vm_start && vaddr < found->vm_end)    return found; //cache hit
    rb_node_t *iter=mm->rb_root.rb_parent;//Cache miss, search the RB tree
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
    else
        MM_TRACE("Could not locate a node containing this vaddr within the VMA pool.\n");
    return found; 
}

vm_area_struct_t *find_vma_and_get(mm_struct_t *mm, uint64 addr){
    if(!holdingsleep(&mm->mm_lock))
        panic("Race Conditions: Accessing mm without lock");
    vm_area_struct_t *vma=find_vma(mm, addr);
    if(vma!=NULL)   vma_get(vma);
    else    MM_TRACE("Could not locate a node containd addr within vma pool.\n");
    return vma;
}

static vm_area_struct_t *find_upper_vma(mm_struct_t *mm, uint64 vaddr){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vaddr=%llx\n", (void *)mm, vaddr);
#endif
    //Find the first vma statifying vma->vm_end > addr
    //Differ from the find_vma, should exist one vma meet requirement unless empty vma_list
    if(!holdingsleep(&mm->mm_lock))
        panic("[find_upper_vma]Race Conditions: access mm without lock!\n");
    vm_area_struct_t *found=NULL;
    if(mm==NULL)    panic("find_vma:pass an invalid argument!\n");
    found=mm->mmap_cache;
    if(found && vaddr >= found->vm_start && vaddr < found->vm_end)
        return found; //cache hit
    rb_node_t *iter=mm->rb_root.rb_parent; //Cache miss, search the RB tree
    rb_node_t *best_fit=NULL;
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
    if(!holdingsleep(&mm->mm_lock))
        panic("[find_vma_and_get]Race Conditions: Accessing mm without lock");
    vm_area_struct_t *vma=find_upper_vma(mm, addr);
    if(vma!=NULL)   vma_get(vma);
    return vma;
}

rb_node_t *rb_search(rb_node_t *node, vm_area_struct_t **predecessor, 
        vm_area_struct_t **successor, const rb_root_t *root){
#ifdef DEBUG_KVM
    KVM_TRACE("node=%p predecessor=%p successor=%p root=%p\n", (void *)node, (void *)predecessor, (void *)successor, (void *)root);
#endif
    //A helper functions that locates the insertion parent and idenitifies
    //the linear list neighbors(prev/next) in a signle traversal.
    if(node==NULL || root->rb_parent==NULL){
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
        if(find_used_vma->vm_start >= node_vma->vm_end){    //turn left
            if(found_succ==0)   *successor=rb_entry(find_used, vm_area_struct_t, vm_rb_node);   
            //Update whne it's not found!
            parent=find_used;
            find_used=find_used->rb_left;//don't record
        }
        else if(node_vma->vm_start >= find_used_vma->vm_end){   //turn right
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
    KVM_TRACE("trying to insert [%llx, %llx)\n", vma->vm_start, vma->vm_end);
#endif
    //Check if cont qualifies for the fast path,
    //if not;fallback to the generic insetions.
    if(vma==NULL){
        pr_err("Insert an empty entry into vma_struct!\n");
        return -1;
    }
    if(vma->vm_start%PGSIZE!=0 || vma->vm_end%PGSIZE!=0){
        pr_err("insert_vma: pass an invalid argument!\n");
        return -1;
    }
    if(vma->vm_mm!=mm){     //Initialize this member explicitly before enter this function.
        pr_err("try to insert vma to another new tree(maybe the tree have already replaced.\n)");
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
    if(!holdingsleep(&mm->mm_lock))
        panic("[insert_vma_fast]Race Conditions: access mm without lock\n");
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
    Cycle_detection(&mm->rb_root);
    rb_insert_color(&vma->vm_rb_node, &mm->rb_root);
    return 0;
}

int insert_vma(mm_struct_t *mm, vm_area_struct_t *vma){
    //for some special cases: e.g. heap_vma,the(vm_start < vm_end) isn't strictly required.
    //Therefore, we relax this constraint and allow such configuraion.
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p vma=%p\n", (void *)mm, (void *)vma);
#endif
    //Builds the structure by linking the new VMA into both 
    //the RB-Tree and the linked list after checking for overlaps
    if(vma->vm_mm!=mm){
        pr_warn("try to insert vma to another new tree(maybe the tree have already replaced.\n)");
        return -1;
    }
    if(mm==NULL || vma==NULL || vma->vm_start%PGSIZE!=0 || vma->vm_end%PGSIZE!=0){
        pr_err("pass an invalid argument!\n");
        return -1;
    }
    if(!holdingsleep(&mm->mm_lock))
        panic("Race Condtions: access mm_struct without lock\n");
    rb_node_t *vma_node=&vma->vm_rb_node;
    rb_node_t **link=NULL;
    vm_area_struct_t *vm_prev, *vm_next;
    rb_node_t *parent_node=rb_search(vma_node, &vm_prev, &vm_next, &mm->rb_root);
    if(parent_node==vma_node){
        pr_err("Attempt to insert duplicate node!\n");
        return -1;
    }
    if(vm_prev && vm_prev->vm_end > vma->vm_start){
        pr_err("Got the wrong predecessor node\n");
        return -1;
    }
    if(vm_next && vm_next->vm_start < vma->vm_end){
        pr_err("Got the wrong successor node\n");
        return -1;
    }
    //Insert to the RB tree
    if(parent_node==NULL){
        link=&mm->rb_root.rb_parent;
        mm->mmap=vma;
        mm->rb_root.rb_parent=vma_node;
    }
    else{
        vm_area_struct_t *parent_vma=rb_entry(parent_node, vm_area_struct_t, vm_rb_node);
        if(parent_vma->vm_start >= vma->vm_end)  link=&parent_node->rb_left;
        else if(vma->vm_start >= parent_vma->vm_end)    link=&parent_node->rb_right;
        else    panic("the Inserted vma overlap with the existing vma!\n");
    }
    rb_link_node(vma_node, parent_node, link);
    Cycle_detection(&mm->rb_root);
    rb_insert_color(vma_node, &mm->rb_root);
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
    if(!holdingsleep(&mm->mm_lock))
        panic("[remove_vma]Race Conditions: access mm without lock!");
    if(vma->vm_mm!=mm){
        pr_warn("try to remove vma from another tree(Dismatch mm_struct_t.\n)");
        return -1;
    }
    //Remove from the list,maintain mmap,also need to free the associated resource
    rb_node_t *vma_node=&vma->vm_rb_node;
    vm_area_struct_t *vm_prev=vma->vm_prev, *vm_next=vma->vm_next;
    if(vm_prev) vm_prev->vm_next=vm_next;
    else    mm->mmap=vm_next;
    if(vm_next) vm_next->vm_prev=vm_prev;
    //tree-operation, Only here can edit rb_root
    rb_erase(vma_node, &mm->rb_root);//remove form the tree
    rb_clear(vma_node); //Deleted nodes shoulds no longer hold pointers to the linked structure.
    //Update the cache to prevent Use-After-Free(UAF)
    if(mm->mmap_cache==vma) mm->mmap_cache=NULL;
    vma->vm_prev=NULL;vma->vm_next=NULL;
    vma->vm_mm=NULL;
    return vma_put(vma);
}

uint64 get_unmapped_area(mm_struct_t *mm, uint64 len, 
        uint64 low_limit, uint64 high_limit, vma_context_t *cont){
#ifdef DEBUG_KVM
    KVM_TRACE("mm=%p len=%llx low_limit=%llx high_limit=%llx cont=%p\n", (void *)mm, len, low_limit, high_limit, (void *)cont);
#endif
    if(mm==NULL || high_limit<=low_limit || len==0 || len%PGSIZE!=0){
        pr_err("get_unmapped_area:get invalid para!\n");
        return -1;
    }
    if(!holdingsleep(&mm->mm_lock))
        panic("[get_unmapped_area]Race Conditions: access mm without lock!");
    uint64 avail_len=high_limit-low_limit;
    if(avail_len<len){
        pr_err("Required size even larger than given range!\n");
        return -1;
    }
    //Size requirements met.
    //find the first vma->end > low_limit
    vm_area_struct_t *cur_vma=find_upper_vma(mm, low_limit);
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
        else    gap=high_limit-cur_vma->vm_end;

        if(gap < len)   cur_vma=next_vma;
        else{
            cont->prev=cur_vma;
            cont->next=next_vma;
            return cur_vma->vm_end;//enough gap to cover len
        }
    }
    return -1;
}

int do_munmap(munmap_context_t *ctx1){
    //Grarantee holding the process's uvm_lock at the same time.
    if(ctx1->mm==NULL || ctx1->length==0 || !holdingsleep(&ctx1->mm->mm_lock)){
        pr_warn("Invalid parameter.\n");
        return -1;
    }
    if((uint64)ctx1->addr + ctx1->length < (uint64)ctx1->addr || 
        (uint64)ctx1->addr + ctx1->length > UPPER_LIMIT){
        pr_err("Invalid length or reach immutable region.");
        return -1;
    }
    //Locate all affected VMAs and sequentially unmap them based on the extent of overlap.
    vm_area_struct_t *tmp=ctx1->mm->mmap, *tmp_next=NULL;
    uint64 cur_addr=(uint64)ctx1->addr, end_addr=(uint64)ctx1->addr+ctx1->length;
    cur_addr=(uint64)ctx1->addr;      //reset
    while(tmp!=NULL){
        tmp_next=tmp->vm_next;
        if(tmp->vm_start < end_addr && tmp->vm_end > cur_addr){
            if(tmp->vm_start > cur_addr && tmp->vm_end<=end_addr){      //Enclose
                cur_addr=tmp->vm_end;
                uvmfree_range(ctx1->rb_array, ctx1->pagetable, tmp->vm_start, tmp->vm_end);
                remove_vma(ctx1->mm, tmp);
            }
            else if(tmp->vm_start > cur_addr && tmp->vm_end > end_addr){    //Head Overlap
                uvmfree_range(ctx1->rb_array, ctx1->pagetable, tmp->vm_start, end_addr);
                tmp->vm_start=end_addr;
                break;
            }
            else if(tmp->vm_start < cur_addr && tmp->vm_end < end_addr){    //Tail Overlap
                uvmfree_range(ctx1->rb_array, ctx1->pagetable, cur_addr, tmp->vm_end);
                uint64 tmp_swap=tmp->vm_end;
                tmp->vm_end=cur_addr;
                cur_addr=tmp_swap;
            }
            else{   //Middle Split
                vm_area_struct_t *new_vma=vma_dup(tmp);
                if(new_vma==NULL){      //must be the first involoved VMA, no additional work to rollback.
                    pr_err("split into two vmas fail.\n");
                    return -1;
                }
                //Fine-grained adjustment.
                new_vma->vm_start=end_addr;
                new_vma->vm_end=tmp->vm_end;
                new_vma->vm_pgoff=tmp->vm_pgoff + ((end_addr -tmp->vm_start) >> PGSHIFT);
                //Perform selective copying based on specific criteria.
                if(insert_vma(ctx1->mm, new_vma)!=0){
                    pr_err("split into two vmas failed.\n");
                    vma_put(new_vma);
                    return -1;
                }
                tmp->vm_end=cur_addr;
                uvmfree_range(ctx1->rb_array, ctx1->pagetable, cur_addr, end_addr);
                break;
            }
        }
        tmp=tmp_next;
    }
    return 0;
}

uint64 find_first_suit_hole(mm_struct_t *mm, uint64 req_len){
    if(mm==NULL || !holdingsleep(&mm->mm_lock)){
        pr_err("Access invalid mm_struct or without necessary lock.");
        return -1;
    }
    vm_area_struct_t *tmp_vma=mm->mmap;
    uint64 prev_end=0;
    int found_suff=0;
    while(tmp_vma!=NULL){
        prev_end=(tmp_vma->vm_prev!=NULL)?(tmp_vma->vm_prev->vm_end):0;
        if(tmp_vma->vm_start-prev_end >= req_len){
            found_suff=1;
            break;
        }
        prev_end=tmp_vma->vm_end;   //record for possible use while can't find sufficient VMA.
        tmp_vma=tmp_vma->vm_next;
    }
    //Failing to find a suitable gap between existing VMAs
    //So the new region is appended to the end of address space.
    if(found_suff==0){
        //check if last_gap is enough
        if(UPPER_LIMIT < prev_end + req_len){
            pr_err("No gap of sufficient size exists bwtween VMAs;mmap failed.");
            return -1;
        }
    }
    return prev_end;
}

void *do_mmap(mmap_context_t *ctx1){
    if(ctx1==NULL){
        pr_err("Empty mmap_context_t.");
        return (void *)-1;
    }
    //Grarantee holding the process's uvm_lock at the same time.
    if(ctx1->mm==NULL || !holdingsleep(&ctx1->mm->mm_lock)){
        pr_err("Access invalid mm_struct or without necessary lock.");
        return (void *)-1;
    }
    uint64 final_start=(uint64)ctx1->sugg_addr;
    vm_area_struct_t *new_vma=alloc_vma_node();
    if(new_vma==NULL){
        pr_err("Sys_mmap failed, unable to create a new vma.\n");
        return (void *)-1;
    }
    new_vma->vm_flags=calc_vm_flags(ctx1->prot, ctx1->flags);
    new_vma->vm_page_prot=flags2page_prot(new_vma->vm_flags) | PTE_U;
    if(ctx1->length & (PGSIZE-1))      ctx1->length=PGROUNDUP(ctx1->length);
    if(final_start + ctx1->length > UPPER_LIMIT){
        pr_err("No gap of sufficient size exists bwtween VMAs;mmap failed.");
        vma_put(new_vma);
        return (void *)-1;
    }
    if(ctx1->sugg_addr!=NULL){
        vm_area_struct_t *tmp_vma=NULL;
        if((tmp_vma=find_upper_vma(ctx1->mm, (uint64)ctx1->sugg_addr))!=NULL){
            uint64 avai_start=(tmp_vma->vm_prev!=NULL)?(tmp_vma->vm_prev->vm_end):0;
            if(!((uint64)ctx1->sugg_addr >= avai_start && 
                (uint64)ctx1->sugg_addr +ctx1->length <=tmp_vma->vm_start)){
                if(!(ctx1->flags & MAP_FIXED)){
                    pr_warn("Not map_fixed and no matching gap at this address.");
                    final_start=find_first_suit_hole(ctx1->mm, ctx1->length);
                    if(final_start==(uint64)-1){
                        vma_put(new_vma);
                        return (void *)-1;
                    }
                }
                //MAP_FIXED, discussing various scenarios.
                else{
                    do_munmap(&(munmap_context_t)
                        {.pagetable=ctx1->pagetable,
                        .rb_array=ctx1->rb_array,
                        .mm=ctx1->mm,
                        .addr=ctx1->sugg_addr, 
                        .length=ctx1->length});
                }
            }
        }
    }
    else{
        final_start=find_first_suit_hole(ctx1->mm, ctx1->length);   //First-fit
        if(final_start==(uint64)-1){
            vma_put(new_vma);
            return (void *)-1;
        }
    }
    new_vma->vm_start=(uint64)final_start;
    new_vma->vm_end=new_vma->vm_start+ctx1->length;
    new_vma->vm_file=filedup(ctx1->f);
    new_vma->vm_pgoff=ctx1->offset;
    new_vma->vm_mm=ctx1->mm;
    new_vma->vm_filesz=(new_vma->vm_file==NULL)?0:new_vma->vm_file->ip->size;
    if(insert_vma(ctx1->mm, new_vma)!=0)    panic("Shouldn't failed.\n");
    return (void *)new_vma->vm_start;
}


//-------------------VMA_OPERATIONS_END---------------------------------