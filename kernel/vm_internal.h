//Private interface layer for internal helper functions, facilitating cross-
// submodule calls within the VM systems.

//Used for copywalk, store the basic info about two pagetable and e.t.c
struct vm_sub_copy_ctx{
    pte_t *dst_pt;  //Dest Page table
    pte_t *src_pt;  //Source Page table
    uint64 base_va;
    uint64 size;
    res_block *old_rblocks;
    res_block *new_rblocks;
    struct spinlock *dst_pt_lock;
    struct spinlock *src_pt_lock;
    int dst_level;
};

struct mmu_free_batch_entry{
    uint64 start_va;
    uint64 len;
    uint64 delete_pa;
};

#define MMU_BATCH_SIZE  32
struct mmu_gather{
    pagetable_t root_pg;
    struct mmu_free_batch_entry data_page_batch[MMU_BATCH_SIZE];
    uint64 dir_pa_batch[MMU_BATCH_SIZE];
    uint64 batch_start_va;
    uint64 batch_flush_len;
    int fullmm;     //fully unmap(1) or partial(0)
    uint8 data_idx;
    uint8 dir_idx;
};

struct mmu_free_batch_listnode{     //Chunked Linked list
    struct mmu_free_batch_entry node[8];
    struct mmu_free_batch_listnode *next;
    int count;
};

//vm_pt.c
uint64 cross_scan_cont_map(pagetable_t pagetable, uint64 src_va, uint64 max);
uint64 same_scan_cont_map(pagetable_t pagetable, uint64 src_va, uint64 max, int level);
pte_t * walk_internal(pagetable_t pagetable, uint64 va, int alloc, 
    int target_level, int *found_level);
int buddy_alloc_backend(struct alloc_context *ctx1, uint64 (*free_fn)(struct alloc_context *),
                        int (*map_fn)(struct map_context *));
struct mmu_gather *mmu_gather_create(void);
void mmu_gather_reclaim(struct mmu_gather *mg1);

//vm_fault.c
int vmfile_load(vm_area_struct_t *vma, uint64 va, uint64 dst_pa, uint64 size);
uint64 vm_cowfault_handler(pagetable_t pagetable, uint64 flush_va, 
    uint64 flush_size, pte_t * pte, int flush);

//vm_thp.c
int in_res_area(uint64 va, res_block *rblocks);