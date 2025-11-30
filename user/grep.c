// Simple grep.  Only supports ^ . * $ operators.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[1024];

/**
 * process_stream: read data from the given file descriptor, cut by line,and invoke regex matcher
 * @parma pattern: the regex pattern
 * @parma fd: file descriptor to read from
 */
void process_stream(char *pattern, int fd){
    int bytes_read, data_length=0;
    char *line_start, *newline_ptr;
    while((bytes_read=read(fd, buf+data_length, sizeof(buf)-1-data_length))>0){
        data_length+=bytes_read;
        buf[data_length]='\0';
        line_start=buf;
        while((newline_ptr=strchr(line_start, '\n'))!=NULL){
            *newline_ptr='\0';
            if(regex_match(pattern, line_start)){
                *newline_ptr='\n';
                write(fd, buf, newline_ptr-line_start+1);
                //Scan by block IO, not by bytes(More efficent)
            }
            line_start=newline_ptr+1;
        }
        if(data_length>0){
            int remaining_len=data_length-(line_start-buf);
            memmove(buf, line_start, remaining_len);
            data_length=remaining_len;
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
