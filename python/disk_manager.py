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
    def __init__(self, target_path="/home/nvidia/data", min_free_percent=5.0, min_free_gb=2.0,
                 log_path="/home/nvidia/webrtc/logs", log_retention_days=7, 
                 monitor_root=True, root_critical_percent=95.0, root_warning_percent=85.0):
        """
        디스크 정리기 초기화
        
        Args:
            target_path: 모니터링할 경로
            min_free_percent: 최소 여유 공간 퍼센트
            min_free_gb: 최소 여유 공간 GB
            log_path: 로그 파일 경로
            log_retention_days: 로그 보관 일수
            monitor_root: 루트 파일시스템 모니터링 여부
            root_critical_percent: 루트 시스템 위험 임계값 (%)
            root_warning_percent: 루트 시스템 경고 임계값 (%)
        """
        self.target_path = Path(target_path)
        self.min_free_percent = min_free_percent
        self.min_free_gb = min_free_gb * 1024 * 1024 * 1024  # GB to bytes
        self.log_path = Path(log_path)
        self.log_retention_days = log_retention_days
        
        # 루트 파일시스템 모니터링 설정
        self.monitor_root = monitor_root
        self.root_critical_percent = root_critical_percent
        self.root_warning_percent = root_warning_percent
        
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
        """오래된 파일 찾기 (RECORD 폴더 우선) - I/O 오류 처리 추가"""
        record_files = []
        event_files = []
        failed_paths = []
        
        for ext in extensions:
            try:
                # rglob 대신 안전한 재귀 검색 사용
                for file_info in self._safe_recursive_search(self.target_path, f'*{ext}'):
                    if file_info is None:
                        continue
                        
                    # 파일이 속한 폴더 확인
                    parent_folder = file_info['path'].parent.name
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
                        
            except Exception as e:
                self.logger.error(f"파일 검색 중 오류 ({ext}): {e}")
                continue
        
        if failed_paths:
            self.logger.warning(f"접근 실패한 경로: {len(failed_paths)}개")
        
        # 각각 날짜 순으로 정렬 (오래된 것부터)
        record_files.sort(key=lambda x: x['mtime'])
        event_files.sort(key=lambda x: x['mtime'])
        
        # RECORD 파일을 먼저, 그 다음 EVENT 파일
        return record_files + event_files
    
    def _safe_recursive_search(self, path, pattern):
        """안전한 재귀 파일 검색"""
        try:
            # 현재 디렉토리의 파일들 검사
            try:
                entries = list(os.scandir(path))
            except OSError as e:
                if e.errno == 5:  # Input/output error
                    self.logger.warning(f"I/O 오류로 디렉토리 스캔 실패: {path}")
                    return
                else:
                    self.logger.warning(f"디렉토리 접근 실패: {path} - {e}")
                    return
            
            for entry in entries:
                try:
                    if entry.is_file():
                        file_path = Path(entry.path)
                        if file_path.match(pattern):
                            try:
                                stat = file_path.stat()
                                yield {
                                    'path': file_path,
                                    'size': stat.st_size,
                                    'mtime': datetime.fromtimestamp(stat.st_mtime)
                                }
                            except OSError as e:
                                if e.errno == 5:
                                    self.logger.warning(f"I/O 오류로 파일 정보 읽기 실패: {file_path}")
                                else:
                                    self.logger.warning(f"파일 정보 읽기 실패: {file_path} - {e}")
                                continue
                                
                    elif entry.is_dir():
                        # 재귀적으로 하위 디렉토리 검색
                        yield from self._safe_recursive_search(Path(entry.path), pattern)
                        
                except OSError as e:
                    if e.errno == 5:
                        self.logger.warning(f"I/O 오류로 항목 접근 실패: {entry.path}")
                    else:
                        self.logger.warning(f"항목 접근 실패: {entry.path} - {e}")
                    continue
                    
        except Exception as e:
            self.logger.error(f"재귀 검색 중 예상치 못한 오류: {path} - {e}")
    
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
    
    def get_root_usage(self):
        """루트 파일시스템 사용량 정보 반환"""
        try:
            stat = shutil.disk_usage("/")
            return {
                'total': stat.total,
                'used': stat.used,
                'free': stat.free,
                'percent': (stat.used / stat.total) * 100
            }
        except Exception as e:
            self.logger.error(f"루트 파일시스템 사용량 조회 실패: {e}")
            return None
    
    def check_root_status(self):
        """루트 파일시스템 상태 체크"""
        if not self.monitor_root:
            return "disabled"
            
        usage = self.get_root_usage()
        if not usage:
            return "error"
            
        if usage['percent'] >= self.root_critical_percent:
            return "critical"
        elif usage['percent'] >= self.root_warning_percent:
            return "warning"
        else:
            return "ok"
    
    def cleanup_root_filesystem(self, aggressive=False):
        """루트 파일시스템 정리"""
        self.logger.warning("🚨 루트 파일시스템 정리 시작")
        
        cleaned_size = 0
        
        # 1. 시스템 로그 정리
        cleaned_size += self.cleanup_system_logs(aggressive)
        
        # 2. 임시 파일 정리
        cleaned_size += self.cleanup_temp_files()
        
        # 3. 패키지 캐시 정리
        cleaned_size += self.cleanup_package_cache()
        
        # 4. 저널 로그 정리
        cleaned_size += self.cleanup_journal_logs(aggressive)
        
        # 5. 코어 덤프 정리
        cleaned_size += self.cleanup_core_dumps()
        
        # 6. aggressive 모드에서는 추가 정리
        if aggressive:
            cleaned_size += self.cleanup_old_kernels()
            cleaned_size += self.cleanup_thumbnails()
        
        self.logger.warning(f"🧹 루트 파일시스템 정리 완료: {cleaned_size / (1024**2):.1f}MB 확보")
        return cleaned_size
    
    def cleanup_system_logs(self, aggressive=False):
        """시스템 로그 정리"""
        cleaned_size = 0
        
        # /var/log 정리
        log_paths = [
            "/var/log/*.log",
            "/var/log/*.log.*",
            "/var/log/kern.log*",
            "/var/log/syslog*",
            "/var/log/auth.log*",
            "/var/log/daemon.log*",
        ]
        
        cutoff_days = 3 if aggressive else 7
        cutoff_time = time.time() - (cutoff_days * 24 * 60 * 60)
        
        import glob
        for pattern in log_paths:
            try:
                for file_path in glob.glob(pattern):
                    if os.path.isfile(file_path):
                        stat = os.stat(file_path)
                        if stat.st_mtime < cutoff_time:
                            try:
                                size = stat.st_size
                                os.remove(file_path)
                                cleaned_size += size
                                self.logger.info(f"시스템 로그 삭제: {file_path} ({size/1024:.1f}KB)")
                            except Exception as e:
                                self.logger.warning(f"로그 삭제 실패: {file_path} - {e}")
            except Exception as e:
                self.logger.warning(f"로그 패턴 검색 실패 ({pattern}): {e}")
        
        return cleaned_size
    
    def cleanup_temp_files(self):
        """임시 파일 정리"""
        cleaned_size = 0
        
        temp_dirs = ["/tmp", "/var/tmp"]
        cutoff_time = time.time() - (24 * 60 * 60)  # 1일
        
        for temp_dir in temp_dirs:
            try:
                if not os.path.exists(temp_dir):
                    continue
                    
                for root, dirs, files in os.walk(temp_dir):
                    for file_name in files:
                        file_path = os.path.join(root, file_name)
                        try:
                            stat = os.stat(file_path)
                            if stat.st_mtime < cutoff_time:
                                size = stat.st_size
                                os.remove(file_path)
                                cleaned_size += size
                        except Exception as e:
                            continue  # 임시파일은 에러 무시
                            
            except Exception as e:
                self.logger.warning(f"임시 파일 정리 실패 ({temp_dir}): {e}")
        
        if cleaned_size > 0:
            self.logger.info(f"임시 파일 정리: {cleaned_size/1024/1024:.1f}MB")
        
        return cleaned_size
    
    def cleanup_package_cache(self):
        """패키지 캐시 정리"""
        cleaned_size = 0
        
        try:
            # APT 캐시 정리
            result = os.system("apt-get clean >/dev/null 2>&1")
            if result == 0:
                self.logger.info("APT 캐시 정리 완료")
                cleaned_size += 50 * 1024 * 1024  # 추정값
                
            # pip 캐시 정리
            result = os.system("pip3 cache purge >/dev/null 2>&1")
            if result == 0:
                self.logger.info("pip 캐시 정리 완료")
                
        except Exception as e:
            self.logger.warning(f"패키지 캐시 정리 실패: {e}")
            
        return cleaned_size
    
    def cleanup_journal_logs(self, aggressive=False):
        """systemd journal 로그 정리"""
        cleaned_size = 0
        
        try:
            if aggressive:
                # 1일치만 보관
                result = os.system("journalctl --vacuum-time=1d >/dev/null 2>&1")
                if result == 0:
                    self.logger.info("systemd journal 로그 정리 (1일치 보관)")
                    cleaned_size += 100 * 1024 * 1024  # 추정값
            else:
                # 3일치 보관
                result = os.system("journalctl --vacuum-time=3d >/dev/null 2>&1")
                if result == 0:
                    self.logger.info("systemd journal 로그 정리 (3일치 보관)")
                    cleaned_size += 50 * 1024 * 1024  # 추정값
                    
        except Exception as e:
            self.logger.warning(f"journal 로그 정리 실패: {e}")
            
        return cleaned_size
    
    def cleanup_core_dumps(self):
        """코어 덤프 파일 정리"""
        cleaned_size = 0
        
        core_patterns = ["/tmp/core*", "/var/crash/*", "/home/*/core*"]
        
        import glob
        for pattern in core_patterns:
            try:
                for file_path in glob.glob(pattern):
                    if os.path.isfile(file_path):
                        try:
                            size = os.path.getsize(file_path)
                            os.remove(file_path)
                            cleaned_size += size
                            self.logger.info(f"코어 덤프 삭제: {file_path} ({size/1024/1024:.1f}MB)")
                        except Exception as e:
                            self.logger.warning(f"코어 덤프 삭제 실패: {file_path} - {e}")
            except Exception as e:
                continue
                
        return cleaned_size
    
    def cleanup_old_kernels(self):
        """오래된 커널 패키지 정리 (aggressive 모드)"""
        cleaned_size = 0
        
        try:
            # 현재 커널 버전 확인
            import subprocess
            current_kernel = subprocess.check_output("uname -r", shell=True).decode().strip()
            self.logger.info(f"현재 커널: {current_kernel}")
            
            # 오래된 커널 패키지 정리 (자동으로 안전한 것만)
            result = os.system("apt-get autoremove --purge -y >/dev/null 2>&1")
            if result == 0:
                self.logger.info("오래된 커널 패키지 자동 정리 완료")
                cleaned_size += 200 * 1024 * 1024  # 추정값
                
        except Exception as e:
            self.logger.warning(f"커널 정리 실패: {e}")
            
        return cleaned_size
    
    def cleanup_thumbnails(self):
        """썸네일 캐시 정리"""
        cleaned_size = 0
        
        thumbnail_dirs = [
            "/home/*/.cache/thumbnails",
            "/home/*/.thumbnails"
        ]
        
        import glob
        for pattern in thumbnail_dirs:
            try:
                for thumb_dir in glob.glob(pattern):
                    if os.path.isdir(thumb_dir):
                        for root, dirs, files in os.walk(thumb_dir):
                            for file_name in files:
                                file_path = os.path.join(root, file_name)
                                try:
                                    size = os.path.getsize(file_path)
                                    os.remove(file_path)
                                    cleaned_size += size
                                except Exception:
                                    continue
            except Exception:
                continue
                
        if cleaned_size > 0:
            self.logger.info(f"썸네일 캐시 정리: {cleaned_size/1024/1024:.1f}MB")
            
        return cleaned_size
    
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
        """디스크 정리 실행 (비디오 + 로그 + 루트 시스템)"""
        # 🚨 루트 파일시스템 긴급 체크 및 정리
        root_status = self.check_root_status()
        if root_status == "critical":
            self.logger.critical("🚨 루트 파일시스템이 위험 수준! 긴급 정리 시작")
            if not dry_run:
                self.cleanup_root_filesystem(aggressive=True)
        elif root_status == "warning":
            self.logger.warning("⚠️ 루트 파일시스템 경고 수준 - 정리 시작")
            if not dry_run:
                self.cleanup_root_filesystem(aggressive=False)
        
        # 프로젝트 로그 정리
        self.cleanup_logs(dry_run=dry_run)
        
        # 데이터 디스크 비디오 파일 정리
        if not self.need_cleanup():
            self.logger.info("데이터 디스크 정리는 필요하지 않습니다.")
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
        try:
            usage = self.get_disk_usage()
        except Exception as e:
            print(f"\n디스크 사용량 조회 실패: {e}")
            return
        
        try:
            files = self.find_old_files()
        except Exception as e:
            print(f"\n파일 목록 조회 실패: {e}")
            print("I/O 오류가 발생했습니다. 하드웨어 점검이 필요할 수 있습니다.")
            return
        
        # 타입별로 분류
        record_files = [f for f in files if f.get('type') == 'RECORD']
        event_files = [f for f in files if f.get('type') == 'EVENT']
        other_files = [f for f in files if f.get('type') == 'OTHER']
        
        total_file_size = sum(f['size'] for f in files)
        record_size = sum(f['size'] for f in record_files)
        event_size = sum(f['size'] for f in event_files)
        
        print("\n=== 디스크 상태 ===")
        
        # 루트 파일시스템 상태
        root_usage = self.get_root_usage()
        root_status = self.check_root_status()
        
        if root_usage and self.monitor_root:
            status_emoji = "🚨" if root_status == "critical" else "⚠️" if root_status == "warning" else "✅"
            print(f"🏠 루트 파일시스템 (/): {status_emoji}")
            print(f"   전체: {root_usage['total'] / (1024**3):.1f} GB")
            print(f"   사용: {root_usage['used'] / (1024**3):.1f} GB ({root_usage['percent']:.1f}%)")
            print(f"   여유: {root_usage['free'] / (1024**3):.1f} GB")
            print(f"   상태: {root_status.upper()} (경고>{self.root_warning_percent}%, 위험>{self.root_critical_percent}%)")
        
        print(f"\n📁 데이터 디스크: {self.target_path}")
        print(f"   전체 용량: {usage['total'] / (1024**3):.1f} GB")
        print(f"   사용 중: {usage['used'] / (1024**3):.1f} GB ({usage['percent']:.1f}%)")
        print(f"   여유 공간: {usage['free'] / (1024**3):.1f} GB")
        
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
                self.move_emmc_to_nvme()
                self.show_status()
                self.cleanup()
            except KeyboardInterrupt:
                self.logger.info("모니터링 종료")
                break
            except Exception as e:
                self.logger.error(f"오류 발생: {e}")
            
            time.sleep(interval)

    def move_emmc_to_nvme(self, emmc_dir="/home/nvidia/videofile", nvme_base="/home/nvidia/data", min_age_minutes=5):
        """
        eMMC → NVMe로 mp4 파일 이동
        """
        emmc_dir = Path(emmc_dir)
        if not emmc_dir.exists():
            self.logger.warning(f"eMMC 디렉토리 없음: {emmc_dir}")
            return

        if not self.is_nvme_mounted(nvme_base):
            self.logger.warning(f"NVMe 마운트 안 됨: {nvme_base} → 이동 생략")
            return

        now = time.time()
        moved_count = 0
        moved_size = 0

        for file in emmc_dir.rglob("*.mp4"):
            try:
                stat = file.stat()
                mtime = stat.st_mtime

                if now - mtime < min_age_minutes * 60:
                    continue  # 아직 쓰고 있는 파일일 가능성 있음

                # RECORD_YYYYMMDD 디렉토리 생성
                date_str = datetime.fromtimestamp(mtime).strftime("RECORD_%Y%m%d")
                target_dir = Path(nvme_base) / date_str
                target_dir.mkdir(parents=True, exist_ok=True)
                shutil.move(str(file), target_dir / file.name)

                # 소유권 보정
                os.system(f"chown nvidia:nvidia {target_dir}/{file.name}")
                moved_count += 1
                moved_size += stat.st_size

            except Exception as e:
                self.logger.error(f"파일 이동 실패: {file} - {e}")

        self.logger.info(f"eMMC → NVMe 이동 완료: {moved_count}개 파일, {moved_size / (1024*1024):.1f}MB 이동됨")

    def is_nvme_mounted(self, mount_point="/home/nvidia/data"):
        """NVMe 마운트 여부 확인"""
        with open("/proc/mounts", "r") as f:
            for line in f:
                if mount_point in line:
                    return True
        return False

def main():
    parser = argparse.ArgumentParser(description='디스크 용량 관리 도구')
    parser.add_argument('--path', default='/home/nvidia/data', help='관리할 경로')
    parser.add_argument('--min-free-gb', type=float, default=2.0, help='최소 여유 공간 (GB)')
    parser.add_argument('--min-free-percent', type=float, default=2.0, help='최소 여유 공간 (%)')
    parser.add_argument('--log-path', default='/home/nvidia/webrtc/logs', help='로그 파일 경로')
    parser.add_argument('--log-retention', type=int, default=7, help='로그 보관 일수')
    parser.add_argument('--dry-run', action='store_true', help='실제로 삭제하지 않고 시뮬레이션')
    parser.add_argument('--monitor', action='store_true', help='계속 모니터링')
    parser.add_argument('--interval', type=int, default=300, help='모니터링 간격 (초)')
    parser.add_argument('--status', action='store_true', help='현재 상태만 표시')
    parser.add_argument('--logs-only', action='store_true', help='로그만 정리')
    parser.add_argument('--move-emmc', action='store_true', help='eMMC에서 NVMe로 mp4 이동')
    parser.add_argument('--emmc-path', default='/home/nvidia/videofile', help='eMMC 영상 경로')
    parser.add_argument('--no-root-monitor', action='store_true', help='루트 파일시스템 모니터링 비활성화')
    parser.add_argument('--root-critical', type=float, default=95.0, help='루트 시스템 위험 임계값 (%)')
    parser.add_argument('--root-warning', type=float, default=85.0, help='루트 시스템 경고 임계값 (%)')
    parser.add_argument('--root-only', action='store_true', help='루트 파일시스템만 정리')

    args = parser.parse_args()
    
    cleaner = DiskCleaner(
        target_path=args.path,
        min_free_percent=args.min_free_percent,
        min_free_gb=args.min_free_gb,
        log_path=args.log_path,
        log_retention_days=args.log_retention,
        monitor_root=not args.no_root_monitor,
        root_critical_percent=args.root_critical,
        root_warning_percent=args.root_warning
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
    elif args.move_emmc:
        cleaner.move_emmc_to_nvme(emmc_dir=args.emmc_path)
    elif args.root_only:
        # 루트 파일시스템만 정리
        cleaner.show_status()
        root_status = cleaner.check_root_status()
        if root_status in ["warning", "critical"]:
            cleaner.cleanup_root_filesystem(aggressive=(root_status == "critical"))
        else:
            print("루트 파일시스템 정리가 필요하지 않습니다.")
    else:
        # 한 번만 실행
        cleaner.show_status()
        cleaner.cleanup(dry_run=args.dry_run)


if __name__ == "__main__":
    main()