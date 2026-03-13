typedef int vm_fault_t;
struct vm_area_struct;
struct mm_struct;   //forward declaration
struct vm_operation_struct;

//Kernel macro, different from user_macro
// Describe this memory's logic attribute
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

// (Kernel Specific) ---
#define VM_KERN         0x4000ULL   // 标识这是一个内核 VMA (关键区分)
#define VM_DONTDUMP     0x8000ULL   // Core dump 时忽略此区域 (保护内核数据)
#define VM_PFNMAP       0x10000ULL  // 纯 PFN 映射 (用于直接管理物理地址的场景)
#define VM_MIXEDMAP     0x20000ULL  // 混合映射 (可能包含 struct page 也可能不包含)

// --- 常用组合 ---
#define VM_R_W_X        (VM_READ | VM_WRITE | VM_EXEC)

//Used for translating user's prot and flags into vm_flags
//(A broad abstraction that encompasses much more than just the init flags parameter.)
static inline uint64 calc_vm_flags(int prot, int flags) {
    // 1. 初始化为 0 或 默认允许未来修改权限 (Policy decision)
    // 通用做法：默认给予“未来可读、可写、可执行”的潜力
    uint64 vm_flags = VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

    // 2. 翻译基本权限 (PROT -> VM)
    if (prot & PROT_READ)
        vm_flags |= VM_READ;
    if (prot & PROT_WRITE)
        vm_flags |= VM_WRITE;
    if (prot & PROT_EXEC)
        vm_flags |= VM_EXEC;

    // 3. 翻译共享属性 (MAP -> VM)
    // MAP_SHARED 意味着修改对其他进程可见
    if (flags & MAP_SHARED) {
        vm_flags |= VM_SHARED | VM_MAYSHARE;
        // 如果是 SHARED，通常允许未来继续是 SHARED
    }
    // MAP_PRIVATE 是默认行为，不需要设置位，它隐含了 COW 行为
    // 4. 翻译 VMA 行为属性 (MAP -> VM)
    // [锁定内存] 防止被 Swap
    if (flags & MAP_LOCKED)
        vm_flags |= VM_LOCKED;

    // [栈自动扩展] 用于栈区
    if (flags & MAP_GROWSDOWN)
        vm_flags |= VM_GROWSDOWN;

    // [非标准扩展] 有些系统支持 MAP_GROWSUP (堆)
    #ifdef MAP_GROWSUP
    if (flags & MAP_GROWSUP)
        vm_flags |= VM_GROWSUP;
    #endif
    // [巨页支持] MAP_HUGETLB -> VM_HUGETLB (如果有定义)
    // if (flags & MAP_HUGETLB) vm_flags |= VM_HUGETLB;
    // 5. 过滤掉“动作类”标志
    // MAP_FIXED, MAP_ANONYMOUS, MAP_POPULATE 这些是“行为动作”，
    // 它们决定如何分配或初始化，但不属于 VMA 本身的属性，所以这里不处理。

    return vm_flags;
}

//Just a Template, always match the vm_flags.
static inline uint64 flags2page_prot(uint64 vm_flags) {    //vm_flags to vm_page_prot
    //As a hardware-agnostic kernel structure, it implements the translation
    //from logical abstraction to physical hardware via the following functions.
    if((vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) ==0)
        return 0;       //guard page check(PROT_NONE)
    uint64 page_prot = 0;
    if (vm_flags & VM_WRITE)    page_prot |= PTE_W;
    if (vm_flags & VM_EXEC)     page_prot |= PTE_X;
    if(!(vm_flags & VM_KERN))   page_prot |= PTE_U;
    page_prot |= PTE_V | PTE_R;     //Readable by default.

    if (vm_flags & VM_IO) {
        page_prot |= (PTE_A | PTE_D);
    }

    return page_prot;
}


struct vm_area_struct{  //Virtual Memory Area for user_mode.
    uint64 vm_start, vm_end;
    uint64 vm_filesz;    //check if .bss segment 
    struct file *vm_file;    //can't store file * directly
    uint64 vm_page_prot; //hardware page table entry prototype at this moment
    uint64 vm_flags; //Logical Permission independent of hareware
    uint64 vm_pgoff;    //File offset in pages units(if file-backed)
    struct mm_struct *vm_mm;    //pointer back tp the process's main memory descriptor
    union{  //Multiplex memory for mutually exclusive data.
        struct{
            struct vm_area_struct *vm_next, *vm_prev;   //linked list of VMAs, sorted by address
            rb_node_t vm_rb_node;   //Red-black tree node
        };
        struct vm_area_struct *next_free;
    };
    const struct vm_operation_struct *vm_ops;
    atomic_t ref_count;  //Atomic operations(used for non-mm_lock operations)
};

struct vma_context{ 
    uint64 addr;
    vm_area_struct_t *prev;
    vm_area_struct_t *next;
};
struct mm_struct{
    struct sleeplock mm_lock;
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
    pagetable_t pagetable;
    atomic_t ref_count;
};   //one process must have one tree
#define REF_SATURATION      0XC0000000

struct mmap_context{
    pagetable_t pagetable;  //Used for all operations involved pte.
    res_block *rb_array;     //Used for potential adjustment for heap reserve.
    mm_struct_t *mm;
    struct spinlock *pt_lock;
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
    struct spinlock *pt_lock;
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