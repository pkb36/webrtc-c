cmake_minimum_required(VERSION 3.10)
project(cow_monitoring_system C)

# 컴파일 옵션 설정
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -ggdb -Wall -fno-omit-frame-pointer")

# DeepStream 버전 및 라이브러리 경로 설정
set(NVDS_VERSION 6.2)
set(DEEPSTREAM_DIR /opt/nvidia/deepstream/deepstream-${NVDS_VERSION})
set(LIB_INSTALL_DIR ${DEEPSTREAM_DIR}/lib)

# include 디렉토리 설정
include_directories(
    ${DEEPSTREAM_DIR}/sources/includes
    .
)

# pkg-config로 필요한 플래그 가져오기
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED 
    glib-2.0 
    gstreamer-1.0 
    gstreamer-sdp-1.0 
    gstreamer-webrtc-1.0 
    json-glib-1.0 
    libsoup-2.4 
    libcurl
)

include_directories(${DEPS_INCLUDE_DIRS})
link_directories(${DEPS_LIBRARY_DIRS})
link_directories(${LIB_INSTALL_DIR})
add_definitions(${DEPS_CFLAGS_OTHER})

# 공통 링크 라이브러리
set(COMMON_LIBS 
    ${DEPS_LIBRARIES}
    -lnvdsgst_meta 
    -lnvds_meta 
    -lm
    pthread
)

# # WebRTC 송신기
# add_executable(webrtc_sender 
#     webrtc_sender.c 
#     socket_comm.c 
#     g_log.c 
#     json_utils.c
# )
# target_link_libraries(webrtc_sender ${COMMON_LIBS})

# # WebRTC 레코더
# add_executable(webrtc_recorder 
#     webrtc_recorder.c 
#     video_convert.c 
#     g_log.c
# )
# target_link_libraries(webrtc_recorder ${COMMON_LIBS})

# # WebRTC 이벤트 레코더
# add_executable(webrtc_event_recorder 
#     webrtc_event_recorder.c 
#     g_log.c
# )
# target_link_libraries(webrtc_event_recorder ${COMMON_LIBS})

# # 디스크 체크 유틸리티
# add_executable(disk_check 
#     disk_check.c 
#     g_log.c
# )
# target_link_libraries(disk_check ${COMMON_LIBS})

# 메인 애플리케이션 - 마지막에 빌드하여 다른 모듈에서 필요한 심볼이 이미 정의되도록 함
add_executable(gstream_main 
    gstream_main.c 
    config.c 
    webrtc_peer.c 
    process_cmd.c 
    gstream_control.c 
    curllib.c 
    device_setting.c 
    nvds_process.c 
    nvds_utils.c 
    event_recorder.c 
    ptz_control.c 
    video_convert.c
    g_log.c
    serial_comm.c
    socket_comm.c
    json_utils.c
)
target_link_libraries(gstream_main ${COMMON_LIBS})

# 설치 대상 설정
# install(TARGETS gstream_main webrtc_sender webrtc_recorder webrtc_event_recorder disk_check
#         RUNTIME DESTINATION bin)
install(TARGETS gstream_main RUNTIME DESTINATION bin)


# 설치 스크립트
install(FILES 
        video_convert.sh
        record_event_buffer.sh
        factory_default.sh
        stop.sh
        DESTINATION bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# 설정 파일 설치
install(FILES 
        config.json
        device_setting.json
        DESTINATION etc/cow_monitoring
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# 로그 디렉토리 생성
install(DIRECTORY DESTINATION var/log/cow_monitoring
        DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE 
                             GROUP_READ GROUP_WRITE GROUP_EXECUTE
                             WORLD_READ WORLD_WRITE WORLD_EXECUTE)