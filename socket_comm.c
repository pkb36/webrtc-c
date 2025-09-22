// Client side implementation of UDP client-server model
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include "socket_comm.h"
#include "unified_log.h"
#include "globals.h"
#include "device_setting.h"
#include "curllib.h"
#include "config.h"
#include "nvds_process.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#define MAX_BUF_SIZE 4096

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifdef PTZ_SUPPORT
#include "ptz_control.h"

void wait_ptz_stop()
{
    while (!is_ptz_motion_stopped())
        sleep(1); // 1 sec sleep
}
#else
void wait_ptz_stop()
{
    return;
}
#endif

// static struct Queue queue;

void *process_socket_comm_input(void *arg)
{
    SOCKETINFO *pInfo = (SOCKETINFO *)arg;
    char buffer[MAX_BUF_SIZE];
    struct sockaddr_in readaddr;
    int n;
    socklen_t len;

    if (!pInfo) {
        glog_error("Invalid SOCKETINFO pointer\n");
        return NULL;
    }

    memset(&readaddr, 0, sizeof(readaddr));
    memset(buffer, 0, sizeof(buffer));
    // glog_trace("thread start %d , %d\n", pInfo->socketfd, pInfo->port);

    while (1)
    {
        len = sizeof(readaddr);
        n = recvfrom(pInfo->socketfd, (char *)buffer, MAX_BUF_SIZE - 1,
                     MSG_WAITALL, (struct sockaddr *)&readaddr, &len);

        // Check for receive errors
        if (n < 0) {
            glog_error("recvfrom error: %s\n", strerror(errno));
            continue;
        }
        
        // Ensure null termination
        if (n >= 0 && n < MAX_BUF_SIZE) {
            buffer[n] = '\0';
        } else {
            buffer[MAX_BUF_SIZE - 1] = '\0';
        }
        // 최적화: 데이터 수신 로그 제거 (성능상 불필요)
        if (strcmp(buffer, "EXIT") == 0)
        {
            break;
        }
        else if (strcmp(buffer, "CONNECT") == 0)
        {
            log_info("Client connected on port %d", pInfo->port);
            pInfo->clientaddr = readaddr;
            pInfo->connect = 1;
            continue;
        }

        if (pInfo->call_fun)
        {
            pInfo->call_fun(buffer, n, pInfo);
        }
    }

    log_info("Socket thread ended: port %d", pInfo->port);
    return 0;
}

SOCKETINFO *init_socket_comm_server(int port)
{
    int sockfd;
    struct sockaddr_in servaddr;

    // Creating socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        glog_error("socket creation failed");
        return NULL;
    }

    // Set SO_REUSEADDR option
    int reuseaddr = 1; // LJH, 20250319
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0)
    {
        glog_error("setsockopt SO_REUSEADDR failed");
        close(sockfd);
        return NULL;
    }

    // Set SO_REUSEPORT option (optional, depending on the platform)
    int reuseport = 1; // LJH, 20250319
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport)) < 0)
    {
        glog_error("setsockopt SO_REUSEPORT failed");
        close(sockfd);
        return NULL;
    }

    memset(&servaddr, 0, sizeof(servaddr));

    // Filling server information
    servaddr.sin_family = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    // Bind the socket with the server address
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        glog_error("bind failed");
        close(sockfd);
        return NULL;
    }

    SOCKETINFO *pSocketInfo = (SOCKETINFO *)malloc(sizeof(SOCKETINFO));
    pSocketInfo->socketfd = sockfd;
    pSocketInfo->port = port;

    pthread_t tid;
    pthread_create(&tid, NULL, process_socket_comm_input, (void *)pSocketInfo);
    pSocketInfo->tid = tid;
    pSocketInfo->call_fun = NULL;
    pSocketInfo->data = NULL;

    return pSocketInfo;
}

#if MINDULE_INCLUDE

#define ONE_SIDE_SECTOR_MAX_NUM 16
#define RESTORE_AUTO_PAN_TIME (300 * 1000) // 300 seconds
#define KOREAN
#define MAX_ITEM_NUM 20 // Define the maximum size of the queue

void close_socket(SOCKETINFO *socket)
{
    if (socket == NULL)
        return;
    close(socket->socketfd);

    glog_trace("close_socket \n ");
    int status;

    char szTemp[16] = "EXIT";
    send_data_socket_comm(socket, szTemp, 5, 1);

    pthread_join(socket->tid, (void **)&status);
    if (socket->tid)
    {
        pthread_kill(socket->tid, 0);
    }

    free(socket);
}

static gboolean restore_auto_pan(gpointer user_data)
{
    glog_trace("restore_auto_pan: g_setting.analysis_status=%d\n", g_setting.analysis_status);
    // if (g_setting.analysis_status == FALSE)   //if not doing analysis already
    //   return G_SOURCE_REMOVE;                 //this removes source id

    glog_trace("restore set_process_analysis(%d)\n", g_setting.analysis_status);
    set_process_analysis(g_setting.analysis_status);
    init_auto_pan();

    return G_SOURCE_REMOVE; // this removes source id
}

void write_position(char side, int index, char *id_str)
{
    char side_str[10] = "";

    if (side == 'L')
    {
        strcpy(side_str, "좌");
    }
    else if (side == 'R')
    {
        strcpy(side_str, "우");
    }
    CurlIinfoType* curlinfo = get_curl_info();
    memset(curlinfo->position, 0, sizeof(curlinfo->position));
#ifdef KOREAN
    snprintf(curlinfo->position, sizeof(curlinfo->position), "위치: %s-%d, 소ID: %s", side_str, index + 1, id_str);
#else
    snprintf(curlinfo->position, sizeof(curlinfo->position), "Position: %s-%d, Cattle ID: %s", side_str, index, id_str);
#endif
    glog_trace("side=%c, index=%d, id_str=%s\n", side, index, id_str);
}

// Define a structure for the item
struct Item
{
    int index;
    char pos_str[20];
    char id_str[20];
};

// Define a structure for the queue that holds item items
struct Queue
{
    struct Item items[MAX_ITEM_NUM];
    int front;
    int rear;
};

// Thread-safe queue with mutex protection
static struct Queue queue;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to initialize the queue (thread-safe)
void initialize_queue(struct Queue *q)
{
    pthread_mutex_lock(&queue_mutex);
    q->front = -1;
    q->rear = -1;
    pthread_mutex_unlock(&queue_mutex);
}

// Function to check if the queue is full (thread-safe)
int is_queue_full(struct Queue *q)
{
    pthread_mutex_lock(&queue_mutex);
    int result = (q->rear == MAX_ITEM_NUM - 1);
    pthread_mutex_unlock(&queue_mutex);
    return result;
}

// Function to check if the queue is empty (thread-safe)
int is_queue_empty(struct Queue *q)
{
    pthread_mutex_lock(&queue_mutex);
    int result = (q->front == -1 || q->front > q->rear);
    pthread_mutex_unlock(&queue_mutex);
    return result;
}

// Function to add a item to the queue (thread-safe enqueue)
void enqueue(struct Queue *q, int index, const char *pos_str, const char *id_str)
{
    if (!q || !pos_str || !id_str) {
        glog_error("Invalid parameters to enqueue\n");
        return;
    }
    
    pthread_mutex_lock(&queue_mutex);
    
    if (q->rear == MAX_ITEM_NUM - 1)
    {
        pthread_mutex_unlock(&queue_mutex);
        glog_trace("Queue is full! Cannot enqueue index=%d,pos_str=%s,id_str=%s\n", index, pos_str, id_str);
        return;
    }
    
    if (q->front == -1)
    {
        q->front = 0; // First item in the queue
    }
    q->rear++;
    q->items[q->rear].index = index;
    
    // Safe string copy with bounds checking
    strncpy(q->items[q->rear].pos_str, pos_str, sizeof(q->items[q->rear].pos_str) - 1);
    q->items[q->rear].pos_str[sizeof(q->items[q->rear].pos_str) - 1] = '\0';
    
    strncpy(q->items[q->rear].id_str, id_str, sizeof(q->items[q->rear].id_str) - 1);
    q->items[q->rear].id_str[sizeof(q->items[q->rear].id_str) - 1] = '\0';
    
    pthread_mutex_unlock(&queue_mutex);
    glog_trace("Enqueued item: index=%d,pos=%s,ID=%s\n", index, pos_str, id_str);
}

// Function to remove a item from the queue (thread-safe dequeue)
struct Item dequeue(struct Queue *q)
{
    struct Item emptyItem = {-1, "", ""}; // Return an empty item if queue is empty
    
    if (!q) {
        glog_error("Invalid queue pointer in dequeue\n");
        return emptyItem;
    }

    pthread_mutex_lock(&queue_mutex);
    
    if (q->front == -1 || q->front > q->rear)
    {
        pthread_mutex_unlock(&queue_mutex);
        // 최적화: 정상 작동에서는 로그 최소화
        return emptyItem; // Return an empty item
    }
    
    struct Item dequeuedItem = q->items[q->front];
    q->front++;
    if (q->front > q->rear)
    {
        q->front = q->rear = -1; // Reset the queue when it's empty
    }
    
    pthread_mutex_unlock(&queue_mutex);
    // 최적화: 대기열 아이템 로그 제거
    return dequeuedItem;
}

// Function to display the students in the queue
void display(struct Queue *q)
{
    if (is_queue_empty(q))
    {
        // 최적화: 비어있는 대기열 로그 제거
    }
    else
    {
        printf("Queue: \n");
        for (int i = q->front; i <= q->rear; i++)
        {
            printf("index=%d,pos=%s,ID=%s\n", q->items[i].index, q->items[i].pos_str, q->items[i].id_str);
        }
    }
}

void move_ptz_and_send_event(int index, char *pos_str, char *id_str)
{
    static guint timer_id = 0;
    int temp_index = index;

    if (pos_str[0] == 'R')
        temp_index += ONE_SIDE_SECTOR_MAX_NUM;
    if (move_ranch_pos(temp_index) == 0)
    {
        wait_ptz_stop();
        glog_trace("index=%d,pos_str[0]=%c,id_str=%s\n", index, pos_str[0], id_str);
        write_position(pos_str[0], index, id_str);

#if (!MINDULE_BLOCK_NOTIFICATION)
        g_event_class_id = CLASS_HEAT_COW;
#else
        glog_trace("blocked notification by MINDULE\n");
#endif

        set_process_analysis(1); // turn on analysis
        glog_trace("set_process_analysis(1)\n");

        if (timer_id > 0)
        {
            g_source_remove(timer_id);
            timer_id = 0;
        }
        timer_id = g_timeout_add((guint)RESTORE_AUTO_PAN_TIME, restore_auto_pan, NULL);
    }
}

void *process_dequeue(void *arg)
{
    struct Item item;

    while (1)
    {
        if (!is_queue_empty(&queue) && !is_event_recording())
        { // if thread is not running
            item = dequeue(&queue);
            if (item.index != -1)
            {
                glog_trace("move_ptz_and_send_event(index=%d,pos_str=%s,id_str=%s)\n", item.index, item.pos_str, item.id_str);
                move_ptz_and_send_event(item.index, item.pos_str, item.id_str);
            }
        }
        sleep(1);
    }

    return NULL;
}

void preprocess_event(int index, char *pos_str, char *id_str)
{
    static int first = 1;
    static pthread_t process_dequeue_tid = 0;

    if (first)
    {
        first = 0;
        initialize_queue(&queue);
    }
    if (is_event_recording())
    { // if recording is on process
        enqueue(&queue, index, pos_str, id_str);
        if (process_dequeue_tid == 0)
            pthread_create(&process_dequeue_tid, NULL, process_dequeue, NULL);
    }
    else
    {
        move_ptz_and_send_event(index, pos_str, id_str); // LJH, 241210
    }
}

void get_substrings(char *str, char *substr1, char *substr2, char *seperator)
{
    // Temporary pointer to handle tokenization
    char *token;

    // Get the first substring before the semicolon
    token = strtok(str, seperator);
    if (token != NULL)
    {
        strcpy(substr1, token); // Copy the first substring to substr1
    }
    else
    {
        substr1[0] = '\0'; // If no substring before semicolon, set empty string
    }

    // Get the second substring after the semicolon
    token = strtok(NULL, seperator);
    if (token != NULL)
    {
        strcpy(substr2, token); // Copy the second substring to substr2
    }
    else
    {
        substr2[0] = '\0'; // If no substring after semicolon, set empty string
    }
}

void process_data(char *buf, int len, void *arg) // send message to webrtc
{
    if (!buf || len <= 0 || len > MAX_BUF_SIZE) {
        glog_error("Invalid parameters in process_data: buf=%p, len=%d\n", buf, len);
        return;
    }
    
    int index = 0;
    char pos_str[20] = {0};
    char id_str[20] = {0};
    
    // Ensure null termination
    char safe_buf[MAX_BUF_SIZE];
    strncpy(safe_buf, buf, MIN(len, MAX_BUF_SIZE - 1));
    safe_buf[MIN(len, MAX_BUF_SIZE - 1)] = '\0';

    get_substrings(safe_buf, pos_str, id_str, ";");
    glog_trace("buf=%s, pos_str=%s, id_str=%s\n", safe_buf, pos_str, id_str);
    
    if (pos_str[0] == 'L' || pos_str[0] == 'R')
    {
        // Safe string to integer conversion
        char *endptr;
        long temp_index = strtol(&pos_str[1], &endptr, 10);
        
        if (endptr == &pos_str[1] || *endptr != '\0') {
            glog_error("Invalid index format in pos_str: %s\n", pos_str);
            return;
        }
        
        index = (int)temp_index - 1;
        if (index < 0 || index >= ONE_SIDE_SECTOR_MAX_NUM)
        {
            glog_error("Index [%d] out of range (0-%d)\n", index, ONE_SIDE_SECTOR_MAX_NUM-1);
            return;
        }
        preprocess_event(index, pos_str, id_str); // LJH, 241210
    }
    else
    {
        glog_warning("Not received left or right side info: pos_str=%s\n", pos_str);
    }
}

void *receive_data(void *arg)
{
    SOCKETINFO *pInfo = (SOCKETINFO *)arg;
    
    if (!pInfo) {
        glog_error("Invalid SOCKETINFO pointer in receive_data\n");
        return NULL;
    }
    
    char buffer[MAX_BUF_SIZE];
    struct sockaddr_in readaddr;
    int n;
    socklen_t len;

    memset(&readaddr, 0, sizeof(readaddr));
    memset(buffer, 0, sizeof(buffer));
    log_info("Socket thread started: port %d", pInfo->port);

    while (1)
    {
        len = sizeof(readaddr);
        n = recvfrom(pInfo->socketfd, (char *)buffer, MAX_BUF_SIZE - 1, MSG_WAITALL, (struct sockaddr *)&readaddr, &len);
        
        if (n < 0) {
            log_error("Socket receive error on port %d: %s", pInfo->port, strerror(errno));
            break;
        }
        
        // Ensure null termination
        if (n >= 0 && n < MAX_BUF_SIZE) {
            buffer[n] = '\0';
        } else {
            buffer[MAX_BUF_SIZE - 1] = '\0';
        }
        
        // 최적화: 수신 데이터 로그 제거 (성능상 불필요)
        if (strcmp(buffer, "EXIT") == 0)
        {
            break;
        }
        if (pInfo->call_fun)
        {
            pInfo->call_fun(buffer, n, pInfo);
        }
    }

    log_info("Socket thread ended: port %d", pInfo->port);
    return 0;
}

SOCKETINFO *init_socket_server(int port, void *(*func_ptr)(void *), void (*call_fun)(char *ptr, int len, void *arg))
{
    int sockfd;
    struct sockaddr_in servaddr;

    // Creating socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        glog_error("socket creation failed");
        return NULL;
    }

    memset(&servaddr, 0, sizeof(servaddr));

    // Filling server information
    servaddr.sin_family = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    // Bind the socket with the server address
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        glog_error("bind failed");
        return NULL;
    }

    SOCKETINFO *pSocketInfo = (SOCKETINFO *)malloc(sizeof(SOCKETINFO));
    pSocketInfo->socketfd = sockfd;
    pSocketInfo->port = port;

    pthread_t tid;
    pthread_create(&tid, NULL, func_ptr, (void *)pSocketInfo);
    pSocketInfo->tid = tid;
    pSocketInfo->call_fun = call_fun;
    pSocketInfo->data = NULL;
    glog_trace("init_socket_server port %d is running\n", port);

    return pSocketInfo;
}
#endif // end of MINDULE_INCLUDE

SOCKETINFO *init_socket_comm_client(int port)
{
    int sockfd;

    // Creating socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        glog_error("socket creation failed");
        return NULL;
    }

    SOCKETINFO *pSocketInfo = (SOCKETINFO *)malloc(sizeof(SOCKETINFO));
    
    // 먼저 구조체 초기화
    pSocketInfo->socketfd = sockfd;
    pSocketInfo->port = port;
    pSocketInfo->call_fun = NULL;
    pSocketInfo->data = NULL;
    pSocketInfo->connect = 0;  // 이것도 초기화 필요
    
    // 그 다음에 스레드 시작
    pthread_t tid;
    pthread_create(&tid, NULL, process_socket_comm_input, (void *)pSocketInfo);
    pSocketInfo->tid = tid;

    return pSocketInfo;
}

int send_data_socket_comm(SOCKETINFO *socket, const char *data, int len, int is_self)
{
    struct sockaddr_in sendaddr;
    memset(&sendaddr, 0, sizeof(sendaddr));

    // Filling server information
    if (is_self)
    {
        sendaddr.sin_family = AF_INET;
        sendaddr.sin_port = htons(socket->port);
        sendaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    else
    {
        sendaddr = socket->clientaddr;
    }

    int n = sendto(socket->socketfd, data, len,
                   MSG_CONFIRM, (const struct sockaddr *)&sendaddr,
                   sizeof(sendaddr));

    return n;
}

void close_socket_comm(SOCKETINFO *socket)
{
    if (socket == NULL)
        return;
    
    glog_trace("close_socket_comm\n");
    
    // 1. EXIT 전송
    char szTemp[16] = "EXIT";
    send_data_socket_comm(socket, szTemp, 5, 1);
    usleep(100000);  // 100ms 대기
    
    // 2. 스레드 강제 종료 (pthread_join 대신)
    pthread_cancel(socket->tid);
    
    // 3. 소켓 닫기
    close(socket->socketfd);
    
    // 4. 메모리 해제
    free(socket);
    
    glog_trace("close_socket_comm completed\n");
}
