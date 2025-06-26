#ifndef __G_JSON_UTILS_H__
#define __G_JSON_UTILS_H__

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

typedef struct 
{
  JsonParser *parser;  
  JsonNode *root;
  JsonObject *object;    
} gJSONObj;

gboolean    send_json_info(const gchar* action,  const gchar* info_json);

gJSONObj*   get_json_object(gchar* json);
gboolean    get_json_action(gJSONObj *obj, const gchar** action);
gboolean    get_json_message(gJSONObj *obj, const gchar** message);

gboolean    get_json_data_from_message(gJSONObj *obj, const gchar* key, const gchar** value);
gchar*      get_json_data_from_message_as_string(gJSONObj *obj, const gchar* key);
void        free_json_object(gJSONObj* obj);

gboolean    cockpit_json_get_string (JsonObject *options, 
                        const gchar *name,
                         const gchar *defawlt,
                         const gchar **value, gboolean dump_string);


#ifdef USE_JSON_MESSAGE_TEMPLATE

static gchar* json_msssage_template = "{\
   \"peerType\": \"camera\",\
   \"action\": \"%s\",\
   \"message\": {\"peer_id\": \"%s\", \"%s\":%s} \
}";

#endif


#endif