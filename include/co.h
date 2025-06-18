#ifndef CO_H
#define CO_H

#include <pthread.h>

// 基本协程API
struct co* co_start(const char *name, void (*func)(void *), void *arg);
void co_yield();
void co_wait(struct co *co);

// 多核协程API
int co_thread(void *(*start_routine)(void *), void *arg);
void co_set_gomaxprocs(int procs);
int co_get_gomaxprocs();

#endif