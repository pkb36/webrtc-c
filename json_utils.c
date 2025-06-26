#include "json_utils.h"
#include "gstream_main.h"


static gchar* json_info_template = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": %s \
}";



gboolean
cockpit_json_get_string (JsonObject *options,
                         const gchar *name,
                         const gchar *defawlt,
                         const gchar **value, gboolean dump_string)
{
  JsonNode *node;

  node = json_object_get_member (options, name);
  if (!node)
    {
      return FALSE;
    }
  else if (json_node_get_value_type (node) == G_TYPE_STRING)
    {
      if (value){
        if(dump_string)
          *value = json_node_dup_string(node);
        else   
          *value = json_node_get_string(node);
      }
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}


static gchar *
get_string_from_json_object (JsonObject * object)
{
  JsonNode *root;
  JsonGenerator *generator;
  gchar *text;

  /* Make it the root node */
  root = json_node_init_object (json_node_alloc (), object);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);

  /* Release everything */
  g_object_unref (generator);
  json_node_free (root);
  return text;
}


gboolean send_json_info(const gchar* action,  const gchar* info_json)
{
  gchar * msg; 
  msg = g_strdup_printf(json_info_template, action, info_json);
  send_msg_server (msg);    
  g_free (msg); 
  return TRUE;    
}


gJSONObj*   get_json_object(gchar* json_msg)
{
  JsonNode *root;
  JsonObject *object;
  JsonParser *parser = json_parser_new ();
  
  if (!json_parser_load_from_data (parser, json_msg, -1, NULL)) {
    glog_error ("Unknown message '%s' ", json_msg);
    return NULL;
  }

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    glog_error ("Unknown json message '%s' ignoring", json_msg);
    g_object_unref (parser);
    return NULL;
  }

  //gst_print ("Message from peer  %s\n", msg);
  object = json_node_get_object (root);
  
  gJSONObj * gJsonObj = (gJSONObj *)malloc(sizeof(gJSONObj));
  gJsonObj->root = root;
  gJsonObj->object = object;
  gJsonObj->parser = parser;

  return gJsonObj;  
}


gboolean    get_json_action(gJSONObj *obj, const gchar** action)
{
  return cockpit_json_get_string(obj->object, "action", NULL, action, FALSE);
}

gboolean    get_json_message(gJSONObj *obj, const gchar** message)
{
  return cockpit_json_get_string(obj->object, "message", NULL, message, FALSE);
}


gboolean    get_json_data_from_message(gJSONObj *obj, const gchar* key, const gchar** value)
{
  JsonNode *node;
  node = json_object_get_member (obj->object, "message");
  if (!node)
    {
      return FALSE;
    }

  JsonObject *object = json_node_get_object (node);
  return  cockpit_json_get_string(object, key, NULL, value, FALSE);
}


gchar*  get_json_data_from_message_as_string(gJSONObj *obj, const gchar* key)
{
  JsonNode *node;
  node = json_object_get_member (obj->object, "message");
  if (!node)
    {
      return NULL;
    }

  JsonObject *object = json_node_get_object (node);
  return  get_string_from_json_object(object);
}


void free_json_object(gJSONObj* obj)
{
  if(obj != NULL){
    g_object_unref (obj->parser);
    free(obj);
  }
}


gboolean get_json_template_message(gchar* json_msg, const gchar** action, const gchar** message)
{
  JsonNode *root;
  JsonObject *object;
  gboolean success=FALSE;
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json_msg, -1, NULL)) {
    glog_error ("Unknown message '%s' ", json_msg);
    goto exit;
  }

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    glog_error ("Unknown json message '%s' ignoring", json_msg);
    goto exit;
  }

  //gst_print ("Message from peer  %s\n", msg);
  object = json_node_get_object (root);
  if(!cockpit_json_get_string(object, "action", NULL, action, TRUE)){
    glog_error ("error get action '%s'", json_msg);
    goto exit;
  }

  if(!cockpit_json_get_string(object, "message", NULL, message, TRUE)){
    glog_error ("error get message '%s'", json_msg);
    //g_free(action); free을 하게 되면 죽게 된다... dup을 해서 free을 해 준건데.... 알 수 없음...
    goto exit;
  }

  success=TRUE;

exit:

  g_object_unref (parser);
  return success;
}

#if 0
void replace_json_value(char *json, const char *key, const char *new_value)           //LJH, 20250220, for variable frame rate
{
  char *key_position = strstr(json, key); // Find the key in the JSON string

  // If key is found
  if (key_position != NULL) {
      // Move the pointer to the start of the value after the key
      key_position = strchr(key_position, ':') + 1;

      // Skip over any leading whitespace
      while (*key_position == ' ' || *key_position == '\t') {
          key_position++;
      }

      // Find the end of the value (either end of the string or closing quote/brace)
      char *value_end = strchr(key_position, ',');

      if (value_end == NULL) {
          value_end = strchr(key_position, '}');  // Handle last value in JSON object
      }

      if (value_end == NULL) {
          value_end = key_position + strlen(key_position); // End of string if no comma
      }

      // Calculate how much space we need to replace
      size_t new_value_len = strlen(new_value);
      size_t value_len = value_end - key_position;

      if (new_value_len != value_len) {
          // If the new value's length differs, we need to adjust the string
          memmove(value_end + (new_value_len - value_len), value_end, strlen(value_end) + 1);
      }

      // Replace the value with the new value
      memcpy(key_position, new_value, new_value_len);
  }
}
#endif

#if 0
int main() {
  char json[] = "{\"name\":\"John\", \"age\":30, \"city\":\"New York\"}";
  printf("Before replacement: %s\n", json);

  replace_json_value(json, "\"age\"", "31");

  printf("After replacement: %s\n", json);

  return 0;
}
#endif