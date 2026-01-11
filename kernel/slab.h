struct page_slab_header{
    uint64 magic;
    void *freelist_head;
    struct page_slab_header *prev_page, *next_page;
    struct slab_cache *cache;   //reverse pointer:belong to what pool?
    uint16 inuse_count;
};

struct slab_cpu_cache{  //Per-cpu structure.
    void *objects[32];  //poitner array.
    int avail;  //number of objects in the packet.
};

struct slab_cache{
    struct slab_cpu_cache cpu_caches[NCPU]; //Avoid racing.
    struct spinlock pool_lock;  //Protects global list(free,partial,full)
    struct page_slab_header *partial_list;
    struct page_slab_header *empty_list;
    struct page_slab_header *full_list;
    char name[32];
    uint16 obj_size;
    uint16 basic_size;
    uint16 align;
    uint16 limit;   //maximun support entries in one page
};