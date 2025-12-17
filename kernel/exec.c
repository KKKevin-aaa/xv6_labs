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

// the implementation of the exec() system call
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
    uint64 kexec_start_time = r_cycle();
    #endif
    #ifdef DEBUG_EXEC
    EXEC_TRACE("ENTRY pid=%d path='%s' argv=%p\n", p->pid, path, argv);
    #endif
    begin_op();
    if ((ip = namei(path)) == 0) {
        #ifdef DEBUG_EXEC
        EXEC_TRACE("FAIL namei not found path=%s\n", path);
        #endif
        end_op();
        return -1;
    }
    ilock(ip);
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
    if (elf.magic != ELF_MAGIC) goto bad;
    if ((pagetable = proc_pagetable(p)) == 0) goto bad;
    #ifdef DEBUG_EXEC
    EXEC_TRACE("new pagetable %p created for pid=%d\n", pagetable, p->pid);
    #endif
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
        if (ph.type != ELF_PROG_LOAD) continue;
        if (ph.memsz < ph.filesz) goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
        if (ph.vaddr % PGSIZE != 0) goto bad;
        uint64 sz1;
        if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0) goto bad;
        sz = sz1;
        if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
        #ifdef DEBUG_EXEC
        EXEC_TRACE("LOAD seg: va=%p memsz=0x%lx filesz=0x%lx\n", (void *)ph.vaddr, ph.memsz, ph.filesz);
        #endif
    }
    iunlockput(ip);
    end_op();
    ip = 0;
    p = myproc();
    uint64 oldsz = p->sz;
    sz = PGROUNDUP(sz);
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) == 0) goto bad;
    sz = sz1;
    uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
    sp = sz;
    stackbase = sp - USERSTACK * PGSIZE;
    #ifdef DEBUG_EXEC
    EXEC_TRACE("STACK setup: base=%p top=%p total_sz=0x%lx pages\n", (void *)stackbase, (void *)sp, sz/PGSIZE);
    #endif
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG) goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;
        if (sp < stackbase) goto bad;
        if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase) goto bad;
    if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;
    p->trapframe->a1 = sp;
    for (last = s = path; *s; s++)
        if (*s == '/') last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));
    oldpagetable = p->pagetable;
    p->pagetable = pagetable;
    p->sz = sz;
    p->trapframe->epc = elf.entry;
    p->trapframe->sp = sp;
    #ifdef DEBUG_EXEC
    EXEC_TRACE("COMMIT: switch pt, freeing old=%p oldsz=0x%lx\n", oldpagetable, oldsz/PGSIZE);
    #endif
    proc_freepagetable(oldpagetable, oldsz);
    #ifdef DEBUG_EXEC
    EXEC_TRACE("SUCCESS: pid=%d exec complete. entry=0x%lx\n", p->pid, p->trapframe->epc);
    #endif
    #ifdef EXEC_TEST_TIME
    uint64 kexec_end_time = r_cycle();
    if(kexec_end_time - kexec_start_time > 10000){
        printf("PERF: exec pid %d took 0x%lx cycles\n", p->pid, kexec_end_time - kexec_start_time);
    }
    #endif
    return argc;
bad:
    #ifdef DEBUG_EXEC
    EXEC_TRACE("FAIL exec path=%s pid=%d\n", path, p->pid);
    #endif
    if (pagetable) proc_freepagetable(pagetable, sz);
    if (ip) {
        iunlockput(ip);
        end_op();
    }
    //init the reserved area
    init_res_array(p->sz);
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
