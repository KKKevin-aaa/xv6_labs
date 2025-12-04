#include "kernel/types.h"
#include "user/user.h" 
unsigned int sleep(unsigned int seconds){
    //pasue seconds,return 0 while sleep normaly
    //return -1 while the process killed
    return pause(seconds);
}
static const char *usage_msg="usage: sleep NUMBER[suffix]\nsuffix can be s(seconds), m(minute), h(hours), d(days)\n";
static char end_char[]={'s', 'm', 'h', 'd', '\0'};
static void skip_unneeded_chars(char **endptr){
    while(**endptr!='\0'){
        for(int i=0;i<sizeof(end_char)/sizeof(char);i++){
            if(**endptr==end_char[i])   return;
        }
        (*endptr)++;
    }
}
int main(int argc, char *argv[]){
    if(argc<=1){
        fprintf(2, "%s", usage_msg);
        exit(1);
    }
    unsigned int sum_cnt=0;
    char *nptr=NULL, *end_char=NULL, **endptr=&end_char;
    //implicit type conversion is allowed hence
    for(int i=1;i<argc;i++){
        nptr=argv[i];
        long tmp_ret=strtol(nptr, endptr, 10);
        skip_unneeded_chars(endptr);
        switch(**endptr){
            case '\0':case 's':
                sum_cnt+=tmp_ret;break;
            case 'm':
                sum_cnt+=tmp_ret*60;break;
            case 'h':
                sum_cnt+=tmp_ret*60*60;break;
            case 'd':
                sum_cnt+=tmp_ret*60*60*24;break;
            default:    //other character,like .
                fprintf(2, "sleep: invalid time suffix '%c'\n", **endptr);
                exit(1);
        }
    }
    sleep(sum_cnt);
    exit(0);
}