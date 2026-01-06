#ifndef __ASSEMBLER__

// which hart (core) is this?
static inline uint64 r_mhartid() {
    uint64 x;
    asm volatile("csrr %0, mhartid" : "=r"(x));
    return x;
}

// Machine Status Register, mstatus

#define MSTATUS_MPP_MASK (3L << 11)  // previous mode.
#define MSTATUS_MPP_M (3L << 11)
#define MSTATUS_MPP_S (1L << 11)
#define MSTATUS_MPP_U (0L << 11)

static inline uint64 r_mstatus() {
    uint64 x;
    asm volatile("csrr %0, mstatus" : "=r"(x));
    return x;
}

static inline void w_mstatus(uint64 x) { asm volatile("csrw mstatus, %0" : : "r"(x)); }

// machine exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void w_mepc(uint64 x) { asm volatile("csrw mepc, %0" : : "r"(x)); }

// Supervisor Status Register, sstatus

#define SSTATUS_SPP (1L << 8)   // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5)  // Supervisor Previous Interrupt Enable
#define SSTATUS_UPIE (1L << 4)  // User Previous Interrupt Enable
#define SSTATUS_SIE (1L << 1)   // Supervisor Interrupt Enable
#define SSTATUS_UIE (1L << 0)   // User Interrupt Enable

static inline uint64 r_sstatus() {
    uint64 x;
    asm volatile("csrr %0, sstatus" : "=r"(x));
    return x;
}

static inline void w_sstatus(uint64 x) { asm volatile("csrw sstatus, %0" : : "r"(x)); }

// Supervisor Interrupt Pending
static inline uint64 r_sip() {
    uint64 x;
    asm volatile("csrr %0, sip" : "=r"(x));
    return x;
}

static inline void w_sip(uint64 x) { asm volatile("csrw sip, %0" : : "r"(x)); }

// Supervisor Interrupt Enable
#define SIE_SEIE (1L << 9)  // external
#define SIE_STIE (1L << 5)  // timer
static inline uint64 r_sie() {
    uint64 x;
    asm volatile("csrr %0, sie" : "=r"(x));
    return x;
}

static inline void w_sie(uint64 x) { asm volatile("csrw sie, %0" : : "r"(x)); }

// Machine-mode Interrupt Enable
#define MIE_STIE (1L << 5)  // supervisor timer
static inline uint64 r_mie() {
    uint64 x;
    asm volatile("csrr %0, mie" : "=r"(x));
    return x;
}

static inline void w_mie(uint64 x) { asm volatile("csrw mie, %0" : : "r"(x)); }

// supervisor exception program counter, holds the
// instruction address to which a return from
// exception will go.
static inline void w_sepc(uint64 x) { asm volatile("csrw sepc, %0" : : "r"(x)); }

static inline uint64 r_sepc() {
    uint64 x;
    asm volatile("csrr %0, sepc" : "=r"(x));
    return x;
}

// Machine Exception Delegation
static inline uint64 r_medeleg() {
    uint64 x;
    asm volatile("csrr %0, medeleg" : "=r"(x));
    return x;
}

static inline void w_medeleg(uint64 x) { asm volatile("csrw medeleg, %0" : : "r"(x)); }

// Machine Interrupt Delegation
static inline uint64 r_mideleg() {
    uint64 x;
    asm volatile("csrr %0, mideleg" : "=r"(x));
    return x;
}

static inline void w_mideleg(uint64 x) { asm volatile("csrw mideleg, %0" : : "r"(x)); }

// Supervisor Trap-Vector Base Address
// low two bits are mode.
static inline void w_stvec(uint64 x) { asm volatile("csrw stvec, %0" : : "r"(x)); }

static inline uint64 r_stvec() {
    uint64 x;
    asm volatile("csrr %0, stvec" : "=r"(x));
    return x;
}

// Supervisor Timer Comparison Register
static inline uint64 r_stimecmp() {
    uint64 x;
    // asm volatile("csrr %0, stimecmp" : "=r" (x) );
    asm volatile("csrr %0, 0x14d" : "=r"(x));
    return x;
}

static inline void w_stimecmp(uint64 x) {
    // asm volatile("csrw stimecmp, %0" : : "r" (x));
    asm volatile("csrw 0x14d, %0" : : "r"(x));
}

// Machine Environment Configuration Register
static inline uint64 r_menvcfg() {
    uint64 x;
    // asm volatile("csrr %0, menvcfg" : "=r" (x) );
    asm volatile("csrr %0, 0x30a" : "=r"(x));
    return x;
}

static inline void 
w_menvcfg(uint64 x)
{
  //asm volatile("csrw menvcfg, %0" : : "r" (x));
  asm volatile("csrw 0x30a, %0" : : "r" (x));
}

// Physical Memory Protection
static inline void w_pmpcfg0(uint64 x) { asm volatile("csrw pmpcfg0, %0" : : "r"(x)); }

static inline void w_pmpaddr0(uint64 x) { asm volatile("csrw pmpaddr0, %0" : : "r"(x)); }

// use riscv's sv39 page table scheme.
#define SATP_SV39 (8L << 60)

#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

// supervisor address translation and protection;
// holds the address of the page table.
static inline void w_satp(uint64 x) { asm volatile("csrw satp, %0" : : "r"(x)); }

static inline uint64 r_satp() {
    uint64 x;
    asm volatile("csrr %0, satp" : "=r"(x));
    return x;
}

// Supervisor Trap Cause
static inline uint64 r_scause() {
    uint64 x;
    asm volatile("csrr %0, scause" : "=r"(x));
    return x;
}

// Supervisor Trap Value
static inline uint64 r_stval() {
    uint64 x;
    asm volatile("csrr %0, stval" : "=r"(x));
    return x;
}

// Machine-mode Counter-Enable
static inline void w_mcounteren(uint64 x) { asm volatile("csrw mcounteren, %0" : : "r"(x)); }

static inline uint64 r_mcounteren() {
    uint64 x;
    asm volatile("csrr %0, mcounteren" : "=r"(x));
    return x;
}

// machine-mode cycle counter
static inline uint64 r_time() {
    uint64 x;
    asm volatile("csrr %0, time" : "=r"(x));
    return x;
}

static inline uint64 r_cycle() {
    uint64 x;
    asm volatile("csrr %0, cycle": "=r"(x));
    return x;
}

// enable device interrupts
static inline void intr_on() { w_sstatus(r_sstatus() | SSTATUS_SIE); }

// disable device interrupts
static inline void intr_off() { w_sstatus(r_sstatus() & ~SSTATUS_SIE); }

// are device interrupts enabled?
static inline int intr_get() {
    uint64 x = r_sstatus();
    return (x & SSTATUS_SIE) != 0;
}

static inline uint64 r_sp() {
    uint64 x;
    asm volatile("mv %0, sp" : "=r"(x));
    return x;
}

static inline uint64 r_fp() {
    uint64 x;
    asm volatile("mv %0, s0" : "=r"(x));
    return x;
}


// read and write tp, the thread pointer, which xv6 uses to hold
// this core's hartid (core number), the index into cpus[].
static inline uint64 r_tp() {
    uint64 x;
    asm volatile("mv %0, tp" : "=r"(x));
    return x;
}

static inline void w_tp(uint64 x) { asm volatile("mv tp, %0" : : "r"(x)); }

static inline uint64 r_ra() {
    uint64 x;
    asm volatile("mv %0, ra" : "=r"(x));
    return x;
}

// flush the TLB.
static inline void sfence_vma() {
    // the zero, zero means flush all TLB entries.
    asm volatile("sfence.vma zero, zero");
}

typedef uint64 pte_t;
typedef uint64 *pagetable_t;  // 512 PTEs

#define RESERVE
#define IN_PLACE_PROMOTE
#define RES_BITMAP_WORDS 8
#define THRESHLOD 2
#define MAX_RES_BLOCK 32
#define MAX_ALLOWED_ALLOCATIONS 4
typedef struct Reservation{
    uint64 pa, va;  //for the block header
    int promoted;   //upgrade to huge page or not
    int is_scattered;   //1=scattered(limit to the memory, allocate superpage fail)
    uint64 pop_count;
    uint64 alloc_attempts;  //Huge page promotion attempts
    uint8 bitmap[512];  //0=free, 1=used
}res_block;
#endif  // __ASSEMBLER__

#define PGSIZE 4096 // bytes per page
#define PGSHIFT 12  // bits of offset within a page

// inline uint8 find_last_set(uint64 x){ 
// Adopt a standard ISA for underlying hareware realization!
//     if(x==0)    return 0;
//     uint64 ret;
//     asm volatile("clz %0, %1"
//                 : "=r"(ret) //output
//                 : "r"(x)    //input
//                 );
//     return 63-ret;  //last_set = 63 - leading_zero
// }
#ifndef __ASSEMBLER__
//exclude S files using the complier builtin macro
inline uint8 __attribute__((always_inline)) i_log2(uint64 x){
    //find most significant bits and floor it.
    if(x==0)    return 0;
    uint8 n=0;
    if(x & 0xffffffff00000000ull)   {n+=32;x>>=32;}
    if(x & 0xffff0000ull)           {n+=16;x>>=16;}
    if(x & 0xff00ull)               {n+=8;x>>=8;}
    if(x & 0xf0ull)                 {n+=4;x>>=4;}
    if(x & 0xcull)                  {n+=2;x>>=2;}
    if(x & 0x2ull)                  {n+=1;}
    return n;
}
#endif
#define MAX_ORDER 12
#define ORDER_BASE 12
#define MAX_LEVEL 3
#define ORDER_LIMIT (MAX_ORDER+ORDER_BASE+3)
#define MEGAPGSIZE (1ULL << 21)
#define GIGAPGSIZE (1ULL << 30)
#define SUPERPGSIZE (1 << 21) // bytes per page
#define SUPERPGROUNDUP(sz)  (((sz)+SUPERPGSIZE-1) & ~(SUPERPGSIZE-1))
#define SUPERPGROUNDDOWN(sz) ((sz) & ~(SUPERPGSIZE-1))
#define HUGEPGSIZE (1 << 30) // bytes per page
#define HUGEPGROUNDUP(sz)  (((sz)+HUGEPGSIZE-1) & ~(HUGEPGSIZE-1))
#define HUGEPGROUNDDOWN(sz) ((sz) & ~(HUGEPGSIZE-1))

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

//flags for vm_flags, paired with vm_page_prot(In vm_area_struct)
//prot parameter macros(simulating <sys/mman.h>)
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define PROT_USER 8

#define PTE_V (1L << 0)  // valid
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // user can access
#define PTE_G (1L << 5)
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)


// #if defined(LAB_MMAP) || defined(LAB_PGTBL) || defined(LAB_COW)
#define PTE_LEAF(pte) (((pte) & PTE_R) | ((pte) & PTE_W) | ((pte) & PTE_X))
// #endif

// shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)

#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// extract the three 9-bit page table indices from a virtual address.
#define PXMASK 0x1FF  // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

// one beyond the highest possible virtual address.
// MAXVA is actually one bit less than the max allowed by
// Sv39, to avoid having to sign-extend virtual addresses
// that have the high bit set.
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))
