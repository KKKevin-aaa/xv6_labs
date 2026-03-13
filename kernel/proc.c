#include "types.h"
#include "param.h"
#include "atomic.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "fcntl.h"
#include "mm.h"
#include "sbi.h"
#include "vm.h"
#include "colors.h"
#include "utils.h"

//Enable COW 
#define COW

// #define PROC_TEST_TIME 
struct cpu cpus[NCPU];
struct mailbox req_mailbox[NCPU];
static slab_cache_t * tlb_data_cache=NULL;

/// @brief static pcb array
struct proc proc[NPROC];

struct proc *initproc;
struct proc *swap_kthread=NULL;


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
    void kvmmap_boot_only(struct map_context *ctx);
    for (p = proc; p < &proc[NPROC]; p++) {
        char *pa = kalloc_page();
        if (pa == 0) panic("kalloc");
        uint64 va = KSTACK((int)(p - proc));
        pr_info("proc %d: kernel_stack located from %llx to 0x%llx", 
            (int)(p-proc), va, va+PGSIZE);
        pr_info("while guard page located from 0x%llx to 0x%llx", 
            va-PGSIZE, va);
        kvmmap_boot_only(&(struct map_context){
            .pagetable=kpgtbl,
            .start_va=va,
            .pa=(uint64)pa,
            .size=PGSIZE,
            .xperm=PTE_R | PTE_W,
            .pt_lock=NULL
        });
        pr_info("\n");      //Seperate.
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
        p->kstack = KSTACK((int)(p - proc));        //kernel stack starting address.
        initlock(&p->uvm_lock, "proc pagetable");
    }
    swap_kthread=kthread_create("swap_worker", swap_out);
    #ifdef DEBUG_KALLOC
        KALLOC_TRACE("initialization complete\n");
    #endif
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
            mm_put(p->mm);
            p->mm=mm_create();  //Already clean on alloc.
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
    // Build the relationship between mm_struct and pagetable
    p->mm->pagetable=p->pagetable;  //An empty pagetable.

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
    if(!holding(&p->lock)){
        pr_err("access without proc's lock.\n");
        return;
    }
    acquire(&p->uvm_lock);
    if (p->trapframe) kfree_page((void *)p->trapframe);
    p->trapframe = 0;
#ifdef PROC_DEBUG
    pr_info("in freeproc oldpagetbale is %p\n", p->pagetable);
#endif
    if(p->mm!=NULL)     mm_put(p->mm);
    //free the pagetable at the same time.(double -check)
#ifdef PROC_DEBUG
    pr_info("Done free this process's memory!\n");
#endif
    p->pagetable = 0;
    p->pid = 0;
    p->parent = 0;
    p->name[0] = 0;
    p->waitChannel = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = PROC_UNUSED;
    release(&p->uvm_lock);
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
    if(mappages(&(struct map_context)
        {.pagetable=pagetable,
        .start_va=TRAMPOLINE,
        .size=PGSIZE,
        .pa=(uint64)trampoline,
        .xperm=PTE_R | PTE_X,
        .pt_lock=&p->uvm_lock}) <0 ){
        uvmremove(pagetable);
        return 0;
    }
    //map the trapframe page just below the trampoline page, for trampoline.S.
    if(mappages(&(struct map_context){
        .pagetable=pagetable,
        .start_va=TRAPFRAME,
        .size=PGSIZE,
        .pa=(uint64)(p->trapframe),
        .xperm=PTE_R | PTE_W,
        .pt_lock=&p->uvm_lock}) <0 ){
        uvmunmap(&(struct map_context){
            .pagetable=pagetable,
            .start_va=TRAMPOLINE,
            .size=PGSIZE,
            .pt_lock=&p->uvm_lock,
            .do_free=0
        });
        uvmremove(pagetable);
        return 0;
    }
    // establish a shared, read-only mapping at USYSCALL to speed up syscalls via direct memory address.
    if(mappages(&(struct map_context)
        {.pagetable=pagetable,
        .start_va=USYSCALL,
        .size=PGSIZE,
        .pa=(uint64)usyscall_pa,
        .xperm=PTE_R | PTE_U,
        .pt_lock=&p->uvm_lock}) <0 ){
        uvmunmap(&(struct map_context){
            .pagetable=pagetable,
            .start_va=TRAMPOLINE,
            .size=PGSIZE,
            .pt_lock=&p->uvm_lock,
            .do_free=0
        });
        uvmunmap(&(struct map_context){
            .pagetable=pagetable,
            .start_va=TRAPFRAME,
            .size=PGSIZE,
            .pt_lock=&p->uvm_lock,
            .do_free=0
        });
        uvmremove(pagetable);
        return 0;
    }
    //initlock/reset pagetable lock at the same time
    initlock(&p->uvm_lock, "User_process lock");
    return pagetable;
}

// Free a process's page table, and free the physical memory it refers to.
void proc_freepagetable(struct spinlock *pt_lock, pagetable_t pagetable) {
    uvmunmap(&(struct map_context){
        .pagetable=pagetable,
        .start_va=TRAMPOLINE,
        .size=PGSIZE,
        .pt_lock=pt_lock,
        .do_free=0
    });
    uvmunmap(&(struct map_context){
        .pagetable=pagetable,
        .start_va=TRAMPOLINE,
        .size=PGSIZE,
        .pt_lock=pt_lock,
        .do_free=0
    });
    uvmunmap(&(struct map_context){
        .pagetable=pagetable,
        .start_va=TRAPFRAME,
        .size=PGSIZE,
        .pt_lock=pt_lock,
        .do_free=0
    });
    //All process share one usyscall page,so don' free here!
    uvmremove(pagetable);       //Remove this pagetable completely.
}

// Set up first user process.
void userinit(void) {
    struct proc *p;

    p = allocproc();
    initproc = p;
    release(&p->lock);  
    // Potentially Acquiring a sleeplock while interrupts 
    // are disabled(holding spinlock) leads to indefinite blocking of the execution flow
    p->cwd = namei("/");
    acquire(&p->lock);
    p->state = PROC_RUNNABLE;

    release(&p->lock);
}

#ifndef RESERVE
// Shrink user memory by n bytes. Return 0 on success, -1 on failure.
//(Eager allocation.!!)
int growproc(int n) {       //Ensure enter this function holding two locks(uvmlock and mm_lock)
    struct proc *cur_proc = myproc();
    pr_info("Entering growproc.");
    if(!holding(&cur_proc->uvm_lock)){
        pr_err("access user's pagetable without lock.\n");
        return -1;
    }
    if(cur_proc->mm==NULL){
        pr_err("Fatal error: proc's mm shouldn't be NULL.\n");
        return -1;
    }
    if(!holdingsleep(&cur_proc->mm->mm_lock))
        pr_err("[Warning]Access process's mm_struct_t without lock(Protected by uvmlock).\n");
    if(cur_proc->mm->heap_vma==NULL){      //check the heap vma
        pr_err("Fatal error: proc's heap_vma shouldn't be NULL.\n");
        return -1;
    }

    if(n==0)    return 0;
    uint64 heap_end=cur_proc->mm->heap_vma->vm_end, limit;
    //Prevent arithmetic overflow caused by an excessively large n.
    if(n>0){
        vm_area_struct_t *next_vma=cur_proc->mm->heap_vma->vm_next;
        if(next_vma==NULL)  limit=UPPER_LIMIT;
        else    limit=next_vma->vm_start;       //Mmap vma
        if(heap_end + n > limit || heap_end + n < heap_end){
            //Prevent excessive n from overwriting kernel memory
            //Alos prevent integer overflow.
            pr_warn("No space to grow, remain space is %llx\n.\n", limit-heap_end);
            return -1;
        }
        // Allocate immediately, bypassing the special case of COW.
        if((heap_end = uvmalloc_thp_region(&(struct alloc_context){
            fixme: 
            .pagetable=cur_proc->pagetable,
            .pt_lock=&cur_proc->uvm_lock,
            .rblocks=cur_proc->rb_array,
            .seg_start=heap_end,
            .seg_end=heap_end + n,
            .xperm=cur_proc->mm->heap_vma->vm_page_prot
        }))==0)
            return -1;
    }
    else{   //The case where n==0 has been explicitly excluded.
        // (n can only be less than zero.)
        limit=cur_proc->mm->heap_vma->vm_start;
        if(-n > heap_end - limit){
            //Avoid direct arithmetic between unsigned and signed number.
            pr_warn("Reached the prev_vma boundary, can't shrink to that position.");
            return -1;
        }
        heap_end = uvmdealloc(&(struct alloc_context){
            .pagetable=cur_proc->pagetable,
            .do_free =1,
            .pt_lock=&cur_proc->uvm_lock,
            .seg_start=heap_end +n,
            .seg_end=heap_end,
            .rblocks=cur_proc->rb_array,
        });
        if(heap_end != cur_proc->mm->heap_vma->vm_end+n)
            return -1;
    }
    cur_proc->mm->heap_vma->vm_end=heap_end; //update the heap boundary
    return 0;
}
#else
int growproc(int n) {       //Ensure enter this function holding two locks(uvmlock and mm_lock)
    struct proc *cur_proc = myproc();
    pr_info("Entering growproc.");
    if(!holding(&cur_proc->uvm_lock)){
        pr_err("access user's pagetable without lock.\n");
        return -1;
    }
    if(cur_proc->mm==NULL){
        pr_err("Fatal error: proc's mm shouldn't be NULL.\n");
        return -1;
    }
    if(!holdingsleep(&cur_proc->mm->mm_lock))
        pr_err("[Warning]Access process's mm_struct_t without lock(Protected by uvmlock).\n");
    if(cur_proc->mm->heap_vma==NULL){      //check the heap vma
        pr_err("Fatal error: proc's heap_vma shouldn't be NULL.\n");
        return -1;
    }

    if(n==0)    return 0;
    uint64 heap_end=cur_proc->mm->heap_vma->vm_end, limit;
    //Prevent arithmetic overflow caused by an excessively large n.
    if(n>0){
        if(expand_vma(cur_proc->mm->heap_vma, cur_proc->mm->heap_vma->vm_start, 
                    cur_proc->mm->heap_vma->vm_end + n)==-1){
            pr_warn("expanding vma boundary fail.");
            return -1;
        }
        // Allocate immediately, bypassing the special case of COW.
        if((heap_end = vmalloc(&(struct alloc_context){
            .pagetable=cur_proc->pagetable,
            .pt_lock=&cur_proc->uvm_lock,
            .rblocks=cur_proc->rb_array,
            .seg_start=heap_end,
            .seg_end=heap_end + n,
            .xperm=cur_proc->mm->heap_vma->vm_page_prot
        }))==0){
            pr_err("Uvmalloc fail.");
            if(shrink_vma(cur_proc->mm->heap_vma, 
                cur_proc->mm->heap_vma->vm_start, heap_end)==-1)
                panic("Unhandled error while reseting vma.");
            return -1;
        }
    }
    else{   //The case where n==0 has been explicitly excluded.
        // (n can only be less than zero.) And NOTE: heap_end(uint64) + n(int) will be treated as uint64.
        limit=cur_proc->mm->heap_vma->vm_start;
        if(-n > heap_end - limit){
            //Avoid direct arithmetic between unsigned and signed number.
            pr_warn("Reached the prev_vma boundary, can't shrink to that position.");
            return -1;
        }
        if(shrink_vma(cur_proc->mm->heap_vma, 
            cur_proc->mm->heap_vma->vm_start, heap_end+n)==-1){
            pr_err("Shrinking vma boundary fail.");
            return -1;
        }
        if(uvmdealloc(&(struct alloc_context){
            .pagetable=cur_proc->pagetable,
            .pt_lock=&cur_proc->uvm_lock,
            .rblocks=cur_proc->rb_array,
            .seg_start=heap_end+n,
            .seg_end=heap_end,
            .do_free=1
        }) !=heap_end){
            pr_err("Uvmdealloc fail.");
            if(expand_vma(cur_proc->mm->heap_vma, 
                cur_proc->mm->heap_vma->vm_start, heap_end)==-1)
                panic("Unhandled error while reseting vma.");
            return -1;
        }
    }
    return 0;
}
#endif

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
    if(new_child->mm==NULL){
        pr_err("Invalid proc without mm_struct_t\n");
        freeproc(new_child);
        release(&new_child->lock);
        return -1;
    }
    release(&new_child->lock);
    //'Casue it's guaranteed not be used by any other function(Special statement)
    //SO we can release this lock firstly, 

    // Copy user memory from parent to child.
    memmove(new_child->rb_array, cur_parent->rb_array, sizeof(struct Reservation)*MAX_RES_BLOCK);
    //copy the rb_array first
    acquiresleep(&cur_parent->mm->mm_lock);
    acquiresleep(&new_child->mm->mm_lock);   //Locks required for mm_struct_t cloning
    acquire(&cur_parent->uvm_lock);
    acquire(&new_child->uvm_lock);      //Locks required for pagetable cloning
    //Create a new one mm_struct(value copy/Deep copy)--Metadata
    vm_area_struct_t *copy_vma=cur_parent->mm->mmap , *tmp_next=NULL, *prev_vma=NULL;
    vm_area_struct_t *new_vma=NULL;
    int cow_occur=0;  //Ensure the IPI is triggered only once via a flag.
    while(copy_vma!=NULL){
        tmp_next=copy_vma->vm_next;
        new_vma=vma_dup(copy_vma);
        //Integrating the new VMA into the Address space.
        new_vma->vm_mm=new_child->mm;
        if(insert_vma_fast(new_child->mm, new_vma, &(vma_context_t){.prev=prev_vma, .next=NULL})!=0){
            if(new_vma->vm_file!=NULL)
                fileclose(new_vma->vm_file);
            if(new_vma->vm_ops!=NULL && new_vma->vm_ops->open!=NULL)
                new_vma->vm_ops->close(new_vma);
            vma_put(new_vma);
            goto error_on_copy;
        }
        //Physical memory Replication
    #ifndef COW
        if(uvmcopy_range(new_child->rb_array, cur_parent->pagetable, new_child->pagetable, 
                copy_vma->vm_start, copy_vma->vm_end - copy_vma->vm_start)<0){
            goto error_on_copy;
        }
    #else   //Adopt sharing mechanism, Only VM_write + VM_shared require COW intervention,
    // While others can be unified as copying PTE and incrementing the physical reference count(Optional)
        struct vm_dupl_ctx ctx1={.base_va=new_vma->vm_start,
            .end_va=new_vma->vm_end,
            .src_pg=cur_parent->pagetable,
            .dst_pg=new_child->pagetable,
            .new_rblocks=new_child->rb_array,
            .old_rblocks=cur_parent->rb_array,
            .dst_level=2};
        if(new_vma->vm_flags & (VM_IO | VM_PFNMAP)){     //NOTE: Highest priority
            if(uvmcopy_range_direct_shared(&ctx1)<0)
                goto error_on_copy;
        }
        else if((new_vma->vm_flags & VM_WRITE) && !(new_vma->vm_flags & VM_SHARED)){
            if(uvmcopy_range_cow(&ctx1)<0)
                goto error_on_copy;
            // NOTE: PTE permission downgrade or page splitting detected.Cross-core TLB via IPI 
            // is mandatory to ensure memory consistency.
            cow_occur=1;
        }
        else{
            if(uvmcopy_range_shared(&ctx1)<0)
                goto error_on_copy;
        }
    #endif
        //Synchronize metadata in mm_struct(Pointer parameter)
        if(copy_vma==cur_parent->mm->heap_vma)
            new_child->mm->heap_vma=new_vma;
        else if(copy_vma==cur_parent->mm->stack_vma)
            new_child->mm->stack_vma=new_vma;

        prev_vma=new_vma;
        copy_vma=tmp_next;
    }
    //Invariants
    new_child->mm->arg_start=cur_parent->mm->arg_start;
    new_child->mm->arg_end=cur_parent->mm->arg_end;
    new_child->mm->env_start=cur_parent->mm->env_start;
    new_child->mm->env_end=cur_parent->mm->env_end;
    new_child->mm->start_code=cur_parent->mm->start_code;
    new_child->mm->end_code=cur_parent->mm->end_code;
    new_child->mm->start_data=cur_parent->mm->start_data;
    new_child->mm->end_data=cur_parent->mm->end_data;
    
    if(cow_occur==1)
        tlb_shootdown_issue_nolock(cur_parent->pagetable, 0, 2 * IPI_REQ_THRESHOLD * PGSIZE);
    release(&new_child->uvm_lock);
    release(&cur_parent->uvm_lock);
    releasesleep(&new_child->mm->mm_lock);
    releasesleep(&cur_parent->mm->mm_lock);
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
    // NOTE: Current np is set USED, make it private and inaccessiale to the scheduler.
    // Wait_lock is used to modify parent-child relationship between processes.
    // A potential deadlock scenario exists: 
    //  CPU A holds p->lock and waits for wait_lock, while CPU B holds wait_lock and waits for p->lock
    //Principle:the global wait_lock must be acquired 
    //          before the specific p->lock.So we must first release the process lock.

    //release(&new_child->lock);
    acquire(&wait_lock);
    new_child->parent = cur_parent;
    release(&wait_lock);
    acquire(&new_child->lock);
    new_child->state = PROC_RUNNABLE;

    release(&new_child->lock);
    return pid;
error_on_copy:
    release(&cur_parent->uvm_lock);
    release(&new_child->uvm_lock);
    releasesleep(&new_child->mm->mm_lock);
    releasesleep(&cur_parent->mm->mm_lock);

    acquire(&new_child->lock);  //acquire the child'lock for reclaim its resource.
    freeproc(new_child);        //remove complete pagetable and mm_struct
    release(&new_child->lock);
    pr_err("Error in kfrok.");
    return -1;
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

    //handle teardown involving potential file operations.
    //Although mm_struct assoicated with pagetable, we use kernel pagetable here.
    //And will not use its outdated pagetable anymore, so delete it is safe.
    mm_put(p->mm);      //Private variant.Immutable for external processes.
    //Use the lock from mm_struct_t itself. 

    p->mm=NULL; //Critical!Reset state explicitly.Pervent double-free or Use-after-free.

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
                // and acquire wait_lock to update the relationship,
                // so now the child's private lock can acquire.
                acquire(&tmp_p->lock);
                havekids = 1;
                if (tmp_p->state == PROC_ZOMBIE) {
                    // Found one.
                    pid = tmp_p->pid;
                    acquire(&p->uvm_lock);      //for copyout;
                    if (addr != 0 &&
                        copyout(p->pagetable, addr, (char *)&tmp_p->xstate, sizeof(tmp_p->xstate)) < 0) {
                        release(&p->uvm_lock);
                        release(&tmp_p->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    release(&p->uvm_lock);
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
        // The most recent process to run may have had interrupts turned off; 
        // enable them to avoid a deadlock if all processes are waiting. 
        // Then turn them back off to avoid a possible race between an interrupt and wfi.
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
                // While finishing this new process, 
                // we will resuming execution at this point(using ra and ret).
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
    static int first = 1;   //Located in Data Segment, shared by all process.
    struct proc *p = myproc();

    // Still holding p->lock from scheduler.
    release(&p->lock);
    // ensure other cores see first=0.
    if(__sync_lock_test_and_set(&first, 0)==1){
        // File system initialization must be run in the context of a
        // regular process (e.g., because it calls sleep), and thus cannot
        // be run from main().
        fsinit(ROOTDEV);
        __sync_synchronize();

        // We can invoke kexec() now that file system is initialized.
        // Put the return value (argc) of kexec into a0.
        p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
        //And exec can replace current process,prepares the system so thata a new program can 
        //be executed upon returning to user space(involves epc, sp, new pagetable, loading elf e.t.c)
        if (p->trapframe->a0 == -1) {
            panic("exec");
        }
        //Initialization must be deferred until the scheduler is operational,
        // as it relies on sleeeplocks and requires process-specific metadata.
        debugsym_init();     // debug sym info(for backtrace)
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
        pr_err("FATAL: Sleep() holding extra lock (potential deadlock).\n");
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

void init_tlb_data_cache(void){
    tlb_data_cache=create_slab_cache("tlb_data_pool", 
        sizeof(struct tlb_shootdown_req), 8, NULL, NULL);
    if(tlb_data_cache==NULL)
        panic("init tlb_data_cache fail.Unable to establish data transport channel.");
}

void tlb_shootdown_issue(pagetable_t pgdir, uint64 va, uint64 len){
    push_off();
    int cur_cpuid=cpuid();
    if(NCPU > 32)
        panic("NCPU exceed the predefined length(32), plz expand!");
    volatile enum box_state *check_pos[NCPU];
    memset(check_pos, 0, sizeof(check_pos));
    uint32 target_mask=0, nr_send_req=0;
    int ready_quit=0;
    struct tlb_shootdown_req * ptr_buf[NCPU];
    memset(ptr_buf, 0, sizeof(ptr_buf));
    struct mailbox *cur_box=NULL;
    for(int i=0;i<NCPU;i++){
        if(i==cur_cpuid || cpus[i].proc==NULL)    continue;
        if(cpus[i].proc->pagetable==pgdir){     //Optimistic Concurrency Control(OCC)
            //read the statement without lock, double validate before use.
            cur_box=&req_mailbox[i];
            acquire(&cur_box->lock);
            uint64 free_slot = cur_box->tail % MAX_MAIL;
            while(cur_box->reqs[free_slot].status != BOX_EMPTY){
                release(&cur_box->lock);
                // asm volatile("nop");
                asm volatile(".word 0x0100000f" : : : "memory");
                acquire(&cur_box->lock);    //relock and check again
            }
            if(cur_box->reqs[free_slot].status != BOX_EMPTY)
                panic("Memory corrupted.");
            //Send one request to one mail(A single empty slot is enough)
            ptr_buf[i]=slab_alloc(tlb_data_cache);
            ptr_buf[i]->len=len;
            ptr_buf[i]->start_va=va;
            ptr_buf[i]->target_pgdir=pgdir;
            cur_box->reqs[free_slot].args=ptr_buf[i];
            cur_box->reqs[free_slot].func=do_flush_tlb;
            cur_box->reqs[free_slot].status=BOX_PENDING;
            check_pos[nr_send_req++]=&cur_box->reqs[free_slot].status;
            cur_box->tail++;    //advance.
            release(&cur_box->lock);
            target_mask |= gen_bitfield_mask(i, i, 32, 0);
        }
    }
    if(nr_send_req==0){
        pop_off();
        return;
    }
    if(sbi_send_ipi(target_mask, 0).error!=SBI_SUCCESS){
        //Starting hartid set as zero.
        panic("Tlb shootdown failed.");
    }
    while(ready_quit==0){
        if(r_sip() & SOFTWARE_INTR_MASK)  software_intr_handler();
        //Polling to handle incoming remote request.
        for(int i=0;i<=nr_send_req;i++){
            if(i==nr_send_req){
                ready_quit=1;
                break;
            }
            if(*check_pos[i] == BOX_PENDING)
                break;
            else if(*check_pos[i] == BOX_DONE)
                *check_pos[i]=BOX_EMPTY;    //release the slot.
        }
    }
    for(int i=0;i<NCPU;i++)
        //reclaim tlb_data and its cache struct
        if(ptr_buf[i]!=NULL)
            slab_free((void *)ptr_buf[i]);
    pop_off();  //Cannot assume interrupts are enabled.
}


void tlb_shootdown_issue_nolock(pagetable_t pgdir, uint64 va, uint64 len){
    //CAS, without lock.
    volatile enum box_state *check_pos[NCPU];
    memset(check_pos, 0, sizeof(check_pos));
    uint32 target_mask=0, nr_send_req=0;
    int ready_quit=0;
    struct tlb_shootdown_req * data1=slab_alloc(tlb_data_cache);
    if(data1==NULL)
        panic("OOM.");
    data1->len=len;
    data1->start_va=va;
    data1->target_pgdir=pgdir;
    struct mailbox *cur_box=NULL;
    push_off();     //Inhibit preemption to ensure hartid stability during communication.
    int cur_cpuid=cpuid(), tail=0;
    for(int i=0;i<NCPU;i++){
        if(i==cur_cpuid || cpus[i].proc==NULL)    continue;
        //Bypass the local hart and any harts without an active process context.
        if(cpus[i].proc->pagetable==pgdir){
            cur_box=&req_mailbox[i];
            tail=atomic_add_and_ret((void *)&cur_box->tail, 1);
            tail %= MAX_MAIL;
            while(cur_box->reqs[tail].status != BOX_EMPTY){     //spin-wait
                asm volatile(".word 0x0100000f" : : : "memory");
            }
            cur_box->reqs[tail].status=BOX_CLAIMED;
            cur_box->reqs[tail].args=data1;
            cur_box->reqs[tail].func=do_flush_tlb;
            check_pos[nr_send_req++]=&cur_box->reqs[tail].status;
            target_mask |= gen_bitfield_mask(i, i, 32, 0);
            asm volatile("fence rw, w" : : : "memory");
            cur_box->reqs[tail].status=BOX_PENDING;
        }
    }
    if(nr_send_req==0){
        slab_free((void *)data1);
        pop_off();
        return;
    }
    if(sbi_send_ipi(target_mask, 0).error != SBI_SUCCESS)
        //Starting hartid set as zero.
        panic("TLB shootdown failed.");
    while(ready_quit==0){
        if(r_sip() & SOFTWARE_INTR_MASK)    software_intr_handler_nolock();
        //Polling to handle incoming remote request.
        for(int i=0;i<=nr_send_req;i++){
            if(i==nr_send_req){
                ready_quit=1;
                break;
            }
            if(*check_pos[i]==BOX_PENDING)  break;
            else if(*check_pos[i]==BOX_DONE)
                *check_pos[i]=BOX_EMPTY;
        }
    }
    slab_free((void *)data1);
    pop_off();
}
