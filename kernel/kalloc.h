// Maximun size is 2^max_order*4KB, and PHYsize=128MB
// here we choose maximun size is 16MB, 
// and larger memory requirements can fulfilled by combining smaller components
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

extern const uint64 start_pfn;
extern const uint64 total_pages;    //Privilege Demotion

static inline uint64 paddr2pfn(uint64 pa){    //paddr convert to Page Frame Number
    // if(pa%PGSIZE!=0)    panic("paddr_to_pfn: unaligned!");
    if(pa%PGSIZE!=0)
        pa=(pa & ~(PGSIZE-1));
    if(pa<KERNBASE || pa>=PHYSTOP)  panic("paddr_to_pfn, out of range");
    return (pa-KERNBASE)/PGSIZE;
}

//Defensive Programming, provide two interface(must exist and try get)
static inline uint64 pfn2paddr(uint64 pfn){
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


#define PAGE2SLAB(pg)   \
    ((uint64)(pg)!=0)?(struct slab_page *)(&(((struct page *)(pg))->u.slab)):(NULL)
#define SLAB2PAGE(sl)   \
    ((uint64)(sl)!=0)?((struct page *)((void *)(sl) - offsetof(struct page, u.slab))):(NULL)
#define PADDR2SLAB(pa)  PAGE2SLAB(get_page_desc_safe(paddr2pfn((uint64)(pa))))
#define PFN2SLAB(pfn)   PAGE2SLAB(get_page_desc_safe(pfn))
#define SLAB2PFN(sl)    ((uint64)sl!=0)?page2pfn(SLAB2PAGE(sl)):0
#define SLAB2PADDR(sl)  pfn2paddr(SLAB2PFN(sl))


typedef enum{
    PG_TYPE_FREE=0,
    PG_TYPE_SLAB=1,
    PG_TYPE_MAPPED=2,
    PG_TYPE_REVERSED=3,
}pg_type_t;

typedef union page_flags{
    uint64 raw;
    struct{
        uint64 type:3;
        uint64 is_head:1;
        uint64 val:60;
    }common;
    //A universal view of invarient attributes shared by all page types.
    struct{
        uint64 type:3;
        uint64 is_head:1;   //must be 1
        uint64 order:60;
    }buddy_head;
    struct{
        uint64 type:3;
        uint64 is_head:1;   //must be 0;
        uint64 offset:60;
    }buddy_tail;
}page_flags_t;
_Static_assert(sizeof(page_flags_t)==8, "FATAL:page_flags_t must be 8 bytes");
_Static_assert(PG_TYPE_REVERSED<=7, "Page type enum overflow!max is 7!");
struct map_page{
    void *rmapping;
    uint64 index;
    uint8 sub_type;
};
struct page{
    page_flags_t flags;
    union 
    {
        struct {struct page *next, *prev;} buddy;    //PG_TYPE_BUDDY
        struct slab_page slab;      //PG_TYPE_SLAB
        struct map_page map;        //PG_TYPE_MAPPED
        void *private_raw;          //raw pointer for debug.
    }u;
};
_Static_assert(sizeof(struct page) <= 64, "Struct page is too big!");
struct listhead{
    struct page *head;
};