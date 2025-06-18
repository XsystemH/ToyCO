#include "co.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ucontext.h>

// Debug输出宏定义
#define DEBUG_PRINT(fmt, ...) printf("\033[33m[debug] " fmt "\033[0m\n", ##__VA_ARGS__)

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
    ucontext_t context;   // 使用ucontext替代jmp_buf
    uint8_t *stack;       // 协程栈
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
static void co_wrapper();

// 程序启动时自动执行，确保main_co为第一个协程
__attribute__((constructor))
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

  // 初始化ucontext
  getcontext(&new_co->context);
  new_co->context.uc_stack.ss_sp = new_co->stack;
  new_co->context.uc_stack.ss_size = STACK_SIZE;
  new_co->context.uc_link = NULL;  // 协程结束后不自动返回
  makecontext(&new_co->context, co_wrapper, 0);

  co_add_to_list(&co_run_list, new_co);
  DEBUG_PRINT("协程 %s 已创建完成, 状态: CO_NEW, 添加到可运行列表", name);

  return new_co;
}

void co_yield() {
  DEBUG_PRINT("协程 %s 调用 co_yield", co_current->name);
  
  struct co *current = co_current;
  co_schedule();  // 选择下一个协程
  
  // 如果调度器选择了不同的协程，进行切换
  if (co_current != current) {
    DEBUG_PRINT("从协程 %s 切换到协程 %s", current->name, co_current->name);
    swapcontext(&current->context, &co_current->context);
    DEBUG_PRINT("协程 %s 从切换中恢复运行", current->name);
  }
}

void co_wait(struct co *co) {
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

static void co_schedule() {
  struct co *next = co_choose_next();
  if (next == NULL) {
    DEBUG_PRINT("没有可运行的协程，程序退出");
    exit(0);
  }
  
  if (next->status == CO_NEW) {
    DEBUG_PRINT("首次启动协程 %s", next->name);
    next->status = CO_RUNNING;
  }
  
  co_current = next;
}

static void co_wrapper() {
  struct co *current = co_current;
  DEBUG_PRINT("协程 %s 开始执行，状态: CO_RUNNING", current->name);

  current->func(current->arg);

  DEBUG_PRINT("协程 %s 函数执行完毕，状态变更: CO_RUNNING -> CO_DEAD", current->name);
  current->status = CO_DEAD;
  co_remove_from_list(&co_run_list, current);
  co_add_to_list(&co_dead_list, current);
  
  if (current->waiter) {
    DEBUG_PRINT("协程 %s 结束，唤醒等待者 %s", current->name, current->waiter->name);
    current->waiter->status = CO_RUNNING;
    co_add_to_list(&co_run_list, current->waiter);
    co_remove_from_list(&co_wait_list, current->waiter);
    DEBUG_PRINT("等待者 %s 状态变更: CO_WAITING -> CO_RUNNING", current->waiter->name);
  }
  
  DEBUG_PRINT("协程 %s 准备退出，调用 co_schedule", current->name);
  co_schedule();
  
  // 切换到下一个协程
  DEBUG_PRINT("从已结束协程 %s 切换到协程 %s", current->name, co_current->name);
  setcontext(&co_current->context);
}