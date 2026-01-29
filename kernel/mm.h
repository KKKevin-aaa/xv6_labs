typedef int vm_fault_t;
struct vm_area_struct;
struct mm_struct;   //forward declaration
struct vm_operation_struct;

//Kernel macro, different from user_macro
// --- 基础权限 (对应 mmap prot) ---
#define VM_READ         0x01ULL     // 可读
#define VM_WRITE        0x02ULL     // 可写
#define VM_EXEC         0x04ULL     // 可执行
#define VM_SHARED       0x08ULL     // 共享映射 (MAP_SHARED)

// --- 进阶属性 (对应 mmap flags 或内部状态) ---
#define VM_MAYREAD      0x10ULL     // 允许改为可读
#define VM_MAYWRITE     0x20ULL     // 允许改为可写
#define VM_MAYEXEC      0x40ULL     // 允许改为可执行
#define VM_MAYSHARE     0x80ULL     // 允许改为共享

// --- 特殊区域标识 ---
#define VM_GROWSDOWN    0x0100ULL   // 栈 (Stack)
#define VM_GROWSUP      0x0200ULL   // 堆 (Heap)
#define VM_NOHUGEPAGE   0x0400ULL   // 禁止巨页
#define VM_DONTEXPAND   0x0800ULL   // 禁止扩展
#define VM_LOCKED       0x1000ULL   // 内存锁定 (mlock)
#define VM_IO           0x2000ULL   // 内存映射IO (MMIO)

// --- 常用组合 ---
#define VM_R_W_X        (VM_READ | VM_WRITE | VM_EXEC)
struct vm_area_struct{  //Virtual Memory Area
    uint64 vm_start, vm_end;
    uint64 vm_filesz;    //check if .bss segment 
    struct file *vm_file;    //can't store file * directly
    uint64 vm_page_prot; //hardware page table entry prototype
    uint64 vm_flags; //Logical Permission independent of hareware
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
    int ref_count;  //Atomic operations
};

struct vma_context{ 
    uint64 addr;
    vm_area_struct_t *prev;
    vm_area_struct_t *next;
};
struct mm_struct{
    struct spinlock mm_lock;
    //Protects concurrent VMA modifications within the same address space.
    //By replacing the lock in mm_struct rather than the process descriptor
    //we reduce contention among threads and decouple memory from process logic.
    vm_area_struct_t *mmap;  //Head of the list of VMAs(sorted by the address)
    rb_root_t rb_root;   //root of the red-block tree of VMAs
    vm_area_struct_t *mmap_cache;
    // AddrSpace *as; //pointer to the address space struct(am-origin.h)
    //informational fields
    uint64 start_code, end_code;    //start and end of the .text sgement
    uint64 start_data, end_data; //start and end of the .data segment
    vm_area_struct_t *heap_vma; //start and end of the heap
    vm_area_struct_t *stack_vma; //tell us the start address of main block
    uint64 arg_start, arg_end;
    uint64 env_start, env_end;  
    int ref_count;
};   //one process must have one tree
#define REF_SATURATION      0XC0000000

struct mmap_context{
    pagetable_t pagetable;  //Used for all operations involved pte.
    res_block *rb_array;     //Used for potential adjustment for heap reserve.
    mm_struct_t *mm;
    void *sugg_addr;
    uint64 length;
    struct file* f;
    uint64 offset;
    int prot;
    int flags;
};


struct munmap_context{
    pagetable_t pagetable;
    res_block *rb_array;
    mm_struct_t *mm;
    void *addr;
    uint64 length;
};

struct vm_operation_struct{
    void (*open)(vm_area_struct_t *vma);
    //.e.g. Maintain a record of count of VMAs currently in use.
    void (*close)(vm_area_struct_t *vma);
    vm_fault_t (*fault)(vm_area_struct_t *vma, uint64 fault_addr);
};

#ifdef DEBUG_MM
#define MM_TRACE(fmt, ...) \
    do { \
        printf("[MM:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define MM_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif