#include "user/user.h"

static const char *msg = "Usage: dirname <path>[<path>]\n";
int main(int argc, char *argv[]){
    if(argc<=1){
        fprintf(2, "%s", msg);
        exit(1);
    } 
    else{
        for(int i=1;i<=argc-1;i++){
            canonicalize_path(argv[i]);
            unsigned offset=get_char_offset(argv[i], '/', -1);
            fprintf(1, "%s\n", &argv[i][offset+1]);
        }
    }
    exit(0);
}