#!/usr/bin/env python3
"""
Jetson Xavier NX 듀얼 카메라 시스템
- RGB/Thermal 카메라 캡처
- H.264 녹화 with 타임스탬프 인덱싱
- UDP 스트리밍 바이패스
- YUYV 포맷 지원
- 프레임 수신 모니터링 및 자동 재시작 기능 추가
"""

import cv2
import numpy as np
import multiprocessing as mp
from multiprocessing import Process, Queue, Event
import time
from datetime import datetime, timedelta
from pathlib import Path
import json
import struct
import gi
import logging
import signal
import sys
import subprocess
import os
from datetime import datetime
import argparse
import threading

gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib

# 로깅 설정
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# 환경 변수 설정
if 'DISPLAY' in os.environ:
    del os.environ['DISPLAY']
    
# GStreamer 초기화
Gst.init(None)

class CameraRecorder:
    """카메라별 녹화 및 스트리밍 클래스"""
    
    def __init__(self, camera_config, output_dir, test_file=None):
        self.config = camera_config
        self.base_output_dir = Path(output_dir)
        self.test_file = test_file
        self.recording_enabled = True
        
        current_date = datetime.now().strftime("RECORD_%Y%m%d")
        self.output_dir = self.base_output_dir / current_date
        # self.output_dir.mkdir(parents=True, exist_ok=True)

        try:
            self.output_dir.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            logger.error(f"디렉토리 생성 실패 ({e.errno}): {self.output_dir} - {e}")
            self.recording_enabled = False  # 녹화 비활성화
            logger.warning(f"{self.config['name']}: 녹화 비활성화됨 - 스트리밍만 동작")

        # 녹화 관련
        self.video_file = None
        self.indexer = None
        self.frame_count = 0
        self.byte_offset = 0
        self.segment_start_time = None
        
        # GStreamer 파이프라인
        self.pipeline = None
        self.loop = None
        
        # 프레임 수신 모니터링
        self.last_frame_time = None
        self.frame_timeout = 10.0  # 10초 동안 프레임이 없으면 문제로 판단
        self.watchdog_thread = None
        self.watchdog_running = False
        self.restart_callback = None
        
        # 재시작 관련
        self.restart_count = 0
        self.max_restart_attempts = 5
        self.last_restart_time = None
        self.restart_cooldown = 30  # 30초 이내 재시작 방지

        self.fragment_count = 0
        
    def set_restart_callback(self, callback):
        """재시작 콜백 설정"""
        self.restart_callback = callback

    def get_output_pattern(self):
        """splitmuxsink용 파일명 패턴 생성"""
        if not self.recording_enabled:
            return "dummy.mp4"  # 더미 값 반환
            
        current_date = datetime.now().strftime("RECORD_%Y%m%d")
        output_dir = self.base_output_dir / current_date
        
        try:
            output_dir.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            logger.error(f"디렉토리 생성 실패: {output_dir} - {e}")
            return "dummy.mp4"  # 에러 시 더미 값 반환
        
        # CAM0 또는 CAM1 형식으로 카메라 구분
        if 'RGB' in self.config['name']:
            cam_prefix = 'CAM0'
        elif 'Thermal' in self.config['name']:
            cam_prefix = 'CAM1'
        else:
            # device_id로 구분
            cam_prefix = f"CAM{self.config['device_id']}"
        
        # CAM0_%H%M%S 형식 (시분초만)
        pattern = str(output_dir / f"{cam_prefix}_%07d.mp4")
        return pattern

    def create_pipeline(self):
        """GStreamer 파이프라인 생성"""
        
        if self.test_file:
            camera_source = self._get_file_source()
        else:
            camera_source = self._get_camera_source()

        encoder_settings = self._get_encoder_settings()
        send_encoder_settings = self._get_send_encoder_settings()
        
        # 녹화가 비활성화된 경우 스트리밍만 수행
        if not self.recording_enabled:
            pipeline_str = f"""
                {camera_source} !
                queue ! videoconvert name=videoconvert0 ! video/x-raw,format=I420 ! 
                rtpvrawpay mtu=65000 !
                udpsink host={self.config['udp_host']} port={self.config['udp_port']} sync=false async=false
            """
            logger.warning(f"{self.config['name']}: 스트리밍 전용 파이프라인 생성")
        else:
            # 기존 전체 파이프라인
            initial_location = "dummy.mp4"
            pipeline_str = f"""
                {camera_source} !
                tee name=t
                
                t. ! queue max-size-buffers=100 max-size-bytes=0 max-size-time=0 ! 
                videoconvert name=videoconvert0 ! 
                video/x-raw, format=I420 !
                nvvidconv ! 
                video/x-raw(memory:NVMM), format=I420 ! 
                {encoder_settings} !
                h264parse !
                splitmuxsink name=filesink 
                    location={initial_location}
                    max-size-time=300000000000 
                    mux-properties="properties,reserved-moov-update-period=1000000000"
                
                t. ! queue ! videoconvert ! video/x-raw,format=I420 ! 
                rtpvrawpay mtu=65000 !
                udpsink host={self.config['udp_host']} port={self.config['udp_port']} sync=false async=false
            """
        
        logger.info(f"파이프라인 생성: {self.config['name']}")
        logger.debug(f"파이프라인: {pipeline_str}")
        
        self.pipeline = Gst.parse_launch(pipeline_str)
        
        h264parse = self.pipeline.get_by_name('videoconvert0')  # 또는 명시적으로 name 지정
        # if not h264parse:
        #     # 파이프라인에서 h264parse 찾기
        #     elements = self.pipeline.iterate_elements()
        #     for element in elements:
        #         if 'h264parse' in element.get_name():
        #             h264parse = element
        #             break
        
        if h264parse:
            # src pad에 프로브 추가
            srcpad = h264parse.get_static_pad('src')
            srcpad.add_probe(Gst.PadProbeType.BUFFER, self.on_frame_probe, None)
            logger.info(f"{self.config['name']}: 프레임 모니터링 프로브 추가")

        splitmux = self.pipeline.get_by_name('filesink')
        if splitmux:
            # format-location 시그널 연결
            splitmux.connect('format-location', self.on_format_location)
            logger.info(f"{self.config['name']}: format-location 시그널 연결")

        # 버스 메시지 핸들러
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect('message', self.on_bus_message)

    def on_format_location(self, splitmux, fragment_id):
        """splitmuxsink가 새 파일을 생성할 때 호출되는 콜백"""
        if not self.recording_enabled:
            return "dummy.mp4"  # 녹화 비활성화 시 더미 값
            
        current_time = datetime.now()
        current_date = current_time.strftime("RECORD_%Y%m%d")
        output_dir = self.base_output_dir / current_date
        
        try:
            output_dir.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            logger.error(f"디렉토리 생성 실패: {output_dir} - {e}")
            self.recording_enabled = False  # 녹화 비활성화
            return "dummy.mp4"
        
        # 카메라 ID 결정
        cam_id = 0 if self.config['device_id'] == 0 else 1
        
        # 파일명 생성
        filename = current_time.strftime(f"CAM{cam_id}_%H%M%S.mp4")
        full_path = str(output_dir / filename)
        
        # 프레임 수신 시간 업데이트 (파일이 생성된다는 것은 데이터가 들어온다는 의미)
        self.last_frame_time = current_time
        
        # 로그
        if fragment_id == 0:
            logger.info(f"{self.config['name']}: 녹화 시작 - {full_path}")
        else:
            logger.info(f"{self.config['name']}: 새 파일 생성 - {full_path} (fragment {fragment_id})")
        
        return full_path

    def on_frame_probe(self, pad, info, user_data):
        """프레임 프로브 콜백 - 프레임 수신 모니터링용"""
        # 프레임 수신 시간 업데이트
        self.last_frame_time = datetime.now()
        
        # if self.config['name'] == "RGB_Camera":
        print(f"{self.config['name']}: 프레임 수신 - {self.frame_count} (시간: {self.last_frame_time})")

        # 주기적인 로그 (선택사항)
        self.frame_count += 1
        if self.frame_count % 1000 == 0:
            logger.info(f"{self.config['name']}: {self.frame_count} 프레임 처리됨")
        
        return Gst.PadProbeReturn.OK

    def _get_camera_source(self):
        """카메라 소스 GStreamer 엘리먼트"""
        if(self.config['device_id'] == 2):
            # Thermal 카메라의 경우 v4l2src를 사용하여 YUYV 포맷으로 캡처
            return (
                f"v4l2src device=/dev/video{self.config['device_id']} ! "
                f"video/x-raw, format=YUY2, width=384, height=290, "
                f"height={self.config['height']},framerate=50/1 ! "
                f"videocrop top=1 bottom=1 ! "
                f"videorate ! "  # 10fps 초과분만 드롭
                f"video/x-raw, framerate=10/1"
            )
        else:
            #YUYV 포맷을 명시적으로 지정
            return (
                f"v4l2src device=/dev/video{self.config['device_id']} ! "
                f"video/x-raw, format=YUY2, width={self.config['width']}, "
                f"height={self.config['height']}, framerate=30/1 ! "
                f"videorate ! "  # 10fps 초과분만 드롭
                f"video/x-raw, framerate=10/1"
            )

    def _get_file_source(self):
        """MP4 파일 소스 GStreamer 엘리먼트"""
        # 파일 존재 확인 (파일이 지정된 경우)
        if self.test_file and self.test_file != "test":
            if not Path(self.test_file).exists():
                logger.error(f"테스트 파일을 찾을 수 없습니다: {self.test_file}")
                raise FileNotFoundError(f"Test file not found: {self.test_file}")
            
            logger.info(f"테스트 파일 사용: {self.test_file}")
            
            # 절대 경로 가져오기
            abs_path = os.path.abspath(self.test_file)
            
            # parsebin을 사용하여 동적 패드 문제 해결
            # parsebin은 자동으로 demuxer와 parser를 처리
            return (
                f"filesrc location={abs_path} ! "
                f"parsebin ! "
                f"nvv4l2decoder ! "
                f"nvvidconv ! "
                f"video/x-raw, format=I420 ! "
                f"videoscale ! "
                f"video/x-raw, width={self.config['width']}, height={self.config['height']} ! "
                f"videorate ! "
                f"video/x-raw, framerate=10/1"
            )
        else:
            # 테스트 패턴 사용 (파일 없이 테스트)
            logger.info(f"테스트 패턴 사용 (videotestsrc)")
            
            # videotestsrc로 테스트 패턴 생성
            # pattern: 0=smpte, 1=snow, 2=black, 18=ball, 19=smpte75
            return (
                f"videotestsrc pattern=0 ! "
                f"video/x-raw, format=I420, width={self.config['width']}, "
                f"height={self.config['height']}, framerate={self.config['fps']}/1"
            )

    def _get_encoder_settings(self):
        """인코더 설정"""
        bitrate = self.config.get('bitrate', 2000000)
        
        # H.264 인코더 설정 (키프레임 간격 30 = 1초)
        return (
            f"nvv4l2h264enc bitrate={bitrate} preset-level=2 "
            f"idrinterval=10 insert-sps-pps=true "
            f"profile=2 maxperf-enable=true"  # High profile, 최대 성능
        )

    def _get_send_encoder_settings(self):
        """인코더 설정"""
        # H.264 인코더 설정 (키프레임 간격 30 = 1초)
        return (
            f"nvv4l2h264enc bitrate=8000000 preset-level=2 "
            f"idrinterval=10 insert-sps-pps=true "
            f"profile=2 maxperf-enable=true"  # High profile, 최대 성능
        )
        
    def _is_keyframe(self, data):
        """키프레임 여부 확인"""
        if len(data) > 4:
            # NAL unit type 확인
            i = 0
            while i < min(len(data) - 4, 100):  # 처음 100바이트만 확인
                if data[i:i+3] == b'\x00\x00\x01':
                    nal_type = data[i+3] & 0x1F
                    if nal_type == 5:  # IDR slice
                        return True
                    i += 3
                elif data[i:i+4] == b'\x00\x00\x00\x01':
                    nal_type = data[i+4] & 0x1F
                    if nal_type == 5:  # IDR slice
                        return True
                    i += 4
                else:
                    i += 1
        
        return False
        
    def on_bus_message(self, bus, message):
        """GStreamer 버스 메시지 처리"""
        t = message.type
        if t == Gst.MessageType.EOS:
            logger.info(f"{self.config['name']}: EOS 수신")
            
        elif t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            logger.error(f"{self.config['name']}: 에러 - {err}, {debug}")
            # 에러 발생 시 파이프라인 재시작 시도
            self.request_restart("GStreamer 에러 발생")
        elif t == Gst.MessageType.WARNING:
            warn, debug = message.parse_warning()
            logger.warning(f"{self.config['name']}: 경고 - {warn}, {debug}")
            
    def watchdog_thread_func(self):
        """프레임 수신 모니터링 스레드"""
        logger.info(f"{self.config['name']}: 워치독 스레드 시작")
        
        while self.watchdog_running:
            try:
                # 마지막 프레임 시간 확인
                if self.last_frame_time:
                    time_since_last_frame = (datetime.now() - self.last_frame_time).total_seconds()
                    
                    if time_since_last_frame > self.frame_timeout:
                        logger.warning(f"{self.config['name']}: {time_since_last_frame:.1f}초 동안 프레임 수신 없음")
                        self.request_restart("프레임 타임아웃")
                        
                        # 재시작 후 대기
                        time.sleep(self.frame_timeout)
                        
                # 1초마다 체크
                time.sleep(1.0)
                
            except Exception as e:
                logger.error(f"{self.config['name']}: 워치독 에러 - {e}")
                
        logger.info(f"{self.config['name']}: 워치독 스레드 종료")
        
    def request_restart(self, reason):
        """재시작 요청"""
        current_time = datetime.now()
        
        # 재시작 쿨다운 체크
        if self.last_restart_time:
            time_since_last_restart = (current_time - self.last_restart_time).total_seconds()
            if time_since_last_restart < self.restart_cooldown:
                logger.warning(f"{self.config['name']}: 재시작 쿨다운 중 ({time_since_last_restart:.1f}초/{self.restart_cooldown}초)")
                return
                
        # 최대 재시작 횟수 체크
        if self.restart_count >= self.max_restart_attempts:
            logger.error(f"{self.config['name']}: 최대 재시작 횟수 초과 ({self.restart_count}/{self.max_restart_attempts})")
            # 전체 프로세스 재시작 요청
            if self.restart_callback:
                self.restart_callback()
            return
            
        logger.warning(f"{self.config['name']}: 재시작 요청 - {reason} (시도 {self.restart_count + 1}/{self.max_restart_attempts})")
        
        # 재시작 카운트 증가
        self.restart_count += 1
        self.last_restart_time = current_time
        
        # 파이프라인 재시작
        self.restart_pipeline()
        
    def restart_pipeline(self):
        """파이프라인 재시작"""
        logger.info(f"{self.config['name']}: 파이프라인 재시작 시도")
        
        try:
            # 기존 파이프라인 정지
            if self.pipeline:
                # splitmuxsink에 EOS 보내서 현재 파일 마무리
                self.pipeline.send_event(Gst.Event.new_eos())
                self.pipeline.get_state(2 * Gst.SECOND)  # EOS 대기
                
                self.pipeline.set_state(Gst.State.NULL)
                time.sleep(2)
                
            # 새 파이프라인 생성
            self.create_pipeline()
            
            # 파이프라인 시작
            ret = self.pipeline.set_state(Gst.State.PLAYING)
            if ret == Gst.StateChangeReturn.FAILURE:
                logger.error(f"{self.config['name']}: 파이프라인 재시작 실패")
                return False
                
            # 상태 변경 대기
            ret, state, pending = self.pipeline.get_state(5 * Gst.SECOND)
            if ret != Gst.StateChangeReturn.SUCCESS:
                logger.error(f"{self.config['name']}: 파이프라인 상태 변경 실패")
                return False
                
            # 프레임 수신 시간 초기화
            self.last_frame_time = datetime.now()
            
            logger.info(f"{self.config['name']}: 파이프라인 재시작 성공")
            return True
            
        except Exception as e:
            logger.error(f"{self.config['name']}: 파이프라인 재시작 중 에러 - {e}")
            return False
            
    def start(self):
        """녹화 시작"""
        logger.info(f"{self.config['name']} 녹화 시작")
        self.create_pipeline()
        
        # 파이프라인 상태 변경 및 확인
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        if ret == Gst.StateChangeReturn.FAILURE:
            logger.error(f"{self.config['name']}: 파이프라인 시작 실패")
            return False
            
        # 상태 변경 대기
        ret, state, pending = self.pipeline.get_state(5 * Gst.SECOND)
        if ret != Gst.StateChangeReturn.SUCCESS:
            logger.error(f"{self.config['name']}: 파이프라인 상태 변경 실패")
            return False
            
        # 프레임 수신 시간 초기화
        self.last_frame_time = datetime.now()
        
        # 워치독 스레드 시작
        self.watchdog_running = True
        self.watchdog_thread = threading.Thread(target=self.watchdog_thread_func)
        self.watchdog_thread.daemon = True
        self.watchdog_thread.start()
        
        logger.info(f"{self.config['name']}: 파이프라인 시작 성공")
        return True
        
    def stop(self):
        """녹화 중지"""
        logger.info(f"{self.config['name']} 녹화 중지")
        
        # 워치독 스레드 중지
        self.watchdog_running = False
        if self.watchdog_thread:
            self.watchdog_thread.join(timeout=5)
            
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)

class CameraProcess:
    """카메라별 독립 프로세스"""
    
    def __init__(self, camera_config, output_dir, test_file=None):
        self.camera_config = camera_config
        self.output_dir = output_dir
        self.recorder = None
        self.main_loop = None
        self.test_file = test_file
        self.restart_requested = False
        
    def run(self):
        """프로세스 실행"""
        logger.info(f"카메라 프로세스 시작: {self.camera_config['name']}")
        
        # 시그널 핸들러
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        
        # 레코더 생성 및 시작
        self.recorder = CameraRecorder(self.camera_config, self.output_dir, test_file=self.test_file)
        self.recorder.set_restart_callback(self.request_process_restart)
        
        if not self.recorder.start():
            logger.error(f"{self.camera_config['name']}: 레코더 시작 실패")
            return
        
        # GLib 메인 루프 실행
        self.main_loop = GLib.MainLoop()
        
        try:
            self.main_loop.run()
        except Exception as e:
            logger.error(f"{self.camera_config['name']}: 메인 루프 에러 - {e}")
        finally:
            self.cleanup()
            
        # 재시작 요청이 있으면 프로세스 종료 코드 반환
        if self.restart_requested:
            sys.exit(100)  # 특별한 종료 코드로 재시작 요청 표시
        
    def request_process_restart(self):
        """프로세스 재시작 요청"""
        logger.warning(f"{self.camera_config['name']}: 전체 프로세스 재시작 요청")
        self.restart_requested = True
        if self.main_loop and self.main_loop.is_running():
            self.main_loop.quit()
        
    def signal_handler(self, sig, frame):
        """시그널 핸들러"""
        logger.info(f"시그널 수신: {sig}")
        self.cleanup()
        
    def cleanup(self):
        """정리 작업"""
        if self.recorder:
            self.recorder.stop()
        if self.main_loop and self.main_loop.is_running():
            self.main_loop.quit()


class DualCameraSystem:
    """듀얼 카메라 시스템 메인 클래스"""
    
    def __init__(self, config_file='camera_config.json', test_mode=False, test_files=None):
        self.config = self.load_config(config_file)
        self.processes = {}  # 딕셔너리로 변경하여 카메라별 프로세스 관리
        self.base_dir = Path(self.config['output_base_dir'])
        self.test_mode = test_mode
        self.test_files = test_files or {}
        self.running = False
        self.monitor_thread = None

        if self.test_mode:
            logger.info("테스트 모드로 실행 중...")

    def load_config(self, config_file):
        """설정 파일 로드 또는 기본값 사용"""
        default_config = {
            'output_base_dir': '/home/nvidia/data',
            'cameras': [
                {
                    'name': 'RGB_Camera',
                    'type': 'usb',
                    'device_id': 0,
                    'width': 1920,
                    'height': 1080,
                    'fps': 10,
                    'bitrate': 2000000,
                    'udp_host': '127.0.0.1',
                    'udp_port': 8877
                },
                {
                    'name': 'Thermal_Camera',
                    'type': 'thermal',
                    'device_id': 2,  # /dev/video2
                    'width': 384,
                    'height': 290,
                    'fps': 10,
                    'bitrate': 2000000,
                    'udp_host': '127.0.0.1',
                    'udp_port': 8878
                }
            ]
        }
        
        config_path = Path(config_file)
        if config_path.exists():
            with open(config_path, 'r') as f:
                return json.load(f)
        else:
            # 기본 설정 파일 생성
            with open(config_path, 'w') as f:
                json.dump(default_config, f, indent=2)
            logger.info(f"기본 설정 파일 생성: {config_file}")
            return default_config
            
    def verify_cameras(self):
        """카메라 존재 확인"""
        logger.info("카메라 확인 중...")
        
        for camera in self.config['cameras']:
            device_path = f"/dev/video{camera['device_id']}"
            if Path(device_path).exists():
                logger.info(f"✓ {camera['name']}: {device_path} 존재")
                
                # v4l2-ctl로 상세 정보 확인
                try:
                    result = subprocess.run(
                        ['v4l2-ctl', f'--device={device_path}', '--get-fmt-video'],
                        capture_output=True, text=True
                    )
                    logger.debug(f"{camera['name']} 현재 설정:\n{result.stdout}")
                except Exception as e:
                    logger.warning(f"{camera['name']} 정보 확인 실패: {e}")
            else:
                logger.error(f"✗ {camera['name']}: {device_path} 없음")
                return False
                
        return True
            
    def monitor_processes(self):
        """프로세스 상태 모니터링 및 재시작"""
        logger.info("프로세스 모니터 스레드 시작")
        
        while self.running:
            try:
                for camera_name, process_info in list(self.processes.items()):
                    process = process_info['process']
                    camera_config = process_info['config']
                    test_file = process_info.get('test_file')
                    
                    if not process.is_alive():
                        exit_code = process.exitcode
                        
                        if exit_code == 100:  # 재시작 요청
                            logger.warning(f"{camera_name}: 프로세스 재시작 요청 감지")
                            
                            # 새 프로세스 시작
                            new_process = mp.Process(
                                target=self._run_camera_process,
                                args=(camera_config, self.base_dir, test_file)
                            )
                            new_process.start()
                            
                            # 프로세스 정보 업데이트
                            self.processes[camera_name] = {
                                'process': new_process,
                                'config': camera_config,
                                'test_file': test_file
                            }
                            
                            logger.info(f"{camera_name}: 프로세스 재시작 완료")
                            
                        elif exit_code is not None:
                            logger.error(f"{camera_name}: 프로세스가 예기치 않게 종료됨 (exit code: {exit_code})")
                            
                time.sleep(5)  # 5초마다 체크
                
            except Exception as e:
                logger.error(f"프로세스 모니터링 에러: {e}")
                
        logger.info("프로세스 모니터 스레드 종료")
            
    def start(self):
        """시스템 시작"""
        logger.info("듀얼 카메라 시스템 시작")
        
        # 카메라 확인
        if not self.test_mode and not self.verify_cameras():
            logger.error("카메라 확인 실패")
            return False
        
        self.running = True
        
        for camera_config in self.config['cameras']:
            # 카메라별 출력 디렉토리
            output_dir = self.base_dir
            
            test_file = None
            if self.test_mode:
                # 카메라별 테스트 파일이 지정되었으면 사용
                if camera_config['name'] in self.test_files:
                    test_file = self.test_files[camera_config['name']]
                # 기본 테스트 파일이 있으면 사용
                elif 'default' in self.test_files:
                    test_file = self.test_files['default']
                else:
                    logger.warning(f"{camera_config['name']}에 대한 테스트 파일이 지정되지 않았습니다.")
                    continue

            # 프로세스 생성
            process = mp.Process(
                target=self._run_camera_process,
                args=(camera_config, output_dir, test_file)
            )
            process.start()
            
            # 프로세스 정보 저장
            self.processes[camera_config['name']] = {
                'process': process,
                'config': camera_config,
                'test_file': test_file
            }
            
            # 프로세스 시작 간격
            time.sleep(1)
            
        # 프로세스 모니터링 스레드 시작
        self.monitor_thread = threading.Thread(target=self.monitor_processes)
        self.monitor_thread.daemon = True
        self.monitor_thread.start()
        
        logger.info(f"{len(self.processes)}개 카메라 프로세스 시작 완료")
        return True
        
    def _run_camera_process(self, camera_config, output_dir, test_file=None):
        """카메라 프로세스 실행"""
        camera_proc = CameraProcess(camera_config, output_dir, test_file)
        camera_proc.run()
        
    def stop(self):
        """시스템 종료"""
        logger.info("듀얼 카메라 시스템 종료 시작")
        
        self.running = False
        
        # 모니터 스레드 종료 대기
        if self.monitor_thread:
            self.monitor_thread.join(timeout=10)
        
        # 모든 프로세스에 종료 시그널
        for camera_name, process_info in self.processes.items():
            process = process_info['process']
            if process.is_alive():
                process.terminate()
                
        # 종료 대기
        for camera_name, process_info in self.processes.items():
            process = process_info['process']
            process.join(timeout=5)
            if process.is_alive():
                logger.warning(f"프로세스 강제 종료: {process.pid}")
                process.kill()
                
        logger.info("듀얼 카메라 시스템 종료 완료")
        
    def wait(self):
        """프로세스 대기"""
        try:
            for camera_name, process_info in self.processes.items():
                process = process_info['process']
                process.join()
        except KeyboardInterrupt:
            logger.info("키보드 인터럽트 감지")
            

def test_camera_capture():
    """카메라 캡처 테스트"""
    logger.info("카메라 캡처 테스트 시작")
    
    # RGB 카메라 테스트
    logger.info("RGB 카메라 테스트...")
    cmd = [
        'gst-launch-1.0', '-v',
        'v4l2src', 'device=/dev/video0', 'num-buffers=10', '!',
        'video/x-raw,format=YUY2,width=1920,height=1080,framerate=30/1', '!',
        'videoconvert', '!',
        'fakesink'
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        logger.info("✓ RGB 카메라 테스트 성공")
    else:
        logger.error(f"✗ RGB 카메라 테스트 실패:\n{result.stderr}")
        
    # Thermal 카메라 테스트
    logger.info("Thermal 카메라 테스트...")
    cmd = [
        'gst-launch-1.0', '-v',
        'v4l2src', 'device=/dev/video2', 'num-buffers=10', '!',
        'video/x-raw,format=YUY2,width=384,height=290,framerate=50/1', '!',
        'videoconvert', '!',
        'fakesink'
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        logger.info("✓ Thermal 카메라 테스트 성공")
    else:
        logger.error(f"✗ Thermal 카메라 테스트 실패:\n{result.stderr}")

def parse_arguments():
    """명령줄 인자 파싱"""
    parser = argparse.ArgumentParser(description='Jetson Xavier NX 듀얼 카메라 시스템')
    parser.add_argument('-c', '--config', default='camera_config.json',
                        help='설정 파일 경로 (기본값: camera_config.json)')
    parser.add_argument('-t', '--test', action='store_true',
                        help='테스트 모드 (MP4 파일 사용)')
    parser.add_argument('-f', '--file', type=str,
                        help='테스트용 MP4 파일 경로 (모든 카메라에 동일 파일 사용)')
    parser.add_argument('--rgb-file', type=str,
                        help='RGB 카메라용 테스트 MP4 파일')
    parser.add_argument('--thermal-file', type=str,
                        help='Thermal 카메라용 테스트 MP4 파일')
    parser.add_argument('--camera-test', action='store_true',
                        help='카메라 캡처 테스트만 실행')
    return parser.parse_args()

def main():
    """메인 함수"""
    # 명령행 인자 처리
    args = parse_arguments()
    
    # 카메라 테스트 모드
    if args.camera_test:
        test_camera_capture()
        return

    test_files = {}
    if args.test:
        if args.file:
            test_files['default'] = args.file
        if args.rgb_file:
            test_files['RGB_Camera'] = args.rgb_file
        if args.thermal_file:
            test_files['Thermal_Camera'] = args.thermal_file
            
        if not test_files:
            logger.error("테스트 모드에서는 최소 하나의 MP4 파일을 지정해야 합니다.")
            logger.error("사용법: python camera.py --test --file test.mp4")
            return
            
    # 시스템 생성 및 실행
    system = DualCameraSystem(config_file=args.config, 
                                test_mode=args.test, 
                                test_files=test_files)
    
    try:
        if system.start():
            system.wait()
        else:
            logger.error("시스템 시작 실패")
    except KeyboardInterrupt:
        logger.info("프로그램 종료 요청")
    finally:
        system.stop()
        

if __name__ == "__main__":
    main()