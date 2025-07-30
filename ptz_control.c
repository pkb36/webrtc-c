/* For json config */
#include <stdio.h>
#include "serial_comm.h"
#include <pthread.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "log_wrapper.h"
#include <json-glib/json-glib.h>
#include "device_setting.h"
#include "serial_comm.h"
#include "nvds_process.h"

static AutoPTZState g_auto_ptz_state = {0};
static guint ptz_watchdog_timer = 0;
static time_t last_ptz_command_time = 0;

pthread_mutex_t g_motion_mutex;

// Format flag (On/Off), time(stay time), Pos count, [POS...]
static unsigned char PTZ_POS_LIST[MAX_PTZ_PRESET][PTZ_POS_SIZE]; // ptz_preset in json

static unsigned char AUTO_PTZ_MOVE_SEQ[MAX_PTZ_PRESET + 3] = {0};     // index of AUTO_PTZ_POS_LIST 의 list, +3 ==> 앞의 3개는 on/off, stay time, setting 된 AUTO_PTZ_POS_LIST 의 index list 의 count, 그 후로는 AUTO_PTZ_POS_LIST 의 index list
static unsigned char AUTO_PTZ_POS_LIST[MAX_PTZ_PRESET][PTZ_POS_SIZE]; // LJH, actual position, auto_ptz_preset in json

#if MINDULE_INCLUDE
static unsigned char RANCH_POS_LIST[MAX_RANCH_POS][PTZ_POS_SIZE]; // LJH, actual position, ranch_pos in json
#endif

static unsigned char auto_ptz_move_speed = 0x08;
static unsigned char ptz_move_speed = 0x10;

extern DeviceSetting g_setting;
extern int stop_retry_count;
extern int g_event_recording;

int g_no_zoom = 0;
int ptz_err_code = PTZ_NORMAL;
int g_move_speed;
int g_preset_index = 0;

static gboolean ptz_watchdog_callback(gpointer user_data)
{
  time_t current_time = time(NULL);

  // 마지막 명령 후 2초 이상 경과시 자동 정지
  if (g_move_speed > 0 && (current_time - last_ptz_command_time) > 2)
  {
    glog_info("PTZ watchdog: No command received for 2 seconds, stopping PTZ\n");
    send_ptz_move_cmd(0, 0); // 강제 정지
    g_move_speed = 0;
  }

  return G_SOURCE_CONTINUE;
}

const char *get_ptz_error_string(PTZErrorCode code)
{
  switch (code)
  {
  case PTZ_SUCCESS:
    return "Success";
  case PTZ_ERROR_SERIAL_NOT_OPEN:
    return "Serial port not open";
  case PTZ_ERROR_INVALID_SEQUENCE:
    return "Invalid sequence format";
  case PTZ_ERROR_PRESET_NOT_SET:
    return "Preset position not configured";
  case PTZ_ERROR_THREAD_CREATE:
    return "Failed to create thread";
  case PTZ_ERROR_ALREADY_RUNNING:
    return "Auto PTZ already running";
  default:
    return "Unknown error";
  }
}

// PTZ 명령 수신시 호출
void update_ptz_watchdog(void)
{
  last_ptz_command_time = time(NULL);

  // 워치독 타이머가 없으면 생성
  if (ptz_watchdog_timer == 0)
  {
    ptz_watchdog_timer = g_timeout_add(500, ptz_watchdog_callback, NULL);
  }
}

void set_ptz_move_speed(int move, int auto_ptz)
{
  auto_ptz_move_speed = auto_ptz;
  ptz_move_speed = move;
}

PTZErrorCode validate_auto_ptz_sequence(const char *move_seq, unsigned char *data, int *data_len)
{
  *data_len = parse_string_to_hex(move_seq, data, MAX_PTZ_PRESET + 8);

  if (*data_len < 4 || *data_len > MAX_PTZ_PRESET + 2)
  {
    glog_error("Invalid sequence length: %d (expected 4-%d)\n", *data_len, MAX_PTZ_PRESET + 2);
    return PTZ_ERROR_INVALID_SEQUENCE;
  }

  if (data[*data_len - 2] != 0xFF)
  {
    glog_error("Sequence end marker (0xFF) not found\n");
    return PTZ_ERROR_INVALID_SEQUENCE;
  }

  // 각 프리셋이 유효한지 검증
  for (int i = 0; i < *data_len - 2; i++)
  {
    if (data[i] >= MAX_PTZ_PRESET)
    {
      glog_error("Invalid preset index %d at position %d\n", data[i], i);
      return PTZ_ERROR_INVALID_SEQUENCE;
    }

    if (AUTO_PTZ_POS_LIST[data[i]][0] == 0)
    {
      glog_error("Preset %d not configured\n", data[i]);
      return PTZ_ERROR_PRESET_NOT_SET;
    }
  }

  return PTZ_SUCCESS;
}

int update_ptz_pos(int index, unsigned char *ptz_pos, int auto_ptz_mode)
{
  if (index >= MAX_PTZ_PRESET)
  {
    return -1;
  }
  if (auto_ptz_mode)
  {
    memcpy(&AUTO_PTZ_POS_LIST[index][1], ptz_pos, 10);
    AUTO_PTZ_POS_LIST[index][0] = 1;
  }
  else
  {
    memcpy(&PTZ_POS_LIST[index][1], ptz_pos, 10);
    PTZ_POS_LIST[index][0] = 1;
  }

  glog_trace("update_ptz_pos auto_ptz_mode[%d] : index[%d] ", auto_ptz_mode, index);
  print_serial_data(ptz_pos, 10);

  return 0;
}

unsigned char get_checksum(unsigned char *data, int len)
{
  unsigned char checksum = 0;

  for (int i = 0; i < len; i++)
    checksum += data[i];
  checksum %= 256;

  return checksum;
}

#if MINDULE_INCLUDE

#define RANCH_PTZ_SPEED 100

static gchar *ranch_setting_template_string_json = "{\n\
   \"ranch_pos\": [\n\
   %s\
   ]\n\
}";

gboolean update_ranch_setting(const char *file_name, RanchSetting *setting)
{
  // 1. make PTZ code
  char ptz_code[(MAX_RANCH_POS + 2) * PTZ_POS_SIZE * 4] = {0};
  char temp[8];

  for (int i = 0; i < MAX_RANCH_POS; i++)
  {
    sprintf(temp, "\"%02x", setting->ranch_pos[i][0]);
    strcat(ptz_code, temp);
    for (int j = 1; j < PTZ_POS_SIZE; j++)
    {
      sprintf(temp, ",%02x", setting->ranch_pos[i][j]);
      strcat(ptz_code, temp);
    }
    if (i < MAX_RANCH_POS - 1)
      strcat(ptz_code, "\",\n");
    else
      strcat(ptz_code, "\"\n");
  }

  // 2. update config
  FILE *fp = fopen(file_name, "w+t, ccs=UTF-8");
  if (fp == NULL)
  {
    glog_trace("fail open device setting  %s \n", file_name);
    return FALSE;
  }

  fprintf(fp, ranch_setting_template_string_json, ptz_code);
  fclose(fp);

  return TRUE;
}

int set_ranch_pos(int index, unsigned char *read_data)
{
  if (!is_open_serial())
  {
    glog_error("serial device not opened before\n");
    return -1;
  }
  request_auto_move_ptz_stop();

  unsigned char cmd_data[7] = {0x96, 0x00, 0x06, 0x01, 0x01, 0x01, 0x9F}; // LJH, command is 0x0106 = Get All Position
  int result = read_cmd_timeout(cmd_data, 7, read_data, 17, 1);           // LJH, response data length is 11, whole response size is 17
  if (result == -1)
  {
    glog_error("fail read set_ptz_pos \n");
    return result;
  }
  update_ranch_pos(index, &read_data[5], 1);
  return 0;
}

int update_ranch_pos(int index, unsigned char *ptz_pos, int set_usage)
{
  RANCH_POS_LIST[index][0] = set_usage;
  if (ptz_pos == NULL)
    return 0;

  memcpy(&RANCH_POS_LIST[index][1], ptz_pos, 10);
  glog_trace("update_ranch_pos index[%d] ", index);
  print_serial_data(ptz_pos, 10);

  return 0;
}

int move_ranch_pos(int index)
{
  if (!is_open_serial())
  {
    glog_error("serial device not opened before \n");
    return -1;
  }

  if (index >= MAX_RANCH_POS)
  {
    glog_error("PTZ index[%d] exceed Max[%d]\n", index, MAX_RANCH_POS);
    return -1;
  }

  int is_set_berfore = RANCH_POS_LIST[index][0];
  if (is_set_berfore == 0)
  {
    glog_error("RANCH PTZ POS index[%d] was not set before\n", index);
    return -1;
  }

  request_auto_move_ptz_stop();

  unsigned char send_data[32] = {0x96, 0x00, 0x01, 0x01, 0x0F}; // LJH, 0x0101 command require response, data length is 15
  // 2. set  move postion
  int move_speed = ptz_move_speed;
  memcpy(&send_data[5], &RANCH_POS_LIST[index][1], 10);
  move_speed = RANCH_PTZ_SPEED;

  // 3. speed 설정
  send_data[15] = send_data[16] = send_data[17] = send_data[18] = move_speed; // LJH, [15] is Pan Speed, [16] is Tilt Speed, [17] is Zoom Speed, [18] is Focus Speed
  send_data[19] = 0;
  send_data[20] = get_checksum(send_data, 20);
  glog_trace("ranch_ptz_move index[%d] move_speed[%d]\n", index, move_speed);

  unsigned char read_data[8];
  int result = read_cmd_timeout(send_data, 21, read_data, 7, 1);
  if (result == -1)
  {
    glog_error("fail read move_ptz_pos\n");
    return result;
  }

  return 0;
}

#endif

int set_ptz_pos(int id, unsigned char *read_data, int auto_ptz_mode)
{
  if (!is_open_serial())
  {
    glog_error("serial device not opened before \n");
    return -1;
  }
  request_auto_move_ptz_stop();
  unsigned char cmd_data[7] = {0x96, 0x00, 0x06, 0x01, 0x01, 0x01, 0x9F}; // LJH, command is 0x0106 = Get All Position
  int result = read_cmd_timeout(cmd_data, 7, read_data, 17, 1);           // LJH, response data length is 11, whole response size is 17
  if (result == -1)
  {
    glog_error("fail read set_ptz_pos \n");
    return result;
  }
  update_ptz_pos(id, &read_data[5], auto_ptz_mode);
  return 0;
}

int get_pt_status()
{
  unsigned char cmd_data[6] = {0x96, 0x00, 0x01, 0x04, 0x00, 0x9B}; // LJH, command is 0x0401 = Get PT State
  unsigned char read_data[9];

  int result = read_cmd_timeout(cmd_data, 6, read_data, 8, 1); // LJH, reponse date length is 2, whole response size is 8
  if (result == -1)
  {
    glog_error("fail read get_pt_status \n");
    return result;
  }
  return read_data[6];
}

int move_ptz_pos(int index, int auto_ptz_move)
{
  if (!is_open_serial())
  {
    glog_error("serial device not opened before \n");
    return -1;
  }

  if (index >= MAX_PTZ_PRESET)
  {
    glog_error("PTZ Preset [%d] exceed Max [%d]\n", index, MAX_PTZ_PRESET);
    return -1;
  }

  unsigned char *preset_data = auto_ptz_move ? &AUTO_PTZ_POS_LIST[index][1] : &PTZ_POS_LIST[index][1];

  int is_set_berfore = auto_ptz_move ? AUTO_PTZ_POS_LIST[index][0] : PTZ_POS_LIST[index][0];
  if (is_set_berfore == 0)
  {
    glog_error("PTZ Preset mode[%d]  Not set before index [%d]\n", auto_ptz_move, index);
    return -1;
  }

  // 목표 위치 파싱
  PTZPosition target_pos;
  parse_target_position(preset_data, &target_pos);

  // 현재 위치 확인
  PTZPosition current_pos;
  if (get_current_position(&current_pos) == 0)
  {
    // 이미 목표 위치에 있는지 확인
    if (is_position_reached(&current_pos, &target_pos, TRUE))
    {
      glog_trace("Already at target position\n");
      return 0;
    }
  }

  glog_trace("current position - Pan: %d, Tilt: %d, Zoom: %d, Focus: %d, Iris: %d\n",
             current_pos.pan, current_pos.tilt, current_pos.zoom, current_pos.focus, current_pos.iris);

  // 1. 현재의 포지션을 가져옴
  // 1. 현재 상태가 움직임 상태일 경우는 움직임을 멈춘다.
  if (!auto_ptz_move)
  {
    request_auto_move_ptz_stop();
    g_no_zoom = 0; // LJH
  }

  unsigned char send_data[32] = {0x96, 0x00, 0x01, 0x01, 0x0F}; // LJH, 0x0101 command require response, data length is 15
  // 2. set  move postion
  int move_speed = ptz_move_speed;
  if (auto_ptz_move)
  {
    memcpy(&send_data[5], &AUTO_PTZ_POS_LIST[index][1], 10);
    move_speed = auto_ptz_move_speed;
  }
  else
  {
    memcpy(&send_data[5], &PTZ_POS_LIST[index][1], 10);
  }

  // 3. speed 설정
  send_data[15] = send_data[16] = send_data[17] = send_data[18] = move_speed; // LJH, [15] is Pan Speed, [16] is Tilt Speed, [17] is Zoom Speed, [18] is Focus Speed
  if (g_no_zoom)
  {
    send_data[17] = 0; // LJH, set zoom speed as 0
  }
  send_data[19] = 0;

  // 4. CheckSum
  send_data[20] = get_checksum(send_data, 20);

  // glog_trace("move_ptz_pos auto_ptz_move[%d] index[%d]\n", auto_ptz_move, index);
  unsigned char read_data[8];
  int result = read_cmd_timeout(send_data, 21, read_data, 7, 1);
  if (result == -1)
  {
    glog_error("fail read move_ptz_pos \n");
    return result;
  }

  return 0;
}

#include <pthread.h>
static pthread_t g_tid;
int is_work_auto_ptz()
{
  return AUTO_PTZ_MOVE_SEQ[0];
}

void request_auto_move_ptz_stop()
{
  // wait thread endup
  if (AUTO_PTZ_MOVE_SEQ[0])
  {
    AUTO_PTZ_MOVE_SEQ[0] = 0;

    // 새로운 상태도 업데이트
    pthread_mutex_lock(&g_auto_ptz_state.mutex);
    g_auto_ptz_state.is_running = FALSE;
    pthread_mutex_unlock(&g_auto_ptz_state.mutex);

    pthread_join(g_tid, NULL);
  }
}

gboolean is_ptz_motion_stopped()
{
  static int first = 1;

  if (first)
  {
    first = 0;
    pthread_mutex_init(&g_motion_mutex, NULL);
  }

  pthread_mutex_lock(&g_motion_mutex);

  if (is_open_serial() == 0)
  {
    g_move_speed = 0;
    pthread_mutex_unlock(&g_motion_mutex);
    return TRUE;
  }

  int motion_status = get_pt_status();
  if ((motion_status & 0xF) == 0)
  {
    g_move_speed = 0;
    pthread_mutex_unlock(&g_motion_mutex);
    return TRUE;
  }

  pthread_mutex_unlock(&g_motion_mutex);
  return FALSE;
}

unsigned int get_auto_move_zoom_val(int index)
{
  unsigned int val = 0;

  val = AUTO_PTZ_POS_LIST[index][4];
  val += AUTO_PTZ_POS_LIST[index][5] * 0xFF;

  return val;
}

int is_zoom_out(int prev, int cur)
{
  if (prev > cur) // LJH, smaller zoom value means far distance from the object
    return 1;
  return 0;
}

// LJH, zoom first is default operation of RGB camera when doing auto ptz
void *process_auto_move_ptz(void *arg)
{
  static int index = 0, prev_index = 0;
  int sleep_100ms_cnt = AUTO_PTZ_MOVE_SEQ[1] * 10;
  int max_index = AUTO_PTZ_MOVE_SEQ[2];
  unsigned int cur_zoom_val = 0, prev_zoom_val = 0;
  int continue_tag = 0;

  while (AUTO_PTZ_MOVE_SEQ[0])
  {
    // 줌 처리 로직 (기존과 동일)
    if (continue_tag == 0)
    {
      prev_zoom_val = get_auto_move_zoom_val(AUTO_PTZ_MOVE_SEQ[prev_index + 3]);
      cur_zoom_val = get_auto_move_zoom_val(AUTO_PTZ_MOVE_SEQ[index + 3]);
      if (is_zoom_out(prev_zoom_val, cur_zoom_val))
        g_no_zoom = 1;
      else
        g_no_zoom = 0;
    }
    else
    {
      continue_tag = 0;
      g_no_zoom = 0;
    }

    // 프리셋 이동
    int current_preset = AUTO_PTZ_MOVE_SEQ[index + 3];
    move_ptz_pos(current_preset, 1);

    // AI 분석 OFF
    if (g_setting.analysis_status)
      set_process_analysis(0);

    // 위치 기반 이동 완료 대기 (최대 AUTO_PTZ_MOVE_SEQ[1] + 10초)
    int max_wait_time = AUTO_PTZ_MOVE_SEQ[1] + 10;
    int move_time = wait_for_ptz_completion(current_preset, max_wait_time);

    if (move_time < 0)
    {
      // 중단 신호를 받음
      index = (index + 1) % max_index;
      break;
    }

    glog_debug("PTZ move to preset %d (index %d) completed in %d seconds\n",
               current_preset, index, move_time);

    // 줌아웃시 분석 스킵 (기존 로직)
    if (g_no_zoom)
    {
      continue_tag = 1;
      continue;
    }
    else
    {
      continue_tag = 0;
    }

    g_preset_index = index; // 기존 변수 업데이트

    // AI 분석 ON
    if (g_setting.analysis_status)
      set_process_analysis(1);

    // 대기 시간 처리 (기존과 동일)
    for (int i = 0; i < sleep_100ms_cnt; i++)
    {
      if (AUTO_PTZ_MOVE_SEQ[0] == 0)
        break;
      usleep(ONE_TENTH_SEC);
    }

    // 이벤트 녹화 대기 (기존과 동일)
    for (int i = 0; i < 15; i++)
    {
      if (g_event_recording == 0)
        break;
      sleep(1);
    }

    if (AUTO_PTZ_MOVE_SEQ[0] == 0)
      break;

    // 인덱스 업데이트
    prev_index = index;
    index = (index + 1) % max_index;

    // 새로운 상태도 동기화
    pthread_mutex_lock(&g_auto_ptz_state.mutex);
    g_auto_ptz_state.current_index = index;
    pthread_mutex_unlock(&g_auto_ptz_state.mutex);
  }

  g_no_zoom = 0;
  glog_trace("end auto_move_ptz\n");

  return 0;
}

int auto_move_ptz(const char *move_seq)
{
  if (!is_open_serial())
  {
    glog_error("serial device not opened before \n");
    return -1;
  }

  unsigned char data[MAX_PTZ_PRESET + 8] = {0};
  int data_len = parse_string_to_hex(move_seq, data, MAX_PTZ_PRESET + 8);

  request_auto_move_ptz_stop(); // 기존 동작 유지

  // 데이터 검증
  if (data_len < 4 || data_len > MAX_PTZ_PRESET + 2)
  {
    glog_error("PTZ Auto Move Preset sequence not validate data_len [%d]\n", data_len);
    return -1;
  }

  if (data[data_len - 2] != 0xFF)
  {
    glog_error("PTZ Auto Move Preset sequence not validate, end mark not found \n");
    return -1;
  }

  // 프리셋 검증
  for (int i = 0; i < data_len - 2; i++)
  {
    if (data[i] >= MAX_PTZ_PRESET)
    {
      glog_error("PTZ Auto Move Preset sequence not validate %d, => %d \n", i, data[i]);
      return -1;
    }

    if (AUTO_PTZ_POS_LIST[data[i]][0] == 0)
    {
      glog_error("PTZ Auto Move Preset is not set: index value ==(data[%d]==%d)\n", i, data[i]);
      return -1;
    }
  }

  // 기존 전역 변수 설정 (호환성)
  AUTO_PTZ_MOVE_SEQ[0] = 1;
  AUTO_PTZ_MOVE_SEQ[1] = data[data_len - 1];
  AUTO_PTZ_MOVE_SEQ[2] = data_len - 2;
  for (int i = 0; i < data_len - 2; i++)
  {
    AUTO_PTZ_MOVE_SEQ[3 + i] = data[i];
  }

  // 새로운 상태 구조체도 업데이트
  pthread_mutex_lock(&g_auto_ptz_state.mutex);
  g_auto_ptz_state.is_running = TRUE;
  g_auto_ptz_state.is_paused = FALSE;
  g_auto_ptz_state.current_index = 0;
  g_auto_ptz_state.total_presets = data_len - 2;
  g_auto_ptz_state.stay_time_sec = data[data_len - 1];

  for (int i = 0; i < data_len - 2; i++)
  {
    g_auto_ptz_state.sequence[i] = data[i];
  }
  pthread_mutex_unlock(&g_auto_ptz_state.mutex);

  // 스레드 시작
  pthread_create(&g_tid, NULL, process_auto_move_ptz, NULL);

  return 0; // 기존과 동일한 반환값
}

gboolean send_ptz_move_cmd(int direction, int ptz_speed)
{
  if (is_open_serial() == 0)
  {
    glog_error("PTZ serial is not open\n");
    ptz_err_code = PTZ_NORMAL;
    stop_retry_count = 0;
    return FALSE;
  }

  if (g_cow_tracking_state.is_tracking && ptz_speed > 0)
  {
    // glog_info("Manual PTZ command blocked during auto tracking\n");
    return FALSE;
  }

  unsigned char read_data[7] = {0}, data[11];
  char direction_str[][12] = {"left", "right", "top", "bottom", "zoom in", "zoom out",
                              "top-left", "top-right", "bottom-left", "bottom-right"};
  int len = 0, size = sizeof(direction_str) / sizeof(direction_str[0]);

  if (size > direction && direction >= 0)
  {
    if (ptz_speed > 0)
    {
      glog_trace("Send PTZ UART command(Direction:%s,Speed:%d)\n", direction_str[direction], ptz_speed);
    }
    else
    {
      glog_trace("Send PTZ UART stop command\n");
    }
  }
  else
  {
    glog_error("PTZ direction(=%d) is out of range\n", direction);
    return FALSE;
  }

  if (ptz_speed > 0)
  {
    update_ptz_watchdog();
  }

  memset(data, 0x00, sizeof(data));

  data[len++] = 0x96; // Sync
  data[len++] = 0x00; // Addr
  data[len++] = 0x00; // CmdH

  if (ptz_speed > 0)
    data[len++] = 0x41; // CmdL: Does not require response
  else
    data[len++] = 0x01; // CmdL: Require response

  data[len++] = 0x05; // Data length

  if (ptz_speed > 0)
  {
    if (direction == 0)
    { // Left
      data[len++] = 0x40;
      data[len] = ptz_speed;
    }
    else if (direction == 1)
    { // Right
      data[len++] = 0x80;
      data[len] = ptz_speed;
    }
    else if (direction == 2)
    { // Top
      data[len++] = 0x10;
      data[len++] = 0x00;
      data[len] = ptz_speed;
    }
    else if (direction == 3)
    { // Bottom
      data[len++] = 0x20;
      data[len++] = 0x00;
      data[len] = ptz_speed;
    }
    else if (direction == 4)
    { // Zoom in
      data[len++] = 0x04;
      data[len++] = 0x00;
      data[len++] = 0x00;
      data[len] = ptz_speed;
    }
    else if (direction == 5)
    { // Zoom out
      data[len++] = 0x08;
      data[len++] = 0x00;
      data[len++] = 0x00;
      data[len] = ptz_speed;
    }
    else if (direction == 9) // Top-Left (대각선)
    {
      data[len++] = 0xA0; // 0x80 (Pan Left) | 0x20 (Tilt Up) = 0xA0
      data[len++] = ptz_speed;
      data[len] = ptz_speed;
    }
    else if (direction == 8) // Top-Right (대각선) 8
    {
      data[len++] = 0x60; // 0x40 (Pan Right) | 0x20 (Tilt Up) = 0x60
      data[len++] = ptz_speed;
      data[len] = ptz_speed;
    }
    else if (direction == 7) // Bottom-Left (대각선) 7
    {
      data[len++] = 0x90; // 0x80 (Pan Left) | 0x10 (Tilt Down) = 0x90
      data[len++] = ptz_speed;
      data[len] = ptz_speed;
    }
    else if (direction == 6) // Bottom-Right (대각선) 6
    {
      data[len++] = 0x50; // 0x40 (Pan Right) | 0x10 (Tilt Down) = 0x50
      data[len++] = ptz_speed;
      data[len] = ptz_speed;
    }
  }

  data[10] = get_checksum(data, 10);
  len = 11;

  if (ptz_speed > 0)
  { // if data is for move, if speed is not 0
    write_serial(data, len);
    ptz_err_code = PTZ_NORMAL;
    stop_retry_count = 0;
  }
  else
  { // if ptz_speed is 0 then it is stop command
    int iRet = read_cmd_timeout(data, len, read_data, 7, 1);
    if (iRet < 0)
    {
      ptz_err_code = PTZ_STOP_FAILED;
      glog_error("Reading serial timed out, PTZ stop failed\n");
    }
    else
    {
      glog_trace("Received UART response : ");
      if (iRet <= sizeof(read_data))
        print_serial_data(read_data, iRet);

// #define TEST_CODE
#ifdef TEST_CODE
      read_data[5] = 0x01;
      glog_trace("TEST code is applied\n");
#endif

      if (read_data[4] == 0x01 && read_data[5] == 0x00)
      { // read_data[4] is data length, read_data[5] is error code
        ptz_err_code = PTZ_NORMAL;
        stop_retry_count = 0;
      }
      else
      {
        ptz_err_code = PTZ_STOP_FAILED;
        glog_error("Error code is %02X\n", read_data[5]);
      }
    }
  }
  g_move_speed = ptz_speed;
  // glog_trace("set g_move_speed = %d\n", g_move_speed);

  return TRUE;
}

gboolean send_ptz_diagonal_move(int pan_direction, int tilt_direction, int speed)
{
  unsigned char data[21];
  int len = 0;

  // 현재 위치 읽기
  unsigned char current_pos[17];
  unsigned char cmd_get_pos[7] = {0x96, 0x00, 0x06, 0x01, 0x01, 0x01, 0x9F};
  if (read_cmd_timeout(cmd_get_pos, 7, current_pos, 17, 1) < 0)
  {
    return FALSE;
  }

  // 현재 위치 파싱
  int current_pan = (current_pos[6] << 8) | current_pos[5];
  int current_tilt = (current_pos[8] << 8) | current_pos[7];

  // 목표 위치 계산 (예: 100 단위씩 이동)
  int target_pan = current_pan;
  int target_tilt = current_tilt;

  if (pan_direction == 0)
    target_pan -= 100; // left
  else if (pan_direction == 1)
    target_pan += 100; // right

  if (tilt_direction == 0)
    target_tilt += 100; // up
  else if (tilt_direction == 1)
    target_tilt -= 100; // down

  // Set All Position 패킷 구성
  data[len++] = 0x96; // Sync
  data[len++] = 0x00; // Addr
  data[len++] = 0x01; // CmdL
  data[len++] = 0x41; // CmdH (no response)
  data[len++] = 0x0F; // Data Size (15 bytes)

  // Pan position
  data[len++] = target_pan & 0xFF;
  data[len++] = (target_pan >> 8) & 0xFF;

  // Tilt position
  data[len++] = target_tilt & 0xFF;
  data[len++] = (target_tilt >> 8) & 0xFF;

  // Zoom, Focus (현재 값 유지)
  memcpy(&data[len], &current_pos[9], 6);
  len += 6;

  // Speeds
  data[len++] = speed; // Pan speed
  data[len++] = speed; // Tilt speed
  data[len++] = 0x00;  // Zoom speed
  data[len++] = 0x00;  // Focus speed
  data[len++] = 0x00;  // Pan direction

  data[20] = get_checksum(data, 20);

  write_serial(data, 21);
  return TRUE;
}

static guint timer_id = 0;
static gboolean timer_stop_ptz_callback(gpointer user_data)
{
  send_ptz_move_cmd(0, 0);
  timer_id = 0;

  return G_SOURCE_REMOVE; // LJH, this removes source id
}

gboolean move_and_stop_ptz(int direction, int ptz_speed, int ptz_delay)
{
  if (send_ptz_move_cmd(direction, ptz_speed) == TRUE)
  {
    if (timer_id > 0)
    {
      g_source_remove(timer_id);
      timer_id = 0;
    }
    timer_id = g_timeout_add((guint)ptz_delay, timer_stop_ptz_callback, NULL);
  }
  else
  {
    return FALSE;
  }

  return TRUE;
}

void send_ptz_move_serial_data(const char *s)
{
  int count = 0;
  int direction = 0, ptz_delay = 0, ptz_speed = 0;
  glog_trace("Received PTZ command from server: %s\n", s);
  char str[100];
  strcpy(str, s);

  char *ptr = strtok((char *)str, ",");
  while (ptr != NULL)
  {
    if (count == 0)
      direction = atoi(ptr);
    else if (count == 1)
      ptz_delay = atoi(ptr);
    else if (count == 2)
      ptz_speed = atoi(ptr);
    count++;
    ptr = strtok(NULL, ",");
  }
  glog_trace("direction:%d,delay:%d,speed:%d\n", direction, ptz_delay, ptz_speed);
  move_and_stop_ptz(direction, ptz_speed, ptz_delay);
}

void send_ptz_move_serial_data_immediate(const char *s)
{
  int count = 0;
  int direction = 0, ptz_delay = 0, ptz_speed = 0;
  glog_trace("Received PTZ command from server: %s\n", s);
  char str[100];
  strcpy(str, s);

  char *ptr = strtok((char *)str, ",");
  while (ptr != NULL)
  {
    if (count == 0)
      direction = atoi(ptr);
    else if (count == 1)
      ptz_delay = atoi(ptr);
    else if (count == 2)
      ptz_speed = atoi(ptr);
    count++;
    ptr = strtok(NULL, ",");
  }
  glog_trace("direction:%d,delay:%d,speed:%d\n", direction, ptz_delay, ptz_speed);
  move_and_stop_ptz_immediate(direction, ptz_speed);
}

void pause_auto_ptz(void)
{
  pthread_mutex_lock(&g_auto_ptz_state.mutex);
  g_auto_ptz_state.is_paused = TRUE;
  pthread_mutex_unlock(&g_auto_ptz_state.mutex);
  glog_info("Auto PTZ paused\n");
}

void resume_auto_ptz(void)
{
  pthread_mutex_lock(&g_auto_ptz_state.mutex);
  g_auto_ptz_state.is_paused = FALSE;
  pthread_mutex_unlock(&g_auto_ptz_state.mutex);
  glog_info("Auto PTZ resumed\n");
}

void stop_auto_ptz(void)
{
  request_auto_move_ptz_stop();
}

int get_current_position(PTZPosition *pos)
{
  if (!is_open_serial())
  {
    glog_error("Serial port not open\n");
    return -1;
  }

  unsigned char cmd_data[7] = {0x96, 0x00, 0x06, 0x01, 0x01, 0x01, 0x9F};
  unsigned char read_data[17];

  int result = read_cmd_timeout(cmd_data, 7, read_data, 17, 1);
  if (result == -1)
  {
    glog_error("Failed to read current position\n");
    return -1;
  }

  // 응답 데이터 파싱
  pos->pan = (read_data[5] << 8) | read_data[6];
  pos->tilt = (read_data[7] << 8) | read_data[8];
  pos->zoom = (read_data[9] << 8) | read_data[10];
  pos->focus = (read_data[11] << 8) | read_data[12];
  pos->iris = (read_data[13] << 8) | read_data[14];

  return 0;
}

void parse_target_position(unsigned char *ptz_data, PTZPosition *pos)
{
  pos->pan = (ptz_data[0] << 8) | ptz_data[1];
  pos->tilt = (ptz_data[2] << 8) | ptz_data[3];
  pos->zoom = (ptz_data[4] << 8) | ptz_data[5];
  pos->focus = (ptz_data[6] << 8) | ptz_data[7];
  pos->iris = (ptz_data[8] << 8) | ptz_data[9];
}

// 위치 비교 함수
gboolean is_position_reached(PTZPosition *current, PTZPosition *target, gboolean check_zoom)
{
  int pan_diff = abs(current->pan - target->pan);
  int tilt_diff = abs(current->tilt - target->tilt);
  int zoom_diff = abs(current->zoom - target->zoom);

  // Pan/Tilt 체크
  if (pan_diff > PAN_TILT_TOLERANCE || tilt_diff > PAN_TILT_TOLERANCE)
  {
    return FALSE;
  }

  // Zoom 체크 (옵션)
  if (check_zoom && zoom_diff > ZOOM_TOLERANCE)
  {
    return FALSE;
  }

  return TRUE;
}

gboolean is_ptz_motion_stopped_with_position_check(PTZPosition *target_pos)
{
  static int check_count = 0;
  PTZPosition current_pos;

  // 먼저 기존 상태 체크
  int motion_status = get_pt_status();
  if ((motion_status & 0xF) != 0)
  {
    check_count = 0;
    return FALSE; // 아직 움직이는 중
  }

  // 목표 위치가 없으면 기존 방식대로
  if (target_pos == NULL)
  {
    return TRUE;
  }

  // 현재 위치 읽기
  if (get_current_position(&current_pos) != 0)
  {
    glog_error("Failed to get current position\n");
    return TRUE; // 에러시 정지로 간주
  }

  // 위치 도달 확인
  if (is_position_reached(&current_pos, target_pos, TRUE))
  {
    check_count++;

    // 2번 연속 확인되면 완료로 판단 (떨림 방지)
    if (check_count >= 2)
    {
      check_count = 0;
      glog_trace("Position reached - Target: P:%d T:%d Z:%d, Current: P:%d T:%d Z:%d\n",
                 target_pos->pan, target_pos->tilt, target_pos->zoom,
                 current_pos.pan, current_pos.tilt, current_pos.zoom);
      return TRUE;
    }
  }
  else
  {
    check_count = 0;
  }

  return FALSE;
}

int wait_for_ptz_completion(int preset_index, int timeout_sec)
{
  PTZPosition target_pos;
  int elapsed = 0;

  // 프리셋의 목표 위치 가져오기
  parse_target_position(&AUTO_PTZ_POS_LIST[preset_index][1], &target_pos);

  // glog_trace("Waiting for PTZ preset %d to reach position P:%d T:%d Z:%d\n",
  //           preset_index, target_pos.pan, target_pos.tilt, target_pos.zoom);

  while (elapsed < timeout_sec)
  {
    // 기존 방식의 모션 정지 확인
    if (is_ptz_motion_stopped())
    {
      // 위치 기반 추가 확인
      PTZPosition current_pos;
      if (get_current_position(&current_pos) == 0)
      {
        if (is_position_reached(&current_pos, &target_pos, !g_no_zoom))
        {
          // glog_trace("PTZ reached target position in %d seconds\n", elapsed);
          return elapsed;
        }
      }
      else
      {
        // 위치 읽기 실패시 기존 방식만 사용
        return elapsed;
      }
    }

    sleep(1);
    elapsed++;

    // 중단 신호 체크
    if (AUTO_PTZ_MOVE_SEQ[0] == 0)
    {
      return -1;
    }
  }

  // glog_info("PTZ motion timeout after %d seconds\n", timeout_sec);
  return timeout_sec;
}

const AutoPTZState *get_auto_ptz_state(void)
{
  return &g_auto_ptz_state;
}

AutoPTZState get_auto_ptz_state_copy(void)
{
  return g_auto_ptz_state;
}

void display_auto_ptz_status(void)
{
  glog_trace("========== All Preset Data ==========\n");

  // 1. Auto PTZ 프리셋 확인
  glog_trace("--- Auto PTZ Presets (AUTO_PTZ_POS_LIST) ---\n");
  int auto_count = 0;
  for (int i = 0; i < MAX_PTZ_PRESET; i++)
  {
    if (AUTO_PTZ_POS_LIST[i][0])
    { // 설정되어 있으면
      PTZPosition pos;
      parse_target_position(&AUTO_PTZ_POS_LIST[i][1], &pos);

      glog_trace("Auto Preset[%2d]: SET - Pan=%5d, Tilt=%5d, Zoom=%5d, Focus=%5d, Iris=%5d\n",
                 i, pos.pan, pos.tilt, pos.zoom, pos.focus, pos.iris);

      // Raw 데이터도 표시
      glog_trace("    Raw data: ");
      for (int j = 0; j < 10; j++)
      {
        glog_trace("%02X ", AUTO_PTZ_POS_LIST[i][j + 1]);
      }
      glog_trace("\n");
      auto_count++;
    }
  }
  glog_trace("Total Auto Presets configured: %d\n\n", auto_count);

  // 2. Manual 프리셋 확인
  glog_trace("--- Manual Presets (PTZ_POS_LIST) ---\n");
  int manual_count = 0;
  for (int i = 0; i < MAX_PTZ_PRESET; i++)
  {
    if (PTZ_POS_LIST[i][0])
    { // 설정되어 있으면
      PTZPosition pos;
      parse_target_position(&PTZ_POS_LIST[i][1], &pos);

      glog_trace("Manual Preset[%2d]: SET - Pan=%5d, Tilt=%5d, Zoom=%5d, Focus=%5d, Iris=%5d\n",
                 i, pos.pan, pos.tilt, pos.zoom, pos.focus, pos.iris);
      manual_count++;
    }
  }
  glog_trace("Total Manual Presets configured: %d\n\n", manual_count);

  // 3. 현재 설정 정보
  glog_trace("--- Current Settings ---\n");
  glog_trace("auto_ptz_seq: '%s'\n", g_setting.auto_ptz_seq);
  glog_trace("AUTO_PTZ_MOVE_SEQ[0] (Enabled): %d\n", AUTO_PTZ_MOVE_SEQ[0]);
  glog_trace("AUTO_PTZ_MOVE_SEQ[1] (Stay Time): %d\n", AUTO_PTZ_MOVE_SEQ[1]);
  glog_trace("AUTO_PTZ_MOVE_SEQ[2] (Count): %d\n", AUTO_PTZ_MOVE_SEQ[2]);

  if (AUTO_PTZ_MOVE_SEQ[2] > 0)
  {
    glog_trace("Sequence order: ");
    for (int i = 0; i < AUTO_PTZ_MOVE_SEQ[2]; i++)
    {
      glog_trace("%d ", AUTO_PTZ_MOVE_SEQ[3 + i]);
    }
    glog_trace("\n");
  }

  glog_trace("=====================================\n");
}

int goto_preset(int preset_index, int use_auto_preset)
{
  if (!is_open_serial())
  {
    glog_error("Serial device not opened\n");
    return -1;
  }

  if (preset_index < 0 || preset_index >= MAX_PTZ_PRESET)
  {
    glog_error("Invalid preset index: %d (valid range: 0-%d)\n",
               preset_index, MAX_PTZ_PRESET - 1);
    return -1;
  }

  // Auto PTZ가 동작 중이면 일시정지
  if (is_work_auto_ptz())
  {
    pause_auto_ptz();
    glog_info("Auto PTZ paused for manual movement\n");
  }

  // 프리셋 데이터 선택
  unsigned char *preset_list = use_auto_preset ? AUTO_PTZ_POS_LIST[preset_index] : PTZ_POS_LIST[preset_index];

  // 프리셋이 설정되어 있는지 확인
  if (preset_list[0] == 0)
  {
    glog_error("%s Preset %d is not configured\n",
               use_auto_preset ? "Auto" : "Manual", preset_index);
    return -1;
  }

  // 목표 위치 파싱
  PTZPosition target_pos;
  parse_target_position(&preset_list[1], &target_pos);

  glog_info("Moving to %s Preset %d: Pan=%d, Tilt=%d, Zoom=%d\n",
            use_auto_preset ? "Auto" : "Manual",
            preset_index, target_pos.pan, target_pos.tilt, target_pos.zoom);

  // PTZ 이동 명령 생성
  unsigned char send_data[32] = {0x96, 0x00, 0x01, 0x01, 0x0F};
  memcpy(&send_data[5], &preset_list[1], 10);

  // 속도 설정 (수동 이동이므로 일반 속도 사용)
  int move_speed = ptz_move_speed;
  send_data[15] = send_data[16] = send_data[17] = send_data[18] = move_speed;
  send_data[19] = 0;
  send_data[20] = get_checksum(send_data, 20);

  // 명령 전송
  unsigned char read_data[8];
  int result = read_cmd_timeout(send_data, 21, read_data, 7, 1);
  if (result == -1)
  {
    glog_error("Failed to send move command\n");
    return -1;
  }

  return 0;
}

int goto_manual_preset(int preset_index)
{
  return goto_preset(preset_index, 0);
}

int goto_auto_preset(int preset_index)
{
  return goto_preset(preset_index, 1);
}

void move_and_stop_ptz_immediate(int direction, int speed)
{
  if (speed > 0)
  {
    // 이동 시작
    // if (direction >= 0 && direction <= 5)
    send_ptz_move_cmd(direction, speed);
    // else
    // {
    //   send_ptz_diagonal_move(direction - 6, direction - 6, speed);
    // }
    g_move_speed = speed;
  }
  else
  {
    // 즉시 정지
    send_ptz_move_cmd(direction, 0);
    g_move_speed = 0;
  }
}