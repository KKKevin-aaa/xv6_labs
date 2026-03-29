#include "param.h"
#include "types.h"
#include "atomic.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "slab.h"
#include "kalloc.h"
#include "utils.h"

//size-special pool
static slab_cache_t boot_cache;
extern slab_cache_t kmalloc_caches[NR_SLAB_CACHES]; //Initialize 
//its basic_size cannot range from MIN_SIZE TO MAX_SIZE

uint64 get_cache_size(uint64 basic_size){
    if(basic_size<(1ull<<MIN_SIZE_SHIFT))   return (1ull<<MIN_SIZE_SHIFT);
    else if(basic_size>(1ull<<MAX_SIZE_SHIFT)){
        SLAB_TRACE("too large.recommend to alloc by buddy-system directly.\n");
        return -1;
    }
    uint64 align_sz=1ull<<i_log2(basic_size);
    if(align_sz != basic_size)  return align_sz<<1;
    return basic_size;
}

void cal_slab_order(uint16 basic_size, uint16 *limit, uint16 *page_order){
    if(basic_size < PGSIZE/8){
        *limit=PGSIZE/basic_size;
        *page_order=0;
        return;
    }
    int cur_order=0, support_max=3;
    int best_order=0;
    int min_waste=100, cur_waste;
    uint16 left_size=0;     //Must lower than basic_size, so uint16 is enough
    uint32 cur_size=0;
    for(;cur_order<=support_max;cur_order++){
        cur_size=1<<(cur_order+ORDER_BASE);
        if(cur_size < basic_size)   continue;
        left_size=cur_size % basic_size;
        cur_waste=left_size*100/cur_size;
        if(cur_waste<13){
            *limit=cur_size/basic_size;
            *page_order=cur_order;
            return;
        }
        //compare with the best
        if(cur_waste<min_waste){
            min_waste=cur_waste;
            best_order=cur_order;
        }
    }
    //None of them meet requirements,so select the best among them.
    if(basic_size<=(1ull<<(best_order+ORDER_BASE))){
        *limit=(1ull<<(best_order+ORDER_BASE))/basic_size;
        *page_order=best_order;
    }
    else{
        *limit=0;
        *page_order=-1;
        SLAB_TRACE("too large.recommend to alloc by buddy-system directly.\n");
    }
}

static int relink_locked(slab_page_t *slab, slab_page_t **old_list,
                         slab_page_t **new_list) {
    if (old_list == NULL || *old_list == NULL || new_list == NULL || old_list == new_list ||
        slab == NULL) {
        SLAB_TRACE("relink: Unexpected agrument!, slab=%p, new_list=%p, old_list=%p\n", slab, new_list,
               old_list);
        return -1;
    }
    //check if all three belong to the same pool(data consistent)
    // NOTE: Require no locks and carrier no risk of data races, 
    //         as cache pointer remains constant throughtout slab's lifecycle.
    if(slab->cache==NULL){
        SLAB_TRACE("relink: slab cache is NULL!\n");
        return -1;
    }
    if((*old_list!=NULL && (*old_list)->cache != slab->cache) ||
        (*new_list!=NULL && (*new_list)->cache!=slab->cache)){
        SLAB_TRACE("Error:these page_slab_headers originate from different pool!\n");
        return -1;
    }
    if (!holding(&slab->cache->pool_lock)) {
        panic("Race Conditions: Accessing list without lock!\n");
    }
    uint64 cnt1 = (*old_list)->inuse_count, cnt2 = slab->inuse_count;
    if (!((cnt1 == cnt2) ||
          (cnt1 > 0 && cnt2 > 0 && cnt1 < slab->cache->limit && cnt2 < slab->cache->limit))) {
        SLAB_TRACE("remove an unexisted entry from dismatch list\n");
        return -1;
    }
    slab_page_t *prev = PFN2SLAB(slab->prev_pfn);
    slab_page_t *next = PFN2SLAB(slab->next_pfn);
    if (prev != NULL) prev->next_pfn = SLAB2PFN(next);
    else
        *old_list = PFN2SLAB(slab->next_pfn);  // update the oldlist's header
    if (next != NULL) next->prev_pfn = SLAB2PFN(prev);
    // hang up to the new_list
    slab->next_pfn = SLAB2PFN(*new_list);
    slab->prev_pfn = 0;
    if (*new_list != NULL) (*new_list)->prev_pfn = SLAB2PFN(slab);
    *new_list = slab;
    return 0;
}

void init_slab_system(void){
    memset(&boot_cache, 0, sizeof(boot_cache));
    boot_cache.obj_size=sizeof(slab_cache_t);
    boot_cache.basic_size=ALIGN_UP(boot_cache.obj_size, 8);
    boot_cache.basic_size=get_cache_size(boot_cache.basic_size);
    cal_slab_order(boot_cache.basic_size, &boot_cache.limit, &boot_cache.page_order);
    if(boot_cache.limit==0 || boot_cache.page_order==-1){
        //Shouldn't fail, panic
        panic("slab_cache_t is too large to alloc required memory, adjust MAX_SIZE_SHIFT.\n");
        return;     //stop and return early
    }
    safestrcpy(boot_cache.name, "boot_cache", 32);
    initlock(&boot_cache.pool_lock, boot_cache.name);

    //initialize kmalloc_caches array for fixed size allocation(Dedicate Cache)
    memset(kmalloc_caches, 0, sizeof(kmalloc_caches));
    for(int i=0;i<NR_SLAB_CACHES;i++){
        kmalloc_caches[i].obj_size=1ull<<(MIN_SIZE_SHIFT+i);
        safestrcpy(kmalloc_caches[i].name, "kmalloc_caches", 32);
        kmalloc_caches[i].basic_size=kmalloc_caches[i].obj_size;
        cal_slab_order(kmalloc_caches[i].basic_size, &kmalloc_caches[i].limit, &kmalloc_caches[i].page_order);
        initlock(&kmalloc_caches[i].pool_lock, kmalloc_caches[i].name);
    }
}

struct slab_page *slab_refill(slab_cache_t *cache){  //Require lock held.
    //the current pool is empty, considering alloc new page,and return page_slab_header.
    if(cache==NULL){
        SLAB_TRACE("invalid cache.\n");
        return NULL;
    }
    if(!holding(&cache->pool_lock))
        panic("Race_condition.access slab_pool without lock!\n");
    if(cache->partial_list!=NULL || cache->empty_list!=NULL){
        SLAB_TRACE("Requesting allocation even when free page are available.\n");
        return NULL;
    }
    void *new_pool_mem=alloc_memory(1ull<<(cache->page_order+ORDER_BASE), GFP_ATOMIC | GFP_ZERO);
    if(new_pool_mem==NULL){
        SLAB_TRACE("slab_refill fail.\n");
        return NULL;
    }
    //Update struct page info, and record metadata off-page.
    struct page *new_page=get_page_desc_assert(paddr2pfn((uint64)new_pool_mem));
    //Post-allocation, pointer is exclusively head by current thread.(No lock!)
    for(int i=0;i<(1ull<<cache->page_order);i++){
        memset((void *)&(new_page+i)->u, 0, sizeof(new_page->u));
        page_set_type(new_page + i, PG_TYPE_SLAB);
        (new_page+i)->u.slab.cache=cache;
        (new_page+i)->u.slab.magic=SLAB_PAGE_MAGIC;
        (new_page+i)->u.slab.inuse_count=0;
    }

    void *cur_ptr=new_pool_mem;
    new_page->u.slab.freelist=cur_ptr;

    for(int i=0;i<cache->limit-1;i++){
        *(uint64 *)cur_ptr=(uint64)cur_ptr + cache->basic_size;
        cur_ptr=(void *)(*(uint64 *)cur_ptr);
    }
    *(void **)cur_ptr=NULL;
    if(cache->empty_list!=NULL){
        SLAB_TRACE("refill page when exist some unused pages.\n");
        new_page->u.slab.next_pfn=SLAB2PFN(cache->empty_list);

    }
    cache->empty_list=PAGE2SLAB(new_page);
    return PADDR2SLAB(new_pool_mem);
}

void *slab_alloc(slab_cache_t *cache){ 
    //also Accepts a function pointer to initialize newly created node,
    // or perform no operations if the pointer is null.(Require return zero if succeed.)
    if(cache==NULL){
        SLAB_TRACE("Invalid cache.\n");
        return NULL;
    }
    push_off();
    int cur_cpuid=cpuid();
    struct slab_cpu_cache *cpu_cache=&cache->cpu_caches[cur_cpuid];
    if(cpu_cache->avail>0){     //fast path
        void *ret=cpu_cache->obj[cpu_cache->avail-1];
        cpu_cache->obj[--cpu_cache->avail]=NULL;
        pop_off();  //Minimize Critical Section.Run time-consuming constructors after re-enabling.
        if(ret!=NULL)
            memset(ret, 0, cache->basic_size);      //clean on alloc
        if(ret!=NULL && cache->ctor!=NULL){ 
            //Object is now private:this thread is responsible for manual deallocation if initialization fails.
            if(cache->ctor(ret)!=0){
                SLAB_TRACE("initialize newly object fail.\n");
                slab_dealloc(ret);
                ret=NULL;
            }
        }
        return ret;
    }
    memset(cpu_cache->obj, 0, sizeof(void *)*PER_CPU_MAXSIZE);
    pop_off();

    //Slow path:Per-cpu cache exhausted;refilling.
    acquire(&cache->pool_lock);
    //Critical sections may be rescheduled to other CPUs, necessitating a refresh
    cur_cpuid=cpuid();
    cpu_cache=&cache->cpu_caches[cur_cpuid];
    //Double-check, the per-CPU cache might now contain available objects.
    if(cpu_cache->avail>0){
        void *ret=cpu_cache->obj[cpu_cache->avail-1];
        cpu_cache->obj[--cpu_cache->avail]=NULL;
        release(&cache->pool_lock);
        if(ret!=NULL)
            memset(ret, 0, cache->basic_size);  //clean on alloc
        //Reduce lock contention by invoking the costly constructor outside the critical section.
        if(cache->ctor!=NULL){
            if(cache->ctor(ret)!=0){
                SLAB_TRACE("initialize newly object fail.\n");
                slab_dealloc(ret);
                ret=NULL;
            }
        }
        return ret;
    }
    // Exhaustion: on objects available in the current cpu.
    struct slab_page *tmp=cache->partial_list;//Extract space from partial list
    struct slab_page *tmp_next=NULL;
    // (extract limit/4 elements each time from the list, 
    // if fewer than four remain, extract one page at most.
    uint16 batch_size=(cache->limit/4 <4)?cache->limit:(cache->limit/4), nr_done=0;
    batch_size=MIN(batch_size, PER_CPU_MAXSIZE/2);  //Refill half
    uint16 cur_avail=0;
    void *ret __attribute__((unused))=NULL;
    while(tmp!=NULL){
        //transit into the full_list(Defer the inuse_count update while in critical state.)
        cur_avail=cache->limit-tmp->inuse_count;
        tmp_next=PFN2SLAB(tmp->next_pfn);
        if(cur_avail<=batch_size-nr_done){
            if(relink_locked(tmp, &cache->partial_list, &cache->full_list)==-1){
                SLAB_TRACE("relink from partial_list to full_list: fail!\n");
                goto pick_from_array;
            }
        }
        uint16 cur_batch_size=MIN(batch_size-nr_done, cur_avail);
        for(int i=0;i<cur_batch_size;i++){
            cpu_cache->obj[i+nr_done]=tmp->freelist;
            tmp->freelist=*(void **)tmp->freelist;
            cpu_cache->avail++;
            tmp->inuse_count++;
        }
        if(tmp->inuse_count==cache->limit)
            tmp->freelist=NULL;    //Relocate pointers to valid address.
        nr_done+=cur_batch_size;
        if(nr_done==batch_size)
            goto pick_from_array;
        tmp=tmp_next;
    }
    //Still short of the target size, alloc new page.
    struct slab_page *new_slabpage=NULL;
    if(cache->empty_list!=NULL){
        new_slabpage=cache->empty_list;
    }
    else
        new_slabpage=slab_refill(cache);
    if(new_slabpage==NULL){
        SLAB_TRACE("failed to populate per-CPU array.return the existing element!\n");
        goto pick_from_array;
    }
    if(batch_size-nr_done<cache->limit){
        if(relink_locked(new_slabpage, &cache->empty_list, &cache->partial_list)==-1){
            SLAB_TRACE("relink from empty_list to partial_list: fail.\n");
            goto pick_from_array;
        }
    }
    else if(relink_locked(new_slabpage, &cache->empty_list, &cache->full_list)==-1){
        SLAB_TRACE("relink from empty_list to full_list: fail.\n");
        goto pick_from_array;
    }

    for(int i=0;i<batch_size-nr_done;i++){
        cpu_cache->obj[i+nr_done]=new_slabpage->freelist;
        new_slabpage->freelist=*(void **)new_slabpage->freelist;
        cpu_cache->avail++;
        new_slabpage->inuse_count++;
    }
pick_from_array:
    if(cpu_cache->avail>0){     //To avoid livelock, detach it from array directly.
        ret=cpu_cache->obj[cpu_cache->avail-1];
        cpu_cache->obj[--cpu_cache->avail]=NULL;
    }
    release(&cache->pool_lock);//Extract the object already.No lock needed now.
    if(ret!=NULL)
        memset(ret, 0, cache->basic_size);  //Clean-on-alloc
    if(ret!=NULL && cache->ctor!=NULL){
        if((cache->ctor)(ret)!=0){
            SLAB_TRACE("initialize newly object fail.\n");
            if(slab_dealloc(ret)!=0)
                panic("Failed to reclaim a object that failed initialization.\n");
            ret=NULL;
        }
    }
    return ret;
}

int slab_free(void *obj){
    //Use Destructor function pointer to destructor firstly.
    if(obj==NULL){
        SLAB_TRACE("free NULL.Invalid parameter\n");
        return 0;
    }
    slab_page_t *obj_page=PADDR2SLAB(obj);
    if(obj_page->cache->dtor!=NULL && obj_page->cache->dtor(obj)!=0){
        SLAB_TRACE("object destruction failed.\n");
        SLAB_TRACE("Forcing memory reclaimation to ensure system safety.\n");
    }
    return slab_dealloc(obj);
}

int slab_dealloc(void *del_obj){
    if(del_obj==NULL)  return 0;
    slab_page_t *obj_page=PADDR2SLAB(del_obj);
    if(obj_page->magic!=SLAB_PAGE_MAGIC){
        SLAB_TRACE("Reclaim an invalid slab_node, that allocator unrecognized\n");
        return -1;  //Before the lock is acquired, just return.
    }
    push_off();
    slab_cache_t *cache=obj_page->cache;
    int cur_cpuid=cpuid();
    struct slab_cpu_cache *cpu_cache=&cache->cpu_caches[cur_cpuid];
    if(cpu_cache->avail<PER_CPU_MAXSIZE){
        cpu_cache->obj[cpu_cache->avail++]=del_obj;
        pop_off();
        return 0;
    }
    pop_off();

    acquire(&cache->pool_lock);
    cur_cpuid=cpuid();
    cpu_cache=&cache->cpu_caches[cur_cpuid];    //update the cpu_caches
    if(cpu_cache->avail<PER_CPU_MAXSIZE){
        cpu_cache->obj[cpu_cache->avail++]=del_obj;
        release(&cache->pool_lock);
        return 0;
    }

    void *ret_obj=NULL;
    slab_page_t *assoc_page=NULL;
    for(int i=PER_CPU_MAXSIZE-1;i>=PER_CPU_MAXSIZE/2;i--){
        ret_obj=cpu_cache->obj[i];
        assoc_page=PADDR2SLAB(ret_obj);
        if(assoc_page->inuse_count==cache->limit &&
                relink_locked(assoc_page, &cache->full_list, &cache->partial_list)==-1)
            goto return_to_array;
        else if(assoc_page->inuse_count==1 &&
                relink_locked(assoc_page, &cache->partial_list, &cache->empty_list)==-1)
            goto return_to_array;
        assoc_page->inuse_count-=1;
        *(uint64 *)ret_obj=(uint64)assoc_page->freelist;
        assoc_page->freelist=ret_obj;
        cpu_cache->obj[i]=NULL;
        cpu_cache->avail--;
    }
    //consolidate memory pool shrinkage logic after returning PER_CPU_MAXSIZE/2 entries.
    uint64 nr_empty=0;
    slab_page_t *tmp=cache->empty_list;
    while(tmp!=NULL){
        nr_empty++;
        if(nr_empty>=POOL_LIMIT)   break;
        tmp=PFN2SLAB(tmp->next_pfn);
    }
    if(nr_empty>=POOL_LIMIT){  //shrink the pool if breaches the limit!
        tmp=cache->empty_list;
        slab_page_t *next=tmp;
        nr_empty=nr_empty/2;
        while(nr_empty>0){
            if(tmp==NULL){
                SLAB_TRACE("slab_dealloc: Inconsistent state transitions!");
                goto return_to_array;
            }
            next=PFN2SLAB(tmp->next_pfn);
            free_pages((void *)SLAB2PADDR(tmp), 1ull<<(cache->page_order+ORDER_BASE));
            nr_empty--;
            tmp=next;
        }
        cache->empty_list=tmp;
        if(tmp) tmp->prev_pfn=0;
    }
return_to_array:
    //Supposing the per-cpu buffer now have available capacity.
    if(cpu_cache->avail<PER_CPU_MAXSIZE){
        memset(del_obj, 0, cache->basic_size);
        cpu_cache->obj[cpu_cache->avail++]=del_obj;
    }
    else{   //Bypass the per-CPU cache and link directly to the target list.
        assoc_page=PADDR2SLAB(del_obj);
        if(assoc_page->inuse_count==cache->limit &&
                relink_locked(assoc_page, &cache->full_list, &cache->partial_list)==-1)
            panic("reclaim slab_object fail, unable to relink to partial_list.\n");
        else if(assoc_page->inuse_count==1 &&
                relink_locked(assoc_page, &cache->partial_list, &cache->empty_list)==-1)
            panic("reclaim slab_objectt fail, unable to relink to empty_list.\n");
        assoc_page->inuse_count-=1;
        memset(del_obj, 0, cache->basic_size);
        *(uint64 *)del_obj=(uint64)assoc_page->freelist;
        assoc_page->freelist=del_obj;
    }
    release(&cache->pool_lock);  //quit safely
    return 0;
}

slab_cache_t *create_slab_cache(char *name, uint16 size, uint16 align,
                                int (*ctor)(void *), int (*dtor)(void *)){
    slab_cache_t *new_slab=slab_alloc(&boot_cache);
    if(new_slab==NULL){
        SLAB_TRACE("alloc slab_cache fail.\n");
        return NULL;
    }
    if((align & (align-1))!=0){
        SLAB_TRACE("create_slab_cache: Invalid alignment, not the power of 2.\n");
        return NULL;
    }
    new_slab->obj_size=size;
    safestrcpy(new_slab->name, name, 32);
    new_slab->basic_size=ALIGN_UP(size, align);
    new_slab->basic_size=get_cache_size(new_slab->basic_size);
    cal_slab_order(new_slab->basic_size, &new_slab->limit, &new_slab->page_order);
    if(new_slab->limit==0 || new_slab->page_order==-1){
        SLAB_TRACE("too large.recommend to alloc by buddy-system directly.\n");
        return NULL;
    }
    new_slab->dtor=dtor;
    new_slab->ctor=ctor;
    initlock(&new_slab->pool_lock, new_slab->name);
    return new_slab;
}
