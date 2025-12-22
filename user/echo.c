#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static unsigned greedy_match_octal(char *buf, int *idx, unsigned max_len){
    unsigned ret=0;
    if(buf==NULL)   return 0;
    for(int i=0;i<max_len;i++){
        if(buf[*idx]>='0' && buf[*idx]<='7')    ret=ret*8+(buf[*idx]-'0');
        else    return ret;
        *idx+=1;
    }
    return ret;
}
static unsigned greedy_match_hex(char *buf, int *idx, unsigned max_len){
    unsigned ret=0;
    if(buf==NULL)   return 0;
    for(int i=0;i<max_len;i++){
        if(buf[*idx]>='0' && buf[*idx]<'9')   ret=ret*16+(buf[*idx]-'0');
        else if(buf[*idx]>='a' && buf[*idx]<='f')   ret=ret*16+(buf[*idx]-'a'+10);
        else if(buf[*idx]>='A' && buf[*idx]<='F')   ret=ret*16+(buf[*idx]-'A'+10);
        else    return ret;
        *idx+=1;
    }
    return ret;
}
int main(int argc, char *argv[])
{
    //Try to introduce predicator to support more option
    //default behavior is -E
    unsigned option_idx=1, inner_idx=1;
    int newline_flag=1; //default append \n
    int escape_flag=0;   //default: do not parse "\"
    while(option_idx<argc){
        if(argv[option_idx]==NULL)  break;
        if(argv[option_idx][0]!='-' \
            || (strlen(argv[option_idx])==2 && strncmp(argv[option_idx], "--", 2))==0)  break;
        inner_idx=1;
        while(inner_idx<strlen(argv[option_idx])){
            switch(argv[option_idx][1]){
                case 'n':
                    newline_flag=0;
                    break;
                case 'e':
                    escape_flag=1;
                    break;
                case 'E':
                    escape_flag=0;
                    break;
                default:
                    fprintf(2, "Not supported short-option in echo!\n");
                    exit(1);
                    break;
            }
            inner_idx++;
        }
        option_idx++;
    }
    for (int i = option_idx; i < argc; i++)
    {
        if(escape_flag==0)  write(1, argv[i], strlen(argv[i]));
        else{
            int idx=0;char convert_char=0;
            while(argv[i][idx]!='\0'){
                if(argv[i][idx]=='\\'){
                    idx++;
                    switch(argv[i][idx]){
                        case '\0':break;
                        case '\\':write(1, "\\", 1);break;
                        case 'a':write(1, "\a", 1);break;
                        case 'b':write(1, "\b", 1);break;
                        case 'c':exit(0);   //Produce no further output
                        case 'e':write(1, "\e", 1);break;
                        case 'f':write(1, "\f", 1);break;
                        case 'n':write(1, "\n", 1);break;
                        case 'r':write(1, "\r", 1);break;
                        case 't':write(1, "\t", 1);break;
                        case 'v':write(1, "\v", 1);break;
                        case '0':
                            idx++;
                            convert_char=greedy_match_octal(argv[i], &idx, 3);
                            write(1, &convert_char, 1);
                            continue;   //Continue statement must within a loop(ignore switch)
                        case 'x':
                            idx++;
                            convert_char=greedy_match_hex(argv[i], &idx, 2);
                            write(1, &convert_char, 1);
                            continue;
                        default:
                            fprintf(2, "Invalid escape char \"\\%c\"", argv[i][idx]);
                            exit(1);
                            break;
                    }
                }
                else    write(1, &argv[i][idx], 1);
                idx++;
            }
        }
        if (i + 1 < argc)   write(1, " ", 1);
        else if(newline_flag==1)    write(1, "\n", 1);
    }
    exit(0);
}
