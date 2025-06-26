#ifndef NVDS_UTILS_H
#define NVDS_UTILS_H

#include <gst/gst.h>
#include <pthread.h>
#include <math.h>
#include "nvds_process.h"

#define MAX_DISTANCE    (1024 + 720)

#define   MAX_NOTI_ITEM_NUM        100

// Define a structure for the item
typedef struct noti_item {
  int cam_idx;			//source cam index
  int class_id;
  char cam_id[64];
  CurlIinfoType curlinfo;
} noti_item;

// Define a structure for the queue that holds item items
typedef struct noti_queue {
  struct noti_item items[MAX_NOTI_ITEM_NUM];
  int front;
  int rear;
} noti_queue;

extern int read_cmd_timeout(unsigned char* cmd_data, int cmd_len, unsigned char* read_data, int read_len, int timeout);
extern unsigned char get_checksum(unsigned char *data, int len);
extern unsigned char g_init_pos_data[20];
extern bool check_time_gap(int timer_id);
extern void init_timer(int timer_id, int time_gap);
extern void enqueue_noti(noti_queue *q, char *cam_id, int cam_idx, int class_id, CurlIinfoType *curlinfo);
extern noti_item dequeue_noti(noti_queue *q);
extern int is_queue_empty_noti(noti_queue *q);
extern void cam_angle_change();

/* Function prototypes */
int is_process_running(const char *process_name);
void wait_recording_finish();
int is_send_eventer_running();
void unlock_sending_event();
void zoomin(int count);
int get_error_pan(int left, int width);
int get_error_tilt(int top, int height);
void *move_to_center(void *arg);
void check_for_zoomin(int total_rect_size, int obj_count);
void move_ptz_along(int top, int left, int width, int height);

extern noti_queue notification_queue;
extern ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];

#endif  // NVDS_UTILS_H
