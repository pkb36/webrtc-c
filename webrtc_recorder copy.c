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


gchar* formatted_file_saving_handler(GstChildProxy *splitmux, guint fragment_id, gpointer user_data)
{
  struct tm *local_time;
  time_t t;
  int cam_idx = *(int *)user_data;

  t = time(NULL);
  local_time = localtime(&t);

  //make folder name
  sprintf (g_time[cam_idx], "RECORD_%04d%02d%02d",  local_time->tm_year + 1900,local_time->tm_mon+1,local_time->tm_mday);
  sprintf (g_filename[cam_idx], "%s/%s", g_location, g_time[cam_idx]);
  
  // date 폴더가 존재하는지 확인하고 없을 경우는 폴더를 생성함 
  struct stat info;
  if (stat(g_filename[cam_idx], &info) != 0) { 
    // stat 함수가 실패하면 폴더가 존재하지 않는 것으로 간주
    mkdir(g_filename[cam_idx], 0777);
  }
  
  sprintf (g_time[cam_idx], "RECORD_%04d%02d%02d/CAM%d_%02d%02d%02d", local_time->tm_year + 1900,local_time->tm_mon+1,local_time->tm_mday, 
    cam_idx, local_time->tm_hour, local_time->tm_min, local_time->tm_sec);
  
  sprintf(g_filename[cam_idx], "%s/%s.webm", g_location, g_time[cam_idx]);
  glog_trace("generate file  g_filename[%d]=%s\n", cam_idx, g_filename[cam_idx]);

  return g_strdup_printf("%s", g_filename[cam_idx]);
} 


static gboolean
start_pipeline (void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;

  /* NOTE: webrtcbin currently does not support dynamic addition/removal of
   * streams, so we use a separate webrtcbin for each peer, but all of them are
   * inside the same pipeline. We start by connecting it to a fakesink so that
   * we can preroll early. */

  char str_pipeline[2048] = {0,};
  char str_video[512];
  for( int i = 0 ; i < g_stream_cnt ;i++){     //g_stream_cnt == "device_cnt" in config.json == 2(==> RGB(5000), Thermal(5001))
    snprintf(str_video, sizeof(str_video), 
      "udpsrc port=%d ! queue ! application/x-rtp,media=video,clock-rate=90000,encoding-name=%s, payload=96  ! %s ! \
        splitmuxsink location=%s max-size-time=%ld name=recorder%d muxer-factory=matroskamux async-finalize=true muxer-properties=\"properties,offset-to-zero=true\"  ",
        g_stream_base_port + i, g_codec_name, g_rtp_depay_name, g_location, g_duration*60000000000, i); 
    strcat(str_pipeline, str_video);
  }
  glog_trace("%lu  %s\n", strlen(str_pipeline), str_pipeline);
  pipeline = gst_parse_launch (str_pipeline, &error);
  if (error) {
    glog_error ("Failed to parse launch: %s\n", error->message);
    g_error_free (error);
    goto err;
  }

  static int cam_idx[2] =  {0,1}; 
  for( int i = 0 ; i< g_stream_cnt ;i++){
    char str_element_name[256];
    sprintf(str_element_name, "recorder%d", i);
    GstElement *recorder = gst_bin_get_by_name (GST_BIN (pipeline), str_element_name);
    g_assert_nonnull (recorder);
    if(recorder){
      g_signal_connect_data( recorder, "format-location", G_CALLBACK (formatted_file_saving_handler), &cam_idx[i], NULL, 0);
    } else {
      return FALSE;
    }
    g_clear_object (&recorder);
  }

  glog_trace ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  return TRUE;

err:
  if (pipeline)
    g_clear_object (&pipeline);
  return FALSE;
}


int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;

  //@@Check nvme0n1이 mount 되었는지 확인...
  char command[100];
  char folderPath[] = "/dev/nvme0n1";  

  // df 명령어 실행을 위한 명령어 문자열 생성
  snprintf(command, sizeof(command), "df -h %s", folderPath);

  // popen을 사용하여 명령어 실행
  FILE *dfOutput = popen(command, "r");
  if (dfOutput == NULL) {
      glog_error("command [%s] error \n", command);
      return EXIT_FAILURE;
  }
  
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

  //1. start webrtc
  start_pipeline();
  
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
  glog_trace ("Pipeline stopped end recorder [%d] \n", g_comm_port);

  gst_object_unref (pipeline);
  return 0;
}
