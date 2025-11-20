#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

void memdump(char *fmt, char *data);

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        printf("Example 1:\n");
        int a[2] = {61810, 2025};
        memdump("ii", (char *)a);

        printf("Example 2:\n");
        memdump("S", "a string");

        printf("Example 3:\n");
        char *s = "another";
        memdump("s", (char *)&s);

        struct sss
        {
            char *ptr;
            int num1;
            short num2;
            char byte;
            char bytes[8];
        } example;

        example.ptr = "hello";
        example.num1 = 1819438967;
        example.num2 = 100;
        example.byte = 'z';
        strcpy(example.bytes, "xyzzy");

        printf("Example 4:\n");
        memdump("pihcS", (char *)&example);

        printf("Example 5:\n");
        memdump("sccccc", (char *)&example);
    }
    else if (argc == 2)
    {
        // format in argv[1], up to 512 bytes of data from standard input.
        char data[512];
        int n = 0;
        memset(data, '\0', sizeof(data));
        while (n < sizeof(data))
        {
            int nn = read(0, data + n, sizeof(data) - n);
            if (nn <= 0)
                break;
            n += nn;
        }
        memdump(argv[1], data);
    }
    else
    {
        printf("Usage: memdump [format]\n");
        exit(1);
    }
    exit(0);
}

void memdump(char *fmt, char *data){    
    //Print the contents of memory pointed to by data in the format desribed by the fmt argument
    while(*fmt!='\0'){
        switch(*fmt){
            case 'i':   //print the next 4 bytes of the data, as a 32-bite integer, in dec
                fprintf(1, "%d\n", *(int *)data);
                data+=sizeof(int);
                break;
            case 'p':   //print the next 8 bytes of the data as 64-bit integer,in hex
                fprintf(1, "%lx\n", *(long *)data);
                data+=sizeof(long); //supposed 8
                break;
            case 'h'://print the next 2 bytes of the data as a 16-bit integer, in dec
                fprintf(1, "%d\n", *(short *)data);
                data+=sizeof(short);
                break;
            case 'c'://print the next 1 bytes of the data as a 8-bit ASCII character
                fprintf(1, "%c\n", *data);
                data+=sizeof(char);
                break;
            case 's'://the next 8 bytes of the data contain a 64-bit pointer to C string;print the string
                fprintf(1, "%s\n", *(char **)data);
                data+=sizeof(long *);
                break;
            case 'S':   //the rest of data contain the bytes of a null-terminated C String,print the string
                fprintf(1, "%s\n", data);
                data+=strlen(data)+1; //point to the end
                break;
            default:
                fprintf(2, "%s","Unsupported format!\n");
                exit(1);
        }
        fmt++;
    }
}
