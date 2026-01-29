#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "file.h"
#include "proc.h"
#ifdef PGTBL_SOL
#include "riscv.h"
#endif
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "mm.h"
#include "vm.h"
#include "fcntl.h"
#include "colors.h"
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
    acquire(&cur_proc->uvm_lock);       //blocks operations like Page fault, exec, exit ...
    acquire(&cur_proc->mm->mm_lock);
    if(cur_proc->mm->heap_vma==NULL){
        PROC_TRACE("Current process lack heap_vma, unable to get necessary info.\n");
        goto release_and_ret;
    }
    //FIXME: // FIXME: 内存布局已改为 [Text->BSS->Stack->Heap]，需修正 exec 初始 SP 位置及 sbrk 起始基址(不再紧接BSS)。
    // 重点检查：uvmcopy 需兼容地址空间空洞(非连续)；栈底需加 Guard Page 防止溢出覆盖全局变量(BSS)。
    // 调试陷阱：以前栈溢出报 PageFault，现在可能静默修改数据，务必警惕！
    addr = cur_proc->mm->heap_vma->vm_end;
    if(n==0)    goto release_and_ret;   //No need to grow explicity
    uint64 limit=0;
    if(n>0){
        vm_area_struct_t *next_vma=cur_proc->mm->heap_vma->vm_next;
        if(next_vma==NULL)
            limit=UPPER_LIMIT;
        else
            limit=next_vma->vm_start;       //Mmap vma
        if(addr +n > limit){
            //Prevent excessive n from overwriting kernel memory
            pr_warn("No space to grow, remain space is %llx\n.\n", limit-addr);
            tmp_ret=-1;
            goto release_and_ret;
        }
    }
    else{   //The case where n==0 has been explicitly excluded.
        vm_area_struct_t *prev_vma=cur_proc->mm->heap_vma->vm_prev;
        if(prev_vma==NULL)
            limit=cur_proc->mm->heap_vma->vm_start;     //Only one heap_vma???
        else    limit=prev_vma->vm_end;
        if(addr + n < limit){
            pr_warn("Reached the prev_vma boundary, can't shrink to that position.");
            tmp_ret=-1;
            goto release_and_ret;
        }
    }
    //Pass defined position verfication.
    if (t == SBRK_EAGER) {
        if (growproc(n) < 0) {      //Update heap_vma boundary in growproc already
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
    release(&cur_proc->uvm_lock);
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

//POSIX: void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
//And return (void *)-1 while failed, return mampped area's start address.
uint64 sys_mmap(void ){
    struct proc *p=myproc();
    uint64 sugg_addr;   //Suggestied address
    uint64 length, offset;
    int prot, flags, fd;
    argaddr(0, &sugg_addr);
    argaddr(1, &length);
    argint(2, &prot);
    argint(3, &flags);
    argint(4, &fd);
    argaddr(5, &offset);
    //Implement front-end parameter validation to fast-fail and return immediately.
    if(flags & MAP_FIXED){
        if(sugg_addr==NULL || ((uint64)sugg_addr & (PGSIZE-1)) || 
            ((uint64)sugg_addr +length >= UPPER_LIMIT)){
            pr_warn("Invalid suggest_address while flags seted as MAP_FIXED.");
            return -1;
        }
    }
    if(length==0 || (offset & (PGSIZE-1))){
        pr_warn("invalid length or offset, zero or unaligned.");
        return -1;      //EINVAL
    }
    if(!((flags & MAP_SHARED) ^ (flags & MAP_PRIVATE))){
        pr_warn("invalid flags, must select between MAP_SHARED and MAP_PRIVATE.\n");
        return -1;
    }
    if(!(flags & MAP_ANONYMOUS)){
        struct file *f=p->ofile[fd];
        if(f==NULL)     return -1;  //EBADF
        if(f->type!=FD_INODE)   return -1;      //EACCES OR ENODEV
        if(f->readable==0)  return -1;      //EACCES (any operation should readable)
        if((flags & MAP_SHARED) && (prot & PROT_WRITE) && f->writable==0)
            return -1;      //EACCES
    }
    acquire(&p->mm->mm_lock);
    uint64 ret=(uint64)do_mmap(&(mmap_context_t){
        .pagetable=p->pagetable,
        .rb_array=p->rb_array,
        .mm=p->mm, 
        .sugg_addr=sugg_addr, 
        .length=length, 
        .prot=prot, 
        .flags=flags, 
        .f=(flags & MAP_ANONYMOUS)?NULL:p->ofile[fd], 
        .offset=offset});
    release(&p->mm->mm_lock);
    return ret;
}