// Sleeping locks
// NOTE: The internal spinlock protects the action of setting variables,
// while the sleeplock protects the entire duration of lock ownership.
//Duration this period, the lock may be migrated to others CPUs, with the struct proc *
//remaining the only invariant entity duration scheduling.
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void initsleeplock(struct sleeplock *lk, char *name) {
    initlock(&lk->lk, "sleep lock");
    lk->name = name;
    lk->locked = 0;
    lk->pid = 0;
}

//NOTE: During the early boot phase before the scheduler is initialized, the system
//operators in a strictly single-threaded environment.Since race conditions are impossible
//at this stage, we should exempt this section from standard locking protocols.

void acquiresleep(struct sleeplock *lk) {
    acquire(&lk->lk);
    int cur_pid=(myproc()==NULL)? 0 : myproc()->pid;
    while (lk->locked) {        //Unfair lock.
        //Each time the process wakes up, there is a possibility that the lock
        //has already seized by another process.
        if(lk->pid==cur_pid)
            panic("Reacquire sleeplock,");
        sleep(lk, &lk->lk);
    }
    lk->locked = 1;
    lk->pid = cur_pid;
    release(&lk->lk);
}

void releasesleep(struct sleeplock *lk) {
    acquire(&lk->lk);
    int cur_pid=(myproc()==NULL)? 0 : myproc()->pid;
    if(lk->pid!=cur_pid || lk->locked==0)
        panic("Illegal release.");
    lk->locked = 0;
    lk->pid = 0;
    //There is no secnario where another process is sleeping while waiting for
    //the lock I hold, therefore, no additional wake-up calls are required.
    if(cur_pid!=0)   wakeup(lk);
    release(&lk->lk);
}

int holdingsleep(struct sleeplock *lk) {
    int r;

    acquire(&lk->lk);
    int cur_pid=(myproc()==NULL)? 0 : myproc()->pid;
    r = lk->locked && (lk->pid == cur_pid);
    release(&lk->lk);
    return r;
}
