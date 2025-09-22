# 🚨 개선된 Disk Manager - 루트 파일시스템 100% 방지

## ⚡ **핵심 문제 해결**

**문제**: 기존 disk_manager는 데이터 디스크(`/home/nvidia/data`)만 관리하여 **루트 파일시스템(`/`)이 100%가 되는 치명적 상황을 방지하지 못함**

**해결**: **루트 파일시스템 실시간 모니터링 + 자동 정리** 기능 추가

## 🔧 **새로 추가된 기능**

### ✅ **1. 루트 파일시스템 모니터링**
```python
# 실시간 루트 시스템 사용량 체크
def get_root_usage(self):
    stat = shutil.disk_usage("/")
    return {
        'total': stat.total,
        'used': stat.used, 
        'free': stat.free,
        'percent': (stat.used / stat.total) * 100
    }

def check_root_status(self):
    # 95% 이상: CRITICAL (긴급)
    # 85% 이상: WARNING (경고)
    # 85% 미만: OK (정상)
```

### ✅ **2. 단계별 자동 정리 시스템**
```
🟢 85% 미만: 정상 동작
🟡 85-95%: 경고 수준 정리 (7일 이상 로그 삭제)
🔴 95% 이상: 긴급 정리 (3일 이상 로그 삭제 + aggressive 모드)
```

### ✅ **3. 포괄적인 루트 시스템 정리**

#### **기본 정리 항목**
- **시스템 로그**: `/var/log/*.log*` (7일 → 3일)
- **임시 파일**: `/tmp`, `/var/tmp` (1일 이상)
- **패키지 캐시**: `apt-get clean`, `pip3 cache purge`
- **저널 로그**: `journalctl --vacuum-time=3d`
- **코어 덤프**: `/tmp/core*`, `/var/crash/*`

#### **Aggressive 모드 (95% 이상)**
- **시스템 로그**: 3일 이상 모두 삭제
- **저널 로그**: 1일치만 보관
- **오래된 커널**: `apt-get autoremove --purge`
- **썸네일 캐시**: `~/.cache/thumbnails` 정리

## 🚀 **사용법**

### **기본 실행 (루트 모니터링 포함)**
```bash
# 루트 + 데이터 디스크 전체 체크 및 정리
python3 disk_manager.py

# 상태만 확인
python3 disk_manager.py --status
```

### **루트 파일시스템 전용**
```bash
# 루트 시스템만 정리 (긴급 시)
python3 disk_manager.py --root-only

# 루트 임계값 조정 (기본: 경고85%, 위험95%)
python3 disk_manager.py --root-warning 80 --root-critical 90
```

### **연속 모니터링 (권장)**
```bash
# 5분마다 자동 체크 및 정리
python3 disk_manager.py --monitor --interval 300
```

## 📊 **화면 출력 예시**

### **정상 상태**
```
=== 디스크 상태 ===

🏠 루트 파일시스템 (/): ✅
   전체: 29.1 GB
   사용: 20.5 GB (70.4%)
   여유: 8.6 GB
   상태: OK (경고>85%, 위험>95%)

📁 데이터 디스크: /home/nvidia/data
   전체 용량: 915.7 GB
   사용 중: 234.2 GB (25.6%)
   여유 공간: 681.5 GB
```

### **위험 상태**
```
=== 디스크 상태 ===

🏠 루트 파일시스템 (/): 🚨
   전체: 29.1 GB
   사용: 28.2 GB (96.9%)
   여유: 0.9 GB
   상태: CRITICAL (경고>85%, 위험>95%)
```

## 🛡️ **자동 정리 로그 예시**

### **긴급 정리 실행**
```
2024-08-28 15:30:15 - CRITICAL - 🚨 루트 파일시스템이 위험 수준! 긴급 정리 시작
2024-08-28 15:30:15 - WARNING - 🚨 루트 파일시스템 정리 시작
2024-08-28 15:30:16 - INFO - 시스템 로그 삭제: /var/log/syslog.2.gz (2.3MB)
2024-08-28 15:30:16 - INFO - 시스템 로그 삭제: /var/log/kern.log.3.gz (1.8MB)
2024-08-28 15:30:17 - INFO - 임시 파일 정리: 45.2MB
2024-08-28 15:30:18 - INFO - APT 캐시 정리 완료
2024-08-28 15:30:19 - INFO - systemd journal 로그 정리 (1일치 보관)
2024-08-28 15:30:20 - INFO - 오래된 커널 패키지 자동 정리 완료
2024-08-28 15:30:20 - WARNING - 🧹 루트 파일시스템 정리 완료: 382.4MB 확보
```

## 🎯 **핵심 개선 사항**

### **이전 문제점**
❌ 루트 시스템 모니터링 없음  
❌ 시스템 로그가 계속 쌓임  
❌ 임시 파일 정리 안됨  
❌ 100% 도달 시 시스템 불안정

### **현재 해결책**
✅ **실시간 루트 모니터링** (95% 위험, 85% 경고)  
✅ **자동 시스템 로그 정리** (7일 → 3일 → 1일)  
✅ **포괄적 임시파일 정리** (/tmp, /var/tmp, 캐시)  
✅ **예방적 시스템 안정화** (100% 도달 방지)

## 🔄 **자동화 설정 권장**

### **Crontab 설정**
```bash
# 매 10분마다 디스크 체크 및 필요 시 정리
*/10 * * * * cd /nvmeroot/Project/itech/webrtc && python3 python/disk_manager.py

# 매일 새벽 3시 전체 정리
0 3 * * * cd /nvmeroot/Project/itech/webrtc && python3 python/disk_manager.py --root-only
```

### **Systemd Service**
```bash
# 지속적 모니터링 서비스 (5분 간격)
python3 disk_manager.py --monitor --interval 300
```

## 📈 **기대 효과**

| 항목 | 이전 | 개선 후 |
|------|------|---------|
| 루트 시스템 모니터링 | ❌ 없음 | ✅ 실시간 |
| 시스템 불안정 위험 | 🔴 높음 | 🟢 낮음 |
| 로그 파일 누적 | 🔴 무제한 | 🟡 자동 정리 |
| 임시 파일 정리 | ❌ 수동 | ✅ 자동 |
| 응급 상황 대응 | ❌ 없음 | ✅ 긴급 정리 |

## 🚨 **긴급 상황 시 수동 대응**

### **루트 시스템 99% 이상**
```bash
# 즉시 긴급 정리 실행
sudo python3 disk_manager.py --root-only

# 수동 추가 정리
sudo journalctl --vacuum-time=1d
sudo apt-get clean
sudo apt-get autoremove --purge -y
sudo find /tmp -type f -mtime +0 -delete
```

### **응급 로그 확인**
```bash
# 가장 큰 로그 파일 찾기
sudo du -h /var/log/* | sort -hr | head -10

# 큰 파일 수동 삭제
sudo rm /var/log/kern.log.*
sudo rm /var/log/syslog.*
```

## 🎉 **결론**

✅ **루트 파일시스템 100% 도달 방지** - 시스템 안정성 크게 향상  
✅ **자동화된 예방 시스템** - 수동 개입 최소화  
✅ **포괄적인 정리 기능** - 로그, 캐시, 임시파일 모두 관리  
✅ **단계적 대응 시스템** - 상황에 맞는 적절한 정리 수준

이제 disk_manager가 **루트 파일시스템이 꽉 차는 치명적 상황을 확실히 방지**할 수 있습니다! 🛡️