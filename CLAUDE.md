# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 빌드 및 실행 명령어

### 빌드
```bash
# 전체 빌드
make clean && make

# 특정 디렉토리에 설치
make install-to DEST_DIR=/home/nvidia/webrtc

# 로컬 디렉토리에 설치 (기본)
make install
```

### 실행 및 중지
```bash
# 시스템 시작
./start.sh

# 시스템 중지
./stop.sh

# 재시작
./restart.sh
```

### 테스트
```bash
# 디스크 체크 테스트
./test.sh

# curl 라이브러리 테스트
./build/curllib_test
```

## 시스템 아키텍처

### 핵심 구조
이 시스템은 WebRTC 기반 비디오 스트리밍 시스템으로, 다음과 같은 구조를 가집니다:

1. **메인 프로세스 (gstream_main)**
   - WebSocket 서버와 연결 관리
   - 클라이언트 연결 시 webrtc_sender 자식 프로세스 생성
   - 시그널링 메시지 라우팅
   - PTZ 카메라 제어, 이벤트 감지 등 시스템 제어

2. **스트리밍 프로세스 (webrtc_sender)** 
   - 각 클라이언트별 독립 프로세스로 실행
   - WebRTC P2P 연결 처리
   - GStreamer 파이프라인으로 비디오 인코딩 및 전송

3. **통신 구조**
   - WebSocket: 시그널링 서버 ↔ gstream_main
   - Unix Socket: gstream_main ↔ webrtc_sender (포트 6000부터 할당)
   - WebRTC P2P: webrtc_sender ↔ 웹 클라이언트
   - UDP 스트림: 카메라 소스 전달 (포트 5000부터 할당)

### 주요 모듈

- **webrtc_peer.c/h**: 피어 연결 관리, 프로세스 생성
- **gstream_control.c**: PTZ 제어, 이벤트 처리
- **socket_comm.c/h**: 프로세스 간 통신
- **serial_comm.c/h**: PTZ 카메라 시리얼 통신
- **nvds_process.c/h**: NVIDIA DeepStream 연동 (객체 감지)
- **circular_buffer.c/h**: 이벤트 버퍼링
- **device_setting.c/h**: 디바이스 설정 관리
- **ptz_control.c/h**: PTZ 카메라 제어 로직

### Python 스크립트

- **gstream_manage.py**: 메인 프로세스 관리 및 로그 정리
- **camera.py**: 카메라 캡처 및 녹화
- **disk_manager.py**: 디스크 공간 관리
- **thermal_check.py**: 열화상 카메라 연결 모니터링
- **watchdog.py**: 프로세스 모니터링

## 설정 파일

### 메인 설정 (config_main.json)
- 카메라 ID, 서버 주소
- 포트 설정 (소켓: 6000, 스트림: 5000)
- 비디오 소스 및 인코더 설정
- 시리얼 포트 설정

### 디바이스 설정 (device_setting.json)
- PTZ 카메라 설정
- 이벤트 감지 설정
- 녹화 설정

## 개발 시 주의사항

### 프로세스 관리
- 클라이언트별 독립 프로세스로 fork() 사용
- 포트는 클라이언트 인덱스에 따라 동적 할당
- 자식 프로세스 종료 시 cleanup 필수

### DTLS 안정화
- WebRTC 파이프라인 시작 시 단계적 상태 변경 필요
- READY 상태 후 500ms 대기 권장
- 실패 시 재시도 로직 구현

### 로깅
- 로그는 /home/nvidia/webrtc/logs/에 날짜별로 저장
- 30일 이상 된 로그는 자동 삭제
- 로그 레벨: LOG_ERR, LOG_WARN, LOG_INFO, LOG_DEBUG

### DeepStream 연동
- NVDS_VERSION=7.1 (Makefile에서 설정)
- YOLO 모델 사용 (RGB_yoloV7.engine, Thermal_yoloV7.engine)
- 객체 감지 이벤트는 WebSocket으로 서버에 전송