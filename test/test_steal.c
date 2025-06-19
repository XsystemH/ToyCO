#include <stdio.h>
#include "co.h"

void* start_routine(void *arg) {
    (void)arg;
    return NULL;
}

void work(void *arg) {
    (void)arg;
    int sum = 0;
    for (int i = 0; i < 100; i++) {
        sum += i;
        if (i % 10 == 0) {
            printf("sum: %d\n", sum);
            co_yield();
        }
    }
}

int main() {
    co_set_gomaxprocs(4);
    for (int i = 0; i < 3; i++) {
        co_thread(start_routine, NULL);
    }

    struct co *co[30];
    for (int i = 0; i < 30; i++) {
        co[i] = co_start("co", work, NULL);
    }
    for (int i = 0; i < 30; i++) {
        co_wait(co[i]);
    }
    return 0;
} 