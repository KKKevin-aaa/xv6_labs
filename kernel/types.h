typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
// typedef unsigned long uint64;   //Windows is 32bits, little dangerous
typedef unsigned long long uint64;  //more safe and protability

typedef short int int16;
typedef int int32;
typedef long long int int64;

typedef uint64 pde_t;
#define NULL ((void *)0)
typedef struct{
    volatile int count;
}atomic_t;
//Complier macro, Builtin function to hint that expression has a extremely
//high probability of equaling a sepcific value, aiding in instruction reordering
//and preventing pipeline flushs.
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#include <stdarg.h>