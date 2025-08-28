#include <string.h>
#include "gstream_control.h"
#include "gstream_main.h"
#include "device_setting.h"
#include "ptz_control.h"
#include "nvds_process.h"
#include "webrtc_peer.h"
#include "serial_comm.h"
#include "logging.h"

// External globals
extern DeviceSetting g_setting;
extern WebRTCConfig g_config;

// Command handler implementations
gboolean handle_ptz_command(JsonObject *object, const gchar *peer_id) {
    const gchar *ptz_ctl_msg;
    if (!cockpit_json_get_string(object, "ptz", NULL, &ptz_ctl_msg, FALSE)) {
        glog_trace("Cannot get PTZ command\n");
        return FALSE;
    }
    send_ptz_serial_data(ptz_ctl_msg);
    return TRUE;
}

gboolean handle_ptz_move(JsonObject *object, const gchar *peer_id) {
    const gchar *msg;
    if (!cockpit_json_get_string(object, "ptz_move", NULL, &msg, FALSE)) {
        glog_trace("Cannot get PTZ move command\n");
        return FALSE;
    }
    send_ptz_move_serial_data(msg);
    return TRUE;
}

gboolean handle_ptz_move_control(JsonObject *object, const gchar *peer_id) {
    const gchar *msg;
    if (!cockpit_json_get_string(object, "ptz_move_control", NULL, &msg, FALSE)) {
        glog_trace("Cannot get PTZ move control command\n");
        return FALSE;
    }
    send_ptz_move_serial_data_immediate(msg);
    return TRUE;
}

gboolean handle_ptz_move_point(JsonObject *object, const gchar *peer_id) {
    int x, y, zoom;
    if (!cockpit_json_get_int(object, "ptz_move_point_x", -1, &x, FALSE) ||
        !cockpit_json_get_int(object, "ptz_move_point_y", -1, &y, FALSE) ||
        !cockpit_json_get_int(object, "ptz_move_point_z", -1, &zoom, FALSE)) {
        glog_trace("Cannot get PTZ move point parameters\n");
        return FALSE;
    }
    
    if (zoom == -1) {
        send_ptz_relative_move_by_pixel_offset(x, y, 20);
    } else {
        send_ptz_area_move_with_response(x, y, x + 100, y + 100, zoom);
    }
    return TRUE;
}

gboolean handle_record(JsonObject *object, const gchar *peer_id) {
    const gchar *msg;
    if (!cockpit_json_get_string(object, "record", NULL, &msg, FALSE)) {
        glog_trace("Cannot get record command\n");
        return FALSE;
    }
    
    g_setting.record_status = (strcmp(msg, "on") == 0 || strcmp(msg, "On") == 0);
    char fname[100];
    get_cur_dir(fname, sizeof(fname));
    strcat(fname, "/device_setting.json");
    update_setting(fname, &g_setting);
    
    if (g_setting.record_status) {
        start_process_rec();
    } else {
        stop_process_rec();
    }
    return TRUE;
}

gboolean handle_analysis(JsonObject *object, const gchar *peer_id) {
    const gchar *msg;
    if (!cockpit_json_get_string(object, "analysis", NULL, &msg, FALSE)) {
        glog_trace("Cannot get analysis command\n");
        return FALSE;
    }
    
    g_setting.analysis_status = (strcmp(msg, "on") == 0 || strcmp(msg, "On") == 0);
    set_process_analysis(g_setting.analysis_status);
    
    char fname[100];
    get_cur_dir(fname, sizeof(fname));
    strcat(fname, "/device_setting.json");
    update_setting(fname, &g_setting);
    
    return TRUE;
}

gboolean handle_color_palette(JsonObject *object, const gchar *peer_id) {
    const gchar *msg;
    if (!cockpit_json_get_string(object, "color_palette", NULL, &msg, FALSE)) {
        glog_trace("Cannot get color palette command\n");
        return FALSE;
    }
    
    g_setting.color_pallet = (msg[0] - '0');
    char fname[100];
    get_cur_dir(fname, sizeof(fname));
    strcat(fname, "/device_setting.json");
    update_setting(fname, &g_setting);
    
    char process_cmd[256];
    sprintf(process_cmd, "/home/nvidia/webrtc/cam_ctl %d", g_setting.color_pallet);
    execute_process(process_cmd, FALSE);
    
    return TRUE;
}

gboolean handle_request_image(JsonObject *object, const gchar *peer_id) {
    const gchar *source;
    if (!cockpit_json_get_string(object, "request_image", NULL, &source, FALSE)) {
        glog_trace("Cannot get image source\n");
        return FALSE;
    }
    
    send_image_to_peer(peer_id, source);
    return TRUE;
}

gboolean handle_request_setting(JsonObject *object, const gchar *peer_id) {
    send_setting_to_peer(peer_id);
    return TRUE;
}

gboolean handle_request_rec_url(JsonObject *object, const gchar *peer_id) {
    const gchar *check_str = "";
    cockpit_json_get_string(object, "request_rec_url", NULL, &check_str, FALSE);
    send_rec_url_to_peer(peer_id, check_str);
    return TRUE;
}

gboolean handle_camera_dn_mode(JsonObject *object, const gchar *peer_id) {
    const gchar *msg;
    if (!cockpit_json_get_string(object, "camera_dn_mode", NULL, &msg, FALSE)) {
        glog_trace("Cannot get camera DN mode\n");
        return FALSE;
    }
    
    g_setting.camera_dn_mode = (msg[0] - '0');
    set_camera_dn_mode(g_setting.camera_dn_mode);
    
    char fname[100];
    get_cur_dir(fname, sizeof(fname));
    strcat(fname, "/device_setting.json");
    update_setting(fname, &g_setting);
    
    return TRUE;
}

// Command table - maps command names to handler functions
static const CommandEntry command_table[] = {
    // PTZ commands
    {"ptz", handle_ptz_command, "Direct PTZ control"},
    {"ptz_move", handle_ptz_move, "PTZ movement control"},
    {"ptz_move_control", handle_ptz_move_control, "Immediate PTZ movement"},
    {"ptz_move_point", handle_ptz_move_point, "PTZ point movement"},
    
    // Recording and analysis
    {"record", handle_record, "Recording control"},
    {"analysis", handle_analysis, "Analysis control"},
    
    // Camera settings
    {"color_palette", handle_color_palette, "Color palette setting"},
    {"camera_dn_mode", handle_camera_dn_mode, "Day/Night mode control"},
    
    // Request operations
    {"request_image", handle_request_image, "Request camera image"},
    {"request_setting", handle_request_setting, "Request current settings"},
    {"request_rec_url", handle_request_rec_url, "Request recording URL"},
    
    // Add more commands as needed
    {NULL, NULL, NULL}  // Terminator
};

// Find and execute command handler
gboolean execute_command(JsonObject *object, const gchar *peer_id) {
    for (int i = 0; command_table[i].command != NULL; i++) {
        if (json_object_has_member(object, command_table[i].command)) {
            glog_trace("Executing command: %s - %s\n", 
                      command_table[i].command, 
                      command_table[i].description);
            return command_table[i].handler(object, peer_id);
        }
    }
    
    // If no command found, log available commands
    glog_trace("Unknown command. Available commands:\n");
    for (int i = 0; command_table[i].command != NULL; i++) {
        glog_trace("  - %s: %s\n", command_table[i].command, command_table[i].description);
    }
    
    return FALSE;
}

// New improved process_message_cmd function
gboolean process_message_cmd_improved(gJSONObj *jsonObj) {
    JsonNode *node = json_object_get_member(jsonObj->object, "message");
    if (!node) {
        glog_trace("Cannot get message node\n");
        return FALSE;
    }
    
    JsonObject *object = json_node_get_object(node);
    
    // Get peer_id if available
    const gchar *peer_id = NULL;
    cockpit_json_get_string(object, "peer_id", NULL, &peer_id, FALSE);
    
    // Execute command using table-driven approach
    return execute_command(object, peer_id);
}