// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

// #define DEBUG_KALLOC

void freerange(void *pa_start, void *pa_end);

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

void kinit() {
    initlock(&kmem.lock, "kmem");
    freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
        kfree(p);
    }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

#ifndef LAB_SYSCALL
    memset(pa, 1, PGSIZE);
#endif

    r = (struct run *)pa;

    acquire(&kmem.lock);

#ifdef DEBUG_KALLOC
    struct proc *p = myproc();
    int pid = p ? p->pid : -1;
    char *name = p ? p->name : "kernel";
    printf("[KFREE] pa=%p by pid=%d(%s), old_head=%p\n", pa, pid, name, kmem.freelist);
#endif

    r->next = kmem.freelist;
    kmem.freelist = r;

#ifdef DEBUG_KALLOC
    printf("        new_head=%p, next=%p\n", kmem.freelist, r->next);
#endif

    release(&kmem.lock);
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r) {
#ifdef DEBUG_KALLOC
        struct proc *p = myproc();
        int pid = p ? p->pid : -1;
        char *name = p ? p->name : "kernel";
        printf("[KALLOC] pa=%p -> pid=%d(%s), old_head=%p, next=%p\n", 
               r, pid, name, kmem.freelist, r->next);
#endif
        kmem.freelist = r->next;
#ifdef DEBUG_KALLOC
        printf("         new_head=%p\n", kmem.freelist);
#endif
    }
    release(&kmem.lock);

#ifndef LAB_SYSCALL
    if (r)
        memset((char *)r, 5, PGSIZE);
#endif
    return (void *)r;
}
