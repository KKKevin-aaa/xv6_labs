#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#ifdef PGTBL_SOL
#include "riscv.h"
#endif
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "mm.h"
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

//Different from vmfault(Explicitly invoked by the user.)
uint64 sys_sbrk(void) {
    uint64 addr;
    int t;
    int n;
    uint64 tmp_ret=0;
    struct proc *cur_proc=myproc();
    argint(0, &n);
    argint(1, &t);
    if(cur_proc->mm==NULL)
        panic("Fatal Error: sbrk invoked on a process lacking an mm_struct.\n");
    acquire(&cur_proc->mm->mm_lock);        ????FIXME: lock!!!
    if(cur_proc->mm->heap_vma==NULL){
        printf("Current process lack heap_vma, unable to get necessary info.\n");
        goto release_and_ret;
    }

    addr = cur_proc->mm->heap_vma->vm_end;
    vm_area_struct_t *next_vma=cur_proc->mm->heap_vma->vm_next;
    uint64 limit=0;
    if(next_vma==NULL){
        printf("[sbrk]Warning:current memlayout lack of stack.\n");
        limit=USERSTACK_END;
    }
    else if(next_vma==cur_proc->mm->stack_vma)
        limit=cur_proc->mm->stack_vma->vm_start-PGSIZE;
    else
        limit=next_vma->vm_start;
    if(addr +n > limit){      //mmap vma
        printf("[sbrk]No space to grow, remain space is %llx\n.\n", limit-addr);
        tmp_ret=-1;
        goto release_and_ret;
    }
    //Prevent excessive n from overwriting kernel memory
    if (t == SBRK_EAGER) {
        if (growproc(n) < 0) {      //Already update heap_vma boundary in growproc.
            tmp_ret=-1;
            goto release_and_ret;
        }
    } 
    else {
        // Lazily allocate memory for this process: increase its memory
        // size but don't allocate memory. If the processes uses the
        // memory, vmfault() will allocate it.
        if(n<0){
        #ifdef RESERVE
            uint64 start_va=cur_proc->mm->heap_vma->vm_end;
            uint64 end_va=start_va+n;
            free_res_memory(cur_proc->rb_array, cur_proc->pagetable, 
                            cur_proc->rb_array[0].va, start_va, end_va);
            cur_proc->mm->heap_vma->vm_end+=n;
        #else
            if(growproc(n)<0){
                tmp_ret=-1;
                goto release_and_ret;
            }  //non-reserve
        #endif
        }
        else    cur_proc->mm->heap_vma->vm_end += n; //In lazy grow
    }
    tmp_ret=addr;
release_and_ret:
    release(&cur_proc->mm->mm_lock);
    return tmp_ret;
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
    backtrace();
    release(&tickslock);
    return 0;
}

// #ifdef LAB_PGTBL
int sys_pgpte(void) {
    uint64 va;
    struct proc *p;

    p = myproc();
    argaddr(0, &va);
    pte_t *pte = pgpte(p->pagetable, va);
    if (pte != 0) {
        return (uint64)*pte;
    }
    return 0;
}
// #endif

// #ifdef LAB_PGTBL
int sys_kpgtbl(void) {
    struct proc *p;

    p = myproc();
    acquire(&p->uvm_lock);
    vmprint(p->pagetable);
    release(&p->uvm_lock);
    return 0;
}
// #endif

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

uint64 sys_interpose(void) {
    struct proc *p = myproc();
    int syscall_mask_input;
    uint64 allowed_path_addr;
    argint(0, &syscall_mask_input);
    argaddr(1, &allowed_path_addr);
    unsigned int mask = (unsigned int)syscall_mask_input;
    if (mask == 0 || (mask & (mask - 1)) != 0) return -1;
    int syscall_id = 0;
    unsigned int mask_copy = mask;
    while ((mask_copy & 0x1) == 0) {
        syscall_id++;
        mask_copy >>= 1;
    }
    if ((p->syscall_mask >> syscall_id & 0x1) == 1) return -1;
    char tmp_path[MAXPATH];
    if (fetchstr(allowed_path_addr, tmp_path, MAXPATH) < 0) return -1;
    if (strlen(tmp_path) != 1 || strncmp(tmp_path, "-", 1) != 0) {
        int prev_len = strlen(p->allow_path_str);  // pass All the tests
        int tmp_len = strlen(tmp_path);
        if (tmp_len + prev_len + 2 >= MAXPATH) return -1;
        strncpy(p->allow_path_str + prev_len, tmp_path, tmp_len);
        // rearrange the end character
        int cur_len = strlen(p->allow_path_str);
        p->allow_path_str[cur_len] = '\n';
        p->allow_path_str[cur_len + 1] = '\0';
    }
    p->syscall_mask |= mask;  // make sure don't affect other restricted syscalls
    return 0;
}
