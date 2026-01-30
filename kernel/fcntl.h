#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400

// kernel/mman.h
//flags for vm_flags, paired with vm_page_prot(In vm_area_struct)
//prot parameter macros(simulating <sys/mman.h>)
// 1. 内存保护权限 (Protection) - 对应页表项的 R/W/X
#define PROT_NONE       0x0     // 页不可访问
#define PROT_READ       0x1     // 页可读
#define PROT_WRITE      0x2     // 页可写
#define PROT_EXEC       0x4     // 页可执行
#define PROT_USER       0x8


// 2. 映射标志 (Flags) - 决定 VMA 的行为
#define MAP_SHARED      0x01    // 共享映射 (写回文件，其他进程可见)
#define MAP_PRIVATE     0x02    // 私有映射 (COW，写时不改文件)

// 3. 映射类型与选项
#define MAP_FIXED       0x10    // 强制使用指定的 addr，否则失败 (慎用)
#define MAP_ANONYMOUS   0x20    // 匿名映射 (不依赖文件，如堆/栈)
//为了兼容性，有的系统写作 MAP_ANON
#ifndef MAP_ANON
#define MAP_ANON        MAP_ANONYMOUS
#endif
#define MAP_LOCKED      0x0200          // 映射后立即锁定内存
#define MAP_GROWSDOWN   0x0100          // 用于栈
#define MAP_POPULATE    0x8000          // 映射时预读缺页 (Pre-fault)
// 4. 错误码
#define MAP_FAILED      ((void *)-1)

