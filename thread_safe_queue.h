#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H

#include <pthread.h>

// 큐를 통해 전달될 메시지 구조체
// 기존의 전역 변수들을 이 구조체로 대체합니다.
typedef struct {
    int class_id;
    int camera_id;
    // 필요 시 타임스탬프 등 다른 정보 추가 가능
} EventMessage;

// 큐의 노드 구조체
typedef struct QueueNode {
    EventMessage data;
    struct QueueNode* next;
} QueueNode;

// 스레드 안전 큐 관리 구조체
typedef struct {
    QueueNode *head, *tail;     // 큐의 시작과 끝
    pthread_mutex_t mutex;      // 큐를 보호하기 위한 뮤텍스
    pthread_cond_t cond;        // Consumer 스레드를 효율적으로 대기시키기 위한 컨디션 변수
    int active;                 // 큐의 활성화 상태 (종료 시 사용)
} ThreadSafeQueue;

// 큐 함수 프로토타입 선언
void queue_init(ThreadSafeQueue* q);
void queue_enqueue(ThreadSafeQueue* q, EventMessage data);
EventMessage queue_dequeue(ThreadSafeQueue* q);
void queue_destroy(ThreadSafeQueue* q);
void queue_signal_shutdown(ThreadSafeQueue* q);

#endif // THREAD_SAFE_QUEUE_H