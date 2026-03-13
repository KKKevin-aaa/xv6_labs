#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "rbtree.h"
#include "kvm.h"
#include "slab.h"
#include "fcntl.h"
#include "mm.h"
#include "colors.h"
#include "utils.h"

// #define EXEC_TEST_TIME
static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags) {
    int perm = 0;
    if (flags & 0x1) perm = PTE_X;
    if (flags & 0x2) perm |= PTE_W;
    return perm;
}
int elf_flags2vm_flags(int elf_flags){
    unsigned long vm_flags = 0;

    if (elf_flags & 0x4)
        vm_flags |= VM_READ;
    if (elf_flags & 0x2)
        vm_flags |= VM_WRITE;
    if (elf_flags & 0x1)
        vm_flags |= VM_EXEC;
    // 此外，VMA 还会根据文件属性自动加上其他标志
    vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC; // 允许未来通过 mprotect 修改
    return vm_flags;
}

// the implementation of the exec() system call
// Build the mm_struct according the ELF memory layout
// also serve as the basis for the fork. 
int kexec(char *path, char **argv) {
    char *s, *last;
    int i, off;
    uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
    struct elfhdr elf;
    struct inode *ip;
    struct proghdr ph;
    struct file *src_file=NULL;
    pagetable_t new_pagetable = 0;
    struct proc *p = myproc();
    //TIPS: Use Shadow MM(A Temporary memory descriptor)
    mm_struct_t *shadow_mm=mm_create();
    if(shadow_mm==NULL){
        pr_warn("create shadow_mm failed.\n");
        return -1;
    }
    initsleeplock(&shadow_mm->mm_lock, p->mm->mm_lock.name);
    begin_op();
    if ((ip = namei(path)) == 0){
        end_op();
        return -1;
    }
    ilock(ip);
    src_file=filealloc();
    if(src_file==NULL)      goto bad;
    src_file->type=FD_INODE;   //Selective Field Initialization, preserving metadata.
    src_file->ip=ip;            //Move, instead of copy. No need to idup.
    src_file->off=0;
    src_file->readable=1;
    src_file->writable=0;

    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) goto bad;
    if (elf.magic != ELF_MAGIC) goto bad;
    if ((new_pagetable = proc_pagetable(p)) == 0) goto bad;
    //Establish some mapping assoicated with kernel directly(remove PTE_U, and keep the same for all processes.)
    shadow_mm->pagetable=new_pagetable;
    vm_area_struct_t *prev_vma=NULL;
    //Loader will scan all program headers, select PT_LOAD segment.
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) goto bad;
        if (ph.type != ELF_PROG_LOAD) continue;
        if (ph.memsz < ph.filesz) goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr) goto bad;
        if (ph.vaddr % PGSIZE != 0) goto bad;
        uint64 sz1;

        vm_area_struct_t *vma = alloc_vma_node();
        if (!vma) goto bad;
        vma->vm_start = PGROUNDDOWN(ph.vaddr); 
        vma->vm_end = PGROUNDUP(ph.vaddr + ph.memsz); 
        
        vma->vm_filesz = ph.filesz; 
        vma->vm_pgoff = ph.off;     //file-backed VMA.
        vma->vm_file = filedup(src_file);         //Increase file's ref_count.
        
        vma->vm_flags = elf_flags2vm_flags(ph.flags);   //VM_PRIVATE by defualt.
        vma->vm_page_prot = flags2page_prot(vma->vm_flags) | PTE_U; 
        //In exec,file content already loaded.So page_prot must be writable.
        //(bypass shared, kernel limit defined in flags2_page_prot).
        
        vma->vm_mm = shadow_mm;
        vma->vm_ops = NULL; // 这是一个匿名加载段（虽然来自文件，但不是 shared mmap）

        if((sz1= vmalloc(&(struct alloc_context){
            .pagetable=new_pagetable,
            .seg_start=vma->vm_start,
            .seg_end=vma->vm_end,
            .pt_lock=NULL,
            .xperm=vma->vm_page_prot,
            .rblocks=NULL
        })) ==0 ){
            vma_put(vma);
            goto bad;
        }
        if (loadseg(new_pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0){
            vma_put(vma);
            buddy_dealloc_backend(&(struct alloc_context){
                .pagetable=new_pagetable,
                .pt_lock=NULL,
                .rblocks=NULL,
                .seg_start=vma->vm_end,
                .seg_end=vma->vm_start
            });
            goto bad;
        }

        pr_info("from elf,loading vma->vm_start=0x%llx, vma->vm_end=0x%llx and pg.filesz is 0x%llx",
            vma->vm_start, vma->vm_end, ph.filesz);
        acquiresleep(&shadow_mm->mm_lock);
        //Thchnically, a lock is not required here.but one in included because the internal
        //insertion logic performs a lock-validation check.
        if(insert_vma_fast(shadow_mm, vma, &(vma_context_t){.prev=prev_vma, .next=NULL})==-1){
            vma_put(vma);
            buddy_dealloc_backend(&(struct alloc_context){
                .pagetable=new_pagetable,
                .pt_lock=NULL,
                .rblocks=NULL,
                .seg_start=vma->vm_end,
                .seg_end=vma->vm_start,
            });
            goto bad;
        }
        releasesleep(&shadow_mm->mm_lock);
        //Thread-private, never contention.(acquire after receive signal from disk)
        prev_vma=vma;       //synchronize prev_vma

        if (vma->vm_flags & VM_EXEC) {
            if (shadow_mm->start_code == 0) shadow_mm->start_code = vma->vm_start;
            shadow_mm->end_code = vma->vm_end;
        } else if (vma->vm_flags & VM_WRITE) {
            if (shadow_mm->start_data == 0) shadow_mm->start_data = vma->vm_start;
            shadow_mm->end_data = vma->vm_end;
        }
        sz = MAX(sz, sz1);
    }   //sz is the highest address among all loadable segments.
    iunlock(ip);    //Ownership has been transferred.Release the lock but don't drop ref.
    end_op();
    ip = NULL;
    fileclose(src_file);
    src_file=NULL;
    acquiresleep(&shadow_mm->mm_lock);       //Acquire after finishing disk loading
    sz = PGROUNDUP(sz);

    //Do some preparation work.
    stackbase=sz+PGSIZE;    //Top-guard + bottom-guard(stack only one-page.)
    if(vmalloc(&(struct alloc_context){
        .pagetable=new_pagetable,
        .seg_start=stackbase-PGSIZE,
        .seg_end=stackbase+2*PGSIZE,
        .xperm=PTE_W
    }) ==0 )    goto bad;
    pr_info("Now alloc and mappage a new stack area in 0x%llx", (uint64)new_pagetable);
    uvmclear(new_pagetable, stackbase-PGSIZE);
    uvmclear(new_pagetable, stackbase+PGSIZE);
    // --- 创建 Stack VMA ---
    vm_area_struct_t *stack_vma = alloc_vma_node();
    if (!stack_vma){
        buddy_dealloc_backend(&(struct alloc_context){
            .pagetable=new_pagetable,
            .pt_lock=NULL,
            .rblocks=NULL,
            .seg_start=stackbase+2*PGSIZE,
            .seg_end=stackbase-PGSIZE
        });
        goto bad;
    }
    stack_vma->vm_start = stackbase;     //Skip the guard pages.
    //This represents the static ownership of the memory region,
    // which remains constant regradless of stack utilization.
    stack_vma->vm_end = stackbase+PGSIZE; 
    stack_vma->vm_flags = VM_READ | VM_WRITE;   //non-shared(private) + writable,
    // trigger the COW handling mechanism, when meeting page fualt.
    stack_vma->vm_page_prot = PTE_R | PTE_W | PTE_U;
    stack_vma->vm_mm = shadow_mm;
    stack_vma->vm_ops = NULL;
    stack_vma->vm_file = NULL;
    if(insert_vma_fast(shadow_mm, stack_vma, &(vma_context_t){.prev=prev_vma, .next=NULL})==-1){
        vma_put(stack_vma);
        buddy_dealloc_backend(&(struct alloc_context){
            .pagetable=shadow_mm->pagetable,
            .pt_lock=NULL,
            .seg_start=stackbase+2*PGSIZE,
            .seg_end=stackbase-PGSIZE,
            .rblocks=NULL
        });
        goto bad;
    }
    shadow_mm->stack_vma = stack_vma;
    prev_vma=stack_vma;
    pr_info("stack_vma->vm_start=0x%llx, stack_vma->vm_end=0x%llx", stack_vma->vm_start, stack_vma->vm_end);

    //Create heap VMA before stack VMA
    vm_area_struct_t *heap_vma = alloc_vma_node();
    if (!heap_vma) goto bad;
    heap_vma->vm_start = stackbase+2*PGSIZE;        //New program break
    heap_vma->vm_end = stackbase+2*PGSIZE; 
    heap_vma->vm_flags = VM_READ | VM_WRITE;
    heap_vma->vm_page_prot = PTE_R | PTE_W | PTE_U;
    heap_vma->vm_mm = shadow_mm;
    heap_vma->vm_ops = NULL;
    heap_vma->vm_file = NULL;     //Anonymous VMA
    if(insert_vma_fast(shadow_mm, heap_vma, &(vma_context_t){.prev=prev_vma, .next=NULL})==-1){
        vma_put(heap_vma);
        //No allocated physical resource, empty heap
        goto bad;
    }
    shadow_mm->heap_vma = heap_vma;
    pr_info("heap_vma->vm_start=0x%llx, heap_vma->vm_end=0x%llx", heap_vma->vm_start, heap_vma->vm_end);


    sp=stackbase+PGSIZE;
    //Pass arguments on the stack;no heap involvement.
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG) goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;
        if (sp < shadow_mm->stack_vma->vm_start) goto bad;   //Stack Underflow.
        if (copyout(new_pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < shadow_mm->stack_vma->vm_start) goto bad;
    if (copyout(new_pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0) goto bad;
    p->trapframe->a1 = sp;
    for (last = s = path; *s; s++)
        if (*s == '/') last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    acquire(&p->uvm_lock);  //Top-level lock.
    mm_struct_t *old_mm=p->mm;
    p->mm=shadow_mm;
    p->pagetable = new_pagetable;
    release(&p->uvm_lock);

    new_pagetable=0;        //Uint64*(Trivial Type)Invalid this pointer directly.
    p->trapframe->epc = elf.entry;
    p->trapframe->sp = sp;

    releasesleep(&shadow_mm->mm_lock);
    shadow_mm=NULL;     //Invalid this pointer copy.

    //Essentially a replacement operatiorn,old pagetable and mm_struct must be cleaned up.
    if(old_mm!=NULL)    mm_put(old_mm);//reclaim the unused resource 
    //init the reserved area(after delete the previous resource)
    init_res_array(p->rb_array, p->mm->heap_vma->vm_start);

    return argc;
bad:
    if(holdingsleep(&shadow_mm->mm_lock))    releasesleep(&shadow_mm->mm_lock);
    mm_put(shadow_mm);
    //No process can get this temporary unpublished pagetable.
    if(src_file!=NULL){
        if (ip!=NULL)   iunlock(ip);
        //Ownership have being handed over to the src_file.(No put.)
        fileclose(src_file);
    }
    else{
        if(ip!=NULL)    iunlockput(ip);
    }
    end_op();
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
