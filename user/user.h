#include "kernel/types.h"
#define SBRK_ERROR ((char *)-1)

struct stat;

// system calls
//Create a new process(duplicate from current process)
//Returns: 0 in valid, PID in parent, -1 on error(Can tell parent from child)
int fork(void);

//Terminate the current process with the specified exit status.
//@status:the exit status code to passed to the parent.
int exit(int) __attribute__((noreturn));

// status is output parameter,record the exit status of the child process.Return the pid of the terminated child process.
// And then parent will recycle the process resources.
int wait(int* status);

// Create a pipei(for inter-process communcations)
// @fds:Array of two integers, fds[0] for read, fds[1] for write
// Returns: 0 on success, -1 on error
int pipe(int* fds);

// Write data to a file descriptor
// @fd:file descriptor to write to
// @buf:buffer contianing data to write
// @n :number of bytes actually written, -1 on error
int write(int fd, const void* buf, int n);

// Read data from a file descriptor
// @fd:file descriptor to read from
// @buf:buffer contianing data to write
// @n :number of bytes actually read, 0 on EOF, -1 on error
int read(int fd, void* buf, int n);

// Close a file descriptor
// return: 0 for success, -1 on error
int close(int);

// Send a signal to terminate a specific process
// @pid: the process id to terminate
// return 0 on success, -1 on error
int kill(int pid);

int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sys_sbrk(int,int);
int pause(int);
int uptime(void);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strncmp(const char *p, const char *q, uint n);
int strcmp(const char*, const char*);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
long strtol(const char *nptr, char **endptr, int base);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char* sbrk(int);
char* sbrklazy(int);
void canonicalize_path(char *path);
unsigned get_char_offset(const char *path, char c, int num);
// printf.c
void fprintf(int, const char*, ...) __attribute__ ((format (printf, 2, 3)));
void printf(const char*, ...) __attribute__ ((format (printf, 1, 2)));
// umalloc.c
void* malloc(uint);
void free(void*);
