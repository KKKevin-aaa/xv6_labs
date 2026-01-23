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

    if (holding(lk)) panic("acquire");

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
                //Seperate intent and fact.
                lock_updated=1;
            }
            //Cycle detection(e.g. AB-BA)starting with lk(try to hold)
            struct spinlock *test_lk=lk;
            while(1){
                struct cpu *snapshot=test_lk->cpu;  //Load to local
                if(snapshot==NULL || snapshot->state==CPU_RUNNING)  break;      //Maybe not cycle or some CPU's release lock
                __sync_synchronize();
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
            //So we must trigger a non-maskable exception to bring the cpu to a halt.
            asm volatile("ebreak");
        }
    }
    if(lock_updated==1){
        __sync_lock_test_and_set(&mycpu()->state, CPU_RUNNING);
        __sync_synchronize();
        mycpu()->wait_lock=NULL;    //Reverse the sequence used during CPU deactivation.
    }
    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();   //Prohibit Out-of-order Execution.

    // Record info about lock acquisition for holding() and debugging.
    struct cpu *cur_cpu=mycpu();
    lk->cpu=cur_cpu;
    if(cur_cpu->noff < MAX_LOCK_DEPTH){
        cur_cpu->held_lock[cur_cpu->noff-1]=lk;
        uint64 cur_s0=r_fp(), ret_addr=0;

        cur_cpu->held_lock_info[cur_cpu->noff-1]= FIXME: 
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
        if(cur_cpu->held_lock[found_idx]==lk){
            //Move last element to held_lock[found_idx]
            cur_cpu->held_lock[found_idx]=cur_cpu->held_lock[end_idx];
            cur_cpu->held_lock[end_idx]=NULL;
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
        struct spinlock *lk=cur_cpu->held_lock[i];
        if(lk!=NULL)
            printf(" [%d]: \"%s\" (ptr 0x%llx)\n", i, lk->name, (uint64)lk);
        else
            printf(" [%d]: ??? (unknown)\n", i);
    }
}