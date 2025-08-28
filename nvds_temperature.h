#ifndef __NVDS_TEMPERATURE_H__
#define __NVDS_TEMPERATURE_H__

#include <gst/gst.h>
#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
#include "global_define.h"

// 온도 관련 상수
#define THRESHOLD_UNDER_TEMP_DEFAULT          15
#define THRESHOLD_UPPER_TEMP_DEFAULT          50
#define HEAT_COUNT_THRESHOLD                  1
#define TEMP_FEVER_THRESHOLD                  39.0
#define TEMP_TOP_PERCENT_THRESHOLD           10.0

// HerdTempStats는 nvds_process.h에 정의되어 있음

// 온도 측정 관련 함수
void get_pixel_color(NvBufSurface *surface, guint batch_idx, guint x, guint y, 
                     unsigned char *r, unsigned char *g, unsigned char *b, unsigned char *a);
float map_rgba_to_temp(unsigned char r, unsigned char g, unsigned char b);
float map_rgba_to_temp_livestock(unsigned char r, unsigned char g, unsigned char b);
float get_pixel_temp(unsigned char r, unsigned char g, unsigned char b, unsigned char a);

// 온도 통계 및 발열 감지
// calculate_herd_temperature_stats는 nvds_process.h에 선언됨
int is_cow_fever(int obj_id, void *herd_stats);
int is_cow_in_top_percent(int obj_id, float percent);
void process_fever_detection(int cam_idx);

// 바운딩 박스 온도 처리
void get_bbox_temp(GstBuffer *buf, int obj_id);
void init_temp_avg();
int is_temp_duration();
void simulate_get_temp_avg();
void add_correction();

// 온도 디스플레이
void temp_display_text(NvDsObjectMeta *obj_meta);
void set_temp_bbox_color(NvDsObjectMeta *obj_meta);

#endif /* __NVDS_TEMPERATURE_H__ */