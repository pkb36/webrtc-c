#include "nvds_optical_flow.h"
#include "nvds_process.h"
#include "nvds_utils.h"
#include "logging.h"
#include "device_setting.h"
#include "ptz_control.h"

// 외부 전역 변수 참조
extern ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];
extern int g_move_speed;
extern DeviceSetting g_setting;

void init_opt_flow(int cam_idx, int obj_id, int is_total)
{
    if (is_total) {
        obj_info[cam_idx][obj_id].opt_flow_detected_count = 0;
        obj_info[cam_idx][obj_id].do_opt_flow = 0;
    }
    
    obj_info[cam_idx][obj_id].opt_flow_check_count = 0;
    obj_info[cam_idx][obj_id].move_size_avg = 0.0;
    obj_info[cam_idx][obj_id].prev_x = 0;
    obj_info[cam_idx][obj_id].prev_y = 0;
    obj_info[cam_idx][obj_id].prev_width = 0;
    obj_info[cam_idx][obj_id].prev_height = 0;
}

int get_opt_flow_result(int cam_idx, int obj_id)
{
    glog_debug("[%d][%d].confi=%.2f opt_flow_detected_count ==> %d\n", 
               cam_idx, obj_id, 
               obj_info[cam_idx][obj_id].confidence, 
               obj_info[cam_idx][obj_id].opt_flow_detected_count);
    
    if (obj_info[cam_idx][obj_id].opt_flow_detected_count >= THRESHOLD_OVER_OPTICAL_FLOW_COUNT)
        return 1;
    return 0;
}

int get_opt_flow_object(int cam_idx, int start_obj_id)
{
    for (int obj_id = start_obj_id; obj_id < NUM_OBJS; obj_id++) {
        if (obj_info[cam_idx][obj_id].do_opt_flow) {
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
    int corr_value = 0;
    
    if (diagonal <= SMALL_BBOX_DIAGONAL) {
        corr_value = (int)(100.0 * (SMALL_BBOX_DIAGONAL - diagonal) / SMALL_BBOX_DIAGONAL);
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

void process_opt_flow(NvDsFrameMeta *frame_meta, int cam_idx, int obj_id, int cam_sec_interval)
{
    if (obj_id < 0)
        return;
    
    int count = 0;
    double move_size = 0.0, move_size_total = 0.0, move_size_avg = 0.0;
    double diagonal = 0;
    int row_start = 0, col_start = 0, row_num = 0, col_num = 0;
    int rows = 0, cols = 0;
    int corr_value = 0;
    int bbox_move = 0, rect_size_change = 0;
    
    for (NvDsMetaList *l_user = frame_meta->frame_user_meta_list; l_user != NULL; l_user = l_user->next) {
        NvDsUserMeta *user_meta = (NvDsUserMeta *)(l_user->data);
        
        if (user_meta->base_meta.meta_type == NVDS_OPTICAL_FLOW_META) {
            NvDsOpticalFlowMeta *opt_flow_meta = (NvDsOpticalFlowMeta *)(user_meta->user_meta_data);
            
            if (!opt_flow_meta || !opt_flow_meta->data) {
                glog_error("[process_opt_flow] ERROR: NULL metadata!\n");
                continue;
            }
            
            rows = opt_flow_meta->rows;
            cols = opt_flow_meta->cols;
            NvOFFlowVector *flow_vectors = (NvOFFlowVector *)(opt_flow_meta->data);
            
            // 좌표 변환: x는 column, y는 row
            col_start = obj_info[cam_idx][obj_id].x / 4;
            row_start = obj_info[cam_idx][obj_id].y / 4;
            col_num = obj_info[cam_idx][obj_id].width / 4;
            row_num = obj_info[cam_idx][obj_id].height / 4;
            
            // 경계 체크 및 조정
            if (col_start < 0) col_start = 0;
            if (row_start < 0) row_start = 0;
            if (col_start >= cols) col_start = cols - 1;
            if (row_start >= rows) row_start = rows - 1;
            
            if (col_start + col_num > cols)
                col_num = cols - col_start;
            if (row_start + row_num > rows)
                row_num = rows - row_start;
            
            diagonal = obj_info[cam_idx][obj_id].diagonal;
            move_size_total = 0.0;
            count = 0;
            
            // Process the motion vectors
            for (int row = row_start; row < (row_start + row_num) && row < rows; ++row) {
                for (int col = col_start; col < (col_start + col_num) && col < cols; ++col) {
                    int index = row * cols + col;
                    int max_index = rows * cols;
                    
                    if (index < 0 || index >= max_index) {
                        glog_error("[process_opt_flow] Index out of bounds! index=%d, max=%d\n",
                                   index, max_index);
                        continue;
                    }
                    
                    NvOFFlowVector flow_vector = flow_vectors[index];
                    move_size = calculate_sqrt(flow_vector.flowx, flow_vector.flowy);
                    move_size_total += move_size;
                    count++;
                }
            }
            
            if (count > 0) {
                move_size_avg = move_size_total / (double)count;
                obj_info[cam_idx][obj_id].opt_flow_check_count++;
                obj_info[cam_idx][obj_id].move_size_avg = update_average(
                    obj_info[cam_idx][obj_id].move_size_avg,
                    obj_info[cam_idx][obj_id].opt_flow_check_count,
                    move_size_avg);
            }
            
            if (cam_sec_interval) {
                bbox_move = get_move_distance(cam_idx, obj_id);
                rect_size_change = get_rect_size_change(cam_idx, obj_id);
                set_prev_xy(cam_idx, obj_id);
                set_prev_rect_size(cam_idx, obj_id);
                
                if (obj_info[cam_idx][obj_id].move_size_avg > 0) {
                    glog_trace("[SEC] [%d][%d].move_size_avg=%.1f,confi=%.2f,diag=%.1f\n",
                               cam_idx, obj_id,
                               obj_info[cam_idx][obj_id].move_size_avg,
                               obj_info[cam_idx][obj_id].confidence,
                               diagonal);
                }
                
                if (bbox_move < THRESHOLD_BBOX_MOVE &&
                    rect_size_change < THRESHOLD_RECT_SIZE_CHANGE &&
                    g_move_speed == 0) {
                    
                    corr_value = get_correction_value(diagonal);
                    
                    if (obj_info[cam_idx][obj_id].move_size_avg > (1.5 + (double)corr_value / 10.0)) {
                        obj_info[cam_idx][obj_id].opt_flow_detected_count++;
                        glog_trace("[SEC] optical flow detected ++,obj_info[%d][%d].opt_flow_detected_count=%d\n",
                                   cam_idx, obj_id, obj_info[cam_idx][obj_id].opt_flow_detected_count);
                    }
                } else {
                    glog_trace("[SEC] bbox_move=%d,rect_size_change=%d,g_move_speed=%d\n",
                               bbox_move, rect_size_change, g_move_speed);
                }
                
                init_opt_flow(cam_idx, obj_id, 0);
            }
        }
    }
}