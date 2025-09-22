// Thread-safe queue and safety functions implementation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "socket_comm.h"
#include "socket_comm_safe.h"
#include "unified_log.h"

// Initialize thread-safe queue
void tsqueue_init(ThreadSafeQueue *q)
{
    if (!q) return;
    
    pthread_mutex_init(&q->mutex, NULL);
    pthread_mutex_lock(&q->mutex);
    q->front = -1;
    q->rear = -1;
    memset(q->items, 0, sizeof(q->items));
    pthread_mutex_unlock(&q->mutex);
}

// Destroy thread-safe queue
void tsqueue_destroy(ThreadSafeQueue *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->mutex);
}

// Check if queue is full (must be called with lock held)
int tsqueue_is_full(ThreadSafeQueue *q)
{
    if (!q) return 1;
    
    pthread_mutex_lock(&q->mutex);
    int full = (q->rear == MAX_ITEM_NUM - 1);
    pthread_mutex_unlock(&q->mutex);
    return full;
}

// Check if queue is empty (must be called with lock held)
int tsqueue_is_empty(ThreadSafeQueue *q)
{
    if (!q) return 1;
    
    pthread_mutex_lock(&q->mutex);
    int empty = (q->front == -1 || q->front > q->rear);
    pthread_mutex_unlock(&q->mutex);
    return empty;
}

// Thread-safe enqueue
int tsqueue_enqueue(ThreadSafeQueue *q, int index, const char *pos_str, const char *id_str)
{
    if (!q || !pos_str || !id_str) return -1;
    
    pthread_mutex_lock(&q->mutex);
    
    // Check if full
    if (q->rear == MAX_ITEM_NUM - 1) {
        pthread_mutex_unlock(&q->mutex);
        glog_warning("Queue is full! Cannot enqueue index=%d\n", index);
        return -1;
    }
    
    // First item
    if (q->front == -1) {
        q->front = 0;
    }
    
    q->rear++;
    q->items[q->rear].index = index;
    
    // Safe string copy with bounds checking
    strncpy(q->items[q->rear].pos_str, pos_str, sizeof(q->items[q->rear].pos_str) - 1);
    q->items[q->rear].pos_str[sizeof(q->items[q->rear].pos_str) - 1] = '\0';
    
    strncpy(q->items[q->rear].id_str, id_str, sizeof(q->items[q->rear].id_str) - 1);
    q->items[q->rear].id_str[sizeof(q->items[q->rear].id_str) - 1] = '\0';
    
    pthread_mutex_unlock(&q->mutex);
    
    glog_debug("Enqueued item: index=%d, pos=%s, ID=%s\n", index, pos_str, id_str);
    return 0;
}

// Thread-safe dequeue
int tsqueue_dequeue(ThreadSafeQueue *q, struct Item *item)
{
    if (!q || !item) return -1;
    
    pthread_mutex_lock(&q->mutex);
    
    // Check if empty
    if (q->front == -1 || q->front > q->rear) {
        pthread_mutex_unlock(&q->mutex);
        item->index = -1;
        item->pos_str[0] = '\0';
        item->id_str[0] = '\0';
        return -1;
    }
    
    // Copy item
    *item = q->items[q->front];
    q->front++;
    
    // Reset queue if empty
    if (q->front > q->rear) {
        q->front = q->rear = -1;
    }
    
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

// Safe string copy with size checking
int safe_strcpy(char *dest, const char *src, size_t dest_size)
{
    if (!dest || !src || dest_size == 0) return -1;
    
    size_t src_len = strlen(src);
    if (src_len >= dest_size) {
        // String too long, truncate
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
        glog_warning("String truncated: original length %zu, buffer size %zu\n", src_len, dest_size);
        return 1; // Truncated
    }
    
    strcpy(dest, src);
    return 0; // Success
}

// Safe buffer processing with validation
int safe_buffer_process(char *buffer, int received_len, int max_size)
{
    if (!buffer || received_len <= 0 || received_len > max_size) {
        glog_error("Invalid buffer parameters: received=%d, max=%d\n", received_len, max_size);
        return -1;
    }
    
    // Ensure null termination
    if (received_len < max_size) {
        buffer[received_len] = '\0';
    } else {
        buffer[max_size - 1] = '\0';
        glog_warning("Buffer at maximum size, forced null termination\n");
    }
    
    // Check for suspicious patterns (basic validation)
    // Avoid shell injection patterns
    if (strstr(buffer, "$(") || strstr(buffer, "`") || strstr(buffer, "&&") || 
        strstr(buffer, "||") || strstr(buffer, ";")) {
        glog_warning("Suspicious pattern detected in buffer\n");
        return -2;
    }
    
    return 0;
}