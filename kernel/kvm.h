//NOTE:A special structure created to manage temporary allocation without recording them 
//in the page table, designed to avoid excessive physical memory consumption.
//It must be free after use,otherwise, it will cuase memory waste.
struct mem_trans_stash{ //Rearrange the memory layout for check and set poison quickly
    //Fixed length array.
    void *ptr[MAX_ORDER*2];
    uint8 order[MAX_ORDER*2];   //Paired with ptr.
    int count;
    uint32 magic;
    uint8 is_occupied;  //zero means free, while 1 means occupied.
};

#define MEM_TRANS_STACH_MAGIC 0x21792352

