
#define RB_RED 0
#define RB_BLACK 1

struct rb_node{
    uint64 rb_parent_color;  //stores parent pointer and color in one field
    struct rb_node *rb_left;
    struct rb_node *rb_right;
} __attribute__((aligned(__alignof__(uint64))));  //Force alignment.(compiler-Intrinsic Keyword)
//This is critical for the bit-packing to work,as it ensures low bits of the pointer are 0.

struct rb_root{
    rb_node_t *rb_parent;
};

/*
 * WARNING: Below are a LEAF FUNCTION. 
 * To improve performance, the compiler may not save the return address (ra) 
 * to the stack. This will cause this function to be "invisible" in standard 
 * stack backtraces unless DWARF unwind info is used.
 */
static inline int rb_color(rb_node_t *rb){
    //Leaf function: don't call any other function, so ra it not saved on the stack
    //So it's diffcult to backtrace.
    //dummy_force_save_ra();
    if(rb==NULL)    return RB_BLACK;
    return rb->rb_parent_color & 1;
}

//Helper functions (static inline)
static inline rb_node_t *rb_parent(rb_node_t *rb){
    if(rb==NULL)    panic("Try to access nullptr's parent!\n");
    return (rb_node_t*)(rb->rb_parent_color & ~3);
}

// static inline int rb_color(rb_node_t *rb){
//     return rb->rb_parent_color & 1;
// }
static inline void rb_set_parent(rb_node_t *rb, rb_node_t *rb_parent){
    //reset the parent of a node
    if(rb==NULL)    return;
    if(rb==rb_parent)   panic("Attempting to point the parent node to itself.\n");
    rb->rb_parent_color=(rb->rb_parent_color & 3) | (uint64)rb_parent;
}

static inline void rb_set_color(rb_node_t *rb ,int color){
    if(rb==NULL)    return;
    rb->rb_parent_color=(rb->rb_parent_color & ~1) | (uint64)color;
}
/*
 *Useful macro: get the pointer to your containing struct from the embedded rt_node
 * ptr : pointer to the rb_node()
 * the type of your struct(e.g. vm_area which defined in proc.h)
*/
#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static inline void rb_clear(rb_node_t *ptr){
    ptr->rb_left=NULL;
    ptr->rb_right=NULL;
    ptr->rb_parent_color=0;
}
