#!/usr/bin/env python3
"""
Jetson Xavier NX에서 Windows PC로 MP4 파일 전송 스크립트
HTTP POST를 사용한 전송
"""

import os
import time
import json
import logging
import requests
from datetime import datetime, timedelta
from pathlib import Path
import schedule
import hashlib

# 로깅 설정
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('/home/nvidia/file_transfer.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


class HTTPFileTransfer:
    def __init__(self, config_path='http_transfer_config.json'):
        """HTTP 파일 전송 클래스 초기화"""
        with open(config_path, 'r') as f:
            self.config = json.load(f)
        
        self.server_url = self.config['server_url']  # http://192.168.1.100:5000
        self.local_record_path = self.config['local_record_path']
        self.transfer_interval = self.config.get('transfer_interval_minutes', 30)
        self.delete_after_transfer = self.config.get('delete_after_transfer', False)
        self.file_age_minutes = self.config.get('file_age_minutes', 10)
        self.chunk_size = self.config.get('chunk_size_mb', 10) * 1024 * 1024
        
        # 전송 상태 추적 파일
        self.state_file = Path('/home/nvidia/transfer_state.json')
        self.load_state()
        
        # 서버 연결 테스트
        self.test_connection()
    
    def load_state(self):
        """전송 상태 로드"""
        if self.state_file.exists():
            with open(self.state_file, 'r') as f:
                self.state = json.load(f)
        else:
            self.state = {'transferred_files': {}}
    
    def save_state(self):
        """전송 상태 저장"""
        with open(self.state_file, 'w') as f:
            json.dump(self.state, f, indent=2)
    
    def calculate_checksum(self, file_path, chunk_size=8192):
        """파일 체크섬 계산"""
        md5 = hashlib.md5()
        with open(file_path, 'rb') as f:
            while chunk := f.read(chunk_size):
                md5.update(chunk)
        return md5.hexdigest()
    
    def test_connection(self):
        """서버 연결 테스트"""
        try:
            response = requests.get(f"{self.server_url}/status", timeout=5)
            if response.status_code == 200:
                data = response.json()
                logger.info(f"서버 연결 성공: {data}")
                return True
        except Exception as e:
            logger.error(f"서버 연결 실패: {e}")
            return False
    
    def get_files_to_transfer(self):
        """전송할 파일 목록 조회"""
        files_to_transfer = []
        current_time = datetime.now()
        cutoff_time = current_time - timedelta(minutes=self.file_age_minutes)
        
        # 녹화 경로의 모든 MP4 파일 검색
        for root, dirs, files in os.walk(self.local_record_path):
            for file in files:
                if file.endswith('.mp4'):
                    file_path = Path(root) / file
                    
                    # 파일 수정 시간 확인
                    mtime = datetime.fromtimestamp(file_path.stat().st_mtime)
                    
                    # 일정 시간이 지난 파일만 전송 (녹화 중인 파일 제외)
                    if mtime < cutoff_time:
                        # 이미 전송된 파일인지 확인
                        file_str = str(file_path)
                        if file_str not in self.state['transferred_files']:
                            files_to_transfer.append(file_path)
                        elif self.delete_after_transfer:
                            # 이미 전송되었고 삭제 옵션이 켜져있으면 삭제
                            try:
                                file_path.unlink()
                                logger.info(f"전송 완료된 파일 삭제: {file_path}")
                            except Exception as e:
                                logger.error(f"파일 삭제 실패: {e}")
        
        return files_to_transfer
    
    def upload_file_http(self, file_path):
        """HTTP POST로 파일 업로드"""
        try:
            # 파일 메타데이터
            metadata = {
                'original_path': str(file_path),
                'timestamp': datetime.now().isoformat(),
                'checksum': self.calculate_checksum(file_path),
                'device_id': self.config.get('device_id', 'jetson_nx')
            }
            
            # 파일 전송
            with open(file_path, 'rb') as f:
                files = {'file': (file_path.name, f, 'video/mp4')}
                data = {'metadata': json.dumps(metadata)}
                
                # 진행률 표시를 위한 커스텀 업로드
                response = requests.post(
                    f"{self.server_url}/upload",
                    files=files,
                    data=data,
                    timeout=300  # 5분 타임아웃
                )
            
            if response.status_code == 200:
                result = response.json()
                logger.info(f"파일 업로드 성공: {result}")
                return True
            else:
                logger.error(f"업로드 실패: {response.status_code} - {response.text}")
                return False
                
        except requests.exceptions.ConnectionError:
            logger.error("서버에 연결할 수 없습니다. Windows 방화벽을 확인하세요.")
            return False
        except Exception as e:
            logger.error(f"파일 업로드 중 오류: {e}")
            return False
    
    def transfer_files(self):
        """파일 전송 메인 함수"""
        logger.info("파일 전송 작업 시작")
        
        # 서버 연결 확인
        if not self.test_connection():
            logger.error("서버에 연결할 수 없어 전송을 중단합니다")
            return
        
        files_to_transfer = self.get_files_to_transfer()
        
        if not files_to_transfer:
            logger.info("전송할 파일이 없습니다")
            return
        
        logger.info(f"전송 대상 파일 수: {len(files_to_transfer)}")
        
        transferred_count = 0
        failed_count = 0
        
        for file_path in files_to_transfer:
            try:
                logger.info(f"파일 전송 시작: {file_path}")
                
                if self.upload_file_http(file_path):
                    # 전송 성공 기록
                    self.state['transferred_files'][str(file_path)] = {
                        'timestamp': datetime.now().isoformat(),
                        'size': file_path.stat().st_size
                    }
                    self.save_state()
                    
                    transferred_count += 1
                    
                    # 삭제 옵션이 켜져있으면 파일 삭제
                    if self.delete_after_transfer:
                        try:
                            file_path.unlink()
                            logger.info(f"원본 파일 삭제: {file_path}")
                        except Exception as e:
                            logger.error(f"파일 삭제 실패: {e}")
                else:
                    failed_count += 1
                    
            except Exception as e:
                logger.error(f"파일 전송 중 오류 발생: {e}")
                failed_count += 1
        
        logger.info(f"전송 완료 - 성공: {transferred_count}, 실패: {failed_count}")
    
    def cleanup_old_state(self):
        """오래된 전송 기록 정리"""
        cutoff_date = datetime.now() - timedelta(days=7)
        
        files_to_remove = []
        for file_path, info in self.state['transferred_files'].items():
            transfer_date = datetime.fromisoformat(info['timestamp'])
            if transfer_date < cutoff_date:
                files_to_remove.append(file_path)
        
        for file_path in files_to_remove:
            del self.state['transferred_files'][file_path]
        
        if files_to_remove:
            self.save_state()
            logger.info(f"오래된 전송 기록 {len(files_to_remove)}개 정리")
    
    def run_scheduled(self):
        """스케줄 실행"""
        # 초기 실행
        self.transfer_files()
        
        # 주기적 실행 스케줄 설정
        schedule.every(self.transfer_interval).minutes.do(self.transfer_files)
        
        # 매일 자정에 오래된 기록 정리
        schedule.every().day.at("00:00").do(self.cleanup_old_state)
        
        logger.info(f"파일 전송 스케줄러 시작 (주기: {self.transfer_interval}분)")
        
        while True:
            schedule.run_pending()
            time.sleep(60)  # 1분마다 스케줄 확인


if __name__ == "__main__":
    # 설정 파일 예시 생성
    sample_config = {
        "server_url": "http://192.168.1.100:5000",  # Windows PC IP
        "local_record_path": "/home/nvidia/data",
        "transfer_interval_minutes": 30,
        "delete_after_transfer": False,
        "file_age_minutes": 10,
        "chunk_size_mb": 10,
        "device_id": "jetson_nx_01"
    }
    
    # 설정 파일이 없으면 생성
    config_path = 'http_transfer_config.json'
    if not os.path.exists(config_path):
        with open(config_path, 'w') as f:
            json.dump(sample_config, f, indent=2)
        print(f"설정 파일 생성됨: {config_path}")
        print("Windows PC의 IP 주소를 설정 파일에 입력하세요.")
    else:
        # 파일 전송 시작
        try:
            transfer = HTTPFileTransfer(config_path)
            transfer.run_scheduled()
        except KeyboardInterrupt:
            logger.info("프로그램 종료")
        except Exception as e:
            logger.error(f"프로그램 오류: {e}")