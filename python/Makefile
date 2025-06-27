# Makefile for Auto OSD Monitor

CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -g -pthread

# OpenCV 설정
OPENCV_FLAGS = $(shell pkg-config --cflags opencv4)
OPENCV_LIBS = $(shell pkg-config --libs opencv4)

# jsoncpp 설정
JSON_FLAGS = -I/usr/include/jsoncpp
JSON_LIBS = -ljsoncpp

# filesystem 라이브러리
FS_LIBS = -lstdc++fs

# 전체 플래그
INCLUDES = $(OPENCV_FLAGS) $(JSON_FLAGS)
LIBS = $(OPENCV_LIBS) $(JSON_LIBS) $(FS_LIBS) -pthread

# 타겟
TARGET = event_osd_maker

# 소스
SOURCES = event_osd_maker.cpp

# 빌드
all: $(TARGET)

$(TARGET): $(SOURCES)
	@echo "자동 OSD 모니터 컴파일 중..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $@ $(LIBS)
	@echo "컴파일 완료!"

# 실행
run: $(TARGET)
	./$(TARGET) /home/nvidia/data

# 데몬으로 실행
daemon: $(TARGET)
	nohup ./$(TARGET) /home/nvidia/data > auto_osd.log 2>&1 &
	@echo "자동 OSD 모니터가 백그라운드에서 실행됩니다."
	@echo "로그 확인: tail -f auto_osd.log"

# 테스트 (다른 경로)
test: $(TARGET)
	./$(TARGET) ./test_data

# 프로세스 확인
status:
	@ps aux | grep event_osd_maker | grep -v grep || echo "실행 중이 아닙니다."

# 프로세스 종료
stop:
	@pkill -f event_osd_maker || echo "실행 중인 프로세스가 없습니다."

# systemd 서비스 파일 생성
service:
	@echo "[Unit]" > auto_osd.service
	@echo "Description=Auto OSD Monitor Service" >> auto_osd.service
	@echo "After=network.target" >> auto_osd.service
	@echo "" >> auto_osd.service
	@echo "[Service]" >> auto_osd.service
	@echo "Type=simple" >> auto_osd.service
	@echo "User=nvidia" >> auto_osd.service
	@echo "WorkingDirectory=/home/nvidia" >> auto_osd.service
	@echo "ExecStart=$(shell pwd)/$(TARGET) /home/nvidia/data" >> auto_osd.service
	@echo "Restart=always" >> auto_osd.service
	@echo "" >> auto_osd.service
	@echo "[Install]" >> auto_osd.service
	@echo "WantedBy=multi-user.target" >> auto_osd.service
	@echo "서비스 파일이 생성되었습니다: auto_osd.service"
	@echo "설치: sudo cp auto_osd.service /etc/systemd/system/"
	@echo "활성화: sudo systemctl enable auto_osd.service"
	@echo "시작: sudo systemctl start auto_osd.service"

# 정리
clean:
	rm -f $(TARGET) *.o auto_osd.log

# 로그 보기
log:
	tail -f auto_osd.log

.PHONY: all run daemon test status stop service clean log