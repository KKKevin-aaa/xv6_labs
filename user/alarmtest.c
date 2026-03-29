//
// test program for the alarm lab.
// you can modify this file for testing,
// but please make sure your kernel
// modifications pass the original
// versions of these tests.
//

#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"
#include "kernel/fcntl.h"
void test0();
void test1();
void test2();
void test3();
void periodic();
void slow_handler();
void dummy_handler();

void special_write(int fd, char * str, int num){
    if(num > 16){
        //Treat as a general case.
        write(fd, str, num);
        return;
    }
    char fixed_new_str[24]={0};
    memcpy(fixed_new_str + 8, str, num);
    cpuwrite(fd, fixed_new_str, num+8);
    //leave front 8 bytes to add CPU info.
}


int main(int argc, char *argv[]) {
    int num_workers=24, pid=0;
    printf("Starting concurrent stress test "
        "with %d workers...\n", num_workers);
    for (int i = 0; i < num_workers; i++) {
        pid=fork();
        if(pid<0){
            printf("fork failed.\n");
            exit(1);
        }
        if(pid==0){
            close(1);
            close(2);
            char logname[16]="test_logX.txt";
            logname[8]='0' +i;
            open(logname, O_CREATE | O_WRONLY);
            dup(1);
            test0();
            test1();
            test2();
            test3();
            exit(0);
        }
    }
    for(int i=0;i<num_workers;i++){
        wait(0);
    }
    printf("all concurent tests finished.\n");
    exit(0);
}

volatile static int count;

#define A0_CHECKVAL 1337
#define S11_CHECKVAL 6181
register int s11_var asm("s11");

void periodic() {
    s11_var = ~S11_CHECKVAL;
    count = count + 1;
    printf("alarm!\n");
    sigreturn();
    printf("oops, sigreturn returned!\n");
    exit(1);
}

// tests whether the kernel calls
// the alarm handler even a single time.
void test0() {
    int i;
    printf("test0 start\n");
    count = 0;
    sigalarm(2, periodic);
    for (i = 0; i < 1000 * 500000; i++) {
        if ((i % 1000000) == 0) special_write(2, ".", 1);
        if (count > 0) break;
    }
    printf("should end signal output.\n");
    sigalarm(0, 0);
    if (count > 0) {
        printf("test0 passed\n");
    } else {
        printf("\ntest0 failed: the kernel never called the alarm handler\n");
    }
}

int __attribute__((noinline)) foo(int i, int *j) {
    if ((i % 2500000) == 0) {
        special_write(2, ".", 1);
    }
    *j += 1;
    return A0_CHECKVAL;
}

//
// tests that the kernel calls the handler multiple times.
//
// tests that, when the handler returns, it returns to
// the point in the program where the timer interrupt
// occurred, with all registers holding the same values they
// held when the interrupt occurred.
//
void test1() {
    int i;
    int j;

    printf("test1 start\n");
    s11_var = S11_CHECKVAL;
    count = 0;
    j = 0;
    sigalarm(2, periodic);
    for (i = 0; i < 500000000; i++) {
        if (count >= 10) break;
        if (foo(i, &j) != A0_CHECKVAL) {
            printf("\ntest1 failed: a0 not preserved\n");
            exit(1);
        }
        if (s11_var != S11_CHECKVAL) {
            printf("\ntest1 failed: register s11 not preserved\n");
            exit(1);
        }
    }
    sigalarm(0, 0);
    if (count < 10) {
        printf("\ntest1 failed: too few calls to the handler\n");
        printf("currnet count is %d and current i is %d\n", count, i);
    } else if (i != j) {
        // the loop should have called foo() i times, and foo() should
        // have incremented j once per call, so j should equal i.
        // once possible source of errors is that the handler may
        // return somewhere other than where the timer interrupt
        // occurred; another is that that registers may not be
        // restored correctly, causing i or j or the address ofj
        // to get an incorrect value.
        printf("\ntest1 failed: foo() executed fewer times than it was called\n");
    } else {
        printf("test1 passed\n");
    }
}

//
// tests that kernel does not allow reentrant alarm calls.
void test2() {
    int i;
    int pid;
    int status;

    printf("test2 start\n");
    if ((pid = fork()) < 0) {
        printf("test2: fork failed\n");
    }
    if (pid == 0) {
        count = 0;
        sigalarm(2, slow_handler);
        for (i = 0; i < 1000 * 500000; i++) {
            if ((i % 1000000) == 0) special_write(2, ".", 1);
            if (count > 0) break;
        }
        if (count == 0) {
            printf("\ntest2 failed: alarm not called\n");
            exit(1);
        }
        exit(0);
    }
    wait(&status);
    sigalarm(0, 0);
    if (status == 0) {
        printf("test2 passed\n");
    }
}

void slow_handler() {
    count++;
    printf("alarm!\n");
    if (count > 1) {
        printf("test2 failed: alarm handler called more than once\n");
        exit(1);
    }
    for (int i = 0; i < 1000 * 500000; i++) {
        asm volatile("nop");  // avoid compiler optimizing away loop
    }
    sigalarm(0, 0);
    sigreturn();
}

//
// dummy alarm handler; after running immediately uninstall
// itself and finish signal handling
void dummy_handler() {
    sigalarm(0, 0);
    sigreturn();
}

//
// tests that the return from sys_sigreturn() does not
// modify the a0 register
void test3() {
    uint64 a0;

    sigalarm(2, dummy_handler);
    printf("test3 start\n");

    asm volatile("lui a5, 0");
    asm volatile("addi a0, a5, 0xac" : : : "a0");
    for (int i = 0; i < 500000000; i++);
    asm volatile("mv %0, a0" : "=r"(a0));
    sigalarm(0, 0);
    if (a0 != 0xac) printf("test3 failed: register a0 changed\n");
    else
        printf("test3 passed\n");
}
