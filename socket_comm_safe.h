#ifndef __SOCKET_COMM_SAFE_H__
#define __SOCKET_COMM_SAFE_H__

#include <pthread.h>

// Thread-safe queue implementation
typedef struct {
    struct Item {
        int index;
        char pos_str[20];
        char id_str[20];
    } items[MAX_ITEM_NUM];
    int front;
    int rear;
    pthread_mutex_t mutex;
} ThreadSafeQueue;

// Thread-safe queue functions
void tsqueue_init(ThreadSafeQueue *q);
void tsqueue_destroy(ThreadSafeQueue *q);
int tsqueue_enqueue(ThreadSafeQueue *q, int index, const char *pos_str, const char *id_str);
int tsqueue_dequeue(ThreadSafeQueue *q, struct Item *item);
int tsqueue_is_empty(ThreadSafeQueue *q);
int tsqueue_is_full(ThreadSafeQueue *q);

// Safe string operations
int safe_strcpy(char *dest, const char *src, size_t dest_size);
int safe_buffer_process(char *buffer, int received_len, int max_size);

#endif /* __SOCKET_COMM_SAFE_H__ */