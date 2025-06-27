#ifndef __GLOBAL_DEFINE_H__
#define __GLOBAL_DEFINE_H__

#define TRACK_PERSON_INCLUDE                    0
#define MINDULE_INCLUDE                         0               //LJH, 20241030, linkage with mindule tag 
#define MINDULE_BLOCK_NOTIFICATION              0               //LJH, 20250207, for test
#define OPTICAL_FLOW_INCLUDE                    1
#define THERMAL_TEMP_INCLUDE                    1

#define RESNET_50                               1
#define VIDEO_FORMAT                            1
#define TEMP_NOTI                               1
#define TEMP_NOTI_TEST                          0               //for test
#define NOTI_BLOCK_FOR_TEST                     0               //for test

#define TRIGGER_TRACKER_FOR_VIDEO_CLIP          0               //for test, apply this for testing with video clip  

#define NUM_OBJS                              300
#define NUM_CAMS    2
// 색상 열거형
typedef enum {
    BBOX_GREEN = 0,
    BBOX_YELLOW,
    BBOX_RED,
    BBOX_BLUE,
    BBOX_NONE
} BboxColor;

typedef enum {
	RGB_CAM = 0,
	THERMAL_CAM,
	NUMBER_CAMS,
} CameraDevice;

typedef enum {
	SENDER = 0,
	RECORDER,
	EVENT_RECORDER,
} UDPClientProcess;

typedef enum {
	MAIN_STREAM = 0,
	SECOND_STREAM,
} StreamChoice;

typedef struct {
    guint64 timestamp;      // 타임스탬프 (나노초)
    guint frame_number;     // 프레임 번호
    guint camera_id;        // 카메라 ID
    guint num_objects;      // 검출된 객체 수
    struct {
        gint class_id;
        gfloat confidence;
        gfloat x, y, width, height;
        BboxColor bbox_color;   // 박스 색상 추가
        gboolean has_bbox;      // 박스 표시 여부
    } objects[NUM_OBJS];    // 객체 정보 배열
} DetectionData;


#endif
