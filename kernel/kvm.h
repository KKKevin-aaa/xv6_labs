typedef int vm_fault_t;
struct vm_area_struct;
struct mm_struct;   //forward declaration
struct vm_operation_struct;

struct vm_area_struct{  //Virtual Memory Area
    uint64 vm_start, vm_end;
    uint64 vm_filesz;    //check if .bss segment 
    int file_fd;    //can't store file * directly
    uint64 vm_page_prot; //hardware page table entry prototype
    uint64 vm_flags; //Logical Permission indenpedent of hareware
    uint64 vm_pgoff;
    struct mm_struct *vm_mm;    //pointer back tp the process's main memory descriptor
    union{  //Multiplex memory for mutually exclusive data.
        struct{
            struct vm_area_struct *vm_next, *vm_prev;   //linked list of VMAs, sorted by address
            rb_node_t vm_rb_node;   //Red-black tree node
        };
        struct vm_area_struct *next_free;
    };
    const struct vm_operation_struct *vm_ops;
    struct spinlock vma_lock;   //Protect the "external usage state"(Logic Lifecycle)
    int ref_count;  //Atomic operations
};
struct vma_context{ 
    uint64 addr;
    vm_area_struct_t *prev;
    vm_area_struct_t *next;
};
struct mm_struct{
    vm_area_struct_t *mmap;  //Head of the list of VMAs(sorted by the address)
    rb_root_t *rb_root;   //root of the red-block tree of VMAs
    vm_area_struct_t *mmap_cache;
    // AddrSpace *as; //pointer to the address space struct(am-origin.h)
    //informational fields
    uint64 start_code, end_code;    //start and end of the .text sgement
    uint64 start_data, end_data; //start and end of the .data segment
    vm_area_struct_t *heap_vma; //start and end of the heap
    vm_area_struct_t *stack_vma; //tell us the start address of main block
    uint64 arg_start, arg_end;
    uint64 env_start, env_end; 
    struct spinlock mm_lock;
};   //one process have one tree

struct vm_operation_struct{
    void (*open)(vm_area_struct_t *vma);
    void (*close)(vm_area_struct_t *vma);
    vm_fault_t (*fault)(vm_area_struct_t *vma, uint64 fault_addr);
};

struct page_slab_header{
    uint64 magic;   //Magic number
    uint64 inuse_count; //Number of active objects in this page
    vm_area_struct_t *freelist_head;    //Start of the free item chain within this page
    struct page_slab_header *prev_page, *next_page;
    int cpu_id; //current slab is belong to which cpu's pool
};

struct vma_pool{
    struct spinlock pool_lock;
    page_slab_header_t *partial;
    page_slab_header_t *full;
    page_slab_header_t *empty;
};
#define PAGE_SLAB_HEADER_MAGIC  0x32142361
//Account for memory alignment when calculating structure size
#define VMA_SLAB_LIMIT ((PGSIZE - sizeof(page_slab_header_t)) / sizeof(vm_area_struct_t))
#define VMA_POOL_LIMIT 5
