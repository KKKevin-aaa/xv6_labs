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
#define PAGE2PFN(pg)        page_to_pfn(pg)
// struct page{
//     uint64 flags;   //If bit 63 is 1:record order, size is 2^(order + ORDER_BASE)
//     //To support partial deallocation, embed the head offset within order metadata.(bit 63 is 0)
//     //which means the maximum num is 1ull<<62(>MAX_Order), and last bit 0 indicate free or occuiped?

//     void *rmapping; //Reverse mapping to virtual address.
//     //bit 0==0:point to struct address_sapce, bit 0==1:point to struct anon_vma.
//     uint64 index;

//     struct page *next;
//     struct page *prev;  //for delete node form list quickly
// };
struct listhead{
    struct page *head;
};

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