#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include "device_setting.h"
#include "logging.h"
#include "serial_comm.h"
#include "nvds_process.h"

// Thread safety for file operations
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Helper function for parsing optional integer fields
static void parse_optional_int(JsonObject *obj, const char *key, int *dest, int default_value) {
    if (json_object_has_member(obj, key)) {
        *dest = json_object_get_int_member(obj, key);
        glog_trace("Parsed %s: %d\n", key, *dest);
    } else {
        *dest = default_value;
        if (default_value != 0) {
            glog_trace("Using default for %s: %d\n", key, default_value);
        }
    }
}

// Helper function for parsing optional string fields
static void parse_optional_string(JsonObject *obj, const char *key, char *dest, size_t dest_size, const char *default_value) {
    if (json_object_has_member(obj, key)) {
        const char *value = json_object_get_string_member(obj, key);
        if (value) {
            g_strlcpy(dest, value, dest_size);
            glog_trace("Parsed %s: %s\n", key, value);
        } else {
            g_strlcpy(dest, default_value, dest_size);
        }
    } else {
        g_strlcpy(dest, default_value, dest_size);
    }
}

#if MINDULE_INCLUDE

gboolean load_ranch_setting(const char *file_name, RanchSetting* setting)
{
  JsonParser *parser;
  GError *error;
  JsonNode *root;
  JsonObject *object;   

  parser = json_parser_new();
  error = NULL;
  json_parser_load_from_file(parser, file_name, &error);
  if (error) {
    glog_trace("Unable to parse file '%s': %s\n", file_name, error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonReader *reader = json_reader_new (json_parser_get_root (parser));
  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    g_object_unref (parser);
    return FALSE;
  }

  object = json_node_get_object (root);
  JsonArray *ranch_pos_array = json_object_get_array_member(object, "ranch_pos");
  guint array_size = json_array_get_length(ranch_pos_array);

  for (guint i = 0; i < array_size; ++i) {
      const gchar *pos = json_array_get_string_element(ranch_pos_array, i);
      glog_trace("Ranch Pos %d: %s\n", i, pos);

      unsigned char str_array[MAX_RANCH_POS+1] = {0};
      int data_len = parse_string_to_hex(pos, str_array, MAX_RANCH_POS+1);
      for( int j = 0 ; j < data_len ; j++){
        setting->ranch_pos[i][j] = str_array[j];
      }

      //업데이트 PTZ 설정 ...
      if (str_array[0]) 
        update_ranch_pos(i, &str_array[1], 1);
  }

  g_object_unref (reader);
  g_object_unref(parser);
    
  return TRUE;
}
#endif

void display_confidence_duration(void) {
    static const char* class_names[] = {
        "CLASS_NORMAL_COW",
        "CLASS_HEAT_COW", 
        "CLASS_FLIP_COW",
        "CLASS_LABOR_SIGN_COW",
        "CLASS_NORMAL_COW_SITTING"
    };
    
    for (int i = CLASS_NORMAL_COW; i < NUM_CLASSES; i++) {
        const char *name = (i < sizeof(class_names)/sizeof(class_names[0])) ? 
                          class_names[i] : "UNKNOWN_CLASS";
        glog_trace("%s threshold:%.2f, duration:%d\n", 
                   name, threshold_confidence[i], threshold_event_duration[i]);
    }
}

// Initialize device setting with default values
void init_default_device_setting(DeviceSetting* setting) {
    memset(setting, 0, sizeof(DeviceSetting));
    
    // Set non-zero defaults
    setting->auto_ptz_move_speed = DEFAULT_AUTO_PTZ_MOVE_SPEED;
    setting->ptz_move_speed = DEFAULT_PTZ_MOVE_SPEED;
    setting->heat_time = DEFAULT_EVENT_DURATION;
    setting->flip_time = DEFAULT_EVENT_DURATION;
    setting->labor_sign_time = DEFAULT_EVENT_DURATION;
    setting->over_temp_time = DEFAULT_EVENT_DURATION;
    setting->temp_diff_threshold = DEFAULT_TEMP_DIFF_THRESHOLD;
    setting->opt_flow_apply = 1;
    setting->resnet50_threshold = RESNET50_THRESHOLD_DEFAULT;
    setting->heat_event_apply = 1;
    setting->flip_event_apply = 1;
    setting->labor_event_apply = 1;
}

// Parse PTZ preset arrays from JSON
static void parse_ptz_presets(JsonObject *object, DeviceSetting *setting) {
    JsonArray *ptz_preset_array = json_object_get_array_member(object, "ptz_preset");
    if (ptz_preset_array) {
        guint array_size = json_array_get_length(ptz_preset_array);
        for (guint i = 0; i < array_size && i < MAX_PTZ_PRESET; ++i) {
            const gchar *preset = json_array_get_string_element(ptz_preset_array, i);
            if (preset) {
                glog_trace("PTZ Preset %d: %s\n", i, preset);
                
                unsigned char str_array[PTZ_POS_SIZE+1] = {0};
                int data_len = parse_string_to_hex(preset, str_array, PTZ_POS_SIZE+1);
                for (int j = 0; j < data_len && j < PTZ_POS_SIZE; j++) {
                    setting->ptz_preset[i][j] = str_array[j];
                }
                
                if (str_array[0]) {
                    update_ptz_pos(i, &str_array[1], 0);
                }
            }
        }
    }
    
    JsonArray *auto_ptz_preset_array = json_object_get_array_member(object, "auto_ptz_preset");
    if (auto_ptz_preset_array) {
        guint array_size = json_array_get_length(auto_ptz_preset_array);
        for (guint i = 0; i < array_size && i < MAX_PTZ_PRESET; ++i) {
            const gchar *preset = json_array_get_string_element(auto_ptz_preset_array, i);
            if (preset) {
                glog_trace("Auto PTZ Preset %d: %s\n", i, preset);
                
                unsigned char str_array[PTZ_POS_SIZE+1] = {0};
                int data_len = parse_string_to_hex(preset, str_array, PTZ_POS_SIZE+1);
                for (int j = 0; j < data_len && j < PTZ_POS_SIZE; j++) {
                    setting->auto_ptz_preset[i][j] = str_array[j];
                }
                
                if (str_array[0]) {
                    update_ptz_pos(i, &str_array[1], 1);
                }
            }
        }
    }
}

// Parse threshold settings and update global arrays
static void parse_threshold_settings(JsonObject *object, DeviceSetting *setting) {
    parse_optional_int(object, "normal_threshold", &setting->normal_threshold, 0);
    if (setting->normal_threshold > 0) {
        threshold_confidence[CLASS_NORMAL_COW] = (float)setting->normal_threshold / 100.0f;
    }
    
    parse_optional_int(object, "heat_threshold", &setting->heat_threshold, 0);
    if (setting->heat_threshold > 0) {
        threshold_confidence[CLASS_HEAT_COW] = (float)setting->heat_threshold / 100.0f;
    }
    
    parse_optional_int(object, "flip_threshold", &setting->flip_threshold, 0);
    if (setting->flip_threshold > 0) {
        threshold_confidence[CLASS_FLIP_COW] = (float)setting->flip_threshold / 100.0f;
    }
    
    parse_optional_int(object, "labor_sign_threshold", &setting->labor_sign_threshold, 0);
    if (setting->labor_sign_threshold > 0) {
        threshold_confidence[CLASS_LABOR_SIGN_COW] = (float)setting->labor_sign_threshold / 100.0f;
    }
    
    parse_optional_int(object, "normal_sitting_threshold", &setting->normal_sitting_threshold, 0);
    if (setting->normal_sitting_threshold > 0) {
        threshold_confidence[CLASS_NORMAL_COW_SITTING] = (float)setting->normal_sitting_threshold / 100.0f;
    }
}

// Parse time duration settings and update global arrays
static void parse_duration_settings(JsonObject *object, DeviceSetting *setting) {
    parse_optional_int(object, "heat_time", &setting->heat_time, DEFAULT_EVENT_DURATION);
    threshold_event_duration[CLASS_HEAT_COW] = setting->heat_time;
    
    parse_optional_int(object, "flip_time", &setting->flip_time, DEFAULT_EVENT_DURATION);
    threshold_event_duration[CLASS_FLIP_COW] = setting->flip_time;
    
    parse_optional_int(object, "labor_sign_time", &setting->labor_sign_time, DEFAULT_EVENT_DURATION);
    threshold_event_duration[CLASS_LABOR_SIGN_COW] = setting->labor_sign_time;
}

gboolean load_device_setting(const char *file_name, DeviceSetting* setting) {
    pthread_mutex_lock(&file_mutex);
    
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    gboolean result = FALSE;
    
    // Initialize with defaults
    init_default_device_setting(setting);
    
    if (!json_parser_load_from_file(parser, file_name, &error)) {
        glog_trace("Unable to parse file '%s': %s\n", file_name, error->message);
        g_error_free(error);
        goto cleanup;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        glog_trace("JSON root is not an object\n");
        goto cleanup;
    }
    
    JsonObject *object = json_node_get_object(root);
    JsonReader *reader = json_reader_new(root);
    // Parse basic settings
    parse_optional_int(object, "color_platte", &setting->color_pallet, 0);
    parse_optional_int(object, "record_status", &setting->record_status, 0);
    parse_optional_int(object, "analysis_status", &setting->analysis_status, 0);
    parse_optional_int(object, "auto_ptz_move_speed", &setting->auto_ptz_move_speed, DEFAULT_AUTO_PTZ_MOVE_SPEED);
    parse_optional_int(object, "ptz_move_speed", &setting->ptz_move_speed, DEFAULT_PTZ_MOVE_SPEED);
    parse_optional_int(object, "enable_event_notify", &setting->enable_event_notify, 0);
    parse_optional_int(object, "camera_dn_mode", &setting->camera_dn_mode, CAMERA_MODE_AUTO);
    
    // Parse string fields
    parse_optional_string(object, "auto_ptz_seq", setting->auto_ptz_seq, sizeof(setting->auto_ptz_seq), ""); 

    // Parse PTZ presets
    parse_ptz_presets(object, setting);

    // Parse detection and analysis settings
    parse_optional_int(object, "nv_interval", &setting->nv_interval, 0);
    parse_optional_int(object, "opt_flow_threshold", &setting->opt_flow_threshold, 0);
    
    // Parse threshold settings
    parse_threshold_settings(object, setting); 


    // Parse camera and AI settings
    parse_optional_int(object, "display_temp", &setting->display_temp, 0);
    parse_optional_int(object, "camera_index", &setting->camera_index, 0);
    if (setting->camera_index != 0) {
        g_source_cam_idx = setting->camera_index;
    }
    
    parse_optional_int(object, "resnet50_apply", &setting->resnet50_apply, 0);
    parse_optional_int(object, "resnet50_threshold", &setting->resnet50_threshold, RESNET50_THRESHOLD_DEFAULT);
    parse_optional_int(object, "opt_flow_apply", &setting->opt_flow_apply, 1); 


    // Parse duration settings
    parse_duration_settings(object, setting);
    
    // Parse temperature settings
    parse_optional_int(object, "temp_diff_threshold", &setting->temp_diff_threshold, DEFAULT_TEMP_DIFF_THRESHOLD);
    parse_optional_int(object, "over_temp_time", &setting->over_temp_time, DEFAULT_EVENT_DURATION);
    parse_optional_int(object, "temp_correction", &setting->temp_correction, 0);
    parse_optional_int(object, "temp_apply", &setting->temp_apply, 0);
    parse_optional_int(object, "threshold_upper_temp", &setting->threshold_upper_temp, THRESHOLD_UPPER_TEMP_DEFAULT);
    parse_optional_int(object, "threshold_under_temp", &setting->threshold_under_temp, THRESHOLD_UNDER_TEMP_DEFAULT);
    parse_optional_int(object, "show_normal_text", &setting->show_normal_text, 0); 

    // Parse event settings
    parse_optional_int(object, "event_buffer_apply", &setting->event_buffer_apply, 0);
    parse_optional_int(object, "heat_event_apply", &setting->heat_event_apply, 1);
    parse_optional_int(object, "flip_event_apply", &setting->flip_event_apply, 1);
    parse_optional_int(object, "labor_event_apply", &setting->labor_event_apply, 1);
    
    display_confidence_duration();
    result = TRUE;
    
cleanup:
    if (reader) g_object_unref(reader);
    if (parser) g_object_unref(parser);
    pthread_mutex_unlock(&file_mutex);
    
    return result;
}


// Remove unused function - sprintf can be used directly


// Generate PTZ preset JSON string
static void generate_ptz_preset_json(const DeviceSetting *setting, char *ptz_code, char *auto_ptz_code, size_t buffer_size) {
    char temp[16];
    
    // Generate regular PTZ presets
    ptz_code[0] = '\0';
    for (int i = 0; i < MAX_PTZ_PRESET; i++) {
        snprintf(temp, sizeof(temp), "\"%02x", setting->ptz_preset[i][0]);
        g_strlcat(ptz_code, temp, buffer_size);
        
        for (int j = 1; j < PTZ_POS_SIZE; j++) {
            snprintf(temp, sizeof(temp), ",%02x", setting->ptz_preset[i][j]);
            g_strlcat(ptz_code, temp, buffer_size);
        }
        
        if (i < MAX_PTZ_PRESET - 1) {
            g_strlcat(ptz_code, "\",\n", buffer_size);
        } else {
            g_strlcat(ptz_code, "\"\n", buffer_size);
        }
    }
    
    // Generate auto PTZ presets
    auto_ptz_code[0] = '\0';
    for (int i = 0; i < MAX_PTZ_PRESET; i++) {
        snprintf(temp, sizeof(temp), "\"%02x", setting->auto_ptz_preset[i][0]);
        g_strlcat(auto_ptz_code, temp, buffer_size);
        
        for (int j = 1; j < PTZ_POS_SIZE; j++) {
            snprintf(temp, sizeof(temp), ",%02x", setting->auto_ptz_preset[i][j]);
            g_strlcat(auto_ptz_code, temp, buffer_size);
        }
        
        if (i < MAX_PTZ_PRESET - 1) {
            g_strlcat(auto_ptz_code, "\",\n", buffer_size);
        } else {
            g_strlcat(auto_ptz_code, "\"\n", buffer_size);
        }
    }
}

// Write device settings to JSON file using json-glib
static gboolean write_settings_to_json(const char *file_name, const DeviceSetting *setting) {
    JsonBuilder *builder = json_builder_new();
    JsonGenerator *generator = json_generator_new();
    gboolean result = FALSE;
    
    // Build JSON object
    json_builder_begin_object(builder);
    
    // Add all settings
    json_builder_set_member_name(builder, "color_platte");
    json_builder_add_int_value(builder, setting->color_pallet);
    
    json_builder_set_member_name(builder, "record_status");
    json_builder_add_int_value(builder, setting->record_status);
    
    json_builder_set_member_name(builder, "analysis_status");
    json_builder_add_int_value(builder, setting->analysis_status);
    
    json_builder_set_member_name(builder, "auto_ptz_seq");
    json_builder_add_string_value(builder, setting->auto_ptz_seq);
    
    // Add PTZ presets as hex string arrays
    json_builder_set_member_name(builder, "ptz_preset");
    json_builder_begin_array(builder);
    for (int i = 0; i < MAX_PTZ_PRESET; i++) {
        char hex_string[PTZ_POS_SIZE * 3 + 10];
        snprintf(hex_string, sizeof(hex_string), "%02x", setting->ptz_preset[i][0]);
        for (int j = 1; j < PTZ_POS_SIZE; j++) {
            char temp[4];
            snprintf(temp, sizeof(temp), ",%02x", setting->ptz_preset[i][j]);
            g_strlcat(hex_string, temp, sizeof(hex_string));
        }
        json_builder_add_string_value(builder, hex_string);
    }
    json_builder_end_array(builder);
    
    json_builder_set_member_name(builder, "auto_ptz_preset");
    json_builder_begin_array(builder);
    for (int i = 0; i < MAX_PTZ_PRESET; i++) {
        char hex_string[PTZ_POS_SIZE * 3 + 10];
        snprintf(hex_string, sizeof(hex_string), "%02x", setting->auto_ptz_preset[i][0]);
        for (int j = 1; j < PTZ_POS_SIZE; j++) {
            char temp[4];
            snprintf(temp, sizeof(temp), ",%02x", setting->auto_ptz_preset[i][j]);
            g_strlcat(hex_string, temp, sizeof(hex_string));
        }
        json_builder_add_string_value(builder, hex_string);
    }
    json_builder_end_array(builder);
    
    // Add remaining integer settings
    json_builder_set_member_name(builder, "auto_ptz_move_speed");
    json_builder_add_int_value(builder, setting->auto_ptz_move_speed);
    
    json_builder_set_member_name(builder, "ptz_move_speed");
    json_builder_add_int_value(builder, setting->ptz_move_speed);
    
    json_builder_set_member_name(builder, "enable_event_notify");
    json_builder_add_int_value(builder, setting->enable_event_notify);
    
    json_builder_set_member_name(builder, "camera_dn_mode");
    json_builder_add_int_value(builder, setting->camera_dn_mode);
    
    json_builder_set_member_name(builder, "nv_interval");
    json_builder_add_int_value(builder, setting->nv_interval);
    
    json_builder_set_member_name(builder, "opt_flow_threshold");
    json_builder_add_int_value(builder, setting->opt_flow_threshold);
    
    json_builder_set_member_name(builder, "opt_flow_apply");
    json_builder_add_int_value(builder, setting->opt_flow_apply);
    
    json_builder_set_member_name(builder, "resnet50_threshold");
    json_builder_add_int_value(builder, setting->resnet50_threshold);
    
    json_builder_set_member_name(builder, "resnet50_apply");
    json_builder_add_int_value(builder, setting->resnet50_apply);
    
    json_builder_set_member_name(builder, "normal_threshold");
    json_builder_add_int_value(builder, setting->normal_threshold);
    
    json_builder_set_member_name(builder, "heat_threshold");
    json_builder_add_int_value(builder, setting->heat_threshold);
    
    json_builder_set_member_name(builder, "flip_threshold");
    json_builder_add_int_value(builder, setting->flip_threshold);
    
    json_builder_set_member_name(builder, "labor_sign_threshold");
    json_builder_add_int_value(builder, setting->labor_sign_threshold);
    
    json_builder_set_member_name(builder, "normal_sitting_threshold");
    json_builder_add_int_value(builder, setting->normal_sitting_threshold);
    
    json_builder_set_member_name(builder, "display_temp");
    json_builder_add_int_value(builder, setting->display_temp);
    
    json_builder_set_member_name(builder, "temp_diff_threshold");
    json_builder_add_int_value(builder, setting->temp_diff_threshold);
    
    json_builder_set_member_name(builder, "camera_index");
    json_builder_add_int_value(builder, setting->camera_index);
    
    json_builder_set_member_name(builder, "heat_time");
    json_builder_add_int_value(builder, setting->heat_time);
    
    json_builder_set_member_name(builder, "flip_time");
    json_builder_add_int_value(builder, setting->flip_time);
    
    json_builder_set_member_name(builder, "labor_sign_time");
    json_builder_add_int_value(builder, setting->labor_sign_time);
    
    json_builder_set_member_name(builder, "over_temp_time");
    json_builder_add_int_value(builder, setting->over_temp_time);
    
    json_builder_set_member_name(builder, "temp_correction");
    json_builder_add_int_value(builder, setting->temp_correction);
    
    json_builder_set_member_name(builder, "threshold_upper_temp");
    json_builder_add_int_value(builder, setting->threshold_upper_temp);
    
    json_builder_set_member_name(builder, "threshold_under_temp");
    json_builder_add_int_value(builder, setting->threshold_under_temp);
    
    json_builder_set_member_name(builder, "temp_apply");
    json_builder_add_int_value(builder, setting->temp_apply);
    
    json_builder_set_member_name(builder, "show_normal_text");
    json_builder_add_int_value(builder, setting->show_normal_text);
    
    // Add event settings
    json_builder_set_member_name(builder, "event_buffer_apply");
    json_builder_add_int_value(builder, setting->event_buffer_apply);
    
    json_builder_set_member_name(builder, "heat_event_apply");
    json_builder_add_int_value(builder, setting->heat_event_apply);
    
    json_builder_set_member_name(builder, "flip_event_apply");
    json_builder_add_int_value(builder, setting->flip_event_apply);
    
    json_builder_set_member_name(builder, "labor_event_apply");
    json_builder_add_int_value(builder, setting->labor_event_apply);
    
    json_builder_end_object(builder);
    
    // Generate JSON and write to file
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    
    GError *error = NULL;
    if (json_generator_to_file(generator, file_name, &error)) {
        result = TRUE;
    } else {
        glog_trace("Failed to write JSON file: %s\n", error->message);
        g_error_free(error);
    }
    
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    
    return result;
}


gboolean update_setting(const char *file_name, DeviceSetting* setting) {
    glog_trace("Updating device settings: %s\n", file_name);
    
    pthread_mutex_lock(&file_mutex);
    
    char temp_file[512];
    char backup_file[512];
    gboolean result = FALSE;
    
    // Generate file names
    snprintf(temp_file, sizeof(temp_file), "%s.tmp", file_name);
    snprintf(backup_file, sizeof(backup_file), "%s.bak", file_name);
    
    // Backup existing file
    if (access(file_name, F_OK) == 0) {
        if (rename(file_name, backup_file) != 0) {
            glog_trace("Failed to backup existing file: %s\n", strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            return FALSE;
        }
    }
    
    // Write to temporary file using json-glib
    if (write_settings_to_json(temp_file, setting)) {
        // Apply file permissions from backup
        struct stat st;
        if (access(backup_file, F_OK) == 0 && stat(backup_file, &st) == 0) {
            chmod(temp_file, st.st_mode);
            chown(temp_file, st.st_uid, st.st_gid);
        } else {
            chmod(temp_file, 0664);
        }
        
        // Atomically move temp file to final location
        if (rename(temp_file, file_name) == 0) {
            // Success - remove backup
            if (access(backup_file, F_OK) == 0) {
                unlink(backup_file);
            }
            glog_trace("Successfully updated device setting file: %s\n", file_name);
            result = TRUE;
        } else {
            glog_trace("Failed to move temp file to final location: %s\n", strerror(errno));
            unlink(temp_file);
            // Restore backup
            if (access(backup_file, F_OK) == 0) {
                rename(backup_file, file_name);
            }
        }
    } else {
        glog_trace("Failed to write settings to temp file\n");
        unlink(temp_file);
        // Restore backup
        if (access(backup_file, F_OK) == 0) {
            rename(backup_file, file_name);
        }
    }
    
    pthread_mutex_unlock(&file_mutex);
    return result;
}

// device_setting.c에 추가할 함수들
gboolean validate_settings_file(const char *file_name)
{
  struct stat st;
  if (stat(file_name, &st) != 0) {
    glog_trace("Settings file does not exist: %s\n", file_name);
    return FALSE;
  }
  
  // 파일 크기 검증 (최소 10바이트 이상이어야 함)
  if (st.st_size < 10) {
    glog_trace("Settings file is too small (size: %ld): %s\n", st.st_size, file_name);
    return FALSE;
  }
  
  // JSON 형식 검증
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  
  if (!json_parser_load_from_file(parser, file_name, &error)) {
    glog_trace("Invalid JSON in settings file: %s, error: %s\n", file_name, error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }
  
  g_object_unref(parser);
  return TRUE;
}

void ensure_valid_settings_file(const char *file_name, DeviceSetting* default_setting)
{
  char backup_file[512];
  char good_backup_file[512];
  
  snprintf(backup_file, sizeof(backup_file), "%s.bak", file_name);
  snprintf(good_backup_file, sizeof(good_backup_file), "%s.good", file_name);
  
  // 메인 파일 검증
  if (!validate_settings_file(file_name)) {
    glog_trace("Main settings file is invalid, attempting recovery...\n");
    
    // 1. 먼저 .bak 파일에서 복구 시도
    if (validate_settings_file(backup_file)) {
      glog_trace("Restoring from .bak file...\n");
      if (rename(backup_file, file_name) == 0) {
        glog_trace("Successfully restored from .bak backup\n");
        return;
      }
    }
    
    // 2. .good 파일에서 복구 시도
    if (validate_settings_file(good_backup_file)) {
      glog_trace("Restoring from .good file...\n");
      
      FILE *src = fopen(good_backup_file, "rb");
      FILE *dst = fopen(file_name, "wb");
      
      if (src && dst) {
        char buffer[4096];
        size_t bytes;
        
        while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
          fwrite(buffer, 1, bytes, dst);
        }
        
        glog_trace("Successfully restored from .good backup\n");
      }
      
      if (src) fclose(src);
      if (dst) fclose(dst);
      
      return;
    }
    
    // 3. 모든 백업이 실패하면 기본값으로 생성
    glog_trace("All backups failed, creating new settings file with default values...\n");
    
    // 기본값 설정
    memset(default_setting, 0, sizeof(DeviceSetting));
    default_setting->auto_ptz_move_speed = 0x08;
    default_setting->ptz_move_speed = 0x10;
    default_setting->heat_time = 15;
    default_setting->flip_time = 15;
    default_setting->labor_sign_time = 15;
    default_setting->over_temp_time = 15;
    default_setting->temp_diff_threshold = 7;
    default_setting->opt_flow_apply = 1;
    
    update_setting(file_name, default_setting);
  }
}

void create_settings_backup(const char *file_name)
{
  char backup_file[512];
  char good_backup_file[512];
  
  snprintf(backup_file, sizeof(backup_file), "%s.bak", file_name);
  snprintf(good_backup_file, sizeof(good_backup_file), "%s.good", file_name);
  
  // 현재 파일이 정상이면 .good 백업 생성
  if (validate_settings_file(file_name)) {
    // 기존 .good 파일이 있으면 삭제
    if (access(good_backup_file, F_OK) == 0) {
      unlink(good_backup_file);
    }
    
    // 정상적인 파일을 .good으로 백업
    FILE *src = fopen(file_name, "rb");
    FILE *dst = fopen(good_backup_file, "wb");
    
    if (src && dst) {
      char buffer[4096];
      size_t bytes;
      
      while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
      }
      
      glog_trace("Created good backup: %s\n", good_backup_file);
    }
    
    if (src) fclose(src);
    if (dst) fclose(dst);
  }
}

#ifdef TEST_SETTING
int main(int argc, char *argv[])
{
  DeviceSetting setting ={};
  load_device_setting("device_setting.json", &setting);

  glog_trace("color_pallet  %d \n", setting.color_pallet);  
  glog_trace("record_status  %d \n", setting.record_status);  

  for(int i = 0 ; i < MAX_PTZ_PRESET ; i++){
    glog_trace("ptz_preset  %d : %d\n", i , setting.ptz_preset[i][0]);  
  }

/*
  char ptz_code[360];
  char temp[8];
  for(int i = 0 ; i < MAX_PTZ_PRESET ; i++){
    sprintf(temp, "\"%02x",setting.ptz_preset[i][0]);
    strcat(ptz_code,temp);
    for(int j = 1 ; j < 11 ; j++){
      sprintf(temp, ",%02x",setting.ptz_preset[i][j]);
      strcat(ptz_code,temp);
    }
    if (i < MAX_PTZ_PRESET-1 ) 
      strcat(ptz_code,"\",\n");
    else 
      strcat(ptz_code,"\"\n");
  }
  glog_trace("ptz_code \n  %s ", ptz_code);  
  */

 update_setting("device_setting_2.json", &setting);

 DeviceSetting setting2 ={};  
 load_device_setting("device_setting_2.json", &setting2);
 glog_trace("color_pallet  %d \n", setting2.color_pallet);  
 glog_trace("record_status  %d \n", setting2.record_status);  


}
#endif


