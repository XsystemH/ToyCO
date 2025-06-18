#include <stdio.h>
#include "co.h"

#define MAX_ITERATIONS 10  // 设置每个协程的最大运行次数

struct co_arg {
    const char *letter;
    int max_count;
};

void entry(void *arg) {
    struct co_arg *co_arg = (struct co_arg *)arg;
    int count = 0;
    
    while (count < co_arg->max_count) {
        printf("%s", co_arg->letter);
        fflush(stdout);  // 确保输出立即显示
        count++;
        co_yield();
    }
    printf("\n协程 %s 完成，共打印了 %d 次\n", co_arg->letter, count);
}

int main() {
    struct co_arg arg1 = {"a", MAX_ITERATIONS};
    struct co_arg arg2 = {"b", MAX_ITERATIONS};
    
    struct co *co1 = co_start("co1", entry, &arg1);
    struct co *co2 = co_start("co2", entry, &arg2);
    
    co_wait(co1);
    co_wait(co2);
    
    printf("所有协程执行完毕!\n");
    return 0;
} 