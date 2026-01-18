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
        p->state = PROC_UNUSED;
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
        if (p->state == PROC_UNUSED) {
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
    p->state = PROC_USED;
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
    p->state = PROC_UNUSED;
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
    //initlock/reset pagetable lock at the same time
    initlock(&p->uvm_lock, "User_process lock");
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

    p->state = PROC_RUNNABLE;

    release(&p->lock);
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n) {
    uint64 sz;
    struct proc *p = myproc();
    if(!holding(&p->uvm_lock))
        panic("access user's pagetable without lock.\n");
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
    struct proc *new_child;
    struct proc *cur_parent = myproc();

    // Allocate process(process control block).
    // Critical: allocproc() acquires p->lock of the new process.
    // NOTE: (Still holding the process's private lock upon return.)
    if ((new_child = allocproc()) == 0) 
        return -1;

    // Copy user memory from parent to child.
    memmove(new_child->rb_array, cur_parent->rb_array, sizeof(struct Reservation)*MAX_RES_BLOCK);   //copy the rb_array first
    acquire(&cur_parent->uvm_lock);

    if (uvmcopy(new_child->rb_array, cur_parent->pagetable, new_child->pagetable, cur_parent->sz) < 0) {

        freeproc(new_child);
        release(&new_child->lock);     //Release the lock got from allocproc()
        release(&cur_parent->uvm_lock);
        return -1;
    }
    new_child->sz = cur_parent->sz;
    release(&cur_parent->uvm_lock);

    // Copy saved user registers.
    *(new_child->trapframe) = *(cur_parent->trapframe);
    // NOTE: Duplicate all user stacks and registers,only modifying the a0 register
    // as the fork return value,so we can tell child and parent from return value.
    new_child->trapframe->a0 = 0;

    // Increment reference counts on open file descriptors.
    for (i = 0; i < NOFILE; i++)
        if (cur_parent->ofile[i]) new_child->ofile[i] = filedup(cur_parent->ofile[i]);
    // Duplicate the current working directory.
    new_child->cwd = idup(cur_parent->cwd);
    // Copy the process name
    safestrcpy(new_child->name, cur_parent->name, sizeof(cur_parent->name));
    pid = new_child->pid;
    new_child->syscall_mask = cur_parent->syscall_mask;
    safestrcpy(new_child->allow_path_str, cur_parent->allow_path_str, MAXPATH);

    // Lock Ordering Dance (Avoid Deadlock)
    // NOTE: Current np is set USED,make it private and inaccessiale to the scheduler.
    // Wait_lock is used to modify parent-child relationship between processes.
    //A potential deadlock scenario exists: 
    //  CPU A holds p->lock and waits for wait_lock, while CPU B holds wait_lock and waits for p->lock
    //Principle:the global wait_lock must be acquired before the specific p->lock—we must first release the process lock.

    release(&new_child->lock);
    acquire(&wait_lock);
    new_child->parent = cur_parent;
    release(&wait_lock);
    acquire(&new_child->lock);
    new_child->state = PROC_RUNNABLE;

    release(&new_child->lock);

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
    p->state = PROC_ZOMBIE;

    release(&wait_lock);

    // Jump into the scheduler, never to return.
    scheduleProcess();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int kwait(uint64 addr) {
    struct proc *tmp_p;
    int havekids, pid;
    struct proc *p = myproc();

    acquire(&wait_lock);

    for (;;) {
        // Scan through table looking for exited children.
        havekids = 0;   //Initially assume no children are ready.
        for (tmp_p = proc; tmp_p < &proc[NPROC]; tmp_p++) {
            if (tmp_p->parent == p) {
                // make sure the child isn't still in exit() or swtch().
                // e.g. for kfork(): child must release its process lock 
                // and acquire wait_lock to update the relationship,so now the child's private lock can acquire.
                acquire(&tmp_p->lock);

                havekids = 1;
                if (tmp_p->state == PROC_ZOMBIE) {
                    // Found one.
                    pid = tmp_p->pid;
                    if (addr != 0 &&
                        copyout(p->pagetable, addr, (char *)&tmp_p->xstate, sizeof(tmp_p->xstate)) < 0) {
                        release(&tmp_p->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    freeproc(tmp_p);
                    release(&tmp_p->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&tmp_p->lock);
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
            if (p->state == PROC_RUNNABLE) {
                // Switch to chosen process.  It is the process's job
                // to release its lock and then reacquire it
                // before jumping back to us.
                p->state = PROC_RUNNING;
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
    if (p->state == PROC_RUNNING) panic("scheduleProcess RUNNING");
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
    p->state = PROC_RUNNABLE;
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
    p->state = PROC_SLEEPING;

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
            if (p->state == PROC_SLEEPING && p->waitChannel == waitChannel) {
                p->state = PROC_RUNNABLE;
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
            if (p->state == PROC_SLEEPING && p->waitChannel == waitChannel) {
                p->state = PROC_RUNNABLE;
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
            if (p->state == PROC_SLEEPING) {
                // Wake process from sleep().
                p->state = PROC_RUNNABLE;
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
        acquire(&p->uvm_lock);
        int tmp_ret=copyout(p->pagetable, dst, src, len);
        release(&p->uvm_lock);
        return tmp_ret;
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
        acquire(&p->uvm_lock);
        int tmp_ret=copyin(p->pagetable, dst, src, len);
        release(&p->uvm_lock);
        return tmp_ret;
    } else {
        memmove(dst, (char *)src, len);
        return 0;
    }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
    static char *states[] = {[PROC_UNUSED] "unused",   [PROC_USED] "used",      [PROC_SLEEPING] "sleep ",
                             [PROC_RUNNABLE] "runble", [PROC_RUNNING] "run   ", [PROC_ZOMBIE] "zombie"};
    struct proc *p;
    char *state;

    printf("\n");
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == PROC_UNUSED) continue;
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
        if(new_kthread->state==PROC_UNUSED)
            goto found;
        else    release(&new_kthread->lock);
    }
    panic("Out-of-pcb.Unable to create kernel thread!");
    return NULL;
found:
    new_kthread->pid=allocpid();
    new_kthread->state=PROC_RUNNABLE;
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
