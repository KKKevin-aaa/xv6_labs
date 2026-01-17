#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "mm.h"
// #define DEBUG_FORK
#ifdef DEBUG_FORK
#define FORK_TRACE(fmt, ...) \
    do { \
        printf("[FORK:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define FORK_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif
// #define PROC_DEBUG
// #define PROC_TEST_TIME 
struct cpu cpus[NCPU];

/// @brief static pcb array
struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[];  // trampoline.S
extern void *usyscall_pa;
// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl) {
    struct proc *p;
    void kvmmap_boot_only(pagetable_t,uint64,uint64,uint64,int);
    for (p = proc; p < &proc[NPROC]; p++) {
        char *pa = kalloc_page();
        if (pa == 0) panic("kalloc");
        uint64 va = KSTACK((int)(p - proc));
        printf("[proc_mapstacks] proc %d: kernel_stack located at %llx\n", (int)(p-proc), va);
        kvmmap_boot_only(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
}

// initialize the proc table.
void procinit(void) {
    struct proc *p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for (p = proc; p < &proc[NPROC]; p++) {
        initlock(&p->lock, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK((int)(p - proc));
    }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid() {
    int id = r_tp();
    return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *mycpu(void) {
    int id = cpuid();
    struct cpu *c = &cpus[id];
    return c;
}

// Return the current struct proc *, or zero if none.
struct proc *myproc(void) {
    push_off();
    struct cpu *c = mycpu();
    struct proc *p = c->proc;
    pop_off();
    return p;
}

int allocpid() {
    int pid;

    acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);

    return pid;
}

// Look in the process table to search an UNUSED proc.
// If found, initialize state required to run in the kernel, and return with p->lock held
// If there are no free procs, or a memory allocation fails, return 0.
// Default return funtion is forkret, which kexec "init process" and return to userspace.
static struct proc *allocproc(void) {
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->state == UNUSED) {
            if(p->mm==NULL){
                p->mm=mm_create();
                memset(p->mm, 0, sizeof(struct mm_struct));
            }
            else    clear_mm_internal(p->mm);
            goto found;
        } else {
            release(&p->lock);
        }
    }
    return 0;

found:
    p->pid = allocpid();
    p->state = USED;
    p->syscall_mask =0;
    // Allocate a trapframe page.
    if ((p->trapframe = (struct trapframe *)kalloc_page()) == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // An empty user page table.
    p->pagetable = proc_pagetable(p);
    if (p->pagetable == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // Set up new context to start executing at forkret,
    // which returns to user space.
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;

    return p;
}

// free a proc structure and the data hanging from it,
// including user pages. p->lock must be held.
static void freeproc(struct proc *p) {
    if (p->trapframe) kfree_page((void *)p->trapframe);
    p->trapframe = 0;
    #ifdef PROC_DEBUG
    printf("in freeproc oldpagetbale is %p\n", p->pagetable);
    #endif
    if (p->pagetable) proc_freepagetable(p->rb_array, p->pagetable, p->sz);
    #ifdef PROC_DEBUG
    printf("Done free this process's memory!\n");
    #endif
    p->pagetable = 0;
    p->sz = 0;
    p->pid = 0;
    p->parent = 0;
    p->name[0] = 0;
    p->waitChannel = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t proc_pagetable(struct proc *p) {
    pagetable_t pagetable;

    // An empty page table.
    pagetable = uvmcreate();
    if (pagetable == 0) return 0;
    if(usyscall_pa==0)
        return 0;
    else
        ((struct usyscall *)usyscall_pa)->pid=p->pid;
    // map the trampoline code (for system call return)
    // at the highest user virtual address.
    // only the supervisor uses it, on the way
    // to/from user space, so not PTE_U.
    if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0) {
        uvmfree(p->rb_array, pagetable, 0);
        return 0;
    }

    // map the trapframe page just below the trampoline page, for
    // trampoline.S.
    if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0) {
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(p->rb_array, pagetable, 0);
        return 0;
    }

    // establish a shared, read-only mapping at USYSCALL to speed up syscalls via direct memory address.
    if(mappages(pagetable, USYSCALL, PGSIZE, (uint64)usyscall_pa, PTE_R | PTE_U)<0){
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmunmap(pagetable, TRAPFRAME,  1, 0);
        uvmfree(p->rb_array, pagetable, 0);  //have not alloc memory, so size equal zero!
        return 0;
    }

    return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(res_block *rb_array, pagetable_t pagetable, uint64 sz) {
    uvmunmap(pagetable, TRAMPOLINE, PGSIZE, 0);
    uvmunmap(pagetable, TRAPFRAME, PGSIZE, 0);
    uvmunmap(pagetable, USYSCALL, PGSIZE, 0);
    //All process share one usyscall page,so don' free here!
    uvmfree(rb_array, pagetable, sz);
}

// Set up first user process.
void userinit(void) {
    struct proc *p;

    p = allocproc();
    initproc = p;

    p->cwd = namei("/");

    p->state = RUNNABLE;

    release(&p->lock);
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n) {
    uint64 sz;
    struct proc *p = myproc();

    sz = p->sz;
    if (n > 0) {
        if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
            return -1;
        }
    } else if (n < 0) {
        sz = uvmdealloc(p->pagetable, sz, sz + n);
    }
    p->sz = sz;
    return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int kfork(void) {
    int i, pid;
    struct proc *np;
    struct proc *p = myproc();
    #ifdef PROC_TEST_TIME
    uint64 kfork_start_time = r_cycle();
    #endif
    #ifdef DEBUG_FORK
    FORK_TRACE("ENTRY parent pid=%d name='%s' sz=0x%llx mask=0x%x\n", 
               p->pid, p->name, p->sz, p->syscall_mask);
    #endif
    // Allocate process(process control block).
    // Critical: allocproc() acquires p->lock of the new process.
    // NOTE: (Still holding the process's private lock upon return.)
    if ((np = allocproc()) == 0) {
        #ifdef DEBUG_FORK
        FORK_TRACE("FAIL allocproc returned 0 (no free procs)\n");
        #endif
        return -1;
    }
    #ifdef DEBUG_FORK
    FORK_TRACE("allocproc success: child pid=%d pt=%p\n", np->pid, np->pagetable);
    FORK_TRACE("START uvmcopy: parent_pt=%p -> child_pt=%p sz=0x%llx\n", 
               p->pagetable, np->pagetable, p->sz);
    #endif
    // Copy user memory from parent to child.
    memmove(np->rb_array, p->rb_array, sizeof(struct Reservation)*MAX_RES_BLOCK);   //copy the rb_array first
    if (uvmcopy(np->rb_array, p->pagetable, np->pagetable, p->sz) < 0) {
        #ifdef DEBUG_FORK
        FORK_TRACE("FAIL uvmcopy error for child pid=%d\n", np->pid);
        #endif
        freeproc(np);
        release(&np->lock);     //Release the lock got from allocproc()
        return -1;
    }
    np->sz = p->sz;
    #ifdef DEBUG_FORK
    FORK_TRACE("END uvmcopy: success. child sz=0x%llx\n", np->sz);
    #endif
    
    // Copy saved user registers.
    *(np->trapframe) = *(p->trapframe);
    // NOTE: Duplicate all user stacks and registers,only modifying the a0 register
    // as the fork return value,so we can tell child and parent from return value.
    np->trapframe->a0 = 0;

    // Increment reference counts on open file descriptors.
    for (i = 0; i < NOFILE; i++)
        if (p->ofile[i]) np->ofile[i] = filedup(p->ofile[i]);
    // Duplicate the current working directory.
    np->cwd = idup(p->cwd);
    // Copy the process name
    safestrcpy(np->name, p->name, sizeof(p->name));
    pid = np->pid;
    np->syscall_mask = p->syscall_mask;
    safestrcpy(np->allow_path_str, p->allow_path_str, MAXPATH);

    // Lock Ordering Dance (Avoid Deadlock)
    // NOTE: Current np is set USED,make it private and inaccessiale to the scheduler.
    // Wait_lock is used to modify parent-child relationship between processes.
    //A potential deadlock scenario exists: 
    //  CPU A holds p->lock and waits for wait_lock, while CPU B holds wait_lock and waits for p->lock
    //Principle:the global wait_lock must be acquired before the specific p->lock—we must first release the process lock.
    release(&np->lock);
    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);
    acquire(&np->lock);
    np->state = RUNNABLE;
    #ifdef DEBUG_FORK
        FORK_TRACE("STATE UPDATE: child pid=%d is now RUNNABLE\n", np->pid);
    #endif
    #ifdef PROC_TEST_TIME
    uint64 kfork_end_time = r_cycle();
    if(kfork_end_time - kfork_start_time > 10000){
        printf("PERF: fork pid %d took %lld cycles\n", np->pid, kfork_end_time - kfork_start_time);
    }
    #endif
    release(&np->lock);
    #ifdef DEBUG_FORK
        FORK_TRACE("EXIT returns child pid=%d\n", pid);
    #endif
    return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p) {
    struct proc *pp;

    for (pp = proc; pp < &proc[NPROC]; pp++) {
        if (pp->parent == p) {
            pp->parent = initproc;
            wakeup(initproc);
        }
    }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() and reclaim its resources.NOTE:
void kexit(int status) {
    struct proc *p = myproc();

    if (p == initproc) panic("init exiting");

    // Close all open files.
    for (int fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd]) {
            struct file *f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;

    acquire(&wait_lock);

    // Give any children to init.
    reparent(p);

    // Parent might be sleeping in wait().
    wakeup(p->parent);  //or send SIGCHLD to wake up(LINUX)

    acquire(&p->lock);

    p->xstate = status; //eXit state
    p->state = ZOMBIE;

    release(&wait_lock);

    // Jump into the scheduler, never to return.
    scheduleProcess();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int kwait(uint64 addr) {
    struct proc *pp;
    int havekids, pid;
    struct proc *p = myproc();

    acquire(&wait_lock);

    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;   //Initially assume no children are ready.
        for (pp = proc; pp < &proc[NPROC]; pp++) {
            if (pp->parent == p) {
                // make sure the child isn't still in exit() or swtch().
                // e.g. for kfork(): child must release its process lock 
                // and acquire wait_lock to update the relationship,so now the child's private lock can acquire.
                acquire(&pp->lock);

                havekids = 1;
                if (pp->state == ZOMBIE) {
                    // Found one.
                    pid = pp->pid;
                    if (addr != 0 &&
                        copyout(p->pagetable, addr, (char *)&pp->xstate, sizeof(pp->xstate)) < 0) {
                        release(&pp->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    freeproc(pp);
                    release(&pp->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&pp->lock);
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || killed(p)) {
            release(&wait_lock);
            return -1;
        }

        // Wait for a child to exit.
        sleep(p, &wait_lock);  // DOC: wait-sleep
    }
}

// Per-CPU process scheduler.
// (NOTE:  Not residing in any process,on the cpu's dedicated stack)
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control via swtch back to the scheduler.
void scheduler(void) {
    struct proc *p;
    struct cpu *c = mycpu();

    c->proc = 0;
    for (;;) {
        // The most recent process to run may have had interrupts
        // turned off; enable them to avoid a deadlock if all
        // processes are waiting. Then turn them back off
        // to avoid a possible race between an interrupt
        // and wfi.
        intr_on();
        intr_off();

        int found = 0;
        for (p = proc; p < &proc[NPROC]; p++) {
            acquire(&p->lock);  //Only holding process's lock,cpu can take control it.
            if (p->state == RUNNABLE) {
                // Switch to chosen process.  It is the process's job
                // to release its lock and then reacquire it
                // before jumping back to us.
                p->state = RUNNING;
                c->proc = p;
                // Save my context(include ra,sp and all callee-save register:s0-s11)to c->context,
                // and then load the registers from p->context(where we going to)
                // While finishing switching, we will execute p->context(a new process)
                // While finishing this new process, we will resuming execution at this point(using ra and ret).
                swtch(&c->context, &p->context);

                // Process is done running for now.
                // It should have changed its p->state before coming back.
                c->proc = 0;
                found = 1;
            }
            release(&p->lock);
        }
        if (found == 0) {
            // nothing to run; stop running on this core until an interrupt.
            asm volatile("wfi");    //Full expansion:Wait for interrupt
        }
    }
}

// Switch to scheduler.  Must hold only p->lock and have changed proc->state. 
// Saves and restores intena because intena is a property of this kernel thread, 
// not this CPU. It should be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but there's no process.
//NOTE: It is called by process itself, run on the kernel stack of process. 
void scheduleProcess(void) {
    int intena;         //full name is: INTerrupt ENAble.
    struct proc *p = myproc();

    if (!holding(&p->lock)) panic("scheduleProcess p->lock");
    if (mycpu()->noff != 1){
        print_held_locks();
        panic("scheduleProcess locks(Sleep while holding a lock.)");
    }
    if (p->state == RUNNING) panic("scheduleProcess RUNNING");
    if (intr_get()) panic("scheduleProcess interruptible");

    intena = mycpu()->intena;

    //Save my(current process) registers to p->context
    // NOTE: (ra indicate where to continue,sp, all callee-save registers),
    //and then load the registers from mycpu->context(already prepared in scheduler())
    //to restore the scheduler loop state and back to the scheduler.
    swtch(&p->context, &mycpu()->context);

    //--------Resumption Point-------------------
    //At this stage, the cpu has returned from the scheduler)
    mycpu()->intena = intena;   //Restore the 'intena' state to the value saved before switching.
}

// Different from the Spinlock, while acquiring mutex fail,process will sleep, yield 
// cpu for other mission, involve context switch.
// Give up the CPU for one scheduling round.
void yield(void) {
    struct proc *p = myproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    scheduleProcess();
    release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void) {
    extern char userret[];
    static int first = 1;
    struct proc *p = myproc();

    // Still holding p->lock from scheduler.
    release(&p->lock);

    if (first) {
        // File system initialization must be run in the context of a
        // regular process (e.g., because it calls sleep), and thus cannot
        // be run from main().
        fsinit(ROOTDEV);

        first = 0;
        // ensure other cores see first=0.
        __sync_synchronize();

        // We can invoke kexec() now that file system is initialized.
        // Put the return value (argc) of kexec into a0.
        p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
        //And exec can replace current process,prepares the system so thata a new program can 
        //be executed upon returning to user space(involves epc, sp, new pagetable, loading elf e.t.c)
        if (p->trapframe->a0 == -1) {
            panic("exec");
        }
    }

    // return to user space, mimicing usertrap()'s return.
    prepare_return();   //change the stvec from kerneltrap to usertrap
    uint64 satp = MAKE_SATP(p->pagetable);  //function parameter.
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);//fnction pointer
    ((void (*)(uint64))trampoline_userret)(satp);
}

// waitchannel is a address.When process A call sleep(&x, &lock), it informs the kernel:
// "If someone sends messages to address &x(By calling wakeup(&x)), please wake up me!"
// For second parameter, make sure release it before I sleep
// and make sure you re-acquire this lock for me when I wake up.
void sleep(void *waitChannel, struct spinlock *lk) {
    struct proc *p = myproc();

    // Must acquire p->lock in order to change p->state and then call scheduleProcess.
    // Once we hold p->lock, we can be guaranteed that we won't miss any wakeup
    // (TIPS: Wakeup require holding p->lock, but we hold p->lock first.So wakeup continue waiting.)
    // (Otherwise we will miss wakeup if we release wakeup first and then acquire p->lock)
    // so it's okay to release lk.

    acquire(&p->lock);  // DOC: sleeplock1

    //Internal release for concurrency(safe release,Since the intr disabled.)
    release(lk);

    struct cpu *cur_cpu=mycpu();
    if(cur_cpu->noff!=1){
        printf("FATAL: Sleep() holding extra lock (potential deadlock).\n");
        print_held_locks();
        panic("Sleep locks.\n");
    }

    // Go to sleep.
    p->waitChannel = waitChannel;
    p->state = SLEEPING;

    scheduleProcess();

    // Tidy up, waiting anymore.
    p->waitChannel = 0;

    // Reacquire original lock.The caller of sleep assumes the lock is held
    // throughout the logical blocks,therefore, sleep must be reacquire it before
    // returning to maintain the atomicity invariant.
    release(&p->lock);
    acquire(lk);
}

// Wake up all processes sleeping on channel waitChannel.
// Caller should hold the condition lock.
void wakeup(void *waitChannel) {
    struct proc *p;
    //Sequentially acquires process lock to check conditions, updating the state
    // make it runnable.(Broadcast)
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p != myproc()) {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->waitChannel == waitChannel) {
                p->state = RUNNABLE;
            }
            release(&p->lock);
        }
    }
}

void wakeup_one(void *waitChannel){
    struct proc *p;
    // (NOTE: Different from wakeup, exit immediately once wakeup one process successfully)
    // Avoid thundering herd.(Unicast)
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p != myproc()) {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->waitChannel == waitChannel) {
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return; 
        }
    }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kkill(int pid) {
    struct proc *p;

    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            p->killed = 1;
            if (p->state == SLEEPING) {
                // Wake process from sleep().
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

void setkilled(struct proc *p) {
    acquire(&p->lock);
    p->killed = 1;
    release(&p->lock);
}

int killed(struct proc *p) {
    int k;

    acquire(&p->lock);
    k = p->killed;
    release(&p->lock);
    return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len) {
    struct proc *p = myproc();
    if (user_dst) {
        return copyout(p->pagetable, dst, src, len);
    } else {
        memmove((char *)dst, src, len);
        return 0;
    }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len) {
    struct proc *p = myproc();
    if (user_src) {
        return copyin(p->pagetable, dst, src, len);
    } else {
        memmove(dst, (char *)src, len);
        return 0;
    }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
    static char *states[] = {[UNUSED] "unused",   [USED] "used",      [SLEEPING] "sleep ",
                             [RUNNABLE] "runble", [RUNNING] "run   ", [ZOMBIE] "zombie"};
    struct proc *p;
    char *state;

    printf("\n");
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED) continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state]) state = states[p->state];
        else
            state = "???";
        printf("%d %s %s", p->pid, state, p->name);
        printf("\n");
    }
}

struct proc *kthread_create(const char *name, void (*func)(void)){
    //create a new lightweight kernel thread
    struct proc *new_kthread;
    for(new_kthread=proc;new_kthread< &proc[NPROC];new_kthread++){
        acquire(&new_kthread->lock);
        if(new_kthread->state==UNUSED)
            goto found;
        else    release(&new_kthread->lock);
    }
    panic("Out-of-pcb.Unable to create kernel thread!");
    return NULL;
found:
    new_kthread->pid=allocpid();
    new_kthread->state=RUNNABLE;
    //Able to be scheduled by the cpu and enter the entry function for execution.
    safestrcpy(new_kthread->name, name, strlen(name));  //null-terminated
    memset(&new_kthread->context, 0, sizeof(new_kthread->context));
    new_kthread->context.ra=(uint64)func;
    new_kthread->context.sp=new_kthread->kstack+PGSIZE;
    release(&new_kthread->lock);
    //NOTE: Must release process's lock immideately,other scheduler can acquire and 
    //execute it on a corresponding CPU once it is detected as runnable.
    return (void *)new_kthread;
}
