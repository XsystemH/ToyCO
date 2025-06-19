# ToyCO

Thanks JasonFan & Luowen for the Guidance and Help!

## API

```c
struct co *co_start(const char *name, void (*func)(void *), void *arg);
void       co_yield();
void       co_wait(struct co *co);
```

1. co_start(name, func, arg) 创建一个新的协程，并返回一个指向 struct co 的指针 (类似于 pthread_create)。
  - 新创建的协程从函数 func 开始执行，并传入参数 arg。新创建的协程不会立即执行，而是调用 co_start 的协程继续执行。
  - co_start 返回的 struct co 指针需要 malloc() 分配内存。
2. co_wait(co) 表示当前协程需要等待，直到 co 协程的执行完成才能继续执行 (类似于 pthread_join)。
  - 允许一个协程被多个协程等待。
  - co 结束时不会释放 co 占用的内存, main 函数结束时会释放所有协程占用的内存。
3. co_yield() 实现协程的切换。协程运行后一直在 CPU 上执行，直到 func 函数返回或调用 co_yield 使当前运行的协程暂时放弃执行。co_yield 时若系统中有多个可运行的协程时 (包括当前协程)，你随机选择下一个系统中可运行的协程。
4. main 函数的执行也是一个协程，因此可以在 main 中调用 co_yield 或 co_wait。main 函数返回后，无论有多少协程，进程都将直接终止。

## Example

### 1. 交替打印 a 和 b

创建两个 (永不结束的) 协程，分别打印 a 和 b，交替执行。将会看到随机的 ab 交替出现的序列，例如 ababbabaaaabbaa...

```c
#include <stdio.h>
#include "co.h"

void entry(void *arg) {
    while (1) {
        printf("%s", (const char *)arg);
        co_yield();
    }
}

int main() {
    struct co *co1 = co_start("co1", entry, "a");
    struct co *co2 = co_start("co2", entry, "b");
    co_wait(co1); // never returns
    co_wait(co2);
}
```

### 2. 交替打印 a 和 b，共享 counter 变量

两个协程会交替执行，共享 counter 变量：字母是随机的 (a 或 b)，数字则从 1 到 10 递增。

```
b[1] a[2] b[3] b[4] a[5] b[6] b[7] a[8] a[9] a[10] Done
```

```c
#include <stdio.h>
#include "co.h"

int count = 1; // 协程之间共享

void entry(void *arg) {
    for (int i = 0; i < 5; i++) {
        printf("%s[%d] ", (const char *)arg, count++);
        co_yield();
    }
}

int main() {
    struct co *co1 = co_start("co1", entry, "a");
    struct co *co2 = co_start("co2", entry, "b");
    co_wait(co1);
    co_wait(co2);
    printf("Done\n");
}
```

## Multi-core

对并行的多个线程的协程调度。

co_yeild co_wait co_start 应为基本原语 类似于goroutine的实现

![alt text](imgs/multi-core.jpg)

### G-M-P

- G: 协程(coroutine)
  - 创建时优先存储在P的本地队列中，如果本地队列不足，则存储在全局队列中。
- M: 内核线程(kernel thread) 绑定一个P
  - 每个M会有一个自己的G0，G0是M的主协程，没有函数，用于调度M上的其他协程。在调度或系统调用时会使用G0的栈空间, 全局变量的G0是M0的G0。
  - 在开始运行时会创建M0，对应的实例会在全局变量runtime.m0中，不需要在heap上分配，M0负责执行初始化操作和启动第一个G(main函数)，此后M0与其余M等价。
- P: 协程调度器(coroutine processor)
  - 最多存在GOMAXPROCS个P，GOMAXPROCS是环境变量，默认是CPU核数。
  - 本地队列: 存储协程G，其中所有的状态均为New或Running。

co_wait可以跨越P进行等待其他P上的G。

为了性能，每个会绑定一个 P，当前执行 G 在 co_yeild 时会先从绑定的 P 中的本地队列中找下一个调度的 G，如果本地队列不足，则从全局队列中找下一个调度的 G。该G会被移动到执行其的P的本地队列中。

### 实现细节

- 封装pthread
  - 当程序期望多线程使用协程时，不直接调用pthread，而是调用co_thread，在co_thread中创建一个pthread，并绑定一个P。
- 单线程表现
  - 当程序不涉及多线程时，仍然会有一个P绑定M0，通过G0调度协程。、
- 线程安全
  - 协程只保证调度的安全，数据由用户自己保证。
  - 由于只有全局队列会被多核访问，需要加锁。

**P-P-Steal**: 当P的本地队列为空时，P会从其他P的本地队列中偷取协程。
- 每个P会维护两个本地队列private和public，一个只会被自己访问（无需加锁），另一个会被其他P访问用于被偷取（需要加锁）。
- 当全局队列中没有协程时，P会尝试从其他P的队列中偷取协程，偷取时会将被偷取的public队列中的所有协程移动到自己的private队列中。
- 当P有新协程创建时，优先将新协程添加到private队列中。
- 当P有协程yield时，会将该协程添加到public队列中（若已满则放入全局队列）。
- 当结束的协程为private的最后一个协程时，将自己的public中的协程全部移动至private。

## 调度算法

结合G-M-P模型，实现了如下的调度算法：

1. 当P有协程创建时，优先将新协程添加到private队列中。
2. 当P有协程yield时，会将该协程添加到public队列中（若已满则放入全局队列）。
3. 当P的private队列为空时，将public队列中的所有协程移动到private队列中。
5. 当P仍为空时，从其他P的public队列中偷取协程。
4. 当P仍为空时，从全局队列中获取协程。

即所有的G被分为了3个优先级:

- 优先级1: private队列中的协程
- 优先级2: public队列中的协程
- 优先级3: 全局队列中的协程

同优先级协程的调度顺序是随机的。

### Example Changes

**单线程**
- 样例一中，两个协程交替打印a和b，交替执行。
- 样例二中，两个协程交替打印a和b，共享counter变量。

```
ababababab
a[1] b[2] a[3] b[4] a[5] b[6] a[7] b[8] a[9] b[10] Done
```

**多线程**: ab出现顺序不固定

## Implementation

Language: C

### co struct

```c
struct co {
  char *name;
  void (*func)(void *);
  void *arg;

  co_status_t status;
  struct co *waiter;
  struct co *waiter;    // 等待的协程
  jmp_buf context;
  char *stack;          // 协程栈
}
```