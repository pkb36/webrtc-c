# 📡 이벤트 발생 시 시그널링 서버 전송 흐름

## 🔄 **전체 시스템 흐름**

```
🔍 AI 객체 탐지 → 📊 이벤트 수집 → ✅ 알림 조건 체크 → 📸 스냅샷 생성 → 🚀 HTTP POST 전송
```

## 📋 **단계별 상세 분석**

### **1. AI 객체 탐지 및 이벤트 수집**
**파일**: `nvds_process.c:2286`
```c
// AI 모델에서 객체 탐지 시 호출
send_event_to_recorder_simple(class_id, g_source_cam_idx);
```

### **2. 이벤트 알림 체크**
**파일**: `nvds_process.c:296-312`
```c
int send_notification_to_server(int class_id, int camera_id)
{
    glog_trace("try sending class_id=%d, camera_id=%d, enable_event_notify=%d\n", 
               class_id, camera_id, g_setting.enable_event_notify);

    if (g_setting.enable_event_notify)  // 이벤트 알림 활성화 체크
    {
        g_event_recording = 1;  // 이벤트 녹화 시작
        g_timeout_add_seconds(30, event_recording_timeout, NULL);  // 30초 타이머

        if (send_event_to_recorder_simple(class_id, camera_id) == TRUE)
        {
            // 이벤트 녹화 및 알림 처리
            return TRUE;
        }
    }
    return FALSE;
}
```

### **3. HTTP API 호출 준비**
**파일**: `curllib.c:243-337`

#### **3-1. 서버 설정** (`config.json`)
```json
{
    "event_user_id" : "01027061463",
    "event_user_pw" : "12341234", 
    "event_server_ip" : "52.194.238.184",
    "camera_id":"ITC100A-23081001"
}
```

#### **3-2. API 엔드포인트**
```c
static char *login_url = "/api/login/";           // 로그인
static char *notification_url = "/api/notification/create/";  // 이벤트 알림
```

### **4. 인증 토큰 획득**
```c
// 토큰이 없으면 먼저 로그인
if (j->token[0] == 0)
    login_request(j);  // POST /api/login/

// 로그인 요청 데이터
{
    "email": "01027061463",
    "password": "12341234", 
    "fcmToken": ""
}

// 로그인 응답
{
    "token": "b70cf154882db7992749ee56646fc2a5dcdf31f0",
    "user": {
        "id": 1,
        "email": "01027061463" 
    }
}
```

### **5. 이벤트 알림 전송**
**함수**: `notification_request(cam, evt, j)`

#### **5-1. HTTP POST 요청**
```
URL: http://52.194.238.184/api/notification/create/
Method: POST
Content-Type: multipart/form-data
Authorization: Token b70cf154882db7992749ee56646fc2a5dcdf31f0
```

#### **5-2. 전송 데이터**
```c
// 멀티파트 폼 데이터
field = curl_mime_addpart(form);
curl_mime_name(field, "camera");                    // 카메라 ID
curl_mime_data(field, cam, CURL_ZERO_TERMINATED);

field = curl_mime_addpart(form);
curl_mime_name(field, "notificationCategory");      // 이벤트 유형
curl_mime_data(field, evt, CURL_ZERO_TERMINATED);

field = curl_mime_addpart(form);
curl_mime_name(field, "image");                     // 스냅샷 이미지
curl_mime_filedata(field, j->snapshot_path);        // /home/nvidia/webrtc/cam0_snapshot.jpg

field = curl_mime_addpart(form);
curl_mime_name(field, "video_url");                 // 비디오 URL
curl_mime_data(field, j->video_url, strlen(j->video_url));

field = curl_mime_addpart(form);
curl_mime_name(field, "position");                  // PTZ 위치 (선택적)
curl_mime_data(field, j->position, CURL_ZERO_TERMINATED);
```

## 🎯 **실제 전송 예시**

### **탐지된 이벤트**
```
class_id = 1 (예: 사람 탐지)
camera_id = 0 (RGB 카메라)
```

### **API 호출**
```bash
curl -X POST http://52.194.238.184/api/notification/create/ \
  -H "Authorization: Token b70cf154882db7992749ee56646fc2a5dcdf31f0" \
  -F "camera=ITC100A-23081001" \
  -F "notificationCategory=person" \
  -F "image=@/home/nvidia/webrtc/cam0_snapshot.jpg" \
  -F "video_url=rtmp://server/live/cam0" \
  -F "position=preset1"
```

### **서버 응답**
```json
{
    "id": 12345,
    "camera": "ITC100A-23081001",
    "notificationCategory": "person",
    "created_at": "2024-08-28T15:30:15+09:00",
    "status": "sent"
}
```

## ⚙️ **설정 및 제어**

### **이벤트 알림 활성화/비활성화**
```c
// device_setting.json에서 제어
g_setting.enable_event_notify = 1;  // 1: 활성화, 0: 비활성화
```

### **스냅샷 경로**
```c
// 카메라별 스냅샷 파일
CAM0: /home/nvidia/webrtc/cam0_snapshot.jpg
CAM1: /home/nvidia/webrtc/cam1_snapshot.jpg
```

### **타임아웃 설정**
```c
curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);        // 10초 전송 타임아웃
curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);  // 5초 연결 타임아웃
```

## 🔧 **동시성 및 안정성**

### **WebSocket 연결 (실시간 통신)**
```c
// gstream_main.c:930
gchar *url_path = g_strdup_printf("%s/signaling/%s/?token=test&peerType=camera", 
                                  g_config.server_ip, g_config.camera_id);
// WebSocket URL: ws://52.194.238.184/signaling/ITC100A-23081001/?token=test&peerType=camera
```

### **HTTP vs WebSocket 역할 분담**
- **HTTP POST**: 이벤트 알림 + 이미지 전송 (일회성)
- **WebSocket**: 실시간 명령/제어 통신 (지속적)

### **스레드 안전성**
```c
pthread_mutex_lock(&g_send_mutex);          // 전송 보호
soup_websocket_connection_send_text(ws_conn, message);
pthread_mutex_unlock(&g_send_mutex);
```

## 🚨 **에러 처리**

### **네트워크 오류**
```c
if (res != CURLE_OK)
    glog_error("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
else
    glog_trace("Notification request sent successfully.\n");
```

### **인증 실패**
- 토큰 만료 시 자동 재로그인
- HTTP 401/403 응답 시 토큰 갱신

### **리소스 정리**
```c
cleanup:
    curl_mime_free(form);
    if (headerlist) curl_slist_free_all(headerlist);
    curl_easy_cleanup(curl);
```

## 📊 **모니터링 포인트**

### **로그 확인**
```bash
# 이벤트 전송 로그
tail -f /var/log/syslog | grep "Notification request"

# 응답 확인
cat /home/nvidia/webrtc/event_result.html
```

### **디버깅**
```c
#define CURLLIB_DEBUG 1  // curllib.c에서 디버그 모드 활성화
```

## 🎉 **정리**

✅ **AI 객체 탐지** → nvds_process.c에서 이벤트 수집  
✅ **조건 체크** → enable_event_notify 설정 확인  
✅ **인증** → `/api/login/`으로 토큰 획득  
✅ **알림 전송** → `/api/notification/create/`로 POST 전송  
✅ **데이터 포함** → 카메라ID, 이벤트유형, 스냅샷, 비디오URL, PTZ위치  
✅ **실시간 통신** → WebSocket으로 양방향 명령/제어  

이벤트 발생 시 **HTTP API를 통해 스냅샷과 메타데이터를 시그널링 서버**로 전송하는 완전한 시스템입니다! 🚀