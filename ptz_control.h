#ifndef __PTZ_CONTROL_H__
#define __PTZ_CONTROL_H__

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <pthread.h>  // pthread_t, pthread_mutex_t를 위해 추가
#include <glib.h>     // gboolean을 위해 추가
#include "global_define.h"

#define MAX_PTZ_PRESET  12
#define PTZ_POS_SIZE    11
#define MAX_RANCH_POS   32

#define PAN_TILT_TOLERANCE  10   // Pan/Tilt 허용 오차
#define ZOOM_TOLERANCE      5    // Zoom 허용 오차
#define FOCUS_TOLERANCE     5    // Focus 허용 오차

enum {
  PTZ_NORMAL = 0,
  PTZ_STOP_FAILED,
};

// Auto PTZ 상태 구조체
typedef struct {
    gboolean is_running;
    gboolean is_paused;
    int current_index;
    int total_presets;
    int stay_time_sec;
    unsigned char sequence[MAX_PTZ_PRESET];
    pthread_t thread_id;
    pthread_mutex_t mutex;
} AutoPTZState;

// PTZ 에러 코드
typedef enum {
    PTZ_SUCCESS = 0,
    PTZ_ERROR_SERIAL_NOT_OPEN = -1,
    PTZ_ERROR_INVALID_SEQUENCE = -2,
    PTZ_ERROR_PRESET_NOT_SET = -3,
    PTZ_ERROR_THREAD_CREATE = -4,
    PTZ_ERROR_ALREADY_RUNNING = -5
} PTZErrorCode;

typedef struct {
    unsigned short pan;
    unsigned short tilt;
    unsigned short zoom;
    unsigned short focus;
    unsigned short iris;
} PTZPosition;

/* PTZ Data Format
 [Flag][PTZ_DATA]
 Flag => 0,1 check data setting..
*/

// 기존 함수들
int update_ptz_pos(int index, unsigned char* pos, int auto_ptz_mode);
int is_work_auto_ptz();
void request_auto_move_ptz_stop();

void set_ptz_move_speed(int move, int auto_ptz);
int set_ptz_pos(int id, unsigned char* read_data, int auto_ptz_mode);
int move_ptz_pos(int index, int auto_ptz_move);
int auto_move_ptz(const char* move_seq);
int get_pt_status();

void send_ptz_move_serial_data(const char *s);
gboolean send_ptz_move_cmd(int direction, int ptz_speed);
gboolean move_and_stop_ptz(int direction, int ptz_speed, int ptz_delay);
gboolean is_ptz_motion_stopped();

// Ranch 관련 함수들 (조건부 컴파일)
#if MINDULE_INCLUDE
int update_ranch_pos(int index, unsigned char* ptz_pos, int set_usage);
int set_ranch_pos(int index, unsigned char* read_data);
int move_ranch_pos(int index);
gboolean update_ranch_setting(const char *file_name, RanchSetting* setting);
#endif

// 새로 추가된 함수들
PTZErrorCode validate_auto_ptz_sequence(const char* move_seq, unsigned char* data, int* data_len);
const char* get_ptz_error_string(PTZErrorCode code);
void pause_auto_ptz(void);
void resume_auto_ptz(void);
void stop_auto_ptz(void);

int get_current_position(PTZPosition* pos);
void parse_target_position(unsigned char* ptz_data, PTZPosition* pos);
gboolean is_position_reached(PTZPosition* current, PTZPosition* target, gboolean check_zoom);
gboolean is_ptz_motion_stopped_with_position_check(PTZPosition* target_pos);
int wait_for_ptz_completion(int preset_index, int timeout_sec);
// 전역 변수
extern int ptz_err_code;
extern int g_move_speed;
extern int g_preset_index;
extern int g_no_zoom;
extern pthread_mutex_t g_motion_mutex;

#endif /* __PTZ_CONTROL_H__ */