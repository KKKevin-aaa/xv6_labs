#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
// #define DEBUG_EXEC
#ifdef DEBUG_EXEC
#define EXEC_TRACE(fmt, ...) \
    do { \
        printf("[EXEC:%s] " fmt, __func__, ##__VA_ARGS__); \
    } while (0)
#else
#define EXEC_TRACE(fmt, ...) \
    do { \
    } while (0)
#endif
// #define EXEC_TEST_TIME
static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags) {
    int perm = 0;
    if (flags & 0x1) perm = PTE_X;
    if (flags & 0x2) perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int kexec(char *path, char **argv) {
    char *s, *last;
    int i, off;
    uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    pagetable_t pagetable = 0, oldpagetable;
    struct proc *p = myproc();
    #ifdef EXEC_TEST_TIME
        uint64 kexec_start_time=r_cycle();
    #endif
    EXEC_TRACE("kexec start pid=%d path=%s argv=%p\n", p->pid, path, argv);
    begin_op();

    // Open the executable file.
    if ((ip = namei(path)) == 0) {
    EXEC_TRACE("namei failed for %s\n", path);
        end_op();
        return -1;
    }
    ilock(ip);

    // Read the ELF header.
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;

    // Is this really an ELF file?
    if (elf.magic != ELF_MAGIC) goto bad;

    if ((pagetable = proc_pagetable(p)) == 0) goto bad;
    EXEC_TRACE("new pagetable %p created for pid=%d\n", pagetable, p->pid);

    // Load program into memory.
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
        if (ph.type != ELF_PROG_LOAD) continue;
        if (ph.memsz < ph.filesz) goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
        if (ph.vaddr % PGSIZE != 0) goto bad;
        uint64 sz1;
        if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
            goto bad;
        sz = sz1;
        EXEC_TRACE("mapped segment va=%p memsz=0x%lx filesz=0x%lx\n", (void *)ph.vaddr, ph.memsz, ph.filesz);
        if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
    }
    iunlockput(ip);
    end_op();
    ip = 0;

    p = myproc();
    uint64 oldsz = p->sz;

    // Allocate some pages at the next page boundary.
    // Make the first inaccessible as a stack guard.
    // Use the rest as the user stack.
    sz = PGROUNDUP(sz);
        EXEC_TRACE("Process %s(pid=%d): OLD sz=0x%lx pages, NEW text/data sz=0x%lx pages\n", 
            p->name, p->pid, oldsz/PGSIZE, sz/PGSIZE);
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) == 0) goto bad;
    sz = sz1;
    EXEC_TRACE("After adding stack: NEW total sz=0x%lx pages\n", sz/PGSIZE);
    uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
    sp = sz;
    stackbase = sp - USERSTACK * PGSIZE;    //the end of user_stack,should not arrived!
    EXEC_TRACE("stack window base=%p top=%p\n", (void *)stackbase, (void *)sp);

    // Copy argument strings into new stack, remember their
    // addresses in ustack[].
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG) goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;  // riscv sp must be 16-byte aligned
        if (sp < stackbase) goto bad;
        if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;

    // push a copy of ustack[], the array of argv[] pointers.
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase) goto bad;
    if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;

    // a0 and a1 contain arguments to user main(argc, argv)
    // argc is returned via the system call return
    // value, which goes in a0.
    p->trapframe->a1 = sp;

    // Save program name for debugging.
    for (last = s = path; *s; s++)
        if (*s == '/') last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    // Commit to the user image.
    oldpagetable = p->pagetable;
    p->pagetable = pagetable;
    p->sz = sz;
    p->trapframe->epc = elf.entry;  // initial program counter = main
    p->trapframe->sp = sp;          // initial stack pointer
        EXEC_TRACE("Now freeing OLD pagetable=%p with oldsz=0x%lx pages\n", 
            oldpagetable, oldsz/PGSIZE);
    proc_freepagetable(oldpagetable, oldsz);
        EXEC_TRACE("Process %s(pid=%d) exec complete. Final sz=0x%lx pages\n", 
            p->name, p->pid, sz/PGSIZE);
    #ifdef EXEC_TEST_TIME
        uint64 kexec_end_time=r_cycle();
        if(kexec_end_time-kexec_start_time>10000){
            printf("PERF: exec pid %d took 0x%lx cycles\n", p->pid, kexec_end_time-kexec_start_time);
        }
    #endif
    return argc;  // this ends up in a0, the first argument to main(argc, argv)

bad:
    EXEC_TRACE("exec failed for path=%s pid=%d\n", path, p->pid);
    if (pagetable) proc_freepagetable(pagetable, sz);
    if (ip) {
        iunlockput(ip);
        end_op();
    }
    return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz) {
    uint i, n;
    uint64 pa;

    for (i = 0; i < sz; i += PGSIZE) {
        pa = walkaddr(pagetable, va + i);
        if (pa == 0) panic("loadseg: address should exist");
        if (sz - i < PGSIZE) n = sz - i;
        else
            n = PGSIZE;
        if (readi(ip, 0, (uint64)pa, offset + i, n) != n) return -1;
    }

    return 0;
}
