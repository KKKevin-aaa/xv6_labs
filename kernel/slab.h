#define PER_CPU_MAXSIZE 32
#define SLAB_PAGE_MAGIC  0x32142361

#define ALIGN_UP(a, size)   (((a) + (size) -1) & ~((size) -1))
#define POOL_LIMIT 5
#define DEBUG_SLAB

#ifdef DEBUG_SLAB
#define SLAB_TRACE(fmt, ...) \
    do { \
        printf("[SLAB:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define KVM_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif

struct slab_page{       //Slab page metadata
    void *freelist;
    slab_cache_t *cache;
    uint32 prev_pfn;
    uint32 next_pfn;
    uint32 magic;
    uint16 inuse_count;
};
struct slab_cpu_cache{  //Per-cpu structure.
    void *obj[PER_CPU_MAXSIZE];  //poitner array.
    int avail;  //number of objects in the packet.
};

struct slab_cache{
    struct slab_cpu_cache cpu_caches[NCPU]; //Avoid racing.
    struct spinlock pool_lock;  //Protects global list(free,partial,full)
    struct slab_page *partial_list;
    struct slab_page *empty_list;
    struct slab_page *full_list;
    char name[32];
    uint16 obj_size;
    uint16 basic_size;
    uint16 limit;   //maximun support entries in one page
    uint16 page_order;  //Determine how much memory needs to be allocated at this time,
                        //Expressed in order, and ensure the conversion is performed.
    int (*ctor)(void *);
    int (*dtor)(void *);
};
