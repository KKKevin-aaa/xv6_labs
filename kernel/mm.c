//All operation about mm_struct_t and vm_area_struct_t
#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "mm.h"
#include "colors.h"
#define DEBUG_MM


//Compresssing pre-CPU data within caches eliminates the need of 
// create a seperate cache object for each CPU entry.
slab_cache_t *vma_cache=NULL;
slab_cache_t *mm_cache=NULL;
//Some basic function declerations.
static int reclaim_vma_node(vm_area_struct_t *);
int remove_mm(mm_struct_t *mm);

uint64 gene_page_prot(uint64 vm_flags) {    //VMA->PTE
    //As a hardware-agnostic kernel structure, it implements the translation
    //from logical abstraction to physical hardware via the following functions.
    uint64 page_prot = 0;
    if (vm_flags & VM_READ)     page_prot |= PTE_R;
    if (vm_flags & VM_WRITE)    page_prot |= PTE_W;
    if (vm_flags & VM_EXEC)     page_prot |= PTE_X;
    if(vm_flags & PROT_USER)    page_prot |= PTE_U;
    page_prot |= PTE_V;

    if (vm_flags & VM_IO) {
        page_prot |= (PTE_A | PTE_D);
    }

    return page_prot;
}
uint64 gene_flags(uint64 vm_page_prot) {    //PTE->VMA
    uint64 vm_flags = 0;
    if (vm_page_prot & PTE_R)   vm_flags |= VM_READ;
    if (vm_page_prot & PTE_W)   vm_flags |= VM_WRITE;
    if (vm_page_prot & PTE_X)   vm_flags |= VM_EXEC;
    if(vm_page_prot & PTE_U)    vm_flags |= PROT_USER;
    // 2. 关键逻辑：推断 COW 状态
    // 如果物理页表显示“只读”(无 PTE_W)，但同时标记了软件位 COW (PTE_COW)，
    // 说明这个页面在逻辑上其实是“可写”的 (VM_WRITE)，只是暂时被内核锁住了。
    // 在恢复 vm_flags 时，必须把 VM_WRITE 加回去。
    if ((vm_page_prot & PTE_COW) && !(vm_page_prot & PTE_W)) {
        vm_flags |= VM_WRITE;
    }
    // 3. 辅助位处理
    // PTE_A, PTE_D, PTE_U 通常不直接对应 VMA 的 flag，
    // 而是用于页面置换算法或权限检查，此处不需要映射回去。
    return vm_flags;
}


void vma_get(vm_area_struct_t *vma){    //Acquire one reference
    if(vma==NULL)   return;
    __sync_fetch_and_add(&vma->ref_count, 1);   //Atomic operations(lock-free)
    #ifdef DEBUG_REF
        printf("VMA %p get:ref=%d\n", vma, vma.ref_count);
    #endif
}

int vma_put(vm_area_struct_t *vma){    //Drop one reference
    //If I am the last one decrease ref_count return 1, otherwise return 0.
    if(vma==NULL)   return 0;
    int new_ref=__sync_sub_and_fetch(&vma->ref_count, 1);
    #ifdef DEBUG_REF
        printf("VMA %p put:ref=%d\n", vma, new_ref);
    #endif
    if(new_ref==0){
        if(vma->vm_ops && vma->vm_ops->close)
            vma->vm_ops->close(vma);
        if(vma->vm_file!=NULL)
            fileclose(vma->vm_file);
        reclaim_vma_node(vma);
        return 1;
    }
    else if(new_ref<0){
        MM_TRACE("VMA Ref-count underflow!Double free deteched!");
        pr_err("Extermely! dangerous in free %llx\n", (uint64)vma);
        __sync_lock_test_and_set(&vma->ref_count, REF_SATURATION);
        return 0;
    }
    return 0;
}

static int vma_ctor(void *ptr){
    //treat as vma,initialize ref_count
    vm_area_struct_t *vma=(vm_area_struct_t *)ptr;
    if(vma==NULL)
        return -1;
    else{   //do some additional initialization.
        vma->ref_count=1;
        return 0;
    }
}

void mm_get(mm_struct_t *mm){    //Acquire one reference
    if(mm==NULL)   return;
    __sync_fetch_and_add(&mm->ref_count, 1);
    #ifdef DEBUG_REF
        printf("VMA %p get:ref=%d\n", mm, mm.ref_count);
    #endif
}

int mm_put(mm_struct_t *mm){    //Drop one reference
    // return 1 If I am the last one decrease ref_count, otherwise return 0.
    if(mm==NULL)   return 0;
    int new_ref=__sync_sub_and_fetch(&mm->ref_count, 1);
    #ifdef DEBUG_REF
        printf("VMA %p put:ref=%d\n", mm, new_ref);
    #endif
    if(new_ref==0){
        if(remove_mm(mm)!=0)
            pr_warn("remove_mm fail, Check the reclaim logic.\n");
        return 1;
    }
    else if(new_ref<0){
        pr_warn("VMA Ref-count underflow!Double free deteched!");
        __sync_lock_test_and_set(&mm->ref_count, REF_SATURATION);
    }
    return 0;
}

static int mm_ctor(void *ptr){
    //treat as vma,initialize ref_count
    mm_struct_t *mm=(mm_struct_t *)ptr;
    if(mm==NULL)
        return -1;
    else{   //do some additional initialization.
        mm->ref_count=1;
        initlock(&mm->mm_lock, "mm_struct's lock.");
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

vm_area_struct_t *insert_vma_helper(mm_struct_t *mm, uint64 va, uint64 sz, int perm){
#ifdef DEBUG_KVM
    KVM_TRACE("va=%llx sz=%llx perm=%d\n", va, sz, perm);
#endif
    if(!holding(&mm->mm_lock))    //Kernel-specific VMA initialization
        panic("[Insert_vma_helper]Race Conditions: access global_mm without lock\n");
    vm_area_struct_t *tmp_vma=alloc_kernel_vma();
    if(tmp_vma==NULL)   return NULL;
    memset(tmp_vma, 0, sizeof(vm_area_struct_t));
    tmp_vma->vm_start=va;
    tmp_vma->vm_end=va+sz;
    tmp_vma->vm_page_prot=perm;
    tmp_vma->vm_flags=gene_flags(tmp_vma->vm_page_prot);
    tmp_vma->vm_mm=mm;
    tmp_vma->ref_count=1;   //Held by mm_struct
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

__attribute__((warn_unused_result)) mm_struct_t *mm_create() {
    return (mm_struct_t *)slab_alloc(mm_cache);
}

static int reclaim_vma_node(vm_area_struct_t *node){
    if(node->vm_file!=NULL)     fileclose(node->vm_file);
    if(node->vm_ops!=NULL && node->vm_ops->close!=NULL)
        node->vm_ops->close(node);
    return slab_free((void *)node);
}

int remove_mm(mm_struct_t *mm){
    if(mm==NULL)    return 0;
    acquire(&mm->mm_lock);
    vm_area_struct_t *clear_vma=mm->mmap, *tmp_next;
    while(clear_vma!=NULL){
        tmp_next=clear_vma->vm_next;
        if(remove_vma(mm, clear_vma)!=0)   //remove from the existing mm completely
            panic("remove vma %p fail.\n", clear_vma);
        clear_vma=tmp_next;
    }
    release(&mm->mm_lock);
    //The cpu cannot unlock a memory area that has already been deallocated
    return slab_free((void *)mm);
}

vm_area_struct_t *find_vma(mm_struct_t *mm, uint64 vaddr){
    //Assuming hold mm->mm_lock(Lock-Prected Borrowing)
    //No refcount update is needed since the object isn't leaked out of the critical sections.
#ifdef DEBUG_KVM
    KVM_TRACE("While mm=%p, try to find vaddr %llx\n", (void *)mm, vaddr);
#endif
    if(!holding(&mm->mm_lock))
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
    if(!holding(&mm->mm_lock))
        panic("[find_vma_and_get]Race Conditions: Accessing mm without lock");
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
    if(!holding(&mm->mm_lock))
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
    if(!holding(&mm->mm_lock))
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
    if(!holding(&mm->mm_lock))
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
    if(!holding(&mm->mm_lock))
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
    if(!holding(&mm->mm_lock))
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
    if(!holding(&mm->mm_lock))
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
//-------------------VMA_OPERATIONS_END---------------------------------