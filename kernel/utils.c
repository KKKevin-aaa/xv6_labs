#include "param.h"
#include "types.h"
#include "atomic.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "per-cpu.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "colors.h"
#include "utils.h"

extern struct spinlock rbtree_debug_lock;
extern struct spinlock rcu_writer_lock;
extern struct mailbox req_mailbox_locked[NCPU];

//Use a single-byte repetitive pattern to verify and set poison.
int check_poison(void *ptr, uint64 size){
    //return 0 while all poison(clean), return -1 means dirty
    if(ptr==NULL)   return -1;
    if(size==0)   return 0;
    if((uint64)ptr & 0x7){ //prelogue, skip to 64-bits aligned address.
        uint8 *ptr8=(uint8 *)ptr;
        do{
            if(*ptr8!=POISON_BYTE)  return -1;
            ptr8++;
            if(--size==0) return 0;
        }while(((uint64)ptr8 & 0x7));
        ptr=(void *)ptr8;
    }
    uint64 *ptr64=(uint64 *)ptr;
    while(size>=8){
        size-=8;
        if(ptr64[0]!=POISON_64) return -1;
        ptr64++;
    }
    if(size>0){ //Epilogue
        uint8 *ptr8=(uint8 *)ptr64;
        do{
            if(*ptr8!=POISON_BYTE)  return -1;
            ptr8++;
            if(--size==0)   return 0;
        }while(1);
    }
    return 0;
}

int set_poison(void *ptr, uint64 size){
    if(ptr==NULL || size==0)   return -1;
    //for SIMD optimization and Code simplicity,use memset directly
    memset(ptr, POISON_BYTE, size);
    return 0;
}

int is_all_same_swar(const uint8 *data, uint64 len){    //SIMD Within A Register
    if(len==0)  return 1;
    uint8 ref=data[0];
    uint64 pattern=ref;
    pattern |= pattern<<8;
    pattern |= pattern<<16;
    pattern |= pattern<<32;
    const uint8 *ptr=data;
    while(((uint64)ptr & 0x7)!=0 && len >0){    //deal with prelog
        if(*(uint8 *)ptr!=ref)  return 0;
        len--;ptr++;
    }
    const uint64 *ptr64=(uint64 *)ptr;
    while(len>=8){
        if(*ptr64!=pattern) 
            return 0;
        ptr64++;
        len-=8;
    }
    ptr=(uint8 *)ptr64; //deal with epilog
    while(len >0){
        if(*ptr!=ref)
            return 0;
        len--;ptr++;
    }
    return 1;
}

__attribute__((noinline)) void nop_func(void) {}

__init_code void init_utils(void){
    //initialize relevant lock
    initlock(&rbtree_debug_lock, "rbtree_debug_lock");
    initlock(&rcu_writer_lock, "rcu_writer_lock");
    char tmp_name[40];
    for(int i=0;i<NCPU;i++){
        snprintf(tmp_name, 40, "req_mailbox_locked lock(NO.%d)", i);
        initlock(&req_mailbox_locked->lock, tmp_name);
    }
}