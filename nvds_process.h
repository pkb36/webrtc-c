#ifndef __NVDS_PROCESS_H__
#define __NVDS_PROCESS_H__

#include <gst/gst.h>
#include "global_define.h"
#include "device_setting.h"
#include "curllib.h"
#include "config.h"
#include "webrtc_peer.h"
#include "serial_comm.h"
#include "gstream_main.h"
#include "json_utils.h"
#include "ptz_control.h"
#include "gstnvdsmeta.h"
#include "global_define.h"

#define EVENT_EXIT                            9999
#define CENTER_X                              (1280/2)
#define CENTER_Y                              (720/2)

#define PER_CAM_SEC_FRAME                     10
#define BUFFER_SIZE                           4
#define XY_DIVISOR                            4
#define SMALL_BBOX_DIAGONAL                   (160.0)

#define THRESHOLD_OVER_OPTICAL_FLOW_COUNT     2
#define THRESHOLD_BBOX_MOVE                   30
#define THRESHOLD_RECT_SIZE_CHANGE            30
#define THRESHOLD_UNDER_TEMP_DEFAULT          15
#define THRESHOLD_UPPER_TEMP_DEFAULT          50
#define THRESHOLD_NORMAL                      (0.2)
#define HEAT_COUNT_THRESHOLD                  1

#define CLASSES_NUM                           5

// TCP 통신 관련 정의
#define API_SERVER_PORT 8888
#define EVENT_TCP_PORT 9999
#define TCP_BUFFER_SIZE 4096

#if CLASSES_NUM == 5
enum 
{
  CLASS_NORMAL_COW = 0, 
  CLASS_HEAT_COW, 
  CLASS_FLIP_COW, 
  CLASS_LABOR_SIGN_COW, 
  CLASS_NORMAL_COW_SITTING,
  CLASS_OVER_TEMP,
  NUM_CLASSES
};

#elif CLASSES_NUM == 2
#elif CLASSES_NUM == 3
enum 
{
  CLASS_NORMAL_COW = 0, 
  CLASS_FLIP_COW = 1, 
  CLASS_NORMAL_COW_SITTING = 2,
  CLASS_HEAT_COW = 3,             //don't care
  CLASS_LABOR_SIGN_COW = 4,       //don't care
  CLASS_OVER_TEMP = 5,            //don't care
  NUM_CLASSES = 6                 //this should be 6
};

#else
#endif

enum 
{
  LEFT = 0, 
  RIGHT, 
  TOP, 
  BOTTOM
};


#define TEMP_EVENT_TIME_GAP        300


// Struct to hold information for each timer
typedef struct {
  int call_count;         // Number of times the timer has been called
  time_t last_call_time;  // Time of the last call
  int time_gap;           // Time gap (in seconds)
} Timer;


// Structure to calculate the avg of the last 10 values
typedef struct {
  int buffer[BUFFER_SIZE];  // Circular buffer to store the last 10 values
  int index;                // Current index where the next value will be inserted
  int count;                // Number of values added (up to 10)
  int sum;                  // Sum of the values in the buffer
} AvgCalculator;


typedef struct {
  int detected_frame_count;
  int duration;
  int class_id;
  float confidence;
  int notification_flag;

  int do_opt_flow;
  int x, y, width, height;
  int center_x, center_y, corrected;
  int prev_x, prev_y, prev_width, prev_height;
  double diagonal;
  double move_size_avg;
  int opt_flow_check_count;
  int bbox_temp;
  int temp_duration;
  int opt_flow_detected_count;
  AvgCalculator temp_avg_calculator; // Embedded temp AvgCalculator structure for each object
  int heat_count;
  int temp_event_time_gap;
} ObjMonitor;


typedef enum {
  PRE_INIT_STATE,
  INIT_STATE,
  TRACKED_STATE,
  OUT_STATE,
} ObjState;

typedef struct {
  int top;
  int left;
  int width;
  int height;
  int detected;
  int undected_count;
  int distance;
} PersonObj;


enum {
  GREEN_COLOR = 0,
  YELLO_COLOR,
  RED_COLOR,
  BLUE_COLOR,
  NO_BBOX,
};

extern int g_event_class_id;
extern CurlIinfoType g_curlinfo;
extern GstElement *g_pipeline;
extern WebRTCConfig g_config;
extern DeviceSetting g_setting;
extern int g_source_cam_idx;
extern int g_move_speed;
extern int g_preset_index;
extern ObjState object_state;
extern int g_top, g_left, g_width, g_height;
extern int g_move_to_center_running;
extern Timer timers[];
extern gboolean move_and_stop_ptz(int direction, int ptz_speed, int ptz_delay);
extern int g_frame_count[];
extern enum AppState g_app_state;

extern void add_value_and_calculate_avg(ObjMonitor* obj, int new_value);
extern double calculate_sqrt(double width, double height);
extern int check_process(int port);
extern gboolean move_and_stop_ptz(int direction, int ptz_speed, int ptz_delay);
extern void wait_ptz_stop();
extern gboolean is_ptz_motion_stopped();
extern void init_objects(PersonObj object[]);
extern void set_person_obj_state(PersonObj object[], NvDsObjectMeta *obj_meta);
extern ObjState track_object(ObjState object_state, PersonObj object[]);
extern void remove_newlines(char *str);
extern void init_obj_info();

void set_process_analysis(gboolean OnOff);
void setup_nv_analysis();
void endup_nv_analysis();
void check_events_for_notification(int cam_idx, int init);
int get_opt_flow_object(int cam_idx, int obj_id);
void set_custom_label(NvDsObjectMeta *obj_meta, NvDsFrameMeta *frame_meta, 
                      NvDsBatchMeta *batch_meta, int cam_idx, int temp_display);
gboolean send_event_to_recorder_simple(int class_id, int camera_id);

BboxColor get_object_color(guint camera_id, guint object_id, gint class_id);
int send_notification_to_server(int class_id);

#endif
