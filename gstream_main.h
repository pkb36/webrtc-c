#ifndef __GSTREAMER_MAIN_H__
#define __GSTREAMER_MAIN_H__

#include <gst/gst.h>
#include "json_utils.h"
#include "log_wrapper.h"
#include "version.h"
#include "global_define.h"

enum AppState
{
	APP_STATE_UNKNOWN = 0,
	APP_STATE_ERROR = 1,	 /* generic error */
	APP_STATE_ERROR_TIMEOUT, /* generic error */
	SERVER_CONNECTING = 1000,
	SERVER_CONNECTION_ERROR,
	SERVER_CONNECTED, /* Ready to register */
	SERVER_REGISTERING = 2000,
	SERVER_REGISTRATION_ERROR,
	SERVER_REGISTERED, /* Ready to call a peer */
	SERVER_CLOSED,	   /* server connection closed by us or the server */
	ROOM_JOINING = 3000,
	ROOM_JOIN_ERROR,
	ROOM_JOINED,
	ROOM_CALL_NEGOTIATING = 4000, /* negotiating with some or all peers */
	ROOM_CALL_OFFERING,			  /* when we're the one sending the offer */
	ROOM_CALL_ANSWERING,		  /* when we're the one answering an offer */
	ROOM_CALL_STARTED,			  /* in a call with some or all peers */
	ROOM_CALL_STOPPING,
	ROOM_CALL_STOPPED,
	ROOM_CALL_ERROR,
};

// Enum to represent internet connection states
typedef enum
{
	INIT,
	INTERNET_CONNECTED,
	INTERNET_DISCONNECTED,
	INTERNET_RECONNECTED
} InternetState;

typedef struct
{
	// 네트워크 설정
	gint rgb_port;
	gint thermal_port;
	const gchar *udp_host;
	gint base_port_high; // 5000
	gint base_port_low;	 // 5100
	gint num_streams;	 // 12

	// 비디오 설정
	gint rgb_width;
	gint rgb_height;
	gint thermal_width;
	gint thermal_height;
	gint rgb_output_width;
	gint rgb_output_height;
	gint thermal_output_width;
	gint thermal_output_height;

	gint rgb_flip_method;	 // 0: none, 1: horizontal, 2: vertical, 3: both

	// 스냅샷 설정
	gint snapshot_width_rgb;
	gint snapshot_height_rgb;
	gint snapshot_width_thermal;
	gint snapshot_height_thermal;
	const gchar *snapshot_path_rgb;
	const gchar *snapshot_path_thermal;

	// 인코더 설정
	gint bitrate_high_rgb;
	gint bitrate_low_rgb;
	gint bitrate_high_thermal;
	gint bitrate_low_thermal;

	// AI 모델 설정
	const gchar *model_config_rgb;
	const gchar *model_config_thermal;

	// 성능 설정
	gint queue_size;
	gint low_framerate;
} PipelineConfig;

#define MAX_WAIT_REPLY_CNT 5

#define PING_TEST "ping -c 1 8.8.8.8 > /dev/null 2>&1"

void send_msg_server(const gchar *msg);
// implement process_cmd
int execute_process(char *cmd, gboolean check_id);

gboolean process_message_cmd(gJSONObj *jsonObj);

gboolean apply_setting();
gboolean cleanup_and_retry_connect(const gchar *msg, enum AppState state);

void start_heartbit(int timeout);
void kill_heartbit();

PipelineConfig *get_default_config();
gchar *build_udp_source(gint port, gint flip_method);
gchar *build_snapshot_branch(const gchar *tee_name, gint width, gint height, const gchar *location);
gchar *build_inference_branch(const gchar *tee_name, const gchar *mux_name,
							  gint width, gint height, const gchar *config_file,
							  const gchar *nvinfer_name, const gchar *postproc_name,
							  const gchar *osd_name);
gchar *build_encoder_branch(gint output_width, gint output_height,
							gint bitrate, const gchar *parse_name,
							const gchar *tee_name);
gchar *build_low_res_branch(const gchar *tee_name, gint framerate,
							gint width, gint height, gint bitrate,
							const gchar *enc_tee_name);
gchar *build_udp_sinks(PipelineConfig *config);
gchar *build_complete_pipeline(PipelineConfig *config);

extern gboolean cleanup_and_retry_connect(const gchar *msg, enum AppState state);
extern void send_camera_info_to_server();
extern int is_process_running(const char *process_name);
extern int get_temp(int index);
extern void dec_temp_event_time_gap();

#endif
