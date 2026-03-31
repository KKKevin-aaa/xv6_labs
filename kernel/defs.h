//header-only-include-nothing,Opaque pointer
#ifdef LAB_MMAP
typedef unsigned long size_t;
typedef long int off_t;
#endif
struct buf;
struct context;
struct file;
struct inode;
struct pipe;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;
struct vm_dupl_ctx;
struct map_context;
struct alloc_context;
struct tlb_shootdown_req;
struct mmu_gather;
struct sighand;

typedef struct rb_node rb_node_t;
typedef struct rb_root rb_root_t;
typedef struct vm_area_struct vm_area_struct_t;
typedef struct vma_context  vma_context_t;
typedef struct mm_struct mm_struct_t;
typedef struct vm_operation_struct vm_operation_struct_t;
typedef struct mmap_context mmap_context_t;
typedef struct munmap_context munmap_context_t;

typedef struct slab_cache slab_cache_t;

typedef struct vma_pool cpu_vma_pool_t;
typedef struct mem_trans_stash mem_trans_stash_t;

typedef struct page page_t;
typedef struct slab_page slab_page_t;
typedef struct map_page map_page_t;

#ifndef offsetof
#define offsetof(TYPE, MEMBER)  ((uint64)&((TYPE *)0)->MEMBER)
#endif
#ifdef LAB_LOCK
struct rwspinlock;
#endif

// Must initlialize explicitly in kernel.ld
// Use PROVIDE that automatically define this symbol if used.
#define __init_code __attribute__((section(".init.text")))

// bio.c
__init_code void binit(void);
struct buf*     bread(uint dev, uint blockno);
void            brelse(struct buf* b);
void            bwrite(struct buf* b);
void            bpin(struct buf* b);
void            bunpin(struct buf* b);

// console.c
__init_code void consoleinit(void);
void            consoleintr(int c);
void            consputc(int c);

// exec.c
int             kexec(char* path, char** argv);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file* f);
struct file*    filedup(struct file* f);
__init_code void fileinit(void);
int             fileread(struct file* f, uint64 addr, int n);
int             filestat(struct file* f, uint64 addr);
int             filewrite(struct file* f, uint64 addr, int n);
int             fileioctl(struct file* f, int request, uint64 addr);

// fs.c
void            fsinit(int dev);
int             dirlink(struct inode* dp, char* name, uint inum);
struct inode*   dirlookup(struct inode* dp, char* name, uint* poff);
struct inode*   ialloc(uint dev, short type);
struct inode*   idup(struct inode* ip);
__init_code void iinit(void);
void            ilock(struct inode* ip);
void            iput(struct inode* ip);
void            iunlock(struct inode* ip);
void            iunlockput(struct inode* ip);
void            iupdate(struct inode* ip);
int             namecmp(const char* s, const char* t);
struct inode*   namei(char* path);
struct inode*   nameiparent(char* path, char* name);
int             readi(struct inode* ip, int user_dst, uint64 dst, uint off, uint n);
void            stati(struct inode* ip, struct stat* st);
int             writei(struct inode* ip, int user_src, uint64 src, uint off, uint n);
void            itrunc(struct inode* ip);
void            ireclaim(int dev);

// slab.c
__init_code void init_slab_system(void);
struct slab_page* slab_refill(slab_cache_t *cache);
void*           slab_alloc(slab_cache_t *cache);
int             slab_dealloc(void *node);
int             slab_free(void *obj);
uint64          slab_reclaim_empty_pages(uint64 target_pages);
slab_cache_t    *create_slab_cache_nolock(char *name, uint16 size, 
                                uint16 align, int (*ctor)(void *), int (*dtor)(void *));
slab_cache_t    *create_slab_cache(char *name, uint16 size, 
                                uint16 align, int (*ctor)(void *), int (*dtor)(void *));
uint64          get_cache_size(uint64 basic_size);
void            cal_slab_order(uint16 basic_size, uint16 *limit, uint16 *page_order);

// kalloc.c
void*           swap_in(void);
void            swap_out(void); 
page_t*         get_page_desc_safe(uint64 pfn);
page_t*         get_page_desc_assert(uint64 pfn);
void*           kalloc_page(void);
void            kfree_page(void *pa);
__init_code void kinit(void);
uint64          page2pfn(struct page * pg);
void            free_pages(void *pa, uint64 sz); //for huge page
void            reclaim_and_merge(uint64 head_pfn, uint64 init_order);
int             reclaim_orphan_pages(void *pa, uint64 size);
void*           alloc_memory(uint64 size, int flags);
uint64          get_order(uint64 pa);
uint8           is_head(uint64 pa);
uint8           is_managed_memory(uint64 pa);
void            dump_memory_map(void);
int             check_poison(void *ptr, uint64 size);
int             set_poison(void *ptr, uint64 size);
void            inc_ref_range(void *pa, uint64 size);
int             page_get_ref_wrapper(void *pa);
// inline          void sync_rmap(void *p, uint64 size, pte_t *pte);

// log.c
void            initlog(int dev, struct superblock* sb);
void            log_write(struct buf* b);
void            begin_op(void);
void            end_op(void);

// pipe.c
int             pipealloc(struct file** f0, struct file** f1);
void            pipeclose(struct pipe* pi, int writable);
int             piperead(struct pipe* pi, uint64 addr, int n);
int             pipewrite(struct pipe* pi, uint64 addr, int n);

// printf.c
int             vsnprintf(char *buf, int size, char *fmt, va_list ap);
int             snprintf(char *buf, int size, char *fmt, ...);
int             dummy_printf(char *fmt, ...);
__attribute__ ((format (printf, 1, 2))) int printf(char* fmt, ...);
__attribute__((noreturn)) void __panic(const char *file, int line, const char *func, char *s, ...);
//A Useful macro, get more necessary info without DEBUG-mode
#define panic(msg, ...)    __panic(__FILE__, __LINE__, __func__, (msg), ##__VA_ARGS__)
void            find_debug_info(uint64 pa, char *buf, uint16 buf_size);
int             safe_load_data(uint64 pa, uint64 *val, uint64 stack_start, uint64 stack_end);
__init_code void printfinit(void);
void            backtrace(void);
int             load_debug_sym_vm(void);
int             load_debug_sym(void);
void            debugsym_init(void);

// proc.c
int             cpuid(void);
void            kexit(int status);
int             kfork(void);
int             growproc(int n);
void            proc_mapstacks(pagetable_t kpgtbl);
pagetable_t     proc_pagetable(struct proc *p);
void            proc_freepagetable(struct spinlock *pt_lock, pagetable_t pagetable);
int             kkill(int pid);
int             killed(struct proc* p);
void            setkilled(struct proc* p);
struct cpu*     mycpu(void);
struct proc*    myproc(void);
__init_code void procinit(void);
__attribute__((noreturn)) void scheduler(void);
void            scheduleProcess(void);
void            sleep(void* chan, struct spinlock* lk);
__init_code void userinit(void);
int             kwait(uint64 addr);
void            wakeup(void* chan);
void            wakeup_one(void *chan);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);
struct proc *   kthread_create(const char *name, void (*func)(void));

//utils.c
__attribute__((noinline)) void nop_func(void);
__init_code void init_utils(void);

// per-cpu.c
__init_code void init_tlb_data_cache(void);
__init_code void init_timer_payload_cache(void);
void tlb_shootdown_issue(pagetable_t pgdir, uint64 va, uint64 len);
void tlb_shootdown_issue_nolock(pagetable_t pgdir, uint64 va, uint64 len);
int             timer_less_cmp(void *data, uint64 a, uint64 b);
int             timer_great_cmp(void *data, uint64 a, uint64 b);
void            timer_swap(void *data, uint64 a, uint64 b);
int             add_signal(int nr_ticks, void (*handler)(void), int nr_repeat);
int             pause_signal(void);
uint64          kreturn(void);

// swtch.S
void            swtch(struct context *store_to, struct context *load_from);

// spinlock.c
void            acquire(struct spinlock* lk);
int             holding(struct spinlock* lk);
void            initlock(struct spinlock* lk, char* name);
void            release(struct spinlock* lk);
void            push_off(void);
void            pop_off(void);
// int             atomic_read4(int *addr);
void            print_held_locks(void);
#ifdef LAB_LOCK
void            freelock(struct spinlock* lk);
void            read_acquire(struct rwspinlock* lk);
void            read_release(struct rwspinlock* lk);
void            write_acquire(struct rwspinlock* lk);
void            write_release(struct rwspinlock* lk);
void            rwspinlock_test(void);
#endif

// sleeplock.c
void            acquiresleep(struct sleeplock* lk);
void            releasesleep(struct sleeplock* lk);
int             holdingsleep(struct sleeplock* lk);
void            initsleeplock(struct sleeplock* lk, char* name);

// string.c
int             memcmp(const void* s1, const void* s2, uint n);
void*           memmove(void* dst, const void* src, uint n);
void*           memset(void* dst, int c, uint n);
char*           safestrcpy(char* s, const char* t, int n);
int             strlen(const char* s);
int             strncmp(const char* s1, const char* s2, uint n);
char*           strncpy(char* s, const char* t, int n);

// syscall.c
void            argint(int n, int* ip);
int             argstr(int n, char* buf, int max);
void            argaddr(int n, uint64 *ip);
int             fetchstr(uint64 addr, char* buf, int max);
int             fetchaddr(uint64 addr, uint64* ip);
void            syscall(void);

// trap.c
extern uint     ticks;
void            do_flush_tlb(void * args);
uint64          cal_flush_num(struct tlb_shootdown_req **buffer, int num, uint64 threshold);
void            tlb_flush_handler(struct tlb_shootdown_req **buferr, int num, int global_flush);
void            tlb_intr_handler(void);
void            software_intr_handler_nolock(void);
void            software_intr_handler_locked(void);
__init_code void trapinit(void);
__init_code void trapinithart(void);
extern struct spinlock tickslock;
void            prepare_return(void);

// uart.c
__init_code void uartinit(void);
void            uartintr(void);
void            uartwrite(char buf[], int n);
void            uartputc_sync(int c);
int             uartgetc(void);

// vm.c
__init_code void init_mmu_free_batch_cache(void);
__init_code void init_mmu_gather_cache(void);
__init_code void init_kheap_node_cache(void);
int             mappages(struct map_context *ctx1);
pagetable_t     uvmcreate(void);
void            uvmremove(pagetable_t pagetable);
uint64          uvmalloc(struct alloc_context *ctx1);
uint64          uvmdealloc(struct alloc_context *actx);
uint64          kvmalloc(pagetable_t Kpagetable, uint64 req_sz, int xperm);
uint64          kvmdealloc(struct alloc_context *actx);
int             uvmcopy_range_private(struct vm_dupl_ctx *ctx1);
int             uvmcopy_range_shared(struct vm_dupl_ctx *ctx1);
int             uvmcopy_range_direct_shared(struct vm_dupl_ctx *ctx1);
int             uvmcopy_range_cow(struct vm_dupl_ctx *ctx1);
void            vmunmap(struct map_context *ctx1);
void            uvmclear(pagetable_t pagetable, uint64 va);
pte_t *         walk(pagetable_t pagetable, uint64 va, int alloc, int target_level);
uint64          walkaddr(pagetable_t pagetable, uint64 va);
int             copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len);
int             copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int             copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);
int             ismapped(pagetable_t pagetable, uint64 va);
uint64          vmfault(res_block *rb_array, pagetable_t pagetable, uint64 va, uint64 scause);
// #if defined(LAB_PGTBL) || defined(SOL_MMAP)
void            vmprint(pagetable_t pagetable);
int             is_directory_empty(pagetable_t pagetable);

// #endif
// #ifdef LAB_PGTBL
pte_t*          pgpte(pagetable_t pagetable, uint64 va);
// #endif
void            init_res_array(res_block *rblocks, uint64 init_heap_start);
uint64          uvmalloc_thp_region(struct alloc_context *ctx1);
uint64          uvmdealloc_thp_region(struct alloc_context *ctx1);
void            freewalk(pagetable_t pagetable, int do_free, int level, struct mmu_gather *ctx2);
void            freewalk_iter(pagetable_t pagetable, int do_free, struct mmu_gather *ctx2);



//rbtree_impl.c
void            Cycle_detection(rb_root_t *root);
void            check_rbtree_integrity(rb_root_t *root);
void            rb_link_node(rb_node_t *node, rb_node_t *rb_parent, rb_node_t **rb_link);
void            rb_insert_color(rb_node_t *node, rb_root_t*root);
void            rb_erase(rb_node_t *node, rb_root_t*root);

//heap_impl.c
void            heap_sift_up(void *data, uint64 up_idx, uint64 cur_size, 
                            int (*cmp_pred)(void *, uint64, uint64), 
                            void (*swap_func)(void *, uint64, uint64));
void            heap_sift_down(void *data, uint64 down_idx, uint64 cur_size,
                            int (*cmp_pred)(void *, uint64, uint64), 
                            void (*swap_func)(void *, uint64, uint64));
void            heap_remove_idx(void *data, uint64 del_idx, uint64 cur_size,
                            int (*cmp_pred)(void *, uint64, uint64), 
                            void (*swap_func)(void *, uint64, uint64));

//mm.c
void            vma_get(vm_area_struct_t *vma);
int             vma_put(vm_area_struct_t *vma);
void            mm_get(mm_struct_t *mm);
int             mm_put(mm_struct_t *mm);
void            init_mm(void);
vm_area_struct_t *find_contain_vma(mm_struct_t *mm, uint64 vaddr);
vm_area_struct_t *find_contain_vma_and_get(mm_struct_t *mm, uint64 vaddr);
vm_area_struct_t *find_upper_vma_and_get(mm_struct_t *mm, uint64 vaddr);
mm_struct_t     *mm_create(void);
vm_area_struct_t *alloc_vma_node(void);
vm_area_struct_t *vma_dup(const vm_area_struct_t *vma);
int             insert_vma(mm_struct_t *mm, vm_area_struct_t *vma);
int             insert_vma_fast(mm_struct_t *mm, vm_area_struct_t *vma, vma_context_t *cont);
int             remove_vma(vm_area_struct_t *vma);
int             expand_vma(vm_area_struct_t *vma, uint64 new_start, uint64 new_end);
int             shrink_vma(vm_area_struct_t *vma, uint64 new_start, uint64 new_end);
int             merge_vma(vm_area_struct_t *prev_vma, vm_area_struct_t *next_vma);
uint64          get_unmapped_area(mm_struct_t *mm, uint64 len, uint64 low_limit, 
                    uint64 high_limit, vma_context_t *cont);
//Detailed implemation of rb_node,should defined and finined in here, not in rbtree.h
rb_node_t*      rb_search(rb_node_t *node, vm_area_struct_t **predecessor, 
                        vm_area_struct_t ** successor, const rb_root_t *root);
void *          do_mmap(mmap_context_t *ctx1);
int             do_munmap(munmap_context_t *ctx1);
__init_code void kvminit(void);
__init_code void kvminithart(void);

// plic.c
void            plicinit(void);
void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int irq);

// virtio_disk.c
void            virtio_disk_init(void);
void            virtio_disk_rw(struct buf *b, int write);
void            virtio_disk_intr(void);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))



#ifdef LAB_PGTBL
// vmcopyin.c
int             copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int             copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);
#endif

#ifdef LAB_LOCK
// stats.c
void            statsinit(void);
void            statsinc(void);

// sprintf.c
int             snprintf(char* buf, unsigned long size, const char* fmt, ...);
#endif

#ifdef KCSAN
void            kcsaninit(void);
#endif

#ifdef LAB_NET
// pci.c
void            pci_init(void);

// e1000.c
void            e1000_init(uint32 *x);
void            e1000_intr(void);
int             e1000_transmit(char *buf, int len);

// net.c
void            netinit(void);
void            net_rx(char *buf, int len);

#endif
