#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "kernel/stat.h"
#include "kernel/vm.h"
#include "user/user.h"

//
// wrapper so that it's OK if main() does not call exit().
//
void start() {
    extern int main();
    main();
    exit(0);
}

char *strcpy(char *s, const char *t) {
    char *os;

    os = s;
    while ((*s++ = *t++) != 0);
    return os;
}

int strncmp(const char *p, const char *q, uint n) {
    while (n > 0 && *p && *p == *q) n--, p++, q++;
    if (n == 0) return 0;
    return (uchar)*p - (uchar)*q;
}

int strcmp(const char *p, const char *q) {
    while (*p && *p == *q) p++, q++;
    return (uchar)*p - (uchar)*q;
}

uint strlen(const char *s) {
    int n;

    for (n = 0; s[n]; n++);
    return n;
}

void *memset(void *dst, int c, uint n) {
    char *cdst = (char *)dst;
    int i;
    for (i = 0; i < n; i++) {
        cdst[i] = c;
    }
    return dst;
}

// return the matched char or return 0(no match at all)
char *strchr(const char *string, char character) {
    for (; *string; string++)
        if (*string == character) return (char *)string;
    return 0;
}
// Maybe we can introduce a intermidiate buffer to achivent Vitural Buffer Reconstruction
char *gets(char *buf, int max) {
    int i, cc;
    char c;

    for (i = 0; i + 1 < max;) {
        cc = read(0, &c, 1);
        if (cc < 1) break;
        buf[i++] = c;
        if (c == '\n' || c == '\r') break;
    }
    buf[i] = '\0';
    return buf;
}

int stat(const char *n, struct stat *st) {
    int fd;
    int r;

    fd = open(n, O_RDONLY);
    if (fd < 0) return -1;
    r = fstat(fd, st);
    close(fd);
    return r;
}

// another more powerful function for atoi
long strtol(const char *nptr, char **endptr, int base) {
    if (nptr == NULL) return 0;
    // skip the leading whitespace
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    int negative = 0;
    if (nptr[0] == '-') {
        negative = 1;
        nptr++;
    }
    // handle the prefix(0x for hex)
    if (base == 16 && nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) nptr += 2;
    long acc_ret = 0;
    int val = 0;
    while (nptr[0] != '\0') {
        if (nptr[0] >= '0' && nptr[0] <= '9') val += (nptr[0] - '0');
        else if (nptr[0] >= 'a' && nptr[0] <= 'z')
            val += (10 + nptr[0] - 'a');
        else if (nptr[0] >= 'A' && nptr[0] <= 'Z')
            val += (10 + nptr[0] - 'A');
        else
            break;
        if (val >= base) break;
        acc_ret += acc_ret * base + val;
        nptr++;
    }
    if (endptr != NULL) *endptr = (char *)nptr;
    return negative ? -acc_ret : acc_ret;
}

int atoi(const char *s) {
    // int n;

    // n = 0;
    // while('0' <= *s && *s <= '9')
    //   n = n*10 + *s++ - '0';
    // return n;
    return strtol(s, NULL, 10);
}

void *memmove(void *vdst, const void *vsrc, uint n) {
    char *dst;
    const char *src;

    dst = vdst;
    src = vsrc;
    if (src > dst) {
        while (n-- > 0) *dst++ = *src++;
    } else {
        dst += n;
        src += n;
        while (n-- > 0) *--dst = *--src;
    }
    return vdst;
}

int memcmp(const void *s1, const void *s2, uint n) {
    const char *p1 = s1, *p2 = s2;
    while (n-- > 0) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

void *memcpy(void *dst, const void *src, uint n) { return memmove(dst, src, n); }

char *sbrk(int n) { return sys_sbrk(n, SBRK_EAGER); }

char *sbrklazy(int n) { return sys_sbrk(n, SBRK_LAZY); }

static unsigned find_prev_slash(char *path, unsigned dst_idx) {
    int new_dst_idx = dst_idx - 1;
    while (new_dst_idx >= 0) {
        if (path[new_dst_idx] == '/') return new_dst_idx;
        new_dst_idx--;
    }
    return (path[0] == '/') ? 1 : 0;  // the top level, return 1 for absolute path
}

void canonicalize_path(char *path) {  // Use In-place Backtracking(Two pointer)
    // explain and expand the path(Make sure the parameter is not the only!)
    if (path == NULL) return;
    unsigned src_idx = 0, dst_idx = 0;
    int component_start = 0, component_len = 0;
    int is_absolute = 0;
    if (path[0] == '/') {
        src_idx++;
        path[dst_idx++] = '/';  // skip the leading '/' protect the absolute path
        is_absolute = 1;
    }
    while (path[src_idx] != '\0') {
        while (path[src_idx] == '/') src_idx++;
        component_start = src_idx;
        while (path[src_idx] != '\0' && path[src_idx] != '/') src_idx++;
        component_len = src_idx - component_start;
        // Analyze the component
        if (component_len == 0) break;  // End of string
        else if (component_len == 1) {
            if (path[component_start] == '.') continue;
            if (dst_idx != 0 && !(is_absolute == 1 && dst_idx == 1)) path[dst_idx++] = '/';
            path[dst_idx++] = path[component_start];
        } else if (component_len == 2) {
            if (strncmp(&path[component_start], "..", 2) == 0)
                dst_idx = find_prev_slash(path, dst_idx);
            else {
                if (dst_idx != 0 && !(is_absolute == 1 && dst_idx == 1)) path[dst_idx++] = '/';
                path[dst_idx++] = path[component_start];
                path[dst_idx++] = path[component_start + 1];
            }
        } else {
            if (dst_idx != 0 && !(is_absolute == 1 && dst_idx == 1)) path[dst_idx++] = '/';
            memcpy(&path[dst_idx], &path[component_start], component_len);
            dst_idx += component_len;
        }
    }
    if (dst_idx == 0) path[dst_idx++] = '.';
    path[dst_idx] = '\0';
}

unsigned get_char_offset(const char *path, char c, int num) {
    // while type is 1 return the first, type is -1 return the last
    if (path == NULL) {
        char *error_msg = "Panic: Invliad input while get basename_offset!\n";
        write(2, error_msg, strlen(error_msg));  // Invoke syscall directly
        exit(1);
    }
    if (num == -1) {
        // find the last one, search from the end
        unsigned len = strlen(path);
        unsigned cur_idx = len;
        while (cur_idx > 0) {
            cur_idx--;
            if (path[cur_idx] == c) return cur_idx;
        }
        return (unsigned)-1;  // not found
    }
    unsigned ret = 0, cur_idx = 0;
    while (path[cur_idx] != '\0') {
        if (path[cur_idx] == c) {
            ret = cur_idx;
            if (num == 1) return ret;
            else
                num--;
        }
        cur_idx++;
    }
    return (unsigned)-1;
}