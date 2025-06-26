#include "nvds_utils.h"

noti_queue notification_queue;

int get_error_pan(int left, int width)
{
  int bbox_center_x = left + (width / 2);
  return (bbox_center_x - CENTER_X);
}


int get_error_tilt(int top, int height)
{
  int bbox_center_y = top + (height / 2);
  return (bbox_center_y - CENTER_Y);
}


void zoomin(int count)
{
  move_and_stop_ptz(4, 100, (count * 500));
}


void *move_to_center(void *arg)
{
  int error_pan = 0, error_tilt = 0;
  error_pan = get_error_pan(g_left, g_width);
  error_tilt = get_error_pan(g_top, g_height);
  int h_speed = 30 + abs((error_pan) / 5);
  int v_speed = 2;
  int h_time_len = 50 + abs((error_pan) / 30);
  int v_time_len = 50;

  if (error_pan > 100) {
    // glog_trace("->LEFT s=%d t=%d\n", h_speed, h_time_len);
    move_and_stop_ptz(LEFT, h_speed, h_time_len);
    usleep(200 * h_speed);
    goto thread_end;
  }
  else if (error_pan < (-100)) {
    // glog_trace("->RIGHT s=%d t=%d\n", h_speed, h_time_len);
    move_and_stop_ptz(RIGHT, h_speed, h_time_len);             //another way is to move to coordiante
    usleep(200 * h_speed);
    goto thread_end;
  }

  if (error_tilt > 100) {
    // glog_trace("->TOP s=%d t=%d\n", v_speed, v_time_len);
    move_and_stop_ptz(TOP, v_speed, v_time_len);
    goto thread_end;
  }
  else if (error_tilt < (-100)) {
    // glog_trace("->BOTTOM s=%d t=%d\n", v_speed, v_time_len);
    move_and_stop_ptz(BOTTOM, v_speed, v_time_len);
    goto thread_end;
  }
thread_end:
  usleep(50000);
  g_move_to_center_running = 0;

  return NULL;
}


#define MIN_RECT_SIZE     300

void check_for_zoomin(int total_rect_size, int obj_count)
{
  int average_rect_size = total_rect_size / obj_count;

  if (average_rect_size > 0)
    glog_trace("average_rect_size=%d\n", average_rect_size);
  if (average_rect_size < MIN_RECT_SIZE) {
    glog_trace("zoomin\n");
    zoomin(1);
    wait_ptz_stop();
  }
}


void move_init()
{
  g_top = CENTER_Y;
  g_left = CENTER_X;
  g_width = 0;
  g_height = 0;
}


void move_ptz_along(int top, int left, int width, int height)
{
  static pthread_t tid;

  if (g_move_to_center_running == 1) 
    return;
  g_move_to_center_running = 1;
  g_top = top;
  g_left = left; 
  g_width = width;
  g_height = height;
  pthread_create(&tid, NULL, move_to_center, NULL);    
}

#if 0
double time_since_last_call() 
{
  static struct timespec last_time = {0, 0};  // Static to retain value across calls
  struct timespec current_time;

  // Get current time
  clock_gettime(CLOCK_MONOTONIC, &current_time);

  // If this is the first call, initialize last_time and return 0
  if (last_time.tv_sec == 0 && last_time.tv_nsec == 0) {
      last_time = current_time;
      return 0.0;
  }

  // Calculate the time difference
  double diff_sec = (current_time.tv_sec - last_time.tv_sec) +
                    (current_time.tv_nsec - last_time.tv_nsec) / 1e9;

  // If 1 second or more has passed, reset the timer and return 1.0
  if (diff_sec >= 1.0) {
      last_time = current_time;  // Reset to current time
      return 1.0;
  }

  return diff_sec;  // Return the elapsed time in seconds
}
#endif


#if 0
// Function to initialize a timer with a specific time gap
void init_timer(int timer_id, int time_gap) 
{
  if (timer_id >= 0 && timer_id < MAX_PTZ_PRESET) {
      timers[timer_id].call_count = 0;
      timers[timer_id].time_gap = time_gap;
      timers[timer_id].last_call_time = 0;  // No calls made yet
      check_time_gap(timer_id);
  }
}


// Function to check the time gap for a specific timer
bool check_time_gap(int timer_id) 
{
  if (timer_id < 0 || timer_id >= MAX_PTZ_PRESET) {
      return false;  // Invalid timer ID
  }

  time_t current_time;
  time(&current_time);  // Get current time

  // If this is the first call for the timer, set the last call time
  if (timers[timer_id].call_count == 0) {
      timers[timer_id].last_call_time = current_time;
      timers[timer_id].call_count++;
      return false;  // Time gap check hasn't started yet
  }

  // Increment the call count
  timers[timer_id].call_count++;

  // Check if the number of calls has reached the threshold
  if (timers[timer_id].call_count >= 1) {
      // Check if the time gap has been reached
      if (difftime(current_time, timers[timer_id].last_call_time) >= timers[timer_id].time_gap) {
          timers[timer_id].last_call_time = current_time;  // Update last call time
          return true;  // Time gap has been reached
      }
  }

  return false;  // Time gap hasn't been reached yet
}
#endif


// Function to initialize the AvgCalculator for each object
void init_calculator(ObjMonitor* obj) 
{
    obj->temp_avg_calculator.index = 0;
    obj->temp_avg_calculator.count = 0;
    obj->temp_avg_calculator.sum = 0;

    // Initialize buffer values to zero
    for (int i = 0; i < BUFFER_SIZE; i++) {
        obj->temp_avg_calculator.buffer[i] = 0;
    }
}

// Function to add a new value for an object and calculate the avg
void add_value_and_calculate_avg(ObjMonitor* obj, int new_value) 
{
    // Get reference to the AvgCalculator of the object
    AvgCalculator* calculator = &obj->temp_avg_calculator;

    // Remove the oldest value from the sum if the buffer is full
    if (calculator->count == BUFFER_SIZE) {
        calculator->sum -= calculator->buffer[calculator->index];
    }

    // Add the new value to the buffer and the sum
    calculator->buffer[calculator->index] = new_value;
    calculator->sum += new_value;

    // Move the index forward in a circular manner
    calculator->index = (calculator->index + 1) % BUFFER_SIZE;

    // Update the count
    if (calculator->count < BUFFER_SIZE) {
        calculator->count++;
    }

    // Calculate and update the avg in the ObjMonitor structure
    obj->bbox_temp = round((float)calculator->sum / (float)calculator->count);
    obj->corrected = 0;
}


double my_sqrt(double num) 
{
  if (num < 0) {
      printf("Error: Negative input. Square root of negative numbers is not defined in real numbers.\n");
      return -1.0;
  }
  if (num == 0 || num == 1) {
      return num;
  }

  double guess = num / 2.0; // Initial guess
  double epsilon = 1e-6;    // Precision threshold

  while ((guess * guess - num) > epsilon || (num - guess * guess) > epsilon) {
      guess = (guess + num / guess) / 2.0; // Newton-Raphson formula
  }

  return guess;
}


void dec_temp_event_time_gap()
{  
  for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
    if (obj_info[THERMAL_CAM][obj_id].temp_event_time_gap > 0) {
      obj_info[THERMAL_CAM][obj_id].temp_event_time_gap--;
    }
  }
}


double calculate_sqrt(double width, double height) 
{
  return my_sqrt((width * width) + (height * height));
}

#if TRACK_PERSON_INCLUDE

static int g_tracked_id = -1;

ObjState object_state = PRE_INIT_STATE;

int get_distance_from_center(PersonObj *object)
{
  int bbox_center_x = object->left + (object->width / 2);
  int bbox_center_y = object->top + (object->height / 2);
  
  return ((bbox_center_x - CENTER_X) + (bbox_center_y - CENTER_Y));
}

int select_object_id(PersonObj object[], int size)
{
  if (size <= 0) {
      return -1; // Return -1 if the array is empty
  }

  int min_index = -1;
  int min_distance = MAX_DISTANCE;

  for (int i = 1; i < size; i++) {
      if (object[i].detected == 0)
        continue;
      if (object[i].distance < min_distance) {
          min_distance = object[i].distance;
          min_index = i;
      }
  }

  return min_index;
}


void init_objects(PersonObj object[])
{
  for (int i = 0; i < NUM_OBJS; i++) {
    memset(&object[i], 0, sizeof(PersonObj));
  }
}

extern void print_serial_data(unsigned char *data, int len);

int move_to_ptz_pos(unsigned char *pos_data, int move_speed)
{
    unsigned char send_data[22] = {0}, read_data[8] = {0};
    int read_len = 0;

    print_serial_data(pos_data, 21);

    memcpy(&send_data[5], &pos_data[1], 10);
    send_data[15] = send_data[16] = send_data[17] = send_data[18] = move_speed;   //LJH, [15] is Pan Speed, [16] is Tilt Speed, [17] is Zoom Speed, [18] is Focus Speed
    send_data[19] = 0;
    send_data[20] = get_checksum(send_data, 20);
    glog_trace("move_speed[%d]\n", move_speed);
    read_len = read_cmd_timeout(send_data, 21, read_data, 7, 1);
    if(read_len == -1){
      glog_error("fail read move_ptz_pos \n");
    }

    return read_len;
}

static int g_move_to_pos_running = 0;

void *move_to_pos(void *arg)
{
  int i = 0;
  
  send_ptz_move_cmd(0, 0);
  usleep(1000);
  move_ptz_pos(0, 1);
  while(1) {
    sleep(1);
    if(is_ptz_motion_stopped()) {
      glog_trace("PTZ stopped after %d sec\n", (i + 1) * 2);
      break;
    }
    i++;
  }
  g_move_to_pos_running = 2;

  return NULL;
}


void move_to_init_pos()
{
  static pthread_t tid = 0;

  if (g_move_to_pos_running == 1)
    return;
  g_move_to_pos_running = 1;
  pthread_create(&tid, NULL, move_to_pos, NULL);
}

extern int g_move_ptz_condition;

ObjState track_object(ObjState object_state, PersonObj object[])
{
  int obj_id = -1;
  static int undetected_count[NUM_OBJS] = {0};
  static int detected_count[NUM_OBJS] = {0};

  switch(object_state) {
    case PRE_INIT_STATE:
      if (g_move_ptz_condition == 0)
        break;

      // glog_trace("PRE_INIT_STATE g_move_to_pos_running=%d\n", g_move_to_pos_running);
      if (g_move_to_pos_running == 0) {
        move_to_init_pos();
      }
      else if (g_move_to_pos_running == 2) {
        g_move_to_pos_running = 0;
        object_state = INIT_STATE;
      }
      break;

    case INIT_STATE:
      // glog_trace("INIT_STATE\n");
      g_tracked_id = -1;
      move_init();
      memset(undetected_count, 0, sizeof(undetected_count));
      memset(detected_count, 0, sizeof(detected_count));
      obj_id = select_object_id(object, NUM_OBJS); 
      if (obj_id != -1) {
        g_tracked_id = obj_id;
        object_state = TRACKED_STATE;
        // glog_trace("g_tracked_id = %d\n", g_tracked_id);
      }
      break;

    case TRACKED_STATE:
      // glog_trace("TRACKED_STATE id=%d\n", g_tracked_id);
      if (object[g_tracked_id].detected == 0) {
        undetected_count[g_tracked_id] = 1;
        object_state = OUT_STATE;
        break;
      }
      else {
        detected_count[g_tracked_id]++;
      }
      undetected_count[g_tracked_id] = 0;
      // glog_trace("TRACKED_STATE id=%d detected=%d\n", g_tracked_id, detected_count[g_tracked_id]);
      // if (detected_count[g_tracked_id] >= 9)
      move_ptz_along(object[g_tracked_id].top, object[g_tracked_id].left, object[g_tracked_id].width, object[g_tracked_id].height);
      break;

    case OUT_STATE:
      // glog_trace("OUT_STATE\n");
      // glog_trace("undetected_count[%d]=%d\n", g_tracked_id, undetected_count[g_tracked_id]);
      if (object[g_tracked_id].detected == 0) {
        if (++undetected_count[g_tracked_id] >= 15) {
          object_state = PRE_INIT_STATE;
        }
      }
      else {
        undetected_count[g_tracked_id] = 0;
        object_state = TRACKED_STATE;
      }
      break;
  }

  return object_state;
}


void set_person_obj_state(PersonObj object[], NvDsObjectMeta *obj_meta)
{
  obj_meta->text_params.font_params.font_size = 10;
  if (obj_meta->class_id >= 0 && obj_meta->class_id <= 3 && obj_meta->confidence > 0.2) {
    object[obj_meta->object_id % NUM_OBJS].top = (int)obj_meta->rect_params.top;
    object[obj_meta->object_id % NUM_OBJS].left = (int)obj_meta->rect_params.left;
    object[obj_meta->object_id % NUM_OBJS].width = (int)obj_meta->rect_params.width;
    object[obj_meta->object_id % NUM_OBJS].height = (int)obj_meta->rect_params.height;
    object[obj_meta->object_id % NUM_OBJS].detected = 1;
    object[obj_meta->object_id % NUM_OBJS].distance = get_distance_from_center(&object[obj_meta->object_id]);

    if (g_tracked_id == obj_meta->object_id) {
      obj_meta->rect_params.border_width = 4;
      obj_meta->rect_params.border_color.red = 0.0;
      obj_meta->rect_params.border_color.green = 0.0;
      obj_meta->rect_params.border_color.blue = 1.0;
      obj_meta->rect_params.border_color.alpha = 1;
    } 
    else {
      obj_meta->rect_params.border_color.red = 0.0;
      obj_meta->rect_params.border_color.green = 1.0;
      obj_meta->rect_params.border_color.blue = 0.0;
      obj_meta->rect_params.border_color.alpha = 1;
    }
    // print_debug(obj_meta);
    if (object_state == PRE_INIT_STATE) {
    // if (object_state == -1) {
      obj_meta->rect_params.border_color.red = 0.0;
      obj_meta->rect_params.border_color.green = 0.0;
      obj_meta->rect_params.border_color.blue = 0.0;
      obj_meta->rect_params.border_color.alpha = 0.0;
      obj_meta->text_params.display_text[0] = 0;
    }
  }
}
#endif

// Function to remove all newline characters from the input string
void remove_newlines(char *str) 
{
    // Iterate over the string
    char *src = str, *dst = str;
    
    // Loop through the string until the null terminator
    while (*src != '\0') {
        if (*src != '\n') {
            // Copy non-newline characters to the destination
            *dst = *src;
            dst++;
        }
        // Move the source pointer forward
        src++;
    }
    // Null terminate the string after removing all newlines
    *dst = '\0';
}


// Function to initialize the notification queue
void initialize_queue_noti(noti_queue *q) 
{
  q->front = -1;
  q->rear = -1;
}


// Function to check if the notification queue is full
int is_queue_full_noti(noti_queue *q) 
{
  return (q->rear == MAX_NOTI_ITEM_NUM - 1);
}


// Function to check if the notification queue is empty
int is_queue_empty_noti(noti_queue *q) 
{
  return (q->front == -1 || q->front > q->rear);
}


// Function to add a notification item to the queue (enqueue)
void enqueue_noti(noti_queue *q, char *cam_id, int cam_idx, int class_id, CurlIinfoType *curlinfo) 
{
  if (is_queue_full_noti(q)) {
      glog_trace("noti_queue is full! Cannot enqueue cam_idx=%d, class_id=%d, video_url=%s\n", cam_idx, class_id, curlinfo->video_url);
  } else {
      if (q->front == -1) {
          q->front = 0; // First item in the queue
      }
      q->rear++;
      strcpy(q->items[q->rear].cam_id, cam_id);
      q->items[q->rear].class_id = class_id;
      q->items[q->rear].cam_idx = cam_idx;
      memcpy(&q->items[q->rear].curlinfo, curlinfo, sizeof(CurlIinfoType));
      glog_trace("Enqueued item: cam_idx=%d, class_id=%d, video_url=%s\n", cam_idx, class_id, curlinfo->video_url);
  }
}


// Function to remove a notification item from the queue (dequeue)
noti_item dequeue_noti(noti_queue *q) 
{
  noti_item empty_item;

  if (is_queue_empty_noti(q)) {
      printf("noti_queue is empty! Cannot dequeue\n");
      memset(&empty_item, 0, sizeof(noti_item));
      empty_item.cam_idx = -1;
      empty_item.class_id = -1;
      return empty_item;  // Return an empty item
  } 
  else {
      noti_item dequeued_item = q->items[q->front];
      q->front++;
      if (q->front > q->rear) {
          q->front = q->rear = -1; // Reset the queue when it's empty
      }
      printf("Dequeued item: cam_id=%s, cam_idx=%d, video_url=%s\n", dequeued_item.cam_id, dequeued_item.cam_idx, dequeued_item.curlinfo.video_url);
      return dequeued_item;
  }
}


// Function to display the notification items in the queue
void display_noti(noti_queue *q) {
  if (is_queue_empty_noti(q)) {
      printf("noti_queue is empty!\n");
  } else {
      printf("noti_queue: \n");
      for (int i = q->front; i <= q->rear; i++) {
          printf("cam_idx=%d, video_url=%s\n", q->items[i].cam_idx, q->items[i].curlinfo.video_url);
      }
  }
}


int is_auto_pan_set()
{
  if (strlen(g_setting.auto_ptz_seq) > 0)
    return 1;
  return 0;
}


#if 0
#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_PATH "resnet50.engine"
#define INPUT_VIDEO "input.mp4"

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR:
            // GError *err;
            // gchar *debug;
            // gst_message_parse_error(msg, &err, &debug);
            // g_printerr("Error: %s\n", err->message);
            // g_error_free(err);
            // g_free(debug);
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }
    return TRUE;
}

static void extract_bounding_box_image(GstBuffer *buffer) {
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        g_print("Extracting bounding box image data...\n");
        // Example: Save buffer data to a file for analysis
        FILE *file = fopen("bounding_box.raw", "wb");
        if (file) {
            fwrite(map.data, 1, map.size, file);
            fclose(file);
            g_print("Bounding box image saved to bounding_box.raw\n");
        }
        gst_buffer_unmap(buffer, &map);
    } else {
        g_printerr("Failed to map buffer\n");
    }
}

static GstPadProbeReturn
buffer_probe_callback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) {
        g_print("Processing bounding box image for analysis...\n");
        extract_bounding_box_image(buffer);
    }
    return GST_PAD_PROBE_OK;
}

int aaa(int argc, char *argv[]) {
    GMainLoop *loop;
    GstElement *pipeline, *source, *decoder, *streammux, *pgie, *nvvidconv, *nvosd, *sink;
    GstBus *bus;
    GstPad *osd_sink_pad;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    pipeline = gst_parse_launch(
        "filesrc location=" INPUT_VIDEO " ! decodebin ! nvstreammux name=mux batch-size=1 width=1920 height=1080 ! "
        "nvinfer config-file-path=" MODEL_PATH " ! nvvideoconvert ! nvdsosd name=osd ! nveglglessink", NULL);

    if (!pipeline) {
        g_printerr("Failed to create pipeline\n");
        return -1;
    }

    osd_sink_pad = gst_element_get_static_pad(gst_bin_get_by_name(GST_BIN(pipeline), "osd"), "sink");
    if (!osd_sink_pad) {
        g_printerr("Failed to get osd sink pad\n");
    } else {
        gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, buffer_probe_callback, NULL, NULL);
        gst_object_unref(osd_sink_pad);
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}

#endif