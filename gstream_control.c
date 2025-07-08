/*
 * Demo gstreamer app for negotiating and streaming a sendrecv audio-only webrtc
 * stream to all the peers in a multiparty room.
 *
 * gcc mp-webrtc-sendrecv.c $(pkg-config --cflags --libs gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0) -o mp-webrtc-sendrecv
 *
 * Author: Nirbheek Chauhan <nirbheek@centricular.com>
 */
#include <gst/gst.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>
#include <stdio.h>
#include "config.h"
#include "gstream_main.h"
#include "serial_comm.h"
#include "device_setting.h"

#define USE_JSON_MESSAGE_TEMPLATE
#include "json_utils.h"
#include "curllib.h"
#include "webrtc_peer.h"
#include "nvds_process.h"
#include "ptz_control.h"
#include "tegrastats_monitor.h"
#include "log_wrapper.h"

extern WebRTCConfig g_config;
extern CurlIinfoType g_curlinfo;
extern GstElement *g_pipeline;
extern DeviceSetting g_setting;
extern int g_wait_reply_cnt;
extern enum AppState g_app_state;
extern pthread_mutex_t g_send_info_mutex;
extern pthread_mutex_t g_retry_connect_mutex;

extern void terminate_program();

static int g_tegrastats_interval = 12; // 5초 * 12 = 60초

#if MINDULE_INCLUDE
#define RANCH_SETTING_FILE "ranch_setting.json"
extern RanchSetting g_ranch_setting;
extern int g_frame_count[];

void get_ranch_setting_path(char *fname);
#endif

static pthread_t g_heartbit_tid = 0;
static int g_heartbit_timeout = 5000;
static gboolean g_firsttime_call = FALSE;

void send_ptz_serial_data(const gchar *str)
{
    glog_trace("Received PTZ Data: %s \n", str);

    char *str_temp = strdup(str);
    unsigned char data[32];
    char *ptr = strtok(str_temp, ",");
    int len = 0;
    while (ptr != NULL)
    {
        data[len++] = hex_str2val(ptr);
        ptr = strtok(NULL, ",");
    }

    if (is_open_serial())
    {
        write_serial(data, len);
    }

    free(str_temp);
}

unsigned char *read_jpeg(const char *file_path, int *jpeg_size)
{
    // 파일 열기
    FILE *file = fopen(file_path, "rb");
    if (file == NULL)
    {
        glog_error("파일을 열 수 없습니다.\n");
        return NULL;
    }

    // 파일 크기 확인
    fseek(file, 0, SEEK_END);     // 파일 끝으로 이동
    long file_size = ftell(file); // 현재 파일 위치 가져오기
    fseek(file, 0, SEEK_SET);     // 파일 시작으로 이동

    if (file_size < 0)
    {
        glog_error("파일 크기를 확인할 수 없습니다.\n");
        fclose(file);
        return NULL;
    }

    // 파일 크기만큼의 메모리 동적 할당
    unsigned char *buffer = (unsigned char *)malloc(file_size);
    if (buffer == NULL)
    {
        glog_error("메모리 할당 오류.\n");
        fclose(file);
        return NULL;
    }

    // 파일 내용 읽어 들이기
    size_t bytes_read = fread(buffer, 1, file_size, file);
    if (bytes_read != (size_t)file_size)
    {
        glog_error("파일 읽기 오류.\n");
        fclose(file);
        free(buffer);
        return NULL;
    }

    *jpeg_size = file_size;
    fclose(file);
    return buffer;
}

gchar *image_to_base64(const gchar *source)
{
    char jpegpath[512];

    int index = 0;
    if (strcmp(source, "RGB") != 0)
    {
        index = 1;
    }

    // glog_trace("image_to_base64 source=%s, index=%d\n", g_config.snapshot_path, index);

    printf("image_to_base64 source=%s, index=%d\n", g_config.snapshot_path, index);

    sprintf(jpegpath, "%s/cam%d_snapshot.jpg", g_config.snapshot_path, index);
    int input_len;
    unsigned char *input = read_jpeg(jpegpath, &input_len);
    if (input == NULL)
    {
        glog_error("fail read [%s] failed.\n", jpegpath);
        return NULL;
    }

    gchar *base64_data = g_base64_encode(input, input_len);
    if (base64_data == NULL)
    {
        glog_error("Base64 encoding failed.\n");
        free(input);
        return NULL;
    }

    free(input);
    return base64_data;
}

static gchar *camera_cam_info_template = "{\"name\": \"%s\",  \"fw_version\": \"%s\",  \"ai_version\": \"%s\"}";
gboolean send_register_with_server(SoupWebsocketConnection *ws_conn)
{
    if (soup_websocket_connection_get_state(ws_conn) !=
        SOUP_WEBSOCKET_STATE_OPEN)
        return FALSE;

    // 추후 이름과 버전 기타 정보를 받아 들일 수 있도록 함
    // 변경이 되지 않는 정보를 전달
    gchar *caminfo_msg;
    caminfo_msg = g_strdup_printf(camera_cam_info_template, "gstream_main", GSTREAM_MAIN_VER, "0.0.1");

    glog_trace("Register id %s with server\n", g_config.camera_id);
    send_json_info("register", caminfo_msg);
    g_free(caminfo_msg);

    return TRUE;
}

int get_temp(int index)
{
    FILE *tempFile;
    char path[256];
    sprintf(path, "/sys/devices/virtual/thermal/thermal_zone%d/temp", index);
    int temperature;

    tempFile = fopen(path, "r");
    if (tempFile == NULL)
    {
        glog_error("fail read [%s] failed.\n", path);
        return 0; // 에러 코드 반환
    }

    fscanf(tempFile, "%d", &temperature);
    fclose(tempFile);
    return temperature;
}

int get_storage_usage()
{
    char command[100];
    char folderPath[] = "/dev/nvme0n1";

    // df 명령어 실행을 위한 명령어 문자열 생성
    snprintf(command, sizeof(command), "df -h %s", folderPath);

    // popen을 사용하여 명령어 실행
    FILE *dfOutput = popen(command, "r");
    if (dfOutput == NULL)
    {
        glog_error("command [%s] error \n", command);
        return EXIT_FAILURE;
    }

    // 결과 읽기
    char buffer[128];
    int index = 0;
    int usagePercentage = 0;
    while (fgets(buffer, sizeof(buffer), dfOutput) != NULL)
    {
        if (index > 1)
        {
            char *token = strtok(buffer, " %");
            // 반복문을 통해 % 값 찾기
            while (token != NULL)
            {
                if (strcmp(token, "%") == 0)
                {
                    // %를 찾았을 때 다음 토큰이 % 값임
                    token = strtok(NULL, " ");
                    if (token != NULL)
                    {
                        // % 값 출력
                        // printf("Usage percentage: %s\n", token);
                        // 혹은 정수로 변환하여 사용하려면 atoi 함수를 사용할 수 있습니다.
                        usagePercentage = atoi(token);
                        break;
                    }
                }
                token = strtok(NULL, " %");
            }
            break;
        }
        index = index + 1;
    }

    // popen으로 실행한 명령어 닫기
    int status = pclose(dfOutput);
    if (status == -1)
    {
        glog_error("pclose");
        return EXIT_FAILURE;
    }
    else if (WIFEXITED(status))
    {
        if (WEXITSTATUS(status) != 0)
        {
            glog_error("Command failed with exit status %d\n", WEXITSTATUS(status));
            return EXIT_FAILURE;
        }
    }
    else
    {
        glog_error("Command did not terminate normally\n");
        return EXIT_FAILURE;
    }

    return usagePercentage;
}

static gchar *camera_cam_status_template = "{\"rec_status\": \"%s\", \"rec_usage\": %d, \"cpu_temp\": %d, \"gpu_temp\": %d, \
                                        \"rgb_snaphot\": \"%s\", \"thermal_snaphot\": \"%s\" }";
void send_camera_info_to_server()
{
    pthread_mutex_lock(&g_send_info_mutex);

    if (g_wait_reply_cnt > MAX_WAIT_REPLY_CNT)
    {
        cleanup_and_retry_connect("Unknown Broken Connection", APP_STATE_ERROR_TIMEOUT);
        terminate_program();
        goto exit_func;
    }

    gchar *RGB_base64_data = image_to_base64("RGB");
    if (RGB_base64_data == NULL)
    {
        goto exit_func;
    }

    gchar *Thermal_base64_data = image_to_base64("Thermal");
    if (Thermal_base64_data == NULL)
    {
        free(RGB_base64_data);
        goto exit_func;
    }

    gchar *msg;
    msg = g_strdup_printf(camera_cam_status_template, g_setting.record_status ? "On" : "Off", get_storage_usage(),
                          get_temp(0), get_temp(1), RGB_base64_data, Thermal_base64_data);

    send_json_info("camstatus", msg);
    g_wait_reply_cnt = g_wait_reply_cnt + 1;
    if (g_wait_reply_cnt > 1)
        glog_trace("send_camera_info_to_server g_wait_reply_cnt=%d\n", g_wait_reply_cnt);

    g_free(msg);
    free(RGB_base64_data);
    free(Thermal_base64_data);

exit_func:
    pthread_mutex_unlock(&g_send_info_mutex);
}

void *process_heartbit(void *arg)
{
    static int count = 0;
    static int tegrastats_count = 0;

    glog_trace("start heartbit process timeout %d \n", g_heartbit_timeout);
    while (1)
    {
        usleep(g_heartbit_timeout * 1000);

        // Loop 가 시작하고 동작 시킨다.
        if (!g_firsttime_call)
        {
            apply_setting();
            g_firsttime_call = TRUE;
        }

        if (g_app_state == SERVER_REGISTERING) // if communication is normally set with signaling server, then state is SERVER_REGISTERING
        {
            if (++count == 3)
            { // per 15 seconds
                count = 0;
                send_camera_info_to_server();
            }
        }

        // if (++tegrastats_count >= 2)
        // {
        //     tegrastats_count = 0;

        //     // tegrastats_monitor.c의 함수 호출
        //     TegrastatsInfo *info = get_tegrastats_info();

        //     if (info)
        //     {
        //         // CPU 평균 계산
        //         float cpu_avg = 0;
        //         for (int i = 0; i < 6; i++) {
        //             cpu_avg += info->cpu_usage[i];
        //         }
        //         cpu_avg /= 6.0;
                
        //         glog_trace("Got system info: CPU temp=%.1f, GPU temp=%.1f, RAM=%d/%dMB\n",
        //           info->cpu_temp, info->gpu_temp, info->ram_used, info->ram_total);
        //     }
        //     else
        //     {
        //         glog_error("Failed to get tegrastats information\n");
        //     }
        // }
    }
    return 0;
}

void start_heartbit(int timeout)
{
    pthread_create(&g_heartbit_tid, NULL, process_heartbit, NULL);
}

void kill_heartbit()
{
    if (g_heartbit_tid)
    {
        pthread_kill(g_heartbit_tid, 0);
        g_heartbit_tid = 0;
    }
}

static gchar *json_msssage_template_string = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": {\"peer_id\": \"%s\", \"%s\":\"%s\"} \
}";

static gchar *json_msssage_template_string_json = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": {\"peer_id\": \"%s\", \"%s\":%s} \
}";

void send_image_to_peer(const gchar *peer_id, const gchar *source)
{
    gchar *base64_data = image_to_base64(source);
    if (base64_data == NULL)
    {
        return;
    }

    gchar *msg;
    msg = g_strdup_printf(json_msssage_template_string, "send_user", peer_id, "image", base64_data);
    send_msg_server(msg);

    free(msg);
    free(base64_data);
}

static gchar *camera_cam_setting_template = "{\"color_palette\": %d, \"record_status\": %d, \"analsys_status\": %d, \
                                              \"ptz_auto_seq\": \"%s\", \"ptz_preset\": \"%s\", \"auto_ptz_preset\": \
                                               \"%s\", \"auto_ptz_mode\": %d , \"enable_event_notify\": %d}";
void send_setting_to_peer(const gchar *peer_id)
{
    gchar *setting_json;
    gchar *msg;
    char ptz_status[MAX_PTZ_PRESET + 1] = {0};
    for (int i = 0; i < MAX_PTZ_PRESET; i++)
    {
        ptz_status[i] = (g_setting.ptz_preset[i][0] == 0) ? '0' : '1';
    }

    char auto_ptz_status[MAX_PTZ_PRESET + 1] = {0};
    for (int i = 0; i < MAX_PTZ_PRESET; i++)
    {
        auto_ptz_status[i] = (g_setting.auto_ptz_preset[i][0] == 0) ? '0' : '1';
    }

    setting_json = g_strdup_printf(camera_cam_setting_template, g_setting.color_pallet, g_setting.record_status, g_setting.analysis_status,
                                   g_setting.auto_ptz_seq, ptz_status, auto_ptz_status, is_work_auto_ptz(), g_setting.enable_event_notify);

    msg = g_strdup_printf(json_msssage_template_string_json, "send_user", peer_id, "setting", setting_json);
    send_msg_server(msg);

    free(msg);
    free(setting_json);
}

void send_rec_url_to_peer(const gchar *peer_id, const gchar *check_str)
{
    gchar *msg;
    msg = g_strdup_printf(json_msssage_template_string, "send_user", peer_id, "rec_url", g_config.http_service_ip);
    send_msg_server(msg);
    free(msg);
}

void remove_data_path(const gchar *remve_path)
{
    char cmd_full_remove_path[512];
    sprintf(cmd_full_remove_path, "rm -rf %s/%s", g_config.record_path, remve_path);

    glog_trace(" System Command %s \n", cmd_full_remove_path);
    execute_process(cmd_full_remove_path, FALSE);
}

void set_camera_dn_mode(int camera_dn_mode) // 0 => auto, 1 => manual day, 2 => manual night mode
{
    if (is_open_serial() == 0)
    {
        return;
    }

    glog_trace("camera_dn_mode [%d]\n", camera_dn_mode);

    unsigned char camera_ptz_cmd[7] = {0x96, 0x00, 0x66, 0x42, 0x01, 0x02, 0x41};
    if (0 == camera_dn_mode)
    {
        camera_ptz_cmd[5] = 0x02;
        camera_ptz_cmd[6] = 0x41;
    }
    else if (1 == camera_dn_mode)
    {
        camera_ptz_cmd[5] = 0x00;
        camera_ptz_cmd[6] = 0x3f;
    }
    else if (2 == camera_dn_mode)
    {
        camera_ptz_cmd[5] = 0x01;
        camera_ptz_cmd[6] = 0x40;
    }
    else
    {
        glog_error("invalied camera mode [%d] \n", camera_dn_mode);
        return;
    }
    write_serial(camera_ptz_cmd, 7);
}

void get_cur_dir(char *cwd, int size)
{
    if (getcwd(cwd, size) == NULL)
    {
        perror("getcwd() error");
    }
}

#if MINDULE_INCLUDE
void get_ranch_setting_path(char *fname)
{
    char path[200] = "";

    get_cur_dir(path, sizeof(path));
    sprintf(fname, "%s/%s", path, (char *)RANCH_SETTING_FILE);
}
#endif

gboolean process_message_cmd(gJSONObj *jsonObj)
{
    JsonNode *node;

    node = json_object_get_member(jsonObj->object, "message");
    if (!node)
    {
        glog_trace("Can not get message Node\n");
        return FALSE;
    }

    JsonObject *object = json_node_get_object(node);
    if (json_object_has_member(object, "ptz"))
    {
        glog_trace("ptz\n");
        const gchar *ptz_ctl_msg;
        if (!cockpit_json_get_string(object, "ptz", NULL, &ptz_ctl_msg, FALSE))
        {
            glog_trace("Can not get message PTZ Command\n");
            return FALSE;
        }
        send_ptz_serial_data(ptz_ctl_msg);
    }
    else if (json_object_has_member(object, "ptz_move"))
    { // LJH, 242826
        glog_trace("ptz_move\n");
        const gchar *msg;
        if (!cockpit_json_get_string(object, "ptz_move", NULL, &msg, FALSE))
        {
            glog_trace("Can not get message PTZ Move Command\n");
            return FALSE;
        }
        send_ptz_move_serial_data(msg);
    }
    else if (json_object_has_member(object, "record"))
    {
        const gchar *on_off;

        if (!cockpit_json_get_string(object, "record", NULL, &on_off, FALSE))
        {
            glog_trace("Can not get message Record Command\n");
            return FALSE;
        }
        glog_trace("record on_off=%s\n", on_off);
        int status = 0;
        if (strcmp(on_off, "On") == 0)
        {
            start_process_rec();
            status = 1;
        }
        else
        {
            stop_process_rec();
        }

        //@@TODO : 실제 record가 시작 되었는지 확인이 필요.
        g_setting.record_status = status;
        update_setting(g_config.device_setting_path, &g_setting);
    }
    else if (json_object_has_member(object, "analysis"))
    {
        glog_trace("analysis\n");
        const gchar *on_off;
        if (!cockpit_json_get_string(object, "analysis", NULL, &on_off, FALSE))
        {
            glog_trace("Can not get message Analysis Command\n");
            return FALSE;
        }
        glog_trace("analysis on_off=%s\n", on_off);

        if (strcmp(on_off, "On") == 0)
        {
            set_process_analysis(TRUE);
            g_setting.analysis_status = 1;
        }
        else
        {
            set_process_analysis(FALSE);
            g_setting.analysis_status = 0;
        }

        g_frame_count[RGB_CAM] = 0;
        g_frame_count[THERMAL_CAM] = 0;

        printf("g_setting.analysis_status = %d\n", g_setting.analysis_status);

        update_setting(g_config.device_setting_path, &g_setting);
    }
    else if (json_object_has_member(object, "color_palette"))
    {
        glog_trace("color_palette\n");
        const gchar *palette_id;
        if (!cockpit_json_get_string(object, "color_palette", NULL, &palette_id, FALSE))
        {
            glog_trace("Can not get message Analysis Command\n");
            return FALSE;
        }

        if (palette_id[0] < '0' || palette_id[0] > '9')
        {
            glog_trace("Invalied palette  id %c \n", palette_id[0]);
            return FALSE;
        }
        glog_trace("palette_id=%c\n", palette_id[0]);

        char process_cmd[256];
        sprintf(process_cmd, "/home/nvidia/webrtc/cam_ctl %c", palette_id[0]);
        execute_process(process_cmd, FALSE);

        g_setting.color_pallet = palette_id[0] - '0';
        update_setting(g_config.device_setting_path, &g_setting);
    }
    else if (json_object_has_member(object, "send_event"))
    { // LJH, this is from test page
        glog_trace("send_event\n");
        const gchar *str_event;
        if (!cockpit_json_get_string(object, "send_event", NULL, &str_event, FALSE))
        {
            glog_trace("Can not get message Analysis Command\n");
            return FALSE;
        }
        send_event_to_recorder_simple(1, 0);
    }
    else if (json_object_has_member(object, "del_ptz_pos"))
    {
        glog_trace("del_ptz_pos\n");
        const gchar *str_id;
        if (!cockpit_json_get_string(object, "del_ptz_pos", NULL, &str_id, FALSE))
        {
            glog_trace("Can not get message PTZ Pos Command\n");
            return FALSE;
        }

        int id = atoi(str_id);
        int mode = id / 16;
        id = id % 16;
        if (id >= MAX_PTZ_PRESET)
        {
            glog_trace("Can not invailied PTZ Pos %d\n", id);
            return FALSE;
        }
        glog_trace("mode=%d, id=%d\n", mode, id);
        char *ptz_preset = (mode == 0) ? g_setting.ptz_preset[id] : g_setting.auto_ptz_preset[id];
        memset(&ptz_preset[0], 0, PTZ_POS_SIZE);
        update_setting(g_config.device_setting_path, &g_setting);
    }
    else if (json_object_has_member(object, "set_ptz_pos"))
    {
        glog_trace("set_ptz_pos\n");
        const gchar *str_id;
        if (!cockpit_json_get_string(object, "set_ptz_pos", NULL, &str_id, FALSE))
        {
            glog_trace("Can not get message PTZ Pos Command\n");
            return FALSE;
        }

        int id = atoi(str_id);
        int mode = id / 16;
        id = id % 16;
        if (id >= MAX_PTZ_PRESET)
        {
            glog_trace("Can not invailied PTZ Pos %d\n", id);
            return FALSE;
        }
        glog_trace("mode=%d, id=%d\n", mode, id);

        char *ptz_preset = (mode == 0) ? g_setting.ptz_preset[id] : g_setting.auto_ptz_preset[id];
        unsigned char read_data[32];
        if (set_ptz_pos(id, read_data, mode) == 0)
        {
            ptz_preset[0] = 1;
            memcpy(&ptz_preset[1], &read_data[5], 10);
            update_setting(g_config.device_setting_path, &g_setting);
        }
    }
    else if (json_object_has_member(object, "move_ptz_pos"))
    {
        glog_trace("move_ptz_pos\n");
        const gchar *str_id;
        if (!cockpit_json_get_string(object, "move_ptz_pos", NULL, &str_id, FALSE))
        {
            glog_trace("Can not get message Analysis Command\n");
            return FALSE;
        }

        int id = atoi(str_id);
        int mode = id / 16;
        id = id % 16;
        if (id >= MAX_PTZ_PRESET)
        {
            glog_trace("Can not invailied PTZ Pos [%d]\n", id);
            return FALSE;
        }
        move_ptz_pos(id, mode);
        glog_trace("mode=%d, id=%d\n", mode, id);
    }
    else if (json_object_has_member(object, "set_auto_ptz_pos"))
    {
        glog_trace("set_auto_ptz_pos\n");
        const gchar *str_id;
        if (!cockpit_json_get_string(object, "set_auto_ptz_pos", NULL, &str_id, FALSE))
        {
            glog_trace("Can not get message Auto PTZ Pos Command\n");
            return FALSE;
        }

        int id = atoi(str_id);
        glog_trace("id=%d\n", id);
        if (id >= MAX_PTZ_PRESET)
        {
            glog_trace("Can not invailied Auto PTZ Pos %d\n", id);
            return FALSE;
        }
        unsigned char read_data[32];
        if (set_ptz_pos(id, read_data, 1) == 0)
        {
            g_setting.auto_ptz_preset[id][0] = 1;
            memcpy(&g_setting.auto_ptz_preset[id][1], &read_data[5], 10);
            update_setting(g_config.device_setting_path, &g_setting);
        }
    }
    else if (json_object_has_member(object, "auto_move_ptz"))
    {
        glog_trace("auto_move_ptz\n");
        const gchar *move_seq;
        int ret = 0;
        if (!cockpit_json_get_string(object, "auto_move_ptz", NULL, &move_seq, FALSE))
        {
            glog_trace("Can not get message Analysis Command\n");
            return FALSE;
        }
        glog_trace("auto_move_ptz:%s (value len=%d)\n", move_seq, strlen(move_seq));
        if ((ret = auto_move_ptz(move_seq)) == 0)
        {
            strcpy(g_setting.auto_ptz_seq, move_seq); // LJH, moved position
            update_setting(g_config.device_setting_path, &g_setting);
        }
        else
        {
            glog_trace("auto_move_ptz() returned %d\n", ret);
            if (strlen(move_seq) == 0)
            {
                g_setting.auto_ptz_seq[0] = 0;
                update_setting(g_config.device_setting_path, &g_setting);
            }
        }
    }
    else if (json_object_has_member(object, "request_image"))
    {
        glog_trace("request_image\n");
        const gchar *peer_id;
        const gchar *source;
        if (!get_json_data_from_message(jsonObj, "peer_id", &peer_id))
        {
            glog_trace("Can not get peer_id in request_image\n");
            return FALSE;
        }
        if (!cockpit_json_get_string(object, "request_image", NULL, &source, FALSE))
        {
            glog_trace("Can not get source in request_image\n");
            return FALSE;
        }
        glog_trace("peer_id=%s source=%s\n", peer_id, source);
        send_image_to_peer(peer_id, source);
    }
    else if (json_object_has_member(object, "request_setting"))
    {
        glog_trace("request_setting\n");
        const gchar *peer_id;
        if (!get_json_data_from_message(jsonObj, "peer_id", &peer_id))
        {
            glog_trace("Can not get peer_id in request_setting\n");
            return FALSE;
        }
        send_setting_to_peer(peer_id);
    }
    else if (json_object_has_member(object, "request_rec_url"))
    {
        glog_trace("request_rec_url\n");
        const gchar *peer_id;
        const gchar *check_str;
        if (!get_json_data_from_message(jsonObj, "peer_id", &peer_id))
        {
            glog_trace("Can not get peer_id in request_rec_url\n");
            return FALSE;
        }

        if (!cockpit_json_get_string(object, "request_rec_url", NULL, &check_str, FALSE))
        {
            glog_trace("Can not get check_str in request_rec_url\n");
            return FALSE;
        }
        glog_trace("peer_id=%s check_str=%s\n", peer_id, check_str);
        send_rec_url_to_peer(peer_id, check_str);
    }
    else if (json_object_has_member(object, "enable_event_notify"))
    {
        glog_trace("enable_event_notify\n");
        const gchar *on_off;
        if (!cockpit_json_get_string(object, "enable_event_notify", NULL, &on_off, FALSE))
        {
            glog_trace("Can not get check_str in enable_event_notify\n");
            return FALSE;
        }

        glog_trace("enable_event_notify on_off=%s\n", on_off);
        if (strcmp(on_off, "On") == 0)
        {
            g_setting.enable_event_notify = 1;
        }
        else
        {
            g_setting.enable_event_notify = 0;
        }

        update_setting(g_config.device_setting_path, &g_setting);
    }
    else if (json_object_has_member(object, "request_reset"))
    {
        glog_trace("reset_request\n");
        execute_process("reboot", FALSE);
    }
    else if (json_object_has_member(object, "request_factory_default"))
    {
        glog_trace("request_factory_default\n");
        execute_process("./factory_default.sh", FALSE);
    }
    else if (json_object_has_member(object, "request_remove_path"))
    {
        glog_trace("request_remove_path\n");
        const gchar *path;
        if (!cockpit_json_get_string(object, "request_remove_path", NULL, &path, FALSE))
        {
            glog_trace("Can not get path in request_remove_path\n");
            return FALSE;
        }
        glog_trace("request_remove_path path [%s] \n", path);
        remove_data_path(path);

        // execute_process("./factory_default.sh", FALSE);
    }
    else if (json_object_has_member(object, "camera_dn_mode"))
    {
        glog_trace("camera_dn_mode\n");
        const gchar *mode_id;
        if (!cockpit_json_get_string(object, "camera_dn_mode", NULL, &mode_id, FALSE))
        {
            glog_trace("Can not get message Analysis Command\n");
            return FALSE;
        }

        if (mode_id[0] < '0' || mode_id[0] > '2')
        {
            glog_trace("Invalied camera mode %c \n", mode_id[0]);
            return FALSE;
        }
        g_setting.camera_dn_mode = mode_id[0] - '0';
        glog_trace("camera_dn_mode=%d\n", g_setting.camera_dn_mode);
        set_camera_dn_mode(g_setting.camera_dn_mode);
        update_setting(g_config.device_setting_path, &g_setting);
    }

#if MINDULE_INCLUDE

    else if (json_object_has_member(object, "set_ranch_pos"))
    {
        glog_trace("set_ranch_pos\n");

        const gchar *str_index;
        if (!cockpit_json_get_string(object, "set_ranch_pos", NULL, &str_index, FALSE))
        {
            glog_trace("Can not get message Ranch PTZ Pos Command\n");
            return FALSE;
        }
        int index = atoi(str_index);
        glog_trace("received index=%d\n", index);
        if (index >= MAX_RANCH_POS)
        {
            glog_trace("ranch pos index(=%d) is out of range\n", index);
            return FALSE;
        }

        unsigned char read_data[32];
        if (set_ranch_pos(index, read_data) == 0)
        {
            g_ranch_setting.ranch_pos[index][0] = 1;
            memcpy(&g_ranch_setting.ranch_pos[index][1], &read_data[5], 10);
            char fname[100];
            get_ranch_setting_path(fname);
            update_ranch_setting(fname, &g_ranch_setting);
        }
    }
    else if (json_object_has_member(object, "move_ranch_pos"))
    {
        glog_trace("move_ranch_pos\n");

        const gchar *str_index;
        if (!cockpit_json_get_string(object, "move_ranch_pos", NULL, &str_index, FALSE))
        {
            glog_trace("Can not get message Ranch PTZ Pos Command\n");
            return FALSE;
        }
        int index = atoi(str_index);
        glog_trace("received index=%d\n", index);
        if (index >= MAX_RANCH_POS)
        {
            glog_trace("ranch pos index(=%d) is out of range\n", index);
            return FALSE;
        }

        if (move_ranch_pos(index) != 0)
        {
            glog_trace("ranch move index(=%d) failed\n", index);
        }
    }
    else if (json_object_has_member(object, "del_ranch_pos"))
    {
        glog_trace("del_ranch_pos\n");

        const gchar *str_index;
        if (!cockpit_json_get_string(object, "del_ranch_pos", NULL, &str_index, FALSE))
        {
            glog_trace("Can not get message Ranch PTZ Pos Command\n");
            return FALSE;
        }
        int index = atoi(str_index);
        glog_trace("received index=%d\n", index);
        if (index >= MAX_RANCH_POS)
        {
            glog_trace("ranch pos index(=%d) is out of range\n", index);
            return FALSE;
        }
        if (update_ranch_pos(index, NULL, 0) == 0)
        {
            memset(&g_ranch_setting.ranch_pos[index], 0, sizeof(g_ranch_setting.ranch_pos[index]));
            char fname[100];
            get_ranch_setting_path(fname);
            update_ranch_setting(fname, &g_ranch_setting);
        }
    }

#endif

    else
    {
        glog_trace("Unknown Command.... \n");
        return FALSE;
    }

    return TRUE;
}

gboolean apply_setting()
{
    char process_cmd[256];
    set_process_analysis(g_setting.analysis_status);
    set_ptz_move_speed(g_setting.ptz_move_speed, g_setting.auto_ptz_move_speed);

    // default position으로 이동함
    if (g_setting.ptz_preset[0][0])
    {
        move_ptz_pos(0, 0);
    }

    sprintf(process_cmd, "/home/nvidia/webrtc/cam_ctl %d", g_setting.color_pallet);
    execute_process(process_cmd, FALSE);
    if (g_setting.record_status)
    {
        start_process_rec();
    }

    set_camera_dn_mode(g_setting.camera_dn_mode);

    return TRUE;
}

void send_pipe_data(const gchar *str)
{
    if (str == NULL || strlen(str) == 0)
    {
        glog_error("send_pipe_data str is NULL or empty\n");
        return;
    }

    if (is_open_serial() == 0)
    {
        return;
    }

    if (strstr(str, "up") != NULL)
    {
        unsigned char camera_ptz_cmd[12] = {0x96, 0x0, 0x14, 0x1, 0x6, 0x81, 0x1, 0x4, 0x16, 0x1, 0xFF, 0x4D};
        write_serial(camera_ptz_cmd, 12);
    }
    else if (strstr(str, "down") != NULL)
    {
        unsigned char camera_ptz_cmd1[12] = {0x96, 0x0, 0x14, 0x1, 0x6, 0x81, 0x1, 0x4, 0x16, 0x2, 0xFF, 0x4E};
        write_serial(camera_ptz_cmd1, 12);
    }
    else if (strstr(str, "left") != NULL)
    {
        unsigned char camera_ptz_cmd2[12] = {0x96, 0x0, 0x14, 0x1, 0x6, 0x81, 0x1, 0x4, 0x16, 0x4, 0xFF, 0x50};
        write_serial(camera_ptz_cmd2, 12);
    }
    else if (strstr(str, "right") != NULL)
    {
        unsigned char camera_ptz_cmd3[12] = {0x96, 0x0, 0x14, 0x1, 0x6, 0x81, 0x1, 0x4, 0x16, 0x8, 0xFF, 0x54};
        write_serial(camera_ptz_cmd3, 12);
    }
    else if (strstr(str, "enter") != NULL)
    {
        unsigned char camera_ptz_cmd4[12] = {0x96, 0x0, 0x14, 0x1, 0x6, 0x81, 0x1, 0x4, 0x16, 0x10, 0xFF, 0x5C};
        write_serial(camera_ptz_cmd4, 12);
    }
    else if (strstr(str, "zoom_init") != NULL)
    {
        unsigned char camera_ptz_cmd5[12] = {0x96, 0x0, 0x14, 0x1, 0x6, 0x81, 0x1, 0x4, 0x19, 0x1, 0xFF, 0x50};
        glog_trace("send zoom_init command\n");
        write_serial(camera_ptz_cmd5, 12);
    }
    else if (strstr(str, "AF_Debug_On") != NULL)
    {
        unsigned char camera_ptz_cmd5[14] = {0x96, 0x00, 0x14, 0x01, 0x08, 0x81, 0x01, 0x04, 0x24, 0x31, 0x00, 0x02, 0xFF, 0x8F};
        glog_trace("send AF_Debug_On command\n");
        write_serial(camera_ptz_cmd5, 14);
    }
    else if (strstr(str, "AF_Debug_Off") != NULL)
    {
        unsigned char camera_ptz_cmd5[14] = {0x96, 0x00, 0x14, 0x01, 0x08, 0x81, 0x01, 0x04, 0x24, 0x31, 0x00, 0x00, 0xFF, 0x8D};
        glog_trace("send AF_Debug_Off command\n");
        write_serial(camera_ptz_cmd5, 14);
    }
    else if (strstr(str, "Focus_Position") != NULL)
    {
        unsigned char camera_ptz_cmd5[12] = {0x96, 0x00, 0x14, 0x01, 0x06, 0x81, 0x09, 0x04, 0x19, 0x1F, 0xFF, 0x76};
        glog_trace("send Focus_Position command\n");
        write_serial(camera_ptz_cmd5, 12);
    }
    else if (strstr(str, "ir_init") != NULL)
    {
        unsigned char ptz_cmd1[27] = {
            0x96, 0x00, 0x22, 0x05, 0x15, 0x01, 0x01, 0x01, 0x20, 0x30,
            0x40, 0x60, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
            0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0xB7};

        write_serial(ptz_cmd1, 27);

        usleep(1500000);

        unsigned char ptz_cmd2[27] = {
            0x96, 0x00, 0x22, 0x05, 0x15, 0x00, 0x7F, 0x7F, 0x7F, 0x7F,
            0x7F, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x5C};

        write_serial(ptz_cmd2, 27);

        glog_debug("send ir_init command\n");
    }
    else
    {
        glog_error("Unknown pipe data %s \n", str);
        return;
    }
}