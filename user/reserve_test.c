#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"
#include "kernel/vm.h"

// --- 配置参数 ---
#ifndef THRESHLOD
#define THRESHLOD 32  
#endif
#ifndef SUPERPGSIZE
#define SUPERPGSIZE (2 * 1024 * 1024)
#endif
#define RESERVED_HEAP_START 0x200000

// --- 全局状态 ---
char *current_test_name = "init";
int global_fail_count = 0;

// --- 宏定义 ---
#define LOG(fmt, ...) printf("  [INFO] " fmt "\n", ##__VA_ARGS__)
#define STEP(fmt, ...) printf("\n>>> [STEP] " fmt "\n", ##__VA_ARGS__)

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s: " msg "\n", current_test_name); \
        global_fail_count++; \
    } else { \
        printf("  [PASS] " msg "\n"); \
    } \
} while(0)

#define CHECK_FATAL(cond, msg) do { \
    if (!(cond)) { \
        printf("\n\t[FATAL ERROR] %s: " msg "\n", current_test_name); \
        exit(1); \
    } else { \
        printf("  [PASS] " msg "\n"); \
    } \
} while(0)

// --- 辅助工具 ---

void err(char *why) {
    printf("pgtbltest: %s failed: %s, pid=%d\n", current_test_name, why, getpid());
    exit(1);
}

// 智能获取物理地址
// 如果是 HugePage PTE，需要手动加上偏移量才能得到真实的 PA
uint64 get_true_pa(uint64 va) {
    pte_t pte = (pte_t)pgpte((void*)va);
    if ((pte & PTE_V) == 0) return 0;

    uint64 base_pa = PTE2PA(pte);
    
    // 判断是否为 Huge Page (Level 1 Leaf)
    // 注意：这里假设 pgpte 返回的是 raw PTE。
    // 在 RISC-V 中，如果 PTE 的 R/W/X 位被设置，则为叶子节点。
    // 我们的 2MB 页通常对齐到 SUPERPGSIZE。
    
    // 如果 base_pa 是 2MB 对齐的，并且我们是在测试大页场景，
    // 且 PTE 指向的是 2MB 范围，那么真实的 PA = Base + Offset
    if (base_pa % SUPERPGSIZE == 0 && (pte & 0xF) != 0) { 
        // 这是一个简化的判断，因为 4K 页也可能偶然 2MB 对齐
        // 但在大页映射中，base_pa 始终是 2MB 的头部
        return base_pa + (va % SUPERPGSIZE);
    }
    
    // 普通 4K 页，PTE2PA 指向的就是该 4K 页的起始
    return base_pa; 
}

// 堆对齐工具
char* align_heap_to_superpage() {
    char *curr = sbrklazy(0);
    uint64 curr_val = (uint64)curr;
    
    if (curr_val < RESERVED_HEAP_START) {
        uint64 gap = RESERVED_HEAP_START - curr_val;
        sbrklazy(gap);
    }

    curr = sbrklazy(0);
    uint64 align_diff = SUPERPGROUNDUP((uint64)curr) - (uint64)curr;
    if (align_diff > 0) {
        sbrklazy(align_diff);
    }
    
    return sbrklazy(0);
}

// =========================================================================
// TEST 1: 预留机制与物理连续性测试
// =========================================================================
void reservation_basic_test() {
    current_test_name = "reservation_basic";
    STEP("Starting Basic Reservation Test...");

    char *base = align_heap_to_superpage();
    LOG("Test Base VA: %p", base);

    // 1. 申请 2MB (Lazy)
    if (sbrklazy(SUPERPGSIZE) == (char*)-1) CHECK_FATAL(0, "sbrk 2M failed");

    // 2. 触发分配 Page 0 和 Page 1
    // 此时尚未达到 Threshold，应该是 4K 映射
    *base = 'A'; 
    *(base + PGSIZE) = 'B';
    *(base + 2*PGSIZE) ='C';
    *(base + 3*PGSIZE) ='D';
    *(base + 4*PGSIZE) ='E';
    pte_t pte_0 = (pte_t)pgpte(base);
    pte_t pte_1 = (pte_t)pgpte(base + PGSIZE);
    
    CHECK((pte_0 & PTE_V) != 0, "Page 0 allocated");
    
    // 3. 验证物理连续性
    // 即使是 4K PTE，如果预留生效，PA 应该是连续的
    uint64 pa_0 = PTE2PA(pte_0);
    uint64 pa_1 = PTE2PA(pte_1); // 4K 模式下，PTE2PA 直接返回物理页地址
    
    LOG("PA_0: 0x%lx, PA_1: 0x%lx", pa_0, pa_1);
    
    if (pa_1 == pa_0 + PGSIZE) {
        CHECK(1, "Physical pages are contiguous (Reservation Active)");
    } else {
        // 这不一定是错误，可能是系统内存碎片导致无法预留，回退到了 Scattered
        printf("  [WARN] Physical pages are SCATTERED (Reservation Inactive or System Fragmented)\n");
    }

    // 清理
    sbrklazy(-SUPERPGSIZE);
}

// =========================================================================
// TEST 2: 升级、数据迁移与降级
// =========================================================================
void huge_promotion_test() {
    current_test_name = "huge_promotion";
    STEP("Starting Promotion & Demotion Test...");

    char *base = align_heap_to_superpage();
    sbrklazy(SUPERPGSIZE);

    // 1. 填充直到阈值前
    for (int i = 0; i < THRESHLOD - 1; i++) {
        *(base + i * PGSIZE) = (char)(i & 0xFF);
    }
    
    // 记录升级前的 Page 0 物理地址
    uint64 pa_0_before = PTE2PA((pte_t)pgpte(base));

    // 2. 触发升级
    LOG("Triggering Promotion (Write at index %d)...", THRESHLOD - 1);
    *(base + (THRESHLOD - 1) * PGSIZE) = 0xAA;

    pte_t huge_pte = (pte_t)pgpte(base);
    uint64 huge_base_pa = PTE2PA(huge_pte);

    // 3. 验证升级结果
    // 检查 A: PTE 指向的 PA 是否 2MB 对齐
    CHECK(huge_base_pa % SUPERPGSIZE == 0, "PA aligned to 2MB (Huge Page)");

    // 检查 B: 迁移 vs 原地
    // 如果之前是连续分配的，huge_base_pa 应该等于 pa_0_before
    if (huge_base_pa == pa_0_before) {
        LOG("Promotion Type: In-Place (Efficient)");
    } else {
        LOG("Promotion Type: Migration (Address changed 0x%lx -> 0x%lx)", pa_0_before, huge_base_pa);
    }

    // [关键修正]: 在大页模式下，验证 VA0 和 VA1 的真实物理地址
    // 如果是 HugePage，pgpte(base) 和 pgpte(base+4k) 返回相同的 PTE
    // 所以我们需要验证的是内核是否正确建立了大页映射
    pte_t pte_check_1 = (pte_t)pgpte(base + PGSIZE);
    if (pte_check_1 == huge_pte) {
         LOG("Confirmed: VA+4K maps to the same Huge PTE as VA_Base");
    } else {
         // 如果你的实现即使在大页下也保留了 4K PTE (不太可能)，这里会不同
         CHECK(0, "Page table structure unclear (Huge PTE mismatch)");
    }

    // 4. 数据校验
    int data_ok = 1;
    for (int i = 0; i < THRESHLOD - 1; i++) {
        if (*(base + i * PGSIZE) != (char)(i & 0xFF)) data_ok = 0;
    }
    CHECK(data_ok, "Data integrity maintained");

    // 5. 降级测试 (Partial Free)
    STEP("Testing Demotion (Freeing half)...");
    
    // 释放 1MB
    sbrklazy(-(SUPERPGSIZE / 2));
    
    // 此时大页应该被拆解 (Split) 回 4K 页
    // 验证 Page 0 的 PTE
    pte_t demoted_pte_0 = (pte_t)pgpte(base);
    uint64 demoted_pa_0 = PTE2PA(demoted_pte_0);
    
    // 验证 Page 1 的 PTE
    pte_t demoted_pte_1 = (pte_t)pgpte(base + PGSIZE);
    uint64 demoted_pa_1 = PTE2PA(demoted_pte_1);
    
    // 降级后，它们应该是独立的 4K PTE，所以 PTE 值本身可能不同(flag不同)，
    // 但物理地址应该依然连续（因为它们来自同一个大页的拆解）
    
    CHECK(*base == 0, "Data accessible after demotion");
    
    if (demoted_pa_1 == demoted_pa_0 + PGSIZE) {
        CHECK(1, "Physical contiguity maintained after demotion");
    } else {
        CHECK(0, "Contiguity lost after demotion (Unexpected logic)");
    }

    sbrklazy(-(SUPERPGSIZE / 2));
}

// =========================================================================
// TEST 3: Deep Copy 测试
// =========================================================================
void huge_fork_deep_copy_test() {
    current_test_name = "huge_fork_deep";
    STEP("Starting Deep Copy Fork Test...");

    char *base = align_heap_to_superpage();

    // 1. 制造大页
    sbrklazy(SUPERPGSIZE);
    for (int i = 0; i < THRESHLOD + 1; i++) *(base + i * PGSIZE) = 0xCC;

    pte_t p_pte = (pte_t)pgpte(base);
    uint64 p_pa = PTE2PA(p_pte);
    CHECK_FATAL(p_pa % SUPERPGSIZE == 0, "Parent has Huge Page");
    kpgtbl();
    int pid = fork();

    if (pid == 0) {
        // >>> 子进程 <<<
        current_test_name = "child";
        int fails = 0;

        pte_t c_pte = (pte_t)pgpte(base);
        uint64 c_pa = PTE2PA(c_pte);
        
        // 检查 1: PTE 有效
        if ((c_pte & PTE_V) == 0) {
            printf("  [CHILD-FAIL] PTE Invalid.\n"); fails++;
        }

        // 检查 2: 物理地址不同 (Deep Copy)
        if (c_pa == p_pa) {
            printf("  [CHILD-FAIL] PA Shared (0x%lx)! Expected Deep Copy.\n", c_pa); fails++;
        } else {
            printf("  [CHILD-PASS] PA Distinct (0x%lx vs 0x%lx).\n", c_pa, p_pa);
        }

        // 检查 3: 大页保留
        if (c_pa % SUPERPGSIZE != 0) {
            printf("  [CHILD-WARN] Child degraded to Small Pages.\n");
        }

        if (fails > 0) exit(1);
        
        char *argv[] = {"echo", "  [CHILD] Exec success.", 0};
        exec("echo", argv);
        exit(0);
    } else {
        wait(0);
        sbrklazy(-SUPERPGSIZE);
    }
}

int main() {
    printf("\n=== PG_TBL_TEST SUITE ===\n");
    // reservation_basic_test();
    // huge_promotion_test();
    huge_fork_deep_copy_test();
    
    if (global_fail_count == 0) printf("\nRESULT: [ SUCCESS ]\n");
    else printf("\nRESULT: [ FAILED ] Errors: %d\n", global_fail_count);
    exit(0);
}