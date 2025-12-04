#ifdef LAB_MMAP
typedef unsigned long size_t;
typedef long int off_t;
#endif

#include "kernel/types.h"
#define SBRK_ERROR ((char *)-1)

struct stat;

//==============================================================================
// System Calls
//==============================================================================

/**
 * Create a new process (duplicate from current process).
 * @return 0 in child, PID in parent, -1 on error.
 */
int fork(void);

/**
 * Terminate the current process with the specified exit status.
 * @param status The exit status code to passed to the parent.
 */
int exit(int status) __attribute__((noreturn));

/**
 * Wait for a child process to exit and retrieve its status.
 * @param status Output parameter to record the exit status of the child.
 * @return The PID of the terminated child, or -1 on error.
 */
int wait(int* status);

/**
 * Create a pipe for inter-process communication.
 * @param fds Array of two integers: fds[0] for read, fds[1] for write.
 * @return 0 on success, -1 on error.
 */
int pipe(int* fds);

/**
 * Write data to a file descriptor.
 * @param fd  File descriptor to write to.
 * @param buf Buffer containing data to write.
 * @param n   Number of bytes to write.
 * @return Number of bytes actually written, or -1 on error.
 */
int write(int fd, const void* buf, int n);

/**
 * Read data from a file descriptor.
 * @param fd  File descriptor to read from.
 * @param buf Buffer to store read data.
 * @param n   Number of bytes to read.
 * @return Number of bytes actually read, 0 on EOF, -1 on error.
 */
int read(int fd, void* buf, int n);

/**
 * Close a file descriptor.
 * @param fd The file descriptor to close.
 * @return 0 on success, -1 on error.
 */
int close(int fd);

/**
 * Send a signal to terminate a specific process.
 * @param pid The process ID to terminate.
 * @return 0 on success, -1 on error.
 */
int kill(int pid);

/**
 * Load and execute a new program (replaces current process image).
 * @param path Path to the executable file.
 * @param argv Null-terminated array of argument strings.
 * @return -1 on error (does not return on success).
 */
int exec(const char* path, char** argv);

/**
 * Open or create a file.
 * @param path File path.
 * @param mode Open mode (e.g., O_RDONLY, O_WRONLY, O_CREATE).
 * @return File descriptor (fd) on success, -1 on error.
 */
int open(const char* path, int mode);

/**
 * Create a special device file (node).
 * @param path  Path to create.
 * @param major Major device number (driver index).
 * @param minor Minor device number (specific device).
 * @return 0 on success, -1 on error.
 */
int mknod(const char* path, short major, short minor);

/**
 * Remove a directory entry (delete file if ref count becomes 0).
 * @param path Path to remove.
 * @return 0 on success, -1 on error.
 */
int unlink(const char* path);

/**
 * Get file status information (size, type, etc.).
 * @param fd Open file descriptor.
 * @param st Pointer to stat structure to fill.
 * @return 0 on success, -1 on error.
 */
int fstat(int fd, struct stat* st);

/**
 * Create a hard link (new name for an existing file).
 * @param oldpath Path to existing file.
 * @param newpath Path for the new link.
 * @return 0 on success, -1 on error.
 */
int link(const char* oldpath, const char* newpath);

/**
 * Create a directory.
 * @param path Path of the directory to create.
 * @return 0 on success, -1 on error.
 */
int mkdir(const char* path);

/**
 * Change current working directory.
 * @param path Path to the new directory.
 * @return 0 on success, -1 on error.
 */
int chdir(const char* path);

/**
 * Duplicate a file descriptor (points to the same struct file).
 * @param fd Existing file descriptor.
 * @return New file descriptor (lowest available), or -1 on error.
 */
int dup(int fd);

/**
 * Get the current process ID.
 * @return The process ID (PID).
 */
int getpid(void);

/**
 * Adjust the process heap size (raw system call).
 * @param n Number of bytes to increment (negative for decrement).
 * @return Pointer to the previous program break, or -1 on error.
 */
char* sys_sbrk(int n, int reserved);

/**
 * Pause execution for a specified duration.
 * @param ticks Number of clock ticks to sleep.
 * @return 0 on success.
 */
int pause(int ticks);

/**
 * Get the time since system startup.
 * @return Number of clock ticks.
 */
int uptime(void);


/**
 * some description for ioctl
 */
int ioctl(int fd, int req, uint64 arg);

//==============================================================================
// ulib.c (User Library)
//==============================================================================

#ifdef LAB_LOCK
int statistics(void*, int);
#endif
#ifdef LAB_NET
int bind(uint16);
int unbind(uint16);
int send(uint16, uint32, uint16, char *, uint32);
int recv(uint16, uint32*, uint16*, char *, uint32);
#endif
#ifdef LAB_PGTBL
int ugetpid(void);
int u_uptime(void);
uint64 pgpte(void*);
void kpgtbl(void);
#endif


/**
 * Retrieve information about a file.
 * @param path File path.
 * @param st   Pointer to the struct where status info is stored.
 * @return 0 on success, -1 on error.
 */
int stat(const char* path, struct stat* st);

/**
 * Copy the source string to the destination buffer.
 * @param dst Pointer to the destination buffer.
 * @param src Pointer to the source string.
 * @return Pointer to 'dst'.
 */
char* strcpy(char* dst, const char* src);

/**
 * Copy memory block, handling overlapping areas safely.
 * @param dst Destination address.
 * @param src Source address.
 * @param n   Number of bytes to copy.
 * @return Pointer to 'dst'.
 */
void* memmove(void* dst, const void* src, uint n);

/**
 * Find the first occurrence of a character in a string.
 * @param s The string to search.
 * @param c The character to find.
 * @return Pointer to the first occurrence of 'c', or NULL if not found.
 */
char* strchr(const char* s, char c);

/**
 * Compare the first n characters of two strings.
 * @param p First string.
 * @param q Second string.
 * @param n Max number of characters to compare.
 * @return <0 if p<q, 0 if p==q, >0 if p>q.
 */
int strncmp(const char* p, const char* q, uint n);

/**
 * Compare two strings.
 * @param p First string.
 * @param q Second string.
 * @return <0 if p<q, 0 if p==q, >0 if p>q.
 */
int strcmp(const char* p, const char* q);

/**
 * Read a line from standard input into the buffer.
 * @param buf Destination buffer.
 * @param max Maximum size of the buffer.
 * @return Pointer to 'buf', or NULL on error/EOF.
 */
char* gets(char* buf, int max);

/**
 * Calculate the length of a string (excluding the null terminator).
 * @param s Input string.
 * @return Number of characters in the string.
 */
uint strlen(const char* s);

/**
 * Set the first n bytes of the memory area to a specific value.
 * @param dst Pointer to the memory area.
 * @param c   Value to set (converted to unsigned char).
 * @param n   Number of bytes.
 * @return Pointer to 'dst'.
 */
void* memset(void* dst, int c, uint n);

/**
 * Convert a string to a long integer.
 * @param nptr   String to convert.
 * @param endptr If not NULL, stores pointer to the character after last digit.
 * @param base   Base of the number system (e.g., 10, 16).
 * @return The converted long value.
 */
long strtol(const char* nptr, char** endptr, int base);

/**
 * Convert a string to an integer.
 * @param s String to convert.
 * @return The converted integer value.
 */
int atoi(const char* s);

/**
 * Compare the contents of two memory blocks.
 * @param v1 First memory block.
 * @param v2 Second memory block.
 * @param n  Number of bytes to compare.
 * @return <0 if v1<v2, 0 if v1==v2, >0 if v1>v2.
 */
int memcmp(const void* v1, const void* v2, uint n);

/**
 * Copy memory block (does not handle overlapping areas).
 * @param dst Destination address.
 * @param src Source address.
 * @param n   Number of bytes to copy.
 * @return Pointer to 'dst'.
 */
void* memcpy(void* dst, const void* src, uint n);

/**
 * Adjust the data segment size of the process (Traditional heap allocation).
 * @param increment Bytes to increment (positive) or decrement (negative).
 * @return Previous program break address, or -1 on error.
 */
char* sbrk(int increment);

/**
 * Lazily adjust data segment size.
 * Optimization: physical pages are allocated only upon access.
 * @param increment Bytes to increment.
 * @return Previous program break address.
 */
char* sbrklazy(int increment);

/**
 * Canonicalize a path (resolving symbols like ".." and ".").
 * @param path The path buffer to be canonicalized in-place.
 */
void canonicalize_path(char* path);

/**
 * Get the offset of the 'num'-th occurrence of a character in a path.
 * @param path File path string.
 * @param c    Target character.
 * @param num  The occurrence index (1-based).
 * @return Offset index, or specific error code if not found.
 */
unsigned get_char_offset(const char* path, char c, int num);

/**
 * @brief Intercepts and restricts a specific system call for the calling process.
 * 
 * This function installs a hook to prevent the kernel fomr executing
 * the specified system call. It essentially "sandbox" the current process by
 * disabling specific functionality.
 * @param syscall_num the unqiue identifier of the system call to block
 * @param keep_params Pointer to specific arguments or a configuration structure.
 * if not NULL, only syscalls matching these paramters might be allowed
 * @return 0 on success; a negative error code on failure
 */
int interpose(int syscall_num, char *keep_params);

//==============================================================================
// printf.c
//==============================================================================

/**
 * Print formatted output to a file descriptor.
 * @param fd  File descriptor (e.g., 1 for stdout, 2 for stderr).
 * @param fmt Format control string.
 * @param ... Variable arguments.
 */
void fprintf(int fd, const char* fmt, ...) __attribute__ ((format (printf, 2, 3)));

/**
 * Print formatted output to standard output.
 * @param fmt Format control string.
 * @param ... Variable arguments.
 */
void printf(const char* fmt, ...) __attribute__ ((format (printf, 1, 2)));

//==============================================================================
// umalloc.c
//==============================================================================

/**
 * Allocate dynamic memory.
 * @param nbytes Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL if failed.
 */
void* malloc(uint nbytes);

/**
 * Free dynamic memory.
 * @param ap Pointer to memory previously allocated by malloc.
 */
void free(void* ap);

//==============================================================================
// regexp.c
//==============================================================================

/**
 * Check if text matches the regex pattern.
 * @param pattern Regular expression pattern.
 * @param text    Text to search.
 * @return 1 on match, 0 on no match.
 */
int regex_match(char *pattern, char *text);