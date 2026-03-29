#define SBRK_EAGER 1
#define SBRK_LAZY  2


struct vm_dupl_ctx {
    pagetable_t src_pg;
    pagetable_t dst_pg;
    uint64 base_va;
    void *ret_va;
    uint64 end_va;
    int dst_level;
    res_block *old_rblocks;
    res_block *new_rblocks;
    struct spinlock *dst_pt_lock;
    struct spinlock *src_pt_lock;
};

enum region_type{
    ZONE_REGION,
    RESERVED_REGION
};

struct kheap_node{
    struct kheap_node *prev, *next;
    uint64 start_va;
    uint64 size;
};

//NOTE:A special structure created to manage temporary allocation without recording them 
//in the page table, designed to avoid excessive physical memory consumption.
//It must be free after use,otherwise, it will cuase memory waste.
struct mem_trans_stash{ //Rearrange the memory layout for check and set poison quickly
    //Fixed length array.
    void *ptr[MAX_ORDER*2];
    uint8 order[MAX_ORDER*2];   //Paired with ptr.
    int count;
    uint32 magic;
    uint8 is_occupied;  //zero means free, while 1 means occupied.
};

struct batch_map_entry{
    uint64 pa;
    uint64 va;
    uint64 size;
    int xperm;
    int status;     //0 means have not mapped, 1 means already mapped.
};

struct alloc_context{
    pagetable_t pagetable;
    uint64 seg_start;
    uint64 seg_end;
    struct spinlock *pt_lock;
    res_block *rblocks;
    int xperm;
    int do_free;
};

struct map_context{
    pagetable_t pagetable;
    uint64 start_va;
    uint64 size;
    uint64 pa;
    struct spinlock *pt_lock;
    int cur_level;
    int do_free;
    int xperm;
};

//Reduce some unused parameter on the basis of map_context
struct map_iter_context{
    pagetable_t pagetable;
    uint64 cur_vpn;
    uint64 end_vpn;
    int cur_level;
};

#define MEM_TRANS_STACH_MAGIC 0x21792352