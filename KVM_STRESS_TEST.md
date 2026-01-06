# KVM Stress Test: `test_kvm_stress_worker`

本文档解释内核中 `test_kvm_stress_worker(iters, max_live, max_pages)` 的设计意图、参数含义、关键控制流、锁约束与失败模式，并回答一个常见疑问：**会不会出现“没有 alloc 就 dealloc”导致错误**。

> 代码位置：`kernel/kvm.c`（函数名：`test_kvm_stress_worker`、`test_kvm_stress_parallel` 等）。

## 1. 这个 worker 在测什么

`test_kvm_stress_worker` 是一个“随机压力测试循环”，核心目标是让如下路径在高频/随机组合下被覆盖：

- **映射分配**：调用 `kvmalloc(kernel_pagetable, sz, perm)` 创建一段连续虚拟地址区间的映射。
- **映射回收**：调用 `kvmdealloc(kernel_pagetable, va, sz)` 解除映射并释放资源。
- **真实访问验证**：对分配到的内存每页写入/读取一个字节（`kvm_test_touch_pages`），强制触发页表翻译与写权限检查。
- **元数据一致性验证**：周期性持有 `global_mm.mm_lock` 调用 `kvm_test_assert_mm_invariants_locked(&global_mm)`，检查 VMA 链表与 RB-tree 的一致性、不重叠、有序等不变量。

它不是“功能测试单点断言”，而是通过随机序列放大竞态、越界、缺页、元数据结构损坏等问题的概率。

## 2. 参数含义与边界处理

函数签名：

- `iters`：循环次数（每次循环执行一次 alloc 或 dealloc）。
  - 如果 `iters <= 0`，会用默认值 `1000`。
- `max_live`：允许同时处于“已分配但尚未释放”的最大记录数（也可理解为最大“存活分配数”）。
  - 如果 `max_live <= 0`，默认 `128`。
  - 会被限制到一个上界（代码里既有外层 clamp，也会被 clamp 到测试内部固定数组容量）。
- `max_pages`：单次分配的最大页数（每次 alloc 会在 `[1, max_pages]` 中随机选一个页数）。
  - 如果 `max_pages <= 0`，默认 `64`。
  - 会被限制到上界（例如 `1024`）。

> 注意：`max_live` 不是“最大 VMA 数”，而是**测试自身的 bookkeeping 容量**；超过它就不会再继续新 alloc，而是倾向于执行 dealloc。

## 3. 关键数据结构：bookkeeping（记录表）

为了避免测试代码本身再去动态分配内存导致递归/干扰，worker 用一个固定大小的数组记录当前活跃分配：

- `static kvm_alloc_rec_t recs[NCPU][KVM_TEST_MAX_LIVE];`
- 每条记录保存：
  - `va`：本次 `kvmalloc` 返回的虚拟地址
  - `sz`：本次分配的字节数（页对齐）

并且**按 CPU 分开记录**：`recs[cpu][...]`，避免多 CPU 并行跑测试时互相写同一份记录表。

## 4. 核心控制流（为什么不会“没 alloc 就 dealloc”）

### 4.1 每轮到底做 alloc 还是 dealloc

每次循环会先根据当前 `live`（当前活跃分配数）决定操作类型：

- `live == 0`：强制 `do_alloc = 1`
  - 这意味着一开始一定先 alloc，**不会在 live=0 时走 dealloc**。
- `live >= max_live`：强制 `do_alloc = 0`
  - 活跃分配太多就开始释放，避免记录表溢出。
- 其他情况：`do_alloc = (rnd & 1)` 随机二选一

### 4.2 dealloc 的对象来自哪里

dealloc 分支会执行：

- `idx = rng % live` 随机挑选一个已记录的活跃分配
- 从 `recs[cpu][idx]` 取出 `(va, sz)`
- 调用 `kvmdealloc(kernel_pagetable, va, sz)`

因此 dealloc 的输入一定来自“之前成功 alloc 且写入记录表”的条目。

### 4.3 为什么不会对未分配地址调用 dealloc

关键点在于：

- 只有 `kvmalloc(...) != NULL` 才会写 `recs[cpu][live]` 并 `live++`。
- 只有 `live > 0` 才可能进入 dealloc 分支。
- dealloc 之后会做 **swap-remove**：
  - `live--`
  - 用 `recs[cpu][live]` 覆盖被删除位置，保证 `[0, live)` 始终是有效活跃集合。

因此正常情况下，不存在“没有 alloc 就 dealloc”的路径。

> 如果你担心的情形是：`kvmalloc` 失败返回 `NULL`，但仍然记录/释放——代码不会这么做。失败时只会 `alloc_fail++`，不会写记录表。

## 5. 结尾 drain：是否可能释放未分配对象

循环结束后会做 drain：

- `while(live > 0) { live--; kvmdealloc(... recs[cpu][live] ...) }`

这仍然只会释放记录表中剩余的“已分配未释放”的条目。因为 `live` 的含义一直是“记录表有效条目数量”。

所以 drain 也不会释放未分配对象。

## 6. 锁与并发相关约束

worker 在入口处检查：

- 不能在已经持有 `kvm_lock` 或 `global_mm.mm_lock` 的情况下调用，否则直接 `panic`。

在循环内部：

- 每隔一段迭代（例如 `i % 257 == 0`），会：
  - `acquire(&global_mm.mm_lock)`
  - `kvm_test_assert_mm_invariants_locked(&global_mm)`
  - `release(&global_mm.mm_lock)`

并且每轮结束都会检查不应“泄漏锁”：

- 如果发现仍持有 `kvm_lock` 或 `global_mm.mm_lock`，直接 `panic("kvm_test: leaked lock")`。

这些检查帮助定位：

- `kvmalloc/kvmdealloc` 里是否有异常路径忘记释放锁
- mm 元数据结构是否在并发/边界情况下被破坏

## 7. 可能触发的失败模式（panic 点）

这个测试倾向于“尽早炸掉”，主要通过 `panic(...)` 报告问题。常见的触发点包括：

- `kvmalloc` 返回的 `va` 不是页对齐
- 返回地址不在预期的 kernel heap 区间（`in_kernel_heap` 检查失败）
- `kvm_test_touch_pages` 写后读不一致（映射/权限/别名/页表错误）
- `kvmdealloc` 返回 `(uint64)-1`（表示释放失败或输入不匹配）
- mm 的 VMA list / RB-tree 不变量不满足（顺序、重叠、计数、cache 等）
- 锁泄漏或锁顺序错误导致的不一致

## 8. `test_kvm_stress_parallel` 与 worker 的关系

`test_kvm_stress_parallel(participants, iters, max_live, max_pages)` 是一个 barrier-based 运行器：

- 需要在不同 CPU 上各调用一次，且 `participants` 必须一致。
- 到齐后一起放行（`go = 1`），并行执行 worker。
- 全部完成后重置 barrier，进入下一轮 epoch。

它用 `static volatile` 计数器 + `__sync_fetch_and_add` 实现简易 barrier。

## 9. 你问的“直接 dealloc 会不会报错？”

- **如果真的对一个从未 `kvmalloc` 的 `(va, sz)` 调用 `kvmdealloc`**：
  - 很可能返回 `(uint64)-1`（并在本测试中被当成错误 `panic`），或者触发其它一致性检查失败。
  - 这取决于 `kvmdealloc` 的实现如何判断“这段映射是否存在/是否匹配”。

- **但在 `test_kvm_stress_worker` 的当前控制流中**：
  - dealloc 的 `(va, sz)` 必定来自之前成功 `kvmalloc` 并记录下来的条目，且 `live==0` 时不会进入 dealloc 分支。
  - 所以正常情况下不会出现“没 alloc 就 dealloc”。

如果你在运行中观察到 `panic("kvm_test: kvmdealloc failed")`，更可能意味着：

- `kvmalloc/kvmdealloc` 之间的 bookkeeping 被破坏（例如并发写共享记录表——当前已按 CPU 拆分降低风险），或
- `kvmdealloc` 的输入 `sz`/边界与内部记录的 VMA/映射不一致（实现 bug），或
- mm 元数据在并发下被破坏，导致释放路径找不到对应 VMA。
