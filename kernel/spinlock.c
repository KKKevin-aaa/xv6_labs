// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

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

    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0);

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
            //move forward element if necessary
            for(int j=found_idx;j<end_idx;j++){
                cur_cpu->held_lock[j]=cur_cpu->held_lock[j+1];
            }
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
    // implies that an assignment might be implemented with
    // multiple store instructions.
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