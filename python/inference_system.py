#!/usr/bin/env python3
"""
추론 프로세스와 검출 데이터 관리 시스템
- UDP로 카메라 스트림 수신
- 객체 검출 수행 (현재는 더미)
- 검출 데이터를 공유 메모리에 저장
- 이벤트 시스템에서 조회 가능한 API 제공
"""

import cv2
import numpy as np
import multiprocessing as mp
from multiprocessing import Process, Queue, Manager, Lock
from multiprocessing.managers import BaseManager
import time
from datetime import datetime, timedelta
from pathlib import Path
import json
import socket
import struct
import threading
from collections import deque
import logging
import signal
import sys
import random

# 로깅 설정
log_dir = Path('logs')
log_dir.mkdir(exist_ok=True)
log_filename = log_dir / f'inference_{datetime.now().strftime("%Y%m%d")}.log'

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(log_filename, encoding='utf-8'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


class DetectionBuffer:
    """검출 데이터 버퍼 - 프로세스 간 공유용"""
    
    def __init__(self, buffer_duration=120):
        self.buffer_duration = buffer_duration
        self.camera_buffers = {}
        self.lock = Lock()
        
    def add_detection(self, camera, frame_data, detections):
        """검출 데이터 추가"""
        with self.lock:
            if camera not in self.camera_buffers:
                self.camera_buffers[camera] = deque(maxlen=10000)  # 최대 10000 프레임
            
            detection_data = {
                'timestamp': datetime.now(),
                'frame_number': frame_data.get('frame_number', 0),
                'camera': camera,
                'objects': detections
            }
            
            self.camera_buffers[camera].append(detection_data)
            
            # 오래된 데이터 제거
            cutoff_time = datetime.now() - timedelta(seconds=self.buffer_duration)
            while (self.camera_buffers[camera] and 
                   self.camera_buffers[camera][0]['timestamp'] < cutoff_time):
                self.camera_buffers[camera].popleft()
                
    def get_detections_for_timerange(self, camera, start_time, end_time):
        """특정 시간 범위의 검출 데이터 조회"""
        detections = []
        
        with self.lock:
            if camera in self.camera_buffers:
                for data in self.camera_buffers[camera]:
                    if start_time <= data['timestamp'] <= end_time:
                        # 복사본 생성 (프로세스 간 전달을 위해)
                        detections.append({
                            'timestamp': data['timestamp'].isoformat(),
                            'frame_number': data['frame_number'],
                            'camera': data['camera'],
                            'objects': data['objects'].copy() if data['objects'] else []
                        })
                        
        return detections
        
    def get_latest_detection(self, camera):
        """최신 검출 데이터 조회"""
        with self.lock:
            if camera in self.camera_buffers and self.camera_buffers[camera]:
                latest = self.camera_buffers[camera][-1]
                return {
                    'timestamp': latest['timestamp'].isoformat(),
                    'frame_number': latest['frame_number'],
                    'camera': latest['camera'],
                    'objects': latest['objects'].copy() if latest['objects'] else []
                }
        return None
        
    def get_buffer_stats(self):
        """버퍼 통계 조회"""
        with self.lock:
            stats = {}
            for camera, buffer in self.camera_buffers.items():
                if buffer:
                    stats[camera] = {
                        'buffer_size': len(buffer),
                        'oldest_timestamp': buffer[0]['timestamp'].isoformat(),
                        'latest_timestamp': buffer[-1]['timestamp'].isoformat(),
                        'duration_seconds': (buffer[-1]['timestamp'] - buffer[0]['timestamp']).total_seconds()
                    }
                else:
                    stats[camera] = {
                        'buffer_size': 0,
                        'oldest_timestamp': None,
                        'latest_timestamp': None,
                        'duration_seconds': 0
                    }
            return stats


class DummyInferenceEngine:
    """더미 추론 엔진 - 실제 추론 엔진으로 교체 가능"""
    
    def __init__(self):
        self.frame_count = 0
        self.classes = ['person', 'car', 'truck', 'bicycle', 'motorcycle', 'bus']
        
    def detect(self, frame, camera_name):
        """더미 객체 검출"""
        self.frame_count += 1
        
        # 100프레임마다 랜덤하게 객체 생성 (10초마다)
        if random.random() > 0.3:
            num_objects = random.randint(1, 3)
            detections = []
            
            h, w = frame.shape[:2] if frame is not None else (1080, 1920)
            
            for _ in range(num_objects):
                # 랜덤 바운딩 박스 생성
                x1 = random.randint(0, w - 200)
                y1 = random.randint(0, h - 200)
                x2 = x1 + random.randint(50, 200)
                y2 = y1 + random.randint(50, 300)
                
                detection = {
                    'class': random.choice(self.classes),
                    'confidence': round(random.uniform(0.7, 0.99), 2),
                    'bbox': [x1, y1, x2, y2]
                }
                detections.append(detection)
                
            return detections
        
        return []


class InferenceProcess:
    """추론 프로세스"""
    
    def __init__(self, camera_config, detection_buffer, event_callback=None):
        self.camera_config = camera_config
        self.detection_buffer = detection_buffer
        self.event_callback = event_callback
        self.running = True
        
        # UDP 수신 설정
        self.udp_port = camera_config.get('udp_port', 5000)
        self.udp_socket = None
        
        # 추론 엔진
        self.inference_engine = DummyInferenceEngine()
        
        # 자동 이벤트 트리거 비활성화
        self.auto_trigger_enabled = False
        
        # 통계
        self.stats = {
            'frames_received': 0,
            'frames_processed': 0,
            'detections_count': 0,
            'objects_detected': 0
        }
        
    def setup_udp_receiver(self):
        """UDP 수신기 설정"""
        self.udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024*1024*10)  # 10MB 버퍼
        self.udp_socket.bind(('0.0.0.0', self.udp_port))
        self.udp_socket.settimeout(1.0)
        logger.info(f"{self.camera_config['name']}: UDP 수신 대기 중 (포트 {self.udp_port})")
        
    def process_frame(self, frame_data):
        """프레임 처리 및 추론"""
        self.stats['frames_received'] += 1
        
        # 실제로는 H.264 스트림을 디코딩해야 하지만, 더미에서는 None 사용
        frame = None
        
        # 추론 수행
        detections = self.inference_engine.detect(frame, self.camera_config['name'])
        
        if detections:
            # 검출 데이터 버퍼에 저장 (항상)
            self.detection_buffer.add_detection(
                self.camera_config['name'],
                {'frame_number': self.stats['frames_received']},
                detections
            )
            
            self.stats['detections_count'] += 1
            self.stats['objects_detected'] += len(detections)
            
            # 자동 이벤트 트리거는 비활성화
            if self.auto_trigger_enabled:
                # 중요 객체 검출 시 이벤트 트리거
                for det in detections:
                    if det['class'] in ['person', 'car'] and det['confidence'] > 0.8:
                        if self.event_callback:
                            event_data = {
                                'type': 'object_detected',
                                'timestamp': datetime.now(),
                                'camera': self.camera_config['name'],
                                'metadata': {
                                    'timestamp': datetime.now(),
                                    'camera': self.camera_config['name'],
                                    'objects': detections
                                }
                            }
                            self.event_callback(event_data)
                            logger.info(f"이벤트 트리거: {det['class']} (신뢰도: {det['confidence']})")
                        break
                    
        self.stats['frames_processed'] += 1
        
        # 통계 로그 (1000프레임마다)
        if self.stats['frames_processed'] % 1000 == 0:
            logger.info(f"{self.camera_config['name']} 통계: "
                       f"수신: {self.stats['frames_received']}, "
                       f"처리: {self.stats['frames_processed']}, "
                       f"검출프레임: {self.stats['detections_count']}, "
                       f"총객체: {self.stats['objects_detected']}")
            
    def run(self):
        """추론 프로세스 실행"""
        logger.info(f"{self.camera_config['name']} 추론 프로세스 시작")
        
        try:
            self.setup_udp_receiver()
            
            while self.running:
                try:
                    # UDP 패킷 수신 (RTP 헤더 포함)
                    data, addr = self.udp_socket.recvfrom(65536)
                    
                    # 간단한 RTP 헤더 파싱 (12바이트)
                    if len(data) > 12:
                        # 실제로는 H.264 NAL 유닛을 파싱해야 함
                        self.process_frame({'data': data[12:]})
                        
                except socket.timeout:
                    continue
                except Exception as e:
                    logger.error(f"프레임 수신 오류: {e}")
                    
        except Exception as e:
            logger.error(f"추론 프로세스 오류: {e}")
        finally:
            if self.udp_socket:
                self.udp_socket.close()
            logger.info(f"{self.camera_config['name']} 추론 프로세스 종료")
            
    def stop(self):
        """프로세스 중지"""
        self.running = False


# 커스텀 매니저 클래스
class DetectionBufferManager(BaseManager):
    pass


# DetectionBuffer를 프로세스 간 공유 가능하도록 등록
DetectionBufferManager.register('DetectionBuffer', DetectionBuffer)


class InferenceSystem:
    """추론 시스템 메인 클래스"""
    
    def __init__(self, config_file='inference_config.json'):
        self.config = self.load_config(config_file)
        self.processes = []
        self.stop_event = mp.Event()
        
        # 초기화
        self.manager = None
        self.detection_buffer = None
        
        try:
            # 공유 검출 버퍼 매니저
            self.manager = DetectionBufferManager()
            self.manager.start()
            self.detection_buffer = self.manager.DetectionBuffer(
                buffer_duration=self.config.get('buffer_duration', 120)
            )
        except Exception as e:
            logger.error(f"매니저 초기화 실패: {e}")
            raise
        
        # TCP 서버 (이벤트 전송용)
        self.event_tcp_client = None
        if self.config.get('event_tcp_enabled', True):
            self.setup_event_client()
            
        # 키보드 입력 스레드
        self.keyboard_thread = None
        
    def load_config(self, config_file):
        """설정 파일 로드"""
        default_config = {
            'cameras': [
                {
                    'name': 'RGB_Camera',
                    'udp_port': 5000,
                    'enabled': True
                },
                {
                    'name': 'Thermal_Camera', 
                    'udp_port': 5001,
                    'enabled': True
                }
            ],
            'buffer_duration': 120,
            'save_interval': 30,
            'event_tcp_enabled': True,
            'event_tcp_host': 'localhost',
            'event_tcp_port': 9999,
            'api_server_enabled': True,
            'api_server_port': 8888,
            'auto_trigger': False  # 자동 트리거 비활성화
        }
        
        config_path = Path(config_file)
        if config_path.exists():
            with open(config_path, 'r') as f:
                return json.load(f)
        else:
            with open(config_path, 'w') as f:
                json.dump(default_config, f, indent=2)
            logger.info(f"기본 설정 파일 생성: {config_file}")
            return default_config
            
    def keyboard_listener(self):
        """키보드 입력 처리"""
        logger.info("키보드 리스너 시작 - 이벤트 트리거: 'e' (RGB), 't' (Thermal), 'q' (종료)")
        
        while not self.stop_event.is_set():
            try:
                import select
                import termios
                import tty
                
                # 터미널 설정 저장
                old_settings = termios.tcgetattr(sys.stdin)
                
                try:
                    # raw 모드로 전환
                    tty.setraw(sys.stdin.fileno())
                    
                    # 입력 대기 (0.1초 타임아웃)
                    if select.select([sys.stdin], [], [], 0.1)[0]:
                        key = sys.stdin.read(1).lower()
                        
                        if key == 'q':
                            logger.info("종료 요청")
                            self.stop_event.set()  # stop() 대신 이벤트만 설정
                            break
                        elif key == 'e':
                            self.trigger_manual_event('RGB_Camera')
                        elif key == 't':
                            self.trigger_manual_event('Thermal_Camera')
                        elif key == 's':
                            self.show_stats()
                        elif key == 'h':
                            # 터미널 복원 후 출력
                            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
                            print("\n사용 가능한 명령:")
                            print("  e - RGB 카메라 이벤트 트리거")
                            print("  t - Thermal 카메라 이벤트 트리거")
                            print("  s - 통계 보기")
                            print("  h - 도움말")
                            print("  q - 종료\n")
                            # 다시 raw 모드로
                            tty.setraw(sys.stdin.fileno())
                            
                finally:
                    # 터미널 설정 복원
                    termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
                    
            except ImportError:
                # Windows 환경에서는 msvcrt 사용
                import msvcrt
                
                if msvcrt.kbhit():
                    key = msvcrt.getch().decode('utf-8').lower()
                    
                    if key == 'q':
                        logger.info("종료 요청")
                        self.stop_event.set()
                        break
                    elif key == 'e':
                        self.trigger_manual_event('RGB_Camera')
                    elif key == 't':
                        self.trigger_manual_event('Thermal_Camera')
                    elif key == 's':
                        self.show_stats()
                    elif key == 'h':
                        print("\n사용 가능한 명령:")
                        print("  e - RGB 카메라 이벤트 트리거")
                        print("  t - Thermal 카메라 이벤트 트리거")
                        print("  s - 통계 보기")
                        print("  h - 도움말")
                        print("  q - 종료\n")
                        
                time.sleep(0.1)
                
            except Exception as e:
                if not self.stop_event.is_set():
                    logger.error(f"키보드 처리 오류: {e}")
                time.sleep(0.1)
                
        logger.info("키보드 리스너 종료")
                
    def trigger_manual_event(self, camera_name):
        """수동 이벤트 트리거"""
        # 최신 검출 데이터 가져오기
        latest = self.detection_buffer.get_latest_detection(camera_name)
        
        if latest and latest['objects']:
            event_data = {
                'type': 'manual_trigger',
                'timestamp': datetime.now(),
                'camera': camera_name,
                'metadata': {
                    'timestamp': datetime.now(),
                    'camera': camera_name,
                    'objects': latest['objects']
                }
            }
            
            self.send_event(event_data)
            logger.info(f"수동 이벤트 트리거: {camera_name} - {len(latest['objects'])}개 객체")
        else:
            logger.warning(f"검출된 객체 없음: {camera_name}")
            
            # 빈 이벤트라도 전송
            event_data = {
                'type': 'manual_trigger',
                'timestamp': datetime.now(),
                'camera': camera_name,
                'metadata': {
                    'timestamp': datetime.now(),
                    'camera': camera_name,
                    'objects': []
                }
            }
            
            self.send_event(event_data)
            logger.info(f"수동 이벤트 트리거 (객체 없음): {camera_name}")
            
    def show_stats(self):
        """통계 표시"""
        print("\n=== 검출 버퍼 통계 ===")
        stats = self.detection_buffer.get_buffer_stats()
        
        for camera, stat in stats.items():
            print(f"\n{camera}:")
            print(f"  버퍼 크기: {stat['buffer_size']} 프레임")
            if stat['buffer_size'] > 0:
                print(f"  버퍼 시간: {stat['duration_seconds']:.1f}초")
                print(f"  최신 검출: {stat['latest_timestamp']}")
                
                # 최신 검출 정보
                latest = self.detection_buffer.get_latest_detection(camera)
                if latest and latest['objects']:
                    print(f"  최신 객체: {len(latest['objects'])}개")
                    for obj in latest['objects']:
                        print(f"    - {obj['class']} ({obj['confidence']:.0%})")
        print("==================\n")
            
    def setup_event_client(self):
        """이벤트 전송 클라이언트 설정"""
        self.event_tcp_host = self.config.get('event_tcp_host', 'localhost')
        self.event_tcp_port = self.config.get('event_tcp_port', 9999)
        
    def send_event(self, event_data):
        """이벤트 레코더로 이벤트 전송"""
        try:
            # TCP 연결
            client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            client.settimeout(2.0)
            client.connect((self.event_tcp_host, self.event_tcp_port))
            
            # JSON 인코딩
            message = json.dumps(event_data, default=str).encode('utf-8')
            message_length = len(message).to_bytes(4, 'big')
            
            # 전송
            client.sendall(message_length + message)
            response = client.recv(1024)
            client.close()
            
            if response == b"OK":
                logger.debug(f"이벤트 전송 성공: {event_data['type']}")
            else:
                logger.warning(f"이벤트 전송 실패: {response.decode()}")
                
        except Exception as e:
            logger.error(f"이벤트 전송 오류: {e}")
            
    def api_server_process(self):
        """API 서버 - 외부에서 검출 데이터 조회"""
        logger.info("API 서버 시작")
        
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(('0.0.0.0', self.config.get('api_server_port', 8888)))
        server_socket.listen(5)
        server_socket.settimeout(1.0)
        
        logger.info(f"API 서버 대기 중: 포트 {self.config.get('api_server_port', 8888)}")
        
        while not self.stop_event.is_set():
            try:
                client_socket, addr = server_socket.accept()
                
                # 요청 읽기
                request = client_socket.recv(4096).decode('utf-8')
                
                try:
                    req_data = json.loads(request)
                    
                    if req_data['action'] == 'get_detections':
                        # 검출 데이터 조회
                        camera = req_data['camera']
                        start_time = datetime.fromisoformat(req_data['start_time'])
                        end_time = datetime.fromisoformat(req_data['end_time'])
                        
                        detections = self.detection_buffer.get_detections_for_timerange(
                            camera, start_time, end_time
                        )
                        
                        response = json.dumps({
                            'status': 'success',
                            'detections': detections
                        })
                        
                    elif req_data['action'] == 'get_latest':
                        # 최신 검출 데이터
                        camera = req_data['camera']
                        latest = self.detection_buffer.get_latest_detection(camera)
                        
                        response = json.dumps({
                            'status': 'success',
                            'detection': latest
                        })
                        
                    else:
                        response = json.dumps({
                            'status': 'error',
                            'message': 'Unknown action'
                        })
                        
                except Exception as e:
                    response = json.dumps({
                        'status': 'error',
                        'message': str(e)
                    })
                    
                client_socket.sendall(response.encode('utf-8'))
                client_socket.close()
                
            except socket.timeout:
                continue
            except Exception as e:
                logger.error(f"API 서버 오류: {e}")
                
        server_socket.close()
        logger.info("API 서버 종료")
        
    def start(self):
        """시스템 시작"""
        logger.info("추론 시스템 시작")
        
        # API 서버 프로세스
        if self.config.get('api_server_enabled', True):
            api_process = mp.Process(target=self.api_server_process)
            api_process.start()
            self.processes.append(api_process)
            
        # 각 카메라별 추론 프로세스 시작
        for camera_config in self.config['cameras']:
            if not camera_config.get('enabled', True):
                continue
                
            # 이벤트 콜백 설정 (자동 트리거가 활성화된 경우만)
            event_callback = None
            if self.config.get('auto_trigger', False):
                def event_callback(event_data):
                    if self.config.get('event_tcp_enabled', True):
                        self.send_event(event_data)
                    
            # 추론 프로세스 생성
            inference = InferenceProcess(
                camera_config,
                self.detection_buffer,
                event_callback
            )
            
            process = mp.Process(target=inference.run)
            process.start()
            self.processes.append(process)
            
        logger.info(f"{len(self.processes)}개 프로세스 시작")
        
        # 키보드 리스너 스레드
        self.keyboard_thread = threading.Thread(target=self.keyboard_listener)
        self.keyboard_thread.daemon = True
        self.keyboard_thread.start()
        
    def _periodic_save(self):
        """주기적으로 버퍼 저장"""
        while not self.stop_event.is_set():
            time.sleep(self.config.get('save_interval', 30))
            
            try:
                saved_files = self.detection_buffer.save_to_file('detection_buffers')
                if saved_files:
                    logger.info(f"버퍼 저장 완료: {len(saved_files)}개 파일")
            except Exception as e:
                logger.error(f"버퍼 저장 실패: {e}")
                
    def stop(self):
        """시스템 종료"""
        logger.info("추론 시스템 종료 시작")
        
        self.stop_event.set()
        
        # 키보드 스레드 종료 대기
        if hasattr(self, 'keyboard_thread') and self.keyboard_thread and self.keyboard_thread.is_alive():
            self.keyboard_thread.join(timeout=1)
        
        # 모든 프로세스에 종료 신호
        for process in self.processes:
            if process.is_alive():
                process.terminate()
        
        # 프로세스 종료 대기
        for process in self.processes:
            process.join(timeout=2)
            if process.is_alive():
                logger.warning(f"프로세스 강제 종료: {process.pid}")
                process.kill()
                process.join(timeout=1)
                
        # 매니저 종료
        try:
            self.manager.shutdown()
        except:
            pass
        
        logger.info("추론 시스템 종료 완료")


def test_api_client():
    """API 클라이언트 테스트"""
    import socket
    import json
    from datetime import datetime, timedelta
    
    # 검출 데이터 조회 요청
    request = {
        'action': 'get_detections',
        'camera': 'RGB_Camera',
        'start_time': (datetime.now() - timedelta(seconds=30)).isoformat(),
        'end_time': datetime.now().isoformat()
    }
    
    try:
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.connect(('localhost', 8888))
        client.sendall(json.dumps(request).encode('utf-8'))
        
        response = client.recv(4096).decode('utf-8')
        result = json.loads(response)
        
        print(f"응답: {result['status']}")
        if result['status'] == 'success':
            print(f"검출 수: {len(result['detections'])}")
            
        client.close()
        
    except Exception as e:
        print(f"API 호출 실패: {e}")


def main():
    """메인 함수"""
    if len(sys.argv) > 1:
        if sys.argv[1] == '--test-api':
            test_api_client()
            return
            
    system = InferenceSystem()
    
    # 시그널 핸들러 - 더 강력한 종료 처리
    def signal_handler(sig, frame):
        logger.info(f"종료 신호 수신: {sig}")
        try:
            system.stop()
        except:
            pass
        
        # 강제 종료
        import os
        os._exit(0)
        
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    try:
        system.start()
        
        # 메인 프로세스는 대기
        while not system.stop_event.is_set():
            time.sleep(0.5)
            
    except KeyboardInterrupt:
        logger.info("키보드 인터럽트")
        system.stop()
    except Exception as e:
        logger.error(f"예상치 못한 오류: {e}")
        system.stop()
    finally:
        # 최종 정리
        try:
            system.stop()
        except:
            pass
        
        # 잔여 프로세스 정리
        for p in mp.active_children():
            logger.warning(f"잔여 프로세스 종료: {p.name}")
            p.terminate()
            p.join(timeout=1)
            
        logger.info("프로그램 완전 종료")


if __name__ == "__main__":
    main()