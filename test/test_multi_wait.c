#include <stdio.h>
#include "co.h"

struct co *target_co;

void target_entry(void *arg) {
    (void)arg; // 避免未使用参数警告
    printf("目标协程开始执行\n");
    for (int i = 0; i < 3; i++) {
        printf("目标协程执行中... %d\n", i);
        co_yield();
    }
    printf("目标协程即将结束\n");
}

void waiter_entry(void *arg) {
    const char *name = (const char *)arg;
    printf("等待者 %s 开始等待目标协程\n", name);
    co_wait(target_co);
    printf("等待者 %s 被唤醒，目标协程已结束\n", name);
}

int main() {
    printf("=== 多协程等待测试 ===\n");
    
    // 创建目标协程
    target_co = co_start("target", target_entry, NULL);
    
    // 创建多个等待者协程
    struct co *waiter1 = co_start("waiter1", waiter_entry, "A");
    struct co *waiter2 = co_start("waiter2", waiter_entry, "B");
    struct co *waiter3 = co_start("waiter3", waiter_entry, "C");
    struct co *waiter4 = co_start("waiter4", waiter_entry, "D");
    struct co *waiter5 = co_start("waiter5", waiter_entry, "E");
    struct co *waiter6 = co_start("waiter6", waiter_entry, "F");
    
    // 主协程也等待所有协程
    co_wait(waiter1);
    co_wait(waiter2);
    co_wait(waiter3);
    co_wait(waiter4);
    co_wait(waiter5);
    co_wait(waiter6);
    
    printf("所有协程都已完成\n");
    return 0;
} 