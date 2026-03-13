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