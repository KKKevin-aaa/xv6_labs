#include "user/user.h"
#include "kernel/types.h"
#include "kernel/fcntl.h"
static const char *usage_msg="usage: sixfive file\n";
static const char seperators[]="\t\n,/ -\r."; //Init with String Literal 
//Some helper function
int main(int argc, char *argv[]){
    if(argc<=1){
        fprintf(2, "%s", usage_msg);
        exit(1);
    }
    int success_quit=1, sum=0, read_ret;
    char c;
    for(int i=1;i<argc;i++){
        int fd=open(argv[i], O_RDONLY);
        if(fd<0){
            fprintf(2, "%s: open failed\n", argv[i]);
            success_quit=0;
            break;
        }
        while((read_ret=read(fd, &c, 1))==1){  //Only support decimal input
            if(c>='0' && c<='9')    sum=sum*10+(c-'0');
            else if(strchr(seperators, c)!=0){
                if(sum%5==0 || sum%6==0)    fprintf(1, "%d\n", sum);
                sum=0;  //reset
            }
            //other invalid character, just skip
        }
        close(fd);
        //deal with the last umcomplete string
        if(read_ret==0){
            if(sum!=0 && (sum%5==0 || sum%6==0))    fprintf(1, "%d\n", sum);
        }
        else{
            success_quit=0;
            break;
        }
    }
    if(success_quit==1) exit(0);
    else    exit(1);
}