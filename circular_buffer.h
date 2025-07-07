#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <gst/gst.h>
#include <pthread.h>
#include <time.h>
#include "config.h"

// 설정 상수
#define NUM_CAMERAS 2
#define CIRCULAR_BUFFER_DURATION 120  // 2분
#define BUFFER_FPS 10
#define MAX_FRAMES (CIRCULAR_BUFFER_DURATION * BUFFER_FPS)
#define MAX_FRAME_SIZE (1024 * 1024)  // 1MB per frame

extern WebRTCConfig g_config;

typedef void (*EventSaveCallback)(int camera_id, int event_id, const char *filename, const char *http_path,
                                 gboolean success, double event_time, void *user_data);
// H.264 프레임 정보 구조체
typedef struct {
    guint8 *data;
    gsize size;
    gboolean is_keyframe;
    GstClockTime pts;
    double timestamp;
    int index;
    int camera_id;
} H264Frame;

// 순환 버퍼 구조체
typedef struct {
    H264Frame frames[MAX_FRAMES];
    int write_pos;
    int frame_count;
    int total_frames_written;
    size_t total_bytes;
    pthread_mutex_t mutex;
    gboolean initialized;
    int camera_id;
} H264CircularBuffer;

// 이벤트 저장 태스크 구조체
typedef struct {
    int camera_id;
    int event_id;
    double event_time;
    int before_sec;
    int after_sec;
    char filename[256];
    char http_path[256];
} SaveEventTask;

// 함수 선언
void init_all_circular_buffers(void);
void cleanup_all_circular_buffers(void);
void add_frame_to_buffer(GstBuffer *buffer, gboolean is_keyframe, int camera_id);
int extract_event_clip(int camera_id, double event_time, int before_sec, int after_sec, 
                      H264Frame **out_frames, int *out_frame_count);
void save_h264_clip(H264Frame *frames, int frame_count, const char *filename);
void save_mp4_clip(H264Frame *frames, int frame_count, const char *filename, const char *http_path, int event_id);
void on_event_detected(int camera_id, int class_id, double event_time);
void get_buffer_status(int camera_id, int *frame_count, double *duration, size_t *total_size);
void save_codec_data(int camera_id, GstCaps *caps);
void set_event_save_callback(EventSaveCallback callback, void *user_data);

#endif // CIRCULAR_BUFFER_H