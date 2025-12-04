#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// Fetch the uint64 at addr from the current process.
int fetchaddr(uint64 addr, uint64 *ip) {
    struct proc *p = myproc();
    if (addr >= p->sz || addr + sizeof(uint64) > p->sz)  // both tests needed, in case of overflow
        return -1;
    if (copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0) return -1;
    return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int fetchstr(uint64 addr, char *buf, int max) {
    struct proc *p = myproc();
    if (copyinstr(p->pagetable, buf, addr, max) < 0) return -1;
    return strlen(buf);
}

static uint64 argraw(int n) {
    struct proc *p = myproc();
    switch (n) {
        case 0:
            return p->trapframe->a0;
        case 1:
            return p->trapframe->a1;
        case 2:
            return p->trapframe->a2;
        case 3:
            return p->trapframe->a3;
        case 4:
            return p->trapframe->a4;
        case 5:
            return p->trapframe->a5;
    }
    panic("argraw");
    return -1;
}

// Fetch the nth 32-bit system call argument.
void argint(int n, int *ip) { *ip = argraw(n); }

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
void argaddr(int n, uint64 *ip) { *ip = argraw(n); }

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int argstr(int n, char *buf, int max) {
    uint64 addr;
    argaddr(n, &addr);
    return fetchstr(addr, buf, max);
}

int path_match(char *input_path, char *allow_path_str){
    struct proc *p=myproc();
    //KMP algorithm
    char *ref_cursor=p->allow_path_str, *input_curosr=input_path;
    while(*ref_cursor!='\0'){
        while(*input_curosr!='\0' && *ref_cursor!='\0' && *ref_cursor!='\n'
                && *ref_cursor==*input_curosr){
            input_curosr++;
            ref_cursor++;
        }
        //Determine the cuase of loop exit
        if(*input_curosr=='\0' && (*ref_cursor=='\n' || *ref_cursor=='\0'))
            return 1;   //Successfully matched(all got the end!)
        else if(*ref_cursor=='\0')
            return 0;
        else{
            input_curosr=input_path;    //Restart matching from the beginning
            while(*ref_cursor!='\0' && *ref_cursor!='\n')
                ref_cursor++;
            if(*ref_cursor=='\0')   //arrive the end
                return 0;
            ref_cursor++;
        }
    }
    return 0;
}

// Prototypes for the functions that handle system calls.
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_pause(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_ioctl(void);
extern uint64 sys_interpose(void);

#ifdef LAB_NET
extern uint64 sys_bind(void);
extern uint64 sys_unbind(void);
extern uint64 sys_send(void);
extern uint64 sys_recv(void);
#endif
#ifdef LAB_PGTBL
extern uint64 sys_pgpte(void);
extern uint64 sys_kpgtbl(void);
#endif

// An array mapping syscall numbers from syscall.h
// to the function that handles the system call.
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_pause]   sys_pause,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_ioctl] sys_ioctl, 
[SYS_interpose] sys_interpose,
#ifdef LAB_NET
[SYS_bind] sys_bind,
[SYS_unbind] sys_unbind,
[SYS_send] sys_send,
[SYS_recv] sys_recv,
#endif
#ifdef LAB_PGTBL
[SYS_pgpte] sys_pgpte,
[SYS_kpgtbl] sys_kpgtbl,
#endif
};

void
syscall(void)
{
  int num;
  struct proc *p = myproc();


    num = p->trapframe->a7;
    //   num=*(int *)0;
    if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
        // Use num to lookup the system call function for num, call it,
        // and store its return value in p->trapframe->a0
        if ((p->syscall_mask >> num & 0x1) == 1) {
            if(num==SYS_open || num==SYS_exec){
                uint64 input_path_addr;
                char input_path[MAXPATH];
                argaddr(0, &input_path_addr);
                if(fetchstr(input_path_addr, input_path, MAXPATH)<0){
                    // printf("Error path:\n");
                    p->trapframe->a0=-1;
                    return;
                }
                if(path_match(input_path, p->allow_path_str)==1){
                    // printf("path matched!Now open or exec is allowed!\n");
                    p->trapframe->a0 = syscalls[num]();
                }
                else{
                    printf("pid: %d process(%s),open or exec is restricted!\n", p->pid, p->name);
                    printf("Meanwhile, the input path is not within the allowed special paths\n");
                    p->trapframe->a0=-1;
                }
            }
            else{
                printf("pid: %d process(%s), syscall(%d) is restricted!\n", p->pid, p->name, num);
                p->trapframe->a0 = -1;
            }
        } else {
            p->trapframe->a0 = syscalls[num]();
        }
    } else {
        printf("%d %s: unknown sys call %d\n", p->pid, p->name, num);
        p->trapframe->a0 = -1;
    }
}
