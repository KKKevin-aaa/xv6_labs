// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
// #define DEBUG_KALLOC
#ifdef DEBUG_KALLOC
#define KALLOC_TRACE(fmt, ...) \
    do { \
        printf("[KALLOC:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define KALLOC_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif
// Maximun size is 2^max_order*4KB, and PHYsize=128MB
// here we choose maximun size is 16MB, 
// and larger memory requirements can fulfilled by combining smaller components
#define SPEC_MASK(bits) (~((1ull << (bits)) -1 ))
#define SPEC_PAGESIZE(order) ( 1ull<<((order)+ORDER_BASE))
#define P_TYPE_MASK 0X8000
#define P_DATA_MASK 0x7ffe
#define P_FREE_MASK 0x0001
#define IS_HEAD(flags)      (((flags) & P_TYPE_MASK) != 0)
#define GET_ORDER(flags)    (((flags) & P_DATA_MASK) >> 1)
#define GET_OFFSET(flags)   (((flags) & P_DATA_MASK) >> 1)
#define IS_FREE(flags)      (((flags) & P_FREE_MASK) != 0)
#define W_HEAD_ORDER(p, order) do{  \
    uint16 _f=(p)->flags & P_FREE_MASK; \
    (p)->flags = _f | P_TYPE_MASK | (((order)<<1) & P_DATA_MASK); \
}while(0);
#define W_TAIL_OFFSET(p, offset) do{ \
    uint16 _f=(p)->flags & P_FREE_MASK; \
    (p)->flags = _f | (((offset)<<1) & P_DATA_MASK);  \
}while(0);
#define SET_FREE(p)         ((p)->flags |= P_FREE_MASK)
#define STE_ALLOC(p)        ((p)->flags &= ~P_FREE_MASK)
#define MAGIC_MERGED 0XFF
void freerange(void *pa_start, void *pa_end);
extern char end[];  // first address after kernel.
                    // defined by kernel.ld.
struct page{
    uint16 flags;   //If bit 15 is 1,record order, size is 2^(order + order_base)
    //To support partial deallocation, embed the head offset within order metadata.(bit 15 is 0)
    //which means the maximum num is 1ull<<14(>=MAX_Order), and last bit 0 indicate free or occuiped?
    struct page *next;
    struct page *prev;  //for delete node form list quickly
};
struct listhead{
    struct page *head;
};
struct {    //Anonymous structure(Single Pattern)
    struct spinlock lock;
    //Buddy system, minimum page size is 4096 bytes(4kB)
    struct page mem_bitmaps[512*64];    //Embedded Array
    struct listhead free_area[MAX_ORDER+1];
} kmem;
static void reset_flags_in_range(struct page *p, uint16 new_order, uint8 free){
    uint64 idx=1, size=1ull<<new_order;
    W_HEAD_ORDER(p, new_order);
    if(free)    SET_FREE(p);
    else    STE_ALLOC(p);
    while(idx<size){
        W_TAIL_OFFSET(p+idx, idx);
        if(free)    SET_FREE(p+idx);
        else    STE_ALLOC(p+idx);
        idx++;
    }
}
static void init_whole_area(){
    acquire(&kmem.lock);
    for(int i=0;i<MAX_ORDER+1;i++){
        kmem.free_area[i].head=NULL;
    }
    memset(kmem.mem_bitmaps, 0, sizeof(kmem.mem_bitmaps));
    struct page *tmp;
    for(uint64 idx=0;idx<512ull*64;idx+=1ull<<MAX_ORDER){
        tmp=&kmem.mem_bitmaps[idx];
        if(kmem.free_area[MAX_ORDER].head!=NULL)
            kmem.free_area[MAX_ORDER].head->prev=tmp;
        tmp->next=kmem.free_area[MAX_ORDER].head;
        kmem.free_area[MAX_ORDER].head=tmp;
        reset_flags_in_range(tmp, MAX_ORDER, 1);
    }
    release(&kmem.lock);
}
static void del_from_list_nolock(struct page *p, int order){    //Occupied
    struct page *prev=p->prev, *next=p->next;
    if(prev==NULL)
        kmem.free_area[order].head=next;
    else
        prev->next=next;
    if(next!=NULL)
        next->prev=prev;
    p->next=NULL;
    p->prev=NULL;
}
static void add_to_list_nolock(struct page *p, int order){  //free
    struct page *old_head=kmem.free_area[order].head;
    struct page *new_head=p;
    if(old_head!=NULL){
        new_head->next=old_head;
        old_head->prev=new_head;
    }
    kmem.free_area[order].head=new_head;
}
uint16 get_order(uint64 pa){
    pa=pa-KERNBASE;
    if(pa%PGSIZE!=0)
        panic("get order: Lookup unaligned address!");
    uint64 pa_idx=pa/PGSIZE;
    acquire(&kmem.lock);
    struct page *p=&kmem.mem_bitmaps[pa_idx];
    if(IS_HEAD((uint64)p->flags)==0)   panic("get order:try to get non-header's order!");
    uint16 tmp=GET_ORDER((uint64)p->flags);
    release(&kmem.lock);
    return tmp;
}
void *alloc_memory(uint64 size){
    //check first, should be 4kB-aligned
    //And it must be ensured that only a single page is allocated within alloc_memory.
    if(find_last_set(size & -size)<12){
        panic("alloc memory: should aligned with 4kB");
        return 0;
    }
    uint8 order=find_last_set(size-1)-11;
    struct page *tmp,*high_tmp;  //higher page
    uint64 step;
    acquire(&kmem.lock);
    if(order>MAX_ORDER){
        panic("out-of-memory!");
        return 0;
    }
    //In general cases, allocating a single is sufficient.
    uint8 split_order=order;uint64 offset;
    while(split_order<=MAX_ORDER && kmem.free_area[split_order].head==NULL){
        split_order++;
    }
    if(split_order>MAX_ORDER){
        release(&kmem.lock);
        panic("alloc_memory: out-of-memory!");
    }
    tmp=kmem.free_area[split_order].head;   //lower page
    high_tmp=tmp;  //higher page
    del_from_list_nolock(tmp, split_order);
    reset_flags_in_range(tmp, split_order, 0);
    while(split_order>order){
        //split into two blocks,both add into the lower level list
        split_order--;
        step=1ull<<split_order;
        high_tmp= tmp + step;
        add_to_list_nolock(tmp, split_order);
        reset_flags_in_range(tmp, split_order, 1);
        reset_flags_in_range(high_tmp, split_order, 0);
        tmp=high_tmp;
    }
    offset=(tmp-kmem.mem_bitmaps)*PGSIZE;
    release(&kmem.lock);
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) allocated size=%lx at pa=%p order=%d\n", 
            pid, name, size, (void *)(KERNBASE+offset), split_order);
#endif
    return (void *)(offset+KERNBASE);
}
void free_pages(void *pa, uint64 size){
    //When releasing an incomplete page, decompage it and splice the 
    //seperated fragments back into their respective slop.
    size=PGROUNDUP(size);
    uint16 cur_order, req_order;
    uint64 b_head_idx, b_tail_idx, b_buddy_idx;
    uint64 head_idx, mid_idx, tail_idx;
    struct page *b_head, *head, *b_buddy;
    b_head_idx=((uint64)pa-KERNBASE) / PGSIZE;
    acquire(&kmem.lock);
    b_head=&kmem.mem_bitmaps[b_head_idx];
    req_order=find_last_set(size-1)-11;
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) free size=%lx at pa=%p order=%d\n", 
            pid, name, size, pa, req_order);
#endif
    if(IS_HEAD((uint64)b_head->flags)==0){ //Not a block's header
        head_idx=b_head_idx-GET_OFFSET((uint64)b_head->flags);
        head=&kmem.mem_bitmaps[head_idx];
        cur_order=GET_ORDER((uint64)head->flags);

        tail_idx=b_head_idx+(1ull<<cur_order);
        mid_idx=(head_idx+tail_idx)/2;
        b_tail_idx=b_head_idx+(1ull<<req_order);
        while(cur_order>req_order){
            //"It is guaranteed that the desired sub-block can be 
            // obtained through repeated binary splitting
            cur_order--;
            reset_flags_in_range(&kmem.mem_bitmaps[head_idx], cur_order, 0);
            reset_flags_in_range(&kmem.mem_bitmaps[mid_idx], cur_order, 0);
            if(mid_idx>b_head_idx)  //block locate on the left half
                tail_idx=mid_idx;
            else
                head_idx=mid_idx;   //block locate on the right half
            mid_idx=(head_idx+tail_idx)/2;
        }
        //breakout form loop, must check pa is new block's header,size="size"
        if(head_idx!=b_head_idx || tail_idx!=b_tail_idx)
            panic("free_page: fail!");
        reset_flags_in_range(b_head, req_order, 1);
    }
    //Attempt to merge current block woth its adjacent block to reduce fragmentation.
    while(req_order<MAX_ORDER){
        b_buddy_idx= b_head_idx ^ (1ull<<req_order);
        if(b_buddy_idx>=512*64) break;;
        b_buddy=&kmem.mem_bitmaps[b_buddy_idx];
        if(!IS_FREE((uint64)b_buddy->flags) || GET_ORDER((uint64)b_buddy->flags)!=req_order)
            break;
        //remove current_page form the correspond free_list
        del_from_list_nolock(b_buddy, req_order);
        b_head_idx=b_buddy_idx & b_head_idx;    //update for the next loop
        req_order++;
    }
    //concatenation
    add_to_list_nolock(&kmem.mem_bitmaps[b_head_idx], req_order);
    reset_flags_in_range(&kmem.mem_bitmaps[b_head_idx], req_order, 1);
    release(&kmem.lock);
    // uint64 mem_idx=((uint64)(pa)-KERNBASE) / PGSIZE, buddy_idx;
    // acquire(&kmem.lock);
    // struct page *cur_page=&kmem.mem_bitmaps[mem_idx], *buddy_page;
    // uint16 cur_order=ORDER_MASK(cur_page->flags);
    // if(cur_order== MAGIC_MERGED){
    //     // while(1);
    //     panic("Kfree:trying to free a middle chunk!");
    // }
    // if(FREE_MASK(cur_page->flags))
    //     panic("double free!");
    // while(cur_order<MAX_ORDER){
    //     buddy_idx=mem_idx ^ (1ull << cur_order);
    //     if(buddy_idx>=512*64)   break;
    //     buddy_page=&kmem.mem_bitmaps[buddy_idx];
    //     if(!FREE_MASK(buddy_page->flags) || ORDER_MASK(buddy_page->flags)!=cur_order)
    //         break;  //stop 
    //     //remove current_page form the correspond free_list
    //     del_form_list_nolock(buddy_page, cur_order);
    //     mem_idx=buddy_idx & mem_idx;    //update for the next loop
    //     cur_order++;
    // }
    // //concatenation
    // add_to_list_nolock(&kmem.mem_bitmaps[mem_idx], cur_order);
    // release(&kmem.lock);
}
int is_all_same_swar(const uint8 *data, uint64 len){
    if(len==0)  return 1;
    uint8 ref=data[0];
    uint64 pattern=ref;
    pattern |= pattern<<8;
    pattern |= pattern<<16;
    pattern |= pattern<<32;
    const uint8 *ptr=data;
    while(((uint64)ptr & 0x7)!=0 && len >0){    //deal with prelog
        if(*(uint8 *)ptr!=ref)  return 0;
        len--;ptr++;
    }
    const uint64 *ptr64=(uint64 *)ptr;
    while(len>=8){
        if(*ptr64!=pattern) 
            return 0;
        ptr64++;
    }
    ptr=(uint8 *)ptr64; //deal with epilog
    while(len >0){
        if(*ptr!=pattern)
            return 0;
        len--;ptr++;
    }
    return 1;
}
void kinit() {
#ifdef DEBUG_KALLOC
    KALLOC_TRACE("initializing memory allocator\n");
#endif
    initlock(&kmem.lock, "kmem");
    init_whole_area();
#ifdef DEBUG_KALLOC
    KALLOC_TRACE("initialization complete\n");
#endif
}
void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
        kfree(p);
    }
}
// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) freeing pa=%p\n", pid, name, pa);
#endif
    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");
#ifndef LAB_SYSCALL
    memset(pa, 1, PGSIZE);
#endif
    free_pages(pa, PGSIZE);
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) requesting PGSIZE\n", pid, name);
#endif
    return alloc_memory(PGSIZE);
}