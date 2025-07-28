# WebRTC 스트리밍 시스템 통신 구조 가이드

## 목차
1. [시스템 개요](#1-시스템-개요)
2. [주요 컴포넌트](#2-주요-컴포넌트)
3. [통신 구조](#3-통신-구조)
4. [메시지 프로토콜](#4-메시지-프로토콜)
5. [동작 시퀀스](#5-동작-시퀀스)
6. [구현 세부사항](#6-구현-세부사항)

---

## 1. 시스템 개요

### 1.1 아키텍처 다이어그램

```
┌─────────────┐     WebSocket      ┌──────────────┐     Socket      ┌─────────────────┐
│   Web       │ ◄─────────────────► │  Signaling   │ ◄─────────────► │  gstream_main   │
│  Client     │                     │   Server     │                  │   (Parent)      │
└─────────────┘                     └──────────────┘                  └────────┬────────┘
                                                                                │ fork()
                                                                                ▼
┌─────────────┐                                                        ┌─────────────────┐
│   Web       │ ◄─────────────── WebRTC P2P ─────────────────────────►│ webrtc_sender   │
│  Client     │                                                        │  (Child #1)     │
└─────────────┘                                                        └─────────────────┘
```

### 1.2 주요 특징
- **다중 클라이언트 지원**: 각 클라이언트별 독립 프로세스
- **실시간 비디오 스트리밍**: GStreamer + WebRTC
- **양방향 통신**: 비디오 스트리밍 + 제어 명령
- **확장성**: 새 클라이언트 연결 시 동적 프로세스 생성

---

## 2. 주요 컴포넌트

### 2.1 gstream_main (메인 프로세스)
- **역할**: 중앙 제어 및 메시지 라우팅
- **주요 기능**:
  - WebSocket 서버 연결 관리
  - 클라이언트별 webrtc_sender 프로세스 생성
  - 시그널링 메시지 중계
  - 시스템 상태 모니터링

### 2.2 webrtc_sender (자식 프로세스)
- **역할**: 개별 클라이언트 WebRTC 연결 처리
- **주요 기능**:
  - WebRTC 파이프라인 구성
  - SDP Offer/Answer 생성
  - ICE candidate 교환
  - 비디오 스트림 전송

### 2.3 gstream_control
- **역할**: 시스템 제어 기능
- **주요 기능**:
  - PTZ 카메라 제어
  - 이벤트 감지 및 알림
  - 시리얼 통신 처리

---

## 3. 통신 구조

### 3.1 통신 채널

#### A. WebSocket 통신 (시그널링 서버 ↔ gstream_main)
- **프로토콜**: WebSocket over TCP
- **포맷**: JSON
- **용도**: 시그널링, 제어 명령

#### B. Unix Socket 통신 (gstream_main ↔ webrtc_sender)
- **프로토콜**: Unix Domain Socket
- **포맷**: JSON/Text
- **용도**: 프로세스 간 메시지 전달

#### C. WebRTC P2P (webrtc_sender ↔ 웹 클라이언트)
- **프로토콜**: WebRTC (DTLS-SRTP)
- **포맷**: RTP
- **용도**: 실시간 비디오 스트리밍

### 3.2 포트 할당

```
기본 포트:
- WebSocket: 8443 (WSS)
- 소켓 통신 베이스: 6000
- UDP 스트림 베이스: 5000

클라이언트별 할당:
- Client #1: 소켓 6000, UDP 5000-5001
- Client #2: 소켓 6001, UDP 5002-5003
- Client #3: 소켓 6002, UDP 5004-5005
```

---

## 4. 메시지 프로토콜

### 4.1 시그널링 서버 ↔ gstream_main

#### 서버로 전송하는 메시지

**1. 디바이스 등록**
```json
{
  "type": "register",
  "sender_id": "device_001",
  "device_setting": {
    "name": "Camera System 1",
    "capabilities": ["RGB", "Thermal"],
    "location": "Building A"
  }
}
```

**2. SDP 전달**
```json
{
  "type": "peer_sdp",
  "peer_id": "client_123",
  "sdp": {
    "type": "offer",
    "sdp": "v=0\r\no=- 123456 2 IN IP4 127.0.0.1..."
  }
}
```

**3. ICE Candidate**
```json
{
  "type": "peer_ice",
  "peer_id": "client_123",
  "ice": {
    "candidate": "candidate:1 1 UDP 2122260223 192.168.1.100 56789...",
    "sdpMLineIndex": 0
  }
}
```

**4. 이벤트 알림**
```json
{
  "type": "event",
  "event_type": "motion_detected",
  "class_id": 1,
  "cam_idx": 0,
  "timestamp": "2024-01-10T10:30:00Z",
  "details": {
    "confidence": 0.95,
    "location": {"x": 100, "y": 200, "w": 50, "h": 60}
  }
}
```

#### 서버에서 수신하는 메시지

**1. 클라이언트 연결 요청**
```json
{
  "type": "join_room",
  "peer_id": "client_123",
  "channel": "RGB",
  "request_id": "req_456"
}
```

**2. 제어 명령**
```json
{
  "type": "control",
  "cmd": "ptz",
  "data": "FF,01,00,08,20,00,29",
  "target": "camera_1"
}
```

### 4.2 gstream_main ↔ webrtc_sender

#### 텍스트 명령
```
CONNECT          # 연결 확인
STOP_WEBRTC      # 종료 명령
```

#### JSON 메시지
```json
// SDP 전달
{
  "type": "sdp",
  "sdp": {
    "type": "answer",
    "sdp": "v=0\r\no=- 654321 2 IN IP4 127.0.0.1..."
  }
}

// ICE 전달
{
  "type": "ice",
  "ice": {
    "candidate": "candidate:2 1 UDP 2122260223 10.0.0.1 12345...",
    "sdpMLineIndex": 0
  }
}
```

---

## 5. 동작 시퀀스

### 5.1 클라이언트 연결 시퀀스

```
Web Client          Signaling Server       gstream_main         webrtc_sender
    │                      │                     │                     │
    │───Connect WSS────────►│                     │                     │
    │                      │                     │                     │
    │──"join_room"─────────►│                     │                     │
    │                      │──"join_room"────────►│                     │
    │                      │                     │                     │
    │                      │                     │──fork()─────────────►│
    │                      │                     │                     │
    │                      │                     │◄──"CONNECT"──────────┤
    │                      │                     │                     │
    │                      │                     │                     │──create_offer()
    │                      │                     │                     │
    │                      │                     │◄──"peer_sdp"─────────┤
    │                      │◄──"peer_sdp"─────────┤                     │
    │◄──"peer_sdp"──────────┤                     │                     │
    │                      │                     │                     │
    │──"answer_sdp"────────►│                     │                     │
    │                      │──"answer_sdp"───────►│                     │
    │                      │                     │──"answer_sdp"───────►│
    │                      │                     │                     │
    │◄═══════════════WebRTC P2P Connection═══════════════════════════►│
    │                      │                     │                     │
```

### 5.2 PTZ 제어 시퀀스

```
Web Client          Signaling Server       gstream_main       gstream_control
    │                      │                     │                     │
    │──"ptz_control"───────►│                     │                     │
    │                      │──"control"──────────►│                     │
    │                      │                     │──send_ptz_data──────►│
    │                      │                     │                     │
    │                      │                     │                     │──Serial Port──►
    │                      │                     │                     │
```

---

## 6. 구현 세부사항

### 6.1 프로세스 생성 (webrtc_peer.c)

```c
gboolean add_peer_to_pipeline(const gchar *peer_id, const gchar *channel) {
    // 1. 빈 슬롯 찾기
    int peer_idx = find_empty_peer_slot();
    
    // 2. 포트 할당
    int stream_base_port = g_stream_base_port + peer_idx * g_device_cnt;
    int comm_socket_port = g_comm_socket_port + peer_idx;
    
    // 3. 프로세스 생성
    int pid = fork();
    if (pid == 0) {
        // 자식 프로세스: webrtc_sender 실행
        char *args[] = {
            "./webrtc_sender",
            "--peer_id", peer_id,
            "--stream_base_port", stream_base_port,
            "--comm_socket_port", comm_socket_port,
            "--codec_name", "H264",
            NULL
        };
        execv(args[0], args);
    }
    
    // 4. 부모 프로세스: 정보 저장
    g_PeerInfos[peer_idx].peer_id = g_strdup(peer_id);
    g_PeerInfos[peer_idx].child_pid = pid;
}
```

### 6.2 DTLS 안정화 (webrtc_sender.c)

```c
static gboolean start_pipeline(void) {
    // 1. 파이프라인 생성
    pipeline = gst_parse_launch(pipeline_string, &error);
    
    // 2. 단계적 상태 변경 (DTLS 안정화)
    gst_element_set_state(pipeline, GST_STATE_READY);
    g_usleep(500000);  // 500ms 대기
    
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    
    // 3. 에러 처리
    if (ret == GST_STATE_CHANGE_FAILURE) {
        if (retry_count < max_retries) {
            retry_count++;
            return restart_pipeline_on_dtls_error();
        }
    }
}
```

### 6.3 메시지 라우팅 (gstream_main.c)

```c
void on_message_from_server(JsonObject *object) {
    const gchar *type = json_object_get_string_member(object, "type");
    const gchar *peer_id = json_object_get_string_member(object, "peer_id");
    
    if (g_strcmp0(type, "join_room") == 0) {
        // 새 클라이언트 연결
        const gchar *channel = json_object_get_string_member(object, "channel");
        add_peer_to_pipeline(peer_id, channel);
        
    } else if (g_strcmp0(type, "peer_sdp") == 0) {
        // SDP 메시지 전달
        int peer_idx = find_peer_index(peer_id);
        send_to_peer(peer_idx, object);
        
    } else if (g_strcmp0(type, "control") == 0) {
        // 제어 명령 처리
        handle_control_command(object);
    }
}
```

### 6.4 에러 처리 및 재연결

```c
// 연결 실패 시 재시도
static gboolean retry_connection(gpointer user_data) {
    if (connect_retry < MAX_RETRY) {
        connect_retry++;
        connect_to_websocket_server_async();
        return G_SOURCE_CONTINUE;
    }
    return G_SOURCE_REMOVE;
}

// 자식 프로세스 모니터링
void monitor_child_processes() {
    for (int i = 0; i < g_MaxPeerCnt; i++) {
        if (g_PeerInfos[i].child_pid > 0) {
            int status;
            pid_t result = waitpid(g_PeerInfos[i].child_pid, &status, WNOHANG);
            if (result > 0) {
                // 프로세스 종료 감지
                cleanup_peer(i);
            }
        }
    }
}
```

---

## 부록: 주요 설정 파일

### config.json 예시
```json
{
  "server": {
    "host": "signaling.example.com",
    "port": 8443,
    "ssl": true
  },
  "streaming": {
    "codec": "H264",
    "framerate": 15,
    "resolution": "1920x1080"
  },
  "ports": {
    "stream_base": 5000,
    "socket_base": 6000
  }
}
```

---

이 문서는 WebRTC 스트리밍 시스템의 핵심 통신 구조를 설명합니다. 
추가적인 세부사항은 소스 코드를 참조하시기 바랍니다.