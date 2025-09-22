#ifndef __GLOBALS_H__
#define __GLOBALS_H__

#include <pthread.h>
#include <gst/gst.h>
#include "config.h"
#include "device_setting.h"
#include "curllib.h"
#include "gstream_main.h"
#include "nvds_process.h"
#include "nvds_utils.h"
#include "json_utils.h"
#include "command_handler.h"

/*
 * Global Variables - centralized extern declarations
 * Moved from scattered .c files to improve maintainability
 */

// Core configuration and state
extern WebRTCConfig g_config;
extern DeviceSetting g_setting;
extern CurlIinfoType g_curlinfo;
extern GstElement *g_pipeline;
extern enum AppState g_app_state;

// Thread synchronization
extern pthread_mutex_t g_send_mutex;
extern pthread_mutex_t g_process_msg_mutex;
extern pthread_mutex_t g_send_info_mutex;
extern pthread_mutex_t g_retry_connect_mutex;
extern pthread_mutex_t g_motion_mutex;

// PTZ and camera control
extern int g_source_cam_idx;
extern int g_move_speed;
extern int g_preset_index;
extern int g_no_zoom;
extern int ptz_err_code;
extern int g_wait_reply_cnt;
extern int stop_retry_count;
extern int g_event_recording;
extern int g_move_ptz_condition;

// Object detection and tracking
extern CowTrackingState g_cow_tracking_state;
extern int g_event_class_id;
extern ObjState object_state;
extern int g_top, g_left, g_width, g_height;
extern int g_move_to_center_running;
extern Timer timers[];
extern int g_frame_count[];
extern PersonObj person_objects[];

// Thresholds and analysis
extern float threshold_confidence[];
extern int threshold_event_duration[];
extern noti_queue notification_queue;
extern ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];
extern unsigned char g_init_pos_data[20];

#if MINDULE_INCLUDE
extern RanchSetting g_ranch_setting;
#endif

/*
 * Global Functions - centralized extern declarations
 */

// Core system functions
extern void set_tracker_analysis(gboolean OnOff);
extern void terminate_program();
extern gboolean cleanup_and_retry_connect(const gchar *msg, enum AppState state);
extern void send_camera_info_to_server();
extern int is_process_running(const char *process_name);
extern int get_temp(int index);
extern void dec_temp_event_time_gap();

// Socket and communication
extern void *receive_data(void *arg);
extern SOCKETINFO *init_socket_server(int port, void *(*func_ptr)(void *), void (*process_data)(char *ptr, int len, void *arg));
extern void process_data(char *buffer, int len, void *arg);
extern char *get_global_ip_with_timeout();
extern void handle_custom_command(gJSONObj *jsonObj, send_message_func_t send_func);
extern int get_service_address(char *address);

// PTZ control functions  
extern gboolean move_and_stop_ptz(int direction, int ptz_speed, int ptz_delay);
extern void wait_ptz_stop();
extern gboolean is_ptz_motion_stopped();
extern int move_ranch_pos(int index);
extern void set_process_analysis(gboolean OnOff);
extern void init_auto_pan();
extern int is_event_recording();

// Object detection and processing
extern void init_objects(PersonObj object[]);
extern void set_person_obj_state(PersonObj object[], NvDsObjectMeta *obj_meta);
extern ObjState track_object(ObjState object_state, PersonObj object[]);
extern void remove_newlines(char *str);
extern void init_obj_info();
extern void add_value_and_calculate_avg(ObjMonitor* obj, int new_value);
extern double calculate_sqrt(double width, double height);
extern int check_process(int port);

// Utility functions
extern int read_cmd_timeout(unsigned char* cmd_data, int cmd_len, unsigned char* read_data, int read_len, int timeout);
extern unsigned char get_checksum(unsigned char *data, int len);
extern bool check_time_gap(int timer_id);
extern void init_timer(int timer_id, int time_gap);
extern void enqueue_noti(noti_queue *q, char *cam_id, int cam_idx, int class_id, CurlIinfoType *curlinfo);
extern noti_item dequeue_noti(noti_queue *q);
extern int is_queue_empty_noti(noti_queue *q);
extern void cam_angle_change();
extern void print_serial_data(unsigned char *data, int len);

#if MINDULE_INCLUDE
extern gboolean load_ranch_setting(const char *file_name, RanchSetting *setting);
extern void get_ranch_setting_path(char *fname);
#endif

// Legacy log system functions (old_log_system)
extern int get_time(char *time, int max_len);
extern gboolean is_file(const char *fname);
extern gboolean is_dir(const char *d);

/*
 * Phase 2: Safe Getter/Setter Functions
 * These provide controlled access to global variables with validation
 */

// Core configuration getters
WebRTCConfig* get_config(void);
DeviceSetting* get_device_setting(void);  
CurlIinfoType* get_curl_info(void);

// Pipeline and state
GstElement* get_pipeline(void);
enum AppState get_app_state(void);
void set_app_state(enum AppState state);

// Camera and PTZ control
int get_source_cam_idx(void);
void set_source_cam_idx(int idx);
int get_move_speed(void);
void set_move_speed(int speed);
int get_preset_index(void);
void set_preset_index(int index);

// Event recording
gboolean is_event_recording_active(void);
void set_event_recording(int recording);

// Thread safety helpers
pthread_mutex_t* get_send_mutex(void);
pthread_mutex_t* get_send_info_mutex(void);
pthread_mutex_t* get_process_msg_mutex(void);
pthread_mutex_t* get_retry_connect_mutex(void);
pthread_mutex_t* get_motion_mutex(void);

// Notification and monitoring
noti_queue* get_notification_queue(void);
ObjMonitor* get_obj_monitor(int cam_idx, int obj_idx);

// Threshold management
float get_threshold_confidence(int class_id);
void set_threshold_confidence(int class_id, float confidence);
int get_threshold_duration(int class_id);
void set_threshold_duration(int class_id, int duration);

#endif /* __GLOBALS_H__ */