#include "nvds_temperature.h"
#include "nvds_process.h"
#include "logging.h"
#include "device_setting.h"
#include <math.h>
#include <string.h>

// 외부 전역 변수 참조
extern ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];
extern DeviceSetting g_setting;
extern int g_frame_count[NUM_CAMS];

// 온도 관련 전역 변수
static float g_temp_avg[NUM_OBJS];
static int g_temp_count[NUM_OBJS];
typedef struct {
    float min_temp;
    float max_temp;
    float avg_temp;
    float std_dev;
    float top_10_percent_threshold;
    int total_count;
    int fever_count;
    int total_cows;  // nvds_process.h의 HerdTempStats와 호환성을 위해 추가
} TempHerdStats;
static TempHerdStats g_herd_stats = {0};

void get_pixel_color(NvBufSurface *surface, guint batch_idx, guint x, guint y,
                     unsigned char *r, unsigned char *g, unsigned char *b, unsigned char *a)
{
    if (!surface || batch_idx >= surface->numFilled) {
        glog_error("Invalid surface or batch index\n");
        return;
    }
    
    NvBufSurfaceParams *surf_params = &surface->surfaceList[batch_idx];
    
    if (x >= surf_params->width || y >= surf_params->height) {
        glog_error("Pixel coordinates out of bounds\n");
        return;
    }
    
    guint pitch = surf_params->pitch;
    guchar *data = (guchar *)surf_params->dataPtr;
    
    if (!data) {
        glog_error("NULL data pointer\n");
        return;
    }
    
    guint offset = y * pitch + x * 4;
    
    *r = data[offset];
    *g = data[offset + 1];
    *b = data[offset + 2];
    *a = data[offset + 3];
}

float map_rgba_to_temp(unsigned char r, unsigned char g, unsigned char b)
{
    float normalized_r = r / 255.0f;
    float min_temp = 20.0f;
    float max_temp = 50.0f;
    float temp = min_temp + (normalized_r * (max_temp - min_temp));
    
    return temp;
}

float map_rgba_to_temp_livestock(unsigned char r, unsigned char g, unsigned char b)
{
    // 가축용 온도 매핑 (37.5°C ~ 42.0°C)
    float min_temp = 37.5f;
    float max_temp = 42.0f;
    
    // RGB를 휘도로 변환
    float luminance = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    
    // 온도로 매핑
    float temp = min_temp + (luminance * (max_temp - min_temp));
    
    return temp;
}

float get_pixel_temp(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    // live_stock_mode는 사용하지 않으므로 기본값 사용
    return map_rgba_to_temp_livestock(r, g, b);
}

// nvds_process.h의 HerdTempStats 구조체 사용
void calculate_herd_temperature_stats(HerdTempStats *stats)
{
    if (!stats) return;
    
    float sum = 0;
    float sum_sq = 0;
    int count = 0;
    float temps[NUM_OBJS];
    int valid_count = 0;
    
    float min_temp = 100.0f;
    float max_temp = 0.0f;
    
    // 모든 객체의 온도 수집
    for (int i = 0; i < NUM_OBJS; i++) {
        if (g_temp_count[i] > 0 && g_temp_avg[i] > 0) {
            float temp = g_temp_avg[i] / g_temp_count[i];
            temps[valid_count++] = temp;
            
            sum += temp;
            sum_sq += temp * temp;
            count++;
            
            if (temp < min_temp) min_temp = temp;
            if (temp > max_temp) max_temp = temp;
            
            if (temp >= TEMP_FEVER_THRESHOLD) {
                // fever_count는 내부 TempHerdStats에만 있음
                g_herd_stats.fever_count++;
            }
        }
    }
    
    if (count > 0) {
        stats->avg_temp = sum / count;
        stats->std_dev = sqrt((sum_sq / count) - (stats->avg_temp * stats->avg_temp));
        stats->total_cows = count;
        
        // 내부 TempHerdStats도 업데이트
        g_herd_stats.min_temp = min_temp;
        g_herd_stats.max_temp = max_temp;
        g_herd_stats.avg_temp = stats->avg_temp;
        g_herd_stats.std_dev = stats->std_dev;
        g_herd_stats.total_count = count;
        
        // 상위 10% 임계값 계산
        if (valid_count > 0) {
            // 간단한 정렬
            for (int i = 0; i < valid_count - 1; i++) {
                for (int j = i + 1; j < valid_count; j++) {
                    if (temps[i] < temps[j]) {
                        float tmp = temps[i];
                        temps[i] = temps[j];
                        temps[j] = tmp;
                    }
                }
            }
            
            int top_10_idx = (int)(valid_count * 0.1);
            if (top_10_idx >= valid_count) top_10_idx = valid_count - 1;
            g_herd_stats.top_10_percent_threshold = temps[top_10_idx];
        }
    }
}

int is_cow_fever(int obj_id, void *herd_stats_ptr)
{
    HerdTempStats *herd_stats = (HerdTempStats *)herd_stats_ptr;
    
    if (!herd_stats || obj_id < 0 || obj_id >= NUM_OBJS) {
        return 0;
    }
    
    if (g_temp_count[obj_id] == 0) {
        return 0;
    }
    
    float obj_temp = g_temp_avg[obj_id] / g_temp_count[obj_id];
    
    // 절대 온도 기준
    if (obj_temp >= TEMP_FEVER_THRESHOLD) {
        return 1;
    }
    
    // 상대 온도 기준 (평균 + 2*표준편차)
    if (herd_stats->total_cows > 5) {
        float threshold = herd_stats->avg_temp + (2.0 * herd_stats->std_dev);
        if (obj_temp >= threshold) {
            return 1;
        }
    }
    
    return 0;
}

int is_cow_in_top_percent(int obj_id, float percent)
{
    if (obj_id < 0 || obj_id >= NUM_OBJS || g_temp_count[obj_id] == 0) {
        return 0;
    }
    
    float obj_temp = g_temp_avg[obj_id] / g_temp_count[obj_id];
    
    return (obj_temp >= g_herd_stats.top_10_percent_threshold) ? 1 : 0;
}

void process_fever_detection(int cam_idx)
{
    if (cam_idx != THERMAL_CAM) {
        return;
    }
    
    // 매 초마다 통계 계산
    if (g_frame_count[cam_idx] % 30 == 0) {
        HerdTempStats stats = {0};
        calculate_herd_temperature_stats(&stats);
        
        // 각 객체에 대해 발열 체크
        for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
            if (obj_info[cam_idx][obj_id].class_id == CLASS_NORMAL_COW ||
                obj_info[cam_idx][obj_id].class_id == CLASS_NORMAL_COW_SITTING) {
                
                if (is_cow_fever(obj_id, &stats)) {
                    // 발열 감지 시 CLASS_HEAT_COW로 변경
                    obj_info[cam_idx][obj_id].class_id = CLASS_HEAT_COW;
                    obj_info[cam_idx][obj_id].heat_count++;
                    
                    glog_info("[FEVER] Object %d detected with fever: %.1f°C (avg: %.1f, std: %.1f)\n",
                              obj_id, 
                              g_temp_avg[obj_id] / g_temp_count[obj_id],
                              stats.avg_temp,
                              stats.std_dev);
                }
            }
        }
    }
}

void get_bbox_temp(GstBuffer *buf, int obj_id)
{
    if (obj_id < 0 || obj_id >= NUM_OBJS) {
        return;
    }
    
    GstMapInfo map_info;
    if (!gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
        glog_error("Failed to map buffer\n");
        return;
    }
    
    NvBufSurface *surface = (NvBufSurface *)map_info.data;
    
    // 바운딩 박스 중심점 온도 측정
    int center_x = obj_info[THERMAL_CAM][obj_id].center_x;
    int center_y = obj_info[THERMAL_CAM][obj_id].center_y;
    
    unsigned char r, g, b, a;
    get_pixel_color(surface, 0, center_x, center_y, &r, &g, &b, &a);
    
    float temp = get_pixel_temp(r, g, b, a);
    
    // 평균 온도 업데이트
    g_temp_avg[obj_id] += temp;
    g_temp_count[obj_id]++;
    
    gst_buffer_unmap(buf, &map_info);
}

void init_temp_avg()
{
    for (int i = 0; i < NUM_OBJS; i++) {
        g_temp_avg[i] = 0;
        g_temp_count[i] = 0;
    }
    
    memset(&g_herd_stats, 0, sizeof(TempHerdStats));
}

int is_temp_duration()
{
    static int frame_count = 0;
    frame_count++;
    
    // 30 프레임마다 (약 1초)
    if (frame_count >= 30) {
        frame_count = 0;
        return 1;
    }
    
    return 0;
}

void temp_display_text(NvDsObjectMeta *obj_meta)
{
    if (!obj_meta || obj_meta->object_id >= NUM_OBJS) {
        return;
    }
    
    int obj_id = obj_meta->object_id;
    
    if (g_temp_count[obj_id] > 0) {
        float avg_temp = g_temp_avg[obj_id] / g_temp_count[obj_id];
        
        char temp_str[64];
        snprintf(temp_str, sizeof(temp_str), "%.1f°C", avg_temp);
        
        // 기존 텍스트에 온도 추가
        if (strlen(obj_meta->text_params.display_text) > 0) {
            strcat(obj_meta->text_params.display_text, " ");
        }
        strcat(obj_meta->text_params.display_text, temp_str);
    }
}

void set_temp_bbox_color(NvDsObjectMeta *obj_meta)
{
    if (!obj_meta || obj_meta->object_id >= NUM_OBJS) {
        return;
    }
    
    int obj_id = obj_meta->object_id;
    
    if (g_temp_count[obj_id] > 0) {
        float avg_temp = g_temp_avg[obj_id] / g_temp_count[obj_id];
        
        // 온도에 따른 색상 설정
        if (avg_temp >= TEMP_FEVER_THRESHOLD) {
            // 빨간색 (발열)
            obj_meta->rect_params.border_color.red = 1.0;
            obj_meta->rect_params.border_color.green = 0.0;
            obj_meta->rect_params.border_color.blue = 0.0;
        } else if (avg_temp >= g_herd_stats.top_10_percent_threshold) {
            // 노란색 (상위 10%)
            obj_meta->rect_params.border_color.red = 1.0;
            obj_meta->rect_params.border_color.green = 1.0;
            obj_meta->rect_params.border_color.blue = 0.0;
        } else {
            // 초록색 (정상)
            obj_meta->rect_params.border_color.red = 0.0;
            obj_meta->rect_params.border_color.green = 1.0;
            obj_meta->rect_params.border_color.blue = 0.0;
        }
    }
}