// Standard SBI Errors
#define SBI_SUCCESS 0
#define SBI_ERR_FAILED  -1
#define SBI_ERR_NOT_SUPPORTED   -2
#define SBI_ERR_INVALID_PARAM   -3
#define SBI_ERR_DENIED  -4
#define SBI_ERR_INVALID_ADDRESS -5
#define SBI_ERR_ALREADY_AVAILABLE   -6
#define SBI_ERR_ALREADY_STARTED -7
#define SBI_ERR_ALREADY_STOPPED -8
#define SBI_NO_SHMEM    -9
#define SBI_ERR_INVALID_STATE   -10
#define SBI_ERR_BAD_RANGE   -11
#define SBI_ERR_TIMEOUT -12
#define SBI_ERR_IO  -13

struct sbiret{
    long error;
    long value;
};

//SBI interface: In LP64(Unix), unsigned long matches the CPU Xlen.
//For risc-V 64, this corresponds to uint64.
static inline struct sbiret sbi_ecall
    (int ext, int fid, uint64 arg0, uint64 arg1, uint64 arg2,
    uint64 arg3, uint64 arg4, uint64 arg5){
    struct sbiret ret;
    //Register Variables, bind variables to specific registers
    //Also use "memory" clobber to prevent optimization.
    register uint64 a0 __asm__("a0") =arg0;
    register uint64 a1 __asm__("a1") =arg1;
    register uint64 a2 __asm__("a2") =arg2;
    register uint64 a3 __asm__("a3") =arg3;
    register uint64 a4 __asm__("a4") =arg4;
    register uint64 a5 __asm__("a5") =arg5;
    register uint64 a6 __asm__("a6") =fid;  //function id
    register uint64 a7 __asm__("a7") =ext;  //extension id
    __asm__ volatile
    ("ecall" 
    : "+r"(a0),  "+r"(a1) 
    : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
    : "memory"
    );
    ret.error=a0;
    ret.value=a1;
    return ret;
}

//CLINE(Core local interrupter) designed for M-mode, using MSIP register.
//Ecall to m-mode, and then relevant handler relegate interrupt to S-mode, become SSIP.
static inline struct sbiret sbi_send_ipi(uint64 hart_mask, uint64 hart_mask_base){
    //With hart_mask_base, we can operator all XLEN hart(may bigger than 64)
    //And support batch processing more convient.
    return sbi_ecall(0x735049, 0, hart_mask, hart_mask_base, 0, 0, 0, 0);
}

static inline struct sbiret sbi_set_timer(uint64 stime_value){
    return sbi_ecall(0x54494D45, 0x0, stime_value, 0, 0, 0, 0, 0);
}