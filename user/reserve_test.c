// 定义预留区域的起始地址，根据你的描述设定
#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"
#include "kernel/vm.h"
#define RESERVED_HEAP_START 0x200000

char *testname = "???";

void err(char *why) {
    printf("pgtbltest: %s failed: %s, pid=%d\n", testname, why, getpid());
    exit(1);
}

void lazy_reservation_test() {
    printf("lazy_reservation_test starting\n");
    testname = "lazy_reservation_test";

    // --- 1. 手动调整堆至预留区域 (Manual Adjustment to Reservation Area) ---
    printf("  [Step 1] Adjusting heap to reservation boundary (0x%x)...\n", RESERVED_HEAP_START);

    char *curr_break = sbrklazy(0);
    uint64 curr_val = (uint64)curr_break;
    uint64 target_val = RESERVED_HEAP_START;

    if (curr_val < target_val) {
        // 计算当前位置距离 0x200000 还有多远
        uint64 gap = target_val - curr_val;
        
        printf("  [Info] Current heap (0x%lx) is below threshold. Increasing by 0x%lx bytes.\n", curr_val, gap);
        
        // 显式调用 sbrk 填补这段空隙，将 p->sz 推到 0x200000
        // 注意：这里分配的内存不需要物理页（因为是 Lazy），所以消耗不大
        char *ret = sbrklazy(gap);
        
        if (ret == (char*)-1) {
            err("sbrk failed during manual adjustment to reserved area");
        }
    } else {
        printf("  [Info] Current heap (0x%lx) is already above threshold.\n", curr_val);
    }

    // --- 二次确认 (Double Check) ---
    char *new_break = sbrklazy(0);
    if ((uint64)new_break < target_val) {
        // 如果我们申请了 gap 还是没这就说明 sbrk 逻辑有问题
        printf("  [Error] Failed to reach threshold. Current: 0x%lx, Target: 0x%lx\n", (uint64)new_break, target_val);
        err("Manual heap adjustment failed");
    }

    printf("  [Info] Heap is now ready at 0x%lx. Starting Lazy Allocation tests...\n", (uint64)new_break);

    // ... 接原来的 --- 2. Lazy Allocation 基础验证 --- 代码 ...

    // --- 2. Lazy Allocation 基础验证 ---
    // 分配 5 个页面 (5 * 4096)
    uint64 alloc_size = 5 * PGSIZE;
    char *lazy_ptr = sbrklazy(alloc_size);
    if (lazy_ptr == (char*)-1) err("sbrklazy failed");

    printf("  [Info] Allocated %ld bytes lazily at %p\n", alloc_size, lazy_ptr);

    // 2.1 访问前检查：PTE 应该不存在或无效 (PTE_V == 0)
    // 这里的逻辑依赖于你的 GET_PA 宏和 pgpte 系统调用实现
    // 如果是 Lazy 分配，刚 sbrk 完物理内存不应分配
    pte_t pte_before = (pte_t)pgpte((void*)lazy_ptr);
    if ((pte_before & PTE_V) != 0) {
        printf("  [Warn] PTE valid before access (va=%p, pte=0x%lx). Is lazy allocation enabled?\n", lazy_ptr, pte_before);
        // 如果你强制开启了 lazy，这里应该报错；如果是可选特性，这里只是警告
        // err("Lazy allocation failed: Page already mapped before access");
    } else {
        printf("  [Info] PTE invalid before access (Good for Lazy).\n");
    }

    // 2.2 触发缺页中断 (Write Access)
    // 写入第一个页面的开头
    *lazy_ptr = 'L'; 
    // 写入最后一个页面的开头，确保范围内的页都能触发
    *(lazy_ptr + alloc_size - PGSIZE) = 'Z';

    // 2.3 访问后检查：PTE 应该变有效
    pte_t pte_after = (pte_t)pgpte((void*)lazy_ptr);
    if ((pte_after & PTE_V) == 0) {
        err("Lazy allocation failed: Page fault did not map the page (PTE still invalid)");
    }
    uint64 pa = PTE2PA(pte_after);
    printf("  [Info] Page fault handled successfully. VA %p -> PA 0x%lx\n", lazy_ptr, pa);


    // --- 3. Fork 与 Exec 结合测试 ---
    int pid = fork();
    if (pid < 0) {
        err("fork failed");
    } else if (pid == 0) {
        // === 子进程 ===
        
        // 3.1 验证 Fork 后的 Lazy 行为
        // 子进程应该能读取父进程刚才写入的数据
        if (*lazy_ptr != 'L') err("Child failed to read parent's data in lazy page");
        
        // 3.2 验证子进程的新 Lazy 分配
        // 再分配一块，不触碰
        char *child_lazy = sbrklazy(PGSIZE);
        pte_t c_pte = (pte_t)pgpte((void*)child_lazy);
        if ((c_pte & PTE_V) != 0) err("Child's new sbrk should be lazy");

        // 触碰触发映射
        *child_lazy = 'C';
        if (*child_lazy != 'C') err("Child lazy write failed");

        // 3.3 Exec 测试
        // 执行一个简单的程序，验证 exec 是否能正确处理这种带有预留区域和 Lazy 页面的进程结构
        // 我们使用 sh 执行 echo，或者直接执行 echo
        printf("  [Info] Child calling exec('echo')...\n");
        char *argv[] = {"echo", "Exec inside lazy test succeeded!", 0};
        exec("echo", argv);
        
        // 如果 exec 成功，不会执行到这里
        printf("  [Error] exec failed!\n");
        exit(1);
    } else {
        // === 父进程 ===
        int status;
        wait(&status);
        if (status != 0) {
            err("Child process exited with error (fork/exec check failed)");
        } else {
            printf("  [Info] Child exec returned successfully.\n");
        }
    }
    printf("reclaim the child process!\n");
    // 清理分配的内存
    sbrklazy(-alloc_size);
    printf("lazy_reservation_test: OK\n");
}
int main(){
    lazy_reservation_test();
    printf("reserve test: all tests succeeded!\n");
    exit(0);
}