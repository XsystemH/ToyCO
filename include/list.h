#ifndef LIST_H
#define LIST_H

// 通用链表节点结构
struct list_node {
    void *data;
    struct list_node *next;
};

// 链表结构
struct list {
    struct list_node *head;
    int size;
};

// 链表操作函数声明
void list_init(struct list *list);
void list_add(struct list *list, void *data);
void list_remove(struct list *list, void *data);
void list_clear(struct list *list);
int list_empty(struct list *list);
void* list_pop_front(struct list *list);

#endif // LIST_H 