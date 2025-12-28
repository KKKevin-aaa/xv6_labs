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
typedef struct page_slab_header page_slab_header_t;
typedef struct vma_pool cpu_vma_pool_t;
#ifndef offsetof
#define offsetof(TYPE, MEMBER)  ((uint64)&((TYPE *)0)->MEMBER)
#endif
#ifdef LAB_LOCK
struct rwspinlock;
#endif
#define MAX(a, b) (((a) < (b)) ? (b) : (a))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

// console.c
void            consoleinit(void);
void            consoleintr(int);
void            consputc(int);

// exec.c
int             kexec(char*, char**);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);
int             fileioctl(struct file*, int, uint64);

// fs.c
void            fsinit(int);
int             dirlink(struct inode*, char*, uint);
struct inode*   dirlookup(struct inode*, char*, uint*);
struct inode*   ialloc(uint, short);
struct inode*   idup(struct inode*);
void            iinit();
void            ilock(struct inode*);
void            iput(struct inode*);
void            iunlock(struct inode*);
void            iunlockput(struct inode*);
void            iupdate(struct inode*);
int             namecmp(const char*, const char*);
struct inode*   namei(char*);
struct inode*   nameiparent(char*, char*);
int             readi(struct inode*, int, uint64, uint, uint);
void            stati(struct inode*, struct stat*);
int             writei(struct inode*, int, uint64, uint, uint);
void            itrunc(struct inode*);
void            ireclaim(int);

// kalloc.c
void*           kalloc(void);
void            kfree(void *);
void            kinit(void);
void            free_pages(void *pa, uint64 sz); //for huge page
void*           alloc_memory(uint64);
uint16          get_order(uint64 pa);
uint8           is_head(uint64 pa);
uint8           is_managed_memory(uint64 pa);
void freewalk(pagetable_t pagetable, int do_free, uint64 base_va, uint64 max_sz, int level);
int             is_pagetable_empty(pagetable_t pagetable);
void            dump_memory_map();
// log.c
void            initlog(int, struct superblock*);
void            log_write(struct buf*);
void            begin_op(void);
void            end_op(void);

// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

// printf.c
int             printf(char*, ...) __attribute__ ((format (printf, 1, 2)));
void            panic(char*) __attribute__((noreturn));
void            printfinit(void);
void            backtrace(void);

// proc.c
int             cpuid(void);
void            kexit(int);
int             kfork(void);
int             growproc(int);
void            proc_mapstacks(pagetable_t);
pagetable_t     proc_pagetable(struct proc *);
void            proc_freepagetable(res_block * , pagetable_t, uint64);
int             kkill(int);
int             killed(struct proc*);
void            setkilled(struct proc*);
struct cpu*     mycpu(void);
struct proc*    myproc();
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            scheduleProcess(void);
void            sleep(void*, struct spinlock*);
void            userinit(void);
int             kwait(uint64);
void            wakeup(void*);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);

// swtch.S
void            swtch(struct context*, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            push_off(void);
void            pop_off(void);
int             atomic_read4(int *addr);
#ifdef LAB_LOCK
void            freelock(struct spinlock*);
void            read_acquire(struct rwspinlock*);
void            read_release(struct rwspinlock*);
void            write_acquire(struct rwspinlock*);
void            write_release(struct rwspinlock*);
void            rwspinlock_test();
#endif

// sleeplock.c
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);

// string.c
int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
char*           strncpy(char*, const char*, int);

// syscall.c
void            argint(int, int*);
int             argstr(int, char*, int);
void            argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
int             fetchaddr(uint64 addr, uint64* ip);
void            syscall();

// trap.c
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock tickslock;
void            prepare_return(void);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartwrite(char [], int);
void            uartputc_sync(int);
int             uartgetc(void);

// vm.c
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
uint64          uvmalloc(pagetable_t, uint64, uint64, int);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(res_block *, pagetable_t, pagetable_t, uint64);
void            uvmfree(res_block *, pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
pte_t *         walk(pagetable_t, uint64, int, int);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
int             ismapped(pagetable_t, uint64);
uint64          vmfault(res_block *, pagetable_t, uint64, int);
// #if defined(LAB_PGTBL) || defined(SOL_MMAP)
void            vmprint(pagetable_t);
// #endif
// #ifdef LAB_PGTBL
pte_t*          pgpte(pagetable_t, uint64);
// #endif
void            init_res_array(res_block *, uint64 init_heap_start);
uint64          alloc_res_memory(res_block *, pagetable_t, uint64, uint64, uint64, int);
uint64          free_res_memory(res_block *, pagetable_t, uint64, uint64, uint64);
void reclaim_res_memory_range(res_block *rb_array, pagetable_t pagetable, uint64 start_va, uint64 end_va);
uint64          Simp_alloc_res_memory(res_block *, pagetable_t, uint64, int);

//rbtree_impl.c
void rb_link_node(rb_node_t *node, rb_node_t *rb_parent, rb_node_t **rb_link);
void rb_insert_color(rb_node_t *node, rb_root_t*root);
void rb_erase(rb_node_t *node, rb_root_t*root);


//kvm.c
void            kvminit(void);
void            kvmmap(pagetable_t, uint64, uint64, uint64, int);
void            kvminithart(void);
vm_area_struct_t *alloc_vma_node(void);
int             reclaim_vma_node(vm_area_struct_t *);
vm_area_struct_t *find_vma(mm_struct_t *mm, uint64 vaddr);
int             insert_vma(mm_struct_t *mm, vm_area_struct_t *vma);
int             remove_vma(mm_struct_t *mm, vm_area_struct_t *vma);
//Detailed implemation of rb_node,should defined and finined in here, not in rbtree.h
rb_node_t*      rb_search(rb_node_t *node, vm_area_struct_t **predecessor, 
                        vm_area_struct_t ** successor, const rb_root_t *root);
void*           kvmalloc(pagetable_t, uint64, int);
uint64          kvmdealloc(pagetable_t, uint64, uint64);

// plic.c
void            plicinit(void);
void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int);

// virtio_disk.c
void            virtio_disk_init(void);
void            virtio_disk_rw(struct buf *, int);
void            virtio_disk_intr(void);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))



#ifdef LAB_PGTBL
// vmcopyin.c
int             copyin_new(pagetable_t, char *, uint64, uint64);
int             copyinstr_new(pagetable_t, char *, uint64, uint64);
#endif

#ifdef LAB_LOCK
// stats.c
void            statsinit(void);
void            statsinc(void);

// sprintf.c
int             snprintf(char*, unsigned long, const char*, ...);
#endif

#ifdef KCSAN
void            kcsaninit();
#endif

#ifdef LAB_NET
// pci.c
void            pci_init();

// e1000.c
void            e1000_init(uint32 *);
void            e1000_intr(void);
int             e1000_transmit(char *, int);

// net.c
void            netinit(void);
void            net_rx(char *buf, int len);

#endif
