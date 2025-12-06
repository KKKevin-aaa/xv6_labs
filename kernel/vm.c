#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;
void *usyscall_pa=NULL;
extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t kvmmake(void) {
    pagetable_t kpgtbl;

    kpgtbl = (pagetable_t)kalloc();
    memset(kpgtbl, 0, PGSIZE);

    usyscall_pa=kalloc();
    memset(usyscall_pa, 0, PGSIZE);
    //Allocated once during system initialization, not per-process;
    //thus, it is immune to memory leak.

    // uart registers
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

    // PLIC
    kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // map kernel data and the physical RAM we'll make use of.
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // allocate and map a kernel stack for each process.
    proc_mapstacks(kpgtbl);

    return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void) { kernel_pagetable = kvmmake(); }

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart() {
    // wait for any previous writes to the page table memory to finish.
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));

    // flush stale entries from the TLB.
    sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
// To support superpage, we introduce new paras: target_level
// and stop while level equals to target_level(0 for 4kB, 1 for 2MB, 2 for 1GB)
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc, int target_level) {
    if (va >= MAXVA) panic("walk");
    for (int level = 2; level > target_level; level--) {
        pte_t *pte = &pagetable[PX(level, va)]; //math
        if (*pte & PTE_V) { //Locate the next level page table using pte(create and init if necessary)
            pagetable = (pagetable_t)PTE2PA(*pte);
        #ifdef LAB_PGTBL    //Superpage support
            if (PTE_LEAF(*pte))
                return pte;
        #endif
        } else {
            if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
            memset(pagetable, 0, PGSIZE);
            *pte = PA2PTE(pagetable) | PTE_V;   
            //Don't set R/W/X, so this is directory entry,not a superpage
        }
    }
    return &pagetable[PX(target_level, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;

    if (va >= MAXVA) return 0;

    pte = walk(pagetable, va, 0, 0);
    if (pte == 0) return 0;
    if ((*pte & PTE_V) == 0) return 0;
    if ((*pte & PTE_U) == 0) return 0;
    pa = PTE2PA(*pte);
    return pa;
}

#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
void prefix_helper(char c, int count){
    int i=count;
    while(i-- > 0)  printf("%c", c);
}
uint64 get_step_size(int depth){
    if(depth==1)    return 1ull<<30;
    else if(depth==2)   return 1ull<<21;
    else if(depth==3)   return 1ull<<12;
    return 0;
}
void walk_all_page(uint64 start_va, pagetable_t pagetable, int depth){
    for(int i=0;i<512;i++){
        pte_t pte=pagetable[i];
        uint64 pa=PTE2PA(pte);
        if(pte & PTE_V){
            prefix_helper('.', depth*2);
            printf("%p: pte %p pa %p\n", (void *)start_va, (void *)pte, (void *)pa);
            if((pte & (PTE_X | PTE_W | PTE_R))==0){
                //this PTE points to a lower-level page table.
                walk_all_page(start_va, (pagetable_t)(void *)pa, depth+1);
                //have not handle current va
            }
        }
        start_va+=get_step_size(depth);
    }
}

void vmprint(pagetable_t pagetable) {
    // your code here'
    printf("page table %p\n", pagetable);
    walk_all_page(0, pagetable, 1);
}
#endif

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
    if (mappages(kpgtbl, va, sz, pa, perm) != 0) panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
// int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
//     uint64 a, last;
//     pte_t *pte;
//     if ((va % PGSIZE) != 0) panic("mappages: va not aligned");
//     if ((size % PGSIZE) != 0) panic("mappages: size not aligned");
//     if (size == 0) panic("mappages: size");
//     a = va;
//     last = va + size - PGSIZE;
//     for (;;) {
//         if ((pte = walk(pagetable, a, 1, 0)) == 0) return -1;
//         if (*pte & PTE_V) panic("mappages: remap");
//         *pte = PA2PTE(pa) | perm | PTE_V;
//         if (a == last) break;
//         a += PGSIZE;
//         pa += PGSIZE;
//     }
//     return 0;
// }
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm){
    pte_t *pte;
    uint64 basic_size=PGSIZE, target_level;
    if(size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (size == 0) panic("mappages: size");
    if(size>=PGSIZE && size<MEGAPGSIZE){
        target_level=0;
        basic_size=PGSIZE;
    }
    else if(size>=MEGAPGSIZE && size<GIGAPGSIZE){
        target_level=1;
        basic_size=MEGAPGSIZE;
    }
    else{
        target_level=2;
        basic_size=GIGAPGSIZE;
    }
    if ((va % basic_size) != 0) panic("mappages: va not aligned");
    if ((pa % basic_size) != 0) panic("mappages: pa not aligned");
    pte=walk(pagetable, va, 1, target_level);//enable superblock
    while(size>0){
        if(pte==0)  return -1;
        if(*pte & PTE_V)    panic("mappages: remap");
        *pte= PA2PTE(pa) | PTE_V | perm;//Assign
        size-=basic_size;
        va+=basic_size;
        pa+=basic_size;
        pte++;  //update the pte quickly,no need to walk agin
        if(PX(target_level, va)==0)    //check if arrive the boundary
            pte=walk(pagetable, va, 1, target_level);   //walk again
    }
    return 0;
}


// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == 0) return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 size, int do_free){
    pte_t *pte; //Can only handle the release of pages with the same size
    uint64 basic_size=PGSIZE, target_level;
    if(size%PGSIZE!=0)  panic("mappages: size not aligned");
    if (size == 0) panic("mappages: size");
    if(size>=PGSIZE && size<MEGAPGSIZE){
        target_level=0;
        basic_size=PGSIZE;
    }
    else if(size>=MEGAPGSIZE && size<GIGAPGSIZE){
        target_level=1;
        basic_size=MEGAPGSIZE;
    }
    else{
        target_level=2;
        basic_size=GIGAPGSIZE;
    }
    if ((va % basic_size) != 0) panic("uvmunmap: va not aligned");
    pte=walk(pagetable, va, 0, target_level);//enable superblock
    while(size>0){
        if(pte==0)  panic("try to unmap an unexisting mapping!");
        if((*pte & PTE_V)!=0){
            if(do_free){
                uint64 pa=PTE2PA(*pte);
                free_pages((void *)pa);
            }
            *pte=0;
        }
        size-=basic_size;
        va+=basic_size;
        pte++;  //update the pte quickly,no need to walk agin
        if(PX(target_level, va)==0)    //check if arrive the boundary
            pte=walk(pagetable, va, 0, target_level);   //walk again
    }
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
    if(newsz<oldsz) return oldsz;
    uint64 aligned_oldsz=PGROUNDUP(oldsz), aligned_newsz=PGROUNDUP(newsz);
    if(aligned_oldsz==aligned_newsz)    return newsz;
    uint8 oldsz_order=find_last_set(aligned_oldsz & -aligned_oldsz);
    oldsz_order=(oldsz_order==0 || oldsz_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):oldsz_order;
    uint64 cur_max_size=1ull << oldsz_order;
    uint64 remain_size=aligned_newsz-aligned_oldsz, alloc_size=cur_max_size;
    void *mem;
    uint64 cur_va=aligned_oldsz, cur_order=0;
    while(remain_size>0){
        alloc_size=cur_max_size;
        while(remain_size < alloc_size && alloc_size>PGSIZE)
            alloc_size/=2;
        mem=alloc_memory(alloc_size);
        if (mem == 0) {
            uvmdealloc(pagetable, cur_va, aligned_oldsz);
            return 0;
        }
        memset(mem, 0, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, mem, PTE_R | PTE_U | xperm)!=0){
            free_pages(mem);
            uvmdealloc(pagetable, cur_va, aligned_oldsz);
            return 0;
        }
        if(remain_size>alloc_size)  remain_size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        cur_order=find_last_set(cur_va & -cur_va);
        cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull << cur_order;
    }
    return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz) return oldsz;
    int aligned_newsz=PGROUNDUP(newsz), aligned_oldsz=PGROUNDUP(oldsz);
    if (aligned_newsz < aligned_oldsz) {
        // Perform the release step-by-step, following the reverse logic of alloc.
        uint8 oldsz_order=find_last_set(aligned_oldsz & -aligned_oldsz);
        oldsz_order=(oldsz_order==0 || oldsz_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):oldsz_order;
        uint64 cur_max_size=1ull << oldsz_order;
        uint64 remain_size=aligned_newsz-aligned_oldsz, free_size=cur_max_size;
        uint64 cur_va=aligned_oldsz, cur_order=0;
        while(remain_size>0){
            free_size=cur_max_size;
            while(remain_size < free_size && free_size>PGSIZE)
                free_size/=2;
            uvmunmap(pagetable, cur_va-free_size, free_size, 1);
            if(remain_size>free_size)  remain_size-=free_size;
            else    break;
            cur_va-=free_size;
            cur_order=find_last_set(cur_va & -cur_va);
            cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
            cur_max_size=1ull << cur_order;
        }
        // int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        // uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    return newsz;
}

// Recursively free page-table pages.
void freewalk(pagetable_t pagetable, int do_free) {
    // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        uint64 pa = PTE2PA(pte);    //child pagetable or physical
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // this PTE points to a lower-level page table.
            freewalk((pagetable_t)pa, do_free);
        } else if (pte & PTE_V && do_free!=0) {   //leaf-node,release the physical page
            free_pages((void *)pa);
        }
        pagetable[i] = 0;
    }
    free_pages((void *)pagetable);
}
// Recursively copy page-table pages.Consider the superpage
// if error is ture, we should free the allocated resources and quit recursively
void copywalk(pagetable_t old_pg, pagetable_t new_pg, uint64 base_va, int depth, int *error){
    pte_t pte;
    uint64 pa,cur_size=get_step_size(depth), cur_va=base_va;
    char *mem;
    uint flags;
    for (int i = 0; i < 512; i++) {
        pte = old_pg[i];
        pa = PTE2PA(pte);    //child pagetable or physical
        flags=PTE_FLAGS(pte);
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // this PTE points to a lower-level page table.
            copywalk((pagetable_t)pa, new_pg, cur_va, depth+1, error);
            if(*error==-1)  return;  //fast exit and releasing all allocated resources.
        } else if (pte & PTE_V) {   //leaf-node,release the physical page
            mem=alloc_memory(cur_size);
            if(mem==0)  goto free_and_release;
            memmove(mem, (char *)pa, cur_size);
            if(mappages(new_pg, cur_va, cur_size, (uint64)mem, flags)!=0){
                free_pages(mem);
                goto free_and_release;
            }
        }
        cur_va+=cur_size;
    }
    return;
free_and_release:   //prevent resources leaks and waste
    uvmfree(new_pg, cur_va);
    *error=1;
    return;
}

// Free user memory pages,(maintain the interface consistency, 
// an non-zero sz implies the need to release the corresponding page)
// Even though the release process itself does not requires the sz parameter.
// Must free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0) freewalk(pagetable, 1);
    else    freewalk(pagetable, 0);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old_pg, pagetable_t new_pg, uint64 sz) {
    uint64 start_va=0;
    int error_flags=0;
    copywalk(old_pg, new_pg, start_va, 1, &error_flags);
    if(error_flags==1)  return -1;
    return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
    pte_t *pte;

    pte = walk(pagetable, va, 0, 0);
    if (pte == 0) panic("uvmclear");
    *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    uint64 aligned_dstva=PGROUNDDOWN(dstva);
    uint8 cur_order=find_last_set(aligned_dstva & -aligned_dstva);
    cur_order=(cur_order==0 || cur_order>MAX_ORDER+ORDER_BASE)
        ?(MAX_ORDER+ORDER_BASE):cur_order;
    uint64 cur_max_size=1ull << cur_order;
    uint64 alloc_size=cur_max_size;
    void *mem;
    uint64 cur_va=dstva, basepage_va, cur_pa;
    pte_t *pte;
    


        memset(mem, 0, alloc_size);
        if(mappages(pagetable, cur_va, alloc_size, mem, PTE_R | PTE_U | xperm)!=0){
            free_pages(mem);
            uvmdealloc(pagetable, cur_va, aligned_oldsz);
            return 0;
        }
        if(remain_size>alloc_size)  remain_size-=alloc_size;
        else    break;
        cur_va+=alloc_size;
        cur_order=find_last_set(cur_va & -cur_va);
        cur_order=(cur_order > MAX_ORDER+ORDER_BASE)?(MAX_ORDER+ORDER_BASE):cur_order;
        cur_max_size=1ull << cur_order;
    }
    return newsz;

    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA) return -1;

        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, va0, 0)) == 0) {
                return -1;
            }
        }

        if ((pte = walk(pagetable, va0, 0, 0)) == 0) {
            // printf("copyout: pte should exist %lx %ld\n", dstva, len);
            return -1;
        }

        // forbid copyout over read-only user text pages.
        if ((*pte & PTE_W) == 0) return -1;

        n = PGSIZE - (dstva - va0);
        if (n > len) n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
    uint64 n, va0, pa0;

    while (len > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, va0, 0)) == 0) {
                return -1;
            }
        }
        n = PGSIZE - (srcva - va0);
        if (n > len) n = len;
        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    uint64 n, va0, pa0;
    int got_null = 0;

    while (got_null == 0 && max > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) return -1;
        n = PGSIZE - (srcva - va0);
        if (n > max) n = max;

        char *p = (char *)(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PGSIZE;
    }
    if (got_null) {
        return 0;
    } else {
        return -1;
    }
}




// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64 vmfault(pagetable_t pagetable, uint64 va, int read) {
    uint64 mem;
    struct proc *p = myproc();

    if (va >= p->sz) return 0;
    va = PGROUNDDOWN(va);
    if (ismapped(pagetable, va)) {
        return 0;
    }
    mem = (uint64)kalloc();
    if (mem == 0) return 0;
    memset((void *)mem, 0, PGSIZE);
    if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
        kfree((void *)mem);
        return 0;
    }
    return mem;
}

int ismapped(pagetable_t pagetable, uint64 va) {
    pte_t *pte = walk(pagetable, va, 0, 0);
    if (pte == 0) {
        return 0;
    }
    if (*pte & PTE_V) {
        return 1;
    }
    return 0;
}

#ifdef LAB_PGTBL
pte_t *pgpte(pagetable_t pagetable, uint64 va) { return walk(pagetable, va, 0, 0); }
#endif
