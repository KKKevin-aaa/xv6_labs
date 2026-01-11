#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "slab.h"

//size-special pool
#define MIN_SIZE 32
#define MAX_SIZE PGSIZE/4
#define SIZE_STRIDE 32
#define NR_SIZE ((MAX_SIZE-MIN_SIZE)/SIZE_STRIDE +1 )
#define ALIGN_UP(a, size)   (((a) + (size) -1) & ~((size) -1))
#define PAGE_SLAB_HEADER_MAGIC  0x32142361
#define POOL_LIMIT 5

static slab_cache_t boot_cache;
static slab_cache_t *size_cache[NR_SIZE+1]; //the first cache is for cache_cache,
//its basic_size cannot range from MIN_SIZE TO MAX_SIZE

static int relink_locked(page_slab_header_t *slab, page_slab_header_t **old_list,
                         page_slab_header_t **new_list) {
    if (old_list == NULL || *old_list == NULL || new_list == NULL || old_list == new_list ||
        slab == NULL) {
        printf("relink: Unexpected agrument!, slab=%p, new_list=%p, old_list=%p\n", slab, new_list,
               old_list);
        return -1;
    }
    //check if all three belong to the same pool(data consistent)
    // NOTE: Require no locks and carrier no risk of data races, 
    //         as cache pointer remains constant throughtout slab's lifecycle.
    if((*old_list)->cache != (*new_list)->cache || slab->cache==NULL ||
        (*new_list!=NULL && (*new_list)->cache!=slab->cache)){
        printf("Error:these page_slab_headers originate from different pool!\n");
        return -1;
    }
    if (!holding(&slab->cache->pool_lock)) {
        panic("Race Conditions: Accessing list without lock!\n");
    }
    uint64 cnt1 = (*old_list)->inuse_count, cnt2 = slab->inuse_count;
    if (!((cnt1 == cnt2) ||
          (cnt1 > 0 && cnt2 > 0 && cnt1 < slab->cache->limit && cnt2 < slab->cache->limit))) {
        printf("remove an unexisted entry from dismatch list\n");
        return -1;
    }
    page_slab_header_t *prev = slab->prev_page;
    page_slab_header_t *next = slab->next_page;
    if (prev != NULL) prev->next_page = next;
    else
        *old_list = slab->next_page;  // update the oldlist's header
    if (next != NULL) next->prev_page = prev;
    // hang up to the new_list
    slab->next_page = *new_list;
    slab->prev_page = NULL;
    if (*new_list != NULL) (*new_list)->prev_page = slab;
    *new_list = slab;
    return 0;
}

void init_slab_system(void){
    memset(&boot_cache, 0, sizeof(boot_cache));
    boot_cache.align=8;
    boot_cache.obj_size=sizeof(slab_cache_t);
    boot_cache.basic_size=ALIGN_UP(boot_cache.basic_size, SIZE_STRIDE);
    safestrcpy(boot_cache.name, "boot_cache", 32);
    initlock(&boot_cache.pool_lock, boot_cache.name);
    //calcalute the limit
    boot_cache.limit=PGSIZE/boot_cache.basic_size -1;
}

void *slab_refill(slab_cache_t *cache){  //Require lock held.
    //the current pool is empty, considering alloc new page
    if(cache==NULL){
        printf("invalid cache.\n");
        return NULL;
    }
    if(!holding(&cache->pool_lock))
        panic("Race_condition.access slab_pool without lock!\n");
    if(cache->partial_list!=NULL || cache->empty_list!=NULL){
        printf("Requesting allocation even when free page are available.\n");
        return NULL;
    }
    void *new_pool_mem=alloc_memory(PGSIZE);
    if(new_pool_mem==NULL){
        printf("slab_refill fail.\n");
        return NULL;
    }
    memset(new_pool_mem, 0, PGSIZE);
    //Embed the management structure at the start of the page.
    page_slab_header_t *new_header=(page_slab_header_t *)new_pool_mem;
    new_header->magic=PAGE_SLAB_HEADER_MAGIC;
    new_header->inuse_count=1;
    new_header->cache=cache;
    uint64 start_addr=cache->basic_size;
    void *cur_ptr=(void *)start_addr;
    for(int i=0;i<cache->limit;i++){
        *(uint64 *)cur_ptr=(uint64)cur_ptr + cache->basic_size;
        cur_ptr=(void *)(*(uint64 *)cur_ptr);
        if(i==0)    new_header->freelist_head=cur_ptr;
    }
    *(uint64 *)cur_ptr=0;
    cache->partial_list=new_header;
    return start_addr;
}

void *slab_alloc(slab_cache_t *cache){
    if(cache==NULL){
        printf("Invalid cache.\n");
        return NULL;
    }
    //FIXME: 
    push_off();
    int cur_cpuid=cpuid();
    struct slab_cpu_cache cur_cpu_cache=cache->cpu_caches[cur_cpuid];
    if(cur_cpu_cache.avail>0){
        void *ret=cur_cpu_cache.objects[--cur_cpu_cache.avail];
        pop_off();
        return ret;
    }
    if(!holding(&cache->pool_lock)){
        panic("Race condition: access pool without lock.\n");
        return NULL;
    }
    page_slab_header_t *tmp=cache->partial_list;//Extract space from partial list
    while(tmp!=NULL){
        if(tmp->inuse_count==cache->limit){
            tmp=tmp->next_page;
        }
        else{
            if(tmp->inuse_count==cache->limit-1){
                //transit into the full_list(Defer the inuse_count update while in critical state.)
                if(tmp->freelist_head==NULL)
                    panic("slab_header metadata inconsisent!\n");
                if(relink_locked(tmp, &cache->partial_list, &cache->full_list)==-1){
                    printf("alloc_vma_node : fail!\n");
                    release(&cache->pool_lock);
                    return NULL;
                }
            }
            void *ret_vma=tmp->freelist_head;
            tmp->freelist_head=*(uint64 *)tmp->freelist_head;
            tmp->inuse_count++; //update at the end.
            //clear and Scrub all data
            memset(ret_vma, 0, cache->obj_size);
            return ret_vma;
        }
    }
    return slab_refill(cache);    //Not found,return.
}

int slab_dealloc(void *node){
    if(node==NULL)  return -1;
    page_slab_header_t *cur_page=(page_slab_header_t *)((uint64)node & ~(PGSIZE -1));
    if(cur_page->magic!=PAGE_SLAB_HEADER_MAGIC){
        printf("Reclaim an invalid slab_node, that allocator unrecognized\n");
        return -1;  //Before the lock is acquired, just return.
    }
    acquire(&cur_page->cache->pool_lock);
    if(cur_page->inuse_count==cur_page->cache->limit){
        if(relink_locked(cur_page, cur_page->cache->full_list, &cur_page->cache->partial_list)==-1)
            goto cleanup;
        memset(node, 0, cur_page->cache->basic_size);
        cur_page->inuse_count-=1;
        *(uint64 *)node=(uint64)cur_page->freelist_head;
        cur_page->freelist_head=node;
    }
    else if(cur_page->inuse_count==1){
        if(relink_locked(cur_page, cur_page->cache->partial_list, cur_page->cache->empty_list)==-1)
            goto cleanup;
        cur_page->inuse_count=0;
        memset(node, 0, cur_page->cache->basic_size);
        *(uint64 *)node=(uint64)cur_page->freelist_head;
        cur_page->freelist_head=node;
        uint64 count=0;
        page_slab_header_t *tmp=cur_page->cache->empty_list;
        while(tmp!=NULL){
            count++;
            if(count>=POOL_LIMIT)   break;
            tmp=tmp->next_page;
        }
        if(count>=POOL_LIMIT){  //shrink the pool if breaches the limit!
            tmp=cur_page->cache->empty_list;
            page_slab_header_t *next=tmp;
            count=count/2;
            while(count>0){
                if(tmp==NULL){
                    printf("slab_dealloc: Inconsistent state transitions!");
                    goto cleanup;
                }
                next=tmp->next_page;
                memset((void *)tmp, 0, PGSIZE);
                free_pages(tmp, PGSIZE);
                count--;
                tmp=next;
            }
            cur_page->cache->empty_list=tmp;
            if(tmp) tmp->prev_page=NULL;
        }
    }
    else{   //No list transition.
        cur_page->inuse_count-=1;
        memset(node, 0, cur_page->cache->basic_size);
        *(uint64 *)node=(uint64)cur_page->freelist_head;
        cur_page->freelist_head=node;
    }
    release(&cur_page->cache->pool_lock);
    return 0;
cleanup:
    release(&cur_page->cache->pool_lock);
    return -1;
}

slab_cache_t *create_slab_cache(char *name, uint16 size, uint16 align){
    acquire(&boot_cache.pool_lock);
    slab_cache_t *new_slab=slab_alloc(&boot_cache);
    if(new_slab==NULL){
        printf("alloc slab_cache fail.\n");
        return NULL;
    }
    if((align & (align-1))!=0){
        printf("create_slab_cache: Invalid alignment, not the power of 2.\n");
        return NULL;
    }
    new_slab->align=align;
    new_slab->obj_size=size;
    safestrcpy(new_slab->name, name, 32);
    new_slab->basic_size=ALIGN_UP(size, align);
    if(new_slab->basic_size>MAX_SIZE){
        printf("current slab_cache basic-size is %d\n", new_slab->basic_size);
        printf("It is truly necessary to employ a slab  \
                allocator for such a relatively large size??\n");
    }
    if(new_slab->basic_size % SIZE_STRIDE !=0){
        new_slab->basic_size=ALIGN_UP(new_slab->basic_size, SIZE_STRIDE);
    }
    uint64 start_addr=ALIGN_UP(sizeof(page_slab_header_t), new_slab->basic_size);
    new_slab->limit=(PGSIZE-start_addr)/new_slab->basic_size;
    initlock(&new_slab->pool_lock, new_slab->name);
    release(&boot_cache.pool_lock);
    return new_slab;
}
