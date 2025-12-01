// #include "defs.h"
// #include "memlayout.h"
// #include "param.h"
// #include "proc.h"
// #include "riscv.h"
// #include "spinlock.h"
// #include "types.h"
// #include "vm.h"
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64 sys_exit(void) {
    int n;
    argint(0, &n);
    kexit(n);
    return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return kfork(); }

uint64 sys_wait(void) {
    uint64 p;
    argaddr(0, &p);
    return kwait(p);
}

uint64 sys_sbrk(void) {
    uint64 addr;
    int t;
    int n;

    argint(0, &n);
    argint(1, &t);
    addr = myproc()->sz;

    if (t == SBRK_EAGER || n < 0) {
        if (growproc(n) < 0) {
            return -1;
        }
    } else {
        // Lazily allocate memory for this process: increase its memory
        // size but don't allocate memory. If the processes uses the
        // memory, vmfault() will allocate it.
        if (addr + n < addr) return -1;
        myproc()->sz += n;
    }
    return addr;
}

uint64 sys_pause(void) {
    int n;
    uint ticks0;

    argint(0, &n);  // Argument Retrieval: Get the sleep duration 'n' form the user stack.
    if (n < 0) n = 0;
    acquire(&tickslock);  // Acquire Lock :Protect the global 'ticks' variables
    ticks0 = ticks;
    while (ticks - ticks0 < n) {
        if (killed(myproc())) {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64 sys_kill(void) {
    int pid;

    argint(0, &pid);
    return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
    uint xticks;

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
    return xticks;
}

uint64 sys_interpose(void){
    struct proc *p=myproc();
    int syscall_mask_input;
    uint64 allowed_path_addr;
    argint(0, &syscall_mask_input);
    argaddr(1, &allowed_path_addr);
    unsigned int mask=(unsigned int)syscall_mask_input;
    if(mask==0 || (mask & (mask -1))!=0) 
        return -1;

    int syscall_id=0;
    unsigned int mask_copy=mask;
    while((mask_copy & 0x1) ==0){
        syscall_id++;
        mask_copy >>=1 ;
    }
    if((p->syscall_mask >> syscall_id & 0x1) == 1)  
        return -1;
    int prev_len=strlen(p->allow_path_str); //pass All the tests
    if(MAXPATH-2-prev_len <0)   return -1;
    if(fetchstr(allowed_path_addr, p->allow_path_str+prev_len, MAXPATH-2-prev_len)<0){
        //Append directly to the tail of it, and truncate it back if fails
        p->allow_path_str[prev_len]='\0';
        return -1;
    }
    //rearrange the end character 
    int cur_len=strlen(p->allow_path_str);
    p->allow_path_str[cur_len]='\n';
    p->allow_path_str[cur_len+1]='\0';
    p->syscall_mask |= mask;    //make sure don't affect other restricted syscalls
    return 0;
}
