#include <json-glib/json-glib.h>
#include <stdio.h>

gchar* json_tesmplat = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": \"%s\" \
}";


gchar* json_message_tesmplat = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": %s \
}";


static gboolean
cockpit_json_get_string (JsonObject *options,
                         const gchar *name,
                         const gchar *defawlt,
                         const gchar **value)
{
  JsonNode *node;

  node = json_object_get_member (options, name);
  if (!node)
    {
      if (value)
        *value = defawlt;
      return TRUE;
    }
  else if (json_node_get_value_type (node) == G_TYPE_STRING)
    {
      if (value)
        *value = json_node_get_string (node);
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

gchar* json_message_tesmplat2 = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": {\"peer_id\": \"%s\", \"%s\" : \"%s\" }\
}";



void test1()
{
  gchar * msg; 
  msg = g_strdup_printf(json_tesmplat, "register","test");
  printf("json_test  %s \n", msg);


  msg = g_strdup_printf(json_message_tesmplat2, "offer","flksdjlfkjsdlfkj", "ice", "etstsetsaaassss");
  printf("json_test  %s \n", msg);


  gchar * json; 
  JsonNode *root;
  JsonObject *object, *child;
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, msg, -1, NULL)) {
    gst_printerr ("Unknown message '%s' \n ", msg);
     return 0;
  }

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    gst_printerr ("Unknown json message '%s' ignoring \n", msg);
    return 0;
  }

  gst_print ("Message from peer  %s\n", msg);
  object = json_node_get_object (root);
  
  gchar *text;
  if(cockpit_json_get_string(object, "action", NULL, &text)){
    printf("get action : %s \n", text);
    return 0;
  }

  if(cockpit_json_get_string(object, "message", NULL, &text)){
    printf("get message : %s \n", text);
    return 0;
  }

  g_free (msg); 

}


int main (int argc, char *argv[])
{
  gchar * msg; 
  msg = g_strdup_printf(json_message_tesmplat2, "offer","flksdjlfkjsdlfkj", "ice", "etstsetsaaassss");
  printf("json_test  %s \n", msg);

  gchar * json; 
  JsonNode *root;
  JsonObject *object, *child;
  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, msg, -1, NULL)) {
    gst_printerr ("Unknown message '%s' \n ", msg);
     return 0;
  }

  root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    gst_printerr ("Unknown json message '%s' ignoring \n", msg);
    return 0;
  }

  gst_print ("Message from peer  %s\n", msg);
  object = json_node_get_object (root);
  
  gchar *text;
  if(!cockpit_json_get_string(object, "action", NULL, &text)){
    printf("error get action : %s \n", text);
    return 0;
  }     
  printf("action : %s \n", text);

  JsonNode *node;  
  node = json_object_get_member (object, "message");
  if (!node){
    gst_print ("Can not get message Node\n");
    return FALSE;
  }
  JsonObject *child_object = json_node_get_object (node);
  char * key_to_find  = "peer_id";
  if (!json_object_has_member(child_object, key_to_find)) {
    gst_print ("Can not get child_object\n");
    return FALSE;
  }

  JsonNode *valueNode = json_object_get_member(child_object, key_to_find);
  const gchar *value_str = json_node_get_string(valueNode);
  printf("Key: %s, Value: %s\n", key_to_find, value_str);

  return 1 ;
}
