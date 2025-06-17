#ifndef CO_H
#define CO_H

#include <stddef.h>
#include <setjmp.h>

// 协程状态枚举
typedef enum {
    CO_NEW,      // 新创建，还未运行
    CO_RUNNING,  // 正在运行
    CO_WAITING,  // 被其他协程等待
    CO_DEAD      // 已经结束
} co_status_t;

// 协程控制块
struct co {
    char *name;           // 协程名称
    void (*func)(void *); // 协程函数
    void *arg;            // 协程函数参数
    
    co_status_t status;   // 协程状态
    union {
        jmp_buf jmp_ctx;  // setjmp/longjmp版本的上下文
        void *stack_ptr;  // 汇编版本的栈指针
    } context;
    char *stack;          // 协程栈
    size_t stack_size;    // 栈大小
    
    struct co *waiter;    // 等待当前协程的协程
    struct co *next;      // 链表指针，用于管理协程队列
};

// 协程管理函数
struct co* co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(struct co *co);

#endif