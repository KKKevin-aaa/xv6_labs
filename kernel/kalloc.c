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
// Maximun size is 2^max_order*4KB, and PHYsize=128MB
// here we choose maximun size is 16MB, 
// and larger memory requirements can fulfilled by combining smaller components
#define SPEC_MASK(bits) (~(uint64)(1ull << (bits) -1 ))
#define SPEC_PAGESIZE(order) ( 1ull<<((order)+ORDER_BASE))
#define ORDER_MASK(flags) (((uint16)(flags) & 0xff00) >> 8)
#define FREE_MASK(flags) ((uint16)(flags) & 0x00ff)
#define MAGIC_MERGED 0XFF
void freerange(void *pa_start, void *pa_end);
extern char end[];  // first address after kernel.
                    // defined by kernel.ld.
struct page{
    uint16 flags;   //(15-8bits) record order, size is 2^(order + order_base)
    //and while for 7-0 bits store free or allocated?
    struct page *next;
    struct page *prev;  //for delete node form list quickly
};
struct listhead{
    struct page *head;
};
struct {    //Anonymous structure(Single Pattern)
    struct spinlock lock;
    //Buddy system, minimum page size is 4096 bytes(4kB)
    struct page mem_bitmaps[512*64];
    struct listhead free_area[MAX_ORDER+1];
} kmem;

static void init_whole_area(){
    acquire(&kmem.lock);
    for(int i=0;i<MAX_ORDER+1;i++){
        kmem.free_area[i].head=NULL;
    }
    for(int i=0;i<512*64;i++){
        kmem.mem_bitmaps[i].flags=MAGIC_MERGED << 8;   //magic_merged and Occupied(only header is free)
        kmem.mem_bitmaps[i].next=NULL;
    }
    struct page *tmp;
    for(uint64 idx=0;idx<512ull*64;idx+=1ull<<MAX_ORDER){
        tmp=&kmem.mem_bitmaps[idx];
        if(kmem.free_area[MAX_ORDER].head!=NULL)
            kmem.free_area[MAX_ORDER].head->prev=tmp;
        tmp->next=kmem.free_area[MAX_ORDER].head;
        kmem.free_area[MAX_ORDER].head=tmp;
        tmp->flags=(MAX_ORDER << 8 | 0x1);  //max_order and free('Cause header)
    }
    release(&kmem.lock);
}
static void del_form_list_nolock(struct page *p, int order){
    struct page *prev=p->prev, *next=p->next;
    if(prev==NULL)
        kmem.free_area[order].head=next;
    else
        prev->next=next;
    if(next!=NULL)
        next->prev=prev;
    p->next=NULL;
    p->prev=NULL;
    p->flags=MAGIC_MERGED << 8;
}
static void add_to_list_nolock(struct page *p, int order){
    struct page *old_head=kmem.free_area[order].head;
    struct page *new_head=p;
    if(old_head!=NULL){
        new_head->next=old_head;
        old_head->prev=new_head;
    }
    kmem.free_area[order].head=new_head;
    new_head->flags=(order << 8 | 0x1);
}
uint16 get_order(uint64 pa){
    pa=pa-KERNBASE;
    if(pa%PGSIZE!=0)
        panic("get order: Lookup unaligned address!");
    uint64 pa_idx=pa/PGSIZE;
    acquire(&kmem.lock);
    uint16 tmp=ORDER_MASK(kmem.mem_bitmaps[pa_idx].flags);
    release(&kmem.lock);
    return tmp;
}
void free_pages(void *pa){
    uint64 mem_idx=((uint64)(pa)-KERNBASE) / PGSIZE, buddy_idx;
    acquire(&kmem.lock);
    struct page *cur_page=&kmem.mem_bitmaps[mem_idx], *buddy_page;
    uint16 cur_order=ORDER_MASK(cur_page->flags);
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    printf("[KALLOC] free_pages: pid=%d(%s) freeing pa=%p order=%d\n", pid, name, (void *)pa, cur_order);
#endif
    if(cur_order== MAGIC_MERGED){
        // while(1);
        panic("Kfree:trying to free a middle chunk!");
    }
    if(FREE_MASK(cur_page->flags))
        panic("double free!");
    //should and always be chunk_header
    while(cur_order<MAX_ORDER){
        buddy_idx=mem_idx ^ (1ull << cur_order);
        if(buddy_idx>=512*64)   break;
        buddy_page=&kmem.mem_bitmaps[buddy_idx];
        if(!FREE_MASK(buddy_page->flags) || ORDER_MASK(buddy_page->flags)!=cur_order)
            break;  //stop 
        //remove current_page form the correspond free_list
        del_form_list_nolock(buddy_page, cur_order);
        mem_idx=buddy_idx & mem_idx;    //update for the next loop
        cur_order++;
    }
    //concatenation
    add_to_list_nolock(&kmem.mem_bitmaps[mem_idx], cur_order);
    release(&kmem.lock);
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
    printf("[KALLOC] kinit: initializing memory allocator\n");
#endif
    initlock(&kmem.lock, "kmem");
    init_whole_area();
#ifdef DEBUG_KALLOC
    printf("[KALLOC] kinit: initialization complete\n");
#endif
}
void *alloc_memory(uint64 size){
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    printf("[KALLOC] alloc_memory: pid=%d(%s) requesting size=%lx\n", pid, name, size);
#endif
    //check first, should be 4kB-aligned
    //And it must be ensured that only a single page is allocated within alloc_memory.
    if(find_last_set(size & -size)<12){
        panic("alloc memory: should aligned with 4kB");
        return 0;
    }
    uint8 order=find_last_set(size-1)-11;
    struct page *tmp;
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
    struct page *high_tmp=tmp;  //higher page
    del_form_list_nolock(tmp, split_order);
    while(split_order>order){
        //split into two blocks,both add into the lower level list
        split_order--;
        high_tmp= tmp + (1ull<<split_order);
        high_tmp->flags=split_order << 8;
        //tmp's flags will be set in add_to_list_nolock functions
        add_to_list_nolock(tmp, split_order);
        tmp=high_tmp;
    }
    high_tmp->flags=split_order << 8;   //check order and free state
    offset=(tmp-kmem.mem_bitmaps)*PGSIZE;
    release(&kmem.lock);
#ifdef DEBUG_KALLOC
    printf("[KALLOC] alloc_memory: pid=%d(%s) allocated size=%lx at pa=%p order=%d\n", 
           pid, name, size, (void *)(KERNBASE+offset), split_order);
#endif
    // if(KERNBASE+offset == 0x87fb2000)   while(1);
    return (void *)(offset+KERNBASE);
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
    printf("[KALLOC] kfree: pid=%d(%s) freeing pa=%p\n", pid, name, pa);
#endif
    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");
#ifndef LAB_SYSCALL
    memset(pa, 1, PGSIZE);
#endif
    free_pages(pa);
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    printf("[KALLOC] kalloc: pid=%d(%s) requesting PGSIZE\n", pid, name);
#endif
    return alloc_memory(PGSIZE);
}