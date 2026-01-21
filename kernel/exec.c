#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "mm.h"
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
    pagetable_t new_pagetable = 0, old_pagetable;
    res_block *old_rbarray=NULL;
    struct proc *p = myproc();
    //TIPS: Use Shadow MM(A Temporary memory descriptor)
    mm_struct_t *shadow_mm=mm_create();
    if(shadow_mm==NULL)     goto bad;
    memset(shadow_mm, 0, sizeof(mm_struct_t));
    //Thead-local object, Exempt from locking.
    initlock(&shadow_mm->mm_lock, p->mm->mm_lock.name);

    begin_op();
    if ((ip = namei(path)) == 0) {
        end_op();
        return -1;
    }
    ilock(ip);
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
    if (elf.magic != ELF_MAGIC) goto bad;
    if ((new_pagetable = proc_pagetable(p)) == 0) goto bad;

    vma_context_t *prev_vma=NULL;
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
        if (ph.type != ELF_PROG_LOAD) continue;
        if (ph.memsz < ph.filesz) goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
        if (ph.vaddr % PGSIZE != 0) goto bad;
        uint64 sz1;
        if ((sz1 = uvmalloc(new_pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0) goto bad;
        if (loadseg(new_pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0) goto bad;
        vm_area_struct_t *vma = alloc_vma_node();
        if (!vma) goto bad;

        vma->vm_start = ph.vaddr; 
        vma->vm_end = PGROUNDUP(ph.vaddr + ph.memsz); 
        
        vma->vm_filesz = ph.filesz;
        vma->vm_pgoff = ph.off; 
        vma->file_fd = -1; // 这里不是 mmap 打开的文件，没有 fd，设为 -1
        
        vma->vm_flags = gene_flags(ph.flags);
        vma->vm_page_prot = gene_page_prot(ph.flags);
        
        vma->vm_mm = shadow_mm;
        vma->vm_ops = NULL; // 这是一个匿名加载段（虽然来自文件，但不是 shared mmap）

        if(insert_vma_fast(shadow_mm, vma, &(vma_context_t){.prev=prev_vma, .next=NULL})==-1){
            remove_vma(shadow_mm, vma);
            goto bad;
        }
        prev_vma=vma;       //synchronize prev_vma

        if (vma->vm_flags & VM_EXEC) {
            if (shadow_mm->start_code == 0) shadow_mm->start_code = vma->vm_start;
            shadow_mm->end_code = vma->vm_end;
        } else if (vma->vm_flags & VM_WRITE) {
            if (shadow_mm->start_data == 0) shadow_mm->start_data = vma->vm_start;
            shadow_mm->end_data = vma->vm_end;
        }
        sz = sz1;
    }
    iunlockput(ip);
    end_op();
    ip = 0;
    p = myproc();
    uint64 oldsz = p->sz, sz1;
    sz = PGROUNDUP(sz);
    if ((sz1 = uvmalloc(new_pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) == 0) goto bad;
    sz = sz1;
    uvmclear(new_pagetable, sz - (USERSTACK + 1) * PGSIZE);
    sp = sz;
    stackbase = sp - USERSTACK * PGSIZE;

    // --- 创建 Stack VMA ---
    vm_area_struct_t *stack_vma = alloc_vma_node();
    if (!stack_vma) goto bad;
    
    stack_vma->vm_start = stackbase;
    stack_vma->vm_end = sz; // 栈顶（高地址）
    stack_vma->vm_flags = VM_READ | VM_WRITE; // 栈通常不可执行
    stack_vma->vm_page_prot = PTE_R | PTE_W | PTE_U;
    stack_vma->vm_mm = shadow_mm;
    stack_vma->vm_ops = NULL;
    stack_vma->file_fd = -1;
    
    if(insert_vma_fast(shadow_mm, stack_vma, &(vma_context_t){.prev=prev_vma, .next=NULL})==-1){
        remove_vma(shadow_mm, stack_vma);
        goto bad;
    }
    shadow_mm->stack_vma = stack_vma;
    shadow_mm->heap_vma = NULL; 

    //Pass arguments on the stack;no heap involvement.
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG) goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;
        if (sp < stackbase) goto bad;
        if (copyout(new_pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase) goto bad;
    if (copyout(new_pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;
    p->trapframe->a1 = sp;
    for (last = s = path; *s; s++)
        if (*s == '/') last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    //Ownership handover(atomic exchange, Nullifying the source)
    acquire(&p->uvm_lock);  //Top-level lock.
    old_pagetable = p->pagetable;
    old_rbarray = p->rb_array;  //store the outdate rb_array firstly

    mm_struct_t *old_mm=p->mm;
    if(old_mm!=NULL)    acquire(&old_mm->mm_lock);  //Lock-Ordering:take the high-level lock first.
    p->mm=shadow_mm;
    shadow_mm=NULL;

    p->pagetable = new_pagetable;
    p->sz = sz;
    p->trapframe->epc = elf.entry;
    p->trapframe->sp = sp;

    if(old_mm!=NULL)    release(&old_mm->mm_lock);
    release(&p->uvm_lock);   //Release locks as early as possible to enhance system concurrency


    proc_freepagetable(old_rbarray, old_pagetable, oldsz);
    //init the reserved area(after delete the previous resource)
    init_res_array(p->rb_array, p->sz);

    if(old_mm!=NULL){   //reclaim the unused resource 
        clear_mm_internal(old_mm);
        remove_mm(old_mm);
    }

    return argc;
bad:
    remove_mm(shadow_mm);
    if (new_pagetable) proc_freepagetable(old_rbarray, new_pagetable, sz);
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
