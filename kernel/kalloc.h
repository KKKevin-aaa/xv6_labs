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

//NOTE: Since we initially qualified it as const, the compiler default to using its
//initial value(e.g. 0)due to constant folding,ignoring subsequent modifications,
//So we must use valatile to force compiler to read the required value from memory every time.

//size-special pool
#define MIN_SIZE_SHIFT 5    //32=1<<5
#define MAX_SIZE_SHIFT 13   //8192=1<<13
#define NR_SLAB_CACHES  (MAX_SIZE_SHIFT-MIN_SIZE_SHIFT+1)

extern const volatile uint64 start_pfn;
extern const volatile uint64 total_pages;    //Privilege Demotion

static inline uint64 paddr2pfn(uint64 pa){    //paddr convert to Page Frame Number
    // if(pa%PGSIZE!=0)    panic("paddr_to_pfn: unaligned!");
    if(pa%PGSIZE!=0)
        pa=(pa & ~(PGSIZE-1));
    if(pa<KERNBASE || pa>=PHYSTOP)  panic("out of range\n");
    return (pa-KERNBASE)/PGSIZE;
}

//Defensive Programming, provide two interface(must exist and try get)
static inline uint64 pfn2paddr(uint64 pfn){
    if(pfn>total_pages)    panic("Segment fault\n");
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
struct map_page{
    void *rmapping;
    uint64 index;
    uint8 sub_type;
};
typedef union page_flags{
    uint64 raw;
    struct{
        atomic_t ref_count;
        //Use Uniform Type,avoid signle bit_field stradding storage unit.
        //and padding casued by disalign.
        union{
            uint32 meta_raw;
            struct{
                uint32 type:3;
                uint32 is_head:1;
                uint32 reserved:4;
                uint32 val:24;
            }common;
            struct{
                uint32 type:3;
                uint32 is_head:1;
                uint32 reserved:4;
                uint32 order:24;
            }buddy_head;
            struct{
                uint32 type:3;
                uint32 is_head:1;
                uint32 reverse:4;
                uint32 offset:24;
            }buddy_tail;
        }meta;
    }parts;
}page_flags_t;
struct page{
    page_flags_t __internal_flags;
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
static inline int page_get_ref(page_t *p){
    if(unlikely(p==NULL))   return -1;
    return atomic_read(&p->__internal_flags.parts.ref_count);
}
//Assign operation(=) will finish Load, Bit manipulation, Store automatically.
static inline int page_inc_ref(page_t *p){
    if(unlikely(p==NULL))   return -1;
    return atomic_inc_and_ret(&p->__internal_flags.parts.ref_count);
}
static inline void page_set_ref(page_t *p, int new_ref){
    if(unlikely(p==NULL))   return;
    atomic_set(&p->__internal_flags.parts.ref_count, new_ref);
}
static inline int page_read_ref(page_t *p){
    if(unlikely(p==NULL))   return -1;
    return atomic_read(&p->__internal_flags.parts.ref_count);
}
static inline int page_dec_ref(page_t *p){
    if(unlikely(p==NULL))   return -1;
    return atomic_dec_and_ret(&p->__internal_flags.parts.ref_count);
}
static inline void page_set_val(page_t *p, uint32 new_val){
    if(unlikely(p==NULL))   return ;
    p->__internal_flags.parts.meta.common.val=new_val;
}
static inline void page_set_order(page_t *p, uint32 new_order){
    page_set_val(p, new_order);
}
static inline void page_set_offset(page_t *p, uint32 new_offset){
    page_set_val(p, new_offset);
}
static inline uint32 page_get_type(page_t *p){
    return p->__internal_flags.parts.meta.common.type;
}

static inline int page_is_head(page_t *p){
    return p->__internal_flags.parts.meta.common.is_head;
}

static inline uint32 page_get_order(page_t *p){
    return p->__internal_flags.parts.meta.buddy_head.order;
}

static inline uint32 page_get_offset(page_t *p){
    return p->__internal_flags.parts.meta.buddy_tail.offset;
}
static inline void page_set_type(page_t *p, uint32 type){
    if(unlikely(p==NULL))   return;
    p->__internal_flags.parts.meta.common.type = type;
}
static inline void page_set_head(page_t *p){
    if(unlikely(p==NULL))   return;
    p->__internal_flags.parts.meta.common.is_head=1;
}
static inline void page_set_tail(page_t *p){
    if(unlikely(p==NULL))   return;
    p->__internal_flags.parts.meta.common.is_head=0;
}
// [New] Logic Helpers
static inline int page_is_free(page_t *p){
    return page_get_type(p) == PG_TYPE_FREE; 
}


_Static_assert(sizeof(page_flags_t)==8, "FATAL:page_flags_t must be 8 bytes");
_Static_assert(PG_TYPE_REVERSED<=7, "Page type enum overflow!max is 7!");