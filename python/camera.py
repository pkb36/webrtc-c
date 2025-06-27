#!/usr/bin/env python3
"""
Jetson Xavier NX 듀얼 카메라 시스템
- RGB/Thermal 카메라 캡처
- H.264 녹화 with 타임스탬프 인덱싱
- UDP 스트리밍 바이패스
- YUYV 포맷 지원
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


class FrameIndexer:
    """프레임 타임스탬프 인덱서"""
    
    def __init__(self, index_file_path):
        self.index_file_path = Path(index_file_path)
        self.file_handle = None
        
    def open(self):
        """인덱스 파일 열기"""
        self.file_handle = open(self.index_file_path, 'wb')
        logger.info(f"인덱스 파일 생성: {self.index_file_path}")
        
    def write_index(self, frame_number, timestamp, byte_offset, frame_size, is_keyframe):
        """인덱스 정보 쓰기"""
        if not self.file_handle:
            return
            
        epoch_time = timestamp.timestamp()
        # struct: frame_number(I), timestamp(d), byte_offset(Q), frame_size(I), is_keyframe(B)
        data = struct.pack('=IdQIB', frame_number, epoch_time, byte_offset, frame_size, is_keyframe)
        self.file_handle.write(data)
        self.file_handle.flush()  # 즉시 디스크에 쓰기
        
    def close(self):
        """파일 닫기"""
        if self.file_handle:
            self.file_handle.close()
            logger.info(f"인덱스 파일 닫기: {self.index_file_path}")


class CameraRecorder:
    """카메라별 녹화 및 스트리밍 클래스"""
    
    def __init__(self, camera_config, output_dir, test_file=None):
        self.config = camera_config
        self.base_output_dir = Path(output_dir)
        self.test_file = test_file

        current_date = datetime.now().strftime("RECORD_%Y%m%d")
        self.output_dir = self.base_output_dir / current_date / self.config['name']
        self.output_dir.mkdir(parents=True, exist_ok=True)

        # 녹화 관련
        self.video_file = None
        self.indexer = None
        self.frame_count = 0
        self.byte_offset = 0
        self.segment_start_time = None
        
        # GStreamer 파이프라인
        self.pipeline = None
        self.loop = None
        
    def create_pipeline(self):
        """GStreamer 파이프라인 생성"""
        if self.test_file:
            camera_source = self._get_file_source()  # 테스트 파일 사용
        else:
            camera_source = self._get_camera_source()  # 기존 카메라 사용

        encoder_settings = self._get_encoder_settings()
        
        pipeline_str = f"""
            {camera_source} !
            tee name=t
            
            t. ! queue max-size-buffers=100 max-size-bytes=0 max-size-time=0 ! 
            videoconvert ! 
            video/x-raw, format=I420 !
            nvvidconv ! 
            video/x-raw(memory:NVMM), format=I420 ! 
            {encoder_settings} !
            h264parse !
            appsink name=filesink emit-signals=true sync=false max-buffers=100 drop=true
            
            t. ! queue max-size-buffers=100 max-size-bytes=0 max-size-time=0 ! 
            videoconvert ! 
            video/x-raw, format=I420 !
            nvvidconv ! 
            video/x-raw(memory:NVMM), format=I420 ! 
            {encoder_settings} !
            h264parse !
            rtph264pay config-interval=1 pt=96 !
            udpsink host={self.config['udp_host']} port={self.config['udp_port']} sync=false
        """
        
        logger.info(f"파이프라인 생성: {self.config['name']}")
        logger.debug(f"파이프라인: {pipeline_str}")
        
        self.pipeline = Gst.parse_launch(pipeline_str)
        
        # appsink 설정
        filesink = self.pipeline.get_by_name('filesink')
        filesink.set_property('emit-signals', True)
        filesink.set_property('max-buffers', 100)
        filesink.set_property('drop', True)
        filesink.connect('new-sample', self.on_new_sample)
        
        # 버스 메시지 핸들러
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect('message', self.on_bus_message)
        
    def _get_camera_source(self):
        """카메라 소스 GStreamer 엘리먼트"""
        if(self.config['device_id'] == 2):
            # Thermal 카메라의 경우 v4l2src를 사용하여 YUYV 포맷으로 캡처
            return (
                f"v4l2src device=/dev/video{self.config['device_id']} ! "
                f"video/x-raw, format=YUY2, width=384, height=290, framerate={self.config['fps']}/1 ! "
                f"videocrop top=1 bottom=1 ! "
                f"videorate ! video/x-raw,width=384,height=288,framerate=10/1"
            )
        else:
            #YUYV 포맷을 명시적으로 지정
            return (
                f"v4l2src device=/dev/video{self.config['device_id']} ! "
                f"video/x-raw, format=YUY2, width={self.config['width']}, "
                f"height={self.config['height']}, framerate={self.config['fps']}/1 ! "
                f"videorate ! video/x-raw,width={self.config['width']},height={self.config['height']},framerate=10/1"
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
        bitrate = self.config.get('bitrate', 8000000)
        
        # H.264 인코더 설정 (키프레임 간격 30 = 1초)
        return (
            f"nvv4l2h264enc bitrate={bitrate} preset-level=2 "
            f"idrinterval=5 insert-sps-pps=true "
            f"profile=2 maxperf-enable=true"  # High profile, 최대 성능
        )
        
    def start_new_segment(self):
        """새 세그먼트 시작"""
        # 이전 세그먼트 닫기
        self.close_current_segment()
        
        # 타임스탬프 생성
        timestamp = datetime.now()

        current_date = timestamp.strftime("RECORD_%Y%m%d") 
        date_output_dir = self.base_output_dir / current_date / self.config['name']
        date_output_dir.mkdir(parents=True, exist_ok=True)

        base_name = timestamp.strftime(f"{self.config['name']}_%Y%m%d_%H%M%S")
        
        # 파일 경로
        video_path = date_output_dir / f"{base_name}.h264"
        index_path = date_output_dir / f"{base_name}.idx"
        
        # 비디오 파일 열기
        self.video_file = open(video_path, 'wb')
        
        # 인덱서 생성
        self.indexer = FrameIndexer(index_path)
        self.indexer.open()
        
        # 메타데이터 저장
        metadata = {
            'camera_name': self.config['name'],
            'camera_type': self.config['type'],
            'device_id': self.config['device_id'],
            'width': self.config['width'],
            'height': self.config['height'],
            'fps': self.config['fps'],
            'bitrate': self.config.get('bitrate', 8000000),
            'start_time': timestamp.isoformat(),
            'video_file': video_path.name,
            'index_file': index_path.name
        }
        
        metadata_path = date_output_dir / f"{base_name}.json"
        with open(metadata_path, 'w') as f:
            json.dump(metadata, f, indent=2)
            
        # 상태 초기화
        self.segment_start_time = timestamp
        self.frame_count = 0
        self.byte_offset = 0
        
        logger.info(f"새 세그먼트 시작: {base_name} in {date_output_dir}")
        
    def close_current_segment(self):
        """현재 세그먼트 닫기"""
        if self.video_file:
            self.video_file.close()
            self.video_file = None
            
        if self.indexer:
            self.indexer.close()
            self.indexer = None
            
        if self.frame_count > 0:
            logger.info(f"세그먼트 종료: {self.frame_count} 프레임 저장됨")
            
    def on_new_sample(self, appsink):
        """새 샘플(인코딩된 프레임) 처리"""
        sample = appsink.emit('pull-sample')
        if not sample:
            return Gst.FlowReturn.OK
            
        # 버퍼 가져오기
        buffer = sample.get_buffer()
        
        # 타임스탬프
        timestamp = datetime.now()
        
        # 세그먼트 확인 (5분마다 새 파일)
        if not self.video_file or (timestamp - self.segment_start_time).seconds >= 300:
            self.start_new_segment()
            
        # H.264 데이터 추출
        success, map_info = buffer.map(Gst.MapFlags.READ)
        if success:
            # 비디오 파일에 쓰기
            frame_data = map_info.data
            frame_size = len(frame_data)
            self.video_file.write(frame_data)
            
            # print(f"{self.config['name']}: 프레임 {self.frame_count} 저장, 크기: {frame_size} 바이트")

            # 키프레임 확인 (간단한 방법: NAL unit type 확인)
            is_keyframe = self._is_keyframe(frame_data)
            if is_keyframe:
                print(f"{self.config['name']}: 키프레임 감지 (프레임 {self.frame_count})")
            else:
                logger.debug(f"{self.config['name']}: 일반 프레임 (프레임 {self.frame_count})")
            
            # 인덱스 저장
            self.indexer.write_index(
                self.frame_count,
                timestamp,
                self.byte_offset,
                frame_size,
                is_keyframe
            )
            
            # 상태 업데이트
            self.frame_count += 1
            self.byte_offset += frame_size
            
            # 주기적인 상태 로그 (1000프레임마다)
            if self.frame_count % 1000 == 0:
                logger.info(f"{self.config['name']}: {self.frame_count} 프레임 녹화됨")
            
            buffer.unmap(map_info)
            
        return Gst.FlowReturn.OK
        
    def _is_keyframe(self, data):
        """키프레임 여부 확인 (디버그 버전)"""
        if len(data) > 4:
            # 처음 몇 개의 NAL unit type 출력
            nal_types = []
            i = 0
            while i < min(len(data) - 4, 100):  # 처음 100바이트만 확인
                if data[i:i+3] == b'\x00\x00\x01':
                    nal_type = data[i+3] & 0x1F
                    nal_types.append(nal_type)
                    if nal_type == 5:
                        return True
                    i += 3
                elif data[i:i+4] == b'\x00\x00\x00\x01':
                    nal_type = data[i+4] & 0x1F
                    nal_types.append(nal_type)
                    if nal_type == 5:
                        return True
                    i += 4
                else:
                    i += 1
            
            # 디버그: 매 30번째 프레임마다 NAL types 출력
            if self.frame_count % 10 == 0:
                logger.debug(f"Frame {self.frame_count} NAL types: {nal_types}")
        
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
            self.restart_pipeline()
        elif t == Gst.MessageType.WARNING:
            warn, debug = message.parse_warning()
            logger.warning(f"{self.config['name']}: 경고 - {warn}, {debug}")
            
    def restart_pipeline(self):
        """파이프라인 재시작"""
        logger.info(f"{self.config['name']}: 파이프라인 재시작 시도")
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
            time.sleep(1)
            self.pipeline.set_state(Gst.State.PLAYING)
            
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
            
        logger.info(f"{self.config['name']}: 파이프라인 시작 성공")
        return True
        
    def stop(self):
        """녹화 중지"""
        logger.info(f"{self.config['name']} 녹화 중지")
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
        self.close_current_segment()


class CameraProcess:
    """카메라별 독립 프로세스"""
    
    def __init__(self, camera_config, output_dir, test_file=None):
        self.camera_config = camera_config
        self.output_dir = output_dir
        self.recorder = None
        self.main_loop = None
        self.test_file = test_file
        
    def run(self):
        """프로세스 실행"""
        logger.info(f"카메라 프로세스 시작: {self.camera_config['name']}")
        
        # 시그널 핸들러
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        
        # 레코더 생성 및 시작
        self.recorder = CameraRecorder(self.camera_config, self.output_dir, test_file=self.test_file)
        
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
        self.processes = []
        self.base_dir = Path(self.config['output_base_dir'])
        self.test_mode = test_mode
        self.test_files = test_files or {}

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
                    'fps': 30,
                    'bitrate': 8000000,
                    'udp_host': '127.0.0.1',
                    'udp_port': 8877
                },
                {
                    'name': 'Thermal_Camera',
                    'type': 'thermal',
                    'device_id': 2,  # /dev/video2
                    'width': 384,
                    'height': 290,
                    'fps': 50,
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
            
    def start(self):
        """시스템 시작"""
        logger.info("듀얼 카메라 시스템 시작")
        
        # 카메라 확인
        if not self.verify_cameras():
            logger.error("카메라 확인 실패")
            return False
        
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
            self.processes.append(process)
            
            # 프로세스 시작 간격
            time.sleep(1)
            
        logger.info(f"{len(self.processes)}개 카메라 프로세스 시작 완료")
        return True
        
    def _run_camera_process(self, camera_config, output_dir, test_file=None):
        """카메라 프로세스 실행"""
        camera_proc = CameraProcess(camera_config, output_dir, test_file)
        camera_proc.run()
        
    def stop(self):
        """시스템 종료"""
        logger.info("듀얼 카메라 시스템 종료 시작")
        
        # 모든 프로세스에 종료 시그널
        for process in self.processes:
            if process.is_alive():
                process.terminate()
                
        # 종료 대기
        for process in self.processes:
            process.join(timeout=5)
            if process.is_alive():
                logger.warning(f"프로세스 강제 종료: {process.pid}")
                process.kill()
                
        logger.info("듀얼 카메라 시스템 종료 완료")
        
    def wait(self):
        """프로세스 대기"""
        try:
            for process in self.processes:
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
    return parser.parse_args()

def main():
    """메인 함수"""
    # 명령행 인자 처리
    args = parse_arguments()

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

