CC	:= gcc
LIBS   := $(shell pkg-config --libs --cflags glib-2.0 gstreamer-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0 json-glib-1.0 libsoup-2.4 libcurl)

CFLAGS := -O0 -ggdb -Wall -fno-omit-frame-pointer -I/opt/nvidia/deepstream/deepstream/sources/includes \
		$(shell pkg-config --cflags glib-2.0 gstreamer-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0 json-glib-1.0 libsoup-2.4)

NVDS_VERSION:=6.2
LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/
LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lm -Wl,-rpath,$(LIB_INSTALL_DIR)

# 빌드 디렉토리 설정
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# 디렉토리 생성
$(shell mkdir -p $(BUILD_DIR) $(OBJ_DIR))

# 오브젝트 파일들
COMMON_OBJS := $(OBJ_DIR)/log.o $(OBJ_DIR)/log_wrapper.o
GSTREAM_OBJS := $(OBJ_DIR)/gstream_main.o $(OBJ_DIR)/config.o $(OBJ_DIR)/serial_comm.o $(OBJ_DIR)/socket_comm.o \
                $(OBJ_DIR)/webrtc_peer.o $(OBJ_DIR)/process_cmd.o $(OBJ_DIR)/json_utils.o $(OBJ_DIR)/command_handler.o \
                $(OBJ_DIR)/gstream_control.o $(OBJ_DIR)/curllib.o $(OBJ_DIR)/device_setting.o $(OBJ_DIR)/nvds_process.o \
                $(OBJ_DIR)/nvds_utils.o $(OBJ_DIR)/event_recorder.o $(OBJ_DIR)/ptz_control.o $(OBJ_DIR)/video_convert.o

# 최종 실행파일들
TARGETS := $(BUILD_DIR)/gstream_main $(BUILD_DIR)/webrtc_sender $(BUILD_DIR)/webrtc_recorder \
           $(BUILD_DIR)/webrtc_event_recorder $(BUILD_DIR)/disk_check

# 기본 타겟
all: $(TARGETS)

# 공통 오브젝트 파일 컴파일 규칙
$(OBJ_DIR)/%.o: %.c
	"$(CC)" $(CFLAGS) -c $< -o $@

# gstream_main 전용 오브젝트 (PTZ 지원)
$(OBJ_DIR)/gstream_main.o: gstream_main.c
	"$(CC)" $(CFLAGS) -DPTZ_SUPPORT -c $< -o $@

# 실행파일 빌드 규칙
$(BUILD_DIR)/gstream_main: $(GSTREAM_OBJS) $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) -DPTZ_SUPPORT $^ $(LIBS) -o $@

$(BUILD_DIR)/webrtc_sender: $(OBJ_DIR)/webrtc_sender.o $(OBJ_DIR)/socket_comm.o $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/webrtc_recorder: $(OBJ_DIR)/webrtc_recorder.o $(OBJ_DIR)/video_convert.o $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/webrtc_event_recorder: $(OBJ_DIR)/webrtc_event_recorder.o $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/disk_check: $(OBJ_DIR)/disk_check.o $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

# 테스트 프로그램들 (선택사항)
$(BUILD_DIR)/json_test: $(OBJ_DIR)/json_test.o
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/curllib_test: $(OBJ_DIR)/curllib_test.o $(OBJ_DIR)/curllib.o $(OBJ_DIR)/json_utils.o $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/settting_test: $(OBJ_DIR)/device_setting.o $(OBJ_DIR)/serial_comm.o
	"$(CC)" $(CFLAGS) -DTEST_SETTING $^ $(LIBS) -o $@

$(BUILD_DIR)/log_test: $(COMMON_OBJS)
	"$(CC)" $(CFLAGS) -DTEST_LOG $^ -o $@

# 설치 (기존 위치로 복사)
install: $(TARGETS)
	cp $(BUILD_DIR)/gstream_main ./
	cp $(BUILD_DIR)/webrtc_sender ./
	cp $(BUILD_DIR)/webrtc_recorder ./
	cp $(BUILD_DIR)/webrtc_event_recorder ./
	cp $(BUILD_DIR)/disk_check ./

# 정리
clean:
	rm -rf $(BUILD_DIR)
	rm -f gstream_main webrtc_sender webrtc_recorder webrtc_event_recorder disk_check

# 의존성 관리 (옵션)
.PHONY: all clean install