
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

//Helper functions (static inline)
static inline rb_node_t *rb_parent(rb_node_t *rb){
    return (rb_node_t*)(rb->rb_parent_color & ~3);
}
static inline int rb_color(rb_node_t *rb){
    return rb->rb_parent_color & 1;
}
static inline void rb_set_parent(rb_node_t *rb, rb_node_t *rb_parent){
    //reset the parent of a node
    rb->rb_parent_color=(rb->rb_parent_color & 3) | (uint64)rb_parent;
}
static inline void rb_set_color(rb_node_t *rb ,int color){
    rb->rb_parent_color=(rb->rb_parent_color & ~1) | (uint64)color;
}
/*
 *Useful macro: get the pointer to your containing struct from the embedded rt_node
 * ptr : pointer to the rb_node()
 * the type of your struct(e.g. vm_area which defined in proc.h)
*/
#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
