#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h> 
#include <sys/stat.h>
#include "gstream_main.h"
#include "config.h"
#include "process_cmd.h"
#include "event_recorder.h"
#include "device_setting.h"
#include "video_convert.h"


int get_udp_port(UDPClientProcess process, CameraDevice device, StreamChoice stream_choice, int stream_index)
{
	int udp_port = 0;
	int max_sender_port = g_config.stream_base_port + (MAIN_STREAM_PORT_SPACE * stream_choice) + (g_config.device_cnt * g_config.max_stream_cnt) - 1;
	int max_recorder_port = max_sender_port + 1 + g_config.device_cnt - 1;

	if (process == SENDER)
		udp_port = g_config.stream_base_port + (MAIN_STREAM_PORT_SPACE * stream_choice) + (g_config.device_cnt * stream_index) + device;
	else if (process == RECORDER)
		udp_port = (max_sender_port + 1) + device;
	else if (process == EVENT_RECORDER)
		udp_port = (max_recorder_port + 1) + device;

	return udp_port;
}


void start_event_buf_process(int cam_idx)
{
	char cmd[256];
	int stream_base_port = get_udp_port(EVENT_RECORDER, cam_idx, g_config.event_record_enc_index, 0); 

	sprintf(cmd, "./record_event_buffer.sh %d %d &", g_config.event_buf_time, stream_base_port);
	glog_trace("cmd=%s\n", cmd);

	system(cmd); 
}

const char* get_file_extension(const char* codec_name) {
    if (strcmp("H264", codec_name) == 0) {
        return ".mp4";
    } else {
        return ".webm";  // VP9, VP8
    }
}

int trigger_event_record(int cam_idx, char* http_str_path_out) {
	char codec_name[32] = {0};
	if(strstr(g_config.video_enc[0], "vp8")){
		strcpy(codec_name, "VP8");
	} else if(strstr(g_config.video_enc[0], "vp9")){
		strcpy(codec_name, "VP9");
	} else if(strstr(g_config.video_enc[0], "264")){
		strcpy(codec_name, "H264");
	}

    glog_trace("trigger_event_record cam_idx=%d, codec=%s\n", cam_idx, codec_name);
    
    int pid = check_process(5200 + cam_idx);
    if(pid != -1){
        glog_trace("event recording is already running [(5200+cam_idx)=%d] [pid=%d]\n", 5200 + cam_idx, pid);
        return FALSE;
    }

    char str_time[256];
    char str_file[512];
    char http_str_path[512];
    char cmd[1024];
    
    // 1. make path
    struct tm *local_time;
    time_t t;
    
    t = time(NULL);
    local_time = localtime(&t);

    // make folder name
    sprintf(str_time, "EVENT_%04d%02d%02d", local_time->tm_year + 1900, local_time->tm_mon+1, local_time->tm_mday);
    sprintf(str_file, "%s/%s", g_config.record_path, str_time);
    
    // date 폴더가 존재하는지 확인하고 없을 경우는 폴더를 생성함
    struct stat info;
    if (stat(str_file, &info) != 0) {
        // stat 함수가 실패하면 폴더가 존재하지 않는 것으로 간주
        mkdir(str_file, 0777);
    }
    
    // 코덱에 따른 파일 확장자 결정
    const char* file_ext = get_file_extension(codec_name);
    
    sprintf(str_time, "EVENT_%04d%02d%02d/CAM%d_%02d%02d%02d", 
        local_time->tm_year + 1900, local_time->tm_mon+1, local_time->tm_mday,
        cam_idx, local_time->tm_hour, local_time->tm_min, local_time->tm_sec);
    
    // 동적 파일 확장자 사용
    sprintf(str_file, "%s/%s%s", g_config.record_path, str_time, file_ext);
    sprintf(http_str_path, "http://%s/data/%s%s", g_config.http_service_ip, str_time, file_ext);

    // 동적 코덱 이름 사용
    sprintf(cmd, "./webrtc_event_recorder --stream_cnt=%d --stream_base_port=%d --codec_name=%s --location=%s --duration=%d &",
        1, 5200 + cam_idx, codec_name, str_file, g_config.event_buf_time * 2);
    
    system(cmd);
    glog_trace("cmd=%s\n", cmd);

#if VIDEO_FORMAT
    // H264인 경우 이미 MP4이므로 변환 불필요
    if (strcmp("H264", codec_name) != 0) {
        char str_temp[512];
        convert_webm_to_mp4((g_config.event_buf_time * 2) + 3, str_file);
        strcpy(str_temp, str_file);
        change_extension(str_temp, ".mp4", str_file);
        strcpy(str_temp, http_str_path);
        change_extension(str_temp, ".mp4", http_str_path);
    }
#endif

    glog_trace("generate file[%s], http[%s]\n", str_file, http_str_path);
    if(http_str_path_out){
        strcpy(http_str_path_out, http_str_path);
    }

    return TRUE;
}
