#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include "../include/co.h"

// 模拟工作负载的协程
void worker_coroutine(void *arg) {
    int worker_id = *(int*)arg;
    int thread_id = pthread_self() % 1000; // 简化显示
    
    for (int i = 0; i < 3; i++) {
        printf("Worker %d on Thread %d: Task %d\n", worker_id, thread_id, i + 1);
        co_yield(); // 让出CPU，触发调度
        usleep(100000); // 模拟工作
    }
    printf("Worker %d on Thread %d: Finished\n", worker_id, thread_id);
}

// 协程生产者，在当前P上创建多个协程
void producer_coroutine(void *arg) {
    int producer_id = *(int*)arg;
    int thread_id = pthread_self() % 1000;
    
    printf("Producer %d on Thread %d: Starting to create workers\n", producer_id, thread_id);
    
    // 在当前P上创建多个工作协程
    struct co *workers[3];
    int worker_ids[3];
    
    for (int i = 0; i < 3; i++) {
        worker_ids[i] = producer_id * 10 + i;
        workers[i] = co_start("worker", worker_coroutine, &worker_ids[i]);
        printf("Producer %d created Worker %d\n", producer_id, worker_ids[i]);
    }
    
    // 等待所有工作协程完成
    for (int i = 0; i < 3; i++) {
        co_wait(workers[i]);
    }
    
    printf("Producer %d on Thread %d: All workers finished\n", producer_id, thread_id);
}

// 跨P等待协程
void cross_p_waiter(void *arg) {
    struct co **target_cos = (struct co **)arg;
    int thread_id = pthread_self() % 1000;
    
    printf("Cross-P Waiter on Thread %d: Waiting for cross-thread coroutines\n", thread_id);
    
    // 等待其他P上的协程
    for (int i = 0; i < 2; i++) {
        if (target_cos[i] != NULL) {
            printf("Cross-P Waiter: Waiting for coroutine from another thread\n");
            co_wait(target_cos[i]);
            printf("Cross-P Waiter: Coroutine %d finished\n", i);
        }
    }
    
    printf("Cross-P Waiter on Thread %d: All cross-P waits completed\n", thread_id);
}

// 线程函数，代表一个M（内核线程）
void* thread_function(void *arg) {
    int thread_id = *(int*)arg;
    printf("Thread %d (M) started with P binding\n", thread_id);
    
    // 在这个P中创建生产者协程
    struct co *producer = co_start("producer", producer_coroutine, &thread_id);
    
    // 模拟一些本地工作
    printf("Thread %d: Doing some local work\n", thread_id);
    co_yield(); // 让生产者协程有机会运行
    
    // 等待生产者完成
    co_wait(producer);
    
    printf("Thread %d (M) finishing\n", thread_id);
    return NULL;
}

int main() {
    printf("=== Multi-Core Coroutine Scheduling Test ===\n");
    printf("Testing G-M-P model: G(Goroutines) - M(Threads) - P(Processors)\n\n");
    
    // 创建多个线程（M），每个线程绑定一个P
    const int num_threads = 3;
    pthread_t threads[num_threads];
    int thread_ids[num_threads];
    struct co *cross_targets[2] = {NULL, NULL};
    
    // 创建线程
    for (int i = 0; i < num_threads; i++) {
        thread_ids[i] = i + 1;
        if (pthread_create(&threads[i], NULL, thread_function, &thread_ids[i]) != 0) {
            perror("pthread_create failed");
            exit(1);
        }
    }
    
    // 主线程也作为一个P，创建跨P等待的协程
    printf("Main thread: Creating cross-P waiter\n");
    struct co *cross_waiter = co_start("cross_waiter", cross_p_waiter, cross_targets);
    
    // 主线程的一些协程工作
    printf("Main thread: Creating some local coroutines\n");
    int main_worker_id = 999;
    struct co *main_worker = co_start("main_worker", worker_coroutine, &main_worker_id);
    
    // 让协程有机会执行
    co_yield();
    
    // 等待主线程的协程
    co_wait(main_worker);
    co_wait(cross_waiter);
    
    // 等待所有线程完成
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("\n=== Expected Behavior Analysis ===\n");
    printf("1. Each thread (M) should bind to a processor (P)\n");
    printf("2. Coroutines (G) created in each P should primarily run on that P\n");
    printf("3. When local queue is empty, coroutines should be stolen from global queue\n");
    printf("4. co_wait should work across different Ps\n");
    printf("5. co_yield should trigger local scheduling first, then global scheduling\n");
    
    return 0;
} 