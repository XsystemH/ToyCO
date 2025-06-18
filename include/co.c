#include "co.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// Debug输出宏定义
#define DEBUG_PRINT(fmt, ...) printf("\033[33m[debug] " fmt "\033[0m\n", ##__VA_ARGS__)

static inline void
stack_switch_call(void *stack_top, void *entry, uintptr_t arg) {
  asm volatile (
#if __x86_64__
  "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
  :
  : "b"((uintptr_t)stack_top - 8),
    "d"(entry),
    "a"(arg)
  : "memory"
#else
  "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
  :
  : "b"((uintptr_t)stack_top - 8),
    "d"(entry),
    "a"(arg)
  : "memory"
#endif
  );
}

#define STACK_SIZE (1 << 16)  // 栈 64KB
#define MAX_CO_NUM 1024

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
    struct co *waiter;    // 等待的协程
    jmp_buf context;
    uint8_t *stack;          // 协程栈
};

struct co_node {
  struct co *co;
  struct co_node *next;
};

struct co_list {
  struct co_node *head;
  int size;
};

struct co_list co_run_list;
struct co_list co_wait_list;
struct co_list co_dead_list;

static struct co *co_current = NULL;    // 当前运行的协程
static struct co main_co = {0};      // 主协程

static void co_list_init(struct co_list *list);
static void co_add_to_list(struct co_list *list, struct co *co);
static void co_remove_from_list(struct co_list *list, struct co *co);
static void co_schedule();
static void co_wrapper(struct co *co);

// 在每次调用函数时执行，确保main_co为第一个协程
static void co_init() {
  if (main_co.name == NULL) {
    DEBUG_PRINT("初始化协程系统");
    main_co.name = strdup("main"); // 使用strdup复制字符串并分配内存
    main_co.func = NULL;
    main_co.arg = NULL;
    main_co.status = CO_RUNNING;
    main_co.waiter = NULL;
    main_co.stack = NULL;

    co_current = &main_co;
    
    // 初始化协程列表
    co_list_init(&co_run_list);
    co_list_init(&co_wait_list);
    co_list_init(&co_dead_list);
    co_add_to_list(&co_run_list, &main_co);

    // 设置随机种子
    srand(time(NULL));
    DEBUG_PRINT("主协程已创建: %s, 状态: CO_RUNNING", main_co.name);
  }
}

struct co* co_start(const char *name, void (*func)(void *), void *arg) {
  co_init();

  DEBUG_PRINT("创建新协程: %s", name);
  struct co *new_co = malloc(sizeof(struct co));
  assert(new_co != NULL);

  new_co->name = strdup(name);
  new_co->func = func;
  new_co->arg = arg;
  new_co->status = CO_NEW;
  new_co->waiter = NULL;

  new_co->stack = (uint8_t *)malloc(STACK_SIZE);
  assert(new_co->stack != NULL);

  co_add_to_list(&co_run_list, new_co);
  DEBUG_PRINT("协程 %s 已创建完成, 状态: CO_NEW, 添加到可运行列表", name);

  return new_co;
}

void co_yield() {
  co_init();

  DEBUG_PRINT("协程 %s 调用 co_yield", co_current->name);
  int val = setjmp(co_current->context);

  if (val == 0) {
    co_schedule();
  } else {
    DEBUG_PRINT("协程 %s 从切换中恢复运行", co_current->name);
    return;
  }
}

void co_wait(struct co *co) {
  co_init();

  assert(co != NULL);
  assert(co != co_current);

  DEBUG_PRINT("协程 %s 等待协程 %s", co_current->name, co->name);
  
  if (co->status == CO_DEAD) {
    DEBUG_PRINT("协程 %s 已经结束，无需等待", co->name);
    return;
  }

  DEBUG_PRINT("协程 %s 状态变更: CO_RUNNING -> CO_WAITING", co_current->name);
  co_current->status = CO_WAITING;
  co->waiter = co_current;

  co_add_to_list(&co_wait_list, co_current);
  co_remove_from_list(&co_run_list, co_current);
  DEBUG_PRINT("协程 %s 移至等待列表", co_current->name);

  co_yield();
}

// ========== 内部辅助函数实现 ==========

static void co_list_init(struct co_list *list) {
  list->head = NULL;
  list->size = 0;
}

static void co_add_to_list(struct co_list *list, struct co *co) {
  struct co_node *node = malloc(sizeof(struct co_node));
  node->co = co;
  node->next = list->head;
  list->head = node;
  list->size++;
}

static void co_remove_from_list(struct co_list *list, struct co *co) {
  struct co_node *node = list->head;
  struct co_node *prev = NULL;
  while (node != NULL) {
    if (node->co == co) {
      if (prev == NULL) {
        list->head = node->next;
      } else {
        prev->next = node->next;
      }
      list->size--;
      free(node);
      return;
    }
    prev = node;
    node = node->next;
  }
  assert(0); // 协程不存在
}

static struct co* co_choose_next() {
  // 如果没有其他协程可运行，返回NULL
  if (co_run_list.size == 0) {
    DEBUG_PRINT("可运行列表为空，无协程可调度");
    return NULL;
  }
  
  // 随机选择一个协程
  int index = rand() % co_run_list.size;
  struct co_node *node = co_run_list.head;
  for (int i = 0; i < index; i++) {
    node = node->next;
  }
  DEBUG_PRINT("调度器选择协程: %s (索引: %d/%d)", node->co->name, index, co_run_list.size);
  return node->co;
}

static void co_switch_to(struct co *target) {
  co_current = target;

  if (target->status == CO_NEW) {
    DEBUG_PRINT("首次启动协程 %s, 创建新栈", target->name);
    stack_switch_call(target->stack + STACK_SIZE, co_wrapper, (uintptr_t)target);
  } else if (target->status == CO_RUNNING) {
    DEBUG_PRINT("恢复协程 %s 的执行", target->name);
    longjmp(target->context, 1);
  } else {
    DEBUG_PRINT("错误：尝试切换到无效状态的协程 %s (状态: %d)", target->name, target->status);
    assert(0);
  }
}

static void co_schedule() {
  struct co *next = co_choose_next();
  co_switch_to(next);
}

static void co_wrapper(struct co *co) {
  DEBUG_PRINT("协程 %s 开始执行，状态变更: CO_NEW -> CO_RUNNING", co->name);
  co->status = CO_RUNNING;

  co->func(co->arg);

  DEBUG_PRINT("协程 %s 函数执行完毕，状态变更: CO_RUNNING -> CO_DEAD", co->name);
  co->status = CO_DEAD;
  co_remove_from_list(&co_run_list, co);
  co_add_to_list(&co_dead_list, co);
  
  if (co->waiter) {
    DEBUG_PRINT("协程 %s 结束，唤醒等待者 %s", co->name, co->waiter->name);
    co->waiter->status = CO_RUNNING;
    co_add_to_list(&co_run_list, co->waiter);
    co_remove_from_list(&co_wait_list, co->waiter);
    DEBUG_PRINT("等待者 %s 状态变更: CO_WAITING -> CO_RUNNING", co->waiter->name);
  }
  
  DEBUG_PRINT("协程 %s 准备退出，调用 co_schedule", co->name);
  co_schedule();
}