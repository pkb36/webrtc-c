/*
 * Demo gstreamer app for negotiating and streaming a sendrecv audio-only webrtc
 * stream to all the peers in a multiparty room.
 *
 * gcc mp-webrtc-sendrecv.c $(pkg-config --cflags --libs gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0) -o mp-webrtc-sendrecv
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 */
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include "config.h"
#include "webrtc_peer.h"
#include "serial_comm.h"
#include "gstream_main.h"
#include "json_utils.h"
#include "curllib.h"
#include "device_setting.h"
#include "nvds_process.h"
#include "nvds_utils.h"
#include "ptz_control.h"
#include "event_recorder.h"
#include "log_wrapper.h"
#include "video_convert.h"
#include "command_handler.h"

#include <unistd.h> // write, close 등을 위해 추가
#include <errno.h>  // strerror를 위해 추가

#define IP_MAX_LENGTH 15

extern void set_tracker_analysis(gboolean OnOff);

pthread_mutex_t g_send_mutex;
pthread_mutex_t g_process_msg_mutex;
pthread_mutex_t g_send_info_mutex;
pthread_mutex_t g_retry_connect_mutex;

WebRTCConfig g_config;
GstElement *g_pipeline;
CurlIinfoType g_curlinfo;
DeviceSetting g_setting;
static gchar *g_config_name = NULL;
static gchar *g_pipe_name = "/home/nvidia/webrtc/webrtc_pipe";
static GOptionEntry entries[] = {
    {"config", 0, 0, G_OPTION_ARG_STRING, &g_config_name, "config file name", "ID"},
    {NULL}};
int g_source_cam_idx = RGB_CAM;

#if MINDULE_INCLUDE

#define SERVER_UDP_PORT 8000
#define REST_SERVER_PATH "/home/nvidia/rest_server_bin"

extern gboolean load_ranch_setting(const char *file_name, RanchSetting *setting);
extern void get_ranch_setting_path(char *fname);
RanchSetting g_ranch_setting;

#endif

static gchar *g_completion_pipe_name = "/home/nvidia/webrtc/recording_completion_pipe";
static GIOChannel *g_completion_channel = NULL;
static guint g_completion_watch_id = 0;

static gboolean init_completion_pipe(void);
static void cleanup_completion_pipe(void);
static gboolean completion_pipe_read_callback(GIOChannel *source, GIOCondition condition, gpointer data);
static void process_recording_completion(const char *file_path);
static int extract_camera_info_from_path(const char *file_path, char *http_url);

static char g_codec_name[16];
static GMainLoop *loop;
static SoupWebsocketConnection *ws_conn = NULL;
// static gboolean strict_ssl = TRUE;
static gboolean strict_ssl = FALSE;

static int connect_retry = 0;
int g_wait_reply_cnt = 0;
enum AppState g_app_state = 0;

gboolean send_register_with_server(SoupWebsocketConnection *ws_conn);
static void connect_to_websocket_server_async(void);
void redirect_output();
void send_pipe_data(const gchar *str);

extern void *receive_data(void *arg);
extern SOCKETINFO *init_socket_server(int port, void *(*func_ptr)(void *), void (*process_data)(char *ptr, int len, void *arg));
extern void process_data(char *buffer, int len, void *arg);
extern char *get_global_ip();
extern void handle_custom_command(gJSONObj *jsonObj, send_message_func_t send_func);

extern pthread_mutex_t g_send_info_mutex;

// PTZ 관련 전역 변수와 함수 선언부 (gstream_main.c 상단에 추가)
static gchar *g_ptz_pipe_name = "/home/nvidia/webrtc/ptz_command_pipe";
static GIOChannel *g_ptz_channel = NULL;
static guint g_ptz_watch_id = 0;

// 함수 선언 (다른 함수들 선언 부분에 추가)
static gboolean init_ptz_pipe(void);
static void cleanup_ptz_pipe(void);
static gboolean ptz_pipe_read_callback(GIOChannel *source, GIOCondition condition, gpointer data);
// static gboolean check_ptz_pipe_status(gpointer data);
static void process_ptz_command(const char *command);

// PTZ 명령 처리 함수
static void process_ptz_command(const char *command)
{
  glog_trace("Received PTZ command: %s\n", command);

  // 명령어에서 개행 문자 제거
  char clean_cmd[64];
  strncpy(clean_cmd, command, sizeof(clean_cmd) - 1);
  clean_cmd[sizeof(clean_cmd) - 1] = '\0';

  // 개행 문자 제거
  char *newline = strchr(clean_cmd, '\n');
  if (newline)
    *newline = '\0';

  if (strcmp(clean_cmd, "up") == 0)
  {
    send_pipe_data("up");
  }
  else if (strcmp(clean_cmd, "down") == 0)
  {
    send_pipe_data("down");
  }
  else if (strcmp(clean_cmd, "left") == 0)
  {
    send_pipe_data("left");
  }
  else if (strcmp(clean_cmd, "right") == 0)
  {
    send_pipe_data("right");
  }
  else if (strcmp(clean_cmd, "enter") == 0)
  {
    send_pipe_data("enter");
  }
  else if (strcmp(clean_cmd, "zoom_init") == 0)
  {
    send_pipe_data("zoom_init");
  }
  else if (strcmp(clean_cmd, "ir_init") == 0)
  {
    send_pipe_data("ir_init");
  }
  else if (strcmp(clean_cmd, "trigger_event") == 0)
  {
    trigger_event_record(0, 0);
  }
  else
  {
    glog_trace("Unknown PTZ command: %s\n", clean_cmd);
  }
}

// PTZ 파이프 읽기 콜백 함수 (수정된 버전 - chars 사용)
static gboolean ptz_pipe_read_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
  glog_trace("PTZ pipe callback triggered, condition: %d\n", condition);

  if (condition & G_IO_HUP)
  {
    glog_trace("PTZ pipe closed by writer, reopening...\n");
    if (g_ptz_channel)
    {
      g_io_channel_unref(g_ptz_channel);
      g_ptz_channel = NULL;
    }
    g_ptz_watch_id = 0;

    // 잠시 후 다시 초기화
    g_timeout_add(1000, (GSourceFunc)init_ptz_pipe, NULL);
    return FALSE; // 이 watch는 제거
  }

  if (condition & G_IO_IN)
  {
    gchar buffer[256];
    gsize bytes_read = 0;
    GError *error = NULL;
    GIOStatus status;

    // 한 번에 여러 바이트 읽기
    status = g_io_channel_read_chars(source, buffer, sizeof(buffer) - 1, &bytes_read, &error);

    if (status == G_IO_STATUS_NORMAL && bytes_read > 0)
    {
      buffer[bytes_read] = '\0'; // null 종료
      glog_trace("Read from PTZ pipe (%zu bytes): %s", bytes_read, buffer);

      // 여러 명령이 한 번에 올 수 있으므로 줄바꿈으로 분리
      gchar **lines = g_strsplit(buffer, "\n", -1);
      for (int i = 0; lines[i] != NULL; i++)
      {
        if (strlen(lines[i]) > 0)
        { // 빈 줄 제외
          process_ptz_command(lines[i]);
        }
      }
      g_strfreev(lines);
    }
    else if (status == G_IO_STATUS_AGAIN)
    {
      // 더 이상 읽을 데이터가 없음 - 정상
    }
    else if (status == G_IO_STATUS_ERROR)
    {
      if (error)
      {
        glog_error("Error reading from PTZ pipe: %s\n", error->message);
        g_error_free(error);
      }
    }
    else if (status == G_IO_STATUS_EOF)
    {
      glog_trace("PTZ pipe EOF\n");
    }
  }

  if (condition & G_IO_ERR)
  {
    glog_error("PTZ pipe error condition\n");
    return FALSE;
  }

  return TRUE; // 계속 모니터링
}

// 주기적으로 파이프 상태를 확인하는 함수
// static gboolean check_ptz_pipe_status(gpointer data) {
//     static int check_count = 0;
//     check_count++;

//     if (check_count % 10 == 0) { // 10초마다
//         glog_trace("PTZ pipe status check #%d\n", check_count);

//         // 파이프 파일이 여전히 존재하는지 확인
//         if (!fifo_exists(g_ptz_pipe_name)) {
//             glog_trace("PTZ pipe disappeared, recreating...\n");
//             init_ptz_pipe();
//         }
//     }

//     return TRUE; // 계속 실행
// }

// PTZ 파이프 초기화 함수
static gboolean init_ptz_pipe(void)
{
  glog_trace("Initializing PTZ command pipe...\n");

  // 기존 watch 제거
  if (g_ptz_watch_id > 0)
  {
    g_source_remove(g_ptz_watch_id);
    g_ptz_watch_id = 0;
  }

  // 기존 채널 정리
  if (g_ptz_channel)
  {
    g_io_channel_unref(g_ptz_channel);
    g_ptz_channel = NULL;
  }

  // Named pipe가 존재하는지 확인하고 삭제 후 재생성
  if (access(g_ptz_pipe_name, F_OK) == 0)
  {
    unlink(g_ptz_pipe_name);
    glog_trace("Removed existing PTZ pipe\n");
  }

  // Named pipe 생성
  if (mkfifo(g_ptz_pipe_name, 0777) == -1)
  { // 더 넓은 권한으로 생성
    glog_error("Failed to create PTZ command pipe: %s, error: %s\n",
               g_ptz_pipe_name, strerror(errno));
    return FALSE;
  }

  // 권한 명시적으로 설정
  if (chmod(g_ptz_pipe_name, 0666) == -1)
  {
    glog_error("Failed to set permissions for PTZ pipe: %s\n", strerror(errno));
  }

  glog_trace("Created PTZ command pipe: %s\n", g_ptz_pipe_name);

  // 파이프를 읽기+쓰기 모드로 열기 (논블로킹)
  int fd = open(g_ptz_pipe_name, O_RDWR | O_NONBLOCK);
  if (fd == -1)
  {
    glog_error("Failed to open PTZ command pipe: %s, error: %s\n",
               g_ptz_pipe_name, strerror(errno));
    return FALSE;
  }

  glog_trace("Opened PTZ pipe with fd: %d\n", fd);

  // GIOChannel 생성
  g_ptz_channel = g_io_channel_unix_new(fd);
  if (!g_ptz_channel)
  {
    glog_error("Failed to create GIOChannel for PTZ pipe\n");
    close(fd);
    return FALSE;
  }

  // 채널 설정 - UTF-8 인코딩 사용
  GError *error = NULL;
  if (g_io_channel_set_encoding(g_ptz_channel, "UTF-8", &error) != G_IO_STATUS_NORMAL)
  {
    if (error)
    {
      glog_error("Failed to set encoding: %s\n", error->message);
      g_error_free(error);
      error = NULL;
    }
    // UTF-8 설정이 실패하면 기본 인코딩 사용
    g_io_channel_set_encoding(g_ptz_channel, "", &error);
    if (error)
    {
      glog_error("Failed to set default encoding: %s\n", error->message);
      g_error_free(error);
    }
  }

  g_io_channel_set_buffered(g_ptz_channel, FALSE);
  g_io_channel_set_close_on_unref(g_ptz_channel, TRUE);

  // 라인 종료 문자 설정
  g_io_channel_set_line_term(g_ptz_channel, "\n", 1);

  // 이벤트 모니터링 추가
  g_ptz_watch_id = g_io_add_watch(g_ptz_channel,
                                  G_IO_IN | G_IO_HUP | G_IO_ERR,
                                  ptz_pipe_read_callback,
                                  NULL);

  if (g_ptz_watch_id == 0)
  {
    glog_error("Failed to add watch for PTZ pipe\n");
    g_io_channel_unref(g_ptz_channel);
    g_ptz_channel = NULL;
    return FALSE;
  }

  glog_trace("PTZ command pipe initialized successfully with watch_id: %d\n", g_ptz_watch_id);
  return TRUE;
}

// PTZ 파이프 정리 함수
static void cleanup_ptz_pipe(void)
{
  if (g_ptz_watch_id > 0)
  {
    g_source_remove(g_ptz_watch_id);
    g_ptz_watch_id = 0;
  }

  if (g_ptz_channel)
  {
    g_io_channel_unref(g_ptz_channel);
    g_ptz_channel = NULL;
  }

  if (access(g_ptz_pipe_name, F_OK) == 0)
  {
    unlink(g_ptz_pipe_name);
  }
}

int check_internet_connection()
{
  return system(PING_TEST) == 0;
}

gboolean cleanup_and_retry_connect(const gchar *msg, enum AppState state)
{
  pthread_mutex_lock(&g_retry_connect_mutex);
  if (msg)
    glog_error("%s AppState %d\n", msg, state);
  if (state > 0)
    g_app_state = state;

  /*@@ clearn up client*/
  free_webrtc_peer(FALSE);

  if (ws_conn)
  {
    if (soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN)
    {
      /* This will call us again */
      soup_websocket_connection_close(ws_conn, 1000, "");
    }
    else
    {
      g_object_unref(ws_conn);
    }
    ws_conn = NULL;
  }

  if (APP_STATE_ERROR_TIMEOUT == state)
  {
    pthread_mutex_unlock(&g_retry_connect_mutex);
    return 1;
  }

  sleep(10);
  glog_trace("try reconnect %d\n", connect_retry++);
  connect_to_websocket_server_async();
  pthread_mutex_unlock(&g_retry_connect_mutex);

  return 0;
}

void send_msg_server(const gchar *msg)
{
  pthread_mutex_lock(&g_send_mutex);
  soup_websocket_connection_send_text(ws_conn, msg);
  pthread_mutex_unlock(&g_send_mutex);
}

static gboolean start_pipeline(void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;

  char str_pipeline[8192 * 2] = {
      0,
  };
  char str_video[4096];

  for (int i = 0; i < g_config.device_cnt; i++)
  {
    snprintf(str_video, 4096,
             "%s  "
             "%s  "
             "location=%s/cam%d_snapshot.jpg  "
             "%s  "
             "%s  "
             "tee name=video_enc_tee1_%d "
             "%s  "
             "tee name=video_enc_tee2_%d ",
             g_config.video_src[i], g_config.snapshot_enc[i], g_config.snapshot_path, i, g_config.video_infer[i], g_config.video_enc[i], i, g_config.video_enc2[i], i);
    strcat(str_pipeline, str_video);
  }

  if (strstr(g_config.video_enc[0], "vp8"))
  {
    strcpy(g_codec_name, "VP8");
  }
  else if (strstr(g_config.video_enc[0], "vp9"))
  {
    strcpy(g_codec_name, "VP9");
  }
  else if (strstr(g_config.video_enc[0], "264"))
  {
    strcpy(g_codec_name, "H264");
  }

  // main stream(=tee1): sender(=10)+record(=1)+event record(=1) == g_config.max_stream_cnt(=10)+2
  // second stream(=tee2): sender(=10)+record(=1)+event record(=1) == g_config.max_stream_cnt(=10)+2, the plus gap from main stream port number is 100
  int udp_port = 0;
  int index = 0;
  CameraDevice device = RGB_CAM;
  for (StreamChoice stream = MAIN_STREAM; stream <= SECOND_STREAM; stream++)
  {
    for (int i = 0; i < g_config.max_stream_cnt + 2; i++)
    {
      for (int j = 0; j < g_config.device_cnt; j++)
      {
        device = j;
        index = i;

        if (i < g_config.max_stream_cnt)
          udp_port = get_udp_port(SENDER, device, stream, index);
        else if (i == g_config.max_stream_cnt)
          udp_port = get_udp_port(RECORDER, device, stream, 0);
        else if (i == (g_config.max_stream_cnt + 1))
          udp_port = get_udp_port(EVENT_RECORDER, device, stream, 0);

        if (stream == MAIN_STREAM)
          snprintf(str_video, sizeof(str_video), " video_enc_tee1_%d. ! queue ! udpsink host=127.0.0.1  port=%d sync=false async=false", device, udp_port);
        else if (stream == SECOND_STREAM)
          snprintf(str_video, sizeof(str_video), " video_enc_tee2_%d. ! queue ! udpsink host=127.0.0.1  port=%d sync=false async=false", device, udp_port);

        strcat(str_pipeline, str_video);
        glog_trace("str_video=%s\n", str_video);
      }
    }
  }

  glog_trace("%lu  %s\n", strlen(str_pipeline), str_pipeline);
  g_pipeline = gst_parse_launch(str_pipeline, &error);
  if (error)
  {
    glog_error("Failed to parse launch: %s\n", error->message);
    g_error_free(error);
    goto err;
  }

  // setup OSD  and event detection.
  setup_nv_analysis();

  glog_trace("Starting pipeline, not transmitting yet\n");
  ret = gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
  {
    g_printerr("State change to PLAYING failed\n");

    GstMessage *msg;
    GstBus *bus = gst_element_get_bus(g_pipeline);
    while ((msg = gst_bus_pop(bus)) != NULL)
    {
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
      {
        GError *err;
        gchar *debug_info;

        gst_message_parse_error(msg, &err, &debug_info);
        g_printerr("Error received from element %s: %s\n",
                   GST_OBJECT_NAME(msg->src), err->message);
        g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");

        g_clear_error(&err);
        g_free(debug_info);
      }
      gst_message_unref(msg);
    }
    gst_object_unref(bus);

    goto err;
  }

  return TRUE;

err:
  glog_critical("State change failure\n");
  if (g_pipeline)
    g_clear_object(&g_pipeline);
  return FALSE;
}

int is_global_ip(const char *ip)
{
  unsigned int a, b, c, d;
  // Split the IP address into its four octets
  if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
  {
    return -1; // Invalid IP format
  }

  // Check for private IP ranges
  if ((a == 10) ||
      (a == 172 && b >= 16 && b <= 31) ||
      (a == 192 && b == 168))
  {
    return 0; // Private IP
  }

  return 1; // Global IP
}

// Function to extract the first IP address from the string
char *extract_ip(const char *input)
{
  static char ip[IP_MAX_LENGTH + 1]; // Buffer to hold the IP address
  const char *ptr = input;           // Pointer to traverse the input string
  int i = 0;

  // Initialize the IP buffer
  memset(ip, 0, sizeof(ip));

  // Loop through the input string to find the first IP address
  while (*ptr && i < IP_MAX_LENGTH)
  {
    // If the character is a digit or a dot, append it to the IP buffer
    if (isdigit(*ptr) || *ptr == '.')
    {
      ip[i++] = *ptr;
    }
    // If we encounter a space or a non-digit, stop reading the IP address
    else if (isspace(*ptr) || (!isalnum(*ptr) && *ptr != '.'))
    {
      break;
    }
    ptr++;
  }

  // Ensure the IP string is null-terminated
  ip[i] = '\0';

  // Return NULL if no valid IP address was found
  return (i > 0) ? ip : NULL;
}

int get_service_address(char *address)
{
  FILE *fp;
  char line[100];

  fp = fopen("local_ip.log", "r+t");
  if (NULL == fp)
  {
    glog_error("fail load local_ip.log");
    return -1;
  }
  fgets(line, sizeof(line), fp);
  fgets(line, sizeof(line), fp);
  fclose(fp);

  strcpy(address, line);
  glog_trace("address = %s\n", line);
  char *ip_address = extract_ip(line);
  if (is_global_ip(ip_address) != 1)
    return 0;

  return 1;
}

static void on_server_closed(SoupWebsocketConnection *conn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  g_app_state = SERVER_CLOSED;
  cleanup_and_retry_connect("Server connection closed", 0);

  char line[256];

  if (get_service_address(line) != 1)
  {
    glog_trace("update_http_service_ip\n");
    update_http_service_ip(&g_config);
  }
  else
  {
    char *global_ip = get_global_ip();
    char *ip_address = extract_ip(line);

    if (global_ip)
    {
      if (strcmp(global_ip, ip_address) != 0)
      {
        glog_trace("global_ip=%s, ip_address=%s, update_http_service_ip\n", global_ip, ip_address);
        update_http_service_ip(&g_config);
      }
      free(global_ip);
    }
  }
}

#if MINDULE_INCLUDE
gboolean is_rest_server()
{
  if (is_dir(REST_SERVER_PATH))
  {
    glog_trace("'%s' direcotry exists\n", REST_SERVER_PATH);
    if (is_process_running("rest_server"))
    {
      glog_trace("rest_server is running\n");
      return 1;
    }
    else
    {
      glog_trace("rest_server is not running\n");
    }
  }
  glog_trace("'%s' directory does not exists\n", REST_SERVER_PATH);

  return 0;
}
#endif

void send_websocket_message(const char *message)
{
  if (!ws_conn)
  {
    glog_error("WebSocket connection not available\n");
    return;
  }

  pthread_mutex_lock(&g_send_mutex);
  soup_websocket_connection_send_text(ws_conn, message);
  pthread_mutex_unlock(&g_send_mutex);

  glog_trace("Sent message via WebSocket: %.100s%s\n",
             message, strlen(message) > 100 ? "..." : "");
}

/* One mega message handler for our asynchronous calling mechanism */
static void on_server_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type, GBytes *message, gpointer user_data)
{
  gchar *json_txt = NULL;

  switch (type)
  {
  case SOUP_WEBSOCKET_DATA_BINARY:
    glog_error("Received unknown binary message, ignoring\n");
    return;
  case SOUP_WEBSOCKET_DATA_TEXT:
  {
    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);
    /* Convert to NULL-terminated string */
    json_txt = g_strndup(data, size);
    break;
  }
  default:
    g_assert_not_reached();
  }

  // glog_trace("json_txt=\'%s\'\n", json_txt);
  gJSONObj *jsonObj = get_json_object(json_txt);

  const gchar *action;
  if (!get_json_action(jsonObj, &action))
  {
    goto err;
  }

  if (g_strcmp0(action, "ROOM_PEER_JOINED") == 0)
  {
    const gchar *peer_id;
    const gchar *source;
    get_json_data_from_message(jsonObj, "peer_id", &peer_id);
    get_json_data_from_message(jsonObj, "source", &source); // LJH, source = RGB/THERMAL

    if (strcmp(source, "RGB") == 0) // LJH, 241211
      g_source_cam_idx = RGB_CAM;
    else if (strcmp(source, "Thermal") == 0)
      g_source_cam_idx = THERMAL_CAM;
    glog_trace("Negotiated with peer_id=%s,source=%s,g_source_cam_idx=%d\n", peer_id, source, g_source_cam_idx);
    g_setting.camera_index = g_source_cam_idx;
    update_setting(g_config.device_setting_path, &g_setting); // LJH, 20250305
    add_peer_to_pipeline(peer_id, source);
  }
  else if (g_strcmp0(action, "ROOM_PEER_LEFT") == 0)
  {
    const gchar *peer_id;
    get_json_data_from_message(jsonObj, "peer_id", &peer_id);

    glog_trace("Peer %s has left the room\n", peer_id);
    remove_peer_from_pipeline(peer_id);
  }
  else if (g_strcmp0(action, "answer") == 0)
  {
    const gchar *peer_id;
    get_json_data_from_message(jsonObj, "peer_id", &peer_id);
    gchar *msg = get_json_data_from_message_as_string(jsonObj, "sdp");
    handle_peer_message(peer_id, msg);
    g_free(msg);
  }
  else if (g_strcmp0(action, "candidate") == 0)
  {
    const gchar *peer_id;
    get_json_data_from_message(jsonObj, "peer_id", &peer_id);
    gchar *msg = get_json_data_from_message_as_string(jsonObj, "ice");
    handle_peer_message(peer_id, msg);
    g_free(msg);
  }
  else if (g_strcmp0(action, "send_camera") == 0)
  {
    pthread_mutex_lock(&g_process_msg_mutex);
    // JSON에서 custom_command 키가 있는지 확인
    const gchar *custom_command = NULL;
    if (get_json_data_from_message(jsonObj, "custom_command", &custom_command))
    {
      // custom_command가 있으면 handle_custom_command로 처리
      glog_trace("Detected custom_command in send_camera: %s\n", custom_command);

      // jsonObj를 custom_command 형태로 변환하여 전달
      handle_custom_command(jsonObj, send_websocket_message);
    }
    else
    {
      // 기존 방식으로 처리
      process_message_cmd(jsonObj);
    }
    pthread_mutex_unlock(&g_process_msg_mutex);
  }
  else if (g_strcmp0(action, "camstatus_reply") == 0)
  {
    g_wait_reply_cnt = g_wait_reply_cnt - 1;
    if (g_wait_reply_cnt > 0)
      glog_trace("camstatus_reply g_wait_reply_cnt=%d\n", g_wait_reply_cnt);
  }
  else
  {
    goto err;
  }

out:
  if (jsonObj)
    free_json_object(jsonObj);
  if (json_txt)
    g_free(json_txt);
  return;

err:
{
  gchar *err_s = g_strdup_printf("ERROR: unknown action %s", json_txt);
  g_free(err_s);
  goto out;
}
}

static void on_server_connected(SoupSession *session, GAsyncResult *res, SoupMessage *msg)
{
  GError *error = NULL;

  ws_conn = soup_session_websocket_connect_finish(session, res, &error);
  if (error)
  {
    cleanup_and_retry_connect(error->message, SERVER_CONNECTION_ERROR);
    g_error_free(error);
    return;
  }

  g_assert_nonnull(ws_conn);

  g_app_state = SERVER_CONNECTED;
  glog_trace("Connected to signalling server\n");

  g_signal_connect(ws_conn, "closed", G_CALLBACK(on_server_closed), NULL);
  g_signal_connect(ws_conn, "message", G_CALLBACK(on_server_message), NULL);

  /* Register with the server so it knows about us and can accept commands
   * responses from the server will be handled in on_server_message() above */
  g_app_state = SERVER_REGISTERING;
  send_register_with_server(ws_conn);

  connect_retry = 0;
  g_wait_reply_cnt = 0;
}

/*
 * Connect to the signalling server. This is the entrypoint for everything else.
 */
static void
connect_to_websocket_server_async(void)
{
  SoupLogger *logger;
  SoupMessage *message;
  SoupSession *session;
  const char *https_aliases[] = {"wss", NULL};

  session = soup_session_new_with_options(SOUP_SESSION_SSL_STRICT, strict_ssl,
                                          SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                          // SOUP_SESSION_SSL_CA_FILE, "/etc/ssl/certs/ca-bundle.crt",
                                          SOUP_SESSION_HTTPS_ALIASES, https_aliases, NULL);

  logger = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
  soup_session_add_feature(session, SOUP_SESSION_FEATURE(logger));
  g_object_unref(logger);

  gchar *url_path = g_strdup_printf("%s/signaling/%s/?token=test&peerType=camera", g_config.server_ip, g_config.camera_id);
  message = soup_message_new(SOUP_METHOD_GET, url_path);
  g_free(url_path);

  glog_trace("Connecting to server...\n");

  /* Once connected, we will register */
  soup_session_websocket_connect_async(session, message, NULL, NULL, NULL,
                                       (GAsyncReadyCallback)on_server_connected, message);
  g_app_state = SERVER_CONNECTING;
}

static gboolean
check_plugins(void)
{
  int i;
  gboolean ret;
  GstRegistry *registry;
  const gchar *needed[] = {"vpx", "nice", "webrtc", "dtls", "srtp",
                           "rtpmanager", "videotestsrc", NULL};

  registry = gst_registry_get();
  ret = TRUE;
  for (i = 0; i < g_strv_length((gchar **)needed); i++)
  {
    GstPlugin *plugin;
    plugin = gst_registry_find_plugin(registry, needed[i]);
    if (!plugin)
    {
      glog_trace("Required gstreamer plugin '%s' not found\n", needed[i]);
      ret = FALSE;
      continue;
    }
    gst_object_unref(plugin);
  }
  return ret;
}

static gboolean handle_keyboard(GIOChannel *source, GIOCondition cond, gpointer data)
{
  gchar *str = NULL;
  gchar *str_temp = NULL;

  if (g_io_channel_read_line(source, &str, NULL, NULL,
                             NULL) != G_IO_STATUS_NORMAL)
  {
    return TRUE;
  }

  switch (g_ascii_tolower(str[0]))
  {
  case 'n':
    str_temp = &str[2];
    str_temp[strlen(str_temp) - 1] = 0;
    glog_trace("add_peer_to_pipeline : peer-id [%s]\n", str_temp);
    add_peer_to_pipeline(str_temp, "RGB");
    break;

  case 'r':
    str_temp = &str[2];
    str_temp[strlen(str_temp) - 1] = 0;
    glog_trace("remove_peer_from_pipeline : peer-id [%s]\n", str_temp);
    remove_peer_from_pipeline(str_temp);
    break;

  case 's':
    printf("key press s\n");
    int status = get_pt_status();
    printf("PTZ status 0x%02X s\n", status);

    break;

  case 'c':
  {
    printf("key press 'c' test sleep for blocking testc\n");
    sleep(15);
    break;
  }

  case 'i':
    printf("key press i, ip_search \n");
    update_http_service_ip(&g_config);
    break;

  case 't':
  {
    printf("key press t\n");
    trigger_event_record(0, 0);
    break;
  }

  case 'q':
    g_main_loop_quit(loop);
    break;

  default:
    break;
  }

  return TRUE;
}

static gboolean timer_camera_info_callback(gpointer user_data)
{
  // 파이프의 Open하고 close 해야만이 Python에 전달이 되는 듯 싶음.... cat는 잘 되는데..
  char start_heart_bit[64];
  static int index = 0;
  int pipe_fd = open(g_pipe_name, O_WRONLY | O_NONBLOCK);
  if (pipe_fd == -1)
  {
    glog_error("==== open file pipe ==== ");
    return G_SOURCE_CONTINUE;
  }

  sprintf(start_heart_bit, "heart_bit_%04d", index++);
  write(pipe_fd, start_heart_bit, strlen(start_heart_bit));

  close(pipe_fd);
  if (index > 9999)
    index = 0;

  return G_SOURCE_CONTINUE;
}

gboolean is_second_count(int init)
{
  static int count = 0;

  if (init)
  {
    count = 0;
    return FALSE;
  }

  if (++count % 2 == 0)
  {
    count = 0;
    return TRUE;
  }

  return FALSE;
}

int stop_retry_count = 0;

#define MAX_STOP_RETRY 5
void check_ptz_stop_command()
{
  if (ptz_err_code == PTZ_STOP_FAILED)
  {
    if (is_second_count(0))
    {
      if (stop_retry_count < MAX_STOP_RETRY)
      {
        send_ptz_move_cmd(0, 0);
        stop_retry_count++;
        glog_trace("stop_retry_count=%d\n", stop_retry_count);
      }
    }
  }
  else
  {
    is_second_count(1);
  }
}

void init_auto_pan()
{
  glog_trace("---------- init_auto_pan --------------\n");
  if (strlen(g_setting.auto_ptz_seq) > 0)
  {
    glog_trace("auto_move_ptz(%s)\n", g_setting.auto_ptz_seq);
    auto_move_ptz(g_setting.auto_ptz_seq);
  }
  glog_trace("---------------------------------------\n");
}

int is_thermal()
{
#if TRIGGER_TRACKER_FOR_VIDEO_CLIP
  return 1; // LJH, for board test
#endif

  const char *device = "/dev/video2"; // Change as needed
  int fd = open(device, O_RDONLY);
  if (fd == -1)
  {
    glog_error("Unable to open /dev/video2\n");
    return 0;
  }

  struct v4l2_capability cap;
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1)
  {
    glog_error("Not a valid V4L2 device\n");
    close(fd);
    return 0;
  }

  // printf("Device: %s\n", cap.card);
  // printf("Driver: %s\n", cap.driver);
  // printf("Capabilities: 0x%x\n", cap.capabilities);

  close(fd);
  return 1;
}

void count_video_device() // LJH, later apply
{
  static int thermal_missing = 0;

  if (is_thermal() == 0)
    thermal_missing++;

  if (thermal_missing == 3)
  {
    if (g_config.device_cnt == 2)
      g_config.device_cnt = 1;
    glog_trace("/dev/video2 does not exist!, so device_cnt=%d\n", g_config.device_cnt);
  }
}

int g_move_ptz_condition = 0;

static gboolean timer_check_callback(gpointer user_data)
{
  static long elapsed_sec = 0;

  manage_log_file();
  check_ptz_stop_command();
  elapsed_sec++;
  if (elapsed_sec == 15)
  {
    init_auto_pan();
    g_move_ptz_condition = 1;
    count_video_device();
  }
  else if (elapsed_sec == 18)
  {
    count_video_device();
  }
  else if (elapsed_sec == 21)
  {
    count_video_device();
  }

#if TRIGGER_TRACKER_FOR_VIDEO_CLIP
  if (g_move_speed == 0 && elapsed_sec > 10 && (elapsed_sec % 4 == 0))
  {
    if (g_setting.analysis_status > 0)
    {
      set_tracker_analysis(g_setting.analysis_status);
      glog_trace("tracker %d\n", g_setting.analysis_status);
    }
  }
#endif
  dec_temp_event_time_gap();

  return G_SOURCE_CONTINUE;
}

// Function to update and return the internet state
InternetState get_internet_state(InternetState internet_state, int conn)
{
  switch (internet_state)
  {
  case INTERNET_CONNECTED:
  case INTERNET_RECONNECTED:
    // If the internet is disconnected (conn == 0), change state to DISCONNECTED
    if (conn == 0)
    {
      internet_state = INTERNET_DISCONNECTED;
      glog_trace("Internet is disconnected\n");
    }
    break;

  case INTERNET_DISCONNECTED:
    // If the internet is reconnected (conn == 1), change state to RECONNECTED
    if (conn == 1)
    {
      internet_state = INTERNET_RECONNECTED;
      glog_trace("Internet is reconnected\n");
    }
    break;

  case INIT:
    if (conn == 1)
    {
      internet_state = INTERNET_CONNECTED;
      glog_trace("Internet is connected for the first time\n");
    }
    break;

  default:
    glog_trace("Unknown internet state\n");
    break;
  }

  return internet_state;
}

void terminate_program() // LJH, 252026
{
  glog_trace("Stop program will be called in 3 seconds...\n");
  sleep(3);
  execute_process("/home/nvidia/webrtc/stop.sh", FALSE);
}

static gboolean connect_check_callback(gpointer user_data)
{
  static InternetState internet_state = INIT;
  int conn = check_internet_connection();
  int sender_num = count_processes_by_name("webrtc_sender");

  glog_trace("g_app_state=%d g_wait_reply_cnt=%d sender_num=%d CPU=%d GPU=%d g_source_cam_index=%d\n",
             g_app_state, g_wait_reply_cnt, sender_num, get_temp(0), get_temp(1), g_source_cam_idx);

  internet_state = get_internet_state(internet_state, conn);
  if (internet_state == INTERNET_RECONNECTED)
  {
    if (g_app_state != 2000)
    {
      terminate_program();
    }
  }

  return G_SOURCE_CONTINUE;
}

// gstream main이 죽을 경우 child process 역시 죽도록 함
void on_handle_exit()
{
  kill(0, SIGTERM);
}

// assert signal 을 막기 위해 죽이지 않는다.
void handle_sigabrt(int sig)
{
  glog_critical("Caught signal %d (SIGABRT)\n", sig);
  // 현재는 Abort가 나오면서 죽어 버린다. 다른 방법을 찾아야한다.
}

int fifo_exists(const char *filename)
{
  struct stat fileStat;

  // 파일의 상태를 가져옵니다
  if (stat(filename, &fileStat) == 0)
  {
    // 파일이 FIFO (named pipe)인지 확인합니다
    if (S_ISFIFO(fileStat.st_mode))
    {
      return 1; // 파일이 FIFO이면 1을 반환
    }
  }

  return 0; // 파일이 존재하지 않거나 FIFO가 아니면 0을 반환
}

void version_log()
{
  char time[36] = "";
  char prev_fname[] = "previous_version.log";
  char fname[] = "version.log";

  if (is_file(fname) == TRUE)
  {
    if (is_file(prev_fname) == TRUE)
    {
      // C 표준 함수 사용 - 더 안전함
      remove(prev_fname); // unlink() 대신 remove() 사용
    }
    // C 표준 함수로 파일 이동
    rename(fname, prev_fname);
  }

  // export version
  export_version("gstream_main", GSTREAM_MAIN_VER, 1);
  export_version("webrtc_sender", WEBRTC_SENDER_VER, 0);
  // export_version("webrtc_recorder", WEBRTC_RECORD_VER, 0);
  get_time(time, sizeof(time));
  export_version("program start time", time, 0);
}

static gboolean init_completion_pipe(void)
{
  glog_trace("Initializing recording completion pipe...\n");

  // 기존 watch 제거
  if (g_completion_watch_id > 0)
  {
    g_source_remove(g_completion_watch_id);
    g_completion_watch_id = 0;
  }

  // 기존 채널 정리
  if (g_completion_channel)
  {
    g_io_channel_unref(g_completion_channel);
    g_completion_channel = NULL;
  }

  // Named pipe 삭제 후 재생성
  if (access(g_completion_pipe_name, F_OK) == 0)
  {
    unlink(g_completion_pipe_name);
  }

  if (mkfifo(g_completion_pipe_name, 0666) == -1)
  {
    glog_error("Failed to create completion pipe: %s, error: %s\n",
               g_completion_pipe_name, strerror(errno));
    return FALSE;
  }

  glog_trace("Created completion pipe: %s\n", g_completion_pipe_name);

  // 파이프를 읽기+쓰기 모드로 열기 (논블로킹)
  int fd = open(g_completion_pipe_name, O_RDWR | O_NONBLOCK);
  if (fd == -1)
  {
    glog_error("Failed to open completion pipe: %s, error: %s\n",
               g_completion_pipe_name, strerror(errno));
    return FALSE;
  }

  // GIOChannel 생성
  g_completion_channel = g_io_channel_unix_new(fd);
  if (!g_completion_channel)
  {
    glog_error("Failed to create GIOChannel for completion pipe\n");
    close(fd);
    return FALSE;
  }

  // 채널 설정
  GError *error = NULL;
  g_io_channel_set_encoding(g_completion_channel, "UTF-8", &error);
  if (error)
  {
    glog_error("Failed to set encoding: %s\n", error->message);
    g_error_free(error);
  }

  g_io_channel_set_buffered(g_completion_channel, FALSE);
  g_io_channel_set_close_on_unref(g_completion_channel, TRUE);
  g_io_channel_set_line_term(g_completion_channel, "\n", 1);

  // 이벤트 모니터링 추가
  g_completion_watch_id = g_io_add_watch(g_completion_channel,
                                         G_IO_IN | G_IO_HUP | G_IO_ERR,
                                         completion_pipe_read_callback,
                                         NULL);

  if (g_completion_watch_id == 0)
  {
    glog_error("Failed to add watch for completion pipe\n");
    g_io_channel_unref(g_completion_channel);
    g_completion_channel = NULL;
    return FALSE;
  }

  glog_trace("Completion pipe initialized successfully\n");
  return TRUE;
}

// 녹화 완료 처리 함수
static void process_recording_completion(const char *file_path)
{
  glog_trace("Processing recording completion for: %s\n", file_path);

  // 파일이 완전히 닫힐 때까지 잠시 대기
  sleep(2);
  
  // 파일 접근 가능 여부 확인
  if (access(file_path, R_OK) != 0) {
      glog_error("File not accessible yet, waiting longer: %s\n", file_path);
      sleep(3);
      
      if (access(file_path, R_OK) != 0) {
          glog_error("File still not accessible: %s\n", file_path);
          return;
      }
  }

  char http_url[512];
  int cam_idx = extract_camera_info_from_path(file_path, http_url);

  if (cam_idx >= 0)
  {
    // video_url 설정
    strncpy(g_curlinfo.video_url, http_url, sizeof(g_curlinfo.video_url) - 1);
    g_curlinfo.video_url[sizeof(g_curlinfo.video_url) - 1] = '\0';

    glog_trace("Sending notification for cam %d with URL: %s\n", cam_idx, http_url);
    notification_request(g_config.camera_id, "3", &g_curlinfo);
  }
}

// 파일 경로에서 카메라 정보 추출
static int extract_camera_info_from_path(const char *file_path, char *http_url)
{
  // 예: /home/nvidia/data/EVENT_20250617/CAM0_153308.mp4
  // -> http://IP/data/EVENT_20250617/CAM0_153308.mp4, cam_idx=0

  const char *data_pos = strstr(file_path, "/data/");
  if (!data_pos)
  {
    glog_error("Invalid file path format: %s\n", file_path);
    return -1;
  }

  // HTTP URL 생성
  snprintf(http_url, 512, "http://%s%s", g_config.http_service_ip, data_pos);

  // 카메라 인덱스 추출 (CAM0, CAM1 등에서)
  const char *cam_pos = strstr(file_path, "CAM");
  if (cam_pos && cam_pos[3] >= '0' && cam_pos[3] <= '9')
  {
    return cam_pos[3] - '0';
  }

  return 0; // 기본값
}

// 완료 파이프 정리
static void cleanup_completion_pipe(void)
{
  if (g_completion_watch_id > 0)
  {
    g_source_remove(g_completion_watch_id);
    g_completion_watch_id = 0;
  }

  if (g_completion_channel)
  {
    g_io_channel_unref(g_completion_channel);
    g_completion_channel = NULL;
  }

  if (access(g_completion_pipe_name, F_OK) == 0)
  {
    unlink(g_completion_pipe_name);
  }
}

static gboolean completion_pipe_read_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
  if (condition & G_IO_HUP)
  {
    glog_trace("Completion pipe closed, reopening...\n");
    if (g_completion_channel)
    {
      g_io_channel_unref(g_completion_channel);
      g_completion_channel = NULL;
    }
    g_completion_watch_id = 0;

    g_timeout_add(1000, (GSourceFunc)init_completion_pipe, NULL);
    return FALSE;
  }

  if (condition & G_IO_IN)
  {
    gchar *line = NULL;
    gsize length = 0;
    GError *error = NULL;
    GIOStatus status;

    // 한 줄씩 읽기
    status = g_io_channel_read_line(source, &line, &length, NULL, &error);

    if (status == G_IO_STATUS_NORMAL && line != NULL)
    {
      // 개행 문자 제거
      g_strstrip(line);

      glog_trace("Received completion signal: %s\n", line);

      // "RECORDING_COMPLETE:" 메시지 파싱
      if (g_str_has_prefix(line, "RECORDING_COMPLETE:"))
      {
        char *file_path = line + strlen("RECORDING_COMPLETE:");
        process_recording_completion(file_path);
      }

      g_free(line);
    }
    else if (status == G_IO_STATUS_ERROR)
    {
      if (error)
      {
        glog_error("Error reading completion pipe: %s\n", error->message);
        g_error_free(error);
      }
    }
  }

  if (condition & G_IO_ERR)
  {
    glog_error("Completion pipe error condition\n");
    return FALSE;
  }

  return TRUE;
}

int main(int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  GIOChannel *io_stdin;

  // redirect_output();

  init_logging("gstream_main");

  printf("=== gstrea_main start version [%s] ===\n", GSTREAM_MAIN_VER);

  glog_trace("=== gstrea_main start version [%s] ===\n", GSTREAM_MAIN_VER);
  context = g_option_context_new("- gstreamer main ");
  g_option_context_add_main_entries(context, entries, NULL);
  g_option_context_add_group(context, gst_init_get_option_group());
  if (!g_option_context_parse(context, &argc, &argv, &error))
  {
    glog_error("Error initializing: %s\n", error->message);
    return -1;
  }

  version_log();

  if (getenv("DISPLAY"))
  {
    printf("DISPLAY 환경변수 제거 중...\n");
    unsetenv("DISPLAY");
  }

  if (!g_config_name)
    g_config_name = g_strdup("config.json");

  if (!check_plugins())
    return -1;

  if (!load_config(g_config_name, &g_config, &g_curlinfo))
  {
    glog_error("fail load config : %s\n", g_config_name);
    return -1;
  }

  DeviceSetting default_setting;
  ensure_valid_settings_file(g_config.device_setting_path, &default_setting);

  if (!load_device_setting(g_config.device_setting_path, &g_setting))
  {
    glog_error("fail load device_setting : %s\n", g_config.device_setting_path);
    return -1;
  }

  // 정상 로드 후 백업 생성
  create_settings_backup(g_config.device_setting_path);

#if MINDULE_INCLUDE
  if (is_rest_server())
  {
    char fname[100] = "";
    get_ranch_setting_path(fname);
    if (load_ranch_setting(fname, &g_ranch_setting) == FALSE)
    {
      update_ranch_setting(fname, &g_ranch_setting);
    }
  }
  else
  {
    glog_trace("\'%s\' does not exist\n", REST_SERVER_PATH);
  }
#endif

  int pipe_fd = -1;
  if (fifo_exists(g_pipe_name))
  {
    pipe_fd = open(g_pipe_name, O_WRONLY | O_NONBLOCK);
    if (pipe_fd == -1)
    {
      glog_error("can not open pipe : %s\n", g_pipe_name);
      // return -1;
    }
    close(pipe_fd);
  }

  if (!open_serial(g_config.tty_name, 38400))
  {
    glog_error("fail open serial : %s\n", g_config.tty_name);
  }

  pthread_mutex_init(&g_send_mutex, NULL);
  pthread_mutex_init(&g_send_info_mutex, NULL);
  pthread_mutex_init(&g_retry_connect_mutex, NULL);
  pthread_mutex_init(&g_process_msg_mutex, NULL);

#if MINDULE_INCLUDE
  if (is_rest_server())
  {
    init_socket_server(SERVER_UDP_PORT, receive_data, process_data);
  }
  else
  {
    glog_trace("\'%s\' does not exist\n", REST_SERVER_PATH);
  }
#endif

  /* Sanitize by removing whitespace, modifies string in-place */
  glog_trace("Our local id is %s\n", g_config.camera_id);

  /* Don't use strict ssl when running a localhost server, because
   * it's probably a test server with a self-signed certificate */
  {
    GstUri *uri = gst_uri_from_string(g_config.server_ip);
    if (g_strcmp0("localhost", gst_uri_get_host(uri)) == 0 ||
        g_strcmp0("127.0.0.1", gst_uri_get_host(uri)) == 0)
      strict_ssl = FALSE;
    gst_uri_unref(uri);
  }

  loop = g_main_loop_new(NULL, FALSE);
  start_pipeline();

  connect_to_websocket_server_async();

  io_stdin = g_io_channel_unix_new(fileno(stdin));
  g_io_add_watch(io_stdin, G_IO_IN, (GIOFunc)handle_keyboard, NULL);

  init_webrtc_peer(g_config.max_stream_cnt, g_config.device_cnt,
                   g_config.stream_base_port, g_codec_name, g_config.comm_socket_port);

  if (!init_ptz_pipe())
  {
    glog_error("Failed to initialize PTZ command pipe\n");
  }

  if (!init_completion_pipe())
  {
    glog_error("Failed to initialize completion pipe\n");
  }

  // 주기적인 이벤트를 위한 GLib 타이머 설정
  if (g_config.status_timer_interval > 0)
  {
    start_heartbit(g_config.status_timer_interval);
  }

  guint source_id = 0;
  if (pipe_fd != -1)
  {
    source_id = g_timeout_add(5000, timer_camera_info_callback, NULL);
  }

  guint source_id_2 = 0;
  source_id_2 = g_timeout_add(1000, timer_check_callback, NULL);

  guint source_id_3 = 0;
  source_id_3 = g_timeout_add((1000 * 60), connect_check_callback, NULL);

  // main이 죽을 경우 child process 역시 죽도록 함
  atexit(on_handle_exit);
  signal(SIGABRT, handle_sigabrt);

  // get token..
  login_request(&g_curlinfo);
  glog_trace("g_curlinfo.token:%s\n", g_curlinfo.token); // LJH, 0923

  g_main_loop_run(loop);

  free_webrtc_peer(TRUE);
  gst_element_set_state(GST_ELEMENT(g_pipeline), GST_STATE_NULL);
  glog_trace("Pipeline stopped\n");

  if (source_id > 0)
    g_source_remove(source_id);
  if (source_id_2 > 0)
    g_source_remove(source_id_2);
  if (source_id_3 > 0)
    g_source_remove(source_id_3);

  if (g_config.status_timer_interval > 0)
    kill_heartbit();

  gst_object_unref(g_pipeline);
  free_config(&g_config);

  endup_nv_analysis();

  cleanup_ptz_pipe();

  cleanup_completion_pipe();

  pthread_mutex_destroy(&g_send_mutex);
  pthread_mutex_destroy(&g_process_msg_mutex);

  cleanup_logging();

  return 0;
}
