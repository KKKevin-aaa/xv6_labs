#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/param.h"
//A simple version:that only support find dirpath filepath(must be basename)
//Since we don't support the predicates, so the only invalid input must be one dirpath and one name
#define MAX_BUFFER_SIZE 128
static char new_path[MAXARG]={'\0'};
static char new_arg_area[MAX_BUFFER_SIZE]={0};  //Use the flat memory to store the argv and its value
static char *new_argv[MAXARG]={NULL};
static unsigned new_argc =0;    //record the result(for the potenital exec)
static unsigned cur_idx=0;
static unsigned need_record=0;
void Given_path_find(char *path, char *name){
    int fd;
    struct dirent cur_de;
    struct stat cur_st;
    char tmp_buf[512], *ptr;

    if((fd=open(path, O_RDONLY))<0){
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }
    if(fstat(fd, &cur_st)<0){
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }
    unsigned start_idx=0;
    switch(cur_st.type){
        case T_DIR:
            if(strlen(path)+1+DIRSIZ>sizeof(tmp_buf)){
                printf("find:path too long\n");
                break;
            }
            strcpy(tmp_buf, path);
            ptr=tmp_buf+strlen(path);
            *ptr++='/';
            while(read(fd, &cur_de, sizeof(struct dirent))==sizeof(struct dirent)){
                if(cur_de.inum==0)  continue;
                if(strcmp(cur_de.name, "..")==0 || strcmp(cur_de.name, ".")==0) continue;
                memmove(ptr, cur_de.name, DIRSIZ);
                ptr[DIRSIZ]='\0';   //fixed length
                Given_path_find(tmp_buf, name);
            }
            break;
        case T_DEVICE:case T_FILE:
            //extract to get basename, then compare
            start_idx=get_char_offset(path, '/', -1);
            if(regex_match(path+(start_idx+1), name)==1){
                printf("regex match successfully in given_path_find\n");
                //Keep searching
                if(need_record==0){
                    fprintf(1, "%s\n", path);
                    break;
                }
                unsigned added_len=strlen(path)+1;
                if(added_len+cur_idx<MAX_BUFFER_SIZE && new_argc < MAXARG){
                    memmove(&new_arg_area[cur_idx], path, added_len);
                    new_argv[new_argc++]=&new_arg_area[cur_idx];
                    cur_idx+=added_len;
                }
                else{
                    fprintf(2, "In find: out of predefined memory!\n");
                    exit(1);
                }
            }
            break;
        default:exit(1);break;
    }
    close(fd);
}

int main(int argc, char *argv[]){
    if(argc<3){
        fprintf(2,"Find :invalid input,please check again!\n");
        exit(1);
    }
    int old_arg_idx=0;
    //Before find, first check if need exec another command(-exec),and adjust our memory arrangement.
    while(old_arg_idx<argc){
        if(strcmp(argv[old_arg_idx], "-exec")==0){
            need_record=1;break;
        }
        old_arg_idx++;
    }
    if(need_record==1){
        old_arg_idx++;  //skip the -exec
        unsigned added_len=strlen(argv[old_arg_idx])+1;
        memmove(new_path, argv[old_arg_idx], added_len);  
        //One copy for kernel to locate the executeable binary(to call open() to get the fd/inode)
        //And one copy for argv[0] in exec(identity:make program know which path it is loaded from)
        while(old_arg_idx<argc){
            if(cur_idx+added_len<MAX_BUFFER_SIZE && argc<MAXARG){
                // printf("going to move %s to new argv\n", argv[old_arg_idx]);
                memmove(&new_arg_area[cur_idx], argv[old_arg_idx], added_len);
                new_argv[new_argc++]=&new_arg_area[cur_idx];
                cur_idx+=added_len;
            }
            else{
                fprintf(2, "In find: out of predefined memory!\n");
                exit(1);
            }
            old_arg_idx++;
            added_len=strlen(argv[old_arg_idx])+1;
        }
    }
    char *path=argv[1];
    canonicalize_path(path);
    Given_path_find(path, argv[2]);
    if(need_record==1){
        int pid;
        pid = fork();
        if (pid == -1){
            fprintf(2, "Error: in find while trying to fork a new process!\n");
            exit(1);
        }   //one call, two returns
        if(pid==0)  exec(new_path, new_argv);   //Must be child process
        else    wait(NULL); //Parent wait for the child process
    }
    exit(0);
}