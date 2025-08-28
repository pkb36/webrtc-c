#include <stdio.h>
#include <stdlib.h>
#include "thread_safe_queue.h"
#include "nvds_process.h"

void queue_init(ThreadSafeQueue* q) {
    q->head = NULL;
    q->tail = NULL;
    q->active = 1;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void queue_enqueue(ThreadSafeQueue* q, EventMessage data) {
    QueueNode* newNode = (QueueNode*)malloc(sizeof(QueueNode));
    if (!newNode) {
        perror("Failed to allocate memory for queue node");
        return;
    }
    newNode->data = data;
    newNode->next = NULL;

    pthread_mutex_lock(&q->mutex);
    if (q->tail) {
        q->tail->next = newNode;
    }
    q->tail = newNode;
    if (q->head == NULL) {
        q->head = newNode;
    }
    // 큐에 데이터가 추가되었음을 대기 중인 스레드에 알림
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

EventMessage queue_dequeue(ThreadSafeQueue* q) {
    pthread_mutex_lock(&q->mutex);
    // 큐가 비어있고 활성 상태이면, 데이터가 들어올 때까지 대기
    while (q->head == NULL && q->active) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    // 큐가 비활성화되고 비어있으면, 종료 신호를 반환
    if (!q->active && q->head == NULL) {
        pthread_mutex_unlock(&q->mutex);
        EventMessage exit_msg = {EVENT_EXIT, -1};
        return exit_msg;
    }
    
    // 큐에서 데이터 꺼내기
    QueueNode* temp = q->head;
    EventMessage data = temp->data;
    q->head = q->head->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mutex);

    free(temp);
    return data;
}

void queue_signal_shutdown(ThreadSafeQueue* q) {
    pthread_mutex_lock(&q->mutex);
    q->active = 0;
    // 대기 중인 모든 스레드를 깨워서 종료 처리
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void queue_destroy(ThreadSafeQueue* q) {
    // 큐에 남은 노드들 메모리 해제
    while (q->head) {
        QueueNode* temp = q->head;
        q->head = q->head->next;
        free(temp);
    }
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}