/*! \file   janus.h
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU Affero General Public License v3
 * \brief  Janus core (headers)
 * \details Implementation of the gateway core. This code takes care of
 * the gateway initialization (command line/configuration) and setup,
 * and implements the web server (based on libmicrohttpd) and Janus protocol
 * (a JSON protocol implemented with Jansson) to interact with the web
 * applications. The core also takes care of bridging peers and plugins
 * accordingly. 
 * 
 * \ingroup core
 * \ref core
 */
 
#ifndef _JANUS_GATEWAY_H
#define _JANUS_GATEWAY_H

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <jansson.h>
#include <microhttpd.h>

#include "mutex.h"
#include "dtls.h"
#include "ice.h"
#include "plugins/plugin.h"


#define BUFSIZE	4096


/*! \brief Incoming HTTP message */
typedef struct janus_http_msg {
	/*! \brief Value of the Access-Control-Request-Headers HTTP header, if any (needed for CORS) */
	gchar *acrh;
	/*! \brief Value of the Access-Control-Request-Method HTTP header, if any (needed for CORS) */
	gchar *acrm;
	/*! \brief Content-Type of the payload */
	gchar *contenttype;
	/*! \brief Payload of the message */
	gchar *payload;
	/*! \brief Length of the message in octets */
	size_t len;
	/*! \brief Gateway-Client session identifier this message belongs to */
	gint64 session_id;
} janus_http_msg;

/*! \brief HTTP event to push */
typedef struct janus_http_event {
	/*! \brief HTTP response code */
	gint code;
	/*! \brief Payload to send to the client, if any */
	gchar *payload;
	/*! \brief Whether the payload has been allocated (and thus needs to be freed) or not */
	gint allocated:1;
} janus_http_event;


/*! \brief Gateway-Client session */
typedef struct janus_session {
	/*! \brief Janus Gateway-Client session ID */
	guint64 session_id;
	/*! \brief Map of handles this session is managing */
	GHashTable *ice_handles;
	/*! \brief Queue of outgoing messages to push */
	GQueue *messages;
	/*! \brief Flag to trigger a lazy session destruction */
	gint destroy:1;
	/*! \brief Mutex to lock/unlock this session */
	janus_mutex mutex;
} janus_session;


/** @name Janus Gateway-Client session methods
 */
///@{
/*! \brief Method to create a new Janus Gateway-Client session
 * @returns The created Janus Gateway-Client session if successful, NULL otherwise */
janus_session *janus_session_create(void);
/*! \brief Method to find an existing Janus Gateway-Client session from its ID
 * @param[in] session_id The Janus Gateway-Client session ID
 * @returns The created Janus Gateway-Client session if successful, NULL otherwise */
janus_session *janus_session_find(guint64 session_id);
/*! \brief Method to destroy a Janus Gateway-Client session
 * @param[in] session_id The Janus Gateway-Client session ID to destroy
 * @returns 0 in case of success, a negative integer otherwise */
gint janus_session_destroy(guint64 session_id);
/*! \brief Method to actually free the resources allocated by a Janus Gateway-Client session
 * @param[in] session The Janus Gateway-Client session instance to free */
void janus_session_free(janus_session *session);
///@}


/** @name Janus web server
 * \details Browsers make use of HTTP to make requests to the gateway.
 * Since the gateway may be deployed on a different domain than the web
 * server hosting the web applications using it, the gateway automatically
 * handles OPTIONS request to comply with the CORS specification.
 * POST requests can be used to ask for the management of a session with
 * the gateway, to attach to a plugin, to send messages to the plugin
 * itself and so on. GET requests instead are used for getting events
 * associated to a gateway session (and as such to all its plugin handles
 * and the events plugins push in the session itself), using a long poll
 * approach. A JavaScript library (janus.js) implements all of this on
 * the client side automatically.
 */
///@{
/*! \brief Callback (libmicrohttpd) invoked when an HTTP message (GET, POST, OPTIONS, etc.) is available */
int janus_ws_handler(void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **ptr);
/*! \brief Callback (libmicrohttpd) invoked when headers of an incoming HTTP message have been parsed */
int janus_ws_headers(void *cls, enum MHD_ValueKind kind, const char *key, const char *value);
/*! \brief Callback (libmicrohttpd) invoked when a request has been processed and can be freed */
void janus_ws_request_completed (void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe);
/*! \brief Method to return a successful Janus response message (JSON) to the browser
 * @param[in] connection The libmicrohttpd MHD_Connection connection instance that is handling the request
 * @param[in] msg The original request
 * @param[in] transaction The Janus transaction identifier
 * @param[in] payload The stringified version of the Janus response (JSON) 
 * @returns MHD_YES on success, MHD_NO otherwise */
int janus_ws_success(struct MHD_Connection *connection, janus_http_msg *msg, const char *transaction, char *payload);
/*! \brief Method to return an error Janus response message (JSON) to the browser
 * @param[in] connection The libmicrohttpd MHD_Connection connection instance that is handling the request
 * @param[in] msg The original request
 * @param[in] transaction The Janus transaction identifier
 * @param[in] error The error code as defined in apierror.h
 * @param[in] format The printf format of the reason string, followed by a variable
 * number of arguments, if needed; if format is NULL, a pre-configured string
 * associated with the error code is used
 * @returns MHD_YES on success, MHD_NO otherwise */
int janus_ws_error(struct MHD_Connection *connection, janus_http_msg *msg, const char *transaction, gint error, const char *format, ...);
/*! \brief Worker to handle requests that are actually long polls
 * \details As this method handles a long poll, it doesn't return until an
 * event (e.g., pushed by a plugin) is available, or a timeout (30 seconds)
 * has been fired. In case of a timeout, a keep-alive Janus response (JSON)
 * is sent to tell the browser that the session is still valid.
 * @param[in] connection The libmicrohttpd MHD_Connection connection instance that is handling the request
 * @param[in] msg The original request, which also manages the request state
 * @returns MHD_YES on success, MHD_NO otherwise */
int janus_ws_notifier(struct MHD_Connection *connection, janus_http_msg *msg);
///@}


/** @name Janus plugin management
 * As anticipated, the gateway doesn't provide any specific feature: it takes
 * care of WebRTC-related stuff, and of sending and receiving JSON-based
 * messages. To implement applications based on these foundations, plugins
 * can be used. These plugins are shared objects that need to implement
 * the interfaces defined in plugin.h and as such are dynamically loaded
 * by the gateway at startup, and unloaded when the gateway closes.
 */
///@{
/*! \brief Callback (g_hash_table_foreach) invoked when it's time to destroy a plugin instance
 * @param[in] key Key of the plugins hash table (package name)
 * @param[in] value The janus_plugin plugin instance to destroy
 * @param[in] user_data User provided data (unused) */
void janus_plugin_close(void *key, void *value, void *user_data);
/*! \brief Callback (g_hash_table_foreach) invoked when it's time to close a plugin
 * @param[in] key Key of the plugins hash table (package name)
 * @param[in] value The janus_plugin plugin instance to close
 * @param[in] user_data User provided data (unused) */
void janus_pluginso_close(void *key, void *value, void *user_data);
/*! \brief Method to return a registered plugin instance out of its package name
 * @param[in] package The unique package name of the plugin
 * @returns The plugin instance */
janus_plugin *janus_plugin_find(const gchar *package);
///@}

/*! \brief Helper method to return the path to the provided server certificate */
gchar *janus_get_server_pem(void);
/*! \brief Helper method to return the path to the provided server certificate key */
gchar *janus_get_server_key(void);


/*! \brief Helper method to return the local IP address (autodetected by default) */
gchar *janus_get_local_ip(void);
/*! \brief Helper method to return the IP address to use in the SDP (autodetected by default) */
gchar *janus_get_public_ip(void);
/*! \brief Helper method to overwrite the IP address to use in the SDP */
void janus_set_public_ip(const char *ip);
/*! \brief Helper method to check whether the gateway is being shut down */
gint janus_is_stopping(void);


#endif
