#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define DATASIZE (30 * 4096)
//Since 'exec' reuses initial memory for shell setup, 
// we must leave a margin toreach the target data.
char data[DATASIZE];

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: secret the-secret\n");
        exit(1);
    }
    strcpy(data, "This may help.");

    strcpy(data + 16, argv[1]);
    // printf("now the data address is %p\n", data);
    exit(0);
}
