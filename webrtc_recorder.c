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

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include "log_wrapper.h"
#include "video_convert.h"
#include <libgen.h>


static GMainLoop *loop;
static GstElement *pipeline;

static int g_stream_cnt;
static int g_stream_base_port;
static int g_comm_port;
static char* g_codec_name;
static char g_rtp_depay_name[64];

static char* g_location;
static int g_duration;

// 디스크 안전장치 관련 전역 변수
static gboolean g_nvme_available = FALSE;
static guint g_disk_check_timer = 0;

static GOptionEntry entries[] = {
  {"stream_cnt", 0, 0, G_OPTION_ARG_INT, &g_stream_cnt, "stream_cnt", NULL},                    //"device_cnt" in config.json
  {"stream_base_port", 0, 0, G_OPTION_ARG_INT, &g_stream_base_port, "stream_port", NULL},
  {"codec_name", 0, 0, G_OPTION_ARG_STRING, &g_codec_name, "codec_name", NULL},
  {"location", 0, 0, G_OPTION_ARG_STRING, &g_location, "store path", NULL},
  {"duration", 0, 0, G_OPTION_ARG_INT, &g_duration, "duratio (second)", NULL},
  {NULL}
};


static char g_filename[2][512];
static char g_time[2][256];

// 데이터 흐름 통계
static guint64 g_total_bytes[2] = {0, 0};
static guint g_packet_count[2] = {0, 0};
static guint g_rtp_packet_count[2] = {0, 0};

// Function to get the directory name from a file path
void get_directory_name(const char *file_path, char *directory) 
{
  // Copy the file path into a mutable buffer
  char path_copy[512];
  strncpy(path_copy, file_path, sizeof(path_copy) - 1);
  path_copy[sizeof(path_copy) - 1] = '\0';

  // Use the dirname() function to get the directory part of the path
  char *dir_name = dirname(path_copy);

  // Copy the directory name into the output parameter
  strncpy(directory, dir_name, 512);
  directory[511] = '\0';  // Ensure the string is null-terminated
}

// 디스크 상태 체크 함수들
static gboolean check_nvme_mount(void) 
{
    char command[256];
    char result[1024];
    FILE *fp;
    
    // nvme 마운트 상태 확인
    snprintf(command, sizeof(command), "df -h | grep nvme0n1");
    fp = popen(command, "r");
    
    if (!fp) {
        glog_error("Failed to execute mount check command\n");
        return FALSE;
    }
    
    if (fgets(result, sizeof(result), fp)) {
        // nvme가 마운트되어 있음
        pclose(fp);
        
        // 사용량 체크 (80% 이상이면 경고)
        snprintf(command, sizeof(command), 
                "df -h %s | tail -1 | awk '{print $5}' | sed 's/%%//'", g_location);
        fp = popen(command, "r");
        
        if (fp && fgets(result, sizeof(result), fp)) {
            int usage = atoi(result);
            glog_trace("NVME usage: %d%%\n", usage);
            if (usage >= 80) {
                glog_error("NVME disk usage high: %d%%\n", usage);
                if (usage >= 95) {
                    glog_critical("NVME disk almost full! Stopping recording!\n");
                    pclose(fp);
                    return FALSE;
                }
            }
            pclose(fp);
        }
        return TRUE;
    }
    
    pclose(fp);
    glog_error("NVME not mounted or not available\n");
    return FALSE;
}

// eMMC 용량 체크 함수
static gboolean check_emmc_space(void) 
{
    char command[256];
    char result[128];
    FILE *fp;
    
    // 루트 파티션(eMMC) 사용량 확인
    snprintf(command, sizeof(command), 
            "df -h / | tail -1 | awk '{print $5}' | sed 's/%%//'");
    fp = popen(command, "r");
    
    if (fp && fgets(result, sizeof(result), fp)) {
        int usage = atoi(result);
        glog_trace("eMMC usage: %d%%\n", usage);
        
        if (usage >= 90) {
            glog_critical("eMMC usage critical: %d%%. Stopping all recording!\n", usage);
            pclose(fp);
            return FALSE;
        } else if (usage >= 80) {
            glog_error("eMMC usage high: %d%%\n", usage);
        }
        pclose(fp);
        return TRUE;
    }
    
    if (fp) pclose(fp);
    return FALSE;
}

// 주기적 디스크 상태 모니터링
static gboolean monitor_disk_status(gpointer data) 
{
    static int check_count = 0;
    check_count++;
    
    glog_debug("=== Disk Status Check #%d ===\n", check_count);
    
    // 1. eMMC 용량 체크 (최우선)
    if (!check_emmc_space()) {
        glog_critical("eMMC full! Emergency stop!\n");
        g_main_loop_quit(loop);
        return FALSE;
    }
    
    // 2. NVME 상태 체크
    gboolean nvme_status = check_nvme_mount();
    
    if (g_nvme_available && !nvme_status) {
        // NVME가 사용 가능했는데 갑자기 불가능해짐
        glog_critical("NVME became unavailable during recording!\n");
        glog_critical("This may cause recording to fail or fill up eMMC!\n");
        
        // 파이프라인 정지 고려
        glog_error("Stopping pipeline to prevent eMMC overflow\n");
        g_main_loop_quit(loop);
        return FALSE;
    }
    
    g_nvme_available = nvme_status;
    
    return TRUE;  // 계속 모니터링
}

// 버스 메시지 콜백 함수
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *main_loop = (GMainLoop*)data;
    
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            glog_trace("End of stream\n");
            g_main_loop_quit(main_loop);
            break;
            
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            glog_error("GStreamer Error: %s\n", error->message);
            if (debug) {
                glog_error("Debug info: %s\n", debug);
            }
            g_error_free(error);
            g_free(debug);
            break;
        }
        
        case GST_MESSAGE_WARNING: {
            gchar *debug;
            GError *warning;
            gst_message_parse_warning(msg, &warning, &debug);
            glog_trace("Warning: %s\n", warning->message);
            g_error_free(warning);
            g_free(debug);
            break;
        }
        
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                glog_trace("Pipeline state changed from %s to %s\n",
                    gst_element_state_get_name(old_state),
                    gst_element_state_get_name(new_state));
            }
            break;
        }
        
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            const gchar *name = gst_structure_get_name(s);
            
            // splitmuxsink 메시지 모니터링
            if (g_str_has_prefix(name, "splitmuxsink")) {
                if (g_strcmp0(name, "splitmuxsink-fragment-opened") == 0) {
                    const gchar *location = gst_structure_get_string(s, "location");
                    if (location) {
                        glog_trace("Started recording to: %s\n", location);
                    }
                }
                else if (g_strcmp0(name, "splitmuxsink-fragment-closed") == 0) {
                    const gchar *location = gst_structure_get_string(s, "location");
                    if (location) {
                        glog_trace("Finished recording to: %s\n", location);
                    }
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return TRUE;
}

// UDP 데이터 프로브 함수
static GstPadProbeReturn udp_data_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    int cam_idx = *(int*)user_data;
    
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        gsize size = gst_buffer_get_size(buffer);
        g_total_bytes[cam_idx] += size;
        g_packet_count[cam_idx]++;
        
        // 100개 패킷마다 로그
        // if (g_packet_count[cam_idx] % 100 == 0) {
        //     glog_debug("CAM%d UDP: %u packets, %lu total bytes\n", 
        //               cam_idx, g_packet_count[cam_idx], g_total_bytes[cam_idx]);
        // }
    }
    
    return GST_PAD_PROBE_OK;
}

// RTP 데이터 프로브 함수
static GstPadProbeReturn rtp_data_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    int cam_idx = *(int*)user_data;
    
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        g_rtp_packet_count[cam_idx]++;
        
        // 50개 RTP 패킷마다 로그
        // if (g_rtp_packet_count[cam_idx] % 50 == 0) {
        //     glog_debug("CAM%d RTP: %u H.264 packets processed\n", 
        //               cam_idx, g_rtp_packet_count[cam_idx]);
        // }
    }
    
    return GST_PAD_PROBE_OK;
}

// 주기적 상태 체크 함수
static gboolean check_data_flow(gpointer data) 
{
    static int check_count = 0;
    check_count++;
    
    glog_debug("=== Data Flow Check #%d ===\n", check_count);
    
    // 각 카메라별 통계
    for (int i = 0; i < g_stream_cnt; i++) {
        glog_debug("CAM%d: UDP=%u pkts (%lu bytes), RTP=%u pkts\n", 
                  i, g_packet_count[i], g_total_bytes[i], g_rtp_packet_count[i]);
    }
    
    // 파일 크기 확인 (간단화)
    char find_cmd[1024];
    for (int i = 0; i < g_stream_cnt; i++) {
        snprintf(find_cmd, sizeof(find_cmd), 
                "ls -la %s/RECORD_*/CAM%d_*.mp4 2>/dev/null | tail -1", 
                g_location, i);
        FILE *fp = popen(find_cmd, "r");
        if (fp) {
            char result[256];
            if (fgets(result, sizeof(result), fp)) {
                glog_trace("CAM%d latest file: %s", i, result);
            }
            pclose(fp);
        }
    }
    
    return TRUE;  // 계속 실행
}

// 개선된 안전한 파일 저장 핸들러
gchar* safe_formatted_file_saving_handler(GstChildProxy *splitmux, guint fragment_id, gpointer user_data)
{
    struct tm *local_time;
    time_t t;
    int cam_idx = *(int *)user_data;
    
    // 디스크 상태 재확인
    if (!check_emmc_space()) {
        glog_critical("eMMC full during recording! Stopping!\n");
        g_main_loop_quit(loop);
        return NULL;
    }
    
    // NVME 상태 확인
    gboolean current_nvme_status = check_nvme_mount();
    char *target_location = g_location;
    
    if (!current_nvme_status && g_nvme_available) {
        // NVME 상태 변경됨
        glog_error("NVME issue detected during recording for CAM%d\n", cam_idx);
        
        // Fallback을 사용할지 결정 (여기서는 에러 처리)
        glog_critical("Cannot continue recording safely. Stopping.\n");
        g_main_loop_quit(loop);
        return NULL;
    }
    
    t = time(NULL);
    local_time = localtime(&t);
    
    // 코덱에 따른 파일 확장자 결정
    const char* file_extension;
    if (strcmp("VP9", g_codec_name) == 0 || strcmp("VP8", g_codec_name) == 0) {
        file_extension = ".webm";
    } else if (strcmp("H264", g_codec_name) == 0) {
        file_extension = ".mp4";
    } else {
        file_extension = ".mkv";
    }
    
    // 날짜 폴더 생성
    sprintf(g_time[cam_idx], "RECORD_%04d%02d%02d", 
            local_time->tm_year + 1900, local_time->tm_mon+1, local_time->tm_mday);
    sprintf(g_filename[cam_idx], "%s/%s", target_location, g_time[cam_idx]);
    
    struct stat info;
    if (stat(g_filename[cam_idx], &info) != 0) {
        if (mkdir(g_filename[cam_idx], 0777) != 0) {
            glog_error("Failed to create directory: %s\n", g_filename[cam_idx]);
            return NULL;
        }
    }
    
    sprintf(g_time[cam_idx], "RECORD_%04d%02d%02d/CAM%d_%02d%02d%02d", 
            local_time->tm_year + 1900, local_time->tm_mon+1, local_time->tm_mday, 
            cam_idx, local_time->tm_hour, local_time->tm_min, local_time->tm_sec);
    
    sprintf(g_filename[cam_idx], "%s/%s%s", target_location, g_time[cam_idx], file_extension);
    glog_trace("Safe file generation: CAM%d -> %s\n", cam_idx, g_filename[cam_idx]);
    
    return g_strdup_printf("%s", g_filename[cam_idx]);
}

static gboolean start_pipeline (void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;

  char str_pipeline[4096] = {0,};
  char str_video[1024];

  const char* muxer_factory;
  const char* parse_element;
  
  if (strcmp("VP9", g_codec_name) == 0) {
    muxer_factory = "webmmux";
    parse_element = "";  // VP9는 추가 파싱 불필요
  } else if (strcmp("VP8", g_codec_name) == 0) {
    muxer_factory = "webmmux"; 
    parse_element = "";  // VP8도 추가 파싱 불필요
  } else if (strcmp("H264", g_codec_name) == 0) {
    muxer_factory = "mp4mux";
    parse_element = "h264parse ! ";  // H.264는 파싱 필요
  } else {
    glog_error("Unsupported codec: %s\n", g_codec_name);
    return FALSE;
  }

  int base_port = 7000;

  for( int i = 0 ; i < g_stream_cnt ;i++){
    snprintf(str_video, sizeof(str_video), 
      "udpsrc port=%d name=udpsrc%d ! queue name=queue%d ! application/x-rtp,media=video,clock-rate=90000,encoding-name=%s,payload=96 ! %s name=%s%d ! "
      "%s"  // H.264인 경우 h264parse 추가
      "splitmuxsink location=%s max-size-time=%ld name=recorder%d muxer-factory=%s async-finalize=true ",
        base_port + i, i,  // udpsrc name
        i,  // queue name
        g_codec_name, 
        g_rtp_depay_name, g_rtp_depay_name, i,  // depay name
        parse_element,  // H.264: "h264parse ! ", VP9/VP8: ""
        g_location, 
        g_duration*60000000000, 
        i,
        muxer_factory);  // 동적 muxer 사용
    strcat(str_pipeline, str_video);
  }

  glog_trace("Pipeline length: %lu\nPipeline: %s\n", strlen(str_pipeline), str_pipeline);
  pipeline = gst_parse_launch(str_pipeline, &error);
  
  if (error) {
    glog_error("Failed to parse launch: %s\n", error->message);
    g_error_free(error);
    goto err;
  }

  // 버스 메시지 모니터링 추가
  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, bus_call, loop);
  gst_object_unref(bus);
  
  // 각 udpsrc와 depay에 데이터 프로브 추가
  static int cam_indices[2] = {0, 1};
  for (int i = 0; i < g_stream_cnt; i++) {
    char element_name[64];
    
    // UDP 데이터 프로브
    snprintf(element_name, sizeof(element_name), "udpsrc%d", i);
    GstElement *udpsrc = gst_bin_get_by_name(GST_BIN(pipeline), element_name);
    if (udpsrc) {
      GstPad *src_pad = gst_element_get_static_pad(udpsrc, "src");
      if (src_pad) {
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                         udp_data_probe, &cam_indices[i], NULL);
        glog_debug("Added UDP data probe for CAM%d (port %d)\n", i, base_port + i);
        gst_object_unref(src_pad);
      }
      gst_object_unref(udpsrc);
    }
    
    // RTP depay 프로브
    char depay_name[128];  // 버퍼 크기 증가
    snprintf(depay_name, sizeof(depay_name), "%s%d", g_rtp_depay_name, i);
    GstElement *depay = gst_bin_get_by_name(GST_BIN(pipeline), depay_name);
    if (depay) {
      GstPad *src_pad = gst_element_get_static_pad(depay, "src");
      if (src_pad) {
        gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                         rtp_data_probe, &cam_indices[i], NULL);
        glog_debug("Added RTP data probe for CAM%d\n", i);
        gst_object_unref(src_pad);
      }
      gst_object_unref(depay);
    }
  }

  // splitmuxsink 콜백 연결 (안전한 핸들러 사용)
  static int cam_idx[2] = {0,1}; 
  for( int i = 0 ; i< g_stream_cnt ;i++){
    char str_element_name[256];
    sprintf(str_element_name, "recorder%d", i);
    GstElement *recorder = gst_bin_get_by_name (GST_BIN (pipeline), str_element_name);
    g_assert_nonnull (recorder);
    if(recorder){
      g_signal_connect_data( recorder, "format-location", G_CALLBACK (safe_formatted_file_saving_handler), &cam_idx[i], NULL, 0);
    } else {
      return FALSE;
    }
    g_clear_object (&recorder);
  }

  glog_trace ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  // 주기적 데이터 체크 시작 (10초마다)
  g_timeout_add(10000, check_data_flow, NULL);
  
  return TRUE;

err:
  if (pipeline)
    g_clear_object (&pipeline);
  return FALSE;
}

// 안전한 녹화 시작 함수
static gboolean start_safe_pipeline(void)
{
    // 1. 초기 디스크 상태 확인
    glog_trace("Checking disk status before starting...\n");
    
    if (!check_emmc_space()) {
        glog_critical("eMMC space insufficient. Cannot start recording.\n");
        return FALSE;
    }
    
    g_nvme_available = check_nvme_mount();
    
    if (!g_nvme_available) {
        glog_error("NVME not available. Recording may fill up eMMC!\n");
        glog_error("Consider fixing NVME before starting recording.\n");
        
        // 선택: NVME 없으면 아예 시작하지 않기
        glog_critical("Cannot start recording without NVME available.\n");
        return FALSE;
        
        // 또는 경고만 하고 계속 진행 (위험함)
        // glog_error("Proceeding with recording despite NVME issue...\n");
    } else {
        glog_trace("NVME available and healthy\n");
    }
    
    // 2. 기존 파이프라인 시작
    if (!start_pipeline()) {
        return FALSE;
    }
    
    // 3. 디스크 모니터링 시작 (30초마다)
    g_disk_check_timer = g_timeout_add(30000, monitor_disk_status, NULL);
    glog_trace("Started disk monitoring (every 30 seconds)\n");
    
    return TRUE;
}

int main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;

  init_logging("recorder");

  context = g_option_context_new ("- gstreamer webrtc recorder ");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    glog_error ("Error initializing: %s\n", error->message);
    return -1;
  }

  glog_trace("start stream_port[%d], stream_cnt[%d], codec_name[%s], location [%s]  duration[%d] \n", 
       g_stream_base_port, g_stream_cnt, g_codec_name, g_location, g_duration);
  
  if (strcmp("VP9",g_codec_name) == 0){
    strcpy(g_rtp_depay_name,"rtpvp9depay");
  } else if(strcmp("VP8",g_codec_name) == 0){
    strcpy(g_rtp_depay_name,"rtpvp8depay");
  } else if(strcmp("H264",g_codec_name) == 0){
    strcpy(g_rtp_depay_name,"rtph264depay");
  } else {
    glog_error ("Wrong Codec : %s\n", g_codec_name);
    return  -1;
  }

  // 안전한 파이프라인 시작
  if (!start_safe_pipeline()) {
    glog_critical("Failed to start safe pipeline\n");
    cleanup_logging();
    return -1;
  }
  
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  // 정리
  if (g_disk_check_timer > 0) {
    g_source_remove(g_disk_check_timer);
  }
  
  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  glog_trace ("Pipeline stopped safely [%d] \n", g_comm_port);

  gst_object_unref (pipeline);
  cleanup_logging();

  return 0;
}