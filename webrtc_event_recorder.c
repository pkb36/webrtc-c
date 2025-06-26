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
#include <stdint.h>
#include <fcntl.h>      // open, O_WRONLY, O_NONBLOCK 등을 위해 추가
#include <unistd.h>     // write, close 등을 위해 추가
#include <errno.h>      // strerror를 위해 추가
#include "log_wrapper.h"

static GMainLoop *main_loop;
static GstElement *pipeline;

static int g_stream_cnt;
static int g_stream_base_port;
static char* g_codec_name;
static char g_rtp_depay_name[64];

static char* g_location;
static int g_duration;

static GOptionEntry entries[] = {
  {"stream_cnt", 0, 0, G_OPTION_ARG_INT, &g_stream_cnt, "stream_cnt", NULL},
  {"stream_base_port", 0, 0, G_OPTION_ARG_INT, &g_stream_base_port, "stream_port", NULL},
  {"codec_name", 0, 0, G_OPTION_ARG_STRING, &g_codec_name, "codec_name", NULL},
  {"location", 0, 0, G_OPTION_ARG_STRING, &g_location, "store path", NULL},
  {"duration", 0, 0, G_OPTION_ARG_INT, &g_duration, "duratio (second)", NULL},
  {NULL}
};

static int recording_started = 0;

// 버스 메시지 핸들러
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      glog_trace("End of stream received - recording completed\n");
      g_main_loop_quit(main_loop);
      break;
    case GST_MESSAGE_ERROR: {
      GError *err;
      gchar *debug;
      gst_message_parse_error(msg, &err, &debug);
      glog_error("Pipeline error: %s\n", err->message);
      glog_error("Debug info: %s\n", debug ? debug : "none");
      g_error_free(err);
      g_free(debug);
      g_main_loop_quit(main_loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err;
      gchar *debug;
      gst_message_parse_warning(msg, &err, &debug);
      glog_trace("Pipeline warning: %s\n", err->message);
      g_error_free(err);
      g_free(debug);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

// timeout 콜백 - 지정된 시간 후 EOS 전송
static gboolean timeout_callback(gpointer user_data) {
  glog_trace("Recording duration (%d seconds) reached, sending EOS\n", g_duration);
  
  if (pipeline) {
    // EOS 이벤트 전송으로 정상 종료
    gst_element_send_event(pipeline, gst_event_new_eos());
  }
  
  return FALSE;  // 타이머 제거
}

// format-location 핸들러 - exit() 제거
gchar* formatted_file_saving_handler(GstChildProxy *splitmux, guint fragment_id, gpointer user_data){
  if(!recording_started){
    glog_trace("start record [%s]\n", g_location);
    recording_started = 1;
  } else {
    glog_trace("format-location called: fragment_id=%d, location=[%s]\n", fragment_id, g_location);
  }
  return g_strdup(g_location);  // 문자열 복사본 반환
} 

static gboolean
start_pipeline (void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;
  
  char str_pipeline[4096] = {0,};
  char str_video[1024];
  const char* muxer_factory;
  const char* parse_element;
  
  // 코덱별 muxer와 파서 설정
  if (strcmp("VP9", g_codec_name) == 0) {
    muxer_factory = "webmmux";
    parse_element = "";  // VP9는 추가 파싱 불필요
  } else if (strcmp("VP8", g_codec_name) == 0) {
    muxer_factory = "webmmux"; 
    parse_element = "";  // VP8도 추가 파싱 불필요
  } else if (strcmp("H264", g_codec_name) == 0) {
    muxer_factory = "mp4mux";
    parse_element = "h264parse config-interval=-1 ! ";  // H.264는 파싱 필요, SPS/PPS 주기적 삽입
  } else {
    glog_error("Unsupported codec: %s\n", g_codec_name);
    return FALSE;
  }
  
  int base_port = g_stream_base_port;
  
  for( int i = 0 ; i < g_stream_cnt ; i++){
    snprintf(str_video, sizeof(str_video), 
      "udpsrc port=%d name=udpsrc%d ! "
      "queue name=queue%d max-size-buffers=100 leaky=downstream ! "
      "application/x-rtp,media=video,clock-rate=90000,encoding-name=%s,payload=96 ! "
      "%s name=%s%d ! "
      "%s"  // H.264인 경우 h264parse 추가
      "splitmuxsink location=%s max-size-time=0 name=recorder%d "
      "muxer-factory=%s async-finalize=false sync=false ",
        base_port + i, i,  // udpsrc port와 name
        i,  // queue name
        g_codec_name, 
        g_rtp_depay_name, g_rtp_depay_name, i,  // depay name
        parse_element,  // H.264: "h264parse ! ", VP9/VP8: ""
        g_location, 
        i,
        muxer_factory);  // 동적 muxer 사용
    
    strcat(str_pipeline, str_video);
  }
  
  glog_trace("Pipeline length: %lu\n%s\n", strlen(str_pipeline), str_pipeline);
  
  pipeline = gst_parse_launch (str_pipeline, &error);
  if (error) {
    glog_error ("Failed to parse launch: %s\n", error->message);
    g_error_free (error);
    goto err;
  }

  // 버스 메시지 핸들러 설정
  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, bus_call, NULL);
  gst_object_unref(bus);

  static int cam_idx[2] = {0,1}; 
  for( int i = 0 ; i< g_stream_cnt ;i++){
    char str_element_name[256];
    sprintf(str_element_name, "recorder%d", i);
    GstElement *recorder = gst_bin_get_by_name (GST_BIN (pipeline), str_element_name);
    if(recorder){
      g_signal_connect_data( recorder, "format-location", G_CALLBACK (formatted_file_saving_handler), &cam_idx[i], NULL, 0);
      g_clear_object (&recorder);
    } else {
      glog_error("Could not find recorder element: %s\n", str_element_name);
      goto err;
    }
  }
  
  glog_trace ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    glog_error("Failed to set pipeline to PLAYING state\n");
    goto err;
  }

  return TRUE;

err:
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_clear_object (&pipeline);
  }
  return FALSE;
}

void send_completion_signal(const char* file_path) {
    const char* pipe_name = "/home/nvidia/webrtc/recording_completion_pipe";
    int pipe_fd;
    char message[1024];
    
    // 파이프 열기 (논블로킹)
    pipe_fd = open(pipe_name, O_WRONLY | O_NONBLOCK);
    if (pipe_fd == -1) {
        glog_error("Failed to open completion pipe: %s\n", strerror(errno));
        return;
    }
    
    // 완료 메시지 작성: "RECORDING_COMPLETE:파일경로"
    snprintf(message, sizeof(message), "RECORDING_COMPLETE:%s\n", file_path);
    
    // 메시지 전송
    ssize_t bytes_written = write(pipe_fd, message, strlen(message));
    if (bytes_written == -1) {
        glog_error("Failed to write to completion pipe: %s\n", strerror(errno));
    } else {
        glog_trace("Sent completion signal: %s", message);
    }
    
    close(pipe_fd);
}

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  
  context = g_option_context_new ("- gstreamer webrtc event recorder ");
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

  // 파이프라인 시작
  if (!start_pipeline()) {
    glog_error("Failed to start pipeline\n");
    return -1;
  }
  
  main_loop = g_main_loop_new (NULL, FALSE);
  
  // timeout 콜백 활성화 - 지정된 시간 후 EOS 전송
  guint timeout_duration = g_duration * 1000;  // 밀리초 변환
  g_timeout_add(timeout_duration, timeout_callback, NULL);
  
  glog_trace("Starting main loop, will record for %d seconds\n", g_duration);
  g_main_loop_run (main_loop);
  
  glog_trace ("Main loop finished, cleaning up\n");
  
  // 정상 종료 처리
  if (pipeline) {
    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
    gst_object_unref (pipeline);
  }
  
  if (main_loop) {
    g_main_loop_unref(main_loop);
  }

  glog_trace ("Recording completed successfully\n");

  send_completion_signal(g_location);
  return 0;
}