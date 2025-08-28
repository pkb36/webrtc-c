#ifndef __GSTREAM_CONTROL_H__
#define __GSTREAM_CONTROL_H__

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "json_utils.h"

// Command handler function type
typedef gboolean (*CommandHandler)(JsonObject *object, const gchar *peer_id);

// Command definition structure
typedef struct {
    const char *command;
    CommandHandler handler;
    const char *description;
} CommandEntry;

// PTZ command structure
typedef struct {
    const char *name;
    const unsigned char *data;
    size_t length;
} PtzCommand;

// Public functions
gboolean process_message_cmd(gJSONObj *jsonObj);
gboolean process_message_cmd_improved(gJSONObj *jsonObj);
gboolean execute_command(JsonObject *object, const gchar *peer_id);
gboolean apply_setting(void);
void send_ptz_serial_data(const gchar *str);
void send_pipe_data(const gchar *str);
void goto_ptz_preset(int index, int use_auto);

// Initialization and cleanup
void start_heartbit(int timeout);
void kill_heartbit(void);

// Server communication
gboolean send_register_with_server(SoupWebsocketConnection *ws_conn);
void send_camera_info_to_server(void);
void send_image_to_peer(const gchar *peer_id, const gchar *source);
void send_setting_to_peer(const gchar *peer_id);
void send_rec_url_to_peer(const gchar *peer_id, const gchar *check_str);

// Utility functions
void set_camera_dn_mode(int camera_dn_mode);
void remove_data_path(const gchar *remove_path);
gchar *image_to_base64(const gchar *source);
void get_cur_dir(char *cwd, int size);

// Command handlers (to be implemented)
gboolean handle_ptz_command(JsonObject *object, const gchar *peer_id);
gboolean handle_ptz_move(JsonObject *object, const gchar *peer_id);
gboolean handle_ptz_move_control(JsonObject *object, const gchar *peer_id);
gboolean handle_ptz_move_point(JsonObject *object, const gchar *peer_id);
gboolean handle_record(JsonObject *object, const gchar *peer_id);
gboolean handle_analysis(JsonObject *object, const gchar *peer_id);
gboolean handle_color_palette(JsonObject *object, const gchar *peer_id);
gboolean handle_send_event(JsonObject *object, const gchar *peer_id);
gboolean handle_ptz_preset_operations(JsonObject *object, const gchar *peer_id);
gboolean handle_request_operations(JsonObject *object, const gchar *peer_id);
gboolean handle_camera_dn_mode(JsonObject *object, const gchar *peer_id);

#endif /* __GSTREAM_CONTROL_H__ */