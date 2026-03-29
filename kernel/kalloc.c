// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

// A cirtical macro supports restrictive access to current private variables,
// enabling inline characteristcic to reduce call overhead.

#include "types.h"
#include "param.h"
#include "atomic.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "per-cpu.h"
#include "slab.h"
#include "kalloc.h"
#include "colors.h"
#include "utils.h"
void dump_memory_map();     // Called with OOM error panic.
struct spinlock swap_lock;  // A logic lock protecting the "need_swap" signal
// and sleep/wake automicity for brief duration.Seperate from "kmem.lock" to avoid scheduling
// latency.
extern struct spinlock rmap_lock;
extern struct proc *swap_kthread;  // Defined in proc.c

static uint64 _hidden_total_pages;
static uint64 free_start_addr;
static uint64 _hidden_start_pfn;  // NOTE: start from "_hidden_start_pfn" instead of 0
extern char end[];                // first address after kernel.
                                  // defined by kernel.ld.
slab_cache_t kmalloc_caches[NR_SLAB_CACHES];

extern res_block rb_array[MAX_RES_BLOCK];
struct {                   // Anonymous structure(Single Pattern)
    struct spinlock lock;  // NOTE: Principle--acquire, get next free frame, release
    // Buddy system, minimum page size is 4096 bytes(4kB)
    page_t *mem_bitmaps;  // Embedded Array
    struct listhead free_area[MAX_ORDER + 1];
    uint64 nr_free;  // The number of current free pages.
    uint64 low_watermark;
} kmem;

// Some Symbol Aliaing(use extern to prevent memory allocation)
extern const volatile uint64 total_pages __attribute__((alias("_hidden_total_pages")));
extern const volatile uint64 start_pfn __attribute__((alias("_hidden_start_pfn")));

// Provide a const extern interface to achieve optimization without modifying the content.

static void free_pages_nolock(void *pa, uint64 size);

inline void sync_rmap(void *p, uint64 size, pte_t *pte) {
    if (!holding(&rmap_lock)) panic("sync_ramp without lock.\n");
}

static inline void w_head_order(page_t *p, uint64 order) {
    // Maintain the current page type.
    if (p == NULL) return;
    page_set_head(p);
    page_set_order(p, order);
}

static inline void w_tail_offset(page_t *p, uint64 offset) {
    if (p == NULL) return;
    page_set_tail(p);
    page_set_offset(p, offset);
}

static inline void set_free(page_t *p) {
    if (unlikely(p == NULL)) return;
    int new_ref = page_dec_ref(p);
    if (unlikely(new_ref < 0)) {
        pr_warn("Page Ref-count underflow!Double free deteched!");
        panic("Extermely! dangerous in free %llx\n", (uint64)pfn2paddr(page2pfn(p)));
    } else if (new_ref == 0)
        page_set_type(p, PG_TYPE_FREE);
}

static inline void set_alloc(page_t *p) {
    if (unlikely(p == NULL)) return;
    // For this shared structure, assign before check
    //(more specific writing is read-modify-write actually for bit_field)
    if (page_inc_ref(p)) {
        if (page_get_type(p) != PG_TYPE_MAPPED) page_set_type(p, PG_TYPE_MAPPED);
    }
}

int page_get_ref_wrapper(void *pa) {
    if ((uint64)pa % PGSIZE != 0) panic("get page ref_count: Lookup unaligned address!");
    if (unlikely(pa == NULL)) return -1;  // error
    return page_get_ref(get_page_desc_assert(paddr2pfn((uint64)pa)));
}

uint64 page2pfn(struct page *pg) {
    if (unlikely(pg == NULL)) return 0;
    return (uint64)(pg - kmem.mem_bitmaps);
}

page_t *get_page_desc_safe(uint64 pfn) {
    if (pfn < _hidden_start_pfn || pfn >= _hidden_total_pages) return NULL;
    return &kmem.mem_bitmaps[pfn];
}

page_t *get_page_desc_assert(uint64 pfn) {
    ensure_pfn_valid(pfn);
    return &kmem.mem_bitmaps[pfn];
}

/**
 * @brief 仅重置伙伴系统元数据（order, is_head, offset），不触碰引用计数或页面类型。
 *
 * @param p 指向块头页的指针。
 * @param new_order 新块的阶数。
 */
// NOTE: all recorded order is relative to ORDER_BASE
static void reset_buddy_metadata(page_t *p, uint64 new_order) {
    uint64 pfn = 1;
    uint64 size = 1ull << new_order;
    uint64 p_pfn = page2pfn(p);

    ensure_pfn_valid(p_pfn + size - 1);
    w_head_order(p, new_order);
    while (pfn < size) {
        w_tail_offset(p + pfn, pfn);
        pfn++;
    }
}

void inc_ref_range(void *pa, uint64 size) {
    // Used by kfork, simply increase the page's ref_count to raise COW in page_fault.
    uint64 start_pfn = paddr2pfn((uint64)pa), page_cnt;
    size = ALIGN_UP(size, PGSIZE);  // page-aligned
    page_cnt = size / PGSIZE;
    ensure_pfn_valid(start_pfn + page_cnt - 1);
    for (int i = 0; i < page_cnt; i++) {
        if (page_inc_not_zero_ref(&kmem.mem_bitmaps[i + start_pfn]) != 1)
            panic(
                "During the batch refence increment,(pa=%llx) "
                "encountered a page with ref_count is zero",
                (uint64)pa);
    }
}

static void dec_ref_range(page_t *p, uint64 size) {
    if (unlikely(p == NULL || size == 0)) return;
    uint64 head_pfn = (uint64)(p - kmem.mem_bitmaps), page_cnt;
    size = ALIGN_UP(size, PGSIZE);
    page_cnt = size / PGSIZE;
    ensure_pfn_valid(head_pfn + page_cnt - 1);
    int new_ref = 0;
    for (int i = 0; i < page_cnt; i++) {
        new_ref = page_dec_ref(&kmem.mem_bitmaps[i + head_pfn]);
        if (new_ref < 0) panic("double-free 0x%llx.", KERNBASE + (i + head_pfn) * PGSIZE);
    }
}

static void init_whole_area(uint64 end_addr, uint64 maximum_addr) {
    acquire(&kmem.lock);
    end_addr = PGROUNDUP(end_addr);
    maximum_addr = PGROUNDDOWN(maximum_addr);
    _hidden_total_pages = (maximum_addr - KERNBASE) / PGSIZE;
    // fpn0 should always corresponds to the absolute physical base address
    kmem.mem_bitmaps = (page_t *)end_addr;
    free_start_addr = end_addr + sizeof(page_t) * _hidden_total_pages;
    free_start_addr = PGROUNDUP(free_start_addr);
    // Important!Bootstrapping, solve by the cost of wasting some array element
    _hidden_total_pages = (maximum_addr - free_start_addr) / PGSIZE;
    // Skip the text,rodata segment of the program, start from the free_start_addr
    _hidden_start_pfn = paddr2pfn(free_start_addr);
    memset(kmem.mem_bitmaps, 0, _hidden_total_pages * sizeof(page_t));
    page_t *tmp_page = NULL;
    for (int i = 0; i < _hidden_total_pages; i++) {  // Set the reversed area(red zone)
        // For atomic variable, initialize the ref_count
        tmp_page = &kmem.mem_bitmaps[i];
        page_set_ref(tmp_page, 0);
        w_head_order(tmp_page, 0);
        page_set_ref(tmp_page, 1);  // Not zero increment
        page_set_type(tmp_page, PG_TYPE_MAPPED);
    }
    for (int i = 0; i < MAX_ORDER + 1; i++) {
        kmem.free_area[i].head = NULL;
    }
    for (int i = _hidden_start_pfn; i < _hidden_total_pages; i++) {
        free_pages_nolock((void *)pfn2paddr(i), PGSIZE);
    }
    kmem.low_watermark = 20;  // 32 * PGSIZE=0X20000
    kmem.nr_free = (_hidden_total_pages - _hidden_start_pfn);
    release(&kmem.lock);
}
// Ensure the element in the list are valid(in the range[start_pfn ,total_size))

static void del_from_list_nolock(page_t *p, uint64 order) {  // Occupied
    // check first!
    uint64 p_pfn = (uint64)(p - kmem.mem_bitmaps);
    ensure_pfn_valid(p_pfn + (1ull << order) - 1);
    for (int i = 0; i < (1ull << order); i++) {
        if (page_get_ref(get_page_desc_safe(p_pfn + i)) != 0) {
            pr_err("Attempted to free page with non-zero reference count.");
            panic("Memory allocator state corruption.");
        }
    }
    if (page_get_order(p) != order) {
        pr_err("try to delete Order.%llu block list while this block is Order.%u \n", order,
               page_get_order(p));
        panic("Inconsistent data.");
    }
    page_t *prev = p->u.buddy.prev, *next = p->u.buddy.next;
    if (prev == NULL) kmem.free_area[order].head = next;
    else
        prev->u.buddy.next = next;
    if (next != NULL) next->u.buddy.prev = prev;
    p->u.buddy.next = NULL;
    p->u.buddy.prev = NULL;
    // check if this block's oreder is expected
    if (kmem.nr_free < (1ull << order)) panic("Critical:memory watermark corruption detected.");
    kmem.nr_free -= (1ull << order);
    page_set_type(p, PG_TYPE_MAPPED);
}

static void add_to_list_nolock(page_t *p, uint64 order) {  // free
    uint64 p_pfn = p - kmem.mem_bitmaps;
    ensure_pfn_valid(p_pfn + (1ull << order) - 1);
    for (int i = 0; i < (1ull << order); i++) {
        if (page_get_ref(get_page_desc_safe(p_pfn + i)) != 0) {
            pr_err("Attempted to free page with non-zero reference count.");
            panic("Memory allocator state corruption.");
        }
    }
    if (page_get_order(p) != order) {
        pr_err("try to add to Order.%llu block list, while this block is Order.%u\n", order,
               page_get_order(p));
        panic("inconsistent data.");
    }
    page_t *old_head = kmem.free_area[order].head, *new_head = p;
    new_head->u.buddy.prev = NULL;
    if (old_head != NULL) {
        new_head->u.buddy.next = old_head;
        old_head->u.buddy.prev = new_head;
    } else
        new_head->u.buddy.next = NULL;
    kmem.free_area[order].head = new_head;
    kmem.nr_free += (1ull << order);
    page_set_type(p, PG_TYPE_FREE);
}

uint64 get_order(uint64 pa) {
    if (pa % PGSIZE != 0) panic("get order: Lookup unaligned address!");
    page_t *p = get_page_desc_assert(paddr2pfn(pa));
    if (page_is_head(p) == 0) return 0;
    return page_get_order(p);
}

uint8 is_head(uint64 pa) {
    if (pa % PGSIZE != 0) panic("get order: Lookup unaligned address!");
    page_t *p = get_page_desc_assert(paddr2pfn(pa));
    return page_is_head(p);
}

void print_memorytable() {  // with lock
    acquire(&kmem.lock);
    release(&kmem.lock);
}

void *alloc_memory(uint64 size, int flags) {
    // check first, should be 4kB-aligned
    // And it must be ensured that only a single page is allocated within alloc_memory.
    if (i_log2(size & -size) < 12) {
        dump_memory_map();
        panic("alloc memory: should aligned with 4kB");
        return NULL;
    }
    uint8 order = i_log2(size - 1) - 11;
    if (size != (1ull << (order + ORDER_BASE))) {
        // size must be a valid set member.No internal splitting performed.
        pr_warn("alloc memory: allocation size is not basic unit supported!");
        panic("alloc_memory");
    }
    page_t *tmp, *high_tmp;  // higher page
    uint64 step, offset;
    if (order > MAX_ORDER) {
        pr_warn(
            "Required size is 0x%llx.Exceeds the maximum supported order, "
            "leading to an allocation failure.\n",
            size);
        return NULL;
    }
    acquire(&kmem.lock);
    // debugging for the extermely large size allocations.(Early warning)
    void *ret_addr = __builtin_return_address(0);
    // pr_info("current free memory is 0x%llx\n", kmem.nr_free);
    if (size >= 0x10 * PGSIZE)
        pr_info("0x%llx allocate a massive contingous block of memory, totaling 0x%llx",
                (uint64)ret_addr, size);
    if (kmem.nr_free <= kmem.low_watermark * 2)
        pr_info("Current free memory lower than watermakr.Early warning!");
    // In general cases, allocating a single is sufficient.
    uint8 split_order = order, kswap_woken = 0, attempts=0;
retry:
    if(++attempts > SWAP_RETRY_THRESHOLD && !(flags & GFP_NOFAIL)){
        release(&kmem.lock);
        return NULL;
    }
    while (split_order <= MAX_ORDER && kmem.free_area[split_order].head == NULL) split_order++;
    if (split_order > MAX_ORDER) {
        if(flags & GFP_ATOMIC){
            release(&kmem.lock);
            return NULL;
        }
        if (mycpu()->noff > 1) {
            if(flags & GFP_NOFAIL)
                panic("alloc_memory: Invalid flag combination"
                    " - sleeping function called from invalid context");
            printf("Holding more than one lock when occur OOM.Cannot attempt to swap.\n");
            // dump_memory_map();
            release(&kmem.lock);
            return NULL;
        }
        // dump_memory_map();
        kswap_woken = 1;
        acquire(&swap_lock);  // Only the kmem_lock was held prior to this.
        // NOTE: Make the "check for out-of-memory" and "go to sleep" into a single atomic
        // operations.
        wakeup((void *)swap_kthread);
        release(&kmem.lock);
        sleep((void *)&kmem, &swap_lock);
        // Sleep safely,ensuring all wakeup signal will definitely be blocked
        release(&swap_lock);  // release first.
        acquire(&kmem.lock);
        printf("Current available space isn't enough, swap starting...\n");
        // Now after swapping to the disk, checking if current space is enough now.
        split_order = order;
        goto retry;
    }
    tmp = kmem.free_area[split_order].head;  // lower page
    high_tmp = tmp;                          // higher page
    del_from_list_nolock(tmp, split_order);  // pop from the list
    if (page_get_ref(tmp) != 0)              // check the node's attribute
        panic("Exist one allocated_page on free_list.");
    reset_buddy_metadata(tmp, split_order);
    uint64 tmp_pfn = (uint64)(tmp - kmem.mem_bitmaps);
    while (split_order > order) {
        // split into two blocks,both add into the lower level list
        split_order--;
        step = 1ull << split_order;
        high_tmp = get_page_desc_assert(tmp_pfn + step);
        reset_buddy_metadata(tmp, split_order);
        reset_buddy_metadata(high_tmp, split_order);
        add_to_list_nolock(high_tmp, split_order);
    }
    for (int i = 0; i < size / PGSIZE; i++)
        set_alloc(get_page_desc_assert(tmp_pfn + i));  // increment the reference count.
    offset = (tmp - kmem.mem_bitmaps) * PGSIZE;
    if (kswap_woken == 1 && kmem.nr_free > kmem.low_watermark) wakeup_one(&kmem);
    release(&kmem.lock);
    if(flags & GFP_ZERO)    memset((void *)offset + KERNBASE, 0, size);
    return (void *)(offset + KERNBASE);
}

void split_block(void *pa, uint64 size) {
    // called by reclaim_and_merge or free_pages(targeted for allocated pages.)
    // Used for split the complete buddy blocks into the target_order
    if ((uint64)pa < free_start_addr) panic("try to free the data segment!");
    if (size == 0) return;   // stop early
    size = PGROUNDUP(size);  // at least one page
    uint64 cur_order, req_order;
    uint64 b_head_pfn, b_tail_pfn;
    uint64 head_pfn, mid_pfn, tail_pfn;
    page_t *b_head, *head;
    b_head_pfn = paddr2pfn((uint64)pa);
    b_head = get_page_desc_assert(b_head_pfn);
    req_order = i_log2(size - 1) - 11;
    if (size != (1ull << (req_order + ORDER_BASE))) {
        // size must be a valid set member.No internal splitting performed.
        panic("free_pages_nolock!");
    }
    b_tail_pfn = b_head_pfn + (1ull << req_order);
    ensure_pfn_valid(b_tail_pfn - 1);
    if (page_is_head(b_head) == 0)
        head_pfn = b_head_pfn - page_get_offset(b_head);  // Not a block's header
    else
        head_pfn = b_head_pfn;
    // Block header(also need consider,unmapping a portion from the begining is possible!)
    head = get_page_desc_assert(head_pfn);
    cur_order = page_get_order(head);
    if (req_order > cur_order) {  // Dismatched
        panic("free_pages_nolock:order error!");
    }
    tail_pfn = head_pfn + (1ull << cur_order);
    ensure_pfn_valid(tail_pfn - 1);
    mid_pfn = head_pfn + (tail_pfn - head_pfn) / 2;
    while (cur_order > req_order) {
        //"It is guaranteed that the desired sub-block can be
        // obtained through repeated binary splitting
        cur_order--;
        reset_buddy_metadata(&kmem.mem_bitmaps[head_pfn], cur_order);
        reset_buddy_metadata(&kmem.mem_bitmaps[mid_pfn], cur_order);
        page_set_type(&kmem.mem_bitmaps[mid_pfn], page_get_type(&kmem.mem_bitmaps[head_pfn]));
        if (mid_pfn > b_head_pfn)  // block locate on the left half
            tail_pfn = mid_pfn;
        else
            head_pfn = mid_pfn;  // block locate on the right half
        mid_pfn = head_pfn + (tail_pfn - head_pfn) / 2;
    }
    // breakout from loop, must check pa is new block's header,size="size"
    if (head_pfn != b_head_pfn || tail_pfn != b_tail_pfn)
        panic("free_page: (non-header page)fail!");
}

static void free_single_block(uint64 start_pfn, uint64 size) {
    uint64 reclaim_start_pfn = 0, reclaim_size = 0, del_order;
    page_t *del_page;
    for (int i = 0; i < size / PGSIZE; i++) {
        del_page = get_page_desc_safe(i + start_pfn);
        // Validation concluded in the preceding step, ues safe_version.
        set_free(del_page);
        if (page_get_ref(del_page) == 0) {
            if (reclaim_size == 0) reclaim_start_pfn = start_pfn + i;
            reclaim_size += PGSIZE;
        } else {  // convert "contiguous free_size" into order(splice and reclaim in batches.)
            while (reclaim_size > 0) {
                del_order = i_log2(reclaim_size - 1) - 11;
                reclaim_and_merge(reclaim_start_pfn, del_order);
                reclaim_start_pfn += (1ull << del_order);
                reclaim_size -= (1ull << (del_order + ORDER_BASE));
            }
        }
    }
    while (reclaim_size > 0) {  // Complete free blocks or unreclaimed left, do again.
        del_order = i_log2(reclaim_size - 1) - 11;
        reclaim_and_merge(reclaim_start_pfn, del_order);
        reclaim_start_pfn += (1ull << del_order);
        reclaim_size -= (1ull << (del_order + ORDER_BASE));
    }
}

void reclaim_and_merge(uint64 head_pfn, uint64 init_order) {
    // Exclusively reclaim blocks of a given order.
    // Also attempt to merge current block with its adjacent block to reduce fragmentation.
    for (int i = 0; i < (1ull << init_order); i++) {
        if (page_get_ref(get_page_desc_assert(head_pfn + i)) != 0) {
            pr_err("Attempt to reclaim one block which ref_count!=0");
            return;
        }
    }
    page_t *buddy;
    uint64 buddy_pfn, new_order = init_order;
    while (new_order < MAX_ORDER) {
        buddy_pfn = head_pfn ^ (1ull << new_order);
        if (buddy_pfn >= _hidden_total_pages || buddy_pfn < _hidden_start_pfn) break;
        buddy = get_page_desc_safe(buddy_pfn);
        if (!buddy || page_get_type(buddy) != PG_TYPE_FREE || page_get_ref(buddy) != 0 ||
            page_get_order(buddy) != new_order)
            break;
        // remove current_page from the correspond free_list
        del_from_list_nolock(buddy, new_order);
        if (page_get_ref(buddy) != 0) panic("Exist one allocated page in free_list.");
        head_pfn = buddy_pfn & head_pfn;  // update for the next loop
        // ensure_pfn_valid(b_head_pfn);
        new_order++;
    }  // concatenation
    page_t *final_head = get_page_desc_safe(head_pfn);
    if (init_order != new_order)  // Avoid double reset, underflow reference.
        reset_buddy_metadata(final_head, new_order);
    // At a minimum, the current block will be integrated into the free list with "init_order"
    if (page_get_ref(final_head) == 0) add_to_list_nolock(final_head, new_order);
    else
        panic("After reclaiming,still exist some reference??");
}

void free_pages_nolock(void *pa, uint64 size) {
    // When releasing an incomplete page, decompage it and splice the
    // seperated fragments back into their respective slop.
    if (size == 0) return;  // stop early
    uint64 req_order = i_log2(size - 1) - 11;
    if (size != (1ull << (req_order + ORDER_BASE))) {
        // size must be a valid set member.No internal splitting performed.
        panic("free_pages_nolock!");
    }
    uint64 b_head_pfn = paddr2pfn((uint64)pa);
    for (uint64 inc = 0; inc < size; inc += PGSIZE) {
        if (page_get_ref_wrapper((void *)((uint64)pa + inc)) == 0)
            panic("double-free 0x%llx while freeing 0x%llx with size 0x%llx.", (uint64)pa + inc,
                  (uint64)pa, size);
    }
    split_block(pa, size);
    free_single_block(b_head_pfn, size);
    if (kmem.nr_free < kmem.low_watermark) wakeup_one(&kmem);
}

void free_pages(void *pa, uint64 size) {
    acquire(&kmem.lock);
    free_pages_nolock(pa, size);
    release(&kmem.lock);
}
/*
* NOTE: Extremely dangerous operation:
  this implies we might deallocate multiple buddy blocks simultaneously.
   Typically, the unmap phase should only use the aforementioned function
   that deletes a single buddy block.
*/
int reclaim_orphan_pages(void *pa, uint64 size) {
    // Designed for those unmap area but still holds the physical resources.
    // Bypass pte and reclaim them directly.
    if (is_managed_memory((uint64)pa) == 0 || size == 0) {
        // pr_warn("Attempted to free a non-RAM region,please verify the pa validity.\n");
        pr_err("Invalid argument.");
        return -1;
    }
    size = PGROUNDUP(size);  // Eager reclaim
    uint64 cur_order, size_order, incr_order;
    // Respectively represent the order of the current block involved, the order of the
    // downward-aligned buddy scale, and the incremental order.
    uint64 b_head_pfn, b_tail_pfn, head_pfn;  // Involved blocks description
    page_t *b_head, *head;
    b_head_pfn = paddr2pfn((uint64)pa);
    size_order = i_log2(size - 1) - ORDER_BASE;
    if (size != 1ull << (size_order + ORDER_BASE)) {
        pr_err("Unsupported size in buddy-system, not power of 2.\n");
        return -1;
    }
    b_tail_pfn = b_head_pfn + (1ull << size_order);
    ensure_pfn_valid(b_tail_pfn - 1);
    // Three cases in total:cross one blocks, cross two blocks, cross multi-blocks
    acquire(&kmem.lock);
    while (b_head_pfn < b_tail_pfn) {
        b_head = get_page_desc_assert(b_head_pfn);
        if (page_is_head(b_head) == 0)
            head_pfn = b_head_pfn - page_get_offset(b_head);  // Not a block's header
        else
            head_pfn = b_head_pfn;
        // Block header(also need consider,unmapping a portion from the begining is possible!)
        head = get_page_desc_assert(head_pfn);
        cur_order = page_get_order(head);
        if (cur_order > size_order)  // Reach the final block
            split_block((void *)(b_head_pfn * PGSIZE + KERNBASE),
                        1ull << (size_order + ORDER_BASE));
        incr_order = MIN(size_order, cur_order);
        free_single_block(b_head_pfn, 1ull << (incr_order + ORDER_BASE));
        // finish potential recliamation.
        b_head_pfn += 1ull << incr_order;
        size -= 1ull << (size_order + ORDER_BASE);
        if (size == 0) break;
        size_order = i_log2(size - 1) - 12;  // update.
    }
    if (kmem.nr_free < kmem.low_watermark) wakeup_one(&kmem);
    release(&kmem.lock);
    return 0;
}

void *kmalloc(uint64 size) {
    if (size >= (1ull << MAX_SIZE_SHIFT)) return alloc_memory(size, GFP_ZERO);
    // Must smaller than (1ull<<MAX_SIZE_SHIFT)
    uint64 size1 = get_cache_size(size);
    if (size1 == -1) panic("Shouldn't reach here.\n");
    int idx = i_log2(size1) - MIN_SIZE_SHIFT;
    if (idx >= NR_SLAB_CACHES) panic("Shouldn't reach here.\n");
    return slab_alloc(&kmalloc_caches[idx]);
}

void kfree(void *pa, uint64 size) {
    page_t *del_page = get_page_desc_safe(paddr2pfn((uint64)pa));
    if (del_page == NULL) {
        KALLOC_TRACE("try to free protected area.\n");
        return;
    }
    if (page_get_type(del_page) == PG_TYPE_SLAB) slab_free(pa);
    else
        free_pages(pa, size);
}

// Check if the pagetable is entirely vacant()
// So that we can reclaim this pagetable.
int is_directory_empty(pagetable_t pagetable) {
    uint64 *ptr64 = (uint64 *)pagetable;
    for (int i = 0; i < 512; i += 8) {
        // Using Cache Line(the minimum unit data transfer,typically 64 bytes in size)
        if (ptr64[i] | ptr64[i + 1] | ptr64[i + 2] | ptr64[i + 3] | ptr64[i + 4] | ptr64[i + 5] |
            ptr64[i + 6] | ptr64[i + 7])
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
    initlock(&rmap_lock, "rmap");
    init_whole_area((uint64)end, PHYSTOP);
    init_slab_system();
}
// return 1 while pa at [free_start_addr, PHYstop) --RAM region
// return 0 while pa outside this region.
uint8 is_managed_memory(uint64 pa) {
    if (pa < free_start_addr || pa >= PHYSTOP) return 0;
    return 1;
}
// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree_page(void *pa) {
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) freeing pa=%p\n", pid, name, pa);
#endif
    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) panic("kfree");
#ifndef LAB_SYSCALL
    memset(pa, 1, PGSIZE);
#endif
    free_pages(pa, PGSIZE);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc_page(void) {
#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    KALLOC_TRACE("pid=%d(%s) requesting PGSIZE\n", pid, name);
#endif
    return alloc_memory(PGSIZE, GFP_ZERO);
}

// check the physical memory usage!
void dump_memory_map() {  // holding the lock
    printf("Address Range           Size      State    Order\n");
    printf("------------------------------------------------\n");
    uint64 cur_pfn = _hidden_start_pfn, end_pfn, size, cur_order;
    page_t *p;
    uint8 lock_here = 0;
    if (!holding(&kmem.lock)) {
        lock_here = 1;
        acquire(&kmem.lock);
    }
    printf("0x0 - 0x%llx  0x%llx pages  [Program]   -1\n", (uint64)(kmem.mem_bitmaps),
           (uint64)(kmem.mem_bitmaps) / PGSIZE);
    printf("0x%llx - 0x%llx  0x%llx pages  [Membitmaps]   -1\n", (uint64)(kmem.mem_bitmaps),
           free_start_addr, (free_start_addr - (uint64)(kmem.mem_bitmaps)) / PGSIZE);
    while (cur_pfn < _hidden_total_pages) {
        p = get_page_desc_assert(cur_pfn);
        if (page_is_head(p) == 1) {
            cur_order = page_get_order(p);
            size = 1ull << (cur_order + ORDER_BASE);
            end_pfn = cur_pfn + (1ull << cur_order);
            ensure_pfn_valid(end_pfn - 1);
            if (page_get_type(p) == PG_TYPE_FREE)
                printf("0x%llx - 0x%llx  0x%llx pages  [FREE]   0x%llx\n", pfn2paddr(cur_pfn),
                       pfn2paddr(end_pfn), size / PGSIZE, cur_order);
            else
                printf("0x%llx - 0x%llx  0x%llx pages  [USED]   0x%llx\n", pfn2paddr(cur_pfn),
                       pfn2paddr(end_pfn), size / PGSIZE, cur_order);
            // Advance the pfn
            cur_pfn = end_pfn;
        } else
            panic("Unorganized memory!");
    }
    if (lock_here == 1) release(&kmem.lock);
}

void swap_out(void) {
    release(&swap_kthread->lock);
    extern struct proc proc[NPROC];
    printf("Swap out started on pid %d\n", myproc()->pid);
    for (;;) {
        acquire(&swap_lock);
        while (0);
        sleep((void *)swap_kthread, &swap_lock);  // Wake up
        release(&swap_lock);  // Allow other processes to signal swap_kthread and enter sleep.

        // NOTE: Determine whether it is truly necessary to trigger the swap mechanism;
        // it's possible that other processes have released memory in the interim.(Re-validation!)
        if (1) {
            printf("Now waking up to reclaim pages...\n");
            // Locate infrequently used physical pages to swap them to the disk(without kmem_lock)
            for (struct proc *tmp = proc; tmp < &proc[NPROC]; tmp++) {
            }
            // Now found it, acquire the lock to update kmem
            acquire(&kmem.lock);
            //????????? FIXME: try to find out which acquire kmem but not release correctly.
            // done, now wakeup to inform all waiting process.(Must holding swap_lock!!)
            release(&kmem.lock);
        }
        acquire(&swap_lock);
        wakeup_one((void *)&kmem);
        release(&swap_lock);
    }
    panic("have not implemented.\n");
}
