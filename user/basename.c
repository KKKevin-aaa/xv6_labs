#include "user/user.h"

static const char *msg = "Usage: find <path> <name>\n";
int main(int argc, char *argv[]){
    if(argc<=1){
        fprintf(2, "%s", msg);
        exit(1);
    } 
    else{
        for(int i=1;i<=argc-1;i++){
            canonicalize_path(argv[i]);
            unsigned start_idx=get_char_offset(argv[i], '/', -1);
            if(start_idx==0)    start_idx=(unsigned)-1;
            unsigned end_idx=get_char_offset(argv[i], '.', -1);
            if(end_idx!=(unsigned)-1)   argv[i][end_idx]='\0';
            fprintf(1, "%s\n", &argv[i][start_idx+1]);
        }
    }
    exit(0);
}