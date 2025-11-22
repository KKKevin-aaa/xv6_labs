#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"

void Given_path_find(char *path, char *name){
    // int fd;
    // struct dirent cur_de;
    // struct stat cur_st;
    // if((fd=open(path, O_RDONLY))<0){
    //     fprintf(2, "find: cannot open %s\n", path);
    //     return;
    // }
    // if(fstat(fd, &cur_st)<0){
    //     fprintf(2, "find: cannot stat %s\n", path);
    //     return;
    // }
    // switch(cur_st.type){
    //     case T_DEVICE:case T_FILE:
    //         //extract to get basename, then compare
            
    //         if(strcmp(path, name)==0){
    //             fprintf(1, "")
    //         }
    // }
}
int main(int agrc, char *argv[]){
    exit(0);
}