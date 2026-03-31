#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

volatile int online_harts = 0;
volatile static int init_done_harts = 0;

//define in main.c(absolutely would be linked with)
//With -finstrument_function flags,force all functions convert to non-leaf functions, 
// save ra So that we can establish a complete calling chain
// Must add "no_instrument_function", otherwise these two function will be stubbed(infinite loop)
__attribute__((no_instrument_function)) 
void __cyg_profile_func_enter(void *ths_fn, void *call_site){
    return;
}
__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site){
    return;
}
void free_initmem();

// start() jumps here in supervisor mode on all CPUs.
// And we block another CPUs except CPU0 by software,
//  Establish uniprocess initialization.
void main() {
    if (cpuid() == 0) { //Bootstrap Processor:Global/System-wide Initialization
        consoleinit();
        printfinit();
        printf("\n");
        printf("xv6 kernel is booting\n");
        printf("\n");
        kinit();             // physical page allocator(before all pagetable operations!)
        kvminit();           // create kernel page table(initialize all the slab_pool)
        kvminithart();       // turn on paging
        procinit();          // process table
        trapinit();          // trap vectors
        trapinithart();      // install kernel trap vector
        plicinit();          // set up interrupt controller
        plicinithart();      // ask PLIC for device interrupts
        binit();             // buffer cache
        iinit();             // inode table
        fileinit();          // file table
        virtio_disk_init();  // emulated hard disk
        userinit();          // first user process
        init_utils();

        asm volatile("fence rw, rw" : : : "memory");
        started = 1;

        // CPU0 also consumes init text in this branch, mark itself done first.
        asm volatile("amoadd.w.aqrl x0, %1, %0" : "+A"(init_done_harts) : "r"(1) : "memory");
        while (init_done_harts < online_harts);
        asm volatile("fence r, rw" : : : "memory");
        free_initmem();
    } else {    //Application Processor(Per-CPU initialization)
        while (started == 0);
        asm volatile("fence r, rw" : : : "memory");
        printf("hart %d starting\n", cpuid());
        kvminithart();   // turn on paging
        trapinithart();  // install kernel trap vector
        plicinithart();  // ask PLIC for device interrupts

        // Mark AP init-text work completion before CPU0 reclaims init memory.
        asm volatile("amoadd.w.aqrl x0, %1, %0" : "+A"(init_done_harts) : "r"(1) : "memory");
        fixme: reparnt test still fail, last problem is variable used in rb_test exist severe lock problem.
    }
    scheduler();
}
