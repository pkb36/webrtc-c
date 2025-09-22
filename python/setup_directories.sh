#!/bin/bash
# 카메라 녹화용 디렉토리 사전 생성 및 권한 설정 스크립트

# 색상 정의
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== 카메라 녹화 디렉토리 설정 ===${NC}"

# 기본 디렉토리 경로 (camera_config.json에서 읽기)
CONFIG_FILE="/nvmeroot/storage/Project/itech/webrtc/python/camera_config.json"
if [ -f "$CONFIG_FILE" ]; then
    DATA_DIR=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE'))['output_base_dir'])" 2>/dev/null)
fi

# 기본값 설정
if [ -z "$DATA_DIR" ]; then
    DATA_DIR="/home/nvidia/data"
    echo -e "${YELLOW}설정 파일을 읽을 수 없습니다. 기본 경로 사용: $DATA_DIR${NC}"
fi

echo -e "데이터 디렉토리: ${GREEN}$DATA_DIR${NC}"

# 디렉토리 생성
echo "1. 디렉토리 생성 중..."
sudo mkdir -p "$DATA_DIR"

# 오늘 날짜 디렉토리도 미리 생성
TODAY_DIR="$DATA_DIR/RECORD_$(date +%Y%m%d)"
sudo mkdir -p "$TODAY_DIR"

# nvidia 사용자 권한 설정
echo "2. 권한 설정 중..."
sudo chown -R nvidia:nvidia "$DATA_DIR"
sudo chmod -R 755 "$DATA_DIR"

# 결과 확인
echo -e "\n${GREEN}=== 설정 완료 ===${NC}"
ls -la "$DATA_DIR"

# 쓰기 테스트
echo -e "\n3. 쓰기 권한 테스트..."
if su - nvidia -c "touch $DATA_DIR/.write_test && rm $DATA_DIR/.write_test" 2>/dev/null; then
    echo -e "${GREEN}✓ 쓰기 권한 정상${NC}"
else
    echo -e "${RED}✗ 쓰기 권한 문제 발생${NC}"
    exit 1
fi

echo -e "\n${GREEN}모든 설정이 완료되었습니다!${NC}"
echo "카메라 서비스를 시작할 수 있습니다:"
echo "  sudo systemctl start camera.service"