#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
int main(int argc, char *argv[]) {
    int nr_cpu=1;
    int pid_list[nr_cpu];
    for (int i = 0; i < nr_cpu; i++) {
        int pid = fork();
        if (pid==0){
            pause(1);
            exit(0);
        }
        pid_list[i]=pid;
    }
    for(int i=0;i<nr_cpu;i++){
        wait(&pid_list[i]);
    }
    exit(0);
}

// int
// main(int argc, char *argv[])
// {
//   pause(1);
//   exit(0);
// }
