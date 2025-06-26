/*
 * GStreamer 1.16용 DTLS 안정화가 적용된 webrtc_sender
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
#include <unistd.h>

#include "socket_comm.h"
#define USE_JSON_MESSAGE_TEMPLATE
#include "json_utils.h"
#include "log_wrapper.h"

static GMainLoop *loop;
static GstElement *pipeline, *webrtc = NULL;

static SOCKETINFO* g_socket = NULL;

static int g_stream_cnt;
static int g_stream_base_port;
static int g_comm_port;
static char* g_codec_name;
static char* peer_id;

// DTLS 안정화 관련 변수들
static gboolean pipeline_created = FALSE;
static gint retry_count = 0;
static const gint max_retries = 3;
static guint retry_timeout_id = 0;

static GOptionEntry entries[] = {
  {"stream_cnt", 0, 0, G_OPTION_ARG_INT, &g_stream_cnt, "stream_cnt", NULL},
  {"stream_base_port", 0, 0, G_OPTION_ARG_INT, &g_stream_base_port, "stream_port", NULL},
  {"comm_socket_port", 0, 0, G_OPTION_ARG_INT, &g_comm_port, "comm_socket_port", NULL},
  {"codec_name", 0, 0, G_OPTION_ARG_STRING, &g_codec_name, "codec_name", NULL},
  {"peer_id", 0, 0, G_OPTION_ARG_STRING, &peer_id, "String ID of the peer to connect to", "ID"},
  {NULL}
};

enum AppState
{
  APP_STATE_UNKNOWN = 0,
  APP_STATE_ERROR = 1,
  SERVER_CONNECTING = 1000,
  SERVER_CONNECTION_ERROR,
  SERVER_CONNECTED,
  SERVER_REGISTERING = 2000,
  SERVER_REGISTRATION_ERROR,
  SERVER_REGISTERED,
  SERVER_CLOSED,
  ROOM_JOINING = 3000,
  ROOM_JOIN_ERROR,
  ROOM_JOINED,
  ROOM_CALL_NEGOTIATING = 4000,
  ROOM_CALL_OFFERING,
  ROOM_CALL_ANSWERING,
  ROOM_CALL_STARTED,
  ROOM_CALL_STOPPING,
  ROOM_CALL_STOPPED,
  ROOM_CALL_ERROR,
};
static enum AppState app_state = 0;

// 함수 전방 선언 (enum AppState 정의 이후)
static gboolean cleanup_and_quit_loop(const gchar * msg, enum AppState state);
static gboolean start_pipeline(void);
static gboolean restart_pipeline_on_dtls_error(gpointer user_data);

// DTLS 에러 감지 및 자동 재시작 함수
static gboolean restart_pipeline_on_dtls_error(gpointer user_data) {
    glog_error("DTLS error detected, attempting pipeline restart (attempt %d/%d)", 
               retry_count + 1, max_retries);
    
    if (retry_count >= max_retries) {
        glog_error("Maximum retry attempts reached, giving up");
        cleanup_and_quit_loop("DTLS connection failed after retries", APP_STATE_ERROR);
        return G_SOURCE_REMOVE;
    }
    
    retry_count++;
    
    // 기존 파이프라인 완전 정리
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
        webrtc = NULL;
        pipeline_created = FALSE;
    }
    
    // 잠시 대기 후 재시작
    g_usleep(1000000);  // 1초 대기
    
    // 파이프라인 재생성
    if (start_pipeline()) {
        glog_trace("Pipeline restarted successfully");
        app_state = ROOM_JOINED;  // 상태 복원
    } else {
        glog_error("Pipeline restart failed");
        return restart_pipeline_on_dtls_error(user_data);
    }
    
    return G_SOURCE_REMOVE;
}

// 개선된 버스 메시지 핸들러
static gboolean bus_message_handler(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            
            glog_error("GStreamer Error: %s", error->message);
            if (debug) {
                glog_error("Debug info: %s", debug);
                
                // DTLS 관련 에러 감지
                if (strstr(debug, "dtls") || strstr(debug, "DTLS") || 
                    strstr(debug, "bio_buffer") || strstr(error->message, "dtls")) {
                    glog_error("DTLS error detected in bus message");
                    
                    // 재시작 스케줄링 (중복 방지)
                    if (retry_timeout_id == 0) {
                        retry_timeout_id = g_timeout_add(2000, restart_pipeline_on_dtls_error, NULL);
                    }
                }
            }
            
            g_error_free(error);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_WARNING: {
            gchar *debug;
            GError *warning;
            gst_message_parse_warning(msg, &warning, &debug);
            
            glog_trace("GStreamer Warning: %s", warning->message);
            
            // DTLS 경고도 감지
            if (debug && (strstr(debug, "dtls") || strstr(debug, "DTLS"))) {
                glog_trace("DTLS warning detected: %s", warning->message);
            }
            
            g_error_free(warning);
            g_free(debug);
            break;
        }
        default:
            break;
    }
    
    return TRUE;
}

static gboolean
cleanup_and_quit_loop (const gchar * msg, enum AppState state)
{
  if (msg)
    glog_error ("%s\n", msg);
  if (state > 0)
    app_state = state;

  // 재시도 타이머 정리
  if (retry_timeout_id > 0) {
    g_source_remove(retry_timeout_id);
    retry_timeout_id = 0;
  }

  if (loop) {
    g_main_loop_quit (loop);
    loop = NULL;
  }

  return G_SOURCE_REMOVE;
}

static gchar *
get_string_from_json_object (JsonObject * object)
{
  JsonNode *root;
  JsonGenerator *generator;
  gchar *text;

  root = json_node_init_object (json_node_alloc (), object);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);

  g_object_unref (generator);
  json_node_free (root);
  return text;
}

static void
send_room_peer_msg (const gchar * action, const gchar * peer_id, const gchar * message_key, const gchar * message_value)
{
  gchar *msg;
  msg = g_strdup_printf (json_msssage_template, action, peer_id, message_key ,message_value);
  send_data_socket_comm(g_socket, msg, strlen(msg), 1);
  g_free (msg);
}

static int first_send_ice = TRUE;
static void
send_ice_candidate_message (GstElement * webrtc G_GNUC_UNUSED, guint mlineindex,
    gchar * candidate, const gchar * data)
{
  gchar *text;
  JsonObject *ice;

  if (app_state < ROOM_CALL_OFFERING) {
    cleanup_and_quit_loop ("Can't send ICE, not in call", APP_STATE_ERROR);
    return;
  }
  
  // ICE 전송 전 지연 시간 증가 (DTLS 안정화)
  if(first_send_ice){
    usleep(100000);  // 100ms로 증가
    first_send_ice = FALSE;
  }

  ice = json_object_new ();
  json_object_set_string_member (ice, "candidate", candidate);
  json_object_set_int_member (ice, "sdpMLineIndex", mlineindex);
  text = get_string_from_json_object (ice);
  json_object_unref (ice);

  send_room_peer_msg ("candidate", peer_id, "ice", text);
  g_free (text);
}

static void
send_room_peer_sdp (GstWebRTCSessionDescription * desc, const gchar * peer_id)
{
  JsonObject *sdp;
  gchar *text, *sdptype, *sdptext;

  g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);

  if (desc->type == GST_WEBRTC_SDP_TYPE_OFFER)
    sdptype = "offer";
  else if (desc->type == GST_WEBRTC_SDP_TYPE_ANSWER)
    sdptype = "answer";
  else
    g_assert_not_reached ();

  text = gst_sdp_message_as_text (desc->sdp);
  glog_trace ("Sending sdp %s to %s:\n%s\n", sdptype, peer_id, text);

  sdp = json_object_new ();
  json_object_set_string_member (sdp, "type", sdptype);
  json_object_set_string_member (sdp, "sdp", text);
  g_free (text);

  sdptext = get_string_from_json_object (sdp);
  json_object_unref (sdp);
  
  send_room_peer_msg (sdptype, peer_id, "sdp", sdptext);
  g_free (sdptext);
}

static void
on_offer_created (GstPromise * promise, GstElement * webrtc)
{
  GstWebRTCSessionDescription *offer;
  const GstStructure *reply;

  g_assert_cmpint (app_state, ==, ROOM_CALL_OFFERING);

  g_assert_cmpint (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "offer",
      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
  gst_promise_unref (promise);

  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-local-description", offer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  send_room_peer_sdp (offer, peer_id);
  gst_webrtc_session_description_free (offer);
}

static void
on_negotiation_needed (GstElement * webrtc, const gchar * data)
{
  GstPromise *promise;
  app_state = ROOM_CALL_OFFERING;
  glog_trace ("[%s] on_negotiation_needed called \n", peer_id);

  // DTLS 안정화를 위한 추가 대기
  g_usleep(200000);  // 200ms 대기

  promise = gst_promise_new_with_change_func (
      (GstPromiseChangeFunc) on_offer_created, (gpointer) webrtc, NULL);
  g_signal_emit_by_name (webrtc, "create-offer", NULL, promise);
}

static void
on_ice_gathering_state_notify (GstElement * webrtcbin, GParamSpec * pspec, gpointer data)
{
  GstWebRTCICEGatheringState ice_gather_state;
  const gchar *new_state = "unknown";
  
  g_object_get (webrtcbin, "ice-gathering-state", &ice_gather_state, NULL);
  switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
      new_state = "new";
      break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
      new_state = "gathering";
      break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
      new_state = "complete";
      // ICE complete 후 DTLS 준비 시간
      g_usleep(100000);  // 100ms 대기
      break;
  }
  glog_trace ("[%s] ICE gathering state changed to %s \n", peer_id, new_state);
}

// 개선된 파이프라인 시작 함수
static gboolean start_pipeline (void)
{
  GstStateChangeReturn ret;
  GError *error = NULL;
  GstBus *bus;
  
  if (pipeline_created) {
    glog_trace("Pipeline already created, skipping");
    return TRUE;
  }

  glog_trace("Creating new pipeline with DTLS stabilization");

  char str_pipeline[2048] = {0,};
  char str_video[512];

  // STUN 서버를 여러 개 시도할 수 있도록 설정
  strcpy(str_pipeline, "webrtcbin stun-server=stun://stun.l.google.com:19302 name=sender ");
  
  for( int i = 0 ; i< g_stream_cnt ;i++){
    snprintf(str_video, 512, 
      "udpsrc port=%d ! queue ! "
      "application/x-rtp,media=video,encoding-name=%s,payload=96,"
      "width=(int)1920,height=(int)1080,framerate=(fraction)15/1 ! "
      "sender. ",  
      g_stream_base_port + i, g_codec_name); 
    strcat(str_pipeline, str_video);
  }
  
  glog_trace("%lu  %s\n", strlen(str_pipeline), str_pipeline);
  pipeline = gst_parse_launch (str_pipeline, &error);

  if (error) {
    glog_error ("Failed to parse launch: %s\n", error->message);
    g_error_free (error);
    goto err;
  }

  webrtc = gst_bin_get_by_name (GST_BIN (pipeline), "sender");
  g_assert_nonnull (webrtc);

  // 버스 메시지 핸들러 추가 (DTLS 에러 감지용)
  bus = gst_element_get_bus(pipeline);
  gst_bus_add_watch(bus, bus_message_handler, pipeline);
  gst_object_unref(bus);

  // WebRTC 시그널 연결
  g_signal_connect (webrtc, "on-negotiation-needed",
      G_CALLBACK (on_negotiation_needed), NULL);
  g_signal_connect (webrtc, "on-ice-candidate",
      G_CALLBACK (send_ice_candidate_message), NULL);
  g_signal_connect (webrtc, "notify::ice-gathering-state",
      G_CALLBACK (on_ice_gathering_state_notify), NULL);

  // 단계적 상태 변경 (DTLS 안정화)
  glog_trace ("Setting pipeline to READY\n");
  gst_element_set_state (pipeline, GST_STATE_READY);
  g_usleep(500000);  // 500ms 대기

  glog_trace ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  pipeline_created = TRUE;
  app_state = ROOM_JOINED;
  
  // 성공 시 재시도 카운터 리셋
  retry_count = 0;
  
  return TRUE;

err:
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_clear_object (&pipeline);
  }
  if (webrtc)
    webrtc = NULL;
  pipeline_created = FALSE;
  return FALSE;
}

// 나머지 함수들은 기존과 동일...
static void
on_answer_created (GstPromise * promise, const gchar * peer_id)
{
  GstWebRTCSessionDescription *answer;
  const GstStructure *reply;

  g_assert_cmpint (app_state, ==, ROOM_CALL_ANSWERING);

  g_assert_cmpint (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "answer",
      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
  gst_promise_unref (promise);

  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-local-description", answer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  send_room_peer_sdp (answer, peer_id);
  gst_webrtc_session_description_free (answer);

  app_state = ROOM_CALL_STARTED;
}

static void
handle_sdp_offer (const gchar * peer_id, const gchar * text)
{
  int ret;
  GstPromise *promise;
  GstSDPMessage *sdp;
  GstWebRTCSessionDescription *offer;

  g_assert_cmpint (app_state, ==, ROOM_CALL_ANSWERING);

  glog_trace ("Received offer:\n%s\n", text);

  ret = gst_sdp_message_new (&sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  ret = gst_sdp_message_parse_buffer ((guint8 *) text, strlen (text), sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  offer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_OFFER, sdp);
  g_assert_nonnull (offer);

  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-remote-description", offer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  promise = gst_promise_new_with_change_func (
      (GstPromiseChangeFunc) on_answer_created, (gpointer) peer_id, NULL);
  g_signal_emit_by_name (webrtc, "create-answer", NULL, promise);

  gst_webrtc_session_description_free (offer);
  gst_object_unref (webrtc);
}

static void
handle_sdp_answer (const gchar * peer_id, const gchar * text)
{
  int ret;
  GstPromise *promise;
  GstSDPMessage *sdp;
  GstWebRTCSessionDescription *answer;

  g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);

  glog_trace ("Received answer:\n%s\n", text);

  ret = gst_sdp_message_new (&sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  ret = gst_sdp_message_parse_buffer ((guint8 *) text, strlen (text), sdp);
  g_assert_cmpint (ret, ==, GST_SDP_OK);

  answer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
  g_assert_nonnull (answer);

  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc, "set-remote-description", answer, promise);
  
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);
}

static void handle_peer_message (gchar * msg, int len, void* arg)
{ 
  if(strcmp(msg, "ENDUP") == 0){
    glog_trace ("ENDUP Message ... Exit program %s\n",  msg);  
    exit(0); 
    return ;
  }
  
  JsonNode *root;
  JsonObject *object, *child;
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, msg, -1, NULL)) {
    glog_error ("Unknown message '%s' from '%s', ignoring", msg, peer_id);
    g_object_unref (parser);
    return;
  }

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    glog_error ("Unknown json message '%s' from '%s', ignoring", msg, peer_id);
    g_object_unref (parser);
    return;
  }

  glog_trace ("Message from peer %s: %s\n", peer_id, msg);

  object = json_node_get_object (root);
  
  if (json_object_has_member (object, "sdp")) {
    const gchar *text, *sdp_type;

    g_assert_cmpint (app_state, >=, ROOM_JOINED);

    child = json_object_get_object_member (object, "sdp");

    if (!json_object_has_member (child, "type")) {
      cleanup_and_quit_loop ("ERROR: received SDP without 'type'", ROOM_CALL_ERROR);
      return;
    }

    sdp_type = json_object_get_string_member (child, "type");
    text = json_object_get_string_member (child, "sdp");

    if (g_strcmp0 (sdp_type, "offer") == 0) {
      app_state = ROOM_CALL_ANSWERING;
      handle_sdp_offer (peer_id, text);
    } else if (g_strcmp0 (sdp_type, "answer") == 0) {
      g_assert_cmpint (app_state, >=, ROOM_CALL_OFFERING);
      handle_sdp_answer (peer_id, text);
      app_state = ROOM_CALL_STARTED;
    } else {
      cleanup_and_quit_loop ("ERROR: invalid sdp_type", ROOM_CALL_ERROR);
      return;
    }
  } else if (json_object_has_member (object, "ice")) {
    const gchar *candidate;
    gint sdpmlineindex;

    child = json_object_get_object_member (object, "ice");
    candidate = json_object_get_string_member (child, "candidate");
    sdpmlineindex = json_object_get_int_member (child, "sdpMLineIndex");

    g_signal_emit_by_name (webrtc, "add-ice-candidate", sdpmlineindex, candidate);
  } else {
    glog_error ("Ignoring unknown JSON message:\n%s\n", msg);
  }
  g_object_unref (parser);
}

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;

  init_logging("sender");

  context = g_option_context_new ("- gstreamer webrtc sender ");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    glog_error ("Error initializing: %s\n", error->message);
    return -1;
  }

  glog_trace("start peer_id : [%s], comm_port[%d], stream_port[%d], stream_cnt[%d] codec_name[%s]\n", 
      peer_id, g_comm_port, g_stream_base_port, g_stream_cnt, g_codec_name);

  // 소켓 연결
  g_socket = init_socket_comm_client(g_comm_port);
  send_data_socket_comm(g_socket, "CONNECT", 8, 1);
  g_socket->call_fun = handle_peer_message;

  // WebRTC 파이프라인 시작
  start_pipeline();
  
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  if (pipeline) {
    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
    gst_object_unref (pipeline);
  }
  
  glog_trace ("Pipeline stopped end client [%d] \n", g_comm_port);

  cleanup_logging();

  return 0;
}