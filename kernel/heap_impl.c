#include "types.h"
#include "param.h"
#include "atomic.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "per-cpu.h"
#include "slab.h"
#include "colors.h"
#include "utils.h"

/* * Array-based heap implementation:
 * - Min-heap: cmp_pred(a, b) returns 1 if $f(a) < f(b)$, otherwise 0.
 * - Max-heap: cmp_pred(a, b) returns 1 if $f(a) > f(b)$, otherwise 0.
 * The comparison predicate is caller-defined and passed as an argument.
 */
static int pre_check(void *data, uint64 idx, uint64 cur_size, 
                    int (*cmp_pred)(void *, uint64, uint64),
                    void (*swap_func)(void *, uint64, uint64)){
    if(!data || !swap_func || !cmp_pred || !(cur_size && idx<=cur_size-1)){
        pr_warn("Incomplete or invalid argument"
            "(Maybe array is full and need expand.).");
        //Required expand, but clearly misplaced.
        return -1;
    }
    return 0;   //No error.
}


void heap_sift_up(void *data, uint64 up_idx, uint64 cur_size, 
                    int (*cmp_pred)(void *, uint64, uint64), 
                    void (*swap_func)(void *, uint64, uint64)){
    if(pre_check(data, up_idx, cur_size, cmp_pred, swap_func)==-1){
        pr_err("error.");
        return;
    }
    if(up_idx==0)   return;
    uint64 p_idx=(up_idx-1)/2;
    int cmp_ret=0;
    while(up_idx > 0){
        cmp_ret=cmp_pred(data, up_idx, p_idx);
        if(cmp_ret==-1)     panic("Unexpected error, exist NULL pointer in heap.");
        if(cmp_ret!=1)  break;
        swap_func(data, up_idx, p_idx);
        
        up_idx=p_idx;
        p_idx=(up_idx-1)/2;
    }
}

void heap_sift_down(void *data, uint64 down_idx, uint64 cur_size, 
                    int (*cmp_pred)(void *, uint64, uint64),
                    void (*swap_func)(void *, uint64, uint64)){
    // assuming have already resumed the previous root, 
    // and replace the root with the last element.
    if(pre_check(data, down_idx, cur_size, cmp_pred, swap_func)==-1){
        pr_err("error.");
        return;
    }
    if(down_idx==cur_size-1)     return;
    uint64 min_idx=down_idx;
    int cmp_ret;
    while(2*down_idx+1 < cur_size){
        if(2*down_idx + 2 < cur_size){
            cmp_ret=cmp_pred(data, min_idx, 2*down_idx+2);
            if(cmp_ret==-1)    panic("Unexpected error, exist NULL pointer in heap.");
            min_idx=(cmp_ret==0)?(2*down_idx + 2):min_idx;
        }
        cmp_ret=cmp_pred(data, min_idx, 2*down_idx+1);
        if(cmp_ret==-1)        panic("Unexpected error, exist NULL pointer in heap.");
        min_idx=(cmp_ret==0)?(2*down_idx + 1): min_idx;
        if(min_idx==down_idx)    return;
        swap_func(data, down_idx, min_idx);
        down_idx=min_idx;
    }
}

void heap_remove_idx(void *data, uint64 del_idx, uint64 cur_size, 
                    int (*cmp_pred)(void *, uint64, uint64),
                    void (*swap_func)(void *, uint64, uint64)){
    if(pre_check(data, del_idx, cur_size, cmp_pred, swap_func)==-1){
        pr_err("error.");
        return;
    }
    if(del_idx==cur_size-1)      return;
    swap_func(data, del_idx, cur_size-1);
    if(del_idx==0){
        heap_sift_down(data, del_idx, cur_size-1, cmp_pred, swap_func);
        return;
    }
    uint64 up_idx=del_idx, p_idx=(up_idx-1)/2;       //Init p_idx must be valid.
    //Attempt to sift_up, and if successfully move, return.
    int cmp_ret=0;
    while(up_idx > 0){
        cmp_ret=cmp_pred(data, up_idx, p_idx);
        if(cmp_ret==-1)     panic("Unexpected error, exist NULL pointer in heap.");
        if(cmp_ret!=1)  break;
        swap_func(data, up_idx, p_idx);
        up_idx=p_idx;
        p_idx=(up_idx-1)/2;
    }
    if(up_idx==del_idx)
        heap_sift_down(data, del_idx, cur_size-1, cmp_pred, swap_func);
}