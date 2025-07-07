/*
 * Demo gstreamer app for negotiating and streaming a sendrecv audio-only webrtc
 * stream to all the peers in a multiparty room.
 *
 * gcc mp-webrtc-sendrecv.c $(pkg-config --cflags --libs gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0) -o mp-webrtc-sendrecv
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 */
#include <gst/gst.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <stdbool.h>
#include <nvbufsurface.h>
#include <arpa/inet.h>

#include "config.h"
#include "gstream_main.h"
#include "gstnvdsmeta.h"
#include "nvds_process.h"
#include "curllib.h"
#include "device_setting.h"
#include "nvds_opticalflow_meta.h"
#include "nvds_utils.h"

static int *g_cam_indices = NULL;
#define MAX_OPT_FLOW_ITERATIONS 1000
// 이벤트 전송 설정
static gchar *g_event_tcp_host = "127.0.0.1";
static gint g_event_tcp_port = EVENT_TCP_PORT;

#define MAX_DETECTION_BUFFER_SIZE 10000 // 최대 10000 프레임
#define BUFFER_DURATION_SEC 120			// 120초 버퍼

typedef struct
{
	DetectionData buffer[MAX_DETECTION_BUFFER_SIZE];
	gint head;	  // 버퍼의 시작 위치
	gint tail;	  // 버퍼의 끝 위치
	gint count;	  // 현재 저장된 아이템 수
	GMutex mutex; // 스레드 안전을 위한 뮤텍스
} CameraBuffer;

static CameraBuffer camera_buffers[NUM_CAMS];
static gboolean buffers_initialized = FALSE;

int g_cam_index = 0;
int g_noti_cam_idx = 0;
int g_top = 0, g_left = 0, g_width = 0, g_height = 0;
int g_move_to_center_running = 0;
int g_frame_count[2];
static pthread_t g_tid;
int g_event_class_id = CLASS_NORMAL_COW;
int g_event_recording = 0;
#if 0
Timer timers[MAX_PTZ_PRESET];
#endif
static int g_event_sender_running = 1;
ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];

int threshold_event_duration[NUM_CLASSES] =
	{
		0,	// NORMAL
		15, // HEAT
		15, // FLIP
		15, // LABOR SIGN
		0,	// NORMAL_SITTING
};

float threshold_confidence[NUM_CLASSES] =
	{
		0.2, // NORMAL
		0.8, // HEAT
		0.8, // FLIP
		0.8, // LABOR SIGN
		0.2, // NORMAL_SITTING
};

void *event_sender_thread(void *arg)
{
    while (g_event_sender_running)
    {
        if (g_event_class_id != CLASS_NORMAL_COW && g_event_class_id != EVENT_EXIT)
        {
            int class_id = g_event_class_id;
            g_event_class_id = CLASS_NORMAL_COW;  // 초기화
            
            int ret = send_notification_to_server(class_id);
			if (ret < 1)
			{
				glog_error("Failed to send event notification to server for class %d\n", class_id);
			}
			else
			{
				glog_info("Event notification sent successfully for class %d\n", class_id);
			}
        }
        
        usleep(100000);  // 100ms 대기
    }
    
    return NULL;
}

static gboolean event_recording_timeout(gpointer data)
{
	g_event_recording = 0;
	glog_trace("Event recording finished\n");
	return G_SOURCE_REMOVE;
}

static GMutex buffer_init_mutex;
// 카메라 버퍼 초기화 함수
void init_camera_buffers()
{
    g_mutex_lock(&buffer_init_mutex);
    
    if (buffers_initialized) {
        glog_trace("[init_camera_buffers] Already initialized\n");
        g_mutex_unlock(&buffer_init_mutex);
        return;
    }

    glog_info("[init_camera_buffers] Initializing camera buffers\n");

    for (int i = 0; i < NUM_CAMS; i++)
    {
        camera_buffers[i].head = 0;
        camera_buffers[i].tail = 0;
        camera_buffers[i].count = 0;
        g_mutex_init(&camera_buffers[i].mutex);
        
        glog_trace("[init_camera_buffers] Initialized buffer for camera %d\n", i);
    }
    
    buffers_initialized = TRUE;
    glog_info("[init_camera_buffers] All buffers initialized successfully\n");
    
    g_mutex_unlock(&buffer_init_mutex);
}

// 검출 데이터를 버퍼에 추가하는 함수
void add_detection_to_buffer(guint camera_id, guint frame_number,
							 NvDsObjectMetaList *obj_meta_list, NvDsFrameMeta *frame_meta)
{
	if (camera_id >= NUM_CAMS)
		return;

	CameraBuffer *cam_buf = &camera_buffers[camera_id];

	g_mutex_lock(&cam_buf->mutex);

	// 새 검출 데이터 생성
	DetectionData *det = &cam_buf->buffer[cam_buf->tail];
	GDateTime *now = g_date_time_new_now_local();
	det->timestamp = g_date_time_to_unix(now) * 1000000000; // 초를 나노초로
	gint microseconds = g_date_time_get_microsecond(now); // 마이크로초 가져오기
	det->timestamp += microseconds * 1000; // 마이크로초를 나노초로 변환해서 추가

	g_date_time_unref(now);
	det->frame_number = frame_number;
	det->camera_id = camera_id;
	det->num_objects = 0;

	// 객체 메타데이터 복사
	NvDsObjectMeta *obj_meta = NULL;
	gint obj_count = 0;

	gint frame_width = frame_meta->source_frame_width;
	gint frame_height = frame_meta->source_frame_height;

	for (NvDsObjectMetaList *l = obj_meta_list; l != NULL && obj_count < NUM_OBJS;
		 l = l->next)
	{
		obj_meta = (NvDsObjectMeta *)l->data;

		det->objects[obj_count].class_id = obj_meta->class_id;
		det->objects[obj_count].confidence = obj_meta->confidence;
		det->objects[obj_count].x = obj_meta->rect_params.left / (gfloat)frame_width;
		det->objects[obj_count].y = obj_meta->rect_params.top / (gfloat)frame_height;
		det->objects[obj_count].width = obj_meta->rect_params.width / (gfloat)frame_width;
		det->objects[obj_count].height = obj_meta->rect_params.height / (gfloat)frame_height;
		// printf("Object %d: class_id=%d, confidence=%.2f, bbox=(%.2f, %.2f, %.2f, %.2f)\n",
		// 	   obj_count, det->objects[obj_count].class_id,
		// 	   det->objects[obj_count].confidence,
		// 	   det->objects[obj_count].x, det->objects[obj_count].y,
		// 	   det->objects[obj_count].width, det->objects[obj_count].height);
			   
		det->objects[obj_count].bbox_color = get_object_color(camera_id,
															  obj_meta->object_id % NUM_OBJS,
															  obj_meta->class_id);
		det->objects[obj_count].has_bbox = (det->objects[obj_count].bbox_color != BBOX_NONE);

		obj_count++;
	}
	det->num_objects = obj_count;

	// 원형 버퍼 업데이트
	cam_buf->tail = (cam_buf->tail + 1) % MAX_DETECTION_BUFFER_SIZE;
	if (cam_buf->count < MAX_DETECTION_BUFFER_SIZE)
	{
		cam_buf->count++;
	}
	else
	{
		// 버퍼가 가득 찬 경우 head도 이동
		cam_buf->head = (cam_buf->head + 1) % MAX_DETECTION_BUFFER_SIZE;
	}

	// 오래된 데이터 제거 (120초 이상)
	guint64 current_time = g_get_real_time() * 1000;
	guint64 cutoff_time = current_time - (BUFFER_DURATION_SEC * 1000000000ULL);

	while (cam_buf->count > 0)
	{
		DetectionData *oldest = &cam_buf->buffer[cam_buf->head];
		if (oldest->timestamp >= cutoff_time)
			break;

		cam_buf->head = (cam_buf->head + 1) % MAX_DETECTION_BUFFER_SIZE;
		cam_buf->count--;
	}

	g_mutex_unlock(&cam_buf->mutex);
}

void set_tracker_analysis(gboolean OnOff)
{
	char element_name[32];

	for (int cam_idx = 0; cam_idx < g_config.device_cnt; cam_idx++)
	{
		GstElement *dspostproc;
		sprintf(element_name, "dspostproc_%d", cam_idx + 1);
		dspostproc = gst_bin_get_by_name(GST_BIN(g_pipeline), element_name);
		if (dspostproc == NULL)
		{
			glog_trace("Fail get %s element\n", element_name);
			continue;
		}

		gboolean rest_val = OnOff ? FALSE : TRUE;
		g_object_set(G_OBJECT(dspostproc), "reset-object", rest_val, NULL);
		g_clear_object(&dspostproc);
	}
}

BboxColor get_object_color(guint camera_id, guint object_id, gint class_id)
{
	// PTZ 이동 중
	if (g_move_speed > 0)
	{
		return BBOX_NONE;
	}

	static float small_obj_diag[2] = {40.0, 40.0};
	static float big_obj_diag[2] = {1000.0, 1000.0};

	// 너무 작거나 큰 객체
	if (obj_info[camera_id][object_id].diagonal < small_obj_diag[camera_id] ||
		obj_info[camera_id][object_id].diagonal > big_obj_diag[camera_id])
	{
		return BBOX_NONE;
	}

	// 클래스별 색상
	switch (class_id)
	{
	case CLASS_NORMAL_COW:
	case CLASS_NORMAL_COW_SITTING:
		return BBOX_GREEN;

	case CLASS_HEAT_COW:
		if (g_setting.resnet50_apply && obj_info[camera_id][object_id].heat_count > 0)
		{
			return BBOX_RED;
		}
		return BBOX_YELLOW;

	case CLASS_FLIP_COW:
		if (g_setting.opt_flow_apply &&
			obj_info[camera_id][object_id].opt_flow_detected_count > 0)
		{
			return BBOX_RED;
		}
		return BBOX_YELLOW;

	case CLASS_LABOR_SIGN_COW:
		return BBOX_RED;

	case CLASS_OVER_TEMP:
		return BBOX_BLUE;

	default:
		return BBOX_GREEN;
	}
}

void set_process_analysis(gboolean OnOff)
{
    if (OnOff == 0)
        check_events_for_notification(0, 1);

    gboolean all_success = TRUE;

    for (int cam_idx = 0; cam_idx < g_config.device_cnt; cam_idx++)
    {
        char element_name[32];
        GstElement *nvinfer = NULL;
        GstElement *dspostproc = NULL;
        
        // nvinfer 처리
        sprintf(element_name, "nvinfer_%d", cam_idx + 1);
        nvinfer = gst_bin_get_by_name(GST_BIN(g_pipeline), element_name);
        if (nvinfer) {
            gint interval = OnOff ? g_setting.nv_interval : G_MAXINT;
            g_object_set(G_OBJECT(nvinfer), "interval", interval, NULL);
            g_clear_object(&nvinfer);
        } else {
            glog_error("Failed to get %s element\n", element_name);
            all_success = FALSE;
        }

        // dspostproc 처리
        sprintf(element_name, "dspostproc_%d", cam_idx + 1);
        dspostproc = gst_bin_get_by_name(GST_BIN(g_pipeline), element_name);
        if (dspostproc) {
            gboolean reset_val = OnOff ? FALSE : TRUE;
            g_object_set(G_OBJECT(dspostproc), "reset-object", reset_val, NULL);
            g_clear_object(&dspostproc);
        } else {
            glog_error("Failed to get %s element\n", element_name);
            all_success = FALSE;
        }
    }
    
    if (!all_success) {
        glog_error("Some elements failed to update\n");
    }
}

int is_process_running(const char *process_name)
{
	char command[256];

	snprintf(command, sizeof(command), "ps aux | grep '%s' | grep -v grep", process_name);
	FILE *fp = popen(command, "r");
	if (fp == NULL)
	{
		perror("popen");
		return -1;
	}
	// Check if there's any output from the command
	char buffer[256];
	while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
		// If we read a line, the process is running
		fclose(fp);
		return 1; // Process is running
	}
	fclose(fp);
	return 0; // Process is not running
}

gboolean send_event_to_recorder_simple(int class_id, int camera_id)
{
	int sock = 0;
	struct sockaddr_in serv_addr;

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		glog_error("이벤트 소켓 생성 실패");
		return FALSE;
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(g_event_tcp_port);

	if (inet_pton(AF_INET, g_event_tcp_host, &serv_addr.sin_addr) <= 0)
	{
		glog_error("잘못된 주소");
		close(sock);
		return FALSE;
	}

	// 타임아웃 설정
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);

	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
	{
		glog_error("이벤트 서버 연결 실패");
		close(sock);
		return FALSE;
	}

	// ISO 8601 형식의 타임스탬프 생성
	GDateTime *now = g_date_time_new_now_local();
	gchar *timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S.%f");

	time_t raw_time;
	gettimeofday(&tv, NULL);
	raw_time = tv.tv_sec;
	struct tm *timeinfo = localtime(&raw_time);

	char timestamp_buf[64];
	snprintf(timestamp_buf, sizeof(timestamp_buf),
			 "%04d-%02d-%02dT%02d:%02d:%02d.%06ld",
			 timeinfo->tm_year + 1900,
			 timeinfo->tm_mon + 1,
			 timeinfo->tm_mday,
			 timeinfo->tm_hour,
			 timeinfo->tm_min,
			 timeinfo->tm_sec,
			 tv.tv_usec);

	// 클래스 이름 매핑
	const char *class_names[] = {
		"normal_cow",
		"heat_cow",
		"flip_cow",
		"labor_sign_cow",
		"normal_cow_sitting",
		"over_temp"};

	const char *camera_name = g_config.camera_id;

	// 최신 검출 데이터 가져오기
	DetectionData latest;
	gboolean has_detection = get_latest_detection(camera_id, &latest);

	// JSON 이벤트 데이터 생성
	GString *json_str = g_string_new(NULL);
	g_string_append_printf(json_str,
						   "{"
						   "\"type\":\"manual_trigger\","
						   "\"event_class\":%d," // CLASS enum 값 추가
						   "\"camera\":\"%s\","
						   "\"camera_type\":%d,"
						   "\"timestamp\":\"%s\","
						   "\"metadata\":{"
						   "\"timestamp\":\"%s\","
						   "\"camera\":\"%s\","
						   "\"event_class\":%d," // metadata에도 추가
						   "\"objects\":[",
						   class_id, // event_class 값
						   camera_name,
						   camera_id,
						   timestamp_buf,
						   timestamp_buf,
						   camera_name,
						   class_id // metadata의 event_class 값
	);

	// 검출된 객체가 있으면 추가
	if (has_detection && latest.num_objects > 0)
	{
		for (guint i = 0; i < latest.num_objects; i++)
		{
			if (i > 0)
				g_string_append(json_str, ",");

			const char *obj_class = class_id < NUM_CLASSES ? class_names[class_id] : "unknown";
			const char *color_names[] = {"green", "yellow", "red", "blue", "null"};

			g_string_append_printf(json_str,
								   "{"
								   "\"class\":\"%s\","
								   "\"confidence\":%.2f,"
								   "\"bbox\":[%.2f,%.2f,%.2f,%.2f],"
								   "\"bbox_color\":\"%s\","
								   "\"has_bbox\":%s"
								   "}",
								   obj_class,
								   latest.objects[i].confidence,
								   latest.objects[i].x,
								   latest.objects[i].y,
								   latest.objects[i].x + latest.objects[i].width,
								   latest.objects[i].y + latest.objects[i].height,
								   color_names[latest.objects[i].bbox_color],
								   latest.objects[i].has_bbox ? "true" : "false");
		}
	}
	else
	{
		// 검출된 객체가 없어도 이벤트 클래스 정보는 포함
		const char *obj_class = class_id < NUM_CLASSES ? class_names[class_id] : "unknown";
		g_string_append_printf(json_str,
							   "{"
							   "\"class\":\"%s\","
							   "\"confidence\":0.95,"
							   "\"bbox\":[0,0,0,0],"
							   "\"bbox_color\":null,"
							   "\"has_bbox\":false"
							   "}",
							   obj_class);
	}

	g_string_append(json_str, "]}}");

	// 메시지 길이 + 데이터 전송
	guint32 msg_len = json_str->len;
	guint32 msg_len_be = htonl(msg_len); // 빅 엔디안으로 변환

	send(sock, &msg_len_be, 4, 0);
	send(sock, json_str->str, msg_len, 0);

	// 응답 대기
	char response[1024];
	int n = recv(sock, response, 1024, 0);
	if (n > 0)
	{
		response[n] = '\0';
		glog_trace("이벤트 서버 응답: %s\n", response);
	}

	g_string_free(json_str, TRUE);
	g_free(timestamp);
	g_date_time_unref(now);
	close(sock);

	glog_trace("이벤트 전송 완료: class_id=%d, camera=%s\n", class_id, camera_name);

	return TRUE;
}

int send_notification_to_server(int class_id)
{
	int cam_idx = RGB_CAM;

	glog_trace("try sending class_id=%d, enable_event_notify=%d\n", class_id, g_setting.enable_event_notify);

	if (g_setting.enable_event_notify)
	{
		// position 체크 제거 - 항상 이 경로로만 실행됨
		cam_idx = g_noti_cam_idx;
		glog_trace("g_noti_cam_idx=%d,g_source_cam_idx=%d\n", g_noti_cam_idx, g_source_cam_idx);

		if (g_noti_cam_idx != g_source_cam_idx)
		{
			glog_trace("g_noti_cam_idx=%d and g_source_cam_idx=%d are different, so return\n", g_noti_cam_idx, g_source_cam_idx);
			return FALSE;
		}

		g_event_recording = 1;
		g_timeout_add_seconds(30, event_recording_timeout, NULL);

		if (send_event_to_recorder_simple(class_id, cam_idx) == TRUE)
		{
			glog_trace("send_event_to_recorder_simple cam_idx=%d,class_id=%d\n", cam_idx, class_id);
			return TRUE;
		}
	}

	return FALSE;
}

void gather_event(int class_id, int obj_id, int cam_idx)
{
	if (obj_id < 0)
		return;
	if (class_id != CLASS_NORMAL_COW && class_id != CLASS_NORMAL_COW_SITTING)
	{
		obj_info[cam_idx][obj_id].detected_frame_count++;
		obj_info[cam_idx][obj_id].class_id = class_id;
	}
}

void init_opt_flow(int cam_idx, int obj_id, int is_total)
{
	if (g_setting.opt_flow_apply == 0)
	{
		return;
	}

	obj_info[cam_idx][obj_id].opt_flow_check_count = 0;
	obj_info[cam_idx][obj_id].move_size_avg = 0.0;
	obj_info[cam_idx][obj_id].do_opt_flow = 0;
	if (is_total)
	{
		obj_info[cam_idx][obj_id].opt_flow_detected_count = 0;
		obj_info[cam_idx][obj_id].prev_x = 0;
		obj_info[cam_idx][obj_id].prev_y = 0;
		obj_info[cam_idx][obj_id].prev_width = 0;
		obj_info[cam_idx][obj_id].prev_height = 0;
		obj_info[cam_idx][obj_id].x = 0;
		obj_info[cam_idx][obj_id].y = 0;
		obj_info[cam_idx][obj_id].width = 0;
		obj_info[cam_idx][obj_id].height = 0;
	}
}

#if RESNET_50
void check_heat_count(int cam_idx, int obj_id)
{
	glog_debug("obj_info[%d][%d].heat_count=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].heat_count);
	if (obj_info[cam_idx][obj_id].heat_count < HEAT_COUNT_THRESHOLD)
	{
		obj_info[cam_idx][obj_id].notification_flag = 0;
	}
	obj_info[cam_idx][obj_id].heat_count = 0;
}
#endif

void check_events_for_notification(int cam_idx, int init)
{
	if (init)
	{
		for (int i = 0; i < NUM_CAMS; i++)
		{
			for (int j = 0; j < NUM_OBJS; j++)
			{
				obj_info[i][j].detected_frame_count = 0;
				obj_info[i][j].duration = 0;
				obj_info[i][j].temp_duration = 0;
				obj_info[i][j].class_id = CLASS_NORMAL_COW;
#if RESNET_50
				obj_info[i][j].heat_count = 0;
#endif
				init_opt_flow(i, j, 1);
			}
		}
		return;
	}

	for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++)
	{
		if (obj_info[cam_idx][obj_id].detected_frame_count >= (PER_CAM_SEC_FRAME - 1))
		{ // if detection continued one second
			// glog_trace("cam_idx=%d, obj_id=%d detected_frame_count=%d duration=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].detected_frame_count, obj_info[cam_idx][obj_id].duration);
			obj_info[cam_idx][obj_id].duration++;
			if (obj_info[cam_idx][obj_id].duration >= threshold_event_duration[obj_info[cam_idx][obj_id].class_id])
			{ // if duration lasted more than designated time
				obj_info[cam_idx][obj_id].duration = 0;
				// check_for_zoomin(g_total_rect_size, detect_count);      //LJH, in progress
				obj_info[cam_idx][obj_id].notification_flag = 1; // send notification later
#if RESNET_50
				if (g_setting.resnet50_apply)
				{
					if (obj_info[cam_idx][obj_id].class_id == CLASS_HEAT_COW)
					{
						check_heat_count(cam_idx, obj_id); // LJH, if heat count is zero, notification is cancelled
					}
				}
#endif
				glog_debug("[%d][%d].class_id=%d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].class_id);
			}

			if (g_setting.opt_flow_apply)
			{
				if (obj_info[cam_idx][obj_id].class_id == CLASS_FLIP_COW)
				{											   // if event was flip do optical flow analysis
					obj_info[cam_idx][obj_id].do_opt_flow = 1; // if detected frame count lasted equal or more than one second then do optical flow analysis
				}
				else
				{
					init_opt_flow(cam_idx, obj_id, 0);
				}
			}
		}
		else
		{ // if detection not continued for one second
			obj_info[cam_idx][obj_id].duration = 0;
			init_opt_flow(cam_idx, obj_id, 1);
		}
		obj_info[cam_idx][obj_id].detected_frame_count = 0;
	}
}

int get_opt_flow_result(int cam_idx, int obj_id)
{
	glog_debug("[%d][%d].confi=%.2f opt_flow_detected_count ==> %d\n", cam_idx, obj_id, obj_info[cam_idx][obj_id].confidence, obj_info[cam_idx][obj_id].opt_flow_detected_count);
	if (obj_info[cam_idx][obj_id].opt_flow_detected_count >= THRESHOLD_OVER_OPTICAL_FLOW_COUNT)
		return 1;
	return 0;
}

#if 0
#define NOTICATION_TIME_GAP 60       

int get_time_gap_result(int preset)               //need to apply to objects
{
  static int first[MAX_PTZ_PRESET] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  int ret = 0;

  if (first[preset]) {
    first[preset] = 0;
    init_timer(preset, NOTICATION_TIME_GAP);
    return 1;
  }

  ret = check_time_gap(preset);
  if (ret == 1) {
    init_timer(preset, NOTICATION_TIME_GAP);
  }

  return ret;
}
#endif

void trigger_notification(int cam_idx)
{
	for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++)
	{
		if (obj_info[cam_idx][obj_id].notification_flag)
		{
			obj_info[cam_idx][obj_id].notification_flag = 0;
			g_event_class_id = obj_info[cam_idx][obj_id].class_id;
			glog_trace("[15SEC] notification_flag==1,cam_idx=%d,obj_id=%d,g_event_class_id=%d,g_preset_index=%d\n", cam_idx, obj_id, g_event_class_id, g_preset_index);
#if OPTICAL_FLOW_INCLUDE
			if (g_setting.opt_flow_apply)
			{
				if (g_event_class_id == CLASS_FLIP_COW)
				{
					glog_trace("[15SEC] g_event_class_id==CLASS_FLIP_COW\n");
					if (get_opt_flow_result(cam_idx, obj_id) == 0)
					{
						glog_trace("[15SEC] get_opt_flow_result(cam_idx=%d,obj_id=%d) ==> 0\n", cam_idx, obj_id);
						init_opt_flow(cam_idx, obj_id, 1);
						continue;
					}
					init_opt_flow(cam_idx, obj_id, 1);
					glog_trace("[15SEC] get_opt_flow_result(cam_idx=%d,obj_id=%d) ==> 1\n", cam_idx, obj_id);
				}
			}
#endif
			g_noti_cam_idx = g_cam_index;
			glog_trace("[[[NOTIFICATION]]] [%d][%d].confi=%.2f,g_source_cam_idx=%d,g_noti_cam_idx=%d,g_event_class_id=%d \n", cam_idx, obj_id, obj_info[cam_idx][obj_id].confidence, g_source_cam_idx, g_noti_cam_idx, g_event_class_id);
			obj_info[cam_idx][obj_id].temp_event_time_gap = TEMP_EVENT_TIME_GAP;
		}
	}
}

void print_debug(NvDsObjectMeta *obj_meta)
{
	glog_debug("obj_meta->class_id=%d confi=%f obj_label=%s top=%d left=%d width=%d height=%d x_offset=%d y_offset=%d display_text=%s font_size=%d cam_idx=%d obj_id=%ld\n",
			   obj_meta->class_id, obj_meta->confidence, obj_meta->obj_label, (int)obj_meta->rect_params.top,
			   (int)obj_meta->rect_params.left, (int)obj_meta->rect_params.width, (int)obj_meta->rect_params.height,
			   (int)obj_meta->text_params.x_offset, (int)obj_meta->text_params.y_offset, obj_meta->text_params.display_text,
			   (int)obj_meta->text_params.font_params.font_size, g_cam_index, obj_meta->object_id);
}

#if OPTICAL_FLOW_INCLUDE

int get_opt_flow_object(int cam_idx, int start_obj_id)
{
	for (int obj_id = start_obj_id; obj_id < NUM_OBJS; obj_id++)
	{
		if (obj_info[cam_idx][obj_id].do_opt_flow)
		{ // if do_opt_flow is set, then it means that it is heat state
			return obj_id;
		}
	}

	return -1;
}

double update_average(double previous_average, int count, double new_value)
{
	return ((previous_average * (count - 1)) + new_value) / count;
}

int get_correction_value(double diagonal)
{
	int corr_value = 0; // Initialize to a default value

	// If diagonal is less than or equal to SMALL_BBOX_DIAGONAL, calculate the correction value
	if (diagonal <= SMALL_BBOX_DIAGONAL)
	{
		corr_value = (int)(((SMALL_BBOX_DIAGONAL - diagonal) / 10) + 1);
	}

	return corr_value;
}

int get_move_distance(int cam_idx, int obj_id)
{
	if (obj_info[cam_idx][obj_id].prev_x == 0 || obj_info[cam_idx][obj_id].prev_y == 0)
		return 0;

	int x_dist = abs(obj_info[cam_idx][obj_id].prev_x - obj_info[cam_idx][obj_id].x);
	int y_dist = abs(obj_info[cam_idx][obj_id].prev_y - obj_info[cam_idx][obj_id].y);

	return (int)calculate_sqrt((double)x_dist, (double)y_dist);
}

int get_rect_size_change(int cam_idx, int obj_id)
{
	if (obj_info[cam_idx][obj_id].prev_width == 0 || obj_info[cam_idx][obj_id].prev_height == 0)
		return 0;

	int width_change = abs(obj_info[cam_idx][obj_id].prev_width - obj_info[cam_idx][obj_id].width);
	int height_change = abs(obj_info[cam_idx][obj_id].prev_height - obj_info[cam_idx][obj_id].height);

	return (int)calculate_sqrt((double)width_change, (double)height_change);
}

void set_prev_xy(int cam_idx, int obj_id)
{
	obj_info[cam_idx][obj_id].prev_x = obj_info[cam_idx][obj_id].x;
	obj_info[cam_idx][obj_id].prev_y = obj_info[cam_idx][obj_id].y;
}

void set_prev_rect_size(int cam_idx, int obj_id)
{
	obj_info[cam_idx][obj_id].prev_width = obj_info[cam_idx][obj_id].width;
	obj_info[cam_idx][obj_id].prev_height = obj_info[cam_idx][obj_id].height;
}

int get_flip_color_over_threshold(int cam_idx, int obj_id)
{
	if (g_setting.opt_flow_apply)
	{
		if (obj_info[cam_idx][obj_id].opt_flow_detected_count > 0)
		{
			return RED_COLOR;
		}
		return YELLO_COLOR;
	}

	return RED_COLOR;
}

int get_heat_color_over_threshold(int cam_idx, int obj_id)
{
	if (g_setting.resnet50_apply)
	{
		if (obj_info[cam_idx][obj_id].heat_count > 0)
		{
			return RED_COLOR;
		}
		return YELLO_COLOR;
	}

	return RED_COLOR;
}

void process_opt_flow(NvDsFrameMeta *frame_meta, int cam_idx, int obj_id, int cam_sec_interval)
{
    if (obj_id < 0)
        return;

    int count = 0;
    double move_size = 0.0, move_size_total = 0.0, move_size_avg = 0.0;
    double diagonal = 0;
    int row_start = 0, col_start = 0, row_num = 0, col_num = 0;
    int rows = 0, cols = 0;  // rows 추가
    int corr_value = 0;
    int bbox_move = 0, rect_size_change = 0;

    // glog_trace("[process_opt_flow] START - cam_idx=%d, obj_id=%d\n", cam_idx, obj_id);

    for (NvDsMetaList *l_user = frame_meta->frame_user_meta_list; l_user != NULL; l_user = l_user->next)
    {
        NvDsUserMeta *user_meta = (NvDsUserMeta *)(l_user->data);
        
        if (user_meta->base_meta.meta_type == NVDS_OPTICAL_FLOW_META)
        {
            // glog_trace("[process_opt_flow] Found optical flow metadata\n");
            
            NvDsOpticalFlowMeta *opt_flow_meta = (NvDsOpticalFlowMeta *)(user_meta->user_meta_data);
            
            if (!opt_flow_meta || !opt_flow_meta->data) {
                glog_error("[process_opt_flow] ERROR: NULL metadata!\n");
                continue;
            }
            
            rows = opt_flow_meta->rows;  // ✅ rows 값 저장
            cols = opt_flow_meta->cols;
            NvOFFlowVector *flow_vectors = (NvOFFlowVector *)(opt_flow_meta->data);
            
            // glog_trace("[process_opt_flow] Flow grid: rows=%d, cols=%d, total_size=%d\n", 
            //            rows, cols, rows * cols);
            
            // ✅ 좌표 변환 수정: x는 column, y는 row
            col_start = obj_info[cam_idx][obj_id].x / 4;      // x → col
            row_start = obj_info[cam_idx][obj_id].y / 4;      // y → row
            col_num = obj_info[cam_idx][obj_id].width / 4;    // width → col 개수
            row_num = obj_info[cam_idx][obj_id].height / 4;   // height → row 개수
            
            // ✅ 경계 체크 및 조정
            if (col_start < 0) col_start = 0;
            if (row_start < 0) row_start = 0;
            if (col_start >= cols) col_start = cols - 1;
            if (row_start >= rows) row_start = rows - 1;
            
            if (col_start + col_num > cols) col_num = cols - col_start;
            if (row_start + row_num > rows) row_num = rows - row_start;
            
            // glog_trace("[process_opt_flow] Adjusted bounds: row[%d-%d), col[%d-%d)\n",
            //            row_start, row_start + row_num, col_start, col_start + col_num);
            
            diagonal = obj_info[cam_idx][obj_id].diagonal;
            move_size_total = 0.0;
            count = 0;
            
            // Process the motion vectors
            for (int row = row_start; row < (row_start + row_num) && row < rows; ++row)
            {
                for (int col = col_start; col < (col_start + col_num) && col < cols; ++col)
                {
                    int index = row * cols + col;
                    int max_index = rows * cols;
                    
                    if (index < 0 || index >= max_index) {
                        glog_error("[process_opt_flow] Index still out of bounds! index=%d, max=%d\n",
                                   index, max_index);
                        continue;
                    }
                    
                    NvOFFlowVector flow_vector = flow_vectors[index];
                    move_size = calculate_sqrt(flow_vector.flowx, flow_vector.flowy);
                    move_size_total += move_size;
                    count++;
                }
            }
            
            // glog_trace("[process_opt_flow] Processed %d flow vectors\n", count);
            
            if (count > 0)
            {
                move_size_avg = move_size_total / (double)count;
                obj_info[cam_idx][obj_id].opt_flow_check_count++;
                obj_info[cam_idx][obj_id].move_size_avg = update_average(
                    obj_info[cam_idx][obj_id].move_size_avg,
                    obj_info[cam_idx][obj_id].opt_flow_check_count, 
                    move_size_avg);
            }
            
            if (cam_sec_interval)
            {
                bbox_move = get_move_distance(cam_idx, obj_id);
                rect_size_change = get_rect_size_change(cam_idx, obj_id);
                set_prev_xy(cam_idx, obj_id);
                set_prev_rect_size(cam_idx, obj_id);

                if (obj_info[cam_idx][obj_id].move_size_avg > 0)
                {
                    glog_trace("[SEC] [%d][%d].move_size_avg=%.1f,confi=%.2f,diag=%.1f\n", 
                               cam_idx, obj_id,
                               obj_info[cam_idx][obj_id].move_size_avg, 
                               obj_info[cam_idx][obj_id].confidence, 
                               diagonal);
                }

                if (bbox_move < THRESHOLD_BBOX_MOVE && 
                    rect_size_change < THRESHOLD_RECT_SIZE_CHANGE && 
                    g_move_speed == 0)
                {
                    corr_value = get_correction_value(diagonal);
                    if (cam_idx == RGB_CAM)
                    {
                        corr_value += 9;
                    }
                    
                    if (obj_info[cam_idx][obj_id].move_size_avg > 
                        (g_setting.opt_flow_threshold + corr_value))
                    {
                        obj_info[cam_idx][obj_id].opt_flow_detected_count++;
                        glog_trace("[%d][%d].opt_flow_detected_count ==> %d\n", 
                                   cam_idx, obj_id, 
                                   obj_info[cam_idx][obj_id].opt_flow_detected_count);
                    }
                }
                else
                {
                    glog_trace("[SEC] bbox_move=%d,rect_size_change=%d,g_move_speed=%d\n", 
                               bbox_move, rect_size_change, g_move_speed);
                }
                
                init_opt_flow(cam_idx, obj_id, 0);
            }
        }
    }
    
    // glog_trace("[process_opt_flow] END\n");
}

#endif

void set_obj_rect_id(int cam_idx, NvDsObjectMeta *obj_meta, int cam_sec_interval)
{
    if (cam_idx < 0 || cam_idx >= NUM_CAMS) {
        glog_error("[set_obj_rect_id] Invalid cam_idx: %d (MAX: %d)\n", cam_idx, NUM_CAMS);
        return;
    }

    if (obj_meta->object_id < 0) {
        glog_error("[set_obj_rect_id] Negative object_id: %d\n", obj_meta->object_id);
        return;
    }

    int obj_idx = obj_meta->object_id % NUM_OBJS;
    
    // 현재 프레임의 bounding box 정보 직접 사용
    int x = (int)obj_meta->rect_params.left;
    int y = (int)obj_meta->rect_params.top;
    int width = (int)obj_meta->rect_params.width;
    int height = (int)obj_meta->rect_params.height;

    // 객체 정보 저장
    obj_info[cam_idx][obj_idx].x = x;
    obj_info[cam_idx][obj_idx].y = y;
    obj_info[cam_idx][obj_idx].width = width;
    obj_info[cam_idx][obj_idx].height = height;

    // center_x, center_y 계산 (버그 수정)
    obj_info[cam_idx][obj_idx].center_x = x + (width / 2);
    obj_info[cam_idx][obj_idx].center_y = y + (height / 2);  // ← 수정됨!

    // 대각선 길이 계산 (피타고라스 정리)
    obj_info[cam_idx][obj_idx].diagonal = calculate_sqrt((double)width, (double)height);

    // 클래스 정보 저장
    obj_info[cam_idx][obj_idx].class_id = (int)obj_meta->class_id;
    obj_info[cam_idx][obj_idx].confidence = (float)obj_meta->confidence;

    // 디버깅을 위한 로그 (필요시 활성화)
    // #ifdef DEBUG_OBJ_RECT
    // if (cam_sec_interval) {
    //     glog_trace("[set_obj_rect_id] cam=%d, obj=%d, bbox=(%d,%d,%d,%d), "
    //                "center=(%d,%d), diag=%.2f, class=%d, conf=%.2f\n",
    //                cam_idx, obj_idx, x, y, width, height,
    //                obj_info[cam_idx][obj_idx].center_x,
    //                obj_info[cam_idx][obj_idx].center_y,
    //                obj_info[cam_idx][obj_idx].diagonal,
    //                obj_info[cam_idx][obj_idx].class_id,
    //                obj_info[cam_idx][obj_idx].confidence);
    // }
    // #endif
}

#if THERMAL_TEMP_INCLUDE
void get_pixel_color(NvBufSurface *surface, guint batch_idx, guint x, guint y, unsigned char *r, unsigned char *g, unsigned char *b, unsigned char *a)
{
	if (!surface) {
        glog_error("[get_pixel_color] Surface is NULL\n");
        return;
    }

    if (batch_idx >= surface->numFilled) {
        glog_error("[get_pixel_color] batch_idx %u exceeds numFilled %u\n", 
                   batch_idx, surface->numFilled);
        return;
    }

    if (!surface->surfaceList) {
        glog_error("[get_pixel_color] surfaceList is NULL\n");
        return;
    }

    NvBufSurfaceParams *params = &surface->surfaceList[batch_idx];
    
    if (!params->dataPtr) {
        glog_error("[get_pixel_color] dataPtr is NULL for batch_idx %u\n", batch_idx);
        return;
    }

	// Get the width, height, and color format of the surface
	int width = params->width;
	int height = params->height;
	NvBufSurfaceColorFormat color_format = params->colorFormat;

	// Check if the pixel coordinates are within the bounds
	if (x >= width || y >= height)
	{
		printf("Pixel coordinates are out of bounds.\n");
		return;
	}

	// Access the pixel data directly
	unsigned char *pixel_data = (unsigned char *)params->dataPtr;

	// Define the pixel size based on the color format
	int pixel_size = 0;

	switch (color_format)
	{
	case NVBUF_COLOR_FORMAT_RGBA:
		pixel_size = 4; // RGBA format (4 bytes per pixel)
		break;
	case NVBUF_COLOR_FORMAT_BGR:
		pixel_size = 3; // BGR format (3 bytes per pixel)
		break;
	case NVBUF_COLOR_FORMAT_NV12:
		// For NV12, you'll need to handle both Y and UV planes separately.
		printf("Pixel color extraction for NV12 is not implemented in this example.\n");
		return;
	default:
		printf("Unsupported color format.\n");
		return;
	}

	// Compute the offset for the pixel at (x, y)
	int pixel_offset = (y * width + x) * pixel_size;

	*r = pixel_data[pixel_offset];								 // Red value
	*g = pixel_data[pixel_offset + 1];							 // Green value
	*b = pixel_data[pixel_offset + 2];							 // Blue value
	*a = (pixel_size == 4) ? pixel_data[pixel_offset + 3] : 255; // Alpha value (if RGBA)
}

// Define a function to map RGBA color to temp
float map_rgba_to_temp(unsigned char r, unsigned char g, unsigned char b)
{
	// Define temp range (e.g., 0°C to 100°C)
	float min_temp = 0.0f;	 // Minimum temp (for Blue)
	float max_temp = 100.0f; // Maximum temp (for Red)

	// Map the 'Red' channel to temp (simple approach)
	// Assuming the color range is from blue (low temp) to red (high temp)
	float temp = (r / 255.0f) * (max_temp - min_temp) + min_temp;

	return temp;
}

// Function to get RGBA color and map it to temp
float get_pixel_temp(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	// Calculate the temp based on RGBA
	float temp = map_rgba_to_temp(r, g, b);

	// Print the temp value
	// glog_trace("Pixel Temp.:%.2f°C\n", temp);

	return temp;
}

void get_bbox_temp(GstBuffer *buf, int obj_id)
{
	if (obj_id < 0)
		return;

	int count = 0;
	float temp_total = 0.0, temp_avg = 0.0;
	int x_start = 0, y_start = 0, width = 0, height = 0;
	float pixel_temp = 0;
	unsigned char r = 0, g = 0, b = 0, a = 0;

	if (obj_id < 0) {
        glog_error("[get_bbox_temp] Invalid obj_id: %d\n", obj_id);
        return;
    }

    NvBufSurface *surface = NULL;
    GstMapInfo map_info;

    if (!gst_buffer_map(buf, &map_info, GST_MAP_READ))
    {
        glog_error("[get_bbox_temp] Failed to map buffer for obj_id: %d\n", obj_id);
        // ❌ unmap 제거
        return;
    }

    surface = (NvBufSurface *)map_info.data;
    if (surface == NULL)
    {
        glog_error("[get_bbox_temp] Surface is NULL for obj_id: %d\n", obj_id);
        gst_buffer_unmap(buf, &map_info);
        return;
    }

    //glog_trace("[get_bbox_temp] Successfully mapped buffer for obj_id: %d\n", obj_id);

	x_start = (obj_info[THERMAL_CAM][obj_id].x);
	y_start = (obj_info[THERMAL_CAM][obj_id].y);
	width = obj_info[THERMAL_CAM][obj_id].width;
	height = obj_info[THERMAL_CAM][obj_id].height;

	temp_total = 0.0;
	count = 0;

	// Process the motion vectors as needed
	for (int x = x_start; x < (x_start + width); ++x)
	{
		if (x % XY_DIVISOR != 0)
			continue;
		for (int y = y_start; y < (y_start + height); ++y)
		{
			if (y % XY_DIVISOR != 0)
				continue;
			get_pixel_color(surface, 0, x, y, &r, &g, &b, &a);
			pixel_temp = get_pixel_temp(r, g, b, a);
			if (pixel_temp < g_setting.threshold_under_temp || pixel_temp > g_setting.threshold_upper_temp)
				continue;
			temp_total += pixel_temp;
			count++;
		}
	}

	if (count > 0)
	{
		temp_avg = temp_total / (float)count;
		add_value_and_calculate_avg(&obj_info[THERMAL_CAM][obj_id], (int)temp_avg);
	}

	// ✅ 반드시 unmap 호출 (메모리 누수 방지)
	gst_buffer_unmap(buf, &map_info);
}

#if 1
// Function to update the display text for an object
void update_display_text(NvDsObjectMeta *obj_meta, const char *text)
{
	// Check if the object meta is valid
	if (!obj_meta)
	{
		return;
	}
	// Set the display text
	strncpy(obj_meta->text_params.display_text, text, sizeof(obj_meta->text_params.display_text) - 1);
}
#endif

void temp_display_text(NvDsObjectMeta *obj_meta)
{
	char display_text[100] = "", append_text[100] = "";
	if (obj_meta->object_id < 0)
		return;
	if (obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp < (g_setting.threshold_under_temp + g_setting.temp_diff_threshold)) // LJH, 20250410
		return;

	strcpy(display_text, obj_meta->text_params.display_text);
	sprintf(append_text, "[%d°C]", obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp + 4);
	strcat(display_text, append_text);
	remove_newlines(display_text);
	if (obj_meta->text_params.display_text)
	{
		g_free(obj_meta->text_params.display_text);
		obj_meta->text_params.display_text = g_strdup(display_text);
	}
}

int objs_temp_avg = 0;
int objs_temp_total = 0;
int objs_count = 0;

void init_temp_avg()
{
	objs_temp_avg = 0;
	objs_count = 0;
	objs_temp_total = 0;
}

void get_temp_total(NvDsObjectMeta *obj_meta)
{
	if (obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp < g_setting.threshold_under_temp)
		return;

	objs_temp_total += obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp;
	objs_count++;
}

void get_temp_avg()
{
	if (objs_count == 0 || objs_temp_total == 0)
	{
		objs_temp_avg = 0;
		return;
	}

	objs_temp_avg = objs_temp_total / objs_count;
	// glog_trace("objs_temp_total=%d objs_count=%d objs_temp_avg=%d\n", objs_temp_total, objs_count, objs_temp_avg);
}

#endif

#if RESNET_50
static int pgie_probe_callback(NvDsObjectMeta *obj_meta)
{
	// Bounding Box Coordinates
	float left = obj_meta->rect_params.left;
	float top = obj_meta->rect_params.top;
	float width = obj_meta->rect_params.width;
	float height = obj_meta->rect_params.height;
	glog_debug("Bounding Box: Left: %.2f, Top: %.2f, Width: %.2f, Height: %.2f\n", left, top, width, height);

	// Extract classification results
	for (NvDsMetaList *l_class = obj_meta->classifier_meta_list; l_class != NULL; l_class = l_class->next)
	{
		NvDsClassifierMeta *class_meta = (NvDsClassifierMeta *)(l_class->data);
		for (NvDsMetaList *l_label = class_meta->label_info_list; l_label != NULL; l_label = l_label->next)
		{
			NvDsLabelInfo *label_info = (NvDsLabelInfo *)(l_label->data);
			glog_debug("ResNet-50 Classification - Class ID: %d, Label: %s, Confidence: %.2f\n",
					   label_info->result_class_id, label_info->result_label, label_info->result_prob);
			if (label_info->result_class_id == 1 && label_info->result_prob >= g_setting.resnet50_threshold)
			{
				glog_debug("return CLASS_HEAT_COW\n");
				return CLASS_HEAT_COW;
			}
		}
	}
	// return GST_PAD_PROBE_OK;
	return CLASS_NORMAL_COW;
}
#endif

void remove_newline_text(NvDsObjectMeta *obj_meta)
{
	char display_text[100] = "";

	if (obj_meta->object_id < 0)
		return;
	if (obj_meta->text_params.display_text[0] == 0)
		return;
	strcpy(display_text, obj_meta->text_params.display_text);
	remove_newlines(display_text);
	if (obj_meta->text_params.display_text)
	{
		g_free(obj_meta->text_params.display_text);
		obj_meta->text_params.display_text = g_strdup(display_text);
	}
}

#if TEMP_NOTI
int is_temp_duration()
{
	for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++)
	{
		if (obj_info[THERMAL_CAM][obj_id].class_id == CLASS_OVER_TEMP && obj_info[THERMAL_CAM][obj_id].temp_duration > 0)
			return 1;
	}

	return 0;
}

void check_for_temp_notification()
{
	if (objs_temp_avg < g_setting.threshold_under_temp || objs_count == 0)
	{
		init_temp_avg();
		return;
	}

	for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++)
	{
		if (obj_info[THERMAL_CAM][obj_id].bbox_temp < g_setting.threshold_under_temp)
		{
			obj_info[THERMAL_CAM][obj_id].temp_duration = 0;
			obj_info[THERMAL_CAM][obj_id].class_id = CLASS_NORMAL_COW;
			continue;
		}

		if (obj_info[THERMAL_CAM][obj_id].bbox_temp > (objs_temp_avg + g_setting.temp_diff_threshold))
		{
			obj_info[THERMAL_CAM][obj_id].temp_duration++;
			glog_debug("objs_temp_avg=%d obj_id=%d bbox_temp=%d temp_duration=%d\n", objs_temp_avg, obj_id, obj_info[THERMAL_CAM][obj_id].bbox_temp, obj_info[THERMAL_CAM][obj_id].temp_duration);
			if (obj_info[THERMAL_CAM][obj_id].temp_duration >= g_setting.over_temp_time)
			{ // if duration lasted more than designated time
				obj_info[THERMAL_CAM][obj_id].temp_duration = 0;
				if (obj_info[THERMAL_CAM][obj_id].temp_event_time_gap == 0)
				{
					obj_info[THERMAL_CAM][obj_id].class_id = CLASS_OVER_TEMP;
					obj_info[THERMAL_CAM][obj_id].notification_flag = 1; // send notification later
					glog_debug("objs_temp_avg=%d obj_id=%d notification_flag=1\n", objs_temp_avg, obj_id);
				}
				else
				{
					glog_debug("obj_info[THERMAL_CAM][%d].temp_event_time_gap=%d is less than TEMP_EVENT_TIME_GAP=%d\n", obj_id, obj_info[THERMAL_CAM][obj_id].temp_event_time_gap, TEMP_EVENT_TIME_GAP);
				}
			}
		}
		else
		{
			obj_info[THERMAL_CAM][obj_id].temp_duration = 0;
			obj_info[THERMAL_CAM][obj_id].class_id = CLASS_NORMAL_COW;
		}
	}

	init_temp_avg();
}

#endif

void simulate_get_temp_avg()
{
	for (int i = 0; i < NUM_OBJS; i++)
		obj_info[THERMAL_CAM][i].bbox_temp = 0;

	obj_info[THERMAL_CAM][1].bbox_temp = 20;
	obj_info[THERMAL_CAM][2].bbox_temp = 21;
	obj_info[THERMAL_CAM][3].bbox_temp = 38;

	objs_temp_avg = 0;
	objs_temp_avg += obj_info[THERMAL_CAM][1].bbox_temp;
	objs_temp_avg += obj_info[THERMAL_CAM][2].bbox_temp;
	objs_temp_avg += obj_info[THERMAL_CAM][3].bbox_temp;

	objs_temp_avg /= 3;
	objs_count = 3;
	// glog_trace("simulate objs_temp_avg=%d\n", objs_temp_avg);
}

void add_correction()
{
	if (g_setting.temp_correction == 0)
		return;

	for (int i = 0; i < NUM_OBJS; i++)
	{
		if (obj_info[THERMAL_CAM][i].corrected == 1)
		{
			continue;
		}

		//    if ((obj_info[THERMAL_CAM][i].center_x < 320 || obj_info[THERMAL_CAM][i].center_x > 960) || obj_info[THERMAL_CAM][i].center_y < 180)
		{
			obj_info[THERMAL_CAM][i].bbox_temp += g_setting.temp_correction;
			obj_info[THERMAL_CAM][i].corrected = 1;
		}
	}
}

void set_color(NvDsObjectMeta *obj_meta, int color, int set_text_blank)
{
	//glog_trace("set_color: obj_id=%ld, color=%d, set_text_blank=%d\n", obj_meta->object_id, color, set_text_blank);
	switch (color)
	{
	case GREEN_COLOR:
		obj_meta->rect_params.border_color.red = 0.0;
		obj_meta->rect_params.border_color.green = 1.0;
		obj_meta->rect_params.border_color.blue = 0.0;
		obj_meta->rect_params.border_color.alpha = 1;
		break;
	case RED_COLOR:
		obj_meta->rect_params.border_color.red = 1.0;
		obj_meta->rect_params.border_color.green = 0.0;
		obj_meta->rect_params.border_color.blue = 0.0;
		obj_meta->rect_params.border_color.alpha = 1;
		break;
	case YELLO_COLOR:
		obj_meta->rect_params.border_color.red = 1.0;
		obj_meta->rect_params.border_color.green = 1.0;
		obj_meta->rect_params.border_color.blue = 0.0;
		obj_meta->rect_params.border_color.alpha = 1;
		break;
	case BLUE_COLOR:
		obj_meta->rect_params.border_color.red = 0.0;
		obj_meta->rect_params.border_color.green = 0.0;
		obj_meta->rect_params.border_color.blue = 1.0;
		obj_meta->rect_params.border_color.alpha = 1;
		break;
	case NO_BBOX:
		obj_meta->rect_params.border_color.red = 0.0;
		obj_meta->rect_params.border_color.green = 0.0;
		obj_meta->rect_params.border_color.blue = 0.0;
		obj_meta->rect_params.border_color.alpha = 0;
		obj_meta->text_params.display_text[0] = 0;
		break;
	}

	if (set_text_blank)
	{
		obj_meta->text_params.display_text[0] = 0;
	}
}

void set_temp_bbox_color(NvDsObjectMeta *obj_meta)
{
	if (obj_info[THERMAL_CAM][obj_meta->object_id].temp_duration > 0)
	{
		set_color(obj_meta, BLUE_COLOR, 0);
		// glog_trace("blue bbox obj_id=%d\n", obj_meta->object_id);
	}
}

// 특정 시간 범위의 검출 데이터 조회
gint get_detections_for_timerange(guint camera_id, guint64 start_time,
								  guint64 end_time, DetectionData *results,
								  gint max_results)
{
	if (camera_id >= NUM_CAMS)
	{
		glog_error("Invalid camera ID: %d\n", camera_id);
		return 0;
	}

	CameraBuffer *cam_buf = &camera_buffers[camera_id];
	gint result_count = 0;

	g_mutex_lock(&cam_buf->mutex);

	// 버퍼 순회
	for (gint i = 0; i < cam_buf->count && result_count < max_results; i++)
	{
		gint idx = (cam_buf->head + i) % MAX_DETECTION_BUFFER_SIZE;
		DetectionData *det = &cam_buf->buffer[idx];

		if (det->timestamp >= start_time && det->timestamp <= end_time)
		{
			// 데이터 복사
			memcpy(&results[result_count], det, sizeof(DetectionData));
			result_count++;
		}
	}

	g_mutex_unlock(&cam_buf->mutex);

	return result_count;
}

// 최신 검출 데이터 조회
gboolean get_latest_detection(guint camera_id, DetectionData *result)
{
	if (camera_id >= NUM_CAMS || result == NULL)
		return FALSE;

	CameraBuffer *cam_buf = &camera_buffers[camera_id];

	g_mutex_lock(&cam_buf->mutex);

	if (cam_buf->count > 0)
	{
		// tail 이전 위치가 최신 데이터
		gint latest_idx = (cam_buf->tail - 1 + MAX_DETECTION_BUFFER_SIZE) %
						  MAX_DETECTION_BUFFER_SIZE;
		memcpy(result, &cam_buf->buffer[latest_idx], sizeof(DetectionData));
		g_mutex_unlock(&cam_buf->mutex);
		return TRUE;
	}

	g_mutex_unlock(&cam_buf->mutex);
	return FALSE;
}

// 버퍼 통계 조회
void get_buffer_stats(guint camera_id, gint *buffer_size,
					  guint64 *oldest_timestamp, guint64 *latest_timestamp)
{
	if (camera_id >= NUM_CAMS)
		return;

	CameraBuffer *cam_buf = &camera_buffers[camera_id];

	g_mutex_lock(&cam_buf->mutex);

	*buffer_size = cam_buf->count;

	if (cam_buf->count > 0)
	{
		*oldest_timestamp = cam_buf->buffer[cam_buf->head].timestamp;
		gint latest_idx = (cam_buf->tail - 1 + MAX_DETECTION_BUFFER_SIZE) %
						  MAX_DETECTION_BUFFER_SIZE;
		*latest_timestamp = cam_buf->buffer[latest_idx].timestamp;
	}
	else
	{
		*oldest_timestamp = 0;
		*latest_timestamp = 0;
	}

	g_mutex_unlock(&cam_buf->mutex);
}

void cleanup_camera_buffers()
{
	if (!buffers_initialized)
		return;

	for (int i = 0; i < NUM_CAMS; i++)
	{
		g_mutex_clear(&camera_buffers[i].mutex);
	}

	buffers_initialized = FALSE;
}

/* osd_sink_pad_buffer_probe  will extract metadata received on OSD sink pad
 * and update params for drawing rectangle, object information etc. */
static GstPadProbeReturn osd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u_data) // LJH, this function is called per frame
{
	if (!g_setting.analysis_status) {
        return GST_PAD_PROBE_OK;
    }

	GstBuffer *buf = (GstBuffer *)info->data;
	NvDsObjectMeta *obj_meta = NULL;
	NvDsMetaList *l_frame = NULL;
	NvDsMetaList *l_obj = NULL;
	NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

	static float small_obj_diag[2] = {40.0, 40.0};
	static float big_obj_diag[2] = {1000.0, 1000.0};
	int cam_idx = *(int *)u_data;
	int sec_interval[NUM_CAMS] = {0}; // common one second interval for RGB and Thermal
#if TRACK_PERSON_INCLUDE
	static PersonObj object[NUM_OBJS];
	init_objects(object);
#else
	int event_class_id = CLASS_NORMAL_COW;
#endif
#if OPTICAL_FLOW_INCLUDE
	int start_obj_id = 0, obj_id = -1;
#endif
#if TEMP_NOTI_TEST
	cam_idx = THERMAL_CAM;
	g_source_cam_idx = cam_idx;
#endif
	static int do_temp_display = 0;

	g_cam_index = cam_idx;
	g_frame_count[cam_idx]++;
	// glog_trace("cam index = %d\n", cam_idx);
	if (g_frame_count[cam_idx] >= PER_CAM_SEC_FRAME)
	{
		g_frame_count[cam_idx] = 0;
		sec_interval[cam_idx] = 1;
	}

#if TEMP_NOTI
	if (sec_interval[THERMAL_CAM])
	{
		init_temp_avg();
	}
#endif
	if (!buffers_initialized)
	{
		init_camera_buffers();
	}

	static guint64 frame_counters[NUM_CAMS] = {0};

	for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
	{
		NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);
		// 카메라 ID와 프레임 번호 추출
		guint camera_id = cam_idx;
		guint frame_number = frame_counters[camera_id]++;

		if (frame_meta->obj_meta_list != NULL)
		{
			add_detection_to_buffer(camera_id, frame_number, frame_meta->obj_meta_list, frame_meta);
		}

		for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next)
		{
			obj_meta = (NvDsObjectMeta *)(l_obj->data);
			obj_meta->rect_params.border_width = 1;
			obj_meta->text_params.set_bg_clr = 0;

			if (cam_idx == THERMAL_CAM)
			{
				obj_meta->text_params.font_params.font_size = 9;
			}
			set_obj_rect_id(cam_idx, obj_meta, sec_interval[cam_idx]);

			event_class_id = CLASS_NORMAL_COW;
#if TRACK_PERSON_INCLUDE
			set_person_obj_state(object, obj_meta);
#else
			// printf("cam_idx=%d, obj_id=%d, class_id=%d, confidence=%.2f\n", cam_idx, obj_meta->object_id, obj_meta->class_id, obj_meta->confidence);
			if (obj_meta->class_id == CLASS_NORMAL_COW || obj_meta->class_id == CLASS_NORMAL_COW_SITTING)
			{
				if (obj_meta->confidence >= threshold_confidence[obj_meta->class_id])
				{
					set_color(obj_meta, GREEN_COLOR, 0);
					// print_debug(obj_meta);
				}
				else
				{
					set_color(obj_meta, NO_BBOX, 0);
				}
				if (g_setting.show_normal_text == 0)
					obj_meta->text_params.display_text[0] = 0;
			}
			else if (obj_meta->class_id == CLASS_HEAT_COW || obj_meta->class_id == CLASS_FLIP_COW || obj_meta->class_id == CLASS_LABOR_SIGN_COW)
			{
				set_color(obj_meta, RED_COLOR, 0);
				if (obj_meta->confidence >= threshold_confidence[obj_meta->class_id])
				{
					event_class_id = obj_meta->class_id;
#if RESNET_50
					if (g_setting.resnet50_apply)
					{
						if (event_class_id == CLASS_HEAT_COW && obj_meta->object_id >= 0)
						{
							if (pgie_probe_callback(obj_meta) == CLASS_HEAT_COW)
							{
								obj_info[cam_idx][obj_meta->object_id].heat_count++;
							}
						}
					}
#endif
					if (event_class_id == CLASS_HEAT_COW)
					{
						if (get_heat_color_over_threshold(cam_idx, obj_meta->object_id) == YELLO_COLOR)
						{
							set_color(obj_meta, YELLO_COLOR, 0);
						}
					}
					else if (event_class_id == CLASS_FLIP_COW)
					{
						if (get_flip_color_over_threshold(cam_idx, obj_meta->object_id) == YELLO_COLOR)
						{
							set_color(obj_meta, YELLO_COLOR, 0);
						}
					}
				}
				else
				{ // LJH, if lower than abnormalty threshold then green
					if (g_setting.show_normal_text == 0)
						set_color(obj_meta, GREEN_COLOR, 1);
					else
						set_color(obj_meta, GREEN_COLOR, 0);
					// glog_trace("id=%d yellow confidence=%f\n", obj_meta->object_id, obj_meta->confidence);     //LJH, for test
				}
				// glog_trace("abnormal id=%d class=%d text=%s confidence=%f\n",
				// obj_meta->object_id, obj_meta->class_id, obj_meta->text_params.display_text, obj_meta->confidence);     //LJH, for test
				// print_debug(obj_meta);
			}
#if THERMAL_TEMP_INCLUDE
			if (g_setting.temp_apply)
			{
				if (cam_idx == THERMAL_CAM && obj_meta->object_id >= 0)
				{
					if (sec_interval[THERMAL_CAM])
					{
						get_bbox_temp(buf, obj_meta->object_id);
						if (obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp > g_setting.threshold_under_temp)
						{
							// glog_trace("id=%d bbox_temp=%d\n", obj_meta->object_id, obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp);
							add_correction();
#if TEMP_NOTI
							get_temp_total(obj_meta); // get temperature total before getting average
#endif
						}
					}
					if (g_setting.display_temp || do_temp_display)
					{
						temp_display_text(obj_meta);
					}
					set_temp_bbox_color(obj_meta); // if temperature is too high then set color
				}
			}
#endif
			// glog_trace("obj_id=%d,class_id=%d\n", obj_meta->object_id, obj_meta->class_id);
			if (g_move_speed > 0)
			{ // if ptz is moving don't display bounding box
				set_color(obj_meta, NO_BBOX, 0);
			}
			else if (obj_meta->object_id >= 0 && (obj_info[cam_idx][obj_meta->object_id].diagonal < small_obj_diag[cam_idx] ||
												  obj_info[cam_idx][obj_meta->object_id].diagonal > big_obj_diag[cam_idx]))
			{ // if bounding box is too small or too big don't display bounding box
				set_color(obj_meta, NO_BBOX, 0);
				//glog_trace("small||big [%d][%d].diagonal=%f\n", cam_idx, obj_meta->object_id, obj_info[cam_idx][obj_meta->object_id].diagonal);
			}
			remove_newline_text(obj_meta);
			// glog_trace("g_move_speed=%d id=%d text=%s\n", g_move_speed, obj_meta->object_id, obj_meta->text_params.display_text);     //LJH, for test
			if (cam_idx == g_source_cam_idx)
			{ // if cam index is identifical to the set source cam
				gather_event(event_class_id, obj_meta->object_id, cam_idx);
			}
#endif
		}

#if TEMP_NOTI_TEST
		simulate_get_temp_avg(); // LJH, for simulation
#endif

		if (cam_idx == g_source_cam_idx)
		{ // if cam index is identifical to the set source cam
			if (sec_interval[cam_idx])
			{
				check_events_for_notification(cam_idx, 0);
#if TEMP_NOTI
				if (g_setting.temp_apply)
				{
					if (cam_idx == THERMAL_CAM)
					{
						get_temp_avg(); // get average temperature for objects in the screen
						check_for_temp_notification();
						do_temp_display = is_temp_duration(); // if over temp state is being counted for notification
					}
				}
#endif
			}
#if OPTICAL_FLOW_INCLUDE
			start_obj_id = 0;
			int iteration_count = 0;
			
			while ((obj_id = get_opt_flow_object(cam_idx, start_obj_id)) != -1)
			{
				if (iteration_count++ > MAX_OPT_FLOW_ITERATIONS) {
					glog_error("[osd_sink_pad_buffer_probe] Possible infinite loop detected! "
							"cam_idx=%d, start_obj_id=%d, obj_id=%d\n", 
							cam_idx, start_obj_id, obj_id);
					break;
				}
				
				glog_trace("[osd_sink_pad_buffer_probe] Processing opt flow: "
						"iteration=%d, cam_idx=%d, obj_id=%d\n", 
						iteration_count, cam_idx, obj_id);
				
				process_opt_flow(frame_meta, cam_idx, obj_id, sec_interval[cam_idx]);
				start_obj_id = (obj_id + 1) % NUM_OBJS;
			}
			
			if (iteration_count > 100) {
				glog_trace("[osd_sink_pad_buffer_probe] High iteration count: %d\n", iteration_count);
			}
#endif
			if (sec_interval[cam_idx])
			{
				trigger_notification(cam_idx);
			}
		}
	}
#if TRACK_PERSON_INCLUDE
	object_state = track_object(object_state, object);
#endif

	return GST_PAD_PROBE_OK;
}

void setup_nv_analysis()
{
	glog_trace("g_config.device_cnt=%d\n", g_config.device_cnt);

	// camera_buffers 초기화
	init_camera_buffers();

	// 각 카메라별로 동적 할당
	g_cam_indices = g_malloc(sizeof(int) * g_config.device_cnt);

	for (int cam_idx = 0; cam_idx < g_config.device_cnt; cam_idx++)
	{
		GstPad *osd_sink_pad = NULL;
		char element_name[32];
		GstElement *nvosd = NULL;

		sprintf(element_name, "nvosd_%d", cam_idx + 1);
		glog_trace("element_name=%s\n", element_name);

		nvosd = gst_bin_get_by_name(GST_BIN(g_pipeline), element_name);
		if (nvosd == NULL)
		{
			glog_error("Fail get %s element\n", element_name);
			continue;
		}

		osd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
		if (!osd_sink_pad)
		{
			g_print("Unable to get sink pad for %s\n", element_name);
			gst_object_unref(nvosd);
			continue;
		}

		// 각 카메라별로 고유한 인덱스 저장
		g_cam_indices[cam_idx] = cam_idx;

		g_print("osd_sink_pad_buffer_probe cam_idx=%d\n", cam_idx);
		gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
						  osd_sink_pad_buffer_probe, &g_cam_indices[cam_idx], NULL);

		gst_object_unref(osd_sink_pad);
		gst_object_unref(nvosd);
	}

	pthread_create(&g_tid, NULL, event_sender_thread, NULL);
}

void endup_nv_analysis()
{
	if (g_tid)
	{
		g_event_class_id = EVENT_EXIT;

		pthread_join(g_tid, NULL);
	}

	cleanup_camera_buffers();

	if (g_cam_indices)
	{
		g_free(g_cam_indices);
		g_cam_indices = NULL;
	}
}

int is_event_recording()
{
	return g_event_recording;
}