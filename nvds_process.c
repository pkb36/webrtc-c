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
#include <sys/stat.h>
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
#include "circular_buffer.h"
#include "ptz_control.h"

static int *g_cam_indices = NULL;
#define MAX_OPT_FLOW_ITERATIONS 1000
#define MAX_DETECTION_BUFFER_SIZE 10000 // 최대 10000 프레임
#define BUFFER_DURATION_SEC 120			// 120초 버퍼

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

typedef struct {
    double last_event_time[NUM_CLASSES][NUM_CAMS];
    double throttle_interval;  // 필터링 간격 (초)
} EventThrottle;

static EventThrottle g_event_throttle = {
    .throttle_interval = 60.0  // 10초 간격
};

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
	printf("set_process_analysis OnOff=%d\n", OnOff);
    if (OnOff == 0)
        check_events_for_notification(0, 1);

    gboolean all_success = TRUE;

    for (int cam_idx = 0; cam_idx < g_config.device_cnt; cam_idx++)
    {
        char element_name[32];
		GstElement *nvinfer;
		sprintf(element_name, "nvinfer_%d", cam_idx+1);
		nvinfer = gst_bin_get_by_name (GST_BIN (g_pipeline), element_name);
		if (nvinfer == NULL){
			glog_trace ("Fail get %s element\n", element_name);
			continue;
		}

		gint interval = OnOff ? g_setting.nv_interval : G_MAXINT;
		g_object_set(G_OBJECT(nvinfer), "interval", interval, NULL);
		g_clear_object (&nvinfer);

        GstElement *dspostproc = NULL;
        
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

	printf("set_process_analysis: OnOff=%d, all_success=%d\n", OnOff, all_success);
    
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
	const AutoPTZState* ptz_state = get_auto_ptz_state();

	struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double event_time = ts.tv_sec + ts.tv_nsec / 1e9;

	double time_diff = event_time - g_event_throttle.last_event_time[class_id][camera_id];

	// if (ptz_state->is_running == FALSE && time_diff < g_event_throttle.throttle_interval) {
	if (time_diff < g_event_throttle.throttle_interval) {
        // 너무 빈번한 이벤트는 필터링
        glog_debug("Event filtered: class=%d, camera=%d, time_diff=%.1f\n", 
                   class_id, camera_id, time_diff);
        return FALSE;
    }

	on_event_detected(camera_id, class_id, event_time);

	g_event_throttle.last_event_time[class_id][camera_id] = event_time;

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

// nvds_process.c에 추가할 함수
static void add_clock_overlay(NvDsFrameMeta *frame_meta, NvDsBatchMeta *batch_meta, int cam_idx)
{
    // 시계용 DisplayMeta 생성
    NvDsDisplayMeta *clock_meta = nvds_acquire_display_meta_from_pool(batch_meta);
    if (!clock_meta) {
        return;
    }
    
    // 현재 시간 가져오기
    time_t rawtime;
    struct tm *timeinfo;
    char time_str[64];
    
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    // 프레임 해상도
    int frame_width = frame_meta->source_frame_width;
    int frame_height = frame_meta->source_frame_height;
    
    if (frame_width == 0 || frame_height == 0) {
        if (cam_idx == THERMAL_CAM) {
            frame_width = 384;
            frame_height = 288;
        } else {
            frame_width = 1920;
            frame_height = 1080;
        }
    }
    
    // 시계 위치 및 크기 설정
    int clock_x, clock_y, clock_font_size;
    
    if (cam_idx == THERMAL_CAM) {
        clock_x = 5;
        clock_y = 5;
        clock_font_size = 12;
    } else {
        clock_x = 10;
        clock_y = 15;
        clock_font_size = 36;
    }
    
    // 8방향 외곽선 오프셋
    int offsets[][2] = {
        {-1, -1}, {0, -1}, {1, -1},
        {-1,  0},          {1,  0},
        {-1,  1}, {0,  1}, {1,  1}
    };
    
    // 배경 사각형 추가
    // clock_meta->num_rects = 1;
    // NvOSD_RectParams *clock_bg = &clock_meta->rect_params[0];
    // clock_bg->left = clock_x - 10;
    // clock_bg->top = clock_y - 5;
    // clock_bg->width = strlen(time_str) * (clock_font_size * 0.6) + 20;
    // clock_bg->height = clock_font_size + 10;
    // clock_bg->has_bg_color = 1;
    // clock_bg->bg_color = (NvOSD_ColorParams){0.0, 0.0, 0.0, 0.4};
    // clock_bg->border_width = 0;
    
    // 텍스트 설정 (8개 외곽선 + 1개 중앙)
    clock_meta->num_labels = 9;
    
    // 검은색 외곽선 (8방향)
    for (int i = 0; i < 8; i++) {
        NvOSD_TextParams *outline = &clock_meta->text_params[i];
        outline->display_text = g_strdup(time_str);
        outline->x_offset = clock_x + offsets[i][0];
        outline->y_offset = clock_y + offsets[i][1];
        outline->font_params.font_name = "Ubuntu";
        outline->font_params.font_size = clock_font_size;
        outline->font_params.font_color = (NvOSD_ColorParams){0.0, 0.0, 0.0, 1.0};
        outline->set_bg_clr = 0;
    }
    
    // 흰색 중앙 텍스트
    NvOSD_TextParams *center = &clock_meta->text_params[8];
    center->display_text = g_strdup(time_str);
    center->x_offset = clock_x;
    center->y_offset = clock_y;
    center->font_params.font_name = "Ubuntu";
    center->font_params.font_size = clock_font_size;
    center->font_params.font_color = (NvOSD_ColorParams){1.0, 1.0, 1.0, 1.0};
    center->set_bg_clr = 0;
    
    nvds_add_display_meta_to_frame(frame_meta, clock_meta);
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

	for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
	{
		NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);

		add_clock_overlay(frame_meta, batch_meta, cam_idx);

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
						// temp_display_text(obj_meta);
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
			set_custom_label(obj_meta, frame_meta, batch_meta, cam_idx, do_temp_display);
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

void set_custom_label(NvDsObjectMeta *obj_meta, NvDsFrameMeta *frame_meta, 
                      NvDsBatchMeta *batch_meta, int cam_idx, int temp_display)
{
    // 박스가 숨겨져 있으면 라벨도 표시 안 함
    if (obj_meta->rect_params.border_color.alpha == 0) {
        return;
    }
    
    // DisplayMeta 할당
    NvDsDisplayMeta *display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
    if (!display_meta) {
        glog_error("Failed to acquire display meta\n");
        return;
    }


    
    // 프레임 해상도 가져오기
    int frame_width = frame_meta->source_frame_width;
    int frame_height = frame_meta->source_frame_height;
    
    // 해상도가 0인 경우 기본값 사용
    if (frame_width == 0 || frame_height == 0) {
        // 카메라별 기본 해상도
        if (cam_idx == THERMAL_CAM) {
            frame_width = 384;
            frame_height = 288;
        } else {
            frame_width = 1920;
            frame_height = 1080;
        }
    }
    
    // 라벨 텍스트 생성
    char label_text[256];
    if (cam_idx == THERMAL_CAM && 
        (g_setting.display_temp || temp_display) &&
        obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp > 0) {
        sprintf(label_text, "[%d°C]", 
                obj_info[THERMAL_CAM][obj_meta->object_id].bbox_temp);
    } else {
        sprintf(label_text, "%s %.0f%%", 
                obj_meta->obj_label, 
                obj_meta->confidence * 100);
    }
    
    // 해상도 기반 폰트 크기 계산
    int base_font_size;
    float scale_factor;
    
    if (cam_idx == THERMAL_CAM) {
        // 열화상은 작은 해상도이므로 다른 비율 적용
        base_font_size = 10;
        scale_factor = frame_height / 288.0;
    } else {
        base_font_size = 14;
        scale_factor = frame_height / 1080.0;
    }
    
    int font_size = (int)(base_font_size * scale_factor);
    font_size = MAX(8, MIN(font_size, 20));  // 8~20 범위로 제한
    
    // 텍스트 크기 계산 개선
    // 폰트 크기에 기반한 문자 폭 계산
    float char_width = font_size * 0.55;  // 폰트 크기의 55%
    
    // 텍스트 길이 계산 (멀티바이트 문자 고려)
    int display_len = 0;
    for (int i = 0; label_text[i] != '\0'; i++) {
        if ((unsigned char)label_text[i] < 128) {
            display_len++;  // ASCII 문자
        } else if (label_text[i] == '[' || label_text[i] == ']') {
            display_len++;  // 대괄호
        }
        // °와 C는 별도로 카운트 (°는 멀티바이트)
    }
    
    // 온도 표시의 경우 특별 처리
    if (cam_idx == THERMAL_CAM && strstr(label_text, "°C")) {
        display_len = strlen(label_text) - 2 + 1;  // °C를 1문자로 계산
    }
    
    // 패딩 계산
    int padding = (int)(font_size * 0.8);  // 폰트 크기에 비례
    int text_width = (int)(display_len * char_width) + padding * 2;
    int text_height = (int)(font_size * 1.6);  // 폰트의 1.6배
    
    // 최소 크기 보장
    text_width = MAX(text_width, 40);
    text_height = MAX(text_height, 16);
    
    // 1. 라벨 배경 사각형 설정
    display_meta->num_rects = 1;
	NvOSD_RectParams *bg_rect = &display_meta->rect_params[0];

	// 위치 설정 (바운딩 박스 왼쪽 위)
	bg_rect->left = obj_meta->rect_params.left;  // 왼쪽 정렬
	bg_rect->top = obj_meta->rect_params.top - text_height - 2;
	bg_rect->width = text_width;
	bg_rect->height = text_height;
    
    // 배경 활성화
    bg_rect->has_bg_color = 1;
    bg_rect->border_width = 0;
    
    // 바운딩 박스 색상에 따라 라벨 배경색 설정
    NvOSD_ColorParams box_color = obj_meta->rect_params.border_color;
    
    if (box_color.red > 0.5 && box_color.green < 0.5 && box_color.blue < 0.5) {
        bg_rect->bg_color = (NvOSD_ColorParams){0.7, 0.0, 0.0, 0.8};
    } else if (box_color.green > 0.5 && box_color.red < 0.5 && box_color.blue < 0.5) {
        bg_rect->bg_color = (NvOSD_ColorParams){0.0, 0.5, 0.0, 0.8};
    } else if (box_color.blue > 0.5 && box_color.red < 0.5 && box_color.green < 0.5) {
        bg_rect->bg_color = (NvOSD_ColorParams){0.0, 0.0, 0.7, 0.8};
    } else if (box_color.red > 0.5 && box_color.green > 0.5) {
        bg_rect->bg_color = (NvOSD_ColorParams){0.7, 0.7, 0.0, 0.8};
    } else {
        bg_rect->bg_color = (NvOSD_ColorParams){0.0, 0.0, 0.0, 0.8};
    }
    
    // 2. 라벨 텍스트 설정
    display_meta->num_labels = 1;
    NvOSD_TextParams *text_params = &display_meta->text_params[0];
    
    text_params->display_text = g_strdup(label_text);
    
    // 텍스트 위치를 배경 중앙에 맞춤
    text_params->x_offset = bg_rect->left;
    text_params->y_offset = bg_rect->top;
    
    // 폰트 설정
    text_params->font_params.font_name = "Arial";
    text_params->font_params.font_size = font_size;
    text_params->font_params.font_color = (NvOSD_ColorParams){1.0, 1.0, 1.0, 1.0};
    
    // 텍스트 배경 비활성화
    text_params->set_bg_clr = 0;
    
    // 3. 화면 밖으로 나가지 않도록 조정
    if (bg_rect->top < 0) {
        bg_rect->top = obj_meta->rect_params.top + obj_meta->rect_params.height + 2;
        text_params->y_offset = bg_rect->top + (text_height - font_size) / 2;
    }
    
    // 좌우 경계 체크
    if (bg_rect->left < 0) {
        bg_rect->left = 0;
        text_params->x_offset = bg_rect->left + padding;
    } else if (bg_rect->left + bg_rect->width > frame_width) {
        bg_rect->left = frame_width - bg_rect->width;
        text_params->x_offset = bg_rect->left + padding;
    }
    
    // 기존 텍스트 숨기기
    obj_meta->text_params.display_text[0] = 0;
    
    // DisplayMeta를 프레임에 추가
    nvds_add_display_meta_to_frame(frame_meta, display_meta);
}

static GstPadProbeReturn
h264_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    int camera_id = *(int *)user_data;
    
    // 캡스 확인하여 H.264 스트림인지 확인
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps) {
		static gboolean codec_data_saved[NUM_CAMERAS] = {FALSE};
        if (!codec_data_saved[camera_id]) {
            save_codec_data(camera_id, caps);
            codec_data_saved[camera_id] = TRUE;
        }
		
        gchar *caps_str = gst_caps_to_string(caps);
        
        // 디버그: 처음 몇 번만 출력
        static int debug_count = 0;
        if (debug_count++ < 5) {
            printf("Camera %d caps: %s\n", camera_id, caps_str);
        }
        
        // H.264 스트림인지 확인
        if (strstr(caps_str, "video/x-h264")) {
            // 키프레임 확인
            gboolean is_keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            
            // 순환 버퍼에 추가
            add_frame_to_buffer(buffer, is_keyframe, camera_id);
        }
        
        g_free(caps_str);
        gst_caps_unref(caps);
    }
    
    return GST_PAD_PROBE_OK;
}

static void on_event_save_complete(int camera_id, int event_id, const char *filename, const char *http_path, 
                                  gboolean success, double event_time, void *user_data) {
    if (success) {
        g_print("=== Event Save Complete ===\n");
        g_print("Camera: %d\n", camera_id);
        g_print("File: %s\n", filename);
        g_print("Event time: %.2f\n", event_time);
        
        // 여기서 필요한 추가 작업 수행
        // 예: 데이터베이스에 기록, 알림 전송 등
		char event_class_id[2] = {0};
		event_class_id[0] = event_id + '0';

		strcpy(g_curlinfo.video_url, http_path);
		
		notification_request(g_config.camera_id, event_class_id, &g_curlinfo);
        
        // 파일 정보 확인
        struct stat st;
        if (stat(filename, &st) == 0) {
            g_print("File size: %.2f MB\n", st.st_size / (1024.0 * 1024.0));
        }
        
        g_print("========================\n");
    } else {
        g_print("Event save failed for camera %d\n", camera_id);
    }
}

void setup_nv_analysis()
{
	glog_trace("g_config.device_cnt=%d\n", g_config.device_cnt);

	init_all_circular_buffers();
	set_event_save_callback(on_event_save_complete, NULL);

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

		gchar *h264_element_name = g_strdup_printf("h264parse_%d", cam_idx + 1);
    	GstElement *h264parse = gst_bin_get_by_name(GST_BIN(g_pipeline), h264_element_name);

		if (h264parse) {
			// src pad에 프로브 추가 (파싱된 H.264 스트림)
			GstPad *srcpad = gst_element_get_static_pad(h264parse, "src");
			gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
							h264_buffer_probe, &g_cam_indices[cam_idx], NULL);
			gst_object_unref(srcpad);
			g_print("Added probe to camera %d h264parse element\n", cam_idx);
		} else {
			g_warning("Could not find h264parse_%d element\n", cam_idx);
		}

		g_free(h264_element_name);
		gst_object_unref(h264parse);

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

	cleanup_all_circular_buffers();

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