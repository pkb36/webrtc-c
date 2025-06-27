#!/usr/bin/env python3
"""
이벤트 트리거 기반 MP4 저장 시스템
- camera.py에서 생성된 5분 단위 H.264 파일과 인덱스 파일 활용
- 이벤트 전후 15초 (총 30초) MP4 저장
- 5분 경계를 넘는 구간 처리
- 객체 검출 좌표 데이터 저장
- 현재 녹화 중인 파일도 읽기 가능
- TCP 서버를 통한 외부 이벤트 수신
- 날짜별 서브폴더에 이벤트 저장
"""

import os
import sys
import json
import struct
import subprocess
import logging
import requests
from pathlib import Path
from datetime import datetime, timedelta
import multiprocessing as mp
from multiprocessing import Process, Queue, Event
import time
import threading
from collections import deque
import random
import signal
import shutil
import heapq
import socket
from typing import Dict, Optional
from ip_address_manager import IPAddressManager

# 로깅 설정
import os
from datetime import datetime

# 로그 디렉토리 생성
log_dir = Path('logs')
log_dir.mkdir(exist_ok=True)

# 로그 파일명 (날짜별)
log_filename = log_dir / f'event_recorder_{datetime.now().strftime("%Y%m%d")}.log'

# 로깅 설정 - 파일과 콘솔 둘 다 출력
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(log_filename, encoding='utf-8'),  # 파일 핸들러
        logging.StreamHandler()  # 콘솔 핸들러
    ]
)
logger = logging.getLogger(__name__)

class NotificationSender:
    """서버로 이벤트 알림 전송"""
    
    def __init__(self, server_ip: str, port: int = 0, phone: str = "", password: str = ""):
        self.server_ip = server_ip
        self.port = port
        self.phone = phone
        self.password = password
        self.token = None
        self.base_url = f"http://{server_ip}"
        # if port > 0:
        #     self.base_url += f":{port}"
            
    def login(self) -> bool:
        """로그인하여 토큰 획득"""
        try:
            url = f"{self.base_url}/api/login/"
            data = {
                "phone": self.phone,
                "password": self.password,
                "fcmToken": ""
            }
            
            logger.info(f"로그인 시도: {self.phone} @ {self.base_url}")
            logger.info(f"로그인 데이터: {data}")
            # POST 요청으로 로그인
            response = requests.post(url, json=data, timeout=10)
            if response.status_code == 200:
                result = response.json()
                self.token = result.get('token')
                logger.info(f"로그인 성공, 토큰: {self.token}")
                return True
            else:
                logger.error(f"로그인 실패: {response.status_code}")
                return False
                
        except Exception as e:
            logger.error(f"로그인 오류: {e}")
            return False
            
    def send_notification(self, camera_id: str, event_type: str, 
                         snapshot_path: str, video_url: str) -> bool:
        """이벤트 알림 전송"""
        # 토큰이 없으면 로그인
        if not self.token:
            if not self.login():
                return False
                
        try:
            url = f"{self.base_url}/api/notification/create/"
            
            # multipart/form-data로 전송
            files = {}
            data = {
                'camera': camera_id,
                'notificationCategory': event_type,
                'video_url': video_url
            }
            
            # 스냅샷 파일이 있으면 추가
            if os.path.exists(snapshot_path):
                files['image'] = open(snapshot_path, 'rb')
                
            headers = {
                'Authorization': f'Token {self.token}'
            }
            
            logger.info(f"알림 전송 시도 - camera: {camera_id}, category: {event_type}, url: {video_url}")

            response = requests.post(url, data=data, files=files, 
                                   headers=headers, timeout=100)
            
            # 파일 닫기
            for f in files.values():
                f.close()
                
            if response.status_code in [200, 201, 202]:
                logger.info(f"알림 전송 성공: {camera_id}")
                return True
            elif response.status_code == 401:
                # 토큰 만료, 재로그인
                logger.info("토큰 만료, 재로그인 시도")
                self.token = None
                return self.send_notification(camera_id, event_type, 
                                            snapshot_path, video_url)
            else:
                logger.error(f"알림 전송 실패: {response.status_code}")
                # if response.text:
                #     logger.error(f"응답 내용: {response.text}")
                return False
                
        except Exception as e:
            logger.error(f"알림 전송 오류: {e}")
            return False


class OSDCompletionMonitor:
    """OSD 처리 완료 모니터링"""
    
    def __init__(self, base_dir: str, notification_sender: NotificationSender,
                 http_service_ip: str):
        self.base_dir = Path(base_dir)
        self.notification_sender = notification_sender
        self.http_service_ip = http_service_ip
        self.processed_signals = set()
        self.running = False
        self.monitor_thread = None
        
    def start(self):
        """모니터링 시작"""
        self.running = True
        self.monitor_thread = threading.Thread(target=self._monitor_loop)
        self.monitor_thread.daemon = True
        self.monitor_thread.start()
        logger.info("OSD 완료 모니터링 시작")
        
    def stop(self):
        """모니터링 중지"""
        self.running = False
        if self.monitor_thread:
            self.monitor_thread.join()
        logger.info("OSD 완료 모니터링 중지")
        
    def _monitor_loop(self):
        """모니터링 루프"""
        while self.running:
            try:
                # EVENT_YYYYMMDD 디렉토리 검색
                for event_dir in self.base_dir.glob("EVENT_*"):
                    if not event_dir.is_dir():
                        continue
                        
                    # .osd_complete 파일 검색
                    for signal_file in event_dir.glob("*.osd_complete"):
                        if str(signal_file) in self.processed_signals:
                            continue
                            
                        # 파일이 완전히 쓰여졌는지 확인
                        if self._is_file_ready(signal_file):
                            self._process_completion_signal(signal_file)
                            
            except Exception as e:
                logger.error(f"모니터링 오류: {e}")
                
            time.sleep(2)  # 2초마다 확인
            
    def _is_file_ready(self, filepath: Path) -> bool:
        """파일이 완전히 쓰여졌는지 확인"""
        try:
            size1 = filepath.stat().st_size
            time.sleep(0.1)
            size2 = filepath.stat().st_size
            return size1 == size2 and size1 > 0
        except:
            return False
            
    def _process_completion_signal(self, signal_file: Path):
        """완료 신호 처리"""
        try:
            # 신호 파일 읽기
            with open(signal_file, 'r') as f:
                signal_data = json.load(f)
                
            video_path = signal_data['video_path']
            camera_id = signal_data['camera_id']
            event_type = signal_data.get('event_type', '1')
            snapshot_path = signal_data['snapshot_path']
            
            # HTTP URL 생성
            # 예: /home/nvidia/data/EVENT_20241211/event_RGB_Camera_20241211_123456.mp4
            # -> http://192.168.1.100/data/EVENT_20241211/event_RGB_Camera_20241211_123456.mp4
            relative_path = Path(video_path).relative_to(self.base_dir)
            video_url = f"http://{self.http_service_ip}/data/{relative_path}"
            
            logger.info(f"OSD 완료 감지: {video_path}")
            logger.info(f"비디오 URL: {video_url}")
            
            # 알림 전송
            if self.notification_sender.send_notification(
                camera_id, event_type, snapshot_path, video_url
            ):
                logger.info(f"알림 전송 완료: {camera_id}")
                
                # 처리 완료 표시
                self.processed_signals.add(str(signal_file))
                
                # 신호 파일 삭제
                signal_file.unlink()
                
                # 스냅샷 파일도 삭제 (선택사항)
                # if Path(snapshot_path).exists():
                # Path(snapshot_path).unlink()
            else:
                logger.error("알림 전송 실패")
                
        except Exception as e:
            logger.error(f"완료 신호 처리 오류: {e}")

class VideoSegmentManager:
    """5분 단위 비디오 세그먼트 관리"""
    
    def __init__(self, base_dir):
        self.base_dir = Path(base_dir)
        self.segment_duration = 300  # 5분
        
    def find_segments_for_timerange(self, camera_name, start_time, end_time):
        """특정 시간 범위에 해당하는 비디오 세그먼트 찾기"""

        current_date = datetime.now().strftime("RECORD_%Y%m%d")
        camera_dir = self.base_dir / current_date / camera_name


        if not camera_dir.exists():
            logger.error(f"카메라 디렉토리 없음: {camera_dir}")
            return []
            
        segments = []
        current_time = datetime.now()
        
        logger.debug(f"세그먼트 검색: {camera_name}, {start_time} ~ {end_time}")
        
        # 디렉토리의 모든 .json 파일 확인
        json_files = list(camera_dir.glob("*.json"))
        logger.debug(f"발견된 JSON 파일 수: {len(json_files)}")
        
        for json_file in sorted(json_files, reverse=True):  # 최신 파일부터
            try:
                with open(json_file, 'r') as f:
                    metadata = json.load(f)
                    
                segment_start = datetime.fromisoformat(metadata['start_time'])
                
                # 현재 녹화 중인 세그먼트는 현재 시간까지
                if segment_start + timedelta(seconds=self.segment_duration) > current_time:
                    segment_end = current_time
                    is_current = True
                else:
                    segment_end = segment_start + timedelta(seconds=self.segment_duration)
                    is_current = False
                
                logger.debug(f"세그먼트 확인: {json_file.name}, {segment_start} ~ {segment_end}, current={is_current}")
                
                # 시간 범위가 겹치는지 확인
                if not (end_time < segment_start or start_time > segment_end):
                    # 파일 존재 확인
                    video_file = camera_dir / metadata['video_file']
                    index_file = camera_dir / metadata['index_file']
                    
                    if not video_file.exists():
                        logger.warning(f"비디오 파일 없음: {video_file}")
                        continue
                    if not index_file.exists():
                        logger.warning(f"인덱스 파일 없음: {index_file}")
                        continue
                        
                    segments.append({
                        'metadata': metadata,
                        'video_file': video_file,
                        'index_file': index_file,
                        'start_time': segment_start,
                        'end_time': segment_end,
                        'is_current': is_current
                    })
                    logger.info(f"세그먼트 추가: {json_file.name}")
                    
            except Exception as e:
                logger.error(f"메타데이터 읽기 실패 {json_file}: {e}")
                
        logger.info(f"찾은 세그먼트 수: {len(segments)}")
        return sorted(segments, key=lambda x: x['start_time'])
        
    def read_index_file(self, index_file_path, is_current=False):
        """인덱스 파일 읽기 (현재 녹화 중인 파일 지원)"""
        indices = []
        
        try:
            # 파일 크기 확인
            file_size = index_file_path.stat().st_size
            if file_size == 0:
                return indices
                
            # struct 크기 계산
            struct_format = '=IdQIB'  # = 표준 크기, 패딩 없음 (camera.py와 일치)
            struct_size = struct.calcsize(struct_format)
            logger.debug(f"Struct size: {struct_size} bytes")
            
            # 현재 녹화 중인 파일은 복사본을 만들어 읽기
            if is_current:
                temp_file = index_file_path.with_suffix('.tmp')
                shutil.copy2(index_file_path, temp_file)
                read_file = temp_file
            else:
                read_file = index_file_path
                
            with open(read_file, 'rb') as f:
                while True:
                    data = f.read(struct_size)
                    if not data or len(data) < struct_size:
                        break
                        
                    frame_number, timestamp, byte_offset, frame_size, is_keyframe = struct.unpack(struct_format, data)
                    indices.append({
                        'frame_number': frame_number,
                        'timestamp': datetime.fromtimestamp(timestamp),
                        'byte_offset': byte_offset,
                        'frame_size': frame_size,
                        'is_keyframe': bool(is_keyframe)
                    })
                    
            # 임시 파일 삭제
            if is_current and temp_file.exists():
                temp_file.unlink()
                
            logger.debug(f"인덱스 파일 읽기 완료: {len(indices)} 프레임")
                
        except Exception as e:
            logger.error(f"인덱스 파일 읽기 실패 {index_file_path}: {e}")
            
        return indices


class EventVideoExtractor:
    """이벤트 비디오 추출기"""
    
    def __init__(self, segment_manager, output_dir):
        self.segment_manager = segment_manager
        self.output_base_dir = Path(output_dir)
        self.output_base_dir.mkdir(parents=True, exist_ok=True)
        
    def get_output_dir_for_date(self, event_time):
        """날짜별 출력 디렉토리 가져오기"""
        date_folder = event_time.strftime("EVENT_%Y%m%d")
        output_dir = self.output_base_dir / date_folder
        output_dir.mkdir(parents=True, exist_ok=True)
        return output_dir
        
    def extract_event_video(self, camera_name, event_time, pre_seconds=15, post_seconds=15):
        """이벤트 전후 비디오 추출"""
        start_time = event_time - timedelta(seconds=pre_seconds)
        end_time = event_time + timedelta(seconds=post_seconds)
        
        # 해당 시간 범위의 세그먼트 찾기
        segments = self.segment_manager.find_segments_for_timerange(
            camera_name, start_time, end_time
        )
        
        if not segments:
            logger.error(f"해당 시간의 비디오 세그먼트를 찾을 수 없음: {camera_name} {event_time}")
            return None
            
        # 날짜별 출력 디렉토리
        output_dir = self.get_output_dir_for_date(event_time)
        
        # 출력 파일명 생성
        output_filename = f"event_{camera_name}_{event_time.strftime('%Y%m%d_%H%M%S')}.mp4"
        output_path = output_dir / output_filename
        
        logger.info(f"이벤트 비디오 저장 경로: {output_path}")
        
        if len(segments) == 1:
            # 단일 세그먼트인 경우
            return self._extract_from_single_segment(
                segments[0], start_time, end_time, output_path
            )
        else:
            # 여러 세그먼트에 걸친 경우
            return self._extract_from_multiple_segments(
                segments, start_time, end_time, output_path
            )
            
    def _extract_from_single_segment(self, segment, start_time, end_time, output_path):
        """단일 세그먼트에서 추출"""
        # 인덱스 읽기
        indices = self.segment_manager.read_index_file(
            segment['index_file'], 
            is_current=segment.get('is_current', False)
        )
        
        if not indices:
            logger.error("인덱스가 비어있음")
            return None
            
        logger.debug(f"인덱스 범위: {indices[0]['timestamp']} ~ {indices[-1]['timestamp']}")
        logger.debug(f"요청 범위: {start_time} ~ {end_time}")
        
        # 시작/종료 프레임 찾기
        start_frame_idx = None
        end_frame_idx = None
        
        # 요청한 시작 시간이 인덱스의 첫 프레임보다 이전인 경우
        if start_time < indices[0]['timestamp']:
            start_frame_idx = 0
        else:
            # 일반적인 경우 - start_time 이상인 첫 프레임 찾기
            for i, idx in enumerate(indices):
                if idx['timestamp'] >= start_time:
                    # 키프레임 확인
                    found_keyframe = False
                    # 이전 256프레임(약 25.6초) 내에서 키프레임 찾기
                    search_start = max(0, i - 256)
                    for j in range(i, search_start - 1, -1):
                        if indices[j]['is_keyframe']:
                            start_frame_idx = j
                            found_keyframe = True
                            logger.debug(f"키프레임 발견: 프레임 {j}, 시간 {indices[j]['timestamp']}")
                            break
                    
                    # 키프레임이 없으면 요청 시간보다 조금 이전부터
                    if not found_keyframe:
                        start_frame_idx = max(0, i - 10)  # 1초 전부터
                        logger.debug("키프레임 없음, 1초 전부터 시작")
                    break
                        
        # 종료 프레임 찾기 - end_time보다 큰 첫 번째 프레임까지
        for i in range(len(indices)):
            if indices[i]['timestamp'] > end_time:
                end_frame_idx = i - 1
                break
        
        # 마지막까지 갔는데 못 찾았으면 마지막 프레임
        if end_frame_idx is None:
            end_frame_idx = len(indices) - 1
            
        if start_frame_idx is None:
            logger.error("시작 프레임을 찾을 수 없음")
            return None
            
        # 실제 시간 범위 계산
        actual_start_time = indices[start_frame_idx]['timestamp']
        actual_end_time = indices[end_frame_idx]['timestamp']
        actual_duration = (actual_end_time - actual_start_time).total_seconds()
        
        logger.info(f"프레임 범위: {start_frame_idx} ~ {end_frame_idx} ({end_frame_idx - start_frame_idx + 1} 프레임)")
        logger.info(f"시간 범위: {actual_start_time.strftime('%H:%M:%S.%f')[:-3]} ~ {actual_end_time.strftime('%H:%M:%S.%f')[:-3]} ({actual_duration:.1f}초)")
            
        # H.264 데이터 추출
        start_offset = indices[start_frame_idx]['byte_offset']
        end_offset = indices[end_frame_idx]['byte_offset'] + indices[end_frame_idx]['frame_size']
        
        # 현재 녹화 중인 파일인 경우 복사본 사용
        if segment.get('is_current', False):
            temp_video = segment['video_file'].with_suffix('.tmp')
            shutil.copy2(segment['video_file'], temp_video)
            video_file = temp_video
        else:
            video_file = segment['video_file']
            
        try:
            # FFmpeg로 변환
            # 실제 녹화 fps 확인 (videorate로 10fps로 변환된 경우)
            actual_fps = 10  # camera.py에서 videorate로 설정한 값
            
            result = self._convert_h264_to_mp4(
                video_file, start_offset, end_offset - start_offset,
                output_path, actual_fps
            )
            
            # 임시 파일 삭제
            if segment.get('is_current', False) and temp_video.exists():
                temp_video.unlink()
                
            return result
            
        except Exception as e:
            logger.error(f"비디오 추출 실패: {e}")
            if segment.get('is_current', False) and temp_video.exists():
                temp_video.unlink()
            return None
        
    def _extract_from_multiple_segments(self, segments, start_time, end_time, output_path):
        """여러 세그먼트에서 추출"""
        # 임시 파일은 출력 디렉토리에 생성
        temp_dir = output_path.parent
        temp_files = []
        
        try:
            for i, segment in enumerate(segments):
                # 각 세그먼트에서 필요한 부분 추출
                indices = self.segment_manager.read_index_file(
                    segment['index_file'],
                    is_current=segment.get('is_current', False)
                )
                
                if not indices:
                    continue
                    
                # 세그먼트별 시작/종료 시간 계산
                seg_start_time = max(start_time, segment['start_time'])
                seg_end_time = min(end_time, segment['end_time'])
                
                # 프레임 인덱스 찾기
                start_frame_idx = None
                end_frame_idx = None
                
                for j, idx in enumerate(indices):
                    if start_frame_idx is None and idx['timestamp'] >= seg_start_time:
                        # 키프레임에서 시작
                        for k in range(j, -1, -1):
                            if indices[k]['is_keyframe']:
                                start_frame_idx = k
                                break
                                
                    if idx['timestamp'] <= seg_end_time:
                        end_frame_idx = j
                        
                if start_frame_idx is None or end_frame_idx is None:
                    continue
                    
                # 임시 파일로 추출
                temp_file = temp_dir / f"temp_segment_{i}.h264"
                temp_files.append(temp_file)
                
                start_offset = indices[start_frame_idx]['byte_offset']
                end_offset = indices[end_frame_idx]['byte_offset'] + indices[end_frame_idx]['frame_size']
                
                # 현재 녹화 중인 파일 처리
                if segment.get('is_current', False):
                    temp_video = segment['video_file'].with_suffix('.tmp')
                    shutil.copy2(segment['video_file'], temp_video)
                    video_file = temp_video
                else:
                    video_file = segment['video_file']
                
                # H.264 데이터 복사
                with open(video_file, 'rb') as src:
                    src.seek(start_offset)
                    data = src.read(end_offset - start_offset)
                    
                with open(temp_file, 'wb') as dst:
                    dst.write(data)
                    
                # 임시 비디오 파일 삭제
                if segment.get('is_current', False) and temp_video.exists():
                    temp_video.unlink()
                    
            # 모든 세그먼트 결합
            if temp_files:
                # 실제 녹화 fps 사용
                actual_fps = 10
                return self._concat_segments_to_mp4(temp_files, output_path, actual_fps)
            else:
                logger.error("추출할 세그먼트가 없음")
                return None
                
        finally:
            # 임시 파일 삭제
            for temp_file in temp_files:
                if temp_file.exists():
                    temp_file.unlink()
                    
    def _convert_h264_to_mp4(self, h264_file, offset, size, output_path, fps):
        """H.264를 MP4로 변환"""
        # H.264 데이터 읽기
        with open(h264_file, 'rb') as f:
            f.seek(offset)
            h264_data = f.read(size)
            
        # FFmpeg 명령 - 복사 모드 사용
        cmd = [
            'ffmpeg',
            '-f', 'h264',
            '-framerate', str(fps),
            '-i', '-',
            '-c:v', 'copy',  # 재인코딩 없이 복사
            '-movflags', 'faststart',
            '-y',
            str(output_path)
        ]
        
        # 실행
        process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = process.communicate(input=h264_data)
        
        if process.returncode == 0:
            logger.info(f"MP4 변환 성공: {output_path}")
            return output_path
        else:
            logger.error(f"FFmpeg 오류: {stderr.decode()}")
            return None
            
    def _concat_segments_to_mp4(self, h264_files, output_path, fps):
        """여러 H.264 파일을 하나의 MP4로 결합"""
        # concat 파일을 출력 디렉토리에 생성
        concat_file = output_path.parent / "concat_list.txt"
        
        try:
            with open(concat_file, 'w') as f:
                for h264_file in h264_files:
                    # 절대 경로 사용
                    f.write(f"file '{h264_file.absolute()}'\n")
                    
            # 파일 내용 확인 (디버그)
            logger.debug(f"Concat list file: {concat_file}")
            with open(concat_file, 'r') as f:
                logger.debug(f"Concat content:\n{f.read()}")
                
            # FFmpeg concat
            cmd = [
                'ffmpeg',
                '-f', 'concat',
                '-safe', '0',
                '-i', str(concat_file.absolute()),
                '-c:v', 'copy',  # 복사 모드 사용
                '-movflags', 'faststart',
                '-y',
                str(output_path.absolute())
            ]
            
            logger.debug(f"FFmpeg command: {' '.join(cmd)}")
            
            result = subprocess.run(cmd, capture_output=True, text=True)
            
            if result.returncode == 0:
                logger.info(f"MP4 결합 성공: {output_path}")
                return output_path
            else:
                logger.error(f"FFmpeg 오류: {result.stderr}")
                return None
                
        finally:
            # concat 파일 삭제
            if concat_file.exists():
                concat_file.unlink()


class DetectionDataRecorder:
    """검출 데이터 기록기"""
    
    def __init__(self, buffer_duration=120):
        self.buffer_duration = buffer_duration
        self.data_buffer = deque()
        self.lock = threading.Lock()
        
    def add_detection(self, detection_data):
        """검출 데이터 추가"""
        with self.lock:
            self.data_buffer.append(detection_data)
            
            # 오래된 데이터 제거
            cutoff_time = datetime.now() - timedelta(seconds=self.buffer_duration)
            while self.data_buffer and self.data_buffer[0]['timestamp'] < cutoff_time:
                self.data_buffer.popleft()
                
    def save_event_detections(self, event_time, output_path, pre_seconds=15, post_seconds=15, camera='unknown'):
        """이벤트 전후 검출 데이터 저장"""
        start_time = event_time - timedelta(seconds=pre_seconds)
        end_time = event_time + timedelta(seconds=post_seconds)
        
        with self.lock:
            event_detections = []
            for data in self.data_buffer:
                if start_time <= data['timestamp'] <= end_time:
                    event_detections.append({
                        'timestamp': data['timestamp'].isoformat(),
                        'camera': data.get('camera', 'unknown'),
                        'objects': data.get('objects', [])
                    })

        logger.info(f"이벤트 검출 데이터 저장: {camera} ({start_time.strftime('%H:%M:%S')} ~ {end_time.strftime('%H:%M:%S')})")            
        # JSON으로 저장
        save_data = {
            'event_time': event_time.isoformat(),
            'start_time': start_time.isoformat(),
            'end_time': end_time.isoformat(),
            'detection_count': len(event_detections),
            'detections': event_detections,
            'camera': camera
        }
        
        with open(output_path, 'w') as f:
            json.dump(save_data, f, indent=2)
            
        logger.info(f"검출 데이터 저장: {output_path} ({len(event_detections)}개)")
        return len(event_detections)

class SimpleDetectionRecorder:
    """간단한 검출 데이터 기록기"""
    
    def __init__(self, inference_api_host='localhost', inference_api_port=8888):
        self.inference_api_host = inference_api_host
        self.inference_api_port = inference_api_port
        
    def get_event_detections(self, camera, event_time, pre_seconds=15, post_seconds=15):
        """이벤트 구간의 모든 검출 데이터 조회"""
        start_time = event_time - timedelta(seconds=pre_seconds)
        end_time = event_time + timedelta(seconds=post_seconds)
        
        logger.info(f"검출 데이터 조회: {camera} ({start_time.strftime('%H:%M:%S')} ~ {end_time.strftime('%H:%M:%S')})")
        
        try:
            # 추론 시스템에서 검출 데이터 조회
            detections = self._query_inference_system(camera, start_time, end_time)
            logger.info(f"검출 데이터 조회 완료: {len(detections)}개")
            return detections
            
        except Exception as e:
            logger.error(f"검출 데이터 조회 실패: {e}")
            return []
    
    def _query_inference_system(self, camera, start_time, end_time):
        """추론 시스템 API 호출"""
        try:
            # TCP 연결
            client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            client.settimeout(5.0)
            client.connect((self.inference_api_host, self.inference_api_port))
            
            # 요청 데이터
            request = {
                'action': 'get_detections',
                'camera': camera,
                'start_time': start_time.isoformat(),
                'end_time': end_time.isoformat()
            }
            
            # 요청 전송
            client.sendall(json.dumps(request).encode('utf-8'))
            
            # 응답 수신
            response_data = b''
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    break
                response_data += chunk
                # JSON 완료 확인
                try:
                    json.loads(response_data.decode('utf-8'))
                    break
                except:
                    continue
            
            client.close()
            
            # 응답 파싱
            response = json.loads(response_data.decode('utf-8'))
            
            if response['status'] == 'success':
                return response['detections']
            else:
                logger.error(f"API 오류: {response.get('message', 'Unknown error')}")
                return []
                
        except Exception as e:
            logger.error(f"추론 시스템 연결 실패: {e}")
            return []
    
    def save_detection_data(self, event_time, camera, detections, output_path, event_class=0):
        """검출 데이터를 JSON 파일로 저장"""
        if not detections:
            logger.warning("저장할 검출 데이터가 없음 - 빈 파일 생성")
            detections = []  # 빈 리스트로 설정
        
        # 필요한 데이터만 추출
        simple_detections = []
        
        for detection in detections:
            timestamp = detection.get('timestamp', '')
            frame_number = detection.get('frame_number', -1)
            objects = detection.get('objects', [])
            
            for obj in objects:
                simple_detection = {
                    'timestamp': timestamp,
                    'frame_number': frame_number,
                    'class_id': obj.get('class_id', ''),
                    'confidence': obj.get('confidence', 0.0),
                    'bbox': obj.get('bbox', [0, 0, 0, 0]),  # [x1, y1, x2, y2]
                    'bbox_color': obj.get('bbox_color', 'green'),  # 각 객체의 색상
                    'has_bbox': obj.get('has_bbox', True)
                }
                simple_detections.append(simple_detection)
        
        # 저장할 데이터 구성
        save_data = {
            'event_time': event_time.isoformat(),
            'event_class': event_class,  # CLASS enum 값 추가
            'camera': camera,
            'total_detections': len(simple_detections),
            'detections': simple_detections
        }
        
        # JSON 파일로 저장
        try:
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(save_data, f, indent=2, ensure_ascii=False)
            
            logger.info(f"검출 데이터 저장 완료: {output_path} (클래스: {event_class})")
            return output_path
            
        except Exception as e:
            logger.error(f"검출 데이터 저장 실패: {e}")
            return None


class EnhancedEventVideoExtractor:
    """검출 데이터를 포함한 비디오 추출기"""
    
    def __init__(self, segment_manager, output_dir):
        # 기존 EventVideoExtractor 초기화
        self.base_extractor = EventVideoExtractor(segment_manager, output_dir)
        
        # 검출 데이터 레코더
        self.detection_recorder = SimpleDetectionRecorder()
        
    def extract_event_video_with_detections(self, camera_name, event_time, 
                                          pre_seconds=15, post_seconds=15, camera_type_name= 'unknown'):
        """검출 데이터와 함께 이벤트 비디오 추출"""
        logger.info(f"이벤트 비디오 + 검출 데이터 추출: {camera_type_name}")
        
        # 1. 비디오 추출
        video_path = self.base_extractor.extract_event_video(
            camera_type_name, event_time, pre_seconds, post_seconds
        )
        
        if not video_path:
            logger.error("비디오 추출 실패")
            return None
        
        # 2. 검출 데이터 조회
        detections = self.detection_recorder.get_event_detections(
            camera_type_name, event_time, pre_seconds, post_seconds
        )
        
        # 3. 검출 데이터 저장
        detection_path = video_path.with_suffix('.detections.json')
        saved_detection_path = self.detection_recorder.save_detection_data(
            event_time, camera_name, detections, detection_path
        )
        
        result = {
            'video_path': video_path,
            'detection_path': saved_detection_path,
            'detection_count': len(detections) if detections else 0
        }
        
        logger.info(f"이벤트 추출 완료: 비디오 + {result['detection_count']}개 검출")
        return result


class EventScheduler:
    """이벤트 처리 스케줄러 - 시간이 지난 이벤트만 처리"""
    
    def __init__(self, process_delay=30):
        self.process_delay = process_delay  # 이벤트 후 처리 대기 시간
        self.pending_events = []  # 힙큐 (우선순위 큐)
        self.lock = threading.Lock()
        self.event_id_counter = 0
        
    def add_event(self, event):
        """이벤트 추가"""
        with self.lock:
            self.event_id_counter += 1
            event['event_id'] = self.event_id_counter
            
            # 처리 예정 시간 계산
            process_time = event['timestamp'] + timedelta(seconds=self.process_delay)
            
            # 힙큐에 추가 (처리시간, 이벤트ID, 이벤트)
            heapq.heappush(self.pending_events, (process_time, self.event_id_counter, event))
            
            logger.info(f"이벤트 #{self.event_id_counter} 스케줄링: "
                       f"{event['camera']} - {event['timestamp'].strftime('%H:%M:%S')} "
                       f"→ 처리예정: {process_time.strftime('%H:%M:%S')}")
            
    def get_ready_events(self):
        """처리 준비된 이벤트 반환"""
        ready_events = []
        current_time = datetime.now()
        
        with self.lock:
            # 처리 시간이 된 이벤트들 추출
            while self.pending_events and self.pending_events[0][0] <= current_time:
                process_time, event_id, event = heapq.heappop(self.pending_events)
                ready_events.append(event)
                
        return ready_events
        
    def get_pending_count(self):
        """대기 중인 이벤트 수"""
        with self.lock:
            return len(self.pending_events)


class EventRecorderSystem:
    """이벤트 기반 녹화 시스템 메인 클래스"""
    
    def __init__(self, config_file='event_config.json'):
        self.config = self.load_config(config_file)
        self.event_queue = mp.Queue(maxsize=100)
        self.detection_queue = mp.Queue(maxsize=1000)
        self.stop_event = mp.Event()
        self.processes = []
        self._setup_auto_ip()

        # 디버그 모드 설정
        if self.config.get('debug', False):
            logging.getLogger().setLevel(logging.DEBUG)
        
        # 컴포넌트 초기화
        self.segment_manager = VideoSegmentManager(self.config['camera_base_dir'])
        self.video_extractor = EnhancedEventVideoExtractor(
            self.segment_manager,
            self.config['event_output_dir']
        )
        self.detection_recorder = DetectionDataRecorder()

        self.notification_sender = NotificationSender(
            server_ip=self.config.get('server_ip', '52.194.238.184'),
            port=self.config.get('server_port', 0),
            phone=self.config.get('phone', ''),
            password=self.config.get('password', '')
        )
        
        # OSD 완료 모니터
        self.osd_monitor = OSDCompletionMonitor(
            base_dir=self.config.get('output_dir', '/home/nvidia/data'),
            notification_sender=self.notification_sender,
            http_service_ip=self.config.get('http_service_ip', '192.168.1.100')
        )
        
        # 이벤트 스케줄러
        process_delay = self.config['pre_event_duration'] + self.config['post_event_duration'] + 2
        self.event_scheduler = EventScheduler(process_delay)
        
        # 프로세스 리스트
        self.processes = []

    def _setup_auto_ip(self):
        """IP 주소 자동 설정"""
        # http_service_ip가 'auto'이거나 비어있으면 자동 감지
        if not self.config.get('http_service_ip') or self.config.get('http_service_ip') == 'auto':
            ip_manager = IPAddressManager(
                http_port=self.config.get('http_service_port', 80)
            )

            # 내부망/외부망 선택 (설정으로 제어 가능)
            prefer_global = self.config.get('prefer_global_ip', True)
            # 포트를 포함한 전체 주소 가져오기
            port = self.config.get('http_service_port', 80)
            ip_only = ip_manager.get_http_service_ip(
            prefer_global=prefer_global,
            include_port=False  # 항상 False로 설정
            )

            if port != 80:
                self.config['http_service_ip'] = f"{ip_only}:{port}"
            else:
                self.config['http_service_ip'] = ip_only
            
            logger.info(f"HTTP 서비스 IP 자동 설정: {self.config['http_service_ip']}")

    def load_config(self, config_file):
        """설정 파일 로드"""
        default_config = {
            'camera_base_dir': '/home/nvidia/data',
            'video_base_dir': '/home/nvidia/data',
            'output_dir': '/home/nvidia/data',
            'pre_event_duration': 15,
            'post_event_duration': 15,
            'tcp_host': '0.0.0.0',
            'tcp_port': 9999,
            'tcp_server_enabled': True,
            # IP 관련 설정
            'http_service_ip': 'auto',      # 'auto' 또는 특정 IP
            'http_service_port': 9615,     # HTTP 서비스 포트
            'prefer_global_ip': True,      # True: 외부IP 우선, False: 내부IP 우선
            # 알림 설정
            'server_ip': '52.194.238.184',
            'server_port': 0,
            'phone': 'itechour',
            'password': '12341234'
        }
        
        config_path = Path(config_file)
        if config_path.exists():
            with open(config_path, 'r') as f:
                config = json.load(f)
                # 기본값으로 누락된 항목 채우기
                for key, value in default_config.items():
                    if key not in config:
                        config[key] = value
                return config
        else:
            # 기본 설정 파일 생성
            with open(config_path, 'w') as f:
                json.dump(default_config, f, indent=2)
            logger.info(f"기본 설정 파일 생성: {config_file}")
            return default_config
            
    def process_events(self):
        """이벤트 처리 프로세스"""
        logger.info("이벤트 처리 프로세스 시작")
        
        while not self.stop_event.is_set():
            try:
                # 스케줄러에서 처리 준비된 이벤트 확인
                ready_events = self.event_scheduler.get_ready_events()
                
                for event in ready_events:
                    logger.info(f"이벤트 처리 시작: {event['type']} at {event['timestamp']}")
                    
                    # 비디오 추출
                    camera_type = event.get('camera_type', 0)
                    camera = 'RGB_Camera' if camera_type == 0 else 'Thermal_Camera'
                    logger.info(f"카메라: {camera}, 이벤트 클래스: {event.get('event_class', 0)}")
                    video_path = self.video_extractor.extract_event_video(
                        event.get('camera', 'unknown'),
                        event['timestamp'],
                        self.config['pre_event_duration'],
                        self.config['post_event_duration']
                    )
                    
                    if video_path:
                        # 이벤트 메타데이터에서 검출 데이터 저장
                        detection_path = video_path.with_suffix('.json')
                        
                        # 이벤트에 포함된 검출 데이터 사용
                        if 'metadata' in event and 'objects' in event['metadata']:
                            save_data = {
                                'event_time': event['timestamp'].isoformat(),
                                'start_time': (event['timestamp'] - timedelta(seconds=self.config['pre_event_duration'])).isoformat(),
                                'end_time': (event['timestamp'] + timedelta(seconds=self.config['post_event_duration'])).isoformat(),
                                'detection_count': 1,
                                'detections': [{
                                    'timestamp': event['metadata']['timestamp'].isoformat(),
                                    'camera': event['metadata']['camera'],
                                    'objects': event['metadata']['objects']
                                }]
                            }
                            
                            with open(detection_path, 'w') as f:
                                json.dump(save_data, f, indent=2)
                            
                            logger.info(f"검출 데이터 저장: {detection_path}")

                            self.detection_recorder.save_event_detections(
                                event['timestamp'],
                                detection_path,
                                self.config['pre_event_duration'],
                                self.config['post_event_duration'],
                                event.get('camera', 'unknown')
                            )
                        
                        logger.info(f"이벤트 처리 완료: {video_path}")
                    else:
                        logger.error("비디오 추출 실패")
                
                # 새 이벤트 확인
                try:
                    event = self.event_queue.get(timeout=1)
                    self.event_scheduler.add_event(event)
                except:
                    # 타임아웃은 정상
                    pass
                    
                # 대기 중인 이벤트 수 로그
                pending_count = self.event_scheduler.get_pending_count()
                if pending_count > 0:
                    logger.debug(f"대기 중인 이벤트: {pending_count}개")
                    
            except Exception as e:
                logger.error(f"이벤트 처리 오류: {e}")

    def process_events_with_detections(self):
        """검출 데이터를 포함한 이벤트 처리"""
        logger.info("검출 데이터 포함 이벤트 처리 시작")
        
        # 기존 비디오 추출기 대신 향상된 버전 사용
        enhanced_extractor = EnhancedEventVideoExtractor(
            self.segment_manager,
            self.config['output_dir']
        )
        
        while not self.stop_event.is_set():
            try:
                ready_events = self.event_scheduler.get_ready_events()
                
                for event in ready_events:
                    event_class = event.get('event_class', 0)
                    camera_type = event.get('camera_type', 0)
                    camera_type_name = 'RGB_Camera' if camera_type == 0 else 'Thermal_Camera'
                    
                    logger.info(f"이벤트 처리: {event['type']} at {event['timestamp']} - 클래스: {event_class}")
                    
                    # 비디오와 검출 데이터 추출 (이미 검출 데이터 저장 포함)
                    result = self.video_extractor.extract_event_video_with_detections(
                        event.get('camera', 'unknown'),
                        event['timestamp'],
                        self.config['pre_event_duration'],
                        self.config['post_event_duration'],
                        camera_type_name
                    )
                    
                    if result:
                        video_path = result['video_path']
                        detection_path = result.get('detection_path')
                        
                        # 이미 저장된 검출 데이터가 있고, event_class 정보가 있으면 업데이트
                        if detection_path and detection_path.exists() and event_class != 0:
                            try:
                                # 기존 파일 읽기
                                with open(detection_path, 'r') as f:
                                    existing_data = json.load(f)
                                
                                # event_class만 업데이트
                                existing_data['event_class'] = event_class
                                
                                # 다시 저장
                                with open(detection_path, 'w') as f:
                                    json.dump(existing_data, f, indent=2)
                                
                                logger.info(f"검출 데이터 클래스 업데이트: {detection_path} (클래스: {event_class})")
                            except Exception as e:
                                logger.error(f"검출 데이터 업데이트 실패: {e}")
                        
                        logger.info(f"이벤트 처리 완료: {video_path}, 검출 수: {result.get('detection_count', 0)}")
                    else:
                        logger.error("비디오 추출 실패")
                
                # 새 이벤트 확인
                try:
                    event = self.event_queue.get(timeout=1)
                    self.event_scheduler.add_event(event)
                except:
                    pass
                    
                # 대기 중인 이벤트 수 로그
                pending_count = self.event_scheduler.get_pending_count()
                if pending_count > 0:
                    logger.debug(f"대기 중인 이벤트: {pending_count}개")
                    
            except Exception as e:
                logger.error(f"이벤트 처리 오류: {e}")
                import traceback
                traceback.print_exc()

    def process_detections(self):
        """검출 데이터 처리 프로세스"""
        logger.info("검출 데이터 처리 프로세스 시작")
        
        while not self.stop_event.is_set():
            try:
                # 검출 데이터 대기
                detection = self.detection_queue.get(timeout=1)
                
                # 버퍼에 추가
                self.detection_recorder.add_detection(detection)
                logger.debug(f"검출 데이터 추가: {detection['camera']} - {len(detection['objects'])}개 객체")
                
            except:
                continue
                
    def tcp_server_process(self):
        """TCP 서버 - 외부 프로세스로부터 이벤트 수신"""
        logger.info("TCP 이벤트 서버 시작")
        
        host = self.config.get('tcp_host', 'localhost')
        port = self.config.get('tcp_port', 9999)
        
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind((host, port))
        server_socket.listen(5)
        server_socket.settimeout(1.0)  # 1초 타임아웃
        
        logger.info(f"TCP 서버 대기 중: {host}:{port}")
        
        while not self.stop_event.is_set():
            try:
                client_socket, addr = server_socket.accept()
                logger.info(f"클라이언트 연결: {addr}")
                
                # 메시지 길이 읽기
                length_bytes = client_socket.recv(4)
                if not length_bytes:
                    continue
                    
                message_length = int.from_bytes(length_bytes, 'big')
                
                # 메시지 읽기
                message = b''
                while len(message) < message_length:
                    chunk = client_socket.recv(min(message_length - len(message), 4096))
                    if not chunk:
                        break
                    message += chunk

                print(f"내용 : {message.decode('utf-8')}")

                # JSON 디코딩
                event_data = json.loads(message.decode('utf-8'))
                
                # timestamp 변환
                if 'timestamp' in event_data and isinstance(event_data['timestamp'], str):
                    event_data['timestamp'] = datetime.fromisoformat(event_data['timestamp'])
                if 'metadata' in event_data and 'timestamp' in event_data['metadata']:
                    if isinstance(event_data['metadata']['timestamp'], str):
                        event_data['metadata']['timestamp'] = datetime.fromisoformat(event_data['metadata']['timestamp'])
                
                # 이벤트 큐에 추가
                try:
                    self.event_queue.put_nowait(event_data)
                    client_socket.sendall(b"OK")
                    logger.info(f"이벤트 수신: {event_data['type']} - {event_data.get('camera', 'unknown')} - 카메라 타입 : {event_data.get('camera_type', 'unknown')}")
                except:
                    client_socket.sendall(b"QUEUE_FULL")
                    logger.warning("이벤트 큐가 가득참")
                    
                client_socket.close()
                
            except socket.timeout:
                continue
            except Exception as e:
                logger.error(f"TCP 서버 오류: {e}")
                
        server_socket.close()
        logger.info("TCP 서버 종료")
        
    def add_external_event(self, event_data):
        """외부 프로세스에서 이벤트 추가하는 API"""
        try:
            # 필수 필드 확인
            required_fields = ['type', 'camera', 'timestamp']
            for field in required_fields:
                if field not in event_data:
                    logger.error(f"이벤트에 필수 필드 누락: {field}")
                    return False
                    
            # timestamp가 문자열인 경우 datetime으로 변환
            if isinstance(event_data['timestamp'], str):
                event_data['timestamp'] = datetime.fromisoformat(event_data['timestamp'])
                
            # 이벤트 큐에 추가
            self.event_queue.put_nowait(event_data)
            logger.info(f"외부 이벤트 추가: {event_data['type']} - {event_data['camera']}")
            return True
            
        except Exception as e:
            logger.error(f"외부 이벤트 추가 실패: {e}")
            return False
            
    def start(self):
        """시스템 시작"""
        logger.info("이벤트 녹화 시스템 시작")

        self.osd_monitor.start()
        
        # 이벤트 처리 프로세스
        event_process = mp.Process(target=self.process_events_with_detections)
        event_process.start()
        self.processes.append(event_process)
        
        # 검출 데이터 처리 프로세스
        detection_process = mp.Process(target=self.process_detections)
        detection_process.start()
        self.processes.append(detection_process)
        
        # TCP 서버 프로세스 (외부 이벤트 수신용)
        if self.config.get('tcp_server_enabled', True):
            tcp_process = mp.Process(target=self.tcp_server_process)
            tcp_process.start()
            self.processes.append(tcp_process)
        
        logger.info("모든 프로세스 시작 완료")
        
    def stop(self):
        """시스템 종료"""
        logger.info("이벤트 녹화 시스템 종료 시작")

        self.osd_monitor.stop()
        
        # 종료 신호
        self.stop_event.set()
        
        # 모든 프로세스 종료 대기
        for process in self.processes:
            process.join(timeout=5)
            if process.is_alive():
                process.terminate()
                
        logger.info("이벤트 녹화 시스템 종료 완료")

def main():
    """메인 함수"""
    # 명령행 인자 처리
    if len(sys.argv) > 1:
        if sys.argv[1] == '--debug':
            logging.getLogger().setLevel(logging.DEBUG)
    
    system = EventRecorderSystem()
    
    # 시그널 핸들러
    def signal_handler(sig, frame):
        logger.info("종료 신호 수신")
        system.stop()
        sys.exit(0)
        
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    try:
        system.start()
        
        # 메인 프로세스는 대기
        while True:
            time.sleep(1)
            
    except KeyboardInterrupt:
        logger.info("키보드 인터럽트")
    finally:
        system.stop()


if __name__ == "__main__":
    main()