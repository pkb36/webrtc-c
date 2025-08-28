#ifndef __NVDS_OPTICAL_FLOW_H__
#define __NVDS_OPTICAL_FLOW_H__

#include <gst/gst.h>
#include "gstnvdsmeta.h"
#include "nvds_opticalflow_meta.h"
#include "global_define.h"

// Optical Flow 관련 상수
#define THRESHOLD_OVER_OPTICAL_FLOW_COUNT     2
#define THRESHOLD_BBOX_MOVE                   30
#define THRESHOLD_RECT_SIZE_CHANGE            30
#define MAX_OPT_FLOW_ITERATIONS               1000
#ifndef SMALL_BBOX_DIAGONAL
#define SMALL_BBOX_DIAGONAL                   160.0
#endif

// Optical Flow 관련 함수
void init_opt_flow(int cam_idx, int obj_id, int is_total);
int get_opt_flow_result(int cam_idx, int obj_id);
int get_opt_flow_object(int cam_idx, int start_obj_id);
void process_opt_flow(NvDsFrameMeta *frame_meta, int cam_idx, int obj_id, int cam_sec_interval);

// 움직임 계산 함수
int get_move_distance(int cam_idx, int obj_id);
int get_rect_size_change(int cam_idx, int obj_id);
void set_prev_xy(int cam_idx, int obj_id);
void set_prev_rect_size(int cam_idx, int obj_id);

// 보정값 계산
int get_correction_value(double diagonal);
double update_average(double previous_average, int count, double new_value);

#endif /* __NVDS_OPTICAL_FLOW_H__ */