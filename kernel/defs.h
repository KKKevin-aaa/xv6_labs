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
#define MAX(a, b) (((a) < (b)) ? (b) : (a))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define POISON_BYTE 0X5A
#define POISON_64 0x5a5a5a5a5a5a5a5aull


// bio.c
void            binit(void);
struct buf*     bread(uint dev, uint blockno);
void            brelse(struct buf* b);
void            bwrite(struct buf* b);
void            bpin(struct buf* b);
void            bunpin(struct buf* b);

// console.c
void            consoleinit(void);
void            consoleintr(int c);
void            consputc(int c);

// exec.c
int             kexec(char* path, char** argv);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file* f);
struct file*    filedup(struct file* f);
void            fileinit(void);
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
void            iinit(void);
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
void            init_slab_system(void);
struct slab_page* slab_refill(slab_cache_t *cache);
void*           slab_alloc(slab_cache_t *cache);
int             slab_dealloc(void *node);
int             slab_free(void *obj);
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
void            kinit(void);
uint64          page2pfn(struct page * pg);
void            free_pages(void *pa, uint64 sz); //for huge page
int             reclaim_orphan_pages(void *pa, uint64 size);
void*           alloc_memory(uint64 size);
uint64          get_order(uint64 pa);
uint8           is_head(uint64 pa);
uint8           is_managed_memory(uint64 pa);
void            freewalk(pagetable_t pagetable, int do_free, int level);
void            freewalk_limit(pagetable_t pagetable, int do_free, uint64 base_va, uint64 max_sz, int level);
int             is_directory_empty(pagetable_t pagetable);
void            dump_memory_map(void);
int             check_poison(void *ptr, uint64 size);
int             set_poison(void *ptr, uint64 size);
void            inc_ref_range(void *pa, uint64 size);
static inline int      get_page_ref_count(void *pa){
    if((uint64)pa%PGSIZE!=0)
        panic("get page ref_count: Lookup unaligned address!");
    if(unlikely(pa==NULL))  return -1;  //error
    return atomic_read(&get_page_desc_assert(paddr2pfn((uint64)pa))->flags.ref_count);
}
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
int             printf(char* fmt, ...) __attribute__ ((format (printf, 1, 2)));
void            __panic(const char *file, int line, const char *func, char *s, ...)  __attribute__((noreturn));
//A Useful macro, get more necessary info without DEBUG-mode
#define panic(msg, ...)    __panic(__FILE__, __LINE__, __func__, (msg), ##__VA_ARGS__)
void            find_debug_info(uint64 pa, char *buf, uint16 buf_size);
int             safe_load_data(uint64 pa, uint64 *val, uint64 stack_start, uint64 stack_end);
void            printfinit(void);
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
void            proc_freepagetable(pagetable_t pagetable);
int             kkill(int pid);
int             killed(struct proc* p);
void            setkilled(struct proc* p);
struct cpu*     mycpu(void);
struct proc*    myproc(void);
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            scheduleProcess(void);
void            sleep(void* chan, struct spinlock* lk);
void            userinit(void);
int             kwait(uint64 addr);
void            wakeup(void* chan);
void            wakeup_one(void *chan);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);
struct proc *   kthread_create(const char *name, void (*func)(void));

// swtch.S
void            swtch(struct context *store_to, struct context *load_from);

// spinlock.c
void            acquire(struct spinlock* lk);
int             holding(struct spinlock* lk);
void            initlock(struct spinlock* lk, char* name);
void            release(struct spinlock* lk);
void            push_off(void);
void            pop_off(void);
int             atomic_read4(int *addr);
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
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock tickslock;
void            prepare_return(void);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartwrite(char buf[], int n);
void            uartputc_sync(int c);
int             uartgetc(void);

// vm.c
int             mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);
pagetable_t     uvmcreate(void);
uint64          uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int perm);
uint64          uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz);
int             uvmcopy_range_private(struct vm_dupl_ctx *ctx1);
int             uvmcopy_range_noalloc(struct vm_dupl_ctx *ctx1);
void            uvmfree_range(res_block *rb_array, pagetable_t pagetable, uint64 start, uint64 end);
void            uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free);
void            uvmclear(pagetable_t pagetable, uint64 va);
pte_t *         walk(pagetable_t pagetable, uint64 va, int alloc, int target_level);
uint64          walkaddr(pagetable_t pagetable, uint64 va);
int             copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len);
int             copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int             copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);
int             ismapped(pagetable_t pagetable, uint64 va);
uint64          vmfault(res_block *rb_array, pagetable_t pagetable, uint64 va, int is_write);
// #if defined(LAB_PGTBL) || defined(SOL_MMAP)
void            vmprint(pagetable_t pagetable);
// #endif
// #ifdef LAB_PGTBL
pte_t*          pgpte(pagetable_t pagetable, uint64 va);
// #endif
void            init_res_array(res_block *rblocks, uint64 init_heap_start);
uint64          alloc_res_memory(res_block *rb_array, pagetable_t pagetable, uint64 start_va, uint64 end_va, uint64 size, int perm);
uint64          free_res_memory(res_block *rb_array, pagetable_t pagetable, uint64 start_va, uint64 end_va, uint64 size);
void            reclaim_res_memory_range(res_block *rb_array, pagetable_t pagetable, uint64 start_va, uint64 end_va);
uint64          Simp_alloc_res_memory(res_block *rb_array, pagetable_t pagetable, uint64 size, int perm);
uint64          scan_contigous_map(pagetable_t paegetable, uint64 src_va);

//rbtree_impl.c
void            Cycle_detection(rb_root_t *root);
void            check_rbtree_integrity(rb_root_t *root);
void            rb_link_node(rb_node_t *node, rb_node_t *rb_parent, rb_node_t **rb_link);
void            rb_insert_color(rb_node_t *node, rb_root_t*root);
void            rb_erase(rb_node_t *node, rb_root_t*root);

//mm.c
void            vma_get(vm_area_struct_t *vma);
int             vma_put(vm_area_struct_t *vma);
void            mm_get(mm_struct_t *mm);
int             mm_put(mm_struct_t *mm);
void            init_mm(void);
vm_area_struct_t *kernel_insert_vma_helper(mm_struct_t *mm, uint64 va, uint64 sz, int perm);
vm_area_struct_t *find_vma(mm_struct_t *mm, uint64 vaddr);
vm_area_struct_t *find_vma_and_get(mm_struct_t *mm, uint64 vaddr);
vm_area_struct_t *find_upper_vma_and_get(mm_struct_t *mm, uint64 vaddr);
mm_struct_t     *mm_create(void);
vm_area_struct_t *alloc_vma_node(void);
vm_area_struct_t *vma_dup(const vm_area_struct_t *vma);
int             insert_vma(mm_struct_t *mm, vm_area_struct_t *vma);
int             insert_vma_fast(mm_struct_t *mm, vm_area_struct_t *vma, vma_context_t *cont);
int             remove_vma(mm_struct_t *mm, vm_area_struct_t *vma);
uint64          get_unmapped_area(mm_struct_t *mm, uint64 len, uint64 low_limit, 
                    uint64 high_limit, vma_context_t *cont);
//Detailed implemation of rb_node,should defined and finined in here, not in rbtree.h
rb_node_t*      rb_search(rb_node_t *node, vm_area_struct_t **predecessor, 
                        vm_area_struct_t ** successor, const rb_root_t *root);
void *          do_mmap(mmap_context_t *ctx1);
int             do_munmap(munmap_context_t *ctx1);

//kvm.c
void            kvminit(void);
int             kvmmap_safe(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm);
void            kvminithart(void);
vm_area_struct_t *alloc_kernel_vma(void);
void*           kvmalloc(pagetable_t kpgtbl, uint64 size, int flags);
uint64          kvmdealloc(pagetable_t kpgtbl, uint64 va, uint64 size);
void            test_kvm_stress_parallel(int participants, int iters, int max_live, int max_pages);


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
