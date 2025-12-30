//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void test_vma_slab_allocator();
void test_vma_rbtree_and_list();
void test_kvmalloc_integrity();
void test_unmapped_area_search();
volatile int panicking = 0;  // printing a panic message
volatile int panicked = 0;   // spinning forever at end of a panic
static uint8 is_debug_sym_loaded=0;
extern uint64 next_vmalloc_addr;
// lock to avoid interleaving concurrent printf's.
static struct {
    struct spinlock lock;
} pr;
//Consider Cache Line.
//MAX physical address is 0x8800000,uint32_max is 2^32 is enough.
struct {    //Save Memory bandwidth(Structure of Array)
    uint32 *start_addr;//one single statement maps to multiple inst.
    //By placing the sequentially,we can save the memory(No record end_addr)
    uint16 *file_id;
    uint16 *line_number;
    uint64 entry_cnt;
}kernel_addr_map;
struct {
    char *filename_map;
    uint64 name_cnt;
    uint64 filename_stride;
}src_file_table;
struct debug_header_t{
    uint32 magic;
    uint32 max_len;
    uint32 name_cnt;
    uint32 addr_cnt;
    uint32 fileid_cnt;
    uint32 line_cnt;
};
static char digits[] = "0123456789abcdef";

static void printint(long long xx, int base, int sign) {
    char buf[20];
    int i;
    unsigned long long x;

    if (sign && (sign = (xx < 0))) x = -xx;
    else
        x = xx;

    i = 0;
    do {
        buf[i++] = digits[x % base];
    } while ((x /= base) != 0);

    if (sign) buf[i++] = '-';

    while (--i >= 0) consputc(buf[i]);
}

static void printptr(uint64 x) {
    int i;
    consputc('0');
    consputc('x');
    for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console.
int printf(char *fmt, ...) {
    va_list ap;
    int i, cx, c0, c1, c2;
    char *s;

    if (panicking == 0) acquire(&pr.lock);

    va_start(ap, fmt);
    for (i = 0; (cx = fmt[i] & 0xff) != 0; i++) {
        if (cx != '%') {
            consputc(cx);
            continue;
        }
        i++;
        c0 = fmt[i + 0] & 0xff;
        c1 = c2 = 0;
        if (c0) c1 = fmt[i + 1] & 0xff;
        if (c1) c2 = fmt[i + 2] & 0xff;
        if (c0 == 'd') {
            printint(va_arg(ap, int), 10, 1);
        } else if (c0 == 'l' && c1 == 'd') {
            printint(va_arg(ap, uint64), 10, 1);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'd') {
            printint(va_arg(ap, uint64), 10, 1);
            i += 2;
        } else if (c0 == 'u') {
            printint(va_arg(ap, uint32), 10, 0);
        } else if (c0 == 'l' && c1 == 'u') {
            printint(va_arg(ap, uint64), 10, 0);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'u') {
            printint(va_arg(ap, uint64), 10, 0);
            i += 2;
        } else if (c0 == 'x') {
            printint(va_arg(ap, uint32), 16, 0);
        } else if (c0 == 'l' && c1 == 'x') {
            printint(va_arg(ap, uint64), 16, 0);
            i += 1;
        } else if (c0 == 'l' && c1 == 'l' && c2 == 'x') {
            printint(va_arg(ap, uint64), 16, 0);
            i += 2;
        } else if (c0 == 'p') {
            printptr(va_arg(ap, uint64));
        } else if (c0 == 'c') {
            consputc(va_arg(ap, uint));
        } else if (c0 == 's') {
            if ((s = va_arg(ap, char *)) == 0) s = "(null)";
            for (; *s; s++) consputc(*s);
        } else if (c0 == '%') {
            consputc('%');
        } else if (c0 == 0) {
            break;
        } else {
            // Print unknown % sequence to draw attention.
            consputc('%');
            consputc(c0);
        }
    }
    va_end(ap);

    if (panicking == 0) release(&pr.lock);

    return 0;
}
//Another method: vmalloc(virtual Contiguous Mapping)
//Allocates multiple non-contiguous physical pages and modifies the
//kernel page tables to map them into a contiguous virtual address range.
int load_debug_sym_vm(){
    if(is_debug_sym_loaded){
        printf("Reinit the addrline table!\n");
        return -1;
    }
    //fd --> struct file*f ->ip --> struct inode*
    struct inode *data_ip;
    if((data_ip=namei("kernel.tbl"))==0){
        printf("Kernel: kernel.tbl not found!\n");  //Ensure kernerl.tbl in filesystem
        return -1;
    }
    ilock(data_ip); //Load data and Guarantee only one process can manipulate it at the same time.
    //readi
    struct debug_header_t header;
    int ret=-1; //Set fail default
    //The data layout is Magic, Signlefilename_max, name_cnt, 
    //                   addr_cnt, fileid_cnt, line_cnt.
    // int n=0;    //Indicate the actual read data len.
    uint32 cur_offset=0;
    if(readi(data_ip, 0, (uint64)&header, 0, sizeof(header))!=sizeof(header)){
        printf("Kernel: kernel.tbl cannot read header data correctly!\n");
        goto cleanup;
    }
    cur_offset+=sizeof(header);

    if(header.addr_cnt!=header.fileid_cnt || header.fileid_cnt!=header.line_cnt){
        printf("Kernel :kernel.tbl contain inconsisent data!\n");
        goto cleanup;
    }
    //Permanent Allocation, persists until system shutdown.
    uint64 str_len=header.max_len*header.name_cnt;
    uint64 alloc_str_len=PGROUNDUP(str_len);
    void *mem0=NULL;
    mem0=kvmalloc((pagetable_t)r_satp(), alloc_str_len, PTE_R | PTE_W);
    if(mem0==NULL){
        printf("Kernel: OOM!\n");
        goto cleanup;
    }
    memset(mem0, 0, alloc_str_len);
    if(readi(data_ip, 0, (uint64)mem0, cur_offset, str_len)!=str_len){
        kvmdealloc((pagetable_t)r_satp(), (uint64)mem0, alloc_str_len);
        printf("Kernel: read wrong data!\n");
        goto cleanup;
    }
    cur_offset+=str_len;

    uint64 data_len=sizeof(uint16)*header.addr_cnt*2 + sizeof(uint32)*header.addr_cnt;
    uint64 alloc_data_len=PGROUNDUP(data_len);
    void *mem1=kvmalloc((pagetable_t)r_satp(), alloc_data_len, PTE_R | PTE_W);
    if(mem1==NULL){
        kvmdealloc((pagetable_t)r_satp(), (uint64)mem0, alloc_str_len);
        printf("Kernel: OOM!\n");
        goto cleanup;
    }
    memset(mem1, 0, alloc_data_len);
    if(readi(data_ip, 0, (uint64)mem1, cur_offset, data_len)!=data_len){
        kvmdealloc((pagetable_t)r_satp(), (uint64)mem0, alloc_str_len);
        kvmdealloc((pagetable_t)r_satp(), (uint64)mem1, alloc_data_len);
        printf("Kernel: read wrong data!\n");
        goto cleanup;
    }
    //Synchronize the metadata
    printf("now the allocated data is 0x%llx, and str_len is 0x%llx\n", alloc_data_len, alloc_str_len);
    src_file_table.filename_map=(char *)mem0;
    src_file_table.name_cnt=header.name_cnt;
    src_file_table.filename_stride=header.max_len;
    kernel_addr_map.entry_cnt=header.addr_cnt;
    kernel_addr_map.start_addr=(uint32 *)mem1;
    kernel_addr_map.file_id=(uint16 *)(kernel_addr_map.start_addr + kernel_addr_map.entry_cnt);
    kernel_addr_map.line_number=(uint16 *)(kernel_addr_map.file_id+kernel_addr_map.entry_cnt);
    is_debug_sym_loaded=1;
    ret=0;
cleanup:
    iunlock(data_ip);
    return ret;
}
// void find_debug_info_vm(uint64 pa){
//     find_debug_info(pa);
// }
//Another method: The Segmented Indexing Approach
//Manages a collection of discrete physical pages using a software directory
//Using the helper function to calculate the correct page index.
// int load_debug_sym_si(){

// }
// void find_debug_info_si(uint64){

// }

//The Eariest way, align the required page to 4KB^n.(Buddy system)
int load_debug_sym(){
    /* * Rationale:
     * This function loads the 'kernel.tbl' generated by offline scripts.
     * It parses the binary format and populates the global debug structures.
     */
    if(is_debug_sym_loaded){
        printf("Reinit the addrline table!\n");
        return -1;
    }
    //fd --> struct file*f ->ip --> struct inode*
    struct inode *data_ip;
    if((data_ip=namei("kernel.tbl"))==0){
        printf("Kernel: kernel.tbl not found!\n");  //Ensure kernerl.tbl in filesystem
        return -1;
    }
    ilock(data_ip); //Load data and Guarantee only one process can manipulate it at the same time.
    //readi
    struct debug_header_t header;
    int ret=-1; //Set fail default
    //The data layout is Magic, Signlefilename_max, name_cnt, 
    //                   addr_cnt, fileid_cnt, line_cnt.
    // int n=0;    //Indicate the actual read data len.
    uint32 cur_offset=0;
    if(readi(data_ip, 0, (uint64)&header, 0, sizeof(header))!=sizeof(header)){
        printf("Kernel: kernel.tbl cannot read header data correctly!\n");
        goto cleanup;
    }
    cur_offset+=sizeof(header);

    if(header.addr_cnt!=header.fileid_cnt || header.fileid_cnt!=header.line_cnt){
        printf("Kernel :kernel.tbl contain inconsisent data!\n");
        goto cleanup;
    }
    //Permanent Allocation, persists until system shutdown.
    uint64 str_len=header.max_len*header.name_cnt;
    uint64 alloc_str_len=PGROUNDUP(str_len);
    if(alloc_str_len & (alloc_str_len-1)){ //Consider Buddy-system
        uint32 msb=i_log2(alloc_str_len);
        alloc_str_len=1ull<<(msb+1);    //Align up
    }
    void *mem0=NULL;
    mem0=(char *)alloc_memory(alloc_str_len);
    if(mem0==NULL){
        printf("Kernel: OOM!\n");
        goto cleanup;
    }
    memset(mem0, 0, alloc_str_len);
    if(readi(data_ip, 0, (uint64)mem0, cur_offset, str_len)!=str_len){
        free_pages(mem0, alloc_str_len);
        printf("Kernel: read wrong data!\n");
        goto cleanup;
    }
    cur_offset+=str_len;

    uint64 data_len=sizeof(uint16)*header.addr_cnt*2 + sizeof(uint32)*header.addr_cnt;
    uint64 alloc_data_len=PGROUNDUP(data_len);
    if(alloc_data_len & (alloc_data_len-1)){ //Considering Buddy-system
        uint32 msb=i_log2(alloc_data_len);
        alloc_data_len=1ull<<(msb+1);
    }
    void *mem1=alloc_memory(alloc_data_len);
    if(mem1==NULL){
        free_pages(mem0, alloc_str_len);
        printf("Kernel: OOM!\n");
        goto cleanup;
    }
    memset(mem1, 0, alloc_data_len);
    if(readi(data_ip, 0, (uint64)mem1, cur_offset, data_len)!=data_len){
        free_pages(mem0, alloc_str_len);
        free_pages(mem1, alloc_data_len);
        printf("Kernel: read wrong data!\n");
        goto cleanup;
    }
    //Synchronize the metadata
    printf("now the allocated data is 0x%llx, and str_len is 0x%llx\n", alloc_data_len, alloc_str_len);
    src_file_table.filename_map=(char *)mem0;
    src_file_table.name_cnt=header.name_cnt;
    src_file_table.filename_stride=header.max_len;
    kernel_addr_map.entry_cnt=header.addr_cnt;
    kernel_addr_map.start_addr=(uint32 *)mem1;
    kernel_addr_map.file_id=(uint16 *)(kernel_addr_map.start_addr + kernel_addr_map.entry_cnt);
    kernel_addr_map.line_number=(uint16 *)(kernel_addr_map.file_id+kernel_addr_map.entry_cnt);
    is_debug_sym_loaded=1;
    ret=0;
cleanup:
    iunlock(data_ip);
    return ret;
}
uint64 sys_load_debug_sym(void){
    test_vma_slab_allocator();
    test_vma_rbtree_and_list();
    test_kvmalloc_integrity();
    test_unmapped_area_search();
    return load_debug_sym_vm();
}

void find_debug_info(uint64 pa){
    if(pa<KERNBASE || pa>=PHYSTOP){
        //Can be virtual address
        // printf("Invalid physical address!\n");
        printf("Unable convert into kernel address:0x%llx\n", pa);
        return;
    }
    if(kernel_addr_map.entry_cnt==0){
        printf("Empty table!\n");
        return;
    }
    //Sorted(Increasing order), Binary search
    uint64 left=0, right=kernel_addr_map.entry_cnt;
    uint64 mid;
    while(left<right){
        mid=(left+right)/2; //Must valid
        if(pa<kernel_addr_map.start_addr[mid])
            right=mid;
        else if(pa>kernel_addr_map.start_addr[mid])
            left=mid+1;
        else{
            uint16 filename_id=kernel_addr_map.file_id[mid];
            uint64 start_idx=filename_id*src_file_table.filename_stride;
            //Ensure each string is NULL-terminated.
            printf("%s:%u(0x%llx)\n", &src_file_table.filename_map[start_idx], 
                (unsigned int)kernel_addr_map.line_number[mid], pa);
            return;
        }
    }   //End with 'left==right'
    uint64 ret_idx=(left==0)?0:(left-1);
    if((ret_idx<kernel_addr_map.entry_cnt-1 && 
        pa >=kernel_addr_map.start_addr[ret_idx] && pa<kernel_addr_map.start_addr[ret_idx+1]) ||
        (ret_idx==kernel_addr_map.entry_cnt-1 && pa>=kernel_addr_map.start_addr[ret_idx])){
        uint64 filename_id=kernel_addr_map.file_id[ret_idx];
        uint64 start_idx=filename_id*src_file_table.filename_stride;
        //Ensure each string is NULL-terminated.
        printf("%s:%u(0x%llx)\n", &src_file_table.filename_map[start_idx], 
            (unsigned int)kernel_addr_map.line_number[ret_idx], pa);
    }
    else    printf("Cannot find 0x%llx\n", pa);
}

//Helper function for safe data access.Return 1 while success, return 0 while fail
//Use kernel_paegtable to translate address.
int safe_load_data(uint64 pa, uint64 *val, uint64 stack_start, uint64 stack_end){
    if(pa% sizeof(uint64) !=0)    return 0;
    if(pa >=stack_start && pa<stack_end){
        *val=*(uint64 *)pa;
        return 1;
    }
    return 0;
}

void backtrace(){
    //print the current frame information according s0 and ra
    if(is_debug_sym_loaded==0){
        printf("Init process have not init symbol_table!\n");
        return;
    }
    printf("backtrace: ");
    uint64 cur_func_ra=0, cur_s0=r_fp();    //Kernel mode,use p->kstack
    uint64 stack_end=PGROUNDUP(cur_s0), stack_start=stack_end-PGSIZE;
    // Kernel stack is mapped into the top of virtual address space
    printf("(Kernel stack range from 0x%llx to 0x%llx)\n", stack_start, stack_end);
    while(1){
        if(cur_s0==0){
            printf("[Reach the stack bottom!]\n");
            return;
        }
        if(safe_load_data(cur_s0-8, &cur_func_ra, stack_start, stack_end)!=1){
            if(cur_func_ra >=TRAMPOLINE)
                printf("[Trap Entry](Trampoline Page)\n");
            else    printf("[Corrupted Stack frame](0x%llx)\n", cur_s0-8);
            return;
        }
        find_debug_info(cur_func_ra);
        uint64 next_s0; //rewind to previous frame 
        if(safe_load_data(cur_s0-16, &next_s0, stack_start, stack_end)!=1){
            break;  //end
        }
        cur_s0=next_s0;
    }
}

void panic(char *s) {
    //Enter panic, system are in highly unstable state,
    //no interrupts, no locks, no allocations!Lazy Loading are strictly forbidden
    panicking = 1;
    backtrace();
    printf("panic: ");
    printf("%s\n", s);
    panicked = 1;  // freeze uart output from other CPUs
    for (;;);
}

void printfinit(void) { initlock(&pr.lock, "pr"); }
