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
            unsigned offset=get_char_offset(argv[i], '/', -1);
            argv[i][offset]='\0';
            fprintf(1, "%s\n", argv[i]);
        }
    }
    exit(0);
}