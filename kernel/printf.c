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

volatile int panicking = 0;  // printing a panic message
volatile int panicked = 0;   // spinning forever at end of a panic
static uint8 init_addrline=0;
// lock to avoid interleaving concurrent printf's.
static struct {
    struct spinlock lock;
} pr;
//Consider Cache Line.
//MAX physical address is 0x8800000,uint32_max is 2^32 is enough.
struct {    //Save Memory bandwidth(Structure of Array)
    uint32 start_addr[10000];//one single statement maps to multiple inst.
    //By placing the sequentially,we can save the memory(No record end_addr)
    uint16 file_fd[200];
    uint16 line_number[10000];
}sys_debug_info;
struct {
    char filenames[200][128];
}filename_table;

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

void init_addrline_table(){ //Python have extracted info from asm file,
    //Here we form the global array for quick and convinient search
    //Assuming the kernel.tbl have packed into file_system,where we can readi it.
    if(init_addrline){
        printf("Reinit the addrline table!\n");
        return;
    }
    //fd --> struct file*f ->ip --> struct inode*
    begin_op();
    struct inode *data_ip=namei("kernel.tbl");
    ilock(data_ip);
    //readi
    iunlock(data_ip);
    end_op();
}

void backtrace(){
    //print the current frame information according s0 and ra
    if(init_addrline==0){
        init_addrline_table();
        init_addrline=1;
    }
    printf("backtrace: \n");
    uint64 cur_func_ra=0, cur_s0=r_fp();    //Kernel mode,use p->kstack
    uint64 cur_stack_top=PGROUNDUP(cur_s0);
    cur_func_ra=*(uint64 *)(cur_s0-8);  //Use kernel_paegtable to translate address.
    while(cur_s0 < cur_stack_top && cur_s0 > (cur_stack_top-PGSIZE)){
        printf("0x%lx\n", cur_func_ra);
        cur_s0=*(uint64 *)(cur_s0-16);   //update, seek the prev frame_node 
        cur_func_ra=*(uint64 *)(cur_s0-8);  //8 bytes
    }
}

void panic(char *s) {
    panicking = 1;
    backtrace();
    printf("panic: ");
    printf("%s\n", s);
    panicked = 1;  // freeze uart output from other CPUs
    for (;;);
}

void printfinit(void) { initlock(&pr.lock, "pr"); }
