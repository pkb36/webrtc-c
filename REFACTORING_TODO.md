# WebRTC 프로젝트 리팩토링 TODO

## 완료된 작업

### Phase 1: Extern 선언 중앙화 ✅
- `globals.h` 파일 생성하여 119개의 extern 선언 통합
- 7개 .c 파일과 3개 .h 파일에서 중복 선언 제거
- 컴파일 성공 및 기능 유지 확인

### Phase 2: 글로벌 변수 접근자 함수 구현 ✅
- `globals.c`에 안전한 getter/setter 함수 생성
- 직접 글로벌 변수 접근을 함수 호출로 교체
  - `g_config` → `get_config()`
  - `g_setting` → `get_device_setting()`
  - `g_curlinfo` → `get_curl_info()`
- 매개변수 유효성 검사 및 범위 확인 추가

## 향후 작업 계획

### Phase 3: 글로벌 상태 통합 (3-5일, 높은 리스크)

#### 목표
모든 글로벌 변수를 단일 컨텍스트 구조체로 통합하여 완전한 캡슐화 달성

#### 주요 작업
1. **AppContext 구조체 설계**
```c
typedef struct {
    // 설정 관련
    WebRTCConfig config;
    DeviceSetting setting;
    CurlIinfoType curlinfo;
    
    // 파이프라인
    GstElement *pipeline;
    enum AppState app_state;
    
    // 동기화
    pthread_mutex_t send_mutex;
    pthread_mutex_t process_msg_mutex;
    pthread_mutex_t send_info_mutex;
    pthread_mutex_t retry_connect_mutex;
    pthread_mutex_t motion_mutex;
    
    // PTZ 제어
    int source_cam_idx;
    int move_speed;
    int preset_index;
    
    // 이벤트 처리
    int event_recording;
    int event_class_id;
    
    // 객체 감지
    CowTrackingState cow_tracking_state;
    ObjState object_state;
    PersonObj person_objects[MAX_PERSONS];
    
    // 모니터링
    noti_queue notification_queue;
    ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];
    
    // 임계값
    float threshold_confidence[NUM_CLASSES];
    int threshold_event_duration[NUM_CLASSES];
} AppContext;
```

2. **함수 시그니처 변경**
- 모든 함수가 `AppContext*`를 첫 번째 매개변수로 받도록 수정
- 예시:
```c
// 기존
void process_command(const char *cmd);

// 변경 후
void process_command(AppContext *ctx, const char *cmd);
```

3. **콜백 함수 처리**
- GStreamer 콜백에 user_data로 context 전달
- 스레드 함수에 context 전달 구조 구현

4. **초기화 및 종료**
```c
AppContext* app_context_new(void);
void app_context_free(AppContext *ctx);
```

#### 영향 범위
- 모든 .c 파일 (약 20개)
- 대부분의 함수 시그니처 변경
- 스레드 생성 코드 수정
- GStreamer 파이프라인 콜백 수정

#### 리스크 및 완화 방안
- **리스크**: 대규모 변경으로 인한 회귀 버그
- **완화**: 단계적 마이그레이션, 철저한 단위 테스트

### Phase 4: 모듈별 리팩토링 (2-3주)

#### 우선순위별 모듈 정리

1. **socket_comm.c/h** (높음)
   - 전역 변수 제거
   - 소켓 관리 구조체 도입
   - 에러 처리 개선

2. **serial_comm.c/h** (높음)
   - 시리얼 통신 캡슐화
   - 명령 큐 구현
   - 타임아웃 처리 개선

3. **ptz_control.c/h** (중간)
   - PTZ 상태 머신 구현
   - 명령 패턴 적용
   - 프리셋 관리 개선

4. **nvds_process.c/h** (중간)
   - DeepStream 처리 모듈화
   - 이벤트 핸들러 패턴
   - 메모리 관리 개선

5. **circular_buffer.c/h** (낮음)
   - 버퍼 관리 캡슐화
   - 스레드 안전성 강화

### Phase 5: 아키텍처 개선 (1-2개월)

1. **이벤트 시스템 구현**
   - Observer 패턴 도입
   - 비동기 이벤트 처리
   - 메시지 큐 기반 통신

2. **설정 관리 개선**
   - 설정 검증 레이어
   - 런타임 설정 변경
   - 설정 마이그레이션

3. **로깅 시스템 고도화**
   - 구조화된 로깅
   - 로그 레벨별 필터링
   - 원격 로그 전송

4. **에러 처리 표준화**
   - 에러 코드 체계
   - 예외 처리 패턴
   - 복구 메커니즘

## 기술 부채 목록

### 즉시 해결 필요
- [ ] 하드코딩된 경로 제거
- [ ] 매직 넘버 상수화
- [ ] 메모리 누수 위험 지점 수정

### 중기 개선
- [ ] 단위 테스트 추가
- [ ] CI/CD 파이프라인 구축
- [ ] 코드 커버리지 측정

### 장기 목표
- [ ] 마이크로서비스 아키텍처 검토
- [ ] 컨테이너화 (Docker)
- [ ] 성능 프로파일링 및 최적화

## 추정 일정

| Phase | 작업량 | 우선순위 | 리스크 |
|-------|--------|----------|--------|
| Phase 1 | ✅ 완료 | - | - |
| Phase 2 | ✅ 완료 | - | - |
| Phase 3 | 3-5일 | 중간 | 높음 |
| Phase 4 | 2-3주 | 높음 | 중간 |
| Phase 5 | 1-2개월 | 낮음 | 낮음 |

## 참고사항

- 각 Phase는 독립적으로 수행 가능
- Phase 3는 큰 변경사항이므로 충분한 테스트 기간 필요
- 현재 Phase 2까지 완료로도 상당한 코드 품질 개선 달성