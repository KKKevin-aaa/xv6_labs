// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "colors.h"

void initlock(struct spinlock *lk, char *name) {
    lk->name = name;
    lk->locked = 0;
    lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void acquire(struct spinlock *lk) {
    push_off();  // disable interrupts to avoid deadlock.
    //potential deadlock without push_off, process A acquired this lock, and now 
    //an interrupt occurs and try to acquire this lock.Because process A is suppressed
    //by the interrupt and cannot release the lock,the ISR will spin indefinitely on the same
    //CPU, leading th a system deadlock.

    if (holding(lk)){
        print_held_locks();
        panic("acquire");
    }

    //NOTE: (New added)Panic on timeout
    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    int cnt=1, lock_updated=0;
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0){
        if(cnt<1000000000)  cnt++;
        else{
            cnt=0;      //Reduce the testing rounds
            struct cpu *cur_cpu=mycpu();
            if(lock_updated==0){
                mycpu()->wait_lock=lk;
                __sync_synchronize();
                __sync_lock_test_and_set(&cur_cpu->state, CPU_SPINNING);
                //Seperate intent and fact.(Payload and then statement)
                lock_updated=1;
            }
            //Cycle detection(e.g. AB-BA)starting with lk(try to hold)
            struct spinlock *test_lk=lk;
            while(1){
                struct cpu *snapshot=test_lk->cpu;  //Load to local
                if(snapshot==NULL || snapshot->state==CPU_RUNNING)  break;
                __sync_synchronize();       //Check and then read
                //Multi-Core concurrency + lock-free + Ordering matters(Explicit Memory barriers)
                struct spinlock *ss_lock=snapshot->wait_lock;
                if(ss_lock==NULL)   break;
                if(ss_lock->cpu==cur_cpu)   goto deadlock;
                test_lk=ss_lock;        //update
            }
            continue;   //Keep wating(no deadlock exist)
deadlock:
            pr_err("DEADLOCK DETECHED!LOCK name is %s held by cpu: %d\n",
                lk->name, (int)(lk->cpu-cpus));
            //At this point, interrupts are masked(disabled) on the cpu,
            //So we must trigger a non-maskable exception to bring the cpu to halt.
            asm volatile("ebreak");
        }
    }
    if(lock_updated==1){
        __sync_lock_test_and_set(&mycpu()->state, CPU_RUNNING);
        __sync_synchronize();
        mycpu()->wait_lock=NULL;    //Reverse the sequence used during CPU deactivation.
    }
    // Tell the C compiler and the processor to not move loads or stores past this point,
    // to ensure that the critical section's memory references happen strictly 
    // after the lock is acquired. On RISC-V, this emits a fence instruction.
    __sync_synchronize();   //Prohibit Out-of-order Execution.

    // Record info about lock acquisition for holding() and debugging.
    struct cpu *cur_cpu=mycpu();
    lk->cpu=cur_cpu;
    if(cur_cpu->noff < MAX_LOCK_DEPTH){
        cur_cpu->lock_lists[cur_cpu->noff-1].held_lock=lk;
        if(lk==NULL || (strncmp(lk->name, "pr", 2)==0 && strlen(lk->name)==2))   return;
        //Stop while acquiring print's lock(Non-reentrant function)
        uint64 cur_s0=r_fp(), ra_ptr=cur_s0-8;
        uint64 stack_end=PGROUNDUP(cur_s0), stack_start=stack_end-PGSIZE;
        if(safe_load_data(ra_ptr, &cur_cpu->lock_lists[cur_cpu->noff-1].ret_addr, 
            stack_start, stack_end)!=1){
            cur_cpu->lock_lists[cur_cpu->noff-1].ret_addr=0x1000000000;    //Invalid addr;
            pr_warn("Invalid address");
            return;
        }
        if(cur_cpu->lock_lists[cur_cpu->noff-1].ret_addr==0)
            panic("henish pheon.\n");
    }
}

// Release the lock.
void release(struct spinlock *lk) {
    if (!holding(lk)) panic("release");

    lk->cpu = 0;

    //update the held lock(considering some hand-over-hand)
    struct cpu *cur_cpu=mycpu();
    if(cur_cpu->noff==0)
        panic("Current no lock recorded at all(Dismatch).\n");
    int end_idx=MIN(cur_cpu->noff-1, MAX_LOCK_DEPTH-1);
    for(int found_idx=end_idx;found_idx>=0;found_idx--){
        if(cur_cpu->lock_lists[found_idx].held_lock==lk){
            for(int j=found_idx;j<end_idx;j++){
                cur_cpu->lock_lists[j]=cur_cpu->lock_lists[j+1];
            }
            memset((void *)&cur_cpu->lock_lists[end_idx], 0, sizeof(struct lock_debug_info));
        }
    }
    // Tell the C compiler and the CPU to not move loads or stores
    // past this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w zero, zero, (s1)
    __sync_lock_release(&lk->locked);

    pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int holding(struct spinlock *lk) {
    int r;
    r = (lk->locked && lk->cpu == mycpu());
    return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void push_off(void) {
    int old = intr_get();   //record current CPU's interrupt is on or off.

    // disable interrupts to prevent an involuntary context
    // switch while using mycpu().
    intr_off();

    if (mycpu()->noff == 0) mycpu()->intena = old;
    //turn off interrupts for the first time,record previsou statement.
    mycpu()->noff += 1;
}

void pop_off(void) {
    struct cpu *c = mycpu();
    if (intr_get()) panic("pop_off - interruptible");
    if (c->noff < 1) panic("pop_off");
    c->noff -= 1;
    if (c->noff == 0 && c->intena) intr_on();
}

void print_held_locks(void){
    struct cpu *cur_cpu=mycpu();
    printf("CPU %d current holds %d lock(s)\n", cpuid(), cur_cpu->noff);
    for(int i=0;i<cur_cpu->noff;i++){
        if(i>=MAX_LOCK_DEPTH){
            printf("Exceed the record lock depths.\n");
            return;
        }
        struct spinlock *lk=cur_cpu->lock_lists[i].held_lock;
        if(lk!=NULL){
            printf(" [%d]: \"%s\" (ptr 0x%llx)\n", i, lk->name, (uint64)lk);
            find_debug_info(cur_cpu->lock_lists[i].ret_addr, NULL, 0);
        }
        else{
            printf(" [%d]: ??? (unknown)\n", i);
            pr_info("NULL");
        }
    }
}