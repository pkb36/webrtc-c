#!/usr/bin/env python3
"""
디스크 용량 체크 및 정리 스크립트
camera.py와 독립적으로 실행되어 디스크 공간을 관리합니다.
- 비디오 파일 정리 (RECORD 우선 삭제)
- 로그 파일 정리 (1주일 이상 된 로그 삭제)
"""

import os
import sys
import shutil
import time
from pathlib import Path
from datetime import datetime, timedelta
import logging
import argparse

class DiskCleaner:
    def __init__(self, target_path="/home/nvidia/data", min_free_percent=20.0, min_free_gb=50.0,
                 log_path="/home/nvidia/webrtc/logs", log_retention_days=7):
        """
        디스크 정리기 초기화
        
        Args:
            target_path: 모니터링할 경로
            min_free_percent: 최소 여유 공간 퍼센트
            min_free_gb: 최소 여유 공간 GB
            log_path: 로그 파일 경로
            log_retention_days: 로그 보관 일수
        """
        self.target_path = Path(target_path)
        self.min_free_percent = min_free_percent
        self.min_free_gb = min_free_gb * 1024 * 1024 * 1024  # GB to bytes
        self.log_path = Path(log_path)
        self.log_retention_days = log_retention_days
        
        # 로깅 설정
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(levelname)s - %(message)s'
        )
        self.logger = logging.getLogger(__name__)
        
    def get_disk_usage(self):
        """디스크 사용량 정보 반환"""
        stat = shutil.disk_usage(self.target_path)
        return {
            'total': stat.total,
            'used': stat.used,
            'free': stat.free,
            'percent': (stat.used / stat.total) * 100
        }
    
    def find_old_files(self, extensions=['.mp4', '.avi', '.mkv', '.mov', '.h264', '.h265']):
        """오래된 파일 찾기 (RECORD 폴더 우선)"""
        record_files = []
        event_files = []
        
        for ext in extensions:
            for file_path in self.target_path.rglob(f'*{ext}'):
                if file_path.is_file():
                    stat = file_path.stat()
                    file_info = {
                        'path': file_path,
                        'size': stat.st_size,
                        'mtime': datetime.fromtimestamp(stat.st_mtime)
                    }
                    
                    # 파일이 속한 폴더 확인
                    parent_folder = file_path.parent.name
                    if parent_folder.startswith('RECORD_'):
                        file_info['type'] = 'RECORD'
                        record_files.append(file_info)
                    elif parent_folder.startswith('EVENT_'):
                        file_info['type'] = 'EVENT'
                        event_files.append(file_info)
                    else:
                        # RECORD/EVENT 폴더가 아닌 경우도 RECORD로 분류 (우선 삭제)
                        file_info['type'] = 'OTHER'
                        record_files.append(file_info)
        
        # 각각 날짜 순으로 정렬 (오래된 것부터)
        record_files.sort(key=lambda x: x['mtime'])
        event_files.sort(key=lambda x: x['mtime'])
        
        # RECORD 파일을 먼저, 그 다음 EVENT 파일
        return record_files + event_files
    
    def need_cleanup(self):
        """정리가 필요한지 확인"""
        usage = self.get_disk_usage()
        
        # 여유 공간이 설정값보다 적은지 확인
        free_percent = 100 - usage['percent']
        
        if usage['free'] < self.min_free_gb:
            self.logger.warning(f"여유 공간 부족: {usage['free'] / (1024**3):.1f}GB < {self.min_free_gb / (1024**3):.1f}GB")
            return True
            
        if free_percent < self.min_free_percent:
            self.logger.warning(f"여유 공간 비율 부족: {free_percent:.1f}% < {self.min_free_percent}%")
            return True
            
        return False
    
    def cleanup_logs(self, dry_run=False):
        """오래된 로그 파일 정리"""
        if not self.log_path.exists():
            self.logger.warning(f"로그 경로가 존재하지 않습니다: {self.log_path}")
            return
        
        cutoff_date = datetime.now() - timedelta(days=self.log_retention_days)
        deleted_count = 0
        deleted_size = 0
        
        self.logger.info(f"로그 정리 시작 ({self.log_retention_days}일 이상 된 파일 삭제)")
        
        try:
            for log_file in self.log_path.glob('*.log'):
                if log_file.is_file():
                    stat = log_file.stat()
                    mtime = datetime.fromtimestamp(stat.st_mtime)
                    
                    if mtime < cutoff_date:
                        file_size = stat.st_size
                        age_days = (datetime.now() - mtime).days
                        
                        if dry_run:
                            self.logger.info(f"[DRY RUN] 로그 삭제 예정: {log_file.name} "
                                           f"({file_size / 1024:.1f}KB, {age_days}일 경과)")
                        else:
                            try:
                                log_file.unlink()
                                deleted_count += 1
                                deleted_size += file_size
                                self.logger.info(f"로그 삭제: {log_file.name} "
                                               f"({file_size / 1024:.1f}KB, {age_days}일 경과)")
                            except Exception as e:
                                self.logger.error(f"로그 삭제 실패: {log_file} - {e}")
        
        except Exception as e:
            self.logger.error(f"로그 정리 중 오류: {e}")
        
        if deleted_count > 0 or dry_run:
            self.logger.info(f"로그 정리 완료: {deleted_count}개 파일, "
                           f"{deleted_size / (1024*1024):.1f}MB 삭제" + 
                           (" 예정" if dry_run else ""))
    
    def get_log_status(self):
        """로그 파일 현황 반환"""
        if not self.log_path.exists():
            return None
        
        log_files = []
        total_size = 0
        
        for log_file in self.log_path.glob('*.log'):
            if log_file.is_file():
                stat = log_file.stat()
                mtime = datetime.fromtimestamp(stat.st_mtime)
                age_days = (datetime.now() - mtime).days
                
                log_files.append({
                    'name': log_file.name,
                    'size': stat.st_size,
                    'mtime': mtime,
                    'age_days': age_days,
                    'will_delete': age_days >= self.log_retention_days
                })
                total_size += stat.st_size
        
        # 날짜순 정렬
        log_files.sort(key=lambda x: x['mtime'])
        
        return {
            'count': len(log_files),
            'total_size': total_size,
            'files': log_files
        }
    
    def cleanup(self, dry_run=False):
        """디스크 정리 실행 (비디오 + 로그)"""
        # 먼저 로그 정리
        self.cleanup_logs(dry_run=dry_run)
        
        # 그 다음 비디오 파일 정리
        if not self.need_cleanup():
            self.logger.info("디스크 정리가 필요하지 않습니다.")
            return
        
        usage = self.get_disk_usage()
        target_free = max(self.min_free_gb, usage['total'] * (self.min_free_percent / 100))
        space_to_free = target_free - usage['free']
        
        self.logger.info(f"확보해야 할 공간: {space_to_free / (1024**3):.1f}GB")
        
        # 삭제할 파일 찾기
        old_files = self.find_old_files()
        
        if not old_files:
            self.logger.warning("삭제할 파일이 없습니다.")
            return
        
        freed_space = 0
        deleted_count = 0
        
        for file_info in old_files:
            if freed_space >= space_to_free:
                break
                
            file_path = file_info['path']
            file_size = file_info['size']
            
            if dry_run:
                self.logger.info(f"[DRY RUN] 삭제 예정 ({file_info.get('type', 'OTHER')}): {file_path} ({file_size / (1024**2):.1f}MB)")
            else:
                try:
                    self.logger.info(f"삭제 중 ({file_info.get('type', 'OTHER')}): {file_path} ({file_size / (1024**2):.1f}MB)")
                    file_path.unlink()
                    deleted_count += 1
                    freed_space += file_size
                except Exception as e:
                    self.logger.error(f"삭제 실패: {file_path} - {e}")
        
        if not dry_run:
            self.logger.info(f"정리 완료: {deleted_count}개 파일 삭제, {freed_space / (1024**3):.1f}GB 확보")
        else:
            self.logger.info(f"[DRY RUN] {deleted_count}개 파일 삭제 시 {freed_space / (1024**3):.1f}GB 확보 예상")
    
    def show_status(self):
        """현재 디스크 상태 표시"""
        usage = self.get_disk_usage()
        files = self.find_old_files()
        
        # 타입별로 분류
        record_files = [f for f in files if f.get('type') == 'RECORD']
        event_files = [f for f in files if f.get('type') == 'EVENT']
        other_files = [f for f in files if f.get('type') == 'OTHER']
        
        total_file_size = sum(f['size'] for f in files)
        record_size = sum(f['size'] for f in record_files)
        event_size = sum(f['size'] for f in event_files)
        
        print("\n=== 디스크 상태 ===")
        print(f"경로: {self.target_path}")
        print(f"전체 용량: {usage['total'] / (1024**3):.1f} GB")
        print(f"사용 중: {usage['used'] / (1024**3):.1f} GB ({usage['percent']:.1f}%)")
        print(f"여유 공간: {usage['free'] / (1024**3):.1f} GB")
        
        print(f"\n=== 파일 현황 ===")
        print(f"RECORD 파일: {len(record_files)}개 ({record_size / (1024**3):.1f} GB)")
        print(f"EVENT 파일: {len(event_files)}개 ({event_size / (1024**3):.1f} GB)")
        if other_files:
            print(f"기타 파일: {len(other_files)}개")
        print(f"전체 비디오 파일: {len(files)}개 ({total_file_size / (1024**3):.1f} GB)")
        
        if files:
            # RECORD 폴더 정보
            if record_files:
                oldest_record = record_files[0]
                newest_record = record_files[-1]
                print(f"\nRECORD 폴더:")
                print(f"  가장 오래된: {oldest_record['path'].parent.name}/{oldest_record['path'].name}")
                print(f"              ({oldest_record['mtime'].strftime('%Y-%m-%d %H:%M')})")
                print(f"  가장 최근: {newest_record['path'].parent.name}/{newest_record['path'].name}")
                print(f"            ({newest_record['mtime'].strftime('%Y-%m-%d %H:%M')})")
            
            # EVENT 폴더 정보
            if event_files:
                oldest_event = event_files[0]
                newest_event = event_files[-1]
                print(f"\nEVENT 폴더:")
                print(f"  가장 오래된: {oldest_event['path'].parent.name}/{oldest_event['path'].name}")
                print(f"              ({oldest_event['mtime'].strftime('%Y-%m-%d %H:%M')})")
                print(f"  가장 최근: {newest_event['path'].parent.name}/{newest_event['path'].name}")
                print(f"            ({newest_event['mtime'].strftime('%Y-%m-%d %H:%M')})")
        
        # 로그 파일 현황
        log_status = self.get_log_status()
        if log_status:
            print(f"\n=== 로그 파일 현황 ===")
            print(f"로그 경로: {self.log_path}")
            print(f"전체 로그: {log_status['count']}개 ({log_status['total_size'] / (1024*1024):.1f} MB)")
            
            old_logs = [f for f in log_status['files'] if f['will_delete']]
            if old_logs:
                print(f"삭제 대상 ({self.log_retention_days}일 이상): {len(old_logs)}개")
                oldest_log = old_logs[0]
                print(f"  가장 오래된: {oldest_log['name']} ({oldest_log['age_days']}일 경과)")
        
        print(f"\n설정된 최소 여유 공간: {self.min_free_gb / (1024**3):.1f} GB 또는 {self.min_free_percent}%")
        print(f"정리 필요: {'예' if self.need_cleanup() else '아니오'}")
        if self.need_cleanup() and record_files:
            print(f"우선 삭제 대상: RECORD 폴더 ({len(record_files)}개 파일)")
        print("==================\n")
    
    def monitor(self, interval=300):
        """주기적으로 모니터링 및 정리"""
        self.logger.info(f"모니터링 시작 (간격: {interval}초)")
        
        while True:
            try:
                self.show_status()
                self.cleanup()
            except KeyboardInterrupt:
                self.logger.info("모니터링 종료")
                break
            except Exception as e:
                self.logger.error(f"오류 발생: {e}")
            
            time.sleep(interval)


def main():
    parser = argparse.ArgumentParser(description='디스크 용량 관리 도구')
    parser.add_argument('--path', default='/home/nvidia/data', help='관리할 경로')
    parser.add_argument('--min-free-gb', type=float, default=50.0, help='최소 여유 공간 (GB)')
    parser.add_argument('--min-free-percent', type=float, default=20.0, help='최소 여유 공간 (%)')
    parser.add_argument('--log-path', default='/home/nvidia/webrtc/logs', help='로그 파일 경로')
    parser.add_argument('--log-retention', type=int, default=7, help='로그 보관 일수')
    parser.add_argument('--dry-run', action='store_true', help='실제로 삭제하지 않고 시뮬레이션')
    parser.add_argument('--monitor', action='store_true', help='계속 모니터링')
    parser.add_argument('--interval', type=int, default=300, help='모니터링 간격 (초)')
    parser.add_argument('--status', action='store_true', help='현재 상태만 표시')
    parser.add_argument('--logs-only', action='store_true', help='로그만 정리')
    
    args = parser.parse_args()
    
    cleaner = DiskCleaner(
        target_path=args.path,
        min_free_percent=args.min_free_percent,
        min_free_gb=args.min_free_gb,
        log_path=args.log_path,
        log_retention_days=args.log_retention
    )
    
    if args.status:
        # 상태만 표시
        cleaner.show_status()
    elif args.logs_only:
        # 로그만 정리
        cleaner.cleanup_logs(dry_run=args.dry_run)
    elif args.monitor:
        # 계속 모니터링
        cleaner.monitor(interval=args.interval)
    else:
        # 한 번만 실행
        cleaner.show_status()
        cleaner.cleanup(dry_run=args.dry_run)


if __name__ == "__main__":
    main()