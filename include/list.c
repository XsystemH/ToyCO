#include "list.h"
#include <stdlib.h>
#include <assert.h>

// 初始化链表
void list_init(struct list *list) {
    list->head = NULL;
    list->size = 0;
}

// 在链表头部添加元素
void list_add(struct list *list, void *data) {
    struct list_node *node = malloc(sizeof(struct list_node));
    assert(node != NULL);
    
    node->data = data;
    node->next = list->head;
    list->head = node;
    list->size++;
}

// 从链表中移除指定元素
void list_remove(struct list *list, void *data) {
    struct list_node *node = list->head;
    struct list_node *prev = NULL;
    
    while (node != NULL) {
        if (node->data == data) {
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
    // 如果元素不存在，不做任何操作
}

// 清空链表
void list_clear(struct list *list) {
    struct list_node *node = list->head;
    while (node != NULL) {
        struct list_node *next = node->next;
        free(node);
        node = next;
    }
    list->head = NULL;
    list->size = 0;
}

// 检查链表是否为空
int list_empty(struct list *list) {
    return list->size == 0;
}

// 弹出并返回链表头部元素
void* list_pop_front(struct list *list) {
    if (list->head == NULL) {
        return NULL;
    }
    
    struct list_node *node = list->head;
    void *data = node->data;
    
    list->head = node->next;
    list->size--;
    free(node);
    
    return data;
} 