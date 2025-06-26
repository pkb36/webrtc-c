/* For json config */
#include <json-glib/json-glib.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <curl/curl.h>
#include "config.h"
#include "nvds_process.h"

extern int get_service_address(char *address);

int load_config(const char *file_name, WebRTCConfig* config, CurlIinfoType *curl_info)
{
   JsonParser *parser;
   GError *error;
   JsonNode *root;
   JsonObject *object, *child;   

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
  if (json_object_has_member (object, "camera_id")) {
      const char* value = json_object_get_string_member (object, "camera_id");
      glog_trace("parse member %s : %s\n", "camera_id", value);  
      config->camera_id = strdup(value);
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "tty")) {
      child = json_object_get_object_member (object, "tty");
      const char* value = json_object_get_string_member (child, "name");
      int value2 = json_object_get_int_member (child, "baudrate");
      glog_trace("parse member %s : %s, %d\n", "tty", value, value2);  
      config->tty_name = strdup(value);
      config->tty_buadrate = value2;

  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "max_stream_cnt")) {
      int value = json_object_get_int_member (object, "max_stream_cnt");
      glog_trace("parse member %s : %d\n", "max_stream_cnt", value);  
      config->max_stream_cnt = value;
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "stream_base_port")) {
      int value = json_object_get_int_member (object, "stream_base_port");
      glog_trace("parse member %s : %d\n", "stream_base_port", value);  
      config->stream_base_port = value;
  } else {
    return FALSE;
  }
  
  if (json_object_has_member (object, "device_cnt")) {
      int value = json_object_get_int_member (object, "device_cnt");
      glog_trace("parse member %s : %d\n", "device_cnt", value);  
      config->device_cnt = value;
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "comm_socket_port")) {
      int value = json_object_get_int_member (object, "comm_socket_port");
      glog_trace("parse member %s : %d\n", "device_cnt", value);  
      config->comm_socket_port = value;
  } else {
    return FALSE;
  }

  for(int i = 0 ; i < config->device_cnt ; i++){
    char video_name[6] = "videox";
    video_name[5] = '0' + i;
    if (json_object_has_member (object, video_name)) {
        child = json_object_get_object_member (object, video_name);
        const char* value_src = json_object_get_string_member (child, "src");
        const char* value_record = json_object_get_string_member (child, "record");
        const char* value_infer = json_object_get_string_member (child, "infer");
        const char* value_enc = json_object_get_string_member (child, "enc");
        const char* value_enc2 = json_object_get_string_member (child, "enc2");
        const char* value_snapshot = json_object_get_string_member (child, "snapshot");
        config->video_src[i] = strdup(value_src);
        config->video_infer[i] = strdup(value_infer);
        config->video_enc[i] = strdup(value_enc);
        config->video_enc2[i] = strdup(value_enc2);
        config->record_enc[i] = strdup(value_record);
        config->snapshot_enc[i] = strdup(value_snapshot);

        glog_trace("parse member %s : %s, %s, %s, %s, %s\n", 
            video_name, value_src, value_infer, value_enc, value_enc2, value_snapshot);
    } else {
      return FALSE;
    }
  }

  if (json_object_has_member (object, "server_ip")) {
      const char* value = json_object_get_string_member (object, "server_ip");
      glog_trace("parse member %s : %s\n", "server_ip", value);  
      config->server_ip = strdup(value);
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "snapshot_path")) {
      const char* value = json_object_get_string_member (object, "snapshot_path");
      glog_trace("parse member %s : %s\n", "snapshot_path", value);  
      config->snapshot_path = strdup(value);
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "status_timer_interval")) {
      int value = json_object_get_int_member (object, "status_timer_interval");
      glog_trace("parse member %s : %d\n", "status_timer_interval", value);  
      config->status_timer_interval = value;
  } else {
    return FALSE;
  }

  
  if (json_object_has_member (object, "device_setting_path")) {
      const char* value = json_object_get_string_member (object, "device_setting_path");
      glog_trace("parse member %s : %s\n", "device_setting_path", value);  
      config->device_setting_path = strdup(value);
  } else {
    return FALSE;
  }


//event server connection info
  if (json_object_has_member (object, "event_user_id")) {
      const char* value = json_object_get_string_member(object, "event_user_id");
      glog_trace("parse member %s : %s\n", "event_user_id", value);  
      strcpy(curl_info->phone, value);
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "event_user_pw")) {
      const char* value = json_object_get_string_member(object, "event_user_pw");
      glog_trace("parse member %s : %s\n", "event_user_pw", value);  
      strcpy(curl_info->password, value);
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "event_server_ip")) {
      const char* value = json_object_get_string_member(object, "event_server_ip");
      glog_trace("parse member %s : %s\n", "event_server_ip", value);  
      strcpy(curl_info->server_ip, value);
      curl_info->port = 0;
  } else {
    return FALSE;
  }
  
  strcpy(curl_info->snapshot_path, config->snapshot_path);

  //record infomation
  if (json_object_has_member (object, "record_path")) {
      const char* value = json_object_get_string_member(object, "record_path");
      glog_trace("parse member %s : %s\n", "record_path", value);  
      config->record_path = strdup(value);
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "record_duration")) {
      int value = json_object_get_int_member(object, "record_duration");
      glog_trace("parse member %s : %d\n", "record_duration", value);  
      config->record_duration = value;
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "event_buf_time")) {
      int value = json_object_get_int_member(object, "event_buf_time");
      glog_trace("parse member %s : %d\n", "event_buf_time", value);  
      config->event_buf_time = value;
  } else {
    return FALSE;
  }

  if (json_object_has_member (object, "record_enc_index")) {
      int value = json_object_get_int_member(object, "record_enc_index");
      glog_trace("parse member %s : %d\n", "record_enc_index", value);  
      config->record_enc_index = value;
  } else {
    config->record_enc_index = 0;
  }

  if (json_object_has_member (object, "event_record_enc_index")) {
      int value = json_object_get_int_member(object, "event_record_enc_index");
      glog_trace("parse member %s : %d\n", "event_record_enc_index", value);  
      config->event_record_enc_index = value;
  } else {
    config->event_record_enc_index = 0;
  }

  if (json_object_has_member (object, "http_service_port")) {
      glog_trace("http_service_port\n");  
      const char* value = json_object_get_string_member(object, "http_service_port");
      config->http_service_port = atoi(value);
      glog_trace("parse member %s : %d\n", "http_service_port", config->http_service_port);  
  } else {
    config->http_service_port = 0;
  }

  update_http_service_ip(config);

  g_object_unref (reader);
  g_object_unref(parser);
    
  return TRUE;
}


void free_config(WebRTCConfig* config)
{
  free(config->camera_id);
  free(config->tty_name);
  for(int i = 0 ; i < 2 ; i++){
    free(config->video_src[i]);
    free(config->video_enc[i]);
  }
  free(config->snapshot_path);
  free(config->device_setting_path);
}


// Callback function to handle the response
size_t write_callback(void *ptr, size_t size, size_t nmemb, char *data) {
    strcat(data, ptr);  // Append the response to the data buffer
    return size * nmemb;
}

// Function to get the global IP address using ipify API
char* get_global_ip() 
{
    CURL *curl;
    CURLcode res;
    char *ip = (char*)malloc(100);  // Allocate memory for the IP address
    ip[0] = '\0';  // Initialize the IP string as empty

    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        // Set the URL for ipify API
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.ipify.org?format=text");

        // Set the callback function to handle the response
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, ip);

        // Perform the request
        res = curl_easy_perform(curl);

        // Check if the request was successful
        if (res != CURLE_OK) {
            fprintf(stderr, "Request failed: %s\n", curl_easy_strerror(res));
            free(ip);  // Free the allocated memory in case of error
            ip = NULL;
        }

        // Cleanup
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return ip;
}


void get_local_ip(char *ip_str) 
{
    struct ifaddrs *ifap, *ifa;
    struct sockaddr_in *sa;
    char *ip_address = NULL;

    // Get the list of network interfaces
    if (getifaddrs(&ifap) == -1) {
        perror("getifaddrs");
        return;
    }
    
    // Iterate over the interfaces
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        // Check if the interface is an IPv4 address
        if (ifa->ifa_addr->sa_family == AF_INET) {
            sa = (struct sockaddr_in *) ifa->ifa_addr;
            ip_address = inet_ntoa(sa->sin_addr);
            // We check if the interface is not a loopback interface (lo)
            if (strcmp(ifa->ifa_name, "lo") != 0) {  // Exclude loopback
                printf("Local IP address of interface %s: %s\n", ifa->ifa_name, ip_address);
                strcpy(ip_str, ip_address);
                break;  // Print the first non-loopback IP address
            }
        }
    }
    // Free the memory allocated by getifaddrs
    freeifaddrs(ifap);
}


void write_lines_to_file(const char *filename, char *line1, char *line2) 
{
    FILE *file = fopen(filename, "w");  // Open the file in write mode ("w")

    if (file == NULL) {
        perror("Failed to open file");
        return;
    }

    // Write each line to the file
    fprintf(file, "%s\n", line1);  // Write line with newline
    fprintf(file, "%s\n", line2);  // Write line with newline

    fclose(file);  // Close the file
    printf("Lines written to file successfully.\n");
}


void update_http_service_ip(WebRTCConfig* config)
{
  char lines[2][100];
  char *global_ip = get_global_ip();

  lines[0][0] = 0;
  lines[1][0] = 0;
  if (global_ip) {
    glog_trace("My global IP address is: %s, port: %d\n", global_ip, config->http_service_port);
    sprintf(lines[1], "%s:%d", global_ip, config->http_service_port);
    free(global_ip);  // Don't forget to free the allocated memory
  } else {
    glog_trace("Failed to retrieve global IP address.\n");
    get_service_address(lines[1]);    
  }
  if(config->http_service_ip) 
    free(config->http_service_ip);
  config->http_service_ip = strdup(lines[1]);
  get_local_ip(lines[0]);
  if (global_ip)
    write_lines_to_file("local_ip.log", lines[0], lines[1]);
  glog_trace("local IP=%s, global IP:Port=%s\n", lines[0], lines[1]);
  glog_trace("set config->http_service_ip as %s\n", config->http_service_ip);  
}


/*
int main(int argc, char *argv[])
{
  WebRTCConfig config ={};
  load_config("config.json", &config);

  glog_trace("camera id  %d \n", config.camera_id);  
  glog_trace("tty name  %s \n", config.tty_name);  
  for(int i = 0 ; i < 2 ; i++){
    glog_trace("video config %d : %s, %d\n", i , config.video_src[i], config.video_bitrate[i]);  
  }

  free_config(&config);
}
*/


