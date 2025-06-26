#include <gst/gst.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include "command_handler.h"
#include "json_utils.h"
#include "log_wrapper.h"

// 안전한 명령어 실행 함수 (extern으로 변경)
char* execute_safe_command(const char* command) {
    // 허용된 명령어 목록 (보안을 위해 제한)
    const char* allowed_commands[] = {
        "uptime",
        "df -h", 
        "free -h",
        "ps aux | grep -E \"(gstream|webrtc)\"",
        "netstat -tuln",
        "cat /proc/cpuinfo | head -20",
        "nvidia-smi",
        "timeout 5 tegrastats",
        NULL
    };
    
    // 명령어가 허용 목록에 있는지 확인
    gboolean is_allowed = FALSE;
    for (int i = 0; allowed_commands[i] != NULL; i++) {
        if (strcmp(command, allowed_commands[i]) == 0) {
            is_allowed = TRUE;
            break;
        }
    }
    
    if (!is_allowed) {
        glog_error("Command not allowed: %s\n", command);
        return g_strdup("ERROR: Command not allowed for security reasons");
    }
    
    // 명령어 실행
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        glog_error("Failed to execute command: %s\n", command);
        return g_strdup("ERROR: Failed to execute command");
    }
    
    // 결과 읽기
    char buffer[4096] = {0};
    char *result = g_malloc(8192);
    result[0] = '\0';
    
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        strncat(result, buffer, 8191 - strlen(result));
        if (strlen(result) >= 8000) { // 결과가 너무 길면 중단
            strcat(result, "\n... (output truncated)");
            break;
        }
    }
    
    int status = pclose(fp);
    if (status == -1) {
        glog_error("pclose failed for command: %s\n", command);
    }
    
    return result;
}

// sudo 명령어 실행 (더 제한적) (extern으로 변경)
char* execute_sudo_command(const char* command) {
    const char* allowed_sudo_commands[] = {
        "systemctl status gstream_main",
        "journalctl -u gstream_main -n 20", 
        "tail -n 20 /var/log/syslog",
        "timeout 5 tegrastats",
        NULL
    };
    
    gboolean is_allowed = FALSE;
    for (int i = 0; allowed_sudo_commands[i] != NULL; i++) {
        if (strcmp(command, allowed_sudo_commands[i]) == 0) {
            is_allowed = TRUE;
            break;
        }
    }
    
    if (!is_allowed) {
        glog_error("Sudo command not allowed: %s\n", command);
        return g_strdup("ERROR: Sudo command not allowed");
    }
    
    char full_command[512];
    snprintf(full_command, sizeof(full_command), "sudo %s", command);
    
    return execute_safe_command(command); // sudo 없이 실행해보고, 필요시 수정
}

// 특별한 tegrastats 파싱 처리 (extern으로 변경)
char* parse_tegrastats_output(void) {
    char* raw_output = execute_safe_command("timeout 5 tegrastats");
    if (!raw_output || strstr(raw_output, "ERROR:") == raw_output) {
        return raw_output;
    }
    
    // tegrastats 출력을 파싱하여 JSON 형태로 변환
    // 실제 파싱 로직은 기존 코드를 참조하여 구현
    char* parsed_result = g_malloc(2048);
    snprintf(parsed_result, 2048, 
        "{\n"
        "  \"tegrastats_raw\": \"%s\",\n"
        "  \"parsed\": true,\n"
        "  \"timestamp\": \"%ld\"\n"
        "}", 
        raw_output, 
        time(NULL));
    
    g_free(raw_output);
    return parsed_result;
}

// 메인 custom_command 처리 함수 (함수 포인터 추가)
void handle_custom_command(gJSONObj* jsonObj, send_message_func_t send_func) {
    const gchar* peer_id = NULL;
    const gchar* command = NULL;
    const gchar* command_type = NULL;
    const gchar* custom_command = NULL;
    
    // JSON에서 데이터 추출 - send_camera 형태와 custom_command 형태 모두 지원
    get_json_data_from_message(jsonObj, "peer_id", &peer_id);
    
    // custom_command 액션인지 확인
    if (get_json_data_from_message(jsonObj, "custom_command", &custom_command)) {
        // send_camera 형태: {"action": "send_camera", "custom_command": "uptime"}
        if (get_json_data_from_message(jsonObj, "command", &command)) {
            // command 키가 별도로 있으면 사용
        } else {
            // custom_command 값을 command로 사용
            command = custom_command;
        }
        get_json_data_from_message(jsonObj, "type", &command_type);
        
        // tegrastats_parsed인 경우 type도 설정
        if (strcmp(custom_command, "tegrastats_parsed") == 0) {
            command_type = "tegrastats_parsed";
            command = "tegrastats";  // 실제 명령어는 tegrastats
        }
    } else {
        // 직접 custom_command 형태: {"action": "custom_command", "command": "uptime"}
        get_json_data_from_message(jsonObj, "command", &command);
        get_json_data_from_message(jsonObj, "type", &command_type);
    }
    
    if (!command) {
        glog_error("No command specified in custom_command\n");
        return;
    }
    
    glog_trace("Processing custom command: %s (type: %s) from peer: %s\n", 
               command, command_type ? command_type : "normal", peer_id ? peer_id : "unknown");
    
    char* result = NULL;
    
    // 명령어 타입에 따른 처리
    if (command_type && strcmp(command_type, "sudo") == 0) {
        result = execute_sudo_command(command);
    }
    else if (command_type && strcmp(command_type, "tegrastats_parsed") == 0) {
        result = parse_tegrastats_output();
    }
    else {
        if(strcmp(command, "zoom_init") == 0) {
            const char* cmd = "echo \"zoom_init\" > /home/nvidia/webrtc/ptz_command_pipe";
            int ret = system(cmd);

            glog_debug("Executing zoom_init command: %s result : %d\n", cmd, ret);
        } else if(strcmp(command, "ir_init") == 0) {
            const char* cmd = "echo \"ir_init\" > /home/nvidia/webrtc/ptz_command_pipe";
            int ret = system(cmd);

            glog_debug("Executing ir_init command: %s result : %d\n", cmd, ret);
        }
        else
            result = execute_safe_command(command);
    }
    
    // 결과를 WebSocket으로 전송
    if (result && send_func) {
        send_command_result_to_websocket(command, result, peer_id, send_func);
        g_free(result);
    }
}

// WebSocket으로 명령어 결과 전송 (기존 포맷에 맞춤)
void send_command_result_to_websocket(const char* command, const char* result, const char* peer_id, send_message_func_t send_func) {
    if (!send_func) {
        glog_error("Send function not available\n");
        return;
    }
    
    // 결과 길이 제한 (너무 크면 연결이 끊어질 수 있음)
    size_t result_len = strlen(result);
    char* truncated_result = NULL;
    
    if (result_len > 2000) {  // 2KB로 제한
        truncated_result = g_malloc(2001);
        strncpy(truncated_result, result, 2000);
        truncated_result[2000] = '\0';
        glog_trace("Result truncated from %zu to 2000 chars\n", result_len);
    } else {
        truncated_result = g_strdup(result);
    }
    
    // JSON 문자열에서 특수문자 이스케이프 (더 안전한 방식)
    size_t escaped_size = strlen(truncated_result) * 2 + 1;
    char* escaped_result = g_malloc(escaped_size);
    int j = 0;
    
    for (int i = 0; truncated_result[i] && j < escaped_size - 10; i++) {
        char c = truncated_result[i];
        switch(c) {
            case '"':
                escaped_result[j++] = '\\';
                escaped_result[j++] = '"';
                break;
            case '\\':
                escaped_result[j++] = '\\';
                escaped_result[j++] = '\\';
                break;
            case '\n':
                escaped_result[j++] = '\\';
                escaped_result[j++] = 'n';
                break;
            case '\r':
                escaped_result[j++] = '\\';
                escaped_result[j++] = 'r';
                break;
            case '\t':
                escaped_result[j++] = '\\';
                escaped_result[j++] = 't';
                break;
            default:
                // 제어 문자는 공백으로 대체
                if (c < 32 && c >= 0) {
                    escaped_result[j++] = ' ';
                } else {
                    escaped_result[j++] = c;
                }
                break;
        }
    }
    escaped_result[j] = '\0';
    
    // 기존 gstream_control.c 포맷에 맞춰 JSON 메시지 생성
    // json_msssage_template_string 포맷 사용
    size_t json_size = strlen(escaped_result) + 512;
    char* json_message = g_malloc(json_size);
    
    snprintf(json_message, json_size,
        "{"
        "\"peerType\": \"camera\","
        "\"action\": \"send_user\","
        "\"message\": {\"peer_id\": \"%s\", \"command_result\": \"%s\"}"
        "}",
        peer_id ? peer_id : "",
        escaped_result);
    
    // 최종 메시지 크기 확인
    size_t final_size = strlen(json_message);
    glog_trace("Sending JSON message (%zu bytes): %.200s%s\n", 
               final_size, json_message, final_size > 200 ? "..." : "");
    
    // 메시지가 너무 크면 축약
    if (final_size > 4096) {  // 4KB 초과시
        glog_error("Message too large (%zu bytes), sending simple response instead\n", final_size);
        
        g_free(json_message);
        json_message = g_malloc(512);
        snprintf(json_message, 512,
            "{"
            "\"peerType\": \"camera\","
            "\"action\": \"send_user\","
            "\"message\": {\"peer_id\": \"%s\", \"command_result\": \"Command '%s' executed. Output too large to display.\"}"
            "}",
            peer_id ? peer_id : "",
            command ? command : "unknown");
    }
    
    // 함수 포인터를 통해 메시지 전송
    send_func(json_message);
    
    g_free(json_message);
    g_free(escaped_result);
    g_free(truncated_result);
    glog_trace("Sent command result via function pointer\n");
}