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

#define DEBUG_PRINT(fmt, ...) printf("\033[33m[TID:%ld][debug] " fmt "\033[0m\n", pthread_self(), ##__VA_ARGS__)

#define STACK_SIZE (1 << 16)  // 栈 64KB
#define MAX_LOCAL_QUEUE 4

typedef enum {
  CO_NEW,
  CO_RUNNING,
  CO_WAITING,
  CO_DEAD
} co_status_t;

// 协程控制块 (G)
struct co {
  char *name;
  void (*func)(void *);
  void *arg;

  co_status_t status;
  struct list waiters;
  ucontext_t context;
  uint8_t *stack;

  struct co *next;
};

// 协程调度器 (P)
struct processor {
  int id;
  
  struct co *private_queue[MAX_LOCAL_QUEUE];
  int private_head;
  int private_tail;
  int private_size;
  
  struct co *public_queue[MAX_LOCAL_QUEUE];
  int public_head;
  int public_tail;
  int public_size;
  pthread_mutex_t public_mutex;
  
  struct co *current_g;
  struct machine *m;
};

// 内核线程 (M)
struct machine {
  pthread_t thread;
  struct processor *p;
  int spinning;
};

// 全局状态
static struct {
  struct co *global_queue_head;
  struct co *global_queue_tail;
  int global_queue_size;
  pthread_mutex_t global_mutex;
    
  struct processor *processors[64];
  struct machine *machines[64];
  int num_processors;
  int num_machines;
    
  struct co *dead_queue_head;
  struct co *dead_queue_tail;
  int dead_queue_size;
  pthread_mutex_t dead_mutex;
    
  int gomaxprocs;
  int initialized;
} runtime;

static __thread struct machine *current_m = NULL;
static __thread struct processor *current_p = NULL;

static struct co main_co = {0};
static struct machine main_machine = {0};
static struct processor main_processor = {0};

static void runtime_init();
static struct co* global_queue_pop();
static void global_queue_push(struct co *g);
static struct co* local_queue_pop(struct processor *p);
static void local_queue_push(struct processor *p, struct co *g);
// static struct co* public_queue_pop(struct processor *p);
static void public_queue_push(struct processor *p, struct co *g);
static void move_public_to_private(struct processor *p);
static struct co* steal_work(struct processor *p);
static void schedule();
static void co_wrapper();
static void dead_queue_push(struct co *g);
static void cleanup_dead_coroutines();

struct thread_init_data {
  struct machine *m;
  void *(*start_routine)(void *);
  void *arg;
};

static void* thread_init_wrapper(void *arg) {
  struct thread_init_data *init_data = (struct thread_init_data *)arg;
  struct machine *m = init_data->m;
  void *(*start_routine)(void *) = init_data->start_routine;
  void *routine_arg = init_data->arg;
  
  current_m = m;
  current_p = m->p;
  
  DEBUG_PRINT("M启动, PID=%d", m->p->id);
  
  if (start_routine) {
    char thread_name[64];
    snprintf(thread_name, sizeof(thread_name), "M-%d", m->p->id);
    
    DEBUG_PRINT("创建协程执行start_routine: %s", thread_name);
    
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
    
    getcontext(&worker_co->context);
    worker_co->context.uc_stack.ss_sp = worker_co->stack;
    worker_co->context.uc_stack.ss_size = STACK_SIZE;
    worker_co->context.uc_link = NULL;
    makecontext(&worker_co->context, co_wrapper, 0);

    local_queue_push(m->p, worker_co);
    m->spinning = 0;
  }
  
  free(init_data);
  
  // 进入machine_loop调度循环，即G0
  while (1) {
    if (m->spinning) {
      usleep(1000); // 1ms
      m->spinning = 0;
    } else {
      schedule();
    }
  }
  
  return NULL;
}

__attribute__((constructor))
static void runtime_init() {
  if (runtime.initialized) return;
    
  DEBUG_PRINT("初始化多核协程运行中...");
    
  runtime.global_queue_head = NULL;
  runtime.global_queue_tail = NULL;
  runtime.global_queue_size = 0;
  pthread_mutex_init(&runtime.global_mutex, NULL);
    
  runtime.num_processors = 0;
  runtime.num_machines = 0;
  runtime.gomaxprocs = get_nprocs(); // 默认为可用的CPU核数
  runtime.initialized = 1;
    
  runtime.dead_queue_head = NULL;
  runtime.dead_queue_tail = NULL;
  runtime.dead_queue_size = 0;
  pthread_mutex_init(&runtime.dead_mutex, NULL);
    
  main_co.name = strdup("main");
  main_co.func = NULL;
  main_co.arg = NULL;
  main_co.status = CO_RUNNING;
  list_init(&main_co.waiters);
  main_co.stack = NULL;
  main_co.next = NULL;
    
  main_processor.id = 0;
  main_processor.private_head = 0;
  main_processor.private_tail = 0;
  main_processor.private_size = 0;
  main_processor.public_head = 0;
  main_processor.public_tail = 0;
  main_processor.public_size = 0;
  pthread_mutex_init(&main_processor.public_mutex, NULL);
  main_processor.current_g = &main_co;
  main_processor.m = &main_machine;
    
  main_machine.thread = pthread_self();
  main_machine.p = &main_processor;
  main_machine.spinning = 0;
    
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

  getcontext(&new_co->context);
  new_co->context.uc_stack.ss_sp = new_co->stack;
  new_co->context.uc_stack.ss_size = STACK_SIZE;
  new_co->context.uc_link = NULL;
  makecontext(&new_co->context, co_wrapper, 0);

  local_queue_push(current_p, new_co);
    
  return new_co;
}

void co_yield() {
  if (!current_p || !current_p->current_g) return;
    
  DEBUG_PRINT("协程 %s 调用 co_yield", current_p->current_g->name);
    
  struct co *current = current_p->current_g;
    
  // 如果当前协程仍然可运行，将其重新加入队列
  if (current->status == CO_RUNNING) {
    public_queue_push(current_p, current); // 函数内会判断是否需要放入全局队列
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

int co_thread(void *(*start_routine)(void *), void *arg) {
  if (runtime.num_machines >= runtime.gomaxprocs) {
    DEBUG_PRINT("已达到最大线程数 %d", runtime.gomaxprocs);
    return -1;
  }
    
  struct machine *m = malloc(sizeof(struct machine));
  struct processor *p = malloc(sizeof(struct processor));
  assert(m != NULL && p != NULL);
    
  p->id = runtime.num_processors;
  p->private_head = 0;
  p->private_tail = 0;
  p->private_size = 0;
  p->public_head = 0;
  p->public_tail = 0;
  p->public_size = 0;
  pthread_mutex_init(&p->public_mutex, NULL);
  p->current_g = NULL;
  p->m = m;
    
  m->p = p;
  m->spinning = 1;
    
  runtime.processors[runtime.num_processors++] = p;
  runtime.machines[runtime.num_machines++] = m;

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
    free(init_data);
    free(m);
    free(p);
    runtime.num_processors--;
    runtime.num_machines--;
  }
    
  return ret;
}

void co_set_gomaxprocs(int procs) {
  if (procs > 0 && procs <= 64) {
    runtime.gomaxprocs = procs;
    DEBUG_PRINT("设置 GOMAXPROCS=%d", procs);
  }
}

int co_get_gomaxprocs() {
  return runtime.gomaxprocs;
}

// ========== 内部调度函数 ==========

static struct co* global_queue_pop() {
  pthread_mutex_lock(&runtime.global_mutex);
    
  if (runtime.global_queue_size == 0) {
    pthread_mutex_unlock(&runtime.global_mutex);
    return NULL;
  }
  
  int random_pos = rand() % runtime.global_queue_size;
  struct co *g = NULL;
  
  if (random_pos == 0) {
    g = runtime.global_queue_head;
    runtime.global_queue_head = g->next;
    if (runtime.global_queue_head == NULL) {
      runtime.global_queue_tail = NULL;
    }
  } else {
    struct co *prev = runtime.global_queue_head;
    for (int i = 0; i < random_pos - 1; i++) {
      prev = prev->next;
    }
    
    g = prev->next;
    prev->next = g->next;
    
    if (g == runtime.global_queue_tail) {
      runtime.global_queue_tail = prev;
    }
  }
  
  runtime.global_queue_size--;
  g->next = NULL;
    
  pthread_mutex_unlock(&runtime.global_mutex);
  return g;
}

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

  DEBUG_PRINT("协程 %s 添加到全局队列", g->name);
}

static struct co* local_queue_pop(struct processor *p) {
  if (p->private_size == 0) {
    move_public_to_private(p);
  }
  if (p->private_size == 0) {
    return NULL;
  }
  
  int random_offset = rand() % p->private_size;
  int random_index = (p->private_head + random_offset) % MAX_LOCAL_QUEUE;
    
  struct co *g = p->private_queue[random_index];
    
  for (int i = 0; i < p->private_size - random_offset - 1; i++) {
    int src_index = (random_index + 1 + i) % MAX_LOCAL_QUEUE;
    int dst_index = (random_index + i) % MAX_LOCAL_QUEUE;
    p->private_queue[dst_index] = p->private_queue[src_index];
  }
    
  p->private_tail = (p->private_tail - 1 + MAX_LOCAL_QUEUE) % MAX_LOCAL_QUEUE;
  p->private_size--;
    
  // 如果这是private队列的最后一个协程，将public队列移动到private队列
  if (p->private_size == 0) {
    move_public_to_private(p);
  }
    
  return g;
}

static void local_queue_push(struct processor *p, struct co *g) {
  if (p->private_size >= MAX_LOCAL_QUEUE) {
    public_queue_push(p, g);
    return;
  }
    
  p->private_queue[p->private_tail] = g;
  p->private_tail = (p->private_tail + 1) % MAX_LOCAL_QUEUE;
  p->private_size++;
  DEBUG_PRINT("协程 %s 添加到P %d 的private队列", g->name, p->id);
}

// 单个弹出开销太大，不使用
// static struct co* public_queue_pop(struct processor *p) {
//   pthread_mutex_lock(&p->public_mutex);
  
//   if (p->public_size == 0) {
//     pthread_mutex_unlock(&p->public_mutex);
//     return NULL;
//   }
  
//   struct co *g = p->public_queue[p->public_head];
//   p->public_head = (p->public_head + 1) % MAX_LOCAL_QUEUE;
//   p->public_size--;
  
//   pthread_mutex_unlock(&p->public_mutex);
//   return g;
// }

static void public_queue_push(struct processor *p, struct co *g) {
  pthread_mutex_lock(&p->public_mutex);
  
  if (p->public_size >= MAX_LOCAL_QUEUE) {
    pthread_mutex_unlock(&p->public_mutex);
    global_queue_push(g);
    return;
  }
  
  p->public_queue[p->public_tail] = g;
  p->public_tail = (p->public_tail + 1) % MAX_LOCAL_QUEUE;
  p->public_size++;

  pthread_mutex_unlock(&p->public_mutex);

  DEBUG_PRINT("协程 %s 添加到P %d 的public队列", g->name, p->id);
}

static void move_public_to_private(struct processor *p) {
  pthread_mutex_lock(&p->public_mutex);
  
  while (p->public_size > 0 && p->private_size < MAX_LOCAL_QUEUE) {
    struct co *g = p->public_queue[p->public_head];
    p->public_head = (p->public_head + 1) % MAX_LOCAL_QUEUE;
    p->public_size--;
    
    p->private_queue[p->private_tail] = g;
    p->private_tail = (p->private_tail + 1) % MAX_LOCAL_QUEUE;
    p->private_size++;
  }
  
  pthread_mutex_unlock(&p->public_mutex);
}

static struct co* steal_work(struct processor *p) {
  int start = rand() % runtime.num_processors;
  for (int attempts = 0; attempts < runtime.num_processors; attempts++) {
    int target_id = (start + attempts) % runtime.num_processors;
    if (target_id == p->id) continue;
    
    struct processor *target_p = runtime.processors[target_id];
    if (!target_p) continue;
    
    pthread_mutex_lock(&target_p->public_mutex);
    
    if (target_p->public_size == 0) {
      pthread_mutex_unlock(&target_p->public_mutex);
      continue;
    }
    
    for (int i = 0; i < target_p->public_size; i++) {
      struct co *g = target_p->public_queue[target_p->public_head];
      target_p->public_head = (target_p->public_head + 1) % MAX_LOCAL_QUEUE;
      target_p->public_size--;
      
      local_queue_push(p, g);
    }

    pthread_mutex_unlock(&target_p->public_mutex);

    return local_queue_pop(p);
  }
  
  return NULL;
}

static void schedule() {
  struct processor *p = current_p;
  if (!p) return;

  struct co *next = NULL;

  // 1. 从本地队列获取
  next = local_queue_pop(p);
    
  // 2. 偷取
  if (!next) {
    next = steal_work(p);
    if (next) {
      DEBUG_PRINT("处理器 %d 通过work stealing获取协程 %s", p->id, next->name);
    }
  }
    
  // 3. 从全局队列获取
  if (!next) {
    next = global_queue_pop();
    if (next) {
      DEBUG_PRINT("处理器 %d 从全局队列获取协程 %s", p->id, next->name);
    }
  }
  
  // 4. 如果还是没有工作，进入自旋
  if (!next) {
    p->m->spinning = 1;
    DEBUG_PRINT("处理器 %d 没有可运行的协程，进入自旋", p->id);
    return;
  }
    
  p->m->spinning = 0;
    
  if (next->status == CO_NEW) {
    next->status = CO_RUNNING;
    DEBUG_PRINT("首次启动协程 %s", next->name);
  }
  
  struct co *prev = p->current_g;
  p->current_g = next;
    
  if (prev && prev != next) {
    DEBUG_PRINT("从协程 %s 切换到协程 %s", prev->name, next->name);
    swapcontext(&prev->context, &next->context);
  } else if (!prev) {
    DEBUG_PRINT("启动协程 %s", next->name);
    setcontext(&next->context);
  }
}

static void co_wrapper() {
  struct co *current = current_p->current_g;
  DEBUG_PRINT("协程 %s 开始执行", current->name);
  
  current->func(current->arg);
    
  DEBUG_PRINT("协程 %s 执行完毕", current->name);
  current->status = CO_DEAD;
  
  while (!list_empty(&current->waiters)) {
    struct co *waiter = (struct co *)list_pop_front(&current->waiters);
    DEBUG_PRINT("唤醒Waiter %s", waiter->name);
    waiter->status = CO_RUNNING;

    public_queue_push(current_p, waiter);
  }
  
  dead_queue_push(current);
  
  current_p->current_g = NULL;
  schedule();
}

static void dead_queue_push(struct co *g) {
  pthread_mutex_lock(&runtime.dead_mutex);
  
  g->next = NULL;
  if (runtime.dead_queue_tail) {
    runtime.dead_queue_tail->next = g;
  } else {
    runtime.dead_queue_head = g;
  }
  runtime.dead_queue_tail = g;
  runtime.dead_queue_size++;
  
  pthread_mutex_unlock(&runtime.dead_mutex);

  DEBUG_PRINT("协程 %s 添加到DEAD队列", g->name);
}

static void cleanup_dead_coroutines() {
  pthread_mutex_lock(&runtime.dead_mutex);
  
  struct co *current = runtime.dead_queue_head;
  while (current) {
    struct co *next = current->next;
    DEBUG_PRINT("清理DEAD协程 %s", current->name);
    if (current->name) {
      free(current->name);
      current->name = NULL;
    }
    if (current->stack) {
      free(current->stack);
      current->stack = NULL;
    }
    while (!list_empty(&current->waiters)) {
      list_pop_front(&current->waiters);
    }
    free(current);
    current = next;
  }
  
  runtime.dead_queue_head = NULL;
  runtime.dead_queue_tail = NULL;
  runtime.dead_queue_size = 0;
  
  pthread_mutex_unlock(&runtime.dead_mutex);
}

__attribute__((destructor))
static void co_cleanup() {
  if (!runtime.initialized) return;
    
  DEBUG_PRINT("清理多核协程Runtime");
    
  pthread_mutex_destroy(&runtime.global_mutex);
  
  cleanup_dead_coroutines();
  pthread_mutex_destroy(&runtime.dead_mutex);
    
  for (int i = 1; i < runtime.num_processors; i++) {
    if (runtime.processors[i] != &main_processor) {
      pthread_mutex_destroy(&runtime.processors[i]->public_mutex);
      free(runtime.processors[i]);
    }
  }
    
  pthread_mutex_destroy(&main_processor.public_mutex);
    
  for (int i = 1; i < runtime.num_machines; i++) {
    if (runtime.machines[i] != &main_machine) {
      free(runtime.machines[i]);
    }
  }
    
  if (main_co.name) {
    free(main_co.name);
    main_co.name = NULL;
  }
    
  DEBUG_PRINT("多核协程Runtime清理完成");
}