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



