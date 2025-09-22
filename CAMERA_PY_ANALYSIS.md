# camera.py 에러 처리 및 롤백 분석

## 발견된 문제점

### 1. 🔴 **무한 재귀 위험**
```python
# Line 415-433: switch_to_fallback()
def switch_to_fallback(self):
    # 문제: fallback도 실패하면 다시 switch_to_fallback() 호출 가능
    if self.pipeline:
        self.pipeline.set_state(Gst.State.NULL)
    
    self.use_fallback = True
    self.create_pipeline()  
    
    ret = self.pipeline.set_state(Gst.State.PLAYING)
    if ret == Gst.StateChangeReturn.FAILURE:
        logger.error(f"{self.config['name']}: Fallback 파이프라인 시작 실패")
        # 여기서 끝나지만, 다시 on_bus_message에서 호출될 수 있음
```

### 2. 🔴 **중복된 Fallback 전환 로직**
```python
# Line 465-531: switch_from_fallback_to_camera()
# 실패 시 fallback으로 3번 시도하는 중복 코드
if ret == Gst.StateChangeReturn.FAILURE:
    # 489-499: 첫 번째 fallback 복귀
    self.use_fallback = True
    self.create_pipeline()
    ...
    
# 503-512: 두 번째 fallback 복귀 (동일한 코드)
if ret != Gst.StateChangeReturn.SUCCESS:
    self.pipeline.set_state(Gst.State.NULL)
    self.use_fallback = True
    self.create_pipeline()
    ...
    
# 521-530: 세 번째 fallback 복귀 (예외 처리에서)
except Exception as e:
    if not self.use_fallback:
        self.use_fallback = True
        self.create_pipeline()
```

### 3. 🟡 **리소스 누수 가능성**
```python
# Line 604-612: restart_pipeline()
if self.pipeline:
    self.pipeline.send_event(Gst.Event.new_eos())
    self.pipeline.get_state(2 * Gst.SECOND)
    self.pipeline.set_state(Gst.State.NULL)
    time.sleep(2)
    # 문제: self.pipeline을 None으로 설정하지 않음
    # 메모리 해제가 보장되지 않음
```

### 4. 🟡 **디렉토리 생성 실패 시 일관성 없는 처리**
```python
# Line 60-65: __init__
try:
    self.output_dir.mkdir(parents=True, exist_ok=True)
except OSError as e:
    self.recording_enabled = False  # 녹화만 비활성화
    # 문제: 스트리밍은 계속 시도함

# Line 224-228: on_format_location
if not self.is_writable(self.base_output_dir):
    return f"/tmp/cam_{self.config['device_id']}_{fragment_id}.mp4"
    # 문제: /tmp가 가득 차면?
```

### 5. 🟡 **재시작 카운터 리셋 없음**
```python
# Line 571-598: request_restart()
self.restart_count += 1
# 문제: restart_count가 성공 후에도 리셋되지 않음
# 시간이 지나도 누적되어 결국 max_restart_attempts 도달
```

### 6. 🔴 **Deadlock 가능성**
```python
# Line 713-715: stop()
self.watchdog_running = False
if self.watchdog_thread:
    self.watchdog_thread.join(timeout=5)
    # 문제: watchdog_thread가 restart_pipeline()에서 블록될 수 있음
```

## 개선 방안

### 1. Fallback 전환 개선
```python
def switch_to_fallback(self, retry_count=0):
    MAX_RETRY = 3
    if retry_count >= MAX_RETRY:
        logger.critical(f"{self.config['name']}: Fallback 전환 {MAX_RETRY}회 실패")
        self.fatal_error = True
        return False
        
    # 기존 파이프라인 안전하게 정리
    self.cleanup_pipeline()
    
    # fallback 플래그 설정
    self.use_fallback = True
    self.fallback_retry_count = retry_count
    
    try:
        self.create_pipeline()
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        
        if ret == Gst.StateChangeReturn.FAILURE:
            logger.error(f"Fallback 시도 {retry_count + 1} 실패")
            return self.switch_to_fallback(retry_count + 1)
            
        # 성공 시 카운터 리셋
        self.restart_count = 0
        self.fallback_retry_count = 0
        return True
        
    except Exception as e:
        logger.error(f"Fallback 전환 예외: {e}")
        return self.switch_to_fallback(retry_count + 1)
```

### 2. 파이프라인 정리 함수
```python
def cleanup_pipeline(self):
    """파이프라인 안전하게 정리"""
    if self.pipeline:
        try:
            # EOS 전송 (타임아웃 포함)
            self.pipeline.send_event(Gst.Event.new_eos())
            self.pipeline.get_state(1 * Gst.SECOND)
        except:
            pass
            
        try:
            self.pipeline.set_state(Gst.State.NULL)
        except:
            pass
            
        # 명시적으로 None 설정 (가비지 컬렉션)
        self.pipeline = None
        time.sleep(0.5)
```

### 3. 재시작 카운터 관리 개선
```python
def request_restart(self, reason):
    current_time = datetime.now()
    
    # 1시간 후 카운터 리셋
    if self.last_restart_time:
        if (current_time - self.last_restart_time).total_seconds() > 3600:
            self.restart_count = 0
            logger.info("재시작 카운터 리셋")
    
    # 쿨다운 체크
    if self.last_restart_time:
        time_since_last = (current_time - self.last_restart_time).total_seconds()
        if time_since_last < self.restart_cooldown:
            logger.warning(f"재시작 쿨다운 중")
            return
    
    # 최대 횟수 체크
    if self.restart_count >= self.max_restart_attempts:
        logger.critical(f"최대 재시작 횟수 초과")
        self.request_process_restart()
        return
        
    # 재시작 실행
    self.restart_count += 1
    self.last_restart_time = current_time
    
    if self.restart_pipeline():
        # 성공 시 카운터 감소 (완전 리셋 아님)
        self.restart_count = max(0, self.restart_count - 1)
```

### 4. 디스크 공간 체크
```python
def check_disk_space(self, path):
    """디스크 공간 체크"""
    try:
        stat = os.statvfs(path)
        available_mb = (stat.f_bavail * stat.f_frsize) / (1024 * 1024)
        
        if available_mb < 100:  # 100MB 미만
            logger.error(f"디스크 공간 부족: {available_mb:.1f}MB")
            return False
        elif available_mb < 500:  # 500MB 미만 경고
            logger.warning(f"디스크 공간 부족 경고: {available_mb:.1f}MB")
            
        return True
    except:
        return False

def on_format_location(self, splitmux, fragment_id):
    # 디스크 공간 체크 추가
    if not self.check_disk_space(self.base_output_dir):
        # 임시 디렉토리도 체크
        if not self.check_disk_space("/tmp"):
            logger.critical("모든 저장 공간 부족")
            self.recording_enabled = False
            return "dummy.mp4"
        return f"/tmp/cam_{self.config['device_id']}_{fragment_id}.mp4"
```

### 5. Watchdog 개선
```python
def watchdog_thread_func(self):
    """개선된 watchdog"""
    while self.watchdog_running:
        try:
            # Non-blocking 체크
            if self.restart_in_progress:
                time.sleep(1)
                continue
                
            # Fallback 모드에서 주기적 복구 체크
            if self.use_fallback:
                if self.check_camera_recovery():
                    self.switch_from_fallback_to_camera()
                    
            # 프레임 타임아웃 체크
            if self.last_frame_time:
                elapsed = (datetime.now() - self.last_frame_time).total_seconds()
                if elapsed > self.frame_timeout:
                    if not self.restart_in_progress:
                        self.restart_in_progress = True
                        self.request_restart("프레임 타임아웃")
                        self.restart_in_progress = False
                        
            time.sleep(1)
            
        except Exception as e:
            logger.error(f"Watchdog 에러: {e}")
            self.restart_in_progress = False
```

## 권장사항

### 우선순위 높음
1. ✅ Fallback 무한 재귀 방지
2. ✅ 파이프라인 정리 함수 추가
3. ✅ 재시작 카운터 관리 개선

### 우선순위 중간
1. 디스크 공간 모니터링
2. Watchdog deadlock 방지
3. 중복 코드 제거

### 우선순위 낮음
1. 상태 머신 패턴 도입
2. 에러 복구 전략 문서화
3. 단위 테스트 추가