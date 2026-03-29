//All atomic operations.
//Encapsulating it as a structure eliminates the possibility of
// direct numeric manipulation.
static inline void atomic_set(atomic_t *ptr, int v){
    *(volatile int *)&ptr->count=v;
}

static inline int atomic_read(atomic_t *ptr){
    return *(volatile int*)&ptr->count;
}

static inline void atomic_add(atomic_t *ptr, int addend){
    __asm__ volatile(
        "amoadd.w zero, %1, (%0)"
        :
        : "r" (&ptr->count), "r" (addend)
        : "memory"
    );
}
//add 1 only if ptr->count != 0 
//return 1 for success, and return 0 while fail.
static inline int atomic_inc_not_zero(atomic_t *ptr){
    int old, tmp;
    __asm__ volatile(
        "1: \n"
        "lr.w.aq %0, (%2)    \n"    //read value and store into old
        "beqz %0, 2f         \n"    //old==0, jump and return(don't add)
        "addi %1, %0, 1      \n"    //tmp=old+1
        "sc.w.rl %0, %1, (%2)    \n" //try to write back to ptr.count
        "bnez %0, 1b          \n"   //if write failed(return 0),retry it
        "li %0, 1             \n"   //set the success flag
        "j  3f                \n"   //jump out
        "2:                   \n"
        "li %0, 0             \n"   //set the failure flag
        "3:                   \n"
        : "=&r" (old), "=&r"(tmp)
        : "r" (&ptr->count)
        : "memory"
    );
    return old;
}

static inline int atomic_add_and_ret(atomic_t *ptr, int addend){
    int ret;
    //"memory" clobber acts as a compiler barrier, preventing the compiler
    //from reordering memory accesses across the barrier during optimization.
    //While: "aqrl" define hardware-level memory ordering,"aq" barrier guarantees that
    //subsequent cannot be reordered before it, "rl" ensures preceding cannot be reordered after it 
    __asm__ volatile(
        "amoadd.w.aqrl %0, %2, (%1)"    //() indicate that value can be dereferenced
        :  "=r" (ret)       // (%0)
        :  "r" (&ptr->count),   // %(1)
            "r" (addend)    //(%2)
        : "memory"
    );
    return ret+addend;
}

//Wrapper function
static inline void atomic_inc(atomic_t *ptr){
    atomic_add(ptr, 1);
}
static inline void atomic_dec(atomic_t *ptr){
    atomic_add(ptr, -1);
}
static inline int atomic_inc_and_ret(atomic_t *ptr){
    return atomic_add_and_ret(ptr, 1);
}
static inline int atomic_dec_and_ret(atomic_t *ptr){
    return atomic_add_and_ret(ptr, -1);
}
static inline int atomic_dec_and_test(atomic_t *ptr){
    return atomic_add_and_ret(ptr, -1)==0;
}