#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include "co_test.h"

int g_count = 0;

static void add_count() {
    g_count++;
}

static int get_count() {
    return g_count;
}

static void work_loop(void *arg) {
    const char *s = (const char*)arg;
    for (int i = 0; i < 100; ++i) {
        printf("%s%d  ", s, get_count());
        add_count();
        co_yield();
    }
}

static void work(void *arg) {
    work_loop(arg);
}

static void test_1() {
    printf("开始测试 #1: 基本协程调度\n");

    struct co *thd1 = co_start("thread-1", work, "X");
    struct co *thd2 = co_start("thread-2", work, "Y");

    co_wait(thd1);
    co_wait(thd2);

    printf("\n测试 #1 完成\n");
}

// -----------------------------------------------

static int g_running = 1;

static void do_produce(Queue *queue) {
    assert(!q_is_full(queue));
    Item *item = (Item*)malloc(sizeof(Item));
    if (!item) {
        fprintf(stderr, "New item failure\n");
        return;
    }
    item->data = (char*)malloc(10);
    if (!item->data) {
        fprintf(stderr, "New data failure\n");
        free(item);
        return;
    }
    memset(item->data, 0, 10);
    sprintf(item->data, "libco-%d", g_count++);
    q_push(queue, item);
}

static void producer(void *arg) {
    Queue *queue = (Queue*)arg;
    for (int i = 0; i < 100; ) {
        if (!q_is_full(queue)) {
            // co_yield();
            do_produce(queue);
            i += 1;
        }
        co_yield();
    }
}

static void do_consume(Queue *queue) {
    assert(!q_is_empty(queue));

    Item *item = q_pop(queue);
    if (item) {
        printf("%s  ", (char *)item->data);
        free(item->data);
        free(item);
    }
}

static void consumer(void *arg) {
    Queue *queue = (Queue*)arg;
    while (g_running) {
        if (!q_is_empty(queue)) {
            do_consume(queue);
        }
        co_yield();
    }
}

static void test_2() {
    printf("\n开始测试 #2: 生产者-消费者模式\n");

    Queue *queue = q_new();

    struct co *thd1 = co_start("producer-1", producer, queue);
    struct co *thd2 = co_start("producer-2", producer, queue);
    struct co *thd3 = co_start("consumer-1", consumer, queue);
    struct co *thd4 = co_start("consumer-2", consumer, queue);

    co_wait(thd1);
    co_wait(thd2);

    g_running = 0;

    co_wait(thd3);
    co_wait(thd4);

    while (!q_is_empty(queue)) {
        do_consume(queue);
    }

    q_free(queue);
    printf("\n测试 #2 完成\n");
}

// 空的线程函数，用于启动额外的工作线程
void* worker_thread(void* arg) {
    // 这个函数会在新线程中作为协程运行
    printf("工作线程 %d 已启动\n", *(int*)arg);
    // 让线程持续运行，等待协程任务
    while (1) {
        sleep(1);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    
    printf("=== 多核协程测试程序 ===\n");
    printf("GOMAXPROCS: %d\n", co_get_gomaxprocs());
    
    // 在测试开始前创建3个额外的线程
    printf("创建3个额外的工作线程...\n");
    static int thread_ids[3] = {1, 2, 3};
    
    for (int i = 0; i < 3; i++) {
        int ret = co_thread(worker_thread, &thread_ids[i]);
        if (ret == 0) {
            printf("成功创建线程 %d\n", i + 1);
        } else {
            printf("创建线程 %d 失败\n", i + 1);
        }
    }
    
    // 给线程一些时间启动
    usleep(100000); // 100ms
    
    printf("\n开始协程测试...\n\n");

    printf("Test #1. Expect: (X|Y){0, 1, 2, ..., 199}\n");
    test_1();

    printf("\n\nTest #2. Expect: (libco-){200, 201, 202, ..., 399}\n");
    test_2();

    printf("\n\n=== 测试完成 ===\n");

    return 0;
} 