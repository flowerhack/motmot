/**
 * purplemot
 * motmot chat protocol, extended from nullprpl chat protocol
 *
 * This is the Pidgin plugin for motmot. It provides a UI wrapper for the
 * functionality of libmotmot, and hooks up the separate parts of the project.
 * Upon login (purplemot_login), a connection is established with a discovery server.
 * Once the connection is established, the user's credentials are authenticated against
 * the discovery server (do_login), and the client begins reading/writing
 * data to the discovery server.
 *
 * The discovery server is used for status updates, presence,
 * and friend requests. Actual chat is handled by the distributed paxos
 * algorithm.
 *
 * Calls to the libmomot api are added in relevant user action functions.
 * In addition event listeners are added to listen for relevant libmotmot
 * actions.
 *
 * In addition to supporting presence and chatting, the protocol interacts
 * with the UI through calls to libpurple functions.
 *
 */

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <glib.h>

#include <ctype.h>
#include <msgpack.h>
#include <errno.h>


// Nop!
#define PURPLE_PLUGINS 1
#define _
#define N_

#define DISPLAY_VERSION "1"
#define MOTMOT_WEBSITE "https://github.com/zenazn/motmot"

#define DEFAULT_SERV "motmottest.com"
#define DEFAULT_PORT 8888


#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "cmds.h"
#include "conversation.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "privacy.h"
#include "prpl.h"
#include "roomlist.h"
#include "sslconn.h"
#include "status.h"
#include "util.h"
#include "version.h"

#include "motmot.h"
#include "purplemot.h"

#define BUFF_SIZE 512
#define PURPLEMOT_ERROR 1
#define PURPLEMOT_OK 0

#define AUTHENTICATE_USER 1
#define REGISTER_FRIEND 2
#define UNREGISTER_FRIEND 3
#define GET_FRIEND_IP 4
#define REGISTER_STATUS 5
#define AUTHENTICATE_SERVER 30
#define SERVER_SEND_FRIEND 31
#define SERVER_SEND_UNFRIEND 32
#define ACCEPT_FRIEND 6
#define SERVER_SEND_ACCEPT 34
#define SERVER_SEND_STATUS_CHANGED 33
#define PUSH_CLIENT_STATUS 20
#define PUSH_FRIEND_ACCEPT 21
#define PUSH_FRIEND_REQUEST 22
#define GET_ALL_STATUSES 7
#define SERVER_GET_STATUS 35
#define SERVER_GET_STATUS_RESP 66
#define ALL_STATUS_RESPONSE 65
#define AUTHENTICATED 61
#define AUTH_FAILED 62
#define SUCCESS 60
#define ACCESS_DENIED 63
#define FRIEND_SERVER_DOWN 91
#define GET_USER_STATUS 8
#define GET_STATUS_RESP 68

#define ONLINE 1
#define AWAY 2
#define OFFLINE 3
#define BUSY 4
#define SERVER 5

static void motmot_report_status(const char *id, motmot_conn *conn);
void get_all_statuses(motmot_conn *conn);
static void motmot_parse(char *buffer, int len, PurpleConnection *gc);
static void
motmot_login_failure(PurpleSslConnection *gsc, PurpleSslErrorType error,
		gpointer data);

static void motmot_login_cb(gpointer data, PurpleSslConnection *gsc, PurpleInputCondition cond);
#define PURPLEMOT_ID "prpl-motmot"
static PurplePlugin *_null_protocol = NULL;
int chat_id = 0;

PurpleAccount *GLOBAL_ACCOUNT = NULL;

#define NULL_STATUS_ONLINE   "online"
#define NULL_STATUS_AWAY     "away"
#define NULL_STATUS_OFFLINE  "offline"


typedef void (*GcFunc)(PurpleConnection *from,
                       PurpleConnection *to,
                       gpointer userdata);

typedef struct {
  GcFunc fn;
  PurpleConnection *from;
  gpointer userdata;
} GcFuncData;

gint check_info(gconstpointer a, gconstpointer data){
  int id = *((int *) data);
  const MotmotInfo *info = a;
  if(id == info -> id){
    return 0;
  }
  return 1;
}

// finds an info
MotmotInfo *find_info(motmot_conn *conn, int id){
  GList *res = g_list_find_custom(conn -> info_list, &id, check_info);
  if(res == NULL){
    return NULL;
  }

  return res -> data;
}
// callbacks to momotinit
// enter function
void *enter(void *data){
  int id;
  MotmotInfo *info = g_new0(MotmotInfo, 1);
  info -> internal_data = data;

  PurpleConnection *gc = GLOBAL_ACCOUNT -> gc;
  motmot_conn *proto = gc -> proto_data;
  while(purple_find_chat(gc, chat_id) != NULL){
    chat_id += 1;
  }
  id = chat_id;
  chat_id += 1;

  serv_got_joined_chat(gc, id, "Chat");

  info -> id = id;

  proto -> info_list = g_list_prepend(proto -> info_list, info);

  return info;
}

// leaving functions
void leave_cb(void *data){
  MotmotInfo *info = data;

  serv_chat_leave(GLOBAL_ACCOUNT -> gc, info -> id);
}

/*
 * stores offline messages that haven't been delivered yet. maps username
 * (char *) to GList * of GOfflineMessages. initialized in purplemot_init.
 */

// global definition of aforementioned struct. motmot everrrrrywhere
// question—do I want this to be global?
// I don't think so; comment this out for now
// MotmotInfo motmot_info;

GHashTable* goffline_messages = NULL;

typedef struct {
  char *from;
  char *message;
  time_t mtime;
  PurpleMessageFlags flags;
} GOfflineMessage;

// Called by print_part_motmot to indicate successful chat departure.
/*
static void left_chat_room(PurpleConvChat *from, PurpleConvChat *to,
                           int id, const char *room, gpointer userdata) {
  if (from != to) {
    purple_debug_info("purplemot", "%s sees that %s left chat room %s\n",
                      to->nick, from->nick, room);
    purple_conv_chat_remove_user(to,
                                 from->nick,
                                 NULL);
  }
}
*/
/**
 * query_status - query the status of a buddy
 *
 * @param username The username of the buddy
 * @param conn The motmot connection handle
 */
static void query_status(const char *username, motmot_conn *conn){
  msgpack_sbuffer *buffer = msgpack_sbuffer_new();
  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);

  msgpack_pack_array(pk, 2);
  msgpack_pack_int(pk, GET_USER_STATUS);

  msgpack_pack_raw(pk, strlen(username) );
  msgpack_pack_raw_body(pk, username, strlen(username) );

  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size); 

  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);
}

/**
 * deser_get_array - deserialize a message pack array
 *
 * @param buffer The serialized message pack object.
 * @param len Length of the buffer
 * @param error Pointer to a integer for error handline
 *               will be set to PURPLEMOT_OK on success of function,
 *               PURPLEMOT_ERROR on failure
 *
 * @returns deserialized array, undefined on failure
 */
static msgpack_object_array deser_get_array(char *buffer, int len, int *error){
  bool success;
  msgpack_object_array ar;
  msgpack_unpacked msg;
  msgpack_unpacked_init(&msg);
  success = msgpack_unpack_next(&msg, buffer, len, NULL);
  if(!success || msg.data.type != MSGPACK_OBJECT_ARRAY){
    *error = PURPLEMOT_ERROR;
    return ar;
  }


  ar = msg.data.via.array;

  *error = PURPLEMOT_OK;
  return ar;
}

/**
 * get_array2 - retrieves an array from a message pack object
 *
 * @param obj Message pack object.
 * @param error Pointer to a integer for error handline
 *               will be set to PURPLEMOT_OK on success of function,
 *               PURPLEMOT_ERROR on failure
 *
 * @returns msgpack_object_array, undefined on failure
 */
static msgpack_object_array get_array2(msgpack_object obj, int *error){
  msgpack_object_array ar;
  if(obj.type != MSGPACK_OBJECT_ARRAY){
    *error = PURPLEMOT_ERROR;
    return ar;
  }
  ar = obj.via.array;

  *error = PURPLEMOT_OK;
  return ar;
}
/**
 * deser_get_pos_int - retrieves a positive int from a msgpack_object_array
 *
 * @param ar The msgpack_object_array
 * @param i index of the int within ar
 * @param error Pointer to a integer for error handline
 *               will be set to PURPLEMOT_OK on success of function,
 *               PURPLEMOT_ERROR on failure
 *
 * @returns the int, 0 on failure (but it could just be 0 on success)
 */
static uint64_t deser_get_pos_int(msgpack_object_array ar, int i, int *error){
  msgpack_object x;
  if(ar.size < i + 1){
    *error = PURPLEMOT_ERROR;
    return 0;
  }
  x = (ar.ptr)[i];
  if(x.type != MSGPACK_OBJECT_POSITIVE_INTEGER){
    *error = PURPLEMOT_ERROR;
    return 0;
  }

  *error = PURPLEMOT_OK;
  return x.via.u64;
}

/**
 * deser_get_pos_string - retrieves a positive string from a msgpack_object_array
 *
 * @param ar The msgpack_object_array
 * @param i index of the int within ar
 * @param error Pointer to a integer for error handline
 *               will be set to PURPLEMOT_OK on success of function,
 *               PURPLEMOT_ERROR on failure
 *
 * @returns the string, null on failure
 */
static char *deser_get_string(msgpack_object_array ar, int i, int *error){
  char *ptr;
  if(ar.size < i + 1){
    *error = PURPLEMOT_ERROR;
    return NULL;
  }
  msgpack_object x = (ar.ptr)[i];
  if(x.type != MSGPACK_OBJECT_RAW){
    *error = PURPLEMOT_ERROR;
    return NULL;
  }

  ptr = g_malloc(x.via.raw.size + 1);
  g_memmove(ptr, x.via.raw.ptr, x.via.raw.size);
  ptr[x.via.raw.size] = '\0';

  *error = PURPLEMOT_OK;
  return ptr;
}

// libmotmot-related functions: function callbacks necessary to init motmot
// TODO finish these
// also declare them non-implicitly oops
// note: const void *handle refers to a PurpleBuddy *buddy

/*
    so, the bigass info struct will take in:
    - purplebuddy
    - a purpleconnection
    - an int_id
    - a message
    - flags
    - ghashtable components

    struct.buddy
    struct.connection
    struct.id
    struct.message
    struct.flags
    struct.components

    also, buddy->protodata is a struct
    struct needs to have (at least) ip field
*/

/**
 * get_purplemot_gc - gets the connection associated with the username
 * @param username     the username
 *
 * @returns           the connection
 */

static PurpleConnection *get_purplemot_gc(const char *username) {
  PurpleAccount *acct = purple_accounts_find(username, PURPLEMOT_ID);
  if (acct && purple_account_is_connected(acct))
    return acct->gc;
  else
    return NULL;
}

// Called by print_chat_motmot, after paxos has said message is OK to send
/*
static void receive_chat_message(PurpleConvChat *from, PurpleConvChat *to,
                                 int id, const char *room, gpointer userdata) {
  const char *message = (const char *)userdata;
  PurpleConnection *to_gc = get_purplemot_gc(to->nick);

  purple_debug_info("purplemot",
                    "%s receives message from %s in chat room %s: %s\n",
                    to->nick, from->nick, room, message);
  serv_got_chat_in(to_gc, id, from->nick, PURPLE_MESSAGE_RECV, message,
                   time(NULL));
}
*/
// returns fd on success, -1 on failure—>TODO(Julie) fix this once MAX does his shit
// it'll also be a GIOChannel * then.  So yeah.

/**
 * connectSuccess - Callback for when a p2p connection is established for libmotmot.
 *
 * @param data  User data.
 * @param source The file descriptor for the socket, -1 on error.
 * @param error_message Error message
 */
static void connectSuccess(gpointer data, gint source, const gchar *error_message)
{
  //MotmotInfo *info = data;
  struct motmot_connect_cb *cb = data;
  if(source < 0){
    (cb -> func)(NULL, cb -> data);
    return;
  }
  (cb -> func)(g_io_channel_unix_new(source), cb -> data);
  return;
}

/**
 * connect_motmot - Connect callback for motmot_init to initiate a function
 *
 * @param desc username
 * @param len length of the username
 * @param motmot_connect_cb continuation necessary for libmotmot
 */
int connect_motmot(const void *desc, size_t len, struct motmot_connect_cb *cb)
{
  PurpleBuddy *bud = purple_find_buddy(GLOBAL_ACCOUNT, (const char *) desc);
  if(bud == NULL){
    return -1;
  }

  motmot_buddy *extra = bud -> proto_data;
  purple_proxy_connect(GLOBAL_ACCOUNT -> gc, GLOBAL_ACCOUNT, extra -> addr,
    extra -> port, connectSuccess, cb);
  return 0;
}

//static int purplemot_send_im(PurpleConnection *gc, const char *who,
//                        const char *message, PurpleMessageFlags flags)

// libmotmot callback. Calls receive_chat_message.
/**
 * print_chat_motmot - callback for receiving a chat, prints message
 *
 * @param message the message
 * @param len length of the message
 * @param desc user who sent it
 * @param size length of username's message
 * @param data MotmotInfo struct to identify chat to libpurple
 */
int print_chat_motmot(const void *message, size_t len, void *desc, size_t size, void *data)
{
  MotmotInfo *minfo = data;
  int id = minfo -> id;
  PurpleConnection *gc = GLOBAL_ACCOUNT -> gc;

  serv_got_chat_in(gc, id, desc, PURPLE_MESSAGE_RECV, message, time(NULL));
  return 0;
}

// libmotmot callback.
//call when someone joins a chat—probably do this based on their sending a msg to chat
// TODO—fill this in, make this pint "so-and-so joined"
int print_join_motmot(const void *unused, size_t unusedl, void *info, size_t len, void *data)
{
  MotmotInfo *minfo = data;
  int id = minfo -> id;
  PurpleConnection *gc = GLOBAL_ACCOUNT -> gc;

  PurpleConversation *chat = purple_find_chat(gc, id);
  PurpleConvChat *conv = purple_conversation_get_chat_data(chat);

  if(conv != NULL){
    purple_conv_chat_add_user(conv, info, NULL, PURPLE_CBFLAGS_NONE, TRUE);
  }
  return 0;
}

// libmotmot callback
//call when someone leaves a chat—do based on logout/inaccessibility I guess
int print_part_motmot(const void *unused, size_t unusedl,void *info, size_t len, void *data)
{
  MotmotInfo *minfo = data;
  int id = minfo -> id;
  PurpleConnection *gc = GLOBAL_ACCOUNT -> gc;

  PurpleConversation *conv = purple_find_chat(gc, id);
  PurpleConvChat *chat = purple_conversation_get_chat_data(conv);

  if(conv != NULL){
    purple_conv_chat_remove_user(chat, info, NULL);
  }

  return 0;
}

static void call_if_purplemot(gpointer data, gpointer userdata) {
  PurpleConnection *gc = (PurpleConnection *)(data);
  GcFuncData *gcfdata = (GcFuncData *)userdata;

  if (!strcmp(gc->account->protocol_id, PURPLEMOT_ID))
    gcfdata->fn(gcfdata->from, gc, gcfdata->userdata);
}

static void foreach_purplemot_gc(GcFunc fn, PurpleConnection *from,
                                gpointer userdata) {
  GcFuncData gcfdata = { fn, from, userdata };
  g_list_foreach(purple_connections_get_all(), call_if_purplemot,
                 &gcfdata);
}


typedef void(*ChatFunc)(PurpleConvChat *from, PurpleConvChat *to,
                        int id, const char *room, gpointer userdata);

typedef struct {
  ChatFunc fn;
  PurpleConvChat *from_chat;
  gpointer userdata;
} ChatFuncData;

static void call_chat_func(gpointer data, gpointer userdata) {
  PurpleConnection *to = (PurpleConnection *)data;
  ChatFuncData *cfdata = (ChatFuncData *)userdata;

  int id = cfdata->from_chat->id;
  PurpleConversation *conv = purple_find_chat(to, id);
  if (conv) {
    PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
    cfdata->fn(cfdata->from_chat, chat, id, conv->name, cfdata->userdata);
  }
}

static void foreach_gc_in_chat(ChatFunc fn, PurpleConnection *from,
                               int id, gpointer userdata) {
  PurpleConversation *conv = purple_find_chat(from, id);
  ChatFuncData cfdata = { fn,
                          purple_conversation_get_chat_data(conv),
                          userdata };

  g_list_foreach(purple_connections_get_all(), call_chat_func,
                 &cfdata);
}

/*
static void discover_status(PurpleConnection *from, PurpleConnection *to,
                            gpointer userdata) {
  const char *from_username = from->account->username;
  const char *to_username = to->account->username;

  if (purple_find_buddy(from->account, to_username)) {
    PurpleStatus *status = purple_account_get_active_status(to->account);
    const char *status_id = purple_status_get_id(status);
    const char *message = purple_status_get_attr_string(status, "message");

    if (!strcmp(status_id, NULL_STATUS_ONLINE) ||
        !strcmp(status_id, NULL_STATUS_AWAY) ||
        !strcmp(status_id, NULL_STATUS_OFFLINE)) {
      purple_debug_info("purplemot", "%s sees that %s is %s: %s\n",
                        from_username, to_username, status_id, message);
      purple_prpl_got_user_status(from->account, to_username, status_id,
                                  (message) ? "message" : NULL, message, NULL);
    } else {
      purple_debug_error("purplemot",
                         "%s's buddy %s has an unknown status: %s, %s",
                         from_username, to_username, status_id, message);
    }
  }
}
*/
/*
static void report_status_change(PurpleConnection *from, PurpleConnection *to,
                                 gpointer userdata) {
  purple_debug_info("purplemot", "notifying %s that %s changed status\n",
                    to->account->username, from->account->username);
  discover_status(to, from, NULL);
}
*/

/*
 * UI callbacks (most of this is unchanged from nullprpl)
 */
static void purplemot_input_user_info(PurplePluginAction *action)
{
  PurpleConnection *gc = (PurpleConnection *)action->context;
  PurpleAccount *acct = purple_connection_get_account(gc);
  purple_debug_info("purplemot", "showing 'Set User Info' dialog for %s\n",
                    acct->username);

  purple_account_request_change_user_info(acct);
}

/* this is set to the actions member of the PurplePluginInfo struct at the
 * bottom.
 */
static GList *purplemot_actions(PurplePlugin *plugin, gpointer context)
{
  PurplePluginAction *action = purple_plugin_action_new(
    _("Set User Info..."), purplemot_input_user_info);
  return g_list_append(NULL, action);
}


/*
 * prpl functions
 */
static const char *purplemot_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
  return "null";
}

static char *purplemot_status_text(PurpleBuddy *buddy) {
  purple_debug_info("purplemot", "getting %s's status text for %s\n",
                    buddy->name, buddy->account->username);

  if (purple_find_buddy(buddy->account, buddy->name)) {
    PurplePresence *presence = purple_buddy_get_presence(buddy);
    PurpleStatus *status = purple_presence_get_active_status(presence);
    const char *name = purple_status_get_name(status);
    const char *message = purple_status_get_attr_string(status, "message");

    char *text;
    if (message && strlen(message) > 0)
      text = g_strdup_printf("%s: %s", name, message);
    else
      text = g_strdup(name);

    purple_debug_info("purplemot", "%s's status text is %s\n", buddy->name, text);
    return text;

  } else {
    purple_debug_info("purplemot", "...but %s is not logged in\n", buddy->name);
    return g_strdup("Not logged in");
  }
}

static void purplemot_tooltip_text(PurpleBuddy *buddy,
                                  PurpleNotifyUserInfo *info,
                                  gboolean full) {
  PurpleConnection *gc = get_purplemot_gc(buddy->name);

  if (gc) {
    /* they're logged in */
    PurplePresence *presence = purple_buddy_get_presence(buddy);
    PurpleStatus *status = purple_presence_get_active_status(presence);
    char *msg = purplemot_status_text(buddy);
    purple_notify_user_info_add_pair(info, purple_status_get_name(status),
                                     msg);
    g_free(msg);

    if (full) {
      const char *user_info = purple_account_get_user_info(gc->account);
      if (user_info)
        purple_notify_user_info_add_pair(info, _("User info"), user_info);
    }

  } else {
    /* they're not logged in */
    purple_notify_user_info_add_pair(info, _("User info"), _("not logged in"));
  }

  purple_debug_info("purplemot", "showing %s tooltip for %s\n",
                    (full) ? "full" : "short", buddy->name);
}

static GList *purplemot_status_types(PurpleAccount *acct)
{
  GList *types = NULL;
  PurpleStatusType *type;

  purple_debug_info("purplemot", "returning status types for %s: %s, %s, %s\n",
                    acct->username,
                    NULL_STATUS_ONLINE, NULL_STATUS_AWAY, NULL_STATUS_OFFLINE);

  type = purple_status_type_new_with_attrs(PURPLE_STATUS_AVAILABLE,
      NULL_STATUS_ONLINE, NULL, TRUE, TRUE, FALSE,
      "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
      NULL);
  types = g_list_prepend(types, type);

  type = purple_status_type_new_with_attrs(PURPLE_STATUS_AWAY,
      NULL_STATUS_AWAY, NULL, TRUE, TRUE, FALSE,
      "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
      NULL);
  types = g_list_prepend(types, type);

  type = purple_status_type_new_with_attrs(PURPLE_STATUS_OFFLINE,
      NULL_STATUS_OFFLINE, NULL, TRUE, TRUE, FALSE,
      "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
      NULL);
  types = g_list_prepend(types, type);

  return g_list_reverse(types);
}

static void blist_example_menu_item(PurpleBlistNode *node, gpointer userdata) {
  purple_debug_info("purplemot", "example menu item clicked on user %s\n",
                    ((PurpleBuddy *)node)->name);

  purple_notify_info(NULL,  /* plugin handle or PurpleConnection */
                     _("Primary title"),
                     _("Secondary title"),
                     _("This is the callback for the purplemot menu item."));
}

static GList *purplemot_blist_node_menu(PurpleBlistNode *node) {
  purple_debug_info("purplemot", "providing buddy list context menu item\n");

  if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
    PurpleMenuAction *action = purple_menu_action_new(
      _("Nullprpl example menu item"),
      PURPLE_CALLBACK(blist_example_menu_item),
      NULL,   /* userdata passed to the callback */
      NULL);  /* child menu items */
    return g_list_append(NULL, action);
  } else {
    return NULL;
  }
}

static GList *purplemot_chat_info(PurpleConnection *gc) {
  struct proto_chat_entry *pce; /* defined in prpl.h */

  purple_debug_info("purplemot", "returning chat setting 'room'\n");

  pce = g_new0(struct proto_chat_entry, 1);
  pce->label = _("Chat _room");
  pce->identifier = "room";
  pce->required = TRUE;

  return g_list_append(NULL, pce);
}

static GHashTable *purplemot_chat_info_defaults(PurpleConnection *gc,
                                               const char *room) {

  //JULIE -> call motmot_session here?
  // motmot_session(self,strlen(self));
  GHashTable *defaults;

  purple_debug_info("purplemot", "returning chat default setting "
                    "'room' = 'default'\n");

  defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  g_hash_table_insert(defaults, "room", g_strdup("default"));
  return defaults;
}

/**
 * purplemot_login - Login for an account. Establishes a connection
 *                   to the discovery server and initializes
 *
 * @param acct       The account to be logged in
 */
static void purplemot_login(PurpleAccount *acct)
{
  char **userparts;
  motmot_conn *conn;
  int port;
/* This doesn't really work.
  if(GLOBAL_ACCOUNT != NULL){
		purple_connection_error_reason (acct -> gc,
			PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
			_("You can't login to more than one motmot account"));
		return;
  }
*/
  GLOBAL_ACCOUNT = acct;

	const char *username = purple_account_get_username(acct);

  PurpleConnection *gc = purple_account_get_connection(acct);
  GList *offline_messages;

  purple_debug_info("purplemot", "logging in %s\n", acct->username);

  /* MOTMOT! */

  // initialize paxos functions
  // right now this does nothing, since these are do-nothing functions:
  // TODO fill out later
  // motmot_init(connect_motmot, print_chat_motmot, print_join_motmot, print_part_motmot, enter, leave_cb);

	if (strpbrk(username, " \t\v\r\n") != NULL) {
		purple_connection_error_reason (gc,
			PURPLE_CONNECTION_ERROR_INVALID_SETTINGS,
			_("Motmot server may not contain whitespace"));
		return;
	}

  gc -> proto_data = conn = g_new0(motmot_conn, 1); 
  conn -> account = acct;
  
  userparts = g_strsplit(username, "@", 2);
  purple_connection_set_display_name(gc, userparts[0]);
  conn -> server = g_strdup(userparts[1]);
  conn -> acceptance_list = NULL;
  conn -> info_list = NULL;
  g_strfreev(userparts);

  port = purple_account_get_int(acct, "disc_port", DEFAULT_PORT);

  purple_connection_update_progress(gc, _("Connecting"),
                                    0,   /* which connection step this is */
                                    2);  /* total number of steps */

  purple_debug_info("motmot", "connecting to discovery server");

  conn -> gsc = purple_ssl_connect(acct, conn -> server, port, motmot_login_cb,
  (PurpleSslErrorFunction)  motmot_login_failure, gc);



  purple_connection_update_progress(gc, _("Connected"),
                                    1,   /* which connection step this is */
                                    2);  /* total number of steps */
  purple_connection_set_state(gc, PURPLE_CONNECTED);


  /* fetch stored offline messages */
  purple_debug_info("purplemot", "checking for offline messages for %s\n",
                    acct->username);
  offline_messages = g_hash_table_lookup(goffline_messages, acct->username);
  while (offline_messages) {
    GOfflineMessage *message = (GOfflineMessage *)offline_messages->data;
    purple_debug_info("purplemot", "delivering offline message to %s: %s\n",
                      acct->username, message->message);
    serv_got_im(gc, message->from, message->message, message->flags,
                message->mtime);
    offline_messages = g_list_next(offline_messages);

    g_free(message->from);
    g_free(message->message);
    g_free(message);
  }

  g_list_free(offline_messages);
  g_hash_table_remove(goffline_messages, &acct->username);
  

}

static gboolean do_login(PurpleConnection *gc){
  msgpack_sbuffer *buffer = msgpack_sbuffer_new(); 
  motmot_conn *conn = gc -> proto_data;
  PurpleAccount *a = conn -> account;

  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);

  msgpack_pack_array(pk, 3);
  msgpack_pack_int(pk, AUTHENTICATE_USER);

  const char *username = purple_account_get_username(a);
  const char *pass = purple_account_get_password(a);
  msgpack_pack_raw(pk, strlen(username) );
  msgpack_pack_raw_body(pk, username, strlen(username) );

  msgpack_pack_raw(pk, strlen(pass) );
  msgpack_pack_raw_body(pk, pass, strlen(pass) );

  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size); 

  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);
  return TRUE;
}

void get_all_statuses(motmot_conn *conn){
  purple_debug_info("motmot", "querying all statuses");
  msgpack_sbuffer *buffer = msgpack_sbuffer_new();
  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);
  msgpack_pack_array(pk, 1);

  msgpack_pack_int(pk, GET_ALL_STATUSES);
  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size);

  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);
}


void update_remote_status(PurpleAccount *a, const char *friend_name, int status){
  purple_debug_info("motmot", "updating status of %s to %d", friend_name, status);
  switch (status){
    case ONLINE: 
      purple_prpl_got_user_status(a, friend_name, NULL_STATUS_ONLINE, NULL); 
      break;
    case AWAY:
    case BUSY:
      purple_prpl_got_user_status(a, friend_name, NULL_STATUS_AWAY, NULL); 
      break;
    case OFFLINE:
      purple_prpl_got_user_status(a, friend_name, NULL_STATUS_OFFLINE, NULL); 
      break;
    default:
      return;
  }
}

static void auth_cb(void *data){
  PurpleConnection *gc = data;
  motmot_conn *conn = gc -> proto_data;
  conn -> acceptance_list = g_list_prepend(conn -> acceptance_list, 
    conn -> data);
  return;
  /*
  msgpack_sbuffer *buffer = msgpack_sbuffer_new();
  PurpleConnection *gc = data;
  motmot_conn *conn = gc -> proto_data;
  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);
  
  msgpack_pack_array(pk, 2);
  msgpack_pack_int(pk, ACCEPT_FRIEND);

  msgpack_pack_raw(pk, strlen(conn -> data));
  msgpack_pack_raw_body(pk, conn -> data, strlen(conn -> data));

  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size);

  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);
  g_free(conn -> data);
  */
}
static void deny_cb(void *data){
  /*
  PurpleConnection *gc = data;
  motmot_conn *conn = gc -> proto_data;
  g_free(conn -> data);
  */
  return;
  
}



static void motmot_parse(char *buffer, int len, PurpleConnection *gc){
  char *friend_name;
  int status;
  int opcode;
  int i;
  int error;
  const char *addr;
  int port;

  msgpack_object o;
  msgpack_object_array ar2;
  msgpack_object_array info_list;
  msgpack_object_array tuple;
  motmot_conn *conn = gc -> proto_data;
  PurpleAccount *a = conn -> account;
  PurpleBuddy *bud;
  motmot_buddy *proto;

  msgpack_object_array ar = deser_get_array(buffer, len, &error);
  purple_debug_info("motmot", "parsing data");
  if(error == PURPLEMOT_ERROR){
    purple_debug_info("motmot", "error");
    return;
  }
  if(ar.size <= 0){
    return;
  }

  opcode = deser_get_pos_int(ar, 0, &error);
  purple_debug_info("motmot", "opcode %d", opcode);
  if(error == PURPLEMOT_ERROR){
    return;
  }

  switch (opcode) {
    case ALL_STATUS_RESPONSE:
      if(ar.size <= 1){
        return;
      }
      o = (ar.ptr)[1];
      ar2 = get_array2(o, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      for(i = 0; i < ar2.size; i++){
        info_list = get_array2((ar2.ptr)[i], &error);
        if(error == PURPLEMOT_ERROR || info_list.size <= 3){
          return;
        }
        tuple = get_array2((info_list.ptr)[0], &error);
        if(error == PURPLEMOT_ERROR){
          return;
        }

        friend_name = deser_get_string(tuple, 0, &error);
        if(error == PURPLEMOT_ERROR){
          return;
        }

        status = deser_get_pos_int(tuple, 1, &error);
        if(error == PURPLEMOT_ERROR){
          return;
        }
        addr = deser_get_string(info_list, 1, &error);
        if(error == PURPLEMOT_ERROR){
          return;
        }
        port = deser_get_pos_int(info_list, 2, &error);
        if(error == PURPLEMOT_ERROR){
          return;
        }
        bud = purple_find_buddy(a, friend_name);
        if(bud == NULL){
          bud = purple_buddy_new(a, friend_name, NULL);
          proto = g_new0(motmot_buddy, 1);
          proto -> addr = addr;
          proto -> port = port;
          bud -> proto_data = proto;
          purple_blist_add_buddy(bud, NULL, NULL, NULL);
          update_remote_status(a, friend_name, status);
        }
        else{
          proto = g_new0(motmot_buddy, 1);
          proto -> addr = addr;
          proto -> port = port;
          bud -> proto_data = proto;
          update_remote_status(a, friend_name, status);
          g_free(friend_name);
        }
      }
      break;
    case GET_STATUS_RESP:
      if(ar.size <= 4){
        return;
      }
      tuple = get_array2((ar.ptr)[1], &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      friend_name = deser_get_string(tuple, 0, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      status = deser_get_pos_int(tuple, 1, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      addr = deser_get_string(tuple, 2, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      port = deser_get_pos_int(tuple, 3, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      update_remote_status(a, friend_name, status);

      bud = purple_find_buddy(a, friend_name);
      if(bud == NULL){
        return;
      }
      proto = bud -> proto_data;
      proto -> addr = addr;
      proto -> port = port;
      g_free(friend_name);
      break;
    case PUSH_FRIEND_ACCEPT:
    case PUSH_CLIENT_STATUS:
      friend_name = deser_get_string(ar, 1, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      status = deser_get_pos_int(ar, 2, &error);
      if(error == PURPLEMOT_ERROR){
        g_free(friend_name);
        return;
      }
      update_remote_status(a, friend_name, status);

      if(opcode == PUSH_FRIEND_ACCEPT){
        break;
      }

      addr = deser_get_string(tuple, 2, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      port = deser_get_pos_int(tuple, 3, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      bud = purple_find_buddy(a, friend_name);
      if(bud == NULL){
        return;
      }
      proto = bud -> proto_data;
      proto -> addr = addr;
      proto -> port = port;
      g_free(friend_name);
      break;
    case PUSH_FRIEND_REQUEST:
      purple_debug_info("purplemot", "getting friend request");
      friend_name = deser_get_string(ar, 1, &error);
      if(error == PURPLEMOT_ERROR){
        return;
      }
      if(purple_find_buddy(a, friend_name) != NULL){
        break;
      }
      conn -> data = friend_name;

      purple_account_request_authorization(a, friend_name, NULL, NULL, NULL,          FALSE, auth_cb, deny_cb, gc);
      break;
    case ACCESS_DENIED:;
    case AUTH_FAILED:
     purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, _("Authentication failed, please check that your username and password are correct"));
     break;
    default:
      return;
  }
}


/* read in data, parse, undertake appropriate action */

static void motmot_input_cb(gpointer *data, PurpleSslConnection *gsc, PurpleInputCondition cond){
  PurpleConnection *gc = (PurpleConnection *) data;
  gsize l;
  gsize read_bytes = 0;
	if(!g_list_find(purple_connections_get_all(), gc)) {
		purple_ssl_close(gsc);
		return;
	}

  char *buffer = g_malloc(BUFF_SIZE);
  char *nxt = buffer;

  purple_debug_info("motmot", "reading data");
  while(true){
    l = purple_ssl_read(gsc, nxt, BUFF_SIZE);

    if (l < 0 && errno == EAGAIN) {
      /* Try again later */
      return;
    } else if (l < 0) {
      gchar *tmp = g_strdup_printf(_("Lost connection with server: %s"),
          g_strerror(errno));
      purple_connection_error_reason (gc,
        PURPLE_CONNECTION_ERROR_NETWORK_ERROR, tmp);
      g_free(tmp);
      return;
	  }
    read_bytes += l;
    if(l < BUFF_SIZE){
      break;
    }

    buffer = g_realloc(buffer, read_bytes + BUFF_SIZE);
    nxt = buffer + read_bytes;

  }


  if (read_bytes == 0) {
		purple_connection_error_reason (gc,
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			_("Server closed the connection"));
		return;
	}

  motmot_parse(buffer, l, gc);
  g_free(buffer);
  return;
}


static void motmot_login_cb(gpointer data, PurpleSslConnection *gsc, PurpleInputCondition cond){
  purple_debug_info("motmot", "connection achieved");
  PurpleConnection *gc = data;
  motmot_conn *conn = gc -> proto_data;
  PurpleAccount *acct = conn -> account;
	if (do_login(gc)) {
		purple_ssl_input_add(gsc, (PurpleSslInputFunction) motmot_input_cb, gc);
    /* tell purple about everyone on our buddy list who's connected */
    get_all_statuses(conn);

    /* notify other purplemot accounts */
    motmot_report_status(purple_status_get_id(purple_account_get_active_status(acct)), conn);
	}
}


static void
motmot_login_failure(PurpleSslConnection *gsc, PurpleSslErrorType error,
		gpointer data)
{
	PurpleConnection *gc = data;
	motmot_conn *motmot = gc->proto_data;

	motmot->gsc = NULL;

	purple_connection_ssl_error (gc, error);
}




static void purplemot_close(PurpleConnection *gc)
{
  /* notify other purplemot accounts */
  motmot_conn *conn = gc -> proto_data;
  PurpleAccount *a = conn -> account;
  motmot_report_status(purple_status_get_id(purple_account_get_active_status(a)), conn);

  purple_ssl_close(conn -> gsc); 

  g_list_free_full(conn -> acceptance_list, g_free);
}

static int purplemot_send_im(PurpleConnection *gc, const char *who,
                            const char *message, PurpleMessageFlags flags)
{

  const char *from_username = gc->account->username;
  PurpleMessageFlags receive_flags = ((flags & ~PURPLE_MESSAGE_SEND)
                                      | PURPLE_MESSAGE_RECV);
  PurpleAccount *to_acct = purple_accounts_find(who, PURPLEMOT_ID);
  PurpleConnection *to;

  purple_debug_info("purplemot", "sending message from %s to %s: %s\n",
                    from_username, who, message);

  /* is the sender blocked by the recipient's privacy settings? */
  if (to_acct && !purple_privacy_check(to_acct, gc->account->username)) {
    char *msg = g_strdup_printf(
      _("Your message was blocked by %s's privacy settings."), who);
    purple_debug_info("purplemot",
                      "discarding; %s is blocked by %s's privacy settings\n",
                      from_username, who);
    purple_conv_present_error(who, gc->account, msg);
    g_free(msg);
    return 0;
  }

  /* is the recipient online? */
  to = get_purplemot_gc(who);
  if (to) {  /* yes, send */

    serv_got_im(to, from_username, message, receive_flags, time(NULL));

  } else {  /* nope, store as an offline message */
    GOfflineMessage *offline_message;
    GList *messages;

    purple_debug_info("purplemot",
                      "%s is offline, sending as offline message\n", who);
    offline_message = g_new0(GOfflineMessage, 1);
    offline_message->from = g_strdup(from_username);
    offline_message->message = g_strdup(message);
    offline_message->mtime = time(NULL);
    offline_message->flags = receive_flags;

    messages = g_hash_table_lookup(goffline_messages, who);
    messages = g_list_append(messages, offline_message);
    g_hash_table_insert(goffline_messages, g_strdup(who), messages);
  }

   return 1;
}

static void purplemot_set_info(PurpleConnection *gc, const char *info) {
  purple_debug_info("purplemot", "setting %s's user info to %s\n",
                    gc->account->username, info);
}

static const char *typing_state_to_string(PurpleTypingState typing) {
  switch (typing) {
  case PURPLE_NOT_TYPING:  return "is not typing";
  case PURPLE_TYPING:      return "is typing";
  case PURPLE_TYPED:       return "stopped typing momentarily";
  default:               return "unknown typing state";
  }
}

static void notify_typing(PurpleConnection *from, PurpleConnection *to,
                          gpointer typing) {
  const char *from_username = from->account->username;
  const char *action = typing_state_to_string((PurpleTypingState)typing);
  purple_debug_info("purplemot", "notifying %s that %s %s\n",
                    to->account->username, from_username, action);

  serv_got_typing(to,
                  from_username,
                  0, /* if non-zero, a timeout in seconds after which to
                      * reset the typing status to PURPLE_NOT_TYPING */
                  (PurpleTypingState)typing);
}

static unsigned int purplemot_send_typing(PurpleConnection *gc, const char *name,
                                         PurpleTypingState typing) {
  purple_debug_info("purplemot", "%s %s\n", gc->account->username,
                    typing_state_to_string(typing));
  foreach_purplemot_gc(notify_typing, gc, (gpointer)typing);
  return 0;
}

static void purplemot_get_info(PurpleConnection *gc, const char *username) {
  const char *body;
  PurpleNotifyUserInfo *info = purple_notify_user_info_new();
  PurpleAccount *acct;

  purple_debug_info("purplemot", "Fetching %s's user info for %s\n", username,
                    gc->account->username);

  if (!get_purplemot_gc(username)) {
    char *msg = g_strdup_printf(_("%s is not logged in."), username);
    purple_notify_error(gc, _("User Info"), _("User info not available. "), msg);
    g_free(msg);
  }

  acct = purple_accounts_find(username, PURPLEMOT_ID);
  if (acct)
    body = purple_account_get_user_info(acct);
  else
    body = _("No user info.");
  purple_notify_user_info_add_pair(info, "Info", body);

  /* show a buddy's user info in a nice dialog box */
  purple_notify_userinfo(gc,        /* connection the buddy info came through */
                         username,  /* buddy's username */
                         info,      /* body */
                         NULL,      /* callback called when dialog closed */
                         NULL);     /* userdata for callback */
}

static void motmot_report_status(const char *id, motmot_conn *conn){
  int code;
  msgpack_sbuffer *buffer;
  if(!strcmp(id, NULL_STATUS_ONLINE)){
    code = ONLINE;
  }
  else if(!strcmp(id, NULL_STATUS_AWAY)){
    code = AWAY;
  }
  else if(!strcmp(id, NULL_STATUS_OFFLINE)){
    code = OFFLINE;
  }
  else{
    purple_debug_info("motmot", "unknown status %s\n", id);
    return;
  }


  buffer = msgpack_sbuffer_new();
  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);

  msgpack_pack_array(pk, 2);
  msgpack_pack_int(pk, REGISTER_STATUS);
  msgpack_pack_int(pk, code);

  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size);
  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);
}

static void purplemot_set_status(PurpleAccount *acct, PurpleStatus *status) {
  PurpleConnection *gc = purple_account_get_connection(acct);
  motmot_conn *conn = gc -> proto_data;
  purple_debug_info("motmot", "setting status and reporting");

  
  motmot_report_status(purple_status_get_id(status), conn);
  
}

static void purplemot_set_idle(PurpleConnection *gc, int idletime) {
  purple_debug_info("purplemot",
                    "purple reports that %s has been idle for %d seconds\n",
                    gc->account->username, idletime);
}


static void purplemot_change_passwd(PurpleConnection *gc, const char *old_pass,
                                   const char *new_pass) {
  purple_debug_info("purplemot", "%s wants to change their password\n",
                    gc->account->username);
}

static gint motmot_node_cmp(const char *a, const char *b){
  return strcmp(a, b);
}
/**
 * purple_mot_add_buddy - code called when a buddy is added in libpurple
 *
 * @param gc The account connection
 * @param buddy The buddy to be added
 * @param group The group (we don't use this)
 */
static void purplemot_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                               PurpleGroup *group)
{
  char *name;
  purple_debug_info("purplemot", "buddy add code");
  
  msgpack_sbuffer *buffer = msgpack_sbuffer_new();
  motmot_conn *conn = gc -> proto_data;

  motmot_buddy *proto = g_new0(motmot_buddy, 1);
  buddy -> proto_data = proto;

  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);
  GList *el = g_list_find_custom(conn -> acceptance_list, 
    purple_buddy_get_name(buddy), (GCompareFunc) motmot_node_cmp);

  // This is a hack so that the acceptance to a friend request is only sent once the buddy is added on our end,
  // not when the add is authorized.
  if(el != NULL){
    name = el -> data;
    msgpack_pack_array(pk, 2);
    msgpack_pack_int(pk, ACCEPT_FRIEND);

    msgpack_pack_raw(pk, strlen(name));
    msgpack_pack_raw_body(pk, name, strlen(name));

    purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size);
    query_status(name, conn);

    msgpack_sbuffer_free(buffer);
    msgpack_packer_free(pk);
    conn -> acceptance_list = g_list_remove(conn -> acceptance_list, name);
    g_free(name);
    
    return;
  }

  // send out the request
  msgpack_pack_array(pk, 2);
  msgpack_pack_int(pk, REGISTER_FRIEND);
  msgpack_pack_raw(pk, strlen(purple_buddy_get_name(buddy)));
  msgpack_pack_raw_body(pk, purple_buddy_get_name(buddy), strlen(purple_buddy_get_name(buddy)));

 
  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size);
  purple_debug_info("motmot", "request sent");
  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);

/*
  const char *username = gc->account->username;
  PurpleConnection *buddy_gc = get_purplemot_gc(buddy->name);

  purple_debug_info("purplemot", "adding %s to %s's buddy list\n", buddy->name,
                    username);

  if (buddy_gc) {
    PurpleAccount *buddy_acct = buddy_gc->account;

    discover_status(gc, buddy_gc, NULL);

    if (purple_find_buddy(buddy_acct, username)) {
      purple_debug_info("purplemot", "%s is already on %s's buddy list\n",
                        username, buddy->name);
    } else {
      purple_debug_info("purplemot", "asking %s if they want to add %s\n",
                        buddy->name, username);
      purple_account_request_add(buddy_acct,
                                 username, 
                                 NULL,  
                                 NULL,   
                                 NULL);  
    }
  }
  */
}

static void purplemot_add_buddies(PurpleConnection *gc, GList *buddies,
                                 GList *groups) {
  GList *buddy = buddies;
  GList *group = groups;

  purple_debug_info("purplemot", "adding multiple buddies\n");

  while (buddy && group) {
    purplemot_add_buddy(gc, (PurpleBuddy *)buddy->data, (PurpleGroup *)group->data);
    buddy = g_list_next(buddy);
    group = g_list_next(group);
  }
}

static void purplemot_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                                  PurpleGroup *group)
{
  const char *name = purple_buddy_get_name(buddy);
  motmot_conn *conn = gc -> proto_data;
  msgpack_sbuffer *buffer = msgpack_sbuffer_new();
  msgpack_packer *pk = msgpack_packer_new(buffer, msgpack_sbuffer_write);
  msgpack_pack_array(pk, 2);
  msgpack_pack_int(pk, UNREGISTER_FRIEND);

  msgpack_pack_raw(pk, strlen(name));
  msgpack_pack_raw_body(pk, name, strlen(name));

  purple_ssl_write(conn -> gsc, buffer -> data, buffer -> size);
 
  msgpack_sbuffer_free(buffer);
  msgpack_packer_free(pk);
}

static void purplemot_remove_buddies(PurpleConnection *gc, GList *buddies,
                                    GList *groups) {
  GList *buddy = buddies;
  GList *group = groups;

  purple_debug_info("purplemot", "removing multiple buddies\n");

  while (buddy && group) {
    purplemot_remove_buddy(gc, (PurpleBuddy *)buddy->data,
                          (PurpleGroup *)group->data);
    buddy = g_list_next(buddy);
    group = g_list_next(group);
  }
}

/*
 * purplemot uses purple's local whitelist and blacklist, stored in blist.xml, as
 * its authoritative privacy settings, and uses purple's logic (specifically
 * purple_privacy_check(), from privacy.h), to determine whether messages are
 * allowed or blocked.
 */
static void purplemot_add_permit(PurpleConnection *gc, const char *name) {
  purple_debug_info("purplemot", "%s adds %s to their allowed list\n",
                    gc->account->username, name);
}

static void purplemot_add_deny(PurpleConnection *gc, const char *name) {
  purple_debug_info("purplemot", "%s adds %s to their blocked list\n",
                    gc->account->username, name);
}

static void purplemot_rem_permit(PurpleConnection *gc, const char *name) {
  purple_debug_info("purplemot", "%s removes %s from their allowed list\n",
                    gc->account->username, name);
}

static void purplemot_rem_deny(PurpleConnection *gc, const char *name) {
  purple_debug_info("purplemot", "%s removes %s from their blocked list\n",
                    gc->account->username, name);
}

static void purplemot_set_permit_deny(PurpleConnection *gc) {
  /* this is for synchronizing the local black/whitelist with the server.
   * for purplemot, it's a noop.
   */
}

static void joined_chat(PurpleConvChat *from, PurpleConvChat *to,
                        int id, const char *room, gpointer userdata) {
  /*  tell their chat window that we joined */
  purple_debug_info("purplemot", "%s sees that %s joined chat room %s\n",
                    to->nick, from->nick, room);
  purple_conv_chat_add_user(to,
                            from->nick,
                            NULL,   /* user-provided join message, IRC style */
                            PURPLE_CBFLAGS_NONE,
                            TRUE);  /* show a join message */

  if (from != to) {
    /* add them to our chat window */
    purple_debug_info("purplemot", "%s sees that %s is in chat room %s\n",
                      from->nick, to->nick, room);
    purple_conv_chat_add_user(from,
                              to->nick,
                              NULL,   /* user-provided join message, IRC style */
                              PURPLE_CBFLAGS_NONE,
                              FALSE);  /* show a join message */
  }
}

// calls motmot_invite (libmotmot)
static void purplemot_join_chat(PurpleConnection *gc, GHashTable *components) {
  // JULIE
  // to join chat, first call upon almighty paxos
  // must figure out distinction between *joining* a chat and *creating* a chat

  const char *username = gc->account->username;
  const char *room = g_hash_table_lookup(components, "room");
  int chat_id = g_str_hash(room);
  purple_debug_info("purplemot", "%s is joining chat room %s\n", username, room);

  if (!purple_find_chat(gc, chat_id)) {
    serv_got_joined_chat(gc, chat_id, room);

    /* tell everyone that we joined, and add them if they're already there */
    foreach_gc_in_chat(joined_chat, gc, chat_id, NULL);
  } else {
    char *tmp = g_strdup_printf(_("%s is already in chat room %s."),
                                username,
                                room);
    purple_debug_info("purplemot", "%s is already in chat room %s\n", username,
                      room);
    purple_notify_info(gc, _("Join chat"), _("Join chat"), tmp);
    g_free(tmp);
  }
}

static void purplemot_reject_chat(PurpleConnection *gc, GHashTable *components) {
  const char *invited_by = g_hash_table_lookup(components, "invited_by");
  const char *room = g_hash_table_lookup(components, "room");
  const char *username = gc->account->username;
  PurpleConnection *invited_by_gc = get_purplemot_gc(invited_by);
  char *message = g_strdup_printf(
    "%s %s %s.",
    username,
    _("has rejected your invitation to join the chat room"),
    room);

  purple_debug_info("purplemot",
                    "%s has rejected %s's invitation to join chat room %s\n",
                    username, invited_by, room);

  purple_notify_info(invited_by_gc,
                     _("Chat invitation rejected"),
                     _("Chat invitation rejected"),
                     message);
  g_free(message);
}

static char *purplemot_get_chat_name(GHashTable *components) {
  const char *room = g_hash_table_lookup(components, "room");
  purple_debug_info("purplemot", "reporting chat room name '%s'\n", room);
  return g_strdup(room);
}

static void purplemot_chat_invite(PurpleConnection *gc, int id,
                                 const char *message, const char *who) {
  const char *username = gc->account->username;
  PurpleConversation *conv = purple_find_chat(gc, id);
  const char *room = conv->name;
  PurpleAccount *to_acct = purple_accounts_find(who, PURPLEMOT_ID);

  purple_debug_info("purplemot", "%s is inviting %s to join chat room %s\n",
                    username, who, room);

  if (to_acct) {
    PurpleConversation *to_conv = purple_find_chat(to_acct->gc, id);
    if (to_conv) {
      char *tmp = g_strdup_printf("%s is already in chat room %s.", who, room);
      purple_debug_info("purplemot",
                        "%s is already in chat room %s; "
                        "ignoring invitation from %s\n",
                        who, room, username);
      purple_notify_info(gc, _("Chat invitation"), _("Chat invitation"), tmp);
      g_free(tmp);
    } else {
      GHashTable *components;
      components = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
      g_hash_table_replace(components, "room", g_strdup(room));
      g_hash_table_replace(components, "invited_by", g_strdup(username));
      serv_got_chat_invite(to_acct->gc, room, username, message, components);
    }
  }
}



// JULIE: add disconnect back in
// calls motmot_disconnect (libmotmot)
static void purplemot_chat_leave(PurpleConnection *gc, int id) {
  motmot_conn *proto = gc -> proto_data;
  MotmotInfo *info =  find_info(proto, id);

  if(info == NULL){
    return;
  }
  // disconnect
  // motmot_disconnect(info -> internal_data);
  purple_debug_info("purplemot", "%s is leaving chat room %s\n",
                    gc->account->username, "THE CHAT");

}

static PurpleCmdRet send_whisper(PurpleConversation *conv, const gchar *cmd,
                                 gchar **args, gchar **error, void *userdata) {
  const char *to_username;
  const char *message;
  const char *from_username;
  PurpleConvChat *chat;
  PurpleConvChatBuddy *chat_buddy;
  PurpleConnection *to;

  /* parse args */
  to_username = args[0];
  message = args[1];

  if (!to_username || strlen(to_username) == 0) {
    *error = g_strdup(_("Whisper is missing recipient."));
    return PURPLE_CMD_RET_FAILED;
  } else if (!message || strlen(message) == 0) {
    *error = g_strdup(_("Whisper is missing message."));
    return PURPLE_CMD_RET_FAILED;
  }

  from_username = conv->account->username;
  purple_debug_info("purplemot", "%s whispers to %s in chat room %s: %s\n",
                    from_username, to_username, conv->name, message);

  chat = purple_conversation_get_chat_data(conv);
  chat_buddy = purple_conv_chat_cb_find(chat, to_username);
  to = get_purplemot_gc(to_username);

  if (!chat_buddy) {
    /* this will be freed by the caller */
    *error = g_strdup_printf(_("%s is not logged in."), to_username);
    return PURPLE_CMD_RET_FAILED;
  } else if (!to) {
    *error = g_strdup_printf(_("%s is not in this chat room."), to_username);
    return PURPLE_CMD_RET_FAILED;
  } else {
    /* write the whisper in the sender's chat window  */
    char *message_to = g_strdup_printf("%s (to %s)", message, to_username);
    purple_conv_chat_write(chat, from_username, message_to,
                           PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_WHISPER,
                           time(NULL));
    g_free(message_to);

    /* send the whisper */
    serv_chat_whisper(to, chat->id, from_username, message);

    return PURPLE_CMD_RET_OK;
  }
}

static void purplemot_chat_whisper(PurpleConnection *gc, int id, const char *who,
                                  const char *message) {
  const char *username = gc->account->username;
  PurpleConversation *conv = purple_find_chat(gc, id);
  purple_debug_info("purplemot",
                    "%s receives whisper from %s in chat room %s: %s\n",
                    username, who, conv->name, message);

  /* receive whisper on recipient's account */
  serv_got_chat_in(gc, id, who, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_WHISPER,
                   message, time(NULL));
}


// calls motmot_send (libmotmot function)
static int purplemot_chat_send(PurpleConnection *gc, int id, const char *message,
                              PurpleMessageFlags flags) {
  motmot_conn *proto = gc -> proto_data;
  MotmotInfo *info =  find_info(proto, id);

  if(info == NULL){
    return -1;
  }
  // motmot_send(message,strlen(message) + 1, info -> internal_data);
  return 0;
}


static void purplemot_register_user(PurpleAccount *acct) {
 purple_debug_info("purplemot", "registering account for %s\n",
                   acct->username);
}

static void purplemot_get_cb_info(PurpleConnection *gc, int id, const char *who) {
  PurpleConversation *conv = purple_find_chat(gc, id);
  purple_debug_info("purplemot",
                    "retrieving %s's info for %s in chat room %s\n", who,
                    gc->account->username, conv->name);

  purplemot_get_info(gc, who);
}

static void purplemot_alias_buddy(PurpleConnection *gc, const char *who,
                                 const char *alias) {
 purple_debug_info("purplemot", "%s sets %s's alias to %s\n",
                   gc->account->username, who, alias);
}

static void purplemot_group_buddy(PurpleConnection *gc, const char *who,
                                 const char *old_group,
                                 const char *new_group) {
  purple_debug_info("purplemot", "%s has moved %s from group %s to group %s\n",
                    gc->account->username, who, old_group, new_group);
}

static void purplemot_rename_group(PurpleConnection *gc, const char *old_name,
                                  PurpleGroup *group, GList *moved_buddies) {
  purple_debug_info("purplemot", "%s has renamed group %s to %s\n",
                    gc->account->username, old_name, group->name);
}

static void purplemot_convo_closed(PurpleConnection *gc, const char *who) {
  purple_debug_info("purplemot", "%s's conversation with %s was closed\n",
                    gc->account->username, who);
}

/* normalize a username (e.g. remove whitespace, add default domain, etc.)
 * for purplemot, this is a noop.
 */
static const char *purplemot_normalize(const PurpleAccount *acct,
                                      const char *input) {
  return NULL;
}

static void purplemot_set_buddy_icon(PurpleConnection *gc,
                                    PurpleStoredImage *img) {
 purple_debug_info("purplemot", "setting %s's buddy icon to %s\n",
                   gc->account->username,
                   img ? purple_imgstore_get_filename(img) : "(null)");
}

static void purplemot_remove_group(PurpleConnection *gc, PurpleGroup *group) {
  purple_debug_info("purplemot", "%s has removed group %s\n",
                    gc->account->username, group->name);
}


static void set_chat_topic_fn(PurpleConvChat *from, PurpleConvChat *to,
                              int id, const char *room, gpointer userdata) {
  const char *topic = (const char *)userdata;
  const char *username = from->conv->account->username;
  char *msg;

  purple_conv_chat_set_topic(to, username, topic);

  if (topic && strlen(topic) > 0)
    msg = g_strdup_printf(_("%s sets topic to: %s"), username, topic);
  else
    msg = g_strdup_printf(_("%s clears topic"), username);

  purple_conv_chat_write(to, username, msg,
                         PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG,
                         time(NULL));
  g_free(msg);
}

static void purplemot_set_chat_topic(PurpleConnection *gc, int id,
                                    const char *topic) {
  PurpleConversation *conv = purple_find_chat(gc, id);
  PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
  const char *last_topic;

  if (!chat)
    return;

  purple_debug_info("purplemot", "%s sets topic of chat room '%s' to '%s'\n",
                    gc->account->username, conv->name, topic);

  last_topic = purple_conv_chat_get_topic(chat);
  if ((!topic && !last_topic) ||
      (topic && last_topic && !strcmp(topic, last_topic)))
    return;  /* topic is unchanged, this is a noop */

  foreach_gc_in_chat(set_chat_topic_fn, gc, id, (gpointer)topic);
}

static gboolean purplemot_finish_get_roomlist(gpointer roomlist) {
  purple_roomlist_set_in_progress((PurpleRoomlist *)roomlist, FALSE);
  return FALSE;
}

static PurpleRoomlist *purplemot_roomlist_get_list(PurpleConnection *gc) {
  const char *username = gc->account->username;
  PurpleRoomlist *roomlist = purple_roomlist_new(gc->account);
  GList *fields = NULL;
  PurpleRoomlistField *field;
  GList *chats;
  GList *seen_ids = NULL;

  purple_debug_info("purplemot", "%s asks for room list; returning:\n", username);

  /* set up the room list */
  field = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "room",
                                    "room", TRUE /* hidden */);
  fields = g_list_append(fields, field);

  field = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT, "Id", "Id", FALSE);
  fields = g_list_append(fields, field);

  purple_roomlist_set_fields(roomlist, fields);

  /* add each chat room. the chat ids are cached in seen_ids so that each room
   * is only returned once, even if multiple users are in it. */
  for (chats  = purple_get_chats(); chats; chats = g_list_next(chats)) {
    PurpleConversation *conv = (PurpleConversation *)chats->data;
    PurpleRoomlistRoom *room;
    const char *name = conv->name;
    int id = purple_conversation_get_chat_data(conv)->id;

    /* have we already added this room? */
    if (g_list_find_custom(seen_ids, name, (GCompareFunc)strcmp))
      continue;                                /* yes! try the next one. */

    /* This cast is OK because this list is only staying around for the life
     * of this function and none of the conversations are being deleted
     * in that timespan. */
    seen_ids = g_list_prepend(seen_ids, (char *)name); /* no, it's new. */
    purple_debug_info("purplemot", "%s (%d), ", name, id);

    room = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, name, NULL);
    purple_roomlist_room_add_field(roomlist, room, name);
    purple_roomlist_room_add_field(roomlist, room, &id);
    purple_roomlist_room_add(roomlist, room);
  }

  g_list_free(seen_ids);
  purple_timeout_add(1 /* ms */, purplemot_finish_get_roomlist, roomlist);
  return roomlist;
}

static void purplemot_roomlist_cancel(PurpleRoomlist *list) {
 purple_debug_info("purplemot", "%s asked to cancel room list request\n",
                   list->account->username);
}

static void purplemot_roomlist_expand_category(PurpleRoomlist *list,
                                              PurpleRoomlistRoom *category) {
 purple_debug_info("purplemot", "%s asked to expand room list category %s\n",
                   list->account->username, category->name);
}

/* purplemot doesn't support file transfer...yet... */
static gboolean purplemot_can_receive_file(PurpleConnection *gc,
                                          const char *who) {
  return FALSE;
}

static gboolean purplemot_offline_message(const PurpleBuddy *buddy) {
  purple_debug_info("purplemot",
                    "reporting that offline messages are supported for %s\n",
                    buddy->name);
  return TRUE;
}


/*
 * prpl stuff. see prpl.h for more information.
 */

static PurplePluginProtocolInfo prpl_info =
{
  /* OPT_PROTO_NO_PASSWORD |  OPT_PROTO_CHAT_TOPIC */ 0,  /* options */
  NULL,               /* user_splits, initialized in purplemot_init() */
  NULL,               /* protocol_options, initialized in purplemot_init() */
  {   /* icon_spec, a PurpleBuddyIconSpec */
      "png,jpg,gif",                   /* format */
      0,                               /* min_width */
      0,                               /* min_height */
      128,                             /* max_width */
      128,                             /* max_height */
      10000,                           /* max_filesize */
      PURPLE_ICON_SCALE_DISPLAY,       /* scale_rules */
  },
  purplemot_list_icon,                  /* list_icon */
  NULL,                                /* list_emblem */
  purplemot_status_text,                /* status_text */
  purplemot_tooltip_text,               /* tooltip_text */
  purplemot_status_types,               /* status_types */
  purplemot_blist_node_menu,            /* blist_node_menu */
  purplemot_chat_info,                  /* chat_info */
  purplemot_chat_info_defaults,         /* chat_info_defaults */
  purplemot_login,                      /* login */
  purplemot_close,                      /* close */
  purplemot_send_im,                    /* send_im */
  purplemot_set_info,                   /* set_info */
  purplemot_send_typing,                /* send_typing */
  purplemot_get_info,                   /* get_info */
  purplemot_set_status,                 /* set_status */
  purplemot_set_idle,                   /* set_idle */
  purplemot_change_passwd,              /* change_passwd */
  purplemot_add_buddy,                  /* add_buddy */
  purplemot_add_buddies,                /* add_buddies */
  purplemot_remove_buddy,               /* remove_buddy */
  purplemot_remove_buddies,             /* remove_buddies */
  purplemot_add_permit,                 /* add_permit */
  purplemot_add_deny,                   /* add_deny */
  purplemot_rem_permit,                 /* rem_permit */
  purplemot_rem_deny,                   /* rem_deny */
  purplemot_set_permit_deny,            /* set_permit_deny */
  purplemot_join_chat,                  /* join_chat */
  purplemot_reject_chat,                /* reject_chat */
  purplemot_get_chat_name,              /* get_chat_name */
  purplemot_chat_invite,                /* chat_invite */
  purplemot_chat_leave,                 /* chat_leave */
  purplemot_chat_whisper,               /* chat_whisper */
  purplemot_chat_send,                  /* chat_send */
  NULL,                                /* keepalive */
  purplemot_register_user,              /* register_user */
  purplemot_get_cb_info,                /* get_cb_info */
  NULL,                                /* get_cb_away */
  purplemot_alias_buddy,                /* alias_buddy */
  purplemot_group_buddy,                /* group_buddy */
  purplemot_rename_group,               /* rename_group */
  NULL,                                /* buddy_free */
  purplemot_convo_closed,               /* convo_closed */
  purplemot_normalize,                  /* normalize */
  purplemot_set_buddy_icon,             /* set_buddy_icon */
  purplemot_remove_group,               /* remove_group */
  NULL,                                /* get_cb_real_name */
  purplemot_set_chat_topic,             /* set_chat_topic */
  NULL,                                /* find_blist_chat */
  purplemot_roomlist_get_list,          /* roomlist_get_list */
  purplemot_roomlist_cancel,            /* roomlist_cancel */
  purplemot_roomlist_expand_category,   /* roomlist_expand_category */
  purplemot_can_receive_file,           /* can_receive_file */
  NULL,                                /* send_file */
  NULL,                                /* new_xfer */
  purplemot_offline_message,            /* offline_message */
  NULL,                                /* whiteboard_prpl_ops */
  NULL,                                /* send_raw */
  NULL,                                /* roomlist_room_serialize */
  NULL,                                /* unregister_user */
  NULL,                                /* send_attention */
  NULL,                                /* get_attention_types */
  sizeof(PurplePluginProtocolInfo),    /* struct_size */
  NULL,                                /* get_account_text_table */
  NULL,                                /* initiate_media */
  NULL,                                /* get_media_caps */
  NULL,                                /* get_moods */
  NULL,                                /* set_public_alias */
  NULL,                                /* get_public_alias */
  NULL,                                /* add_buddy_with_invite */
  NULL                                 /* add_buddies_with_invite */
};

static void purplemot_init(PurplePlugin *plugin)
{
  /* see accountopt.h for information about user splits and protocol options */
  PurpleAccountUserSplit *split = purple_account_user_split_new(
    _("Discovery Server"),  /* text shown to user */
    DEFAULT_SERV,                /* default value */
    '@');                     /* field separator */
  PurpleAccountOption *option = purple_account_option_int_new(
    _("Port"),      /* text shown to user */
    "disc_port",                /* pref name */
    DEFAULT_PORT);               /* default value */

  /* MOTMOT! */
  purple_debug_info("purplemot", "starting up\n");

  prpl_info.user_splits = g_list_append(NULL, split);
  prpl_info.protocol_options = g_list_append(NULL, option);

  /* register whisper chat command, /msg */
  purple_cmd_register("msg",
                    "ws",                  /* args: recipient and message */
                    PURPLE_CMD_P_DEFAULT,  /* priority */
                    PURPLE_CMD_FLAG_CHAT,
                    "prpl-null",
                    send_whisper,
                    "msg &lt;username&gt; &lt;message&gt;: send a private message, aka a whisper",
                    NULL);                 /* userdata */

  /* get ready to store offline messages */
  goffline_messages = g_hash_table_new_full(g_str_hash,  /* hash fn */
                                            g_str_equal, /* key comparison fn */
                                            g_free,      /* key free fn */
                                            NULL);       /* value free fn */

  _null_protocol = plugin;
}

static void purplemot_destroy(PurplePlugin *plugin) {
  purple_debug_info("purplemot", "shutting down\n");
}


static PurplePluginInfo info =
{
  PURPLE_PLUGIN_MAGIC,                                     /* magic */
  PURPLE_MAJOR_VERSION,                                    /* major_version */
  PURPLE_MINOR_VERSION,                                    /* minor_version */
  PURPLE_PLUGIN_PROTOCOL,                                  /* type */
  NULL,                                                    /* ui_requirement */
  0,                                                       /* flags */
  NULL,                                                    /* dependencies */
  PURPLE_PRIORITY_DEFAULT,                                 /* priority */
  PURPLEMOT_ID,                                             /* id */
  "MOTMOT!",                                 /* name */
  DISPLAY_VERSION,                                         /* version */
  N_("Null Protocol Plugin"),                              /* summary */
  N_("Null Protocol Plugin"),                              /* description */
  NULL,                                                    /* author */
  MOTMOT_WEBSITE,                                          /* homepage */
  NULL,                                                    /* load */
  NULL,                                                    /* unload */
  purplemot_destroy,                                        /* destroy */
  NULL,                                                    /* ui_info */
  &prpl_info,                                              /* extra_info */
  NULL,                                                    /* prefs_info */
  purplemot_actions,                                        /* actions */
  NULL,                                                    /* padding... */
  NULL,
  NULL,
  NULL,
};

PURPLE_INIT_PLUGIN(null, purplemot_init, info);





