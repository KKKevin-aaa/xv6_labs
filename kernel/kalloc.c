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
void dump_memory_map();     //Called with OOM error panic.
void swap_out(void);
void swap_in(void);
struct spinlock swap_lock;  //A logic lock protecting the "need_swap" signal 
// and sleep/wake automicity for brief duration.Seperate from "kmem.lock" to avoid scheduling latency.
extern struct spinlock rmap_lock;
static struct proc *swap_kthread=NULL;

// Maximun size is 2^max_order*4KB, and PHYsize=128MB
// here we choose maximun size is 16MB, 
// and larger memory requirements can fulfilled by combining smaller components
#define SPEC_MASK(bits) (~((1ull << (bits)) -1 ))
#define SPEC_PAGESIZE(order) ( 1ull<<((order)+ORDER_BASE))
#define P_TYPE_MASK 0X8000000000000000ULL
#define P_TYPE_HEAD P_TYPE_MASK
#define P_TYPE_TAIL 0
#define P_DATA_MASK 0x7ffffffffffffffeULL
#define P_FREE_MASK 0x0000000000000001ULL
#define IS_HEAD(flags)      (((flags) & P_TYPE_MASK) != 0)
#define GET_ORDER(flags)    (((flags) & P_DATA_MASK) >> 1)
#define GET_OFFSET(flags)   (((flags) & P_DATA_MASK) >> 1)
#define IS_FREE(flags)      (((flags) & P_FREE_MASK) != 0)
#define PA2PAGE_SAFE(pa)    get_page_desc_safe(paddr_to_pfn((uint64)(pa)))
#define PA2PAGE_ASSERT(pa)    get_page_desc_assert(paddr_to_pfn((uint64)(pa)))
uint64 total_pages;
uint64 free_start_addr;
uint64 start_pfn;   //NOTE:start from "start_pfn" instead of 0
extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

extern res_block rb_array[MAX_RES_BLOCK];
//NOTE: ----------SELF-DESCRIBING PHYSICAL PAGES---------------------------------
struct page{
    uint64 flags;   //If bit 63 is 1:record order, size is 2^(order + ORDER_BASE)
    //To support partial deallocation, embed the head offset within order metadata.(bit 63 is 0)
    //which means the maximum num is 1ull<<62(>MAX_Order), and last bit 0 indicate free or occuiped?

    void *rmapping; //Reverse mapping to virtual address.
    //bit 0==0:point to struct address_sapce, bit 0==1:point to struct anon_vma.
    uint64 index;

    struct page *next;
    struct page *prev;  //for delete node form list quickly
};
struct listhead{
    struct page *head;
};
struct {    //Anonymous structure(Single Pattern)
    struct spinlock lock;   //NOTE: Principle--acquire, get next free frame, release
    //Buddy system, minimum page size is 4096 bytes(4kB)
    struct page *mem_bitmaps;    //Embedded Array
    struct listhead free_area[MAX_ORDER+1];
} kmem;
static void free_pages_nolock(void *pa, uint64 size);

inline void sync_rmap(void *p, uint64 size, pte_t *pte){
    if(!holding(&rmap_lock))
        panic("sync_ramp without lock.\n");
    
}

static inline void w_head_order(struct page *p, uint64 order){
    uint64 existing_flag= p->flags & ~(P_DATA_MASK | P_TYPE_MASK);
    uint64 new_val = P_TYPE_HEAD | ((order<<1) & P_DATA_MASK);
    p->flags = existing_flag | new_val;
}

static inline void w_tail_offset(struct page *p, uint64 offset){
    uint64 existing_flag= p->flags & ~(P_DATA_MASK | P_TYPE_MASK);
    uint64 new_val = P_TYPE_TAIL| ((offset<<1) & P_DATA_MASK);
    p->flags = existing_flag | new_val;
}

static inline void set_free(struct page * p){
    p->flags |= P_FREE_MASK;
}

static inline void set_alloc(struct page *p){
    //Syntax Error: function body must be a compound statement
    p->flags &= ~P_FREE_MASK;
}

static inline uint64 paddr_to_pfn(uint64 pa){    //paddr convert to Page Frame Number
    if(pa%PGSIZE!=0)    panic("paddr_to_pfn: unaligned!");
    if(pa<KERNBASE || pa>=PHYSTOP)  panic("paddr_to_pfn, out of range");
    return (pa-KERNBASE)/PGSIZE;
}

//Defensive Programming, provide two interface(must exist and try get)
static inline uint64 pfn_to_paddr(uint64 pfn){
    if(pfn>total_pages)    panic("pfn_to_paddr: Segment fault");
    return (pfn*PGSIZE + KERNBASE);
}

static inline void ensure_pfn_valid(uint64 pfn){
    if(pfn < start_pfn || pfn >= total_pages){
        KALLOC_TRACE("PMM error:Access pfn 0x%llx out-of-range[0x%llx, 0x%llx)",
            pfn, start_pfn, total_pages);
        panic("pfn invalid!");
    }
}

static inline struct page *get_page_desc_safe(uint64 pfn){
    if(pfn < start_pfn || pfn >= total_pages)
        return NULL;
    return &kmem.mem_bitmaps[pfn];
}

static inline struct page *get_page_desc_assert(uint64 pfn){
    ensure_pfn_valid(pfn);
    return &kmem.mem_bitmaps[pfn];
}

//NOTE: all recorded order is relative to ORDER_BASE
static void blocks_flags_reset(struct page *p, uint64 new_order, uint8 free){
    // Caller ensures p validity!
    uint64 pfn=1, size=1ull<<new_order;
    uint64 p_pfn=p-kmem.mem_bitmaps;
    ensure_pfn_valid(p_pfn+size-1);  //check if last is valid
    //the range [p_pfn, p_pfn+size-1) pass the check!
    w_head_order(p, new_order);
    if(free)    set_free(p);
    else    set_alloc(p);
    while(pfn<size){
        w_tail_offset(p+pfn, pfn);
        if(free)    set_free(p+pfn);
        else    set_alloc(p+pfn);
        pfn++;
    }
}

static void init_whole_area(uint64 end_addr, uint64 maximum_addr){
    acquire(&kmem.lock);
    end_addr = PGROUNDUP(end_addr);
    maximum_addr=PGROUNDDOWN(maximum_addr);
    total_pages = (maximum_addr - KERNBASE) / PGSIZE;
    //fpn0 should always corresponds to the absolute physical base address
    kmem.mem_bitmaps=(struct page *)end_addr;
    free_start_addr = end_addr + sizeof(struct page)*total_pages;
    free_start_addr=PGROUNDUP(free_start_addr);
    //Important!Bootstrapping, solve by the cost of wasting some array element
    total_pages= (maximum_addr - free_start_addr)/PGSIZE;
    //Skip the text,rodata segment of the program, start from the free_start_addr
    start_pfn=paddr_to_pfn(free_start_addr);
    memset(kmem.mem_bitmaps, 0, total_pages*sizeof(struct page));
    for(int i=0;i<total_pages;i++){ //Set the reversed area(red zone)
        w_head_order(&kmem.mem_bitmaps[i], 0);
        set_alloc(&kmem.mem_bitmaps[i]);
    }
    for(int i=0;i<MAX_ORDER+1;i++){
        kmem.free_area[i].head=NULL;
    }
    for(int i=start_pfn;i<total_pages;i++){
        free_pages_nolock((void *)pfn_to_paddr(i), PGSIZE);
    }
    release(&kmem.lock);
}
//Ensure the element in the list are valid(in the range[start_pfn ,total_size))

static void del_from_list_nolock(struct page *p, uint64 order){    //Occupied
    struct page *prev=p->prev, *next=p->next;
    //check first!
    uint64 p_pfn=(uint64)(p-kmem.mem_bitmaps);
    ensure_pfn_valid(p_pfn+(1ull<<order)-1);
    if(prev==NULL)
        kmem.free_area[order].head=next;
    else
        prev->next=next;
    if(next!=NULL)
        next->prev=prev;
    p->next=NULL;
    p->prev=NULL;
    //check if this block's oreder is expected
    if(GET_ORDER(p->flags)!=order){
        KALLOC_TRACE("try to delete Order.%llu block list while this block is Order.%llu \n",
            order, GET_ORDER(p->flags));
        panic("del_form_list");
    }
}

static void add_to_list_nolock(struct page *p, uint64 order){  //free
    uint64 p_pfn=p-kmem.mem_bitmaps;
    ensure_pfn_valid(p_pfn+(1ull<<order)-1);
    struct page *old_head=kmem.free_area[order].head;
    struct page *new_head=p;
    if(old_head!=NULL){
        new_head->next=old_head;
        old_head->prev=new_head;
    }
    kmem.free_area[order].head=new_head;
    if(GET_ORDER(p->flags)!=order){
        KALLOC_TRACE("try to add to Order.%llu block list, while this block is Order.%llu\n",
            order, GET_ORDER(p->flags));
        panic("add_to_list");
    }
}

uint64 get_order(uint64 pa){
    if(pa%PGSIZE!=0)
        panic("get order: Lookup unaligned address!");
    acquire(&kmem.lock);
    struct page *p=PA2PAGE_ASSERT(pa);
    uint64 tmp;
    if(IS_HEAD((uint64)p->flags)==0)    tmp=0;
    else    tmp=GET_ORDER((uint64)p->flags);
    // KALLOC_TRACE("flags=0x%llx, pa=0x%llx, NO.0x%llx, head=NO.0x%llx\n",
    //     (uint64)p->flags, pa, pa_pfn, pa_pfn-GET_OFFSET(p->flags));
    // panic("get order:try to get non-header's order!");
    release(&kmem.lock);
    return tmp;
}

uint8 is_head(uint64 pa){
    if(pa%PGSIZE!=0)
    panic("get order: Lookup unaligned address!");
    acquire(&kmem.lock);
    struct page *p=PA2PAGE_ASSERT(pa);
    uint8 ret=IS_HEAD((uint64)p->flags);
    release(&kmem.lock);
    return ret;
}



void print_memorytable(){   //with lock
    acquire(&kmem.lock);
    release(&kmem.lock);
}

void *alloc_memory(uint64 size){
    //check first, should be 4kB-aligned
    //And it must be ensured that only a single page is allocated within alloc_memory.
    if(i_log2(size & -size)<12){
        dump_memory_map();
        panic("alloc memory: should aligned with 4kB");
        return NULL;
    }
    uint8 order=i_log2(size-1)-11;
    if(size != (1ull<<(order+ORDER_BASE))){   //size must be a valid set member.No internal splitting performed.
        KALLOC_TRACE("alloc memory: allocation size is not basic unit supported!");
        panic("alloc_memory");
    }
    struct page *tmp,*high_tmp;  //higher page
    uint64 step;
    acquire(&kmem.lock);
    if(order>MAX_ORDER){
        acquire(&swap_lock);
        wakeup((void *)&swap_kthread);
        release(&kmem.lock);
        sleep((void *)&kmem, &swap_lock);
        acquire(&kmem.lock);
        release(&swap_lock);
        return NULL;
    }
    //In general cases, allocating a single is sufficient.
    uint8 split_order=order;uint64 offset;
    while(split_order<=MAX_ORDER && kmem.free_area[split_order].head==NULL){
        split_order++;
    }
    if(split_order>MAX_ORDER){
        release(&kmem.lock);
        dump_memory_map();
        wakeup((void *)&kmem);
        // panic("alloc_memory: out-of-memory!");
    }
    tmp=kmem.free_area[split_order].head;   //lower page
    high_tmp=tmp;  //higher page
    blocks_flags_reset(tmp, split_order, 0);
    del_from_list_nolock(tmp, split_order);
    uint64 tmp_pfn=(uint64)(tmp-kmem.mem_bitmaps);
    while(split_order>order){
        //split into two blocks,both add into the lower level list
        split_order--;
        step=1ull<<split_order;
        high_tmp=get_page_desc_assert(tmp_pfn+step);
        blocks_flags_reset(tmp, split_order, 1);
        blocks_flags_reset(high_tmp, split_order, 0);
        add_to_list_nolock(tmp, split_order);
        tmp=high_tmp;
        tmp_pfn+=step;  //update tmp_pfn;
    }
    offset=(tmp-kmem.mem_bitmaps)*PGSIZE;
    release(&kmem.lock);
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) allocated size=%llx at pa=%p order=%d\n", 
            pid, name, size, (void *)(KERNBASE+offset), split_order);
#endif
    return (void *)(offset+KERNBASE);
}

void reclaim_and_merge(uint64 head_pfn, uint64 init_order){
    //Exclusively reclaim blocks of a given order.
    //Also attempt to merge current block with its adjacent block to redur fragmentation.
    struct page *head=get_page_desc_assert(head_pfn);
    blocks_flags_reset(head, init_order, 1);
    struct page *buddy;
    uint64 buddy_pfn;
    while(init_order<MAX_ORDER){
        buddy_pfn= head_pfn ^ (1ull<<init_order);
        if(buddy_pfn>=total_pages || buddy_pfn<start_pfn) break;
        buddy=get_page_desc_safe(buddy_pfn);
        if(!buddy || !IS_FREE((uint64)buddy->flags) || GET_ORDER((uint64)buddy->flags)!=init_order)
            break;
        //remove current_page from the correspond free_list
        del_from_list_nolock(buddy, init_order);
        head_pfn=buddy_pfn & head_pfn;    //update for the next loop
        // ensure_pfn_valid(b_head_pfn);
        init_order++;
    }//concatenation
    blocks_flags_reset(&kmem.mem_bitmaps[head_pfn], init_order, 1);
    add_to_list_nolock(&kmem.mem_bitmaps[head_pfn], init_order);
}

void free_pages_nolock(void *pa, uint64 size){
    //When releasing an incomplete page, decompage it and splice the 
    //seperated fragments back into their respective slop.
    if((uint64)pa < free_start_addr)    panic("try to free the data segment!");
    if(size==0) return; //stop early
    size=PGROUNDUP(size);   //at least one page
    uint64 cur_order, req_order;
    uint64 b_head_pfn, b_tail_pfn;
    uint64 head_pfn, mid_pfn, tail_pfn;
    struct page *b_head, *head;
    b_head_pfn=paddr_to_pfn((uint64)pa);
    b_head=get_page_desc_assert(b_head_pfn);
    req_order=i_log2(size-1)-11;
    if(size != (1ull<<(req_order+ORDER_BASE))){   
        //size must be a valid set member.No internal splitting performed.
        panic("free_pages_nolock!");
    }
    b_tail_pfn=b_head_pfn+(1ull<<req_order);
    ensure_pfn_valid(b_tail_pfn-1);
    if(IS_HEAD((uint64)b_head->flags)==0) 
        head_pfn=b_head_pfn-GET_OFFSET((uint64)b_head->flags);//Not a block's header
    else
        head_pfn=b_head_pfn;
        //Block header(also need consider,unmapping a portion from the begining is possible!)
    head=get_page_desc_assert(head_pfn);
    cur_order=GET_ORDER((uint64)head->flags);
    if(req_order > cur_order){  //Dismatched
        panic("free_pages_nolock:order error!");
    }
    tail_pfn=head_pfn+(1ull<<cur_order);
    ensure_pfn_valid(tail_pfn-1);
    mid_pfn=head_pfn + (tail_pfn-head_pfn)/2;
    while(cur_order>req_order){
        //"It is guaranteed that the desired sub-block can be 
        // obtained through repeated binary splitting
        cur_order--;
        blocks_flags_reset(&kmem.mem_bitmaps[head_pfn], cur_order, 0);
        blocks_flags_reset(&kmem.mem_bitmaps[mid_pfn], cur_order, 0);
        if(mid_pfn>b_head_pfn)  //block locate on the left half
            tail_pfn=mid_pfn;
        else
            head_pfn=mid_pfn;   //block locate on the right half
        mid_pfn=head_pfn + (tail_pfn-head_pfn)/2;
    }
    //breakout from loop, must check pa is new block's header,size="size"
    if(head_pfn!=b_head_pfn || tail_pfn!=b_tail_pfn)
        panic("free_page: (non-header page)fail!");
    reclaim_and_merge(b_head_pfn, req_order);
}

void free_pages(void *pa, uint64 size){
    acquire(&kmem.lock);
    free_pages_nolock(pa, size);
    release(&kmem.lock);
}
/*
* NOTE: Extremely dangerous operation: this implies we might deallocate multiple buddy blocks simultaneously.
   Typically, the unmap phase should only use the aforementioned function that deletes a single buddy block.
*/
int reclaim_orphan_pages(void *pa, uint64 size){
    //Designed for those unmap area but still holds the physical resources.
    //Bypass pte and reclaim them
    if((uint64)pa<free_start_addr){
        KALLOC_TRACE("try to free the data segment, check the pa validity.\n");
        return -1;
    }
    if(size==0) return 0;
    size=PGROUNDUP(size);   //Eager reclaim
    uint64 cur_order, size_order, incr_order;
    //Respectively represent the order of the current block involved, the order of the 
    //downward-aligned buddy scale, and the incremental order.
    uint64 b_head_pfn, b_tail_pfn;
    uint64 head_pfn, mid_pfn, tail_pfn; //Involved blocks description
    struct page *b_head, *head;
    b_head_pfn=paddr_to_pfn((uint64)pa);
    size_order=i_log2(size-1)-12;
    b_tail_pfn=b_head_pfn+(1ull<<size_order);
    ensure_pfn_valid(b_tail_pfn-1);
    //Three cases in total:cross one blocks, cross two blocks, cross multi-blocks
    while(b_head_pfn<b_tail_pfn){
        b_head=get_page_desc_assert(b_head_pfn);
        if(IS_HEAD((uint64)b_head->flags)==0) 
            head_pfn=b_head_pfn-GET_OFFSET((uint64)b_head->flags);//Not a block's header
        else
            head_pfn=b_head_pfn;
            //Block header(also need consider,unmapping a portion from the begining is possible!)
        head=get_page_desc_assert(head_pfn);
        cur_order=GET_ORDER((uint64)head->flags);
        if(size_order > cur_order){  //Dismatched
            KALLOC_TRACE("free_pages_nolock:order error!");
            return -1;
        }
        tail_pfn=head_pfn+(1ull<<cur_order);
        mid_pfn=head_pfn+((tail_pfn-head_pfn)>>1);
        ensure_pfn_valid(tail_pfn-1);
        if(cur_order>size_order){    //Reach the final block
            do{
                //"It is guaranteed that the desired sub-block can be 
                // obtained through repeated binary splitting
                cur_order--;
                blocks_flags_reset(&kmem.mem_bitmaps[head_pfn], cur_order, 0);
                blocks_flags_reset(&kmem.mem_bitmaps[mid_pfn], cur_order, 0);
                if(mid_pfn>b_head_pfn)  //block locate on the left half
                    tail_pfn=mid_pfn;
                else
                    head_pfn=mid_pfn;   //block locate on the right half
                mid_pfn=head_pfn + (tail_pfn-head_pfn)/2;
            }while(cur_order>size_order);
            //breakout from loop, must check pa is new block's header,size="size"
            if(head_pfn!=b_head_pfn || tail_pfn!=b_tail_pfn)
                panic("free_page: (non-header page)fail!");
        }
        incr_order=MIN(size_order, cur_order);
        reclaim_and_merge(b_head_pfn, incr_order);
        b_head_pfn+=1ull<<incr_order;
        size-=1ull<<size_order;
        if(size==0) break;
        size_order=i_log2(size-1)-12;
    }
    return 0;
}

int is_all_same_swar(const uint8 *data, uint64 len){    //SIMD Within A Register
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
        len-=8;
    }
    ptr=(uint8 *)ptr64; //deal with epilog
    while(len >0){
        if(*ptr!=ref)
            return 0;
        len--;ptr++;
    }
    return 1;
}

//Check if the pagetable is entirely vacant()
//So that we can reclaim this pagetable.
int is_directory_empty(pagetable_t pagetable){
    uint64 *ptr64=(uint64 *)pagetable;
    for(int i=0;i<512;i+=8){
        //Using Cache Line(the minimum unit data transfer,typically 64 bytes in size)
        if(ptr64[i] | ptr64[i+1] | ptr64[i+2] | ptr64[i+3] | 
            ptr64[i+4] | ptr64[i+5] | ptr64[i+6] | ptr64[i+7])
        return 0;
    }
    return 1;
}

void kinit() {
#ifdef DEBUG_KALLOC
    KALLOC_TRACE("initializing memory allocator\n");
#endif
    initlock(&kmem.lock, "kmem");
    initlock(&swap_lock, "kmem_swap");
    init_whole_area((uint64)end, PHYSTOP);
    swap_kthread=kthread_create("swap_worker", swap_out);
#ifdef DEBUG_KALLOC
    KALLOC_TRACE("initialization complete\n");
#endif
}
//return 1 while pa at [free_start_addr, PHYstop) --RAM region
//return 0 while pa outside this region.
uint8 is_managed_memory(uint64 pa){
    if(pa<free_start_addr || pa>=PHYSTOP)   return 0;
    return 1;
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

//check the physical memory usage!
void dump_memory_map(){ //holding the lock
    printf("Address Range           Size      State    Order\n");
    printf("------------------------------------------------\n");
    uint64 cur_pfn=start_pfn, end_pfn, size, cur_order;
    struct page *p;
    uint8 lock_here=0;
    if(!holding(&kmem.lock)){
        lock_here=1;
        acquire(&kmem.lock);
    }
    printf("0x0 - 0x%llx  0x%llx pages  [Program]   -1\n", 
        (uint64)(kmem.mem_bitmaps), (uint64)(kmem.mem_bitmaps)/PGSIZE);
    printf("0x%llx - 0x%llx  0x%llx pages  [Membitmaps]   -1\n", 
        (uint64)(kmem.mem_bitmaps), free_start_addr, (free_start_addr-(uint64)(kmem.mem_bitmaps))/PGSIZE);
    while(cur_pfn<total_pages){
        p=get_page_desc_assert(cur_pfn);
        if(IS_HEAD(p->flags)){
            cur_order=GET_ORDER(p->flags);
            size=1ull<<(cur_order+ORDER_BASE);
            end_pfn=cur_pfn+(1ull<<cur_order);
            ensure_pfn_valid(end_pfn-1);
            if(IS_FREE(p->flags))
                printf("0x%llx - 0x%llx  0x%llx pages  [FREE]   0x%llx\n", 
                    pfn_to_paddr(cur_pfn), pfn_to_paddr(end_pfn), size/PGSIZE, cur_order);
            else
            printf("0x%llx - 0x%llx  0x%llx pages  [USED]   0x%llx\n", 
                pfn_to_paddr(cur_pfn), pfn_to_paddr(end_pfn), size/PGSIZE, cur_order);
            //Advance the pfn
            cur_pfn=end_pfn;
        }
        else    panic("Unorganized memory!");
    }
    if(lock_here==1)    release(&kmem.lock);
}

//Use a single-byte repetitive pattern to verify and set poison.
int check_poison(void *ptr, uint64 size){
    //return 0 while all poison(clean), return -1 means dirty
    if(ptr==NULL)   return -1;
    if(size==0)   return 0;
    if((uint64)ptr & 0x7){ //prelogue, skip to 64-bits aligned address.
        uint8 *ptr8=(uint8 *)ptr;
        do{
            if(*ptr8!=POISON_BYTE)  return -1;
            ptr8++;
            if(--size==0) return 0;
        }while(((uint64)ptr8 & 0x7));
        ptr=(void *)ptr8;
    }
    uint64 *ptr64=(uint64 *)ptr;
    while(size>=8){
        size-=8;
        if(ptr64[0]!=POISON_64) return -1;
        ptr64++;
    }
    if(size>0){ //Epilogue
        uint8 *ptr8=(uint8 *)ptr64;
        do{
            if(*ptr8!=POISON_BYTE)  return -1;
            ptr8++;
            if(--size==0)   return 0;
        }while(1);
    }
    return 0;
}

int set_poison(void *ptr, uint64 size){
    if(ptr==NULL || size==0)   return -1;
    //for SIMD optimization and Code simplicity,use memset directly
    memset(ptr, POISON_BYTE, size);
    return 0;
}





void swap_out(void){
    release(&swap_kthread->lock);
    extern struct proc proc[NPROC];
    printf("Swap out started on pid %d\n", myproc()->pid);
    for(;;){
        acquire(&swap_lock);
        while(0);
        sleep((void *)&swap_kthread, &swap_lock);   //Wake up 
        release(&swap_lock);    //Allow other processes to signal swap_kthread and enter sleep.
        printf("Now waking up to reclaim pages...\n");
        //Locate infrequently used physical pages to swap them to the disk(without kmem_lock)
        for(struct proc *tmp=proc;tmp<&proc[NPROC];tmp++){

        }
        //Now found it, acquire the lock to update kmem
        acquire(&kmem.lock);

        //done, now wakeup to inform all waiting process.
        wakeup((void *)&kmem);
        release(&kmem.lock);
    }

}