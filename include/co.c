#include "co.h"
#include "list.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ucontext.h>
#include <pthread.h>
#include <sys/sysinfo.h>

// Debug输出宏定义
#define DEBUG_PRINT(fmt, ...) // printf("\033[33m[TID:%ld][debug] " fmt "\033[0m\n", pthread_self(), ##__VA_ARGS__)

#define STACK_SIZE (1 << 16)  // 栈 64KB
#define MAX_LOCAL_QUEUE 4   // 本地队列最大大小

// 协程状态
typedef enum {
  CO_NEW,      // 新创建，还未运行
  CO_RUNNING,  // 正在运行
  CO_WAITING,  // 被其他协程等待
  CO_DEAD      // 已经结束
} co_status_t;

// 协程控制块 (G)
struct co {
  char *name;           // 协程名称
  void (*func)(void *); // 协程函数
  void *arg;            // 协程函数参数

  co_status_t status;   // 协程状态
  struct list waiters;  // 等待这个协程的协程列表
  ucontext_t context;   // 协程上下文
  uint8_t *stack;       // 协程栈

  struct co *next;      // 用于队列链表
};

// 协程调度器 (P)
struct processor {
  int id;                           // 处理器ID
  struct co *local_queue[MAX_LOCAL_QUEUE]; // 本地队列
  int local_head;                   // 本地队列头
  int local_tail;                   // 本地队列尾
  int local_size;                   // 本地队列大小
  struct co *current_g;             // 当前运行的协程
  struct co g0;                     // 调度协程G0
  struct machine *m;                // 绑定的机器
};

// 内核线程 (M)
struct machine {
  pthread_t thread;                 // 系统线程
  struct processor *p;              // 绑定的处理器
  struct co *g0;                    // 机器的G0
  int spinning;                     // 是否在自旋
};

// 全局状态
static struct {
  struct co *global_queue_head;     // 全局队列头
  struct co *global_queue_tail;     // 全局队列尾
  int global_queue_size;            // 全局队列大小
  pthread_mutex_t global_mutex;     // 全局队列锁
    
  struct processor *processors[64]; // 处理器数组
  struct machine *machines[64];     // 机器数组
  int num_processors;               // 处理器数量
  int num_machines;                 // 机器数量
    
  int gomaxprocs;                   // 最大处理器数量
  int initialized;                  // 是否已初始化
} runtime;

// 线程局部存储
static __thread struct machine *current_m = NULL;
static __thread struct processor *current_p = NULL;

// 主协程和主机器
static struct co main_co = {0};
static struct machine main_machine = {0};
static struct processor main_processor = {0};

// 函数声明
static void runtime_init();
static struct co* global_queue_pop();
static void global_queue_push(struct co *g);
static struct co* local_queue_pop(struct processor *p);
static void local_queue_push(struct processor *p, struct co *g);
// static struct co* steal_work(struct processor *p); TODO
static void schedule();
static void co_wrapper();
static void wake_processor();

// 线程初始化数据结构
struct thread_init_data {
  struct machine *m;
  void *(*start_routine)(void *);
  void *arg;
};

// 线程初始化包装函数
static void* thread_init_wrapper(void *arg) {
  struct thread_init_data *init_data = (struct thread_init_data *)arg;
  struct machine *m = init_data->m;
  void *(*start_routine)(void *) = init_data->start_routine;
  void *routine_arg = init_data->arg;
  
  // 设置线程局部变量
  current_m = m;
  current_p = m->p;
  
  DEBUG_PRINT("机器线程启动, 处理器ID=%d", m->p->id);
  
  // 如果有start_routine，在新线程中创建协程来执行它
  if (start_routine) {
    char thread_name[64];
    snprintf(thread_name, sizeof(thread_name), "thread-worker-%d", m->p->id);
    
    DEBUG_PRINT("在线程中创建协程执行start_routine: %s", thread_name);
    
    // 创建协程来执行start_routine
    struct co *worker_co = malloc(sizeof(struct co));
    assert(worker_co != NULL);
    
    worker_co->name = strdup(thread_name);
    worker_co->func = (void (*)(void *))start_routine;
    worker_co->arg = routine_arg;
    worker_co->status = CO_NEW;
    list_init(&worker_co->waiters);
    worker_co->next = NULL;
    
    worker_co->stack = (uint8_t *)malloc(STACK_SIZE);
    assert(worker_co->stack != NULL);
    
    // 初始化ucontext
    getcontext(&worker_co->context);
    worker_co->context.uc_stack.ss_sp = worker_co->stack;
    worker_co->context.uc_stack.ss_size = STACK_SIZE;
    worker_co->context.uc_link = NULL;
    makecontext(&worker_co->context, co_wrapper, 0);
    
    // 将协程放入本地队列
    local_queue_push(m->p, worker_co);
    m->spinning = 0;  // 有工作了，不需要自旋
  }
  
  // 清理初始化数据
  free(init_data);
  
  // 进入machine_loop调度循环
  while (1) {
    if (m->spinning) {
      struct co *g = global_queue_pop();
      // if (!g) {
      //   g = steal_work(m->p);
      // } TODO
            
      if (g) {
        m->spinning = 0;
        m->p->current_g = g;
        if (g->status == CO_NEW) {
          g->status = CO_RUNNING;
        }
        DEBUG_PRINT("机器获得协程 %s", g->name);
        setcontext(&g->context);
      } else {
        // 短暂休眠避免过度自旋
        usleep(1000); // 1ms
      }
    } else {
      schedule();
    }
  }
  
  return NULL;
}

// 初始化运行
__attribute__((constructor))
static void runtime_init() {
  if (runtime.initialized) return;
    
  DEBUG_PRINT("初始化多核协程运行中...");
    
  // 初始化全局状态
  runtime.global_queue_head = NULL;
  runtime.global_queue_tail = NULL;
  runtime.global_queue_size = 0;
  pthread_mutex_init(&runtime.global_mutex, NULL);
    
  runtime.num_processors = 0;
  runtime.num_machines = 0;
  runtime.gomaxprocs = get_nprocs(); // 默认为可用的CPU核数
  runtime.initialized = 1;
    
  // 初始化主协程
  main_co.name = strdup("main");
  main_co.func = NULL;
  main_co.arg = NULL;
  main_co.status = CO_RUNNING;
  list_init(&main_co.waiters);
  main_co.stack = NULL;
  main_co.next = NULL;
    
  // 初始化主处理器
  main_processor.id = 0;
  main_processor.local_head = 0;
  main_processor.local_tail = 0;
  main_processor.local_size = 0;
  main_processor.current_g = &main_co;
  main_processor.m = &main_machine;
    
  // 初始化主机器
  main_machine.thread = pthread_self();
  main_machine.p = &main_processor;
  main_machine.g0 = &main_processor.g0;
  main_machine.spinning = 0;
    
  // 设置线程局部变量
  current_m = &main_machine;
  current_p = &main_processor;
    
  runtime.processors[0] = &main_processor;
  runtime.machines[0] = &main_machine;
  runtime.num_processors = 1;
  runtime.num_machines = 1;
    
  srand(time(NULL));
  DEBUG_PRINT("多核协程运行时初始化完成, GOMAXPROCS=%d", runtime.gomaxprocs);
}

struct co* co_start(const char *name, void (*func)(void *), void *arg) {
  DEBUG_PRINT("创建新协程: %s", name);
  struct co *new_co = malloc(sizeof(struct co));
  assert(new_co != NULL);

  new_co->name = strdup(name);
  new_co->func = func;
  new_co->arg = arg;
  new_co->status = CO_NEW;
  list_init(&new_co->waiters);
  new_co->next = NULL;

  new_co->stack = (uint8_t *)malloc(STACK_SIZE);
  assert(new_co->stack != NULL);

  // 初始化ucontext
  getcontext(&new_co->context);
  new_co->context.uc_stack.ss_sp = new_co->stack;
  new_co->context.uc_stack.ss_size = STACK_SIZE;
  new_co->context.uc_link = NULL;
  makecontext(&new_co->context, co_wrapper, 0);

  // 优先放入本地队列，如果满了则放入全局队列
  if (current_p && current_p->local_size < MAX_LOCAL_QUEUE) {
    local_queue_push(current_p, new_co);
    DEBUG_PRINT("协程 %s 添加到处理器 %d 的本地队列", name, current_p->id);
  } else {
    global_queue_push(new_co);
    DEBUG_PRINT("协程 %s 添加到全局队列", name);
  }
    
  // 尝试唤醒空闲的处理器
  wake_processor();
    
  return new_co;
}

void co_yield() {
  if (!current_p || !current_p->current_g) return;
    
  DEBUG_PRINT("协程 %s 调用 co_yield", current_p->current_g->name);
    
  struct co *current = current_p->current_g;
    
  // 如果当前协程仍然可运行，将其重新加入队列
  if (current->status == CO_RUNNING) {
    if (current_p->local_size < MAX_LOCAL_QUEUE) {
      local_queue_push(current_p, current);
    } else {
      global_queue_push(current);
    }
  }
  schedule();
}

void co_wait(struct co *co) {
  assert(co != NULL);
  assert(current_p && current_p->current_g);
  assert(co != current_p->current_g);
    
  DEBUG_PRINT("协程 %s 等待协程 %s", current_p->current_g->name, co->name);
    
  if (co->status == CO_DEAD) {
    DEBUG_PRINT("协程 %s 已经结束，无需等待", co->name);
    return;
  }
    
  struct co *current = current_p->current_g;
  current->status = CO_WAITING;
  list_add(&co->waiters, current);
    
  DEBUG_PRINT("协程 %s 进入等待状态", current->name);
  schedule();
}

// 创建线程
int co_thread(void *(*start_routine)(void *), void *arg) {
  if (runtime.num_machines >= runtime.gomaxprocs) {
    DEBUG_PRINT("已达到最大线程数 %d", runtime.gomaxprocs);
    return -1;
  }
    
  struct machine *m = malloc(sizeof(struct machine));
  struct processor *p = malloc(sizeof(struct processor));
  assert(m != NULL && p != NULL);
    
  // 初始化处理器
  p->id = runtime.num_processors;
  p->local_head = 0;
  p->local_tail = 0;
  p->local_size = 0;
  p->current_g = NULL;
  p->m = m;
    
  // 初始化机器
  m->p = p;
  m->g0 = &p->g0;
  m->spinning = 1;  // 默认自旋状态
    
  runtime.processors[runtime.num_processors++] = p;
  runtime.machines[runtime.num_machines++] = m;

  // 准备线程初始化数据
  struct thread_init_data *init_data = malloc(sizeof(struct thread_init_data));
  assert(init_data != NULL);
  init_data->m = m;
  init_data->start_routine = start_routine;
  init_data->arg = arg;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
  int ret = pthread_create(&m->thread, &attr, thread_init_wrapper, init_data);
  pthread_attr_destroy(&attr);
    
  if (ret == 0) {
    DEBUG_PRINT("创建新线程成功, 处理器ID=%d", p->id);
  } else {
    // 创建失败，清理资源
    free(init_data);
    free(m);
    free(p);
    runtime.num_processors--;
    runtime.num_machines--;
  }
    
  return ret;
}

// 设置GOMAXPROCS
void co_set_gomaxprocs(int procs) {
  if (procs > 0 && procs <= 64) {
    runtime.gomaxprocs = procs;
    DEBUG_PRINT("设置 GOMAXPROCS=%d", procs);
  }
}

// 获取GOMAXPROCS
int co_get_gomaxprocs() {
  return runtime.gomaxprocs;
}

// ========== 内部调度函数 ==========

// 从全局队列随机弹出协程
static struct co* global_queue_pop() {
  pthread_mutex_lock(&runtime.global_mutex);
    
  if (runtime.global_queue_size == 0) {
    pthread_mutex_unlock(&runtime.global_mutex);
    return NULL;
  }
  
  // 随机选择一个位置
  int random_pos = rand() % runtime.global_queue_size;
  struct co *g = NULL;
  
  if (random_pos == 0) {
    // 选择头部节点
    g = runtime.global_queue_head;
    runtime.global_queue_head = g->next;
    if (runtime.global_queue_head == NULL) {
      runtime.global_queue_tail = NULL;
    }
  } else {
    // 找到要删除节点的前一个节点
    struct co *prev = runtime.global_queue_head;
    for (int i = 0; i < random_pos - 1; i++) {
      prev = prev->next;
    }
    
    g = prev->next;
    prev->next = g->next;
    
    // 如果删除的是尾节点，需要更新tail指针
    if (g == runtime.global_queue_tail) {
      runtime.global_queue_tail = prev;
    }
  }
  
  runtime.global_queue_size--;
  g->next = NULL;
    
  pthread_mutex_unlock(&runtime.global_mutex);
  return g;
}

// 向全局队列推送协程
static void global_queue_push(struct co *g) {
  pthread_mutex_lock(&runtime.global_mutex);
    
  g->next = NULL;
  if (runtime.global_queue_tail) {
    runtime.global_queue_tail->next = g;
  } else {
    runtime.global_queue_head = g;
  }
  runtime.global_queue_tail = g;
  runtime.global_queue_size++;
    
  pthread_mutex_unlock(&runtime.global_mutex);
}

// 从本地队列随机弹出协程
static struct co* local_queue_pop(struct processor *p) {
  if (p->local_size == 0) return NULL;
    
  // 随机选择一个位置
  int random_offset = rand() % p->local_size;
  int random_index = (p->local_head + random_offset) % MAX_LOCAL_QUEUE;
  
  struct co *g = p->local_queue[random_index];
  
  // 将被选中位置后的所有元素向前移动一位
  for (int i = 0; i < p->local_size - random_offset - 1; i++) {
    int src_index = (random_index + 1 + i) % MAX_LOCAL_QUEUE;
    int dst_index = (random_index + i) % MAX_LOCAL_QUEUE;
    p->local_queue[dst_index] = p->local_queue[src_index];
  }
  
  // 更新队列尾部指针和大小
  p->local_tail = (p->local_tail - 1 + MAX_LOCAL_QUEUE) % MAX_LOCAL_QUEUE;
  p->local_size--;
    
  return g;
}

// 向本地队列推送协程
static void local_queue_push(struct processor *p, struct co *g) {
  if (p->local_size >= MAX_LOCAL_QUEUE) return;
    
  p->local_queue[p->local_tail] = g;
  p->local_tail = (p->local_tail + 1) % MAX_LOCAL_QUEUE;
  p->local_size++;
}

// 偷取 TODO
//static struct co* steal_work(struct processor *p) {
//    return NULL;
//}

// 调度器
static void schedule() {
  struct processor *p = current_p;
  if (!p) return;

  struct co *next = NULL;

  // 1. 从本地队列获取
  next = local_queue_pop(p);
    
  // 2. 从全局队列获取
  if (!next) {
    next = global_queue_pop();
    if (next) {
      DEBUG_PRINT("处理器 %d 从全局队列获取协程 %s", p->id, next->name);
    }
  }
    
  // // 3. 偷取 TODO
  // if (!next) {
  //   next = steal_work(p);
  // }
  
  // 4. 如果还是没有工作，进入自旋
  if (!next) {
    p->m->spinning = 1;
    DEBUG_PRINT("处理器 %d 没有可运行的协程，进入自旋", p->id);
    return;
  }
    
  p->m->spinning = 0;
    
  // 设置协程状态
  if (next->status == CO_NEW) {
    next->status = CO_RUNNING;
    DEBUG_PRINT("首次启动协程 %s", next->name);
  }
  
  struct co *prev = p->current_g;
  p->current_g = next;
    
  // 进行上下文切换
  if (prev && prev != next) {
    DEBUG_PRINT("从协程 %s 切换到协程 %s", prev->name, next->name);
    swapcontext(&prev->context, &next->context);
  } else if (!prev) {
    DEBUG_PRINT("启动协程 %s", next->name);
    setcontext(&next->context);
  }
}

// 协程包装函数
static void co_wrapper() {
  struct co *current = current_p->current_g;
  DEBUG_PRINT("协程 %s 开始执行", current->name);
  
  current->func(current->arg);
    
  DEBUG_PRINT("协程 %s 执行完毕", current->name);
  current->status = CO_DEAD;
  
  // 唤醒所有等待者
  while (!list_empty(&current->waiters)) {
    struct co *waiter = (struct co *)list_pop_front(&current->waiters);
    DEBUG_PRINT("唤醒等待者 %s", waiter->name);
    waiter->status = CO_RUNNING;

    if (current_p->local_size < MAX_LOCAL_QUEUE) {
      local_queue_push(current_p, waiter);
    } else {
      global_queue_push(waiter);
    }
  }
    
  current_p->current_g = NULL;
  schedule();
}


// 唤醒空闲处理器
static void wake_processor() {
  for (int i = 0; i < runtime.num_machines; i++) {
    if (runtime.machines[i]->spinning) {
      DEBUG_PRINT("唤醒自旋的处理器 %d", i);
      break;
    }
  }
}

// 程序退出时自动执行内存清理
__attribute__((destructor))
static void co_cleanup() {
  if (!runtime.initialized) return;
    
  DEBUG_PRINT("清理多核协程运行时");
    
  pthread_mutex_destroy(&runtime.global_mutex);
    
  // 清理动态分配的处理器和机器
  for (int i = 1; i < runtime.num_processors; i++) {
    if (runtime.processors[i] != &main_processor) {
      free(runtime.processors[i]);
    }
  }
    
  for (int i = 1; i < runtime.num_machines; i++) {
    if (runtime.machines[i] != &main_machine) {
      free(runtime.machines[i]);
    }
  }
    
  if (main_co.name) {
    free(main_co.name);
    main_co.name = NULL;
  }
    
  DEBUG_PRINT("多核协程运行时清理完成");
}