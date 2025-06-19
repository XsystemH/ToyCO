#include <stdio.h>
#include <unistd.h>
#include "co.h"

void work(void *arg) {
    int worker_id = (int)(long)arg;
    int sum = 0;
    printf("Worker %d started\n", worker_id);
    
    for (int i = 0; i < 100; i++) {
        sum += i;
        if (i % 20 == 0) {
            printf("Worker %d: sum=%d, progress=%d/100\n", worker_id, sum, i);
            co_yield();
        }
    }
    
    printf("Worker %d finished with sum=%d\n", worker_id, sum);
}

int main() {
    printf("=== 测试Work Stealing ===\n");

    co_thread(NULL, NULL);
    
    // 创建15个协程任务
    printf("创建15个协程任务...\n");
    struct co *co[15];
    for (int i = 0; i < 15; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker-%d", i);
        co[i] = co_start(name, work, (void*)(long)i);
        if (i && (i+1) % 5 == 0) {
            co_yield();
        }
    }
    
    printf("等待所有协程完成...\n");
    // 等待所有协程完成
    for (int i = 0; i < 15; i++) {
        co_wait(co[i]);
    }
    
    printf("=== 所有任务完成 ===\n");
    return 0;
} 