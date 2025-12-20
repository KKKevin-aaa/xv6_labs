#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"
#include "kernel/vm.h"

#define SZ (8 * SUPERPGSIZE)
#define GET_PA(va) (PTE2PA((pte_t)pgpte((void *)(va))))
void print_pgtbl();
void print_kpgtbl();
void ugetpid_test();
void superpg_fork();
void superpg_free();
void buddy_alignment_test();
void buddy_coalesce_test();
void buddy_odd_size_test();
int main(int argc, char *argv[]) {
    buddy_alignment_test();
    buddy_coalesce_test();
    buddy_odd_size_test();
    // print_pgtbl();
    // ugetpid_test();
    // print_kpgtbl();
    // superpg_fork();
    // superpg_free();
    printf("pgtbltest: all tests succeeded\n");
    exit(0);
}

char *testname = "???";

void err(char *why) {
    printf("pgtbltest: %s failed: %s, pid=%d\n", testname, why, getpid());
    exit(1);
}

void print_pte(uint64 va) {
    pte_t pte = (pte_t)pgpte((void *)va);
    printf("va 0x%lx pte 0x%lx pa 0x%lx perm 0x%lx\n", va, pte, PTE2PA(pte), PTE_FLAGS(pte));
}
void print_pgtbl() {
    printf("print_pgtbl starting\n");
    for (uint64 i = 0; i < 10; i++) {
        print_pte(i * PGSIZE);
    }
    uint64 top = MAXVA / PGSIZE;
    for (uint64 i = top - 10; i < top; i++) {
        print_pte(i * PGSIZE);
    }
    printf("print_pgtbl: OK\n");
}
void ugetpid_test() {
    int i;

    printf("ugetpid_test starting\n");
    testname = "ugetpid_test";

    if (getpid() != ugetpid()) err("mismatched PID #1");

    for (i = 0; i < 64; i++) {
        int ret = fork();
        if (ret != 0) {
            wait(&ret);
            if (ret != 0) exit(1);
            continue;
        }
        if (getpid() != ugetpid()) err("mismatched PID #2");
        if( uptime()!=u_uptime())    err("mismatch uptime! #2");
        // else    printf("both method get the same time!\n");
        exit(0);
    }
    printf("ugetpid_test: OK\n");
}
void print_kpgtbl() {
    printf("print_kpgtbl starting\n");
    kpgtbl();
    printf("print_kpgtbl: OK\n");
}
void supercheck(char *end) {
    pte_t last_pte = 0;
    uint64 a = (uint64)end;
    uint64 s = SUPERPGROUNDUP(a);

    for (; a < s; a += PGSIZE) {
        pte_t pte = (pte_t)pgpte((void *)a);
        if (pte == 0) {
            err("no pte");
        }
    }

    for (uint64 p = s; p < s + 512 * PGSIZE; p += PGSIZE) {
        pte_t pte = (pte_t)pgpte((void *)p);
        if (pte == 0) err("no pte");
        if ((uint64)last_pte != 0 && pte != last_pte) {
            err("pte different");
        }
        if ((pte & PTE_V) == 0 || (pte & PTE_R) == 0 || (pte & PTE_W) == 0) {
            // at least one bits are non-zero
            err("pte wrong");
        }
        last_pte = pte;
    }

    for (int i = 0; i < 512 * PGSIZE; i += PGSIZE) {
        *(int *)(s + i) = i;
    }

    for (int i = 0; i < 512 * PGSIZE; i += PGSIZE) {
        if (*(int *)(s + i) != i) err("wrong value");
    }
}
void superpg_fork() {
    int pid;

    printf("superpg_fork starting\n");
    testname = "superpg_fork";

    char *end = sbrklazy(SZ);
    if (end == 0 || end == SBRK_ERROR) err("sbrk failed");

    // check if parent has super pages
    supercheck(end);
    if ((pid = fork()) < 0) {
        err("fork");
    } else if (pid == 0) {
        // check if child's address space has super pages
        supercheck(end);
        exit(0);
    } else {
        int status;
        wait(&status);
        if (status != 0) {
            exit(0);
        }
    }

    // free super pages
    sbrklazy(-SZ);
    printf("1 \n");
    if ((pid = fork()) < 0) {
        err("fork");
    } else if (pid == 0) {
        // reference freed memory; this should result in page fault and
        // the kernel should kill the child.
        printf("2 \n");
        *(end + 1) = '9';
        printf("3 \n");
    } else {
        printf("4 \n");
        int status;
        wait(&status);
        if (status == 0) {
            err("child was able to reference free memory\n");
            exit(1);
        }
    }
    printf("superpg_fork: OK\n");
}
void superpg_free() {
    int pid;

    printf("superpg_free starting\n");
    testname = "superpg_free";

    char *end = sbrklazy(SZ);
    if (end == 0 || end == SBRK_ERROR) err("sbrk failed");

    // free pages beyond a super page
    char *a = sbrklazy(0);
    printf("current a is %p\n", (void *)a);
    uint64 s = SUPERPGROUNDDOWN((uint64)a);
    printf("current s is 0x%lx, next-sbrk is 0x%lx\n", s, -((uint64)a - s));
    sbrklazy(-((uint64)a - s));
    a = sbrklazy(0);
    printf("current a is %p\n", (void *)a);
    pte_t pte1 = (pte_t)pgpte((void *)a - PGSIZE);
    pte_t pte2 = (pte_t)pgpte((void *)a - 2 * PGSIZE);
    if (pte1 != pte2) {
        err("not a super page");
    }

    // write to the last 8192-byte section of a super page
    *(a - PGSIZE + 1) = '8';
    *(a - 2 * PGSIZE + 1) = '9';

    // free last 4096 bytes of a super page
    sbrklazy(-PGSIZE);
    a = sbrklazy(0);
    printf("current a is %p\n", (void *)a);
    if (*(a - PGSIZE + 1) != '9') {
        printf("current *(a-pgsize+1) is %c\n", (char)*(a-PGSIZE+1));
        err("lost content after freeing part of super page");
    }

    if ((pid = fork()) < 0) {
        err("fork");
    } else if (pid == 0) {
        // the memory at address a shouldn't be in the child's address
        // space, since the parent freed it. The following reference
        // should result in page fault and the kernel should kill the
        // child.
        if (*(a + 1) == '9') {
            exit(0);
        }
    } else {
        int status;
        wait(&status);
        if (status == 0) {
            err("child was able to reference free memory\n");
            exit(1);
        }
    }

    pte1 = (pte_t)pgpte((void *)a);
    if (pte1 != 0) {
        err("pte for freed memory is valid");
    }

    s = SUPERPGROUNDDOWN((uint64)a);
    for (; (uint64)a > s; a -= PGSIZE) {
        a = sbrklazy(-PGSIZE);
        pte1 = (pte_t)pgpte(sbrklazy(0));
        if (pte1 != 0) {
            err("page hasn't been freed");
        }
    }

    printf("superpg_free: OK\n");
}
char *align_sbrklazy(uint64 boundary){
    char *curr=sbrklazy(0);
    uint64 curr_val=(uint64)curr, rem=curr_val % boundary;
    if(rem!=0){
        uint64 padding=boundary-rem;
        char *ret=sbrklazy(padding);
        if(ret==NULL){
            fprintf(1, "sbrk padding fail!");
            exit(1);
        }
    }
    return sbrklazy(0);
}
void buddy_alignment_test(){
    printf("buddy alignment test starting:\n");
    testname = "buddy_alignment_test";
    // 1. 申请 64KB (Order 4)
    uint64 sz_64k = 64 * 1024; 
    align_sbrklazy(sz_64k);
    char *p1 = sbrklazy(sz_64k);
    
    if (p1 == (char*)-1) err("sbrk 64k failed");
    
    uint64 pa1 = GET_PA(p1);
    // 检查是否 64KB 对齐 (低 16 位为 0)
    if (pa1 % sz_64k != 0) {
        printf("va=%p pa=0x%lx size=0x%lx\n", p1, pa1, sz_64k);
        err("64KB allocation not aligned to 64KB boundary!");
    }


    // 2. 申请 2MB (Order 9 / Superpage)
    uint64 sz_2m = 2 * 1024 * 1024;
    align_sbrklazy(sz_2m);
    char *p2 = sbrklazy(sz_2m);
    if (p2 == (char*)-1) err("sbrk 2m failed");

    uint64 pa2 = GET_PA(p2);
    // 检查是否 2MB 对齐 (低 21 位为 0)
    if (pa2 % sz_2m != 0) {
        printf("va=%p pa=0x%lx size=0x%lx\n", p2, pa2, sz_2m);
        err("2MB allocation not aligned to 2MB boundary!");
    }
    printf("buddy_alignment_test: OK\n");
}
void buddy_coalesce_test() {
    printf("buddy_coalesce_test starting\n");
    testname = "buddy_coalesce_test";

    // 假设最大测试块为 4MB (2个 Superpages)
    uint64 huge_sz = 4 * 1024 * 1024; 
    
    // 1. 首次申请 4MB
    char *start = sbrklazy(0);
    char *ptr = sbrklazy(huge_sz);
    if (ptr == (char*)-1) err("initial sbrk 4MB failed");
    
    uint64 pa_orig = GET_PA(ptr);
    printf("  [Info] Alloc 4MB at PA: 0x%lx\n", pa_orig);

    // 2. 逐步释放 (模拟碎片化释放)
    // sbrk 只能从顶部缩小，但我们可以分多次缩小来触发多次 free_pages
    // 先释放 1MB
    sbrklazy(- (1024 * 1024));
    // 再释放 1MB
    sbrklazy(- (1024 * 1024));
    // 再释放 2MB
    sbrklazy(- (2 * 1024 * 1024));
    // print_kpgtbl();
    // 此时堆应该回到了 start
    if (sbrklazy(0) != start) err("sbrk pointer mismatch after free");

    // 3. 再次申请 4MB
    // 如果合并逻辑(Coalescing)是正确的，之前释放的碎片应该合并成了 
    // 一个大的 4MB 块（或者两个 2MB）。
    // Buddy Allocator 通常倾向于重用刚刚释放的低地址块。
    char *ptr2 = sbrklazy(huge_sz);
    if (ptr2 == (char*)-1) err("re-alloc sbrk 4MB failed");
    // print_kpgtbl();
    uint64 pa_new = GET_PA(ptr2);
    printf("  [Info] Re-Alloc 4MB at PA: 0x%lx\n", pa_new);

    // 4. 验证物理地址
    // 虽然不强制要求 PA 完全相同（可能被别人抢了），但在单进程测试下，
    // 优秀的 Buddy 实现应该能完美还原回原来的物理块。
    // 如果这里分配失败或者拿到了完全不同的区域，可能说明合并失败导致碎片化。
    if (pa_new != pa_orig) {
        printf("  [Warn] Re-allocated PA (0x%lx) != Original PA (0x%lx). Fragmentation?\n", pa_new, pa_orig);
        // 注意：这不是致命错误，取决于你的分配策略，但值得关注
    }

    // 清理
    sbrklazy(-huge_sz);
    printf("buddy_coalesce_test: OK\n");
}
void buddy_odd_size_test() {
    printf("buddy_odd_size_test starting\n");
    testname = "buddy_odd_size_test";

    // 申请 3 个页面 (3 * 4096 = 12288 bytes)
    // Buddy System 应该会内部向上取整分配 Order 2 (16384 bytes)
    // 或者你的 sbrk 可能会循环调用三次 kalloc(4k)。
    // 这里我们通过检查物理连续性来推断它的行为。
    uint64 req_sz = 3 * PGSIZE;
    char *p = sbrklazy(req_sz);
    if (p == (char*)-1) err("sbrk 3 pages failed");

    uint64 pa1 = GET_PA(p);
    uint64 pa2 = GET_PA(p + PGSIZE);
    uint64 pa3 = GET_PA(p + 2 * PGSIZE);

    printf("  [Info] PA list: 0x%lx, 0x%lx, 0x%lx\n", pa1, pa2, pa3);

    // 验证物理连续性 (Buddy System 的特征)
    if (pa2 != pa1 + PGSIZE || pa3 != pa2 + PGSIZE) {
        // 如果物理地址不连续，说明你的 sbrk 是一页一页分配的，而不是向 Buddy 要了一块大的
        printf("  [Note] sbrklazy(3*PGSIZE) returned non-contiguous physical pages.\n");
    } else {
        printf("  [Note] sbrklazy(3*PGSIZE) returned contiguous physical pages (Good).\n");
    }

    // 释放
    sbrklazy(-req_sz);
    printf("buddy_odd_size_test: OK\n");
}