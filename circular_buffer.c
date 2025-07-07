#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include "circular_buffer.h"

// 전역 순환 버퍼 배열
static H264CircularBuffer circular_buffers[NUM_CAMERAS] = {0};
static guint8 *codec_data[NUM_CAMERAS] = {NULL};
static gsize codec_data_size[NUM_CAMERAS] = {0};
static EventSaveCallback g_save_callback = NULL;
static void *g_callback_user_data = NULL;

void set_event_save_callback(EventSaveCallback callback, void *user_data) {
    g_save_callback = callback;
    g_callback_user_data = user_data;
}

// codec_data 저장 함수
void save_codec_data(int camera_id, GstCaps *caps) {
    if (camera_id < 0 || camera_id >= NUM_CAMERAS) return;
    
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const GValue *codec_data_value = gst_structure_get_value(s, "codec_data");
    
    if (codec_data_value) {
        GstBuffer *codec_buffer = gst_value_get_buffer(codec_data_value);
        if (codec_buffer) {
            GstMapInfo map;
            if (gst_buffer_map(codec_buffer, &map, GST_MAP_READ)) {
                // 기존 데이터 해제
                if (codec_data[camera_id]) {
                    g_free(codec_data[camera_id]);
                }
                
                // 새 데이터 저장
                codec_data[camera_id] = g_malloc(map.size);
                memcpy(codec_data[camera_id], map.data, map.size);
                codec_data_size[camera_id] = map.size;
                
                g_print("Camera %d: Saved codec_data (%lu bytes)\n", 
                        camera_id, map.size);
                
                gst_buffer_unmap(codec_buffer, &map);
            }
        }
    }
}
// 순환 버퍼 초기화 (모든 카메라)
void init_all_circular_buffers(void) {
    for (int cam_id = 0; cam_id < NUM_CAMERAS; cam_id++) {
        H264CircularBuffer *buffer = &circular_buffers[cam_id];
        
        pthread_mutex_init(&buffer->mutex, NULL);
        buffer->write_pos = 0;
        buffer->frame_count = 0;
        buffer->total_frames_written = 0;
        buffer->total_bytes = 0;
        buffer->initialized = TRUE;
        buffer->camera_id = cam_id;
        
        // 각 프레임의 데이터 버퍼 미리 할당
        for (int i = 0; i < MAX_FRAMES; i++) {
            buffer->frames[i].data = g_malloc(MAX_FRAME_SIZE);
            buffer->frames[i].size = 0;
            buffer->frames[i].camera_id = cam_id;
        }
        
        g_print("Camera %d circular buffer initialized: %d frames, %d seconds\n", 
                cam_id, MAX_FRAMES, CIRCULAR_BUFFER_DURATION);
    }
}

// 순환 버퍼 정리
void cleanup_all_circular_buffers(void) {
    for (int cam_id = 0; cam_id < NUM_CAMERAS; cam_id++) {
        H264CircularBuffer *buffer = &circular_buffers[cam_id];
        
        pthread_mutex_lock(&buffer->mutex);
        
        // 프레임 데이터 메모리 해제
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (buffer->frames[i].data) {
                g_free(buffer->frames[i].data);
                buffer->frames[i].data = NULL;
            }
        }
        
        pthread_mutex_unlock(&buffer->mutex);
        pthread_mutex_destroy(&buffer->mutex);
        
        g_print("Camera %d circular buffer cleaned up\n", cam_id);
    }
}

// 특정 카메라의 순환 버퍼에 프레임 추가
void add_frame_to_buffer(GstBuffer *buffer, gboolean is_keyframe, int camera_id) {
    if (camera_id < 0 || camera_id >= NUM_CAMERAS) {
        g_warning("Invalid camera ID: %d\n", camera_id);
        return;
    }
    
    H264CircularBuffer *circ_buffer = &circular_buffers[camera_id];
    
    if (!circ_buffer->initialized) {
        g_warning("Buffer for camera %d not initialized\n", camera_id);
        return;
    }
    
    pthread_mutex_lock(&circ_buffer->mutex);
    
    // 현재 시간
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double timestamp = ts.tv_sec + ts.tv_nsec / 1e9;
    
    // GstBuffer에서 데이터 추출
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        pthread_mutex_unlock(&circ_buffer->mutex);
        return;
    }
    
    // 프레임 크기 확인
    if (map.size > MAX_FRAME_SIZE) {
        g_warning("Camera %d: Frame size %lu exceeds maximum %d\n", 
                  camera_id, map.size, MAX_FRAME_SIZE);
        gst_buffer_unmap(buffer, &map);
        pthread_mutex_unlock(&circ_buffer->mutex);
        return;
    }
    
    // 현재 위치의 프레임에 데이터 복사
    H264Frame *frame = &circ_buffer->frames[circ_buffer->write_pos];
    
    // 이전 데이터가 있으면 total_bytes에서 제거
    if (circ_buffer->frame_count >= MAX_FRAMES) {
        circ_buffer->total_bytes -= frame->size;
    }
    
    // 새 데이터 복사
    memcpy(frame->data, map.data, map.size);
    frame->size = map.size;
    frame->is_keyframe = is_keyframe;
    frame->pts = GST_BUFFER_PTS(buffer);
    frame->timestamp = timestamp;
    frame->index = circ_buffer->total_frames_written;
    
    circ_buffer->total_bytes += map.size;
    circ_buffer->total_frames_written++;

    // printf("Camera %d: Added frame %d, size: %lu bytes, keyframe: %d, timestamp: %.3f\n",
    //        camera_id, circ_buffer->write_pos, map.size, is_keyframe, frame->timestamp);
    
    // 순환 버퍼 위치 업데이트
    circ_buffer->write_pos = (circ_buffer->write_pos + 1) % MAX_FRAMES;
    if (circ_buffer->frame_count < MAX_FRAMES) {
        circ_buffer->frame_count++;
    }
    
    // 10초마다 상태 출력
    if (circ_buffer->total_frames_written % (BUFFER_FPS * 10) == 0) {
        g_print("Camera %d buffer stats - Frames: %d, Size: %.2f MB, Total written: %d\n",
                camera_id, circ_buffer->frame_count,
                circ_buffer->total_bytes / (1024.0 * 1024.0),
                circ_buffer->total_frames_written);
    }
    
    gst_buffer_unmap(buffer, &map);
    pthread_mutex_unlock(&circ_buffer->mutex);
}
int extract_event_clip(int camera_id, double event_time, int before_sec, int after_sec, 
                      H264Frame **out_frames, int *out_frame_count) {
    if (camera_id < 0 || camera_id >= NUM_CAMERAS) {
        return -1;
    }
    
    H264CircularBuffer *circ_buffer = &circular_buffers[camera_id];
    pthread_mutex_lock(&circ_buffer->mutex);
    
    if (circ_buffer->frame_count == 0) {
        pthread_mutex_unlock(&circ_buffer->mutex);
        g_warning("Camera %d buffer is empty\n", camera_id);
        return -1;
    }
    
    double start_time = event_time - before_sec;
    double end_time = event_time + after_sec;
    
    g_print("Event time: %.2f\n", event_time);
    g_print("Requested range: %.2f to %.2f (%.1f seconds)\n", 
            start_time, end_time, end_time - start_time);
    
    // 버퍼의 시작 위치 계산
    int buffer_start = (circ_buffer->write_pos - circ_buffer->frame_count + MAX_FRAMES) % MAX_FRAMES;
    
    // 디버그: 버퍼의 시간 범위 출력
    double buffer_start_time = circ_buffer->frames[buffer_start].timestamp;
    double buffer_end_time = circ_buffer->frames[(circ_buffer->write_pos - 1 + MAX_FRAMES) % MAX_FRAMES].timestamp;
    g_print("Buffer time range: %.2f to %.2f (%.1f seconds)\n", 
            buffer_start_time, buffer_end_time, buffer_end_time - buffer_start_time);
    
    // 시작 프레임 찾기
    int start_idx = -1;
    int end_idx = -1;
    
    for (int i = 0; i < circ_buffer->frame_count; i++) {
        int idx = (buffer_start + i) % MAX_FRAMES;
        H264Frame *frame = &circ_buffer->frames[idx];
        
        // 시작 시간보다 큰 첫 키프레임 찾기
        if (start_idx == -1 && frame->timestamp >= start_time) {
            // 키프레임 찾기 (이전으로 최대 50프레임까지만)
            for (int j = i; j >= MAX(0, i - 50); j--) {
                int kidx = (buffer_start + j) % MAX_FRAMES;
                if (circ_buffer->frames[kidx].is_keyframe) {
                    start_idx = j;
                    break;
                }
            }
            if (start_idx == -1) start_idx = i; // 키프레임 못찾으면 그냥 시작
        }
        
        // 종료 시간 찾기
        if (frame->timestamp <= end_time) {
            end_idx = i;
        } else if (frame->timestamp > end_time) {
            break; // 종료 시간을 넘어서면 중단
        }
    }
    
    if (start_idx == -1 || end_idx == -1 || end_idx < start_idx) {
        pthread_mutex_unlock(&circ_buffer->mutex);
        g_warning("Camera %d: No frames found in time range\n", camera_id);
        return -1;
    }
    
    // 실제 추출할 프레임 정보 출력
    int start_frame_idx = (buffer_start + start_idx) % MAX_FRAMES;
    int end_frame_idx = (buffer_start + end_idx) % MAX_FRAMES;
    double actual_start_time = circ_buffer->frames[start_frame_idx].timestamp;
    double actual_end_time = circ_buffer->frames[end_frame_idx].timestamp;
    
    g_print("Actual extraction: frame %d to %d\n", start_idx, end_idx);
    g_print("Actual time: %.2f to %.2f (%.1f seconds)\n", 
            actual_start_time, actual_end_time, actual_end_time - actual_start_time);
    
    // 프레임 복사
    int frame_count = end_idx - start_idx + 1;
    *out_frames = g_malloc(sizeof(H264Frame) * frame_count);
    *out_frame_count = frame_count;
    
    for (int i = 0; i < frame_count; i++) {
        int src_idx = (buffer_start + start_idx + i) % MAX_FRAMES;
        H264Frame *src = &circ_buffer->frames[src_idx];
        H264Frame *dst = &(*out_frames)[i];
        
        dst->data = g_malloc(src->size);
        memcpy(dst->data, src->data, src->size);
        dst->size = src->size;
        dst->is_keyframe = src->is_keyframe;
        dst->pts = src->pts;
        dst->timestamp = src->timestamp;
        dst->index = src->index;
        dst->camera_id = src->camera_id;
    }
    
    double actual_duration = (*out_frames)[frame_count-1].timestamp - (*out_frames)[0].timestamp;
    g_print("Camera %d: Extracted %d frames, duration: %.1f seconds\n", 
            camera_id, frame_count, actual_duration);
    
    pthread_mutex_unlock(&circ_buffer->mutex);
    return 0;
}

// H.264 클립을 파일로 저장
void save_h264_clip(H264Frame *frames, int frame_count, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        g_error("Failed to open file: %s\n", filename);
        return;
    }
    
    // 카메라 ID 확인 (첫 프레임에서)
    int cam_id = (frame_count > 0) ? frames[0].camera_id : -1;
    
    // codec_data가 있으면 먼저 쓰기
    if (cam_id >= 0 && cam_id < NUM_CAMERAS && codec_data[cam_id]) {
        // AVC 포맷을 Annex B로 변환
        guint8 *data = codec_data[cam_id];
        gsize size = codec_data_size[cam_id];
        
        // SPS/PPS 추출 및 쓰기 (간단한 버전)
        // 실제로는 더 복잡한 파싱이 필요할 수 있음
        guint8 start_code[] = {0x00, 0x00, 0x00, 0x01};
        
        if (size > 5) {
            // Skip first 5 bytes (version, profile, etc)
            int pos = 5;
            
            // SPS
            if (pos + 2 < size) {
                int sps_count = data[pos] & 0x1F;
                pos++;
                
                for (int i = 0; i < sps_count && pos + 2 < size; i++) {
                    int sps_len = (data[pos] << 8) | data[pos + 1];
                    pos += 2;
                    
                    if (pos + sps_len <= size) {
                        fwrite(start_code, 1, 4, fp);
                        fwrite(data + pos, 1, sps_len, fp);
                        pos += sps_len;
                    }
                }
            }
            
            // PPS
            if (pos + 1 < size) {
                int pps_count = data[pos];
                pos++;
                
                for (int i = 0; i < pps_count && pos + 2 < size; i++) {
                    int pps_len = (data[pos] << 8) | data[pos + 1];
                    pos += 2;
                    
                    if (pos + pps_len <= size) {
                        fwrite(start_code, 1, 4, fp);
                        fwrite(data + pos, 1, pps_len, fp);
                        pos += pps_len;
                    }
                }
            }
        }
    }
    
    // 프레임 데이터 쓰기
    size_t total_size = 0;
    for (int i = 0; i < frame_count; i++) {
        fwrite(frames[i].data, 1, frames[i].size, fp);
        total_size += frames[i].size;
    }
    
    fclose(fp);
    g_print("Saved H.264 clip: %s (%.2f MB)\n", filename, total_size / (1024.0 * 1024.0));
}

// MP4로 변환하여 저장 (FFmpeg 사용)
void save_mp4_clip(H264Frame *frames, int frame_count, const char *filename, const char *http_path, int event_id) {
    char temp_h264[PATH_MAX];  // PATH_MAX 사용 (보통 4096)
    snprintf(temp_h264, sizeof(temp_h264), "%s.temp.h264", filename);
    save_h264_clip(frames, frame_count, temp_h264);
    
    // 디버그: 프레임 정보 출력
    g_print("Converting %d frames to MP4\n", frame_count);
    if (frame_count > 0) {
        double duration = frames[frame_count-1].timestamp - frames[0].timestamp;
        double fps = frame_count / duration;
        g_print("Duration: %.1f sec, Estimated FPS: %.1f\n", duration, fps);
    }
    
    // FFmpeg로 변환 (더 큰 버퍼 사용)
    char *cmd = g_malloc(PATH_MAX * 2);  // 동적 할당
    char error_log[PATH_MAX];
    
    snprintf(error_log, sizeof(error_log), "%s.ffmpeg.log", filename);
    
    // 길이 체크와 함께 명령어 생성
    int cmd_len = snprintf(cmd, PATH_MAX * 2,
             "ffmpeg -y -r %d -i %s -c:v copy -movflags faststart %s 2>%s",
             BUFFER_FPS, temp_h264, filename, error_log);
    
    if (cmd_len >= PATH_MAX * 2) {
        g_warning("Command too long, truncated\n");
    }
    
    g_print("Running: %s\n", cmd);
    
    int ret = system(cmd);
    gboolean success = (ret == 0);
    
    if (success) {
        g_print("Converted to MP4: %s\n", filename);
        unlink(temp_h264);
        unlink(error_log);
    } else {
        g_warning("FFmpeg failed (exit code: %d). Check %s for errors\n", 
                  WEXITSTATUS(ret), error_log);
        g_print("H.264 file kept: %s\n", temp_h264);
    }

    if (g_save_callback && frame_count > 0) {
        int camera_id = frames[0].camera_id;
        double event_time = frames[frame_count/2].timestamp; // 중간 시점
        g_save_callback(camera_id, event_id, filename, http_path, success, event_time, g_callback_user_data);
    }
    
    g_free(cmd);  // 메모리 해제
}

// 이벤트 발생 워커 스레드
void* save_event_worker(void *arg) {
    SaveEventTask *task = (SaveEventTask*)arg;
    
    g_print("\n=== Save Event Worker ===\n");
    g_print("Camera %d: Event at %.2f\n", task->camera_id, task->event_time);
    g_print("Waiting %d seconds for post-event data...\n", task->after_sec + 1);
    
    sleep(task->after_sec + 1);
    
    g_print("Extracting clip: %d sec before, %d sec after\n", 
            task->before_sec, task->after_sec);
    
    H264Frame *frames = NULL;
    int frame_count = 0;
    
    if (extract_event_clip(task->camera_id, task->event_time, 
                          task->before_sec, task->after_sec, 
                          &frames, &frame_count) == 0) {
        // MP4로 저장
        save_mp4_clip(frames, frame_count, task->filename, task->http_path, task->event_id);
        
        // 메모리 해제
        for (int i = 0; i < frame_count; i++) {
            g_free(frames[i].data);
        }
        g_free(frames);
    }
    
    g_free(task);
    return NULL;
}

// 이벤트 발생 시 호출할 함수
void on_event_detected(int camera_id, int class_id, double event_time) {
    SaveEventTask *task = g_malloc(sizeof(SaveEventTask));
    task->camera_id = camera_id;
    task->event_id = class_id;  // 이벤트
    task->event_time = event_time;
    task->before_sec = 15;
    task->after_sec = 15;
    
    // 파일명 생성 - 수정된 버전
    time_t t = (time_t)event_time;
    struct tm *local_time = localtime(&t);
    char date_folder[256];
    
    sprintf(date_folder, "%s/EVENT_%04d%02d%02d", 
            g_config.record_path,  // 이 경로는 전달받아야 함
            local_time->tm_year + 1900,
            local_time->tm_mon + 1,
            local_time->tm_mday);
    
    // 폴더 존재 확인 및 생성
    struct stat info;
    if (stat(date_folder, &info) != 0) {
        mkdir(date_folder, 0777);
        g_print("Created directory: %s\n", date_folder);
    }

    sprintf(task->filename, "%s/EVENT_%04d%02d%02d/CAM%d_%02d%02d%02d.mp4",
            g_config.record_path,
            local_time->tm_year + 1900,
            local_time->tm_mon + 1,
            local_time->tm_mday,
            camera_id,
            local_time->tm_hour,
            local_time->tm_min,
            local_time->tm_sec);

    sprintf(task->http_path, "http://%s/data/EVENT_%04d%02d%02d/CAM%d_%02d%02d%02d.mp4",
            g_config.http_service_ip,
            local_time->tm_year + 1900,
            local_time->tm_mon + 1,
            local_time->tm_mday,
            camera_id,
            local_time->tm_hour,
            local_time->tm_min,
            local_time->tm_sec);
    
    // 비동기로 저장 작업 실행
    pthread_t thread;
    pthread_create(&thread, NULL, save_event_worker, task);
    pthread_detach(thread);
    
    g_print("Camera %d: Event : %d Event detected at %.2f, clip save scheduled\n", 
            camera_id, class_id, event_time);
    g_print("Save path: %s\n", task->filename);
}

// 버퍼 상태 확인
void get_buffer_status(int camera_id, int *frame_count, double *duration, size_t *total_size) {
    if (camera_id < 0 || camera_id >= NUM_CAMERAS) {
        return;
    }
    
    H264CircularBuffer *buffer = &circular_buffers[camera_id];
    pthread_mutex_lock(&buffer->mutex);
    
    if (frame_count) *frame_count = buffer->frame_count;
    if (total_size) *total_size = buffer->total_bytes;
    
    if (duration && buffer->frame_count > 0) {
        int start_idx = (buffer->write_pos - buffer->frame_count + MAX_FRAMES) % MAX_FRAMES;
        int end_idx = (buffer->write_pos - 1 + MAX_FRAMES) % MAX_FRAMES;
        *duration = buffer->frames[end_idx].timestamp - buffer->frames[start_idx].timestamp;
    }
    
    pthread_mutex_unlock(&buffer->mutex);
}