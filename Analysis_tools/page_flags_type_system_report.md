
# xv6-labs-2025：统一 `struct page.flags` 的页类型体系（带少量可参考代码）

你想做的事可以非常具体地概括为一句话：

> 让每个物理页都有一个“明确的用途类型（page kind）”，并且所有代码在读写 `struct page` 的 union 分支前都先检查这个类型。

这样 slab/buddy/VM 看到同一页时，不会各自“脑补”它是什么，从而减少野指针、双重释放、链表乱串、以及最难调的 silent corruption。

本文目标：
- 不是讲理论；而是给你一套**能直接落地**的 flags 位域/union/锁顺序/迁移步骤。
- 代码只给“模板级片段”，让你能照着写；不试图替你完成实现。

---

## 1) 你现在的 flags 为什么很难扩展（先把坑说清楚）

你当前 `kernel/kalloc.c` 的 `flags` 语义是“强耦合位拼图”：
- bit63：head/tail
- bit0：free/alloc
- 中间大段：head 时表示 order，tail 时表示 offset

这导致一个现实问题：

> 你不能随便再挑几位当“页用途类型”，因为你在很多地方会用掩码清/写 data 位段，容易把新位段误清掉。

所以，本方案的关键不是“加个 enum”，而是：

1) 明确划出一个 **page_kind 位段**，并保证所有 flags 写路径都不会动它。
2) union 的哪个分支有效，由 page_kind 唯一决定。

---

## 2) 最小可行的 page_kind 位域规划（能和你现有 flags 共存）

下面给你一个**具体且可审计**的位域方案（你可以调整 bit 位置，但思路别变）：

- 保留 bit63/head-tail、bit0/free 不动。
- 选一个“目前没用到、也不在 P_DATA_MASK 覆盖范围内”的小位段作为 kind。

示例：把 kind 放在 bit 60..62（三位，最多 8 种类型）。

```c
// 仅示例：你要确认这些位不会被你的 P_DATA_MASK 覆盖。
#define PGK_SHIFT 60
#define PGK_MASK  (0x7ULL << PGK_SHIFT)

enum page_kind {
	PGK_UNKNOWN = 0,
	PGK_FREE    = 1,
	PGK_SLAB    = 2,
	PGK_PGTBL   = 3,
	PGK_ANON    = 4,
	PGK_FILE    = 5,
	PGK_OTHER   = 6,
};
```

注意：你必须做一件事（否则必炸）：

> **修改/封装所有写 flags 的地方**，保证它们在重写 order/offset 时不会把 `PGK_MASK` 清掉。

最实用的做法是：写一个“保留 kind 位段”的 flags 写入口（下面给模板）。

---

## 3) union 复用：让 `struct page` 不变大、还能装 slab 元数据

你担心“把 slab 字段塞进 struct page 会变大”。union 的意义就是：

- buddy free 页只需要 next/prev
- slab 页只需要 freelist/cache/inuse/objects
- 普通页只需要 rmapping/index

同一页不可能同时满足这三种用途，所以可以 union 复用。

模板：

```c
struct slab_page {
	void *freelist;
	void *cache;
	uint16 inuse;
	uint16 objects;
	int32 prev_pfn;
	int32 next_pfn;
};

struct page {
	uint64 flags;
	union {
		struct { struct page *next, *prev; } buddy;
		struct slab_page slab;
		struct { void *rmapping; uint64 index; } map;
	} u;
};
```

说明：
- 我用 `prev_pfn/next_pfn`（int32）是为了让 slab 链表节点更紧凑；你也可以存指针，但 PFN 更容易断言合法范围。
- slab 链表头也建议用 PFN（`-1` 表示空）。

---

## 4) “我到底改哪些地方？”——从最小落地顺序开始

你现在最需要的是一条能走通的路，不是一次性大爆改。

### 阶段 A：先把 page_kind 做出来（先不碰 slab 结构）
1) 选定 `PGK_MASK` 位段。
2) 封装 `set_page_kind/get_page_kind`。
3) 在 **伙伴分配成功** 的路径上给页设置 `PGK_OTHER`（或更具体类型）。
4) 在 **伙伴回收成功** 的路径上给页设置 `PGK_FREE`。

模板（关键点：保留其他位）：

```c
static inline uint64 pgk_get(uint64 flags){
	return (flags & PGK_MASK) >> PGK_SHIFT;
}

static inline void pgk_set(struct page *p, enum page_kind k){
	p->flags = (p->flags & ~PGK_MASK) | (((uint64)k << PGK_SHIFT) & PGK_MASK);
}
```

### 阶段 B：把 slab 页标成 PGK_SLAB，并把 slab 元数据放进 union
1) slab refill：通过 `alloc_memory(PGSIZE)` 拿到页后，找到对应 `struct page*`，设置 kind=SLAB。
2) 初始化 `p->u.slab.{cache,freelist,inuse,objects}`。
3) slab shrink：准备把页还给 buddy 前，先把 kind 改回 OTHER（或做清理），再 `free_pages`。

这一步你最担心的是：怎么从 `pa` 找到 `struct page*`。

你现在已经有 `PA2PAGE_*` 这类宏/函数（见 `kalloc.c` 的 `paddr_to_pfn` / `get_page_desc_*`），所以 slab 里只需要一个小入口：

- `page_from_pa(pa)` 返回 `struct page*`（内部可用现有函数），由 kalloc 提供。

这不是“泄漏”，因为它是内核内部 API；关键是：

> 只允许 slab 模块调用它；其他模块仍通过 alloc/free 管页。

---

## 5) 链表怎么串？没有页内 header 也完全没问题

你现在的 slab 用 `page_slab_header_t.prev_page/next_page` 串 `partial/full/empty`。

换成 union 后：
- 链表节点字段变成 `p->u.slab.prev_pfn/next_pfn`
- cache 的链表头存 PFN（例如 `cache->partial_head_pfn`）

你原来的 `relink_locked()` 思想仍然成立，只是“节点”从 `page_slab_header_t*` 变成 `pfn` 或 `struct page*`。

最关键的纪律是：

> 任何访问 `p->u.slab` 的地方，必须先断言 `pgk_get(p->flags)==PGK_SLAB`。

---

## 6) 锁顺序：避免你最容易踩的 ABBA

你现在有两把关键锁：
- `kmem.lock`：伙伴系统
- `cache->pool_lock`：slab cache

最稳妥的规则（强烈建议你照做）：

> **不要在持有 `cache->pool_lock` 的情况下调用 `alloc_memory/free_pages`。**

否则未来任何地方出现 `kmem.lock`→再拿 `cache->pool_lock`，就 ABBA。

refill/shrink 的标准写法（伪流程）：

```c
// refill：
lock(cache)
if (need_page) { unlock(cache); pa=alloc_memory(PGSIZE); lock(cache); recheck; }
attach_page_to_cache_lists(pa)
unlock(cache)

// shrink：
lock(cache)
if (found_empty_page) { detach; unlock(cache); free_pages(pa,PGSIZE); lock(cache); }
unlock(cache)
```

你会觉得“要 recheck 很烦”，但这是正确性换来的必需复杂度。

---

## 7) 你如何“下手”验证自己没写错（非常实用）

把下面三条当成你实现过程中的硬约束：

1) **page-kind 不变量**
	 - FREE 状态的页：只能出现在 buddy free list。
	 - SLAB 状态的页：只能出现在某个 cache 的链表里，且 `u.slab.cache` 必须非空。

2) **union 分支不变量**
	 - 读写 `u.slab` 前必须断言 kind==SLAB。
	 - 读写 `u.buddy` 前必须断言 kind==FREE。

3) **锁不变量**
	 - `alloc_memory/free_pages` 只能在不持 `cache->pool_lock` 时调用（或你写一个统一锁顺序并全系统遵守）。

只要你把这三条落实成断言/检查，你会发现调试成本会骤降。

---

## 8) 你现在的工程里，最“重”的改动点在哪里？

我不回避：这个方案改动大，重的地方主要是两处：

1) **buddy flags 写路径要审计**：确保不会误清 `PGK_MASK`。
2) **slab 的页链表节点迁移**：从页内 header 的 prev/next 迁移到 `struct page` 的 union 分支。

但好处是：
- 以后你再做 side-table/去掉页内 header/做 debug hardening，都更顺。

---

## 9) 下一步我建议你做什么（最小可跑通路线）

如果你只想先跑通、别一次改死：

1) 先做 **page-kind 位段 + pgk_set/pgk_get**，在 buddy 分配/回收处打上 FREE/OTHER。
2) slab refill 只做一件事：拿到页后把 kind 标成 SLAB，并在 shrink 前恢复回 OTHER（暂时不做 union 链表迁移）。
3) 等 kind 跑稳、断言不报，再迁移 slab 链表节点到 union。

---

如果你愿意，我可以继续做一份“flags 写路径审计清单”（仍然只写报告+少量模板），把你 `kalloc.c` 里每个会写 `p->flags` 的点都列出来，标注：哪些必须保留 `PGK_MASK`，哪些必须/不应当改动。

