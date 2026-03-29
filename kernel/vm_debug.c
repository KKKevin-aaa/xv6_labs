#include "param.h"
#include "types.h"
#include "atomic.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "rbtree.h"
#include "mm.h"
#include "vm_internal.h"
#include "colors.h"

// #if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
void prefix_helper(char c, int count){
    int i=count;
    while(i-- > 0)  printf("%c", c);
}

void walk_all_page(uint64 start_va, pagetable_t pagetable, int level){
    int idx=0, step=1;
    uint64 va_step, standard_stride=get_step_size(level);
    uint64 num_4k_page, page_per_slot;
    while(idx<512){
        pte_t pte=pagetable[idx];
        uint64 pa=PTE2PA(pte);
        if(pte & PTE_V){
            prefix_helper('.', (3-level)*2);
            if((pte & (PTE_X | PTE_W | PTE_R))==0){
                //this PTE points to a lower-level page table.
                printf("(next-level)%p: pte %p pa %p\n", (void *)start_va, (void *)pte, (void *)pa);
                walk_all_page(start_va, (pagetable_t)(void *)pa, level-1);
                //have not handle current va
                step=1;
                va_step=standard_stride;
            }
            else{
                if(is_managed_memory(pa)==0){
                    step=1;
                    va_step=get_step_size(level);
                }
                else{
                    num_4k_page=1ull<<get_order(pa);
                    page_per_slot=1ull<<(level*9);
                    step=num_4k_page/page_per_slot;
                    if(step<=0) step=1;
                    va_step=num_4k_page*PGSIZE;
                }
                printf("(data)%p: pte %p pa %p, size is 0x%llx\n", 
                    (void *)start_va, (void *)pte, (void *)pa, va_step);
            }
        }
        else{
            step=1;
            va_step=standard_stride;
        }
        idx+=step;
        start_va+=va_step;
    }
}
void vmprint(pagetable_t pagetable) {
    // your code here'
    printf("page table %p\n", (void *)pagetable);
    walk_all_page(0, pagetable, 2);
    // dump memory map
    dump_memory_map();
}
// #endif
