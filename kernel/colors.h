#ifndef _COLORS_H
#define _COLORS_H

/* 基础颜色代码 */
#define ANSI_RESET   "\x1b[0m"     // 关闭所有属性（关键！）
#define ANSI_RED     "\x1b[31m"    // 红色 - 这里的 31 代表前景红
#define ANSI_GREEN   "\x1b[32m"    // 绿色
#define ANSI_YELLOW  "\x1b[33m"    // 黄色
#define ANSI_BLUE    "\x1b[34m"    // 蓝色
#define ANSI_MAGENTA "\x1b[35m"    // 紫色
#define ANSI_CYAN    "\x1b[36m"    // 青色

/* 高亮/加粗模式 (通常会更亮) */
#define ANSI_BOLD_RED     "\x1b[1;31m"
#define ANSI_BOLD_YELLOW  "\x1b[1;33m"

#endif


// 这里的 ##__VA_ARGS__ 是 GCC/Clang 特性，允许参数为空
#define pr_err(fmt, ...) \
    do{     \
        printf(ANSI_BOLD_RED "[ERROR] %s:%d: " fmt ANSI_RESET "\n", __func__, __LINE__, ##__VA_ARGS__);    \
        panic("pr_err");    \
    }while(0)


#define pr_warn(fmt, ...) \
    printf(ANSI_YELLOW "[WARN] %s:%d " fmt ANSI_RESET "\n",__func__, __LINE__,  ##__VA_ARGS__)

#define pr_info(fmt, ...) \
    do{     \
        if(0)   printf(ANSI_CYAN "[INFO] %s:%d " fmt ANSI_RESET "\n",__func__, __LINE__,  ##__VA_ARGS__);    \
    }while(0)
    // dummy_printf(ANSI_CYAN "[INFO] %s:%d " fmt ANSI_RESET "\n",__func__, __LINE__,  ##__VA_ARGS__)
    //printf(ANSI_CYAN "[INFO] %s:%d " fmt ANSI_RESET "\n",__func__, __LINE__,  ##__VA_ARGS__)
