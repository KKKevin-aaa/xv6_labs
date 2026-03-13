#define MAX(a, b) (((a) < (b)) ? (b) : (a))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define POISON_BYTE 0X5A
#define POISON_64 0x5a5a5a5a5a5a5a5aull

// fixme: error
static inline uint64 gen_bitfield_mask(uint8 low, uint8 high, uint8 total_bits, int invert){
    if(low >high || high >=total_bits || low>= total_bits){
        printf("[gen_bitfield_mask]Invalid mask range or bit width parameter.");
        return 0;
    }
    uint8 length=high - low +1;
    uint64 retval=(length ==64 ) ? ~0ULL : (1ull << length) -1;
    retval <<= low;
    if(invert==1){
        retval = ~retval;
        if(total_bits < 64){
            uint64 inv_mask=(1ull << total_bits) -1;
            retval &= inv_mask;
        }
    }
    return retval;
}
