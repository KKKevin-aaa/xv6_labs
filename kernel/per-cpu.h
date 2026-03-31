// Per-CPU state.
#define MAX_LOCK_DEPTH 10
struct lock_debug_info{
    uint64 ret_addr;
    struct spinlock *held_lock;
};


enum cpustate {CPU_SPINNING, CPU_RUNNING, CPU_HALTED, CPU_PANIC};
struct cpu {
    struct proc *proc;       // The process running on this cpu, or null.
    struct context context;  // swtch() here to enter scheduler().
    struct lock_debug_info lock_lists[MAX_LOCK_DEPTH];
    volatile enum cpustate state;       //Flags, while corresponding payload is wait_lock
    struct spinlock * volatile wait_lock;//Track locks that exceed the maximum wait duration.
    //NOTE: Optimize to fit within a CPU register to enable lock-free access and reduce cache line footprint.
    int noff;                // The number of spinlocks currently held.
    int intena;              // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];


enum box_state {
    BOX_PENDING, BOX_DONE, BOX_EMPTY, BOX_CLAIMED
};
struct tlb_shootdown_req{
    uint64 start_va;
    uint64 len;
    pagetable_t target_pgdir;
};

struct ipi_message{
    void (*func)(void *);
    void *args;
    enum box_state status;
};


//Use extension prevent false sharing(in CPU cahce Line)
struct mailbox{
    struct spinlock lock;
    struct ipi_message reqs[MAX_MAIL];
    uint64 head, tail;   //Circular buffer.
}__attribute__((aligned(64)));
_Static_assert(sizeof(struct mailbox) % 64==0, "Unaligned to 64");

#define IPI_REASON_TLB  (1 << 0)
#define IPI_REASON_RCU  (1 << 1)
#define IPI_REASON_SCHED    (1 << 2)



// #define TIMER_RUNNING   (1ull << 1)
// #define TIMER_PENDING   (1ULL << 2)
// #define TIMER_EXPIRED   (1ULL << 3)
// #define TIMER_ZOMINE    (1ULL << 4)
// #define TIMER_PAUSE     (1Ull << 5)
enum TIMER_STATE{
    TIMER_RUNNING, TIMER_PENDING, TIMER_EXPIRED, TIMER_PAUSE, TIMER_HANDLING, 
    NR_TIMER_VALID_STATE
};
#define TIMER_STATE_MASK    0X000000ff

struct timer_payload{
    int interval;
    int mask;       //
    int repeat_left;
    int bind_cpu;   // register in which hart.
    int proc_slot;   // proc slot number
    uint64 proc_pid;     // register by which proc.
    uint64 heap_idx;
    void (*handler)(void);
    uint64 backup_sp;       //store the trapframe'sp while invoke its callback func.
    //pointer for proc, locate the proc-specfic timer quickly(cut the relationship)
    struct timer_payload *prev, *next;
    struct timer_payload *handling_next;       //handling list to support re-entrant callback.
}__attribute__((aligned(64)));

static inline void set_timer_state(struct timer_payload *t, enum TIMER_STATE s){
    if(unlikely(t==NULL || s>=NR_TIMER_VALID_STATE))
        panic("illegal set.");
    int other_mask=t->mask & ~TIMER_STATE_MASK;
    t->mask = other_mask | (s & TIMER_STATE_MASK);
}

#define MIN_HEAP_NODE
struct timer_node{
    struct timer_payload *data;
    uint64 next_expr_time;
    //another pointers for min-heap, to support quick check if hardware interrupt comes.
#ifndef MIN_HEAP_NODE
    struct rb_node node;
#endif
};

struct timer_root{
    struct timer_node *root;
#ifdef MIN_HEAP_NODE
    int cur_size;
    int cur_max;
#endif
    struct spinlock lock;
};
