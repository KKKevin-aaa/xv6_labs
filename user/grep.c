// Simple grep.  Only supports ^ . * $ operators.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#define BUF_SIZE 1024
static char buf[BUF_SIZE];

/**
 * process_stream: read data from the given file descriptor, cut by line,and invoke regex matcher
 * @param pattern: the regex pattern
 * @param fd: file descriptor to read from
 * @param 
 */
static void process_stream(char *pattern, int fd){
    int n_read, buffered_len=0;
    char *line_head, *end_of_line;
    while((n_read=read(fd, buf+buffered_len,BUF_SIZE-1-buffered_len))>0){
        buffered_len+=n_read;
        buf[buffered_len]='\0';
        line_head=buf;
        while((end_of_line=strchr(line_head, '\n'))!=NULL){
            *end_of_line='\0';
            if(regex_match(pattern, line_head)){
                *end_of_line='\n';
                write(1, line_head, end_of_line-line_head+1); 
                //Scan by block IO, not by bytes(More efficent)
            }
            line_head=end_of_line+1;
        }
        if(buffered_len>0){
            int remaining_len=buffered_len-(line_head-buf);
            memmove(buf, line_head, remaining_len);
            buffered_len=remaining_len;
        }
    }
}

int main(int argc, char *argv[])
{
    int fd, i;
    char *pattern;

    if (argc <= 1)
    {
        fprintf(2, "usage: grep pattern [file ...]\n");
        exit(1);
    }
    pattern = argv[1];

    if (argc <= 2)
    {
        process_stream(pattern, 0);
        exit(0);
    }

    for (i = 2; i < argc; i++)
    {
        if ((fd = open(argv[i], O_RDONLY)) < 0)
        {
            printf("grep: cannot open %s\n", argv[i]);
            exit(1);
        }
        process_stream(pattern, fd);
        close(fd);
    }
    exit(0);
}
