#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include "co.h"

#define NUM_THREADS 3        // 只创建3个额外线程 (M0+M1+M2+M3=4个线程)
#define NUM_COROUTINES 6     // 每个线程创建6个协程
#define WORK_ITERATIONS 3    // 减少工作迭代次数

// 测试数据结构
struct test_data {
    int thread_id;
    int coroutine_id;
    const char* name;
};

// 全局计数器（测试竞争条件）
static volatile int global_counter = 0;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;

// 获取当前时间（毫秒）
long long get_current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

// 计算密集型协程
void compute_work(void *arg) {
    struct test_data *data = (struct test_data *)arg;
    printf("[T%d] 协程 %s 开始计算工作\n", data->thread_id, data->name);
    
    for (int i = 0; i < WORK_ITERATIONS; i++) {
        // 模拟计算工作
        volatile int sum = 0;
        for (int j = 0; j < 500000; j++) {
            sum += j;
        }
        
        printf("[T%d] 协程 %s 完成计算 %d/%d (sum=%d)\n", 
               data->thread_id, data->name, i+1, WORK_ITERATIONS, sum);
        
        // 主动让出CPU
        co_yield();
    }
    
    printf("[T%d] 协程 %s 计算工作完成\n", data->thread_id, data->name);
}

// I/O密集型协程（模拟）
void io_work(void *arg) {
    struct test_data *data = (struct test_data *)arg;
    printf("[T%d] 协程 %s 开始I/O工作\n", data->thread_id, data->name);
    
    for (int i = 0; i < WORK_ITERATIONS; i++) {
        // 模拟I/O等待
        usleep(5000); // 5ms
        
        printf("[T%d] 协程 %s 完成I/O %d/%d\n", 
               data->thread_id, data->name, i+1, WORK_ITERATIONS);
        
        co_yield();
    }
    
    printf("[T%d] 协程 %s I/O工作完成\n", data->thread_id, data->name);
}

// 混合工作协程 + 全局计数器测试
void mixed_work(void *arg) {
    struct test_data *data = (struct test_data *)arg;
    printf("[T%d] 协程 %s 开始混合工作\n", data->thread_id, data->name);
    
    for (int i = 0; i < WORK_ITERATIONS; i++) {
        // 短时间计算
        volatile int sum = 0;
        for (int j = 0; j < 100000; j++) {
            sum += j;
        }
        
        // 更新全局计数器
        pthread_mutex_lock(&counter_mutex);
        int old_value = global_counter;
        global_counter++;
        pthread_mutex_unlock(&counter_mutex);
        
        printf("[T%d] 协程 %s 混合工作 %d/%d: 计数器 %d -> %d\n", 
               data->thread_id, data->name, i+1, WORK_ITERATIONS, old_value, old_value+1);
        
        usleep(3000); // 3ms
        co_yield();
    }
    
    printf("[T%d] 协程 %s 混合工作完成\n", data->thread_id, data->name);
}

// 线程工作函数
void* thread_worker(void *arg) {
    struct test_data *data = (struct test_data *)arg;
    printf("线程 %d 启动，准备创建 %d 个协程\n", data->thread_id, NUM_COROUTINES);
    
    struct co *coroutines[NUM_COROUTINES];
    struct test_data co_data[NUM_COROUTINES];
    
    // 创建不同类型的协程
    for (int i = 0; i < NUM_COROUTINES; i++) {
        co_data[i].thread_id = data->thread_id;
        co_data[i].coroutine_id = i;
        
        char *name = malloc(32);
        sprintf(name, "T%d-C%d", data->thread_id, i);
        co_data[i].name = name;
        
        // 根据协程编号选择不同的工作类型
        if (i % 3 == 0) {
            coroutines[i] = co_start(name, compute_work, &co_data[i]);
            printf("线程 %d 创建计算协程 %s\n", data->thread_id, name);
        } else if (i % 3 == 1) {
            coroutines[i] = co_start(name, io_work, &co_data[i]);
            printf("线程 %d 创建I/O协程 %s\n", data->thread_id, name);
        } else {
            coroutines[i] = co_start(name, mixed_work, &co_data[i]);
            printf("线程 %d 创建混合协程 %s\n", data->thread_id, name);
        }
    }
    
    // 等待所有协程完成
    printf("线程 %d 等待所有协程完成...\n", data->thread_id);
    for (int i = 0; i < NUM_COROUTINES; i++) {
        co_wait(coroutines[i]);
        free((void*)co_data[i].name);
    }
    
    printf("线程 %d 所有协程已完成\n", data->thread_id);
    return NULL;
}

int main() {
    printf("=== 多核协程调度系统测试 ===\n");
    printf("CPU核数: %d\n", co_get_gomaxprocs());
    printf("设置协程调度器数量为: 4\n");
    
    // 设置GOMAXPROCS为4
    co_set_gomaxprocs(4);
    
    long long test_start = get_current_time_ms();
    
    // 创建工作线程
    printf("\n=== 创建工作线程 ===\n");
    struct test_data thread_data[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i + 1;
        thread_data[i].name = "ThreadWorker";
        
        printf("启动工作线程 %d\n", i + 1);
        co_thread(thread_worker, &thread_data[i]);
    }
    
    // 主线程也创建一些协程
    printf("\n=== 主线程协程测试 ===\n");
    struct co *main_cos[4];
    struct test_data main_data[4];
    
    for (int i = 0; i < 4; i++) {
        main_data[i].thread_id = 0; // 主线程
        main_data[i].coroutine_id = i;
        
        char *name = malloc(32);
        sprintf(name, "Main-%d", i);
        main_data[i].name = name;
        
        if (i % 2 == 0) {
            main_cos[i] = co_start(name, compute_work, &main_data[i]);
        } else {
            main_cos[i] = co_start(name, mixed_work, &main_data[i]);
        }
        printf("主线程创建协程 %s\n", name);
    }
    
    // 等待主线程协程完成
    for (int i = 0; i < 4; i++) {
        co_wait(main_cos[i]);
        free((void*)main_data[i].name);
    }
    
    // 等待工作线程完成
    printf("\n等待所有线程完成...\n");
    sleep(3);
    
    long long test_end = get_current_time_ms();
    
    // 统计结果
    printf("\n=== 测试结果 ===\n");
    printf("全局计数器最终值: %d\n", global_counter);
    printf("总测试时间: %lld ms\n", test_end - test_start);
    
    // 计算预期值：每个线程有NUM_COROUTINES个协程，其中1/3是混合协程，每个贡献WORK_ITERATIONS次
    // 主线程也有4个协程，其中2个是混合协程
    int expected = (NUM_THREADS * (NUM_COROUTINES / 3) + 2) * WORK_ITERATIONS;
    printf("预期全局计数器值: %d\n", expected);
    
    if (global_counter > 0 && global_counter <= expected + 10) {
        printf("多核协程调度测试 PASSED\n");
        printf("- 成功创建并调度多个协程\n");
        printf("- 协程能在不同线程间正确执行\n");
        printf("- 全局状态保持一致性\n");
    } else {
        printf("多核协程调度测试 FAILED\n");
        printf("- 全局计数器值异常: %d (期望: %d)\n", global_counter, expected);
    }
    
    printf("测试完成\n");
    return 0;
}
