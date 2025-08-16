/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2025, skravchenko@stssrv.com
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief ElevenLabs ConvAI dialplan application
 *
 * \author skravchenko@stssrv.com
 *
 * \ingroup applications
 */

#include "asterisk.h"

#include "asterisk/app.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/logger.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/frame.h"
#include "asterisk/framehook.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/pbx.h"

#include <curl/curl.h>
#include <libwebsockets.h>
#include <cjson/cJSON.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

/*** DOCUMENTATION
	<application name="ElevenLabsConvAI" language="en_US">
		<synopsis>
			Connect channel to ElevenLabs ConvAI agent for real-time conversation
		</synopsis>
		<syntax>
			<parameter name="agent_config" required="true">
				<para>The agent configuration section name from elevenlabs.conf</para>
			</parameter>
		</syntax>
		<description>
			<para>This application connects a channel to an ElevenLabs ConvAI agent
			for real-time bidirectional audio streaming. The application handles
			WebSocket communication, audio buffering, and precise playback timing.</para>
			<para>Configuration is loaded from elevenlabs.conf with agent-specific
			settings including API keys, endpoints, and buffer parameters.</para>
		</description>
	</application>
 ***/

static const char app[] = "ElevenLabsConvAI";

/*! \brief Global debug state */
static int elevenlabs_debug = 0;

/*! \brief Configuration file name */
static const char config_file[] = "elevenlabs.conf";

/*! \brief Agent configuration structure */
struct agent_config {
	char name[64];
	char api_key[256];
	char endpoint_url[512];
	char agent_id[64];
	int send_caller_number;
	int buffer_duration_ms;
};

/*! \brief Audio buffer structure */
struct audio_buffer {
	unsigned char *data;
	size_t size;
	size_t capacity;
	ast_mutex_t lock;
};

/*! \brief Audio queue item */
struct audio_queue_item {
	unsigned char *audio_data;
	size_t data_length;
	int event_id;
	struct audio_queue_item *next;
};

/*! \brief Audio playback queue */
struct audio_queue {
	struct audio_queue_item *head;
	struct audio_queue_item *tail;
	int count;
	ast_mutex_t lock;
	ast_cond_t cond;
};

/*! \brief HTTP response data */
struct http_response {
	char *data;
	size_t size;
};

/*! \brief ElevenLabs session data */
struct elevenlabs_session {
	struct ast_channel *channel;
	struct agent_config *config;
	
	/* HTTP client */
	CURL *curl;
	char *signed_url;
	
	/* WebSocket client */
	struct lws_context *ws_context;
	struct lws *ws_connection;
	char ws_host[256];
	char ws_path[512];
	int ws_port;
	int ws_connected;
	
	/* Audio processing */
	struct audio_buffer *send_buffer;
	struct audio_queue *receive_queue;
	
	/* Threading */
	pthread_t playback_thread;
	int playback_running;
	int session_active;
	
	/* Framehook */
	int framehook_id;
	
	/* Synchronization */
	ast_mutex_t session_lock;
};

/*! \brief Global agent configurations */
static struct agent_config *agent_configs = NULL;
static int agent_count = 0;
static ast_mutex_t config_lock = AST_MUTEX_INIT_VALUE;

/*! \brief Forward declarations */
static int elevenlabs_exec(struct ast_channel *chan, const char *data);
static int load_config(void);
static void cleanup_session(struct elevenlabs_session *session);
static struct agent_config *find_agent_config(const char *name);

/* Base64 encoding/decoding */
static char *base64_encode(const unsigned char *data, size_t input_length);
static unsigned char *base64_decode(const char *data, size_t *output_length);

/* Audio buffer management */
static struct audio_buffer *create_audio_buffer(size_t capacity);
static void destroy_audio_buffer(struct audio_buffer *buffer);
static int audio_buffer_append(struct audio_buffer *buffer, const unsigned char *data, size_t length);
static void audio_buffer_clear(struct audio_buffer *buffer);

/* Audio queue management */
static struct audio_queue *create_audio_queue(void);
static void destroy_audio_queue(struct audio_queue *queue);
static int audio_queue_add(struct audio_queue *queue, const unsigned char *data, size_t length, int event_id);
static struct audio_queue_item *audio_queue_get(struct audio_queue *queue);
static void audio_queue_clear_before_event(struct audio_queue *queue, int event_id);

/* WebSocket functions */
static int parse_signed_url(const char *signed_url, char *host, char *path, int *port);
static int init_websocket(struct elevenlabs_session *session);
static void cleanup_websocket(struct elevenlabs_session *session);
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static int send_websocket_message(struct elevenlabs_session *session, const char *message);

/* Audio processing */
static void *playback_thread_func(void *arg);
static int framehook_callback(struct ast_channel *chan, struct ast_frame *frame, enum ast_framehook_event event, void *data);
static int attach_framehook(struct elevenlabs_session *session);
static void detach_framehook(struct elevenlabs_session *session);

/* Protocol handlers */
static void handle_websocket_message(struct elevenlabs_session *session, const char *message);
static void handle_ping_message(struct elevenlabs_session *session, cJSON *json);
static void handle_audio_message(struct elevenlabs_session *session, cJSON *json);
static void handle_interruption_message(struct elevenlabs_session *session, cJSON *json);
static void send_conversation_initiation(struct elevenlabs_session *session);

/*! \brief HTTP response callback */
static size_t http_response_callback(void *contents, size_t size, size_t nmemb, struct http_response *response)
{
	size_t total_size = size * nmemb;
	char *ptr = ast_realloc(response->data, response->size + total_size + 1);
	
	if (!ptr) {
		ast_log(LOG_ERROR, "Failed to allocate memory for HTTP response\n");
		return 0;
	}
	
	response->data = ptr;
	memcpy(&(response->data[response->size]), contents, total_size);
	response->size += total_size;
	response->data[response->size] = '\0';
	
	return total_size;
}

/*! \brief Get signed WebSocket URL from ElevenLabs API */
static char *get_signed_url(struct agent_config *config)
{
	CURL *curl;
	CURLcode res;
	struct http_response response = {0};
	char *signed_url = NULL;
	char url[1024];
	struct curl_slist *headers = NULL;
	char auth_header[512];
	
	curl = curl_easy_init();
	if (!curl) {
		ast_log(LOG_ERROR, "Failed to initialize CURL\n");
		return NULL;
	}
	
	snprintf(url, sizeof(url), "%s?agent_id=%s", config->endpoint_url, config->agent_id);
	snprintf(auth_header, sizeof(auth_header), "xi-api-key: %s", config->api_key);
	
	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "Content-Type: application/json");
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_response_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	
	res = curl_easy_perform(curl);
	
	if (res == CURLE_OK) {
		long response_code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		
		if (response_code == 200 && response.data) {
			cJSON *json = cJSON_Parse(response.data);
			if (json) {
				cJSON *signed_url_item = cJSON_GetObjectItem(json, "signed_url");
				if (signed_url_item && cJSON_IsString(signed_url_item)) {
					signed_url = ast_strdup(signed_url_item->valuestring);
					if (elevenlabs_debug) {
						ast_log(LOG_DEBUG, "Got signed URL: %s\n", signed_url);
					}
				}
				cJSON_Delete(json);
			}
		} else {
			ast_log(LOG_ERROR, "HTTP request failed with code %ld\n", response_code);
		}
	} else {
		ast_log(LOG_ERROR, "CURL request failed: %s\n", curl_easy_strerror(res));
	}
	
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (response.data) {
		ast_free(response.data);
	}
	
	return signed_url;
}

/*! \brief Find agent configuration by name */
static struct agent_config *find_agent_config(const char *name)
{
	int i;
	
	ast_mutex_lock(&config_lock);
	for (i = 0; i < agent_count; i++) {
		if (strcmp(agent_configs[i].name, name) == 0) {
			ast_mutex_unlock(&config_lock);
			return &agent_configs[i];
		}
	}
	ast_mutex_unlock(&config_lock);
	
	return NULL;
}

/*! \brief Load configuration from elevenlabs.conf */
static int load_config(void)
{
	struct ast_config *cfg;
	struct ast_variable *var;
	struct ast_flags config_flags = { 0 };
	char *category = NULL;
	int count = 0;
	int i;
	
	cfg = ast_config_load(config_file, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Unable to load config file %s\n", config_file);
		return -1;
	}
	
	/* Count non-general sections */
	while ((category = ast_category_browse(cfg, category))) {
		if (strcmp(category, "general") != 0) {
			count++;
		}
	}
	
	if (count == 0) {
		ast_log(LOG_WARNING, "No agent configurations found in %s\n", config_file);
		ast_config_destroy(cfg);
		return -1;
	}
	
	ast_mutex_lock(&config_lock);
	
	/* Free existing configurations */
	if (agent_configs) {
		ast_free(agent_configs);
	}
	
	agent_configs = ast_calloc(count, sizeof(struct agent_config));
	if (!agent_configs) {
		ast_log(LOG_ERROR, "Failed to allocate memory for agent configurations\n");
		agent_count = 0;
		ast_mutex_unlock(&config_lock);
		ast_config_destroy(cfg);
		return -1;
	}
	
	agent_count = count;
	i = 0;
	category = NULL;
	
	/* Parse general section first */
	var = ast_variable_browse(cfg, "general");
	while (var) {
		if (strcmp(var->name, "debug") == 0) {
			elevenlabs_debug = ast_true(var->value);
		}
		var = var->next;
	}
	
	/* Parse agent configurations */
	while ((category = ast_category_browse(cfg, category))) {
		if (strcmp(category, "general") == 0) {
			continue;
		}
		
		if (i >= count) {
			break;
		}
		
		ast_copy_string(agent_configs[i].name, category, sizeof(agent_configs[i].name));
		
		/* Set defaults */
		agent_configs[i].send_caller_number = 0;
		agent_configs[i].buffer_duration_ms = 240;
		
		var = ast_variable_browse(cfg, category);
		while (var) {
			if (strcmp(var->name, "elevenlabs_api_key") == 0) {
				ast_copy_string(agent_configs[i].api_key, var->value, sizeof(agent_configs[i].api_key));
			} else if (strcmp(var->name, "elevenlabs_get_signed_url_endpoint") == 0) {
				ast_copy_string(agent_configs[i].endpoint_url, var->value, sizeof(agent_configs[i].endpoint_url));
			} else if (strcmp(var->name, "elevenlabs_agent_id") == 0) {
				ast_copy_string(agent_configs[i].agent_id, var->value, sizeof(agent_configs[i].agent_id));
			} else if (strcmp(var->name, "elevenlabs_send_caller_number") == 0) {
				agent_configs[i].send_caller_number = ast_true(var->value);
			} else if (strcmp(var->name, "elevenlabs_audio_frames_buffer_ms") == 0) {
				agent_configs[i].buffer_duration_ms = atoi(var->value);
			}
			var = var->next;
		}
		
		/* Validate required parameters */
		if (ast_strlen_zero(agent_configs[i].api_key) ||
			ast_strlen_zero(agent_configs[i].endpoint_url) ||
			ast_strlen_zero(agent_configs[i].agent_id)) {
			ast_log(LOG_ERROR, "Agent '%s' missing required parameters\n", agent_configs[i].name);
		}
		
		if (elevenlabs_debug) {
			ast_log(LOG_DEBUG, "Loaded agent config '%s': endpoint=%s, agent_id=%s, buffer=%dms\n",
				agent_configs[i].name, agent_configs[i].endpoint_url, 
				agent_configs[i].agent_id, agent_configs[i].buffer_duration_ms);
		}
		
		i++;
	}
	
	ast_mutex_unlock(&config_lock);
	ast_config_destroy(cfg);
	
	ast_log(LOG_NOTICE, "Loaded %d agent configurations\n", agent_count);
	return 0;
}

/*! \brief Base64 encode data */
static char *base64_encode(const unsigned char *data, size_t input_length)
{
	BIO *bio, *b64;
	BUF_MEM *buffer_ptr;
	char *encoded_data;
	
	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new(BIO_s_mem());
	bio = BIO_push(b64, bio);
	
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	BIO_write(bio, data, input_length);
	BIO_flush(bio);
	BIO_get_mem_ptr(bio, &buffer_ptr);
	
	encoded_data = ast_calloc(1, buffer_ptr->length + 1);
	if (encoded_data) {
		memcpy(encoded_data, buffer_ptr->data, buffer_ptr->length);
		encoded_data[buffer_ptr->length] = '\0';
	}
	
	BIO_free_all(bio);
	return encoded_data;
}

/*! \brief Base64 decode data */
static unsigned char *base64_decode(const char *data, size_t *output_length)
{
	BIO *bio, *b64;
	unsigned char *decoded_data;
	size_t input_length = strlen(data);
	size_t decoded_length;
	
	decoded_data = ast_calloc(1, input_length);
	if (!decoded_data) {
		return NULL;
	}
	
	bio = BIO_new_mem_buf(data, input_length);
	b64 = BIO_new(BIO_f_base64());
	bio = BIO_push(b64, bio);
	
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	decoded_length = BIO_read(bio, decoded_data, input_length);
	
	if (output_length) {
		*output_length = decoded_length;
	}
	
	BIO_free_all(bio);
	return decoded_data;
}

/*! \brief Create audio buffer */
static struct audio_buffer *create_audio_buffer(size_t capacity)
{
	struct audio_buffer *buffer = ast_calloc(1, sizeof(struct audio_buffer));
	if (!buffer) {
		return NULL;
	}
	
	buffer->data = ast_calloc(1, capacity);
	if (!buffer->data) {
		ast_free(buffer);
		return NULL;
	}
	
	buffer->capacity = capacity;
	buffer->size = 0;
	ast_mutex_init(&buffer->lock);
	
	return buffer;
}

/*! \brief Destroy audio buffer */
static void destroy_audio_buffer(struct audio_buffer *buffer)
{
	if (!buffer) {
		return;
	}
	
	ast_mutex_lock(&buffer->lock);
	if (buffer->data) {
		ast_free(buffer->data);
	}
	ast_mutex_unlock(&buffer->lock);
	ast_mutex_destroy(&buffer->lock);
	ast_free(buffer);
}

/*! \brief Append data to audio buffer */
static int audio_buffer_append(struct audio_buffer *buffer, const unsigned char *data, size_t length)
{
	if (!buffer || !data || length == 0) {
		return -1;
	}
	
	ast_mutex_lock(&buffer->lock);
	
	if (buffer->size + length > buffer->capacity) {
		/* Resize buffer if needed */
		size_t new_capacity = buffer->capacity * 2;
		while (new_capacity < buffer->size + length) {
			new_capacity *= 2;
		}
		
		unsigned char *new_data = ast_realloc(buffer->data, new_capacity);
		if (!new_data) {
			ast_mutex_unlock(&buffer->lock);
			return -1;
		}
		
		buffer->data = new_data;
		buffer->capacity = new_capacity;
	}
	
	memcpy(buffer->data + buffer->size, data, length);
	buffer->size += length;
	
	ast_mutex_unlock(&buffer->lock);
	return 0;
}

/*! \brief Clear audio buffer */
static void audio_buffer_clear(struct audio_buffer *buffer)
{
	if (!buffer) {
		return;
	}
	
	ast_mutex_lock(&buffer->lock);
	buffer->size = 0;
	ast_mutex_unlock(&buffer->lock);
}

/*! \brief Create audio queue */
static struct audio_queue *create_audio_queue(void)
{
	struct audio_queue *queue = ast_calloc(1, sizeof(struct audio_queue));
	if (!queue) {
		return NULL;
	}
	
	queue->head = NULL;
	queue->tail = NULL;
	queue->count = 0;
	ast_mutex_init(&queue->lock);
	ast_cond_init(&queue->cond, NULL);
	
	return queue;
}

/*! \brief Destroy audio queue */
static void destroy_audio_queue(struct audio_queue *queue)
{
	struct audio_queue_item *item, *next;
	
	if (!queue) {
		return;
	}
	
	ast_mutex_lock(&queue->lock);
	
	item = queue->head;
	while (item) {
		next = item->next;
		if (item->audio_data) {
			ast_free(item->audio_data);
		}
		ast_free(item);
		item = next;
	}
	
	ast_mutex_unlock(&queue->lock);
	ast_mutex_destroy(&queue->lock);
	ast_cond_destroy(&queue->cond);
	ast_free(queue);
}

/*! \brief Add item to audio queue */
static int audio_queue_add(struct audio_queue *queue, const unsigned char *data, size_t length, int event_id)
{
	struct audio_queue_item *item;
	
	if (!queue || !data || length == 0) {
		return -1;
	}
	
	item = ast_calloc(1, sizeof(struct audio_queue_item));
	if (!item) {
		return -1;
	}
	
	item->audio_data = ast_calloc(1, length);
	if (!item->audio_data) {
		ast_free(item);
		return -1;
	}
	
	memcpy(item->audio_data, data, length);
	item->data_length = length;
	item->event_id = event_id;
	item->next = NULL;
	
	ast_mutex_lock(&queue->lock);
	
	if (queue->tail) {
		queue->tail->next = item;
		queue->tail = item;
	} else {
		queue->head = queue->tail = item;
	}
	
	queue->count++;
	ast_cond_signal(&queue->cond);
	
	ast_mutex_unlock(&queue->lock);
	return 0;
}

/*! \brief Get item from audio queue */
static struct audio_queue_item *audio_queue_get(struct audio_queue *queue)
{
	struct audio_queue_item *item;
	
	if (!queue) {
		return NULL;
	}
	
	ast_mutex_lock(&queue->lock);
	
	item = queue->head;
	if (item) {
		queue->head = item->next;
		if (!queue->head) {
			queue->tail = NULL;
		}
		queue->count--;
	}
	
	ast_mutex_unlock(&queue->lock);
	return item;
}

/*! \brief Clear audio queue items before specific event ID */
static void audio_queue_clear_before_event(struct audio_queue *queue, int event_id)
{
	struct audio_queue_item *item, *next;
	struct audio_queue_item *new_head = NULL;
	
	if (!queue) {
		return;
	}
	
	ast_mutex_lock(&queue->lock);
	
	item = queue->head;
	while (item) {
		next = item->next;
		
		if (item->event_id < event_id) {
			/* Remove this item */
			if (item->audio_data) {
				ast_free(item->audio_data);
			}
			ast_free(item);
			queue->count--;
		} else {
			/* Keep this item and all following */
			new_head = item;
			break;
		}
		
		item = next;
	}
	
	queue->head = new_head;
	if (!new_head) {
		queue->tail = NULL;
	}
	
	ast_mutex_unlock(&queue->lock);
}

/*! \brief Parse WebSocket signed URL */
static int parse_signed_url(const char *signed_url, char *host, char *path, int *port)
{
	const char *protocol_end, *host_start, *host_end, *port_start, *path_start;
	char port_str[16];
	
	if (!signed_url || !host || !path || !port) {
		return -1;
	}
	
	/* Find protocol (wss://) */
	protocol_end = strstr(signed_url, "://");
	if (!protocol_end) {
		return -1;
	}
	
	host_start = protocol_end + 3;
	
	/* Find end of host (either : for port or / for path) */
	host_end = strchr(host_start, ':');
	if (!host_end) {
		host_end = strchr(host_start, '/');
	}
	
	if (!host_end) {
		return -1;
	}
	
	/* Extract host */
	size_t host_len = host_end - host_start;
	if (host_len >= 255) {
		return -1;
	}
	memcpy(host, host_start, host_len);
	host[host_len] = '\0';
	
	/* Check for port */
	if (*host_end == ':') {
		port_start = host_end + 1;
		path_start = strchr(port_start, '/');
		if (!path_start) {
			return -1;
		}
		
		size_t port_len = path_start - port_start;
		if (port_len >= sizeof(port_str)) {
			return -1;
		}
		memcpy(port_str, port_start, port_len);
		port_str[port_len] = '\0';
		*port = atoi(port_str);
	} else {
		*port = 443; /* Default HTTPS port */
		path_start = host_end;
	}
	
	/* Extract path */
	strcpy(path, path_start);
	
	return 0;
}

/*! \brief Initialize WebSocket connection */
static int init_websocket(struct elevenlabs_session *session)
{
	struct lws_context_creation_info info;
	struct lws_client_connect_info connect_info;
	struct lws_protocols protocols[] = {
		{
			"elevenlabs-convai",
			websocket_callback,
			sizeof(void *),
			4096,
			0, session, 0
		},
		{ NULL, NULL, 0, 0 }
	};
	
	if (!session || !session->signed_url) {
		return -1;
	}
	
	/* Parse signed URL */
	if (parse_signed_url(session->signed_url, session->ws_host, 
			session->ws_path, &session->ws_port) != 0) {
		ast_log(LOG_ERROR, "Failed to parse signed URL: %s\n", session->signed_url);
		return -1;
	}
	
	/* Initialize WebSocket context */
	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	
	session->ws_context = lws_create_context(&info);
	if (!session->ws_context) {
		ast_log(LOG_ERROR, "Failed to create WebSocket context\n");
		return -1;
	}
	
	/* Connect to WebSocket */
	memset(&connect_info, 0, sizeof(connect_info));
	connect_info.context = session->ws_context;
	connect_info.address = session->ws_host;
	connect_info.port = session->ws_port;
	connect_info.path = session->ws_path;
	connect_info.host = session->ws_host;
	connect_info.origin = session->ws_host;
	connect_info.protocol = "elevenlabs-convai";
	connect_info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	connect_info.userdata = session;
	
	session->ws_connection = lws_client_connect_via_info(&connect_info);
	if (!session->ws_connection) {
		ast_log(LOG_ERROR, "Failed to connect to WebSocket\n");
		lws_context_destroy(session->ws_context);
		session->ws_context = NULL;
		return -1;
	}
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "WebSocket connection initiated to %s:%d%s\n", 
			session->ws_host, session->ws_port, session->ws_path);
	}
	
	return 0;
}

/*! \brief Cleanup WebSocket connection */
static void cleanup_websocket(struct elevenlabs_session *session)
{
	if (!session) {
		return;
	}
	
	if (session->ws_connection) {
		lws_close_reason(session->ws_connection, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
		session->ws_connection = NULL;
	}
	
	if (session->ws_context) {
		lws_context_destroy(session->ws_context);
		session->ws_context = NULL;
	}
	
	session->ws_connected = 0;
}

/*! \brief WebSocket callback handler */
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	struct elevenlabs_session *session = (struct elevenlabs_session *)lws_context_user(lws_get_context(wsi));
	
	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		session->ws_connected = 1;
		if (elevenlabs_debug) {
			ast_log(LOG_DEBUG, "WebSocket connection established\n");
		}
		/* Send conversation initiation if enabled */
		send_conversation_initiation(session);
		break;
		
	case LWS_CALLBACK_CLIENT_RECEIVE:
		if (in && len > 0) {
			char *message = ast_calloc(1, len + 1);
			if (message) {
				memcpy(message, in, len);
				message[len] = '\0';
				handle_websocket_message(session, message);
				ast_free(message);
			}
		}
		break;
		
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		ast_log(LOG_ERROR, "WebSocket connection error\n");
		session->ws_connected = 0;
		break;
		
	case LWS_CALLBACK_CLOSED:
		if (elevenlabs_debug) {
			ast_log(LOG_DEBUG, "WebSocket connection closed\n");
		}
		session->ws_connected = 0;
		break;
		
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		/* Handle outgoing data if needed */
		break;
		
	default:
		break;
	}
	
	return 0;
}

/*! \brief Send WebSocket message */
static int send_websocket_message(struct elevenlabs_session *session, const char *message)
{
	size_t len;
	unsigned char *buf;
	
	if (!session || !session->ws_connection || !session->ws_connected || !message) {
		return -1;
	}
	
	len = strlen(message);
	buf = ast_calloc(1, LWS_PRE + len + 1);
	if (!buf) {
		return -1;
	}
	
	memcpy(buf + LWS_PRE, message, len);
	
	if (lws_write(session->ws_connection, buf + LWS_PRE, len, LWS_WRITE_TEXT) < 0) {
		ast_log(LOG_ERROR, "Failed to send WebSocket message\n");
		ast_free(buf);
		return -1;
	}
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Sent WebSocket message: %s\n", message);
	}
	
	ast_free(buf);
	return 0;
}

/*! \brief Handle incoming WebSocket message */
static void handle_websocket_message(struct elevenlabs_session *session, const char *message)
{
	cJSON *json;
	cJSON *type_item;
	
	if (!session || !message) {
		return;
	}
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Received WebSocket message: %s\n", message);
	}
	
	json = cJSON_Parse(message);
	if (!json) {
		ast_log(LOG_WARNING, "Failed to parse JSON message\n");
		return;
	}
	
	type_item = cJSON_GetObjectItem(json, "type");
	if (!type_item || !cJSON_IsString(type_item)) {
		ast_log(LOG_WARNING, "Message missing 'type' field\n");
		cJSON_Delete(json);
		return;
	}
	
	if (strcmp(type_item->valuestring, "ping") == 0) {
		handle_ping_message(session, json);
	} else if (strcmp(type_item->valuestring, "audio") == 0) {
		handle_audio_message(session, json);
	} else if (strcmp(type_item->valuestring, "interruption") == 0) {
		handle_interruption_message(session, json);
	} else {
		if (elevenlabs_debug) {
			ast_log(LOG_DEBUG, "Unknown message type: %s\n", type_item->valuestring);
		}
	}
	
	cJSON_Delete(json);
}

/*! \brief Handle ping message */
static void handle_ping_message(struct elevenlabs_session *session, cJSON *json)
{
	cJSON *ping_event, *event_id_item, *ping_ms_item;
	cJSON *pong_json, *pong_event_json;
	char *pong_message;
	
	ping_event = cJSON_GetObjectItem(json, "ping_event");
	if (!ping_event) {
		return;
	}
	
	event_id_item = cJSON_GetObjectItem(ping_event, "event_id");
	ping_ms_item = cJSON_GetObjectItem(ping_event, "ping_ms");
	
	if (!event_id_item || !cJSON_IsNumber(event_id_item)) {
		return;
	}
	
	/* Create pong response */
	pong_json = cJSON_CreateObject();
	cJSON_AddStringToObject(pong_json, "type", "pong");
	cJSON_AddNumberToObject(pong_json, "event_id", event_id_item->valueint);
	
	pong_message = cJSON_Print(pong_json);
	if (pong_message) {
		send_websocket_message(session, pong_message);
		ast_free(pong_message);
	}
	
	cJSON_Delete(pong_json);
	
	if (elevenlabs_debug && ping_ms_item && cJSON_IsNumber(ping_ms_item)) {
		ast_log(LOG_DEBUG, "Handled ping (event_id: %d, ping_ms: %d)\n", 
			event_id_item->valueint, ping_ms_item->valueint);
	}
}

/*! \brief Handle audio message */
static void handle_audio_message(struct elevenlabs_session *session, cJSON *json)
{
	cJSON *audio_event, *audio_base64_item, *event_id_item;
	unsigned char *audio_data;
	size_t audio_length;
	
	audio_event = cJSON_GetObjectItem(json, "audio_event");
	if (!audio_event) {
		return;
	}
	
	audio_base64_item = cJSON_GetObjectItem(audio_event, "audio_base_64");
	event_id_item = cJSON_GetObjectItem(audio_event, "event_id");
	
	if (!audio_base64_item || !cJSON_IsString(audio_base64_item) ||
		!event_id_item || !cJSON_IsNumber(event_id_item)) {
		return;
	}
	
	/* Decode base64 audio */
	audio_data = base64_decode(audio_base64_item->valuestring, &audio_length);
	if (!audio_data) {
		ast_log(LOG_ERROR, "Failed to decode audio data\n");
		return;
	}
	
	/* Add to receive queue */
	if (audio_queue_add(session->receive_queue, audio_data, audio_length, event_id_item->valueint) != 0) {
		ast_log(LOG_ERROR, "Failed to add audio to receive queue\n");
	}
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Received audio (event_id: %d, length: %zu)\n", 
			event_id_item->valueint, audio_length);
	}
	
	ast_free(audio_data);
}

/*! \brief Handle interruption message */
static void handle_interruption_message(struct elevenlabs_session *session, cJSON *json)
{
	cJSON *interruption_event, *event_id_item;
	
	interruption_event = cJSON_GetObjectItem(json, "interruption_event");
	if (!interruption_event) {
		return;
	}
	
	event_id_item = cJSON_GetObjectItem(interruption_event, "event_id");
	if (!event_id_item || !cJSON_IsNumber(event_id_item)) {
		return;
	}
	
	/* Clear audio queue before this event ID */
	audio_queue_clear_before_event(session->receive_queue, event_id_item->valueint);
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Handled interruption (event_id: %d)\n", event_id_item->valueint);
	}
}

/*! \brief Send conversation initiation message */
static void send_conversation_initiation(struct elevenlabs_session *session)
{
	cJSON *json, *dynamic_vars;
	char *message;
	const char *caller_id;
	
	if (!session || !session->config->send_caller_number) {
		return;
	}
	
	caller_id = ast_channel_caller(session->channel)->id.number.str;
	if (ast_strlen_zero(caller_id)) {
		return;
	}
	
	json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "type", "conversation_initiation_client_data");
	
	dynamic_vars = cJSON_CreateObject();
	cJSON_AddStringToObject(dynamic_vars, "caller_id", caller_id);
	cJSON_AddItemToObject(json, "dynamic_variables", dynamic_vars);
	
	message = cJSON_Print(json);
	if (message) {
		send_websocket_message(session, message);
		ast_free(message);
	}
	
	cJSON_Delete(json);
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Sent conversation initiation with caller_id: %s\n", caller_id);
	}
}

/*! \brief Audio playback thread */
static void *playback_thread_func(void *arg)
{
	struct elevenlabs_session *session = (struct elevenlabs_session *)arg;
	struct audio_queue_item *item;
	struct ast_frame *frame;
	struct timespec sleep_time;
	size_t frame_size = 320; /* 20ms at 8kHz, 16-bit = 320 bytes */
	unsigned char *audio_ptr;
	size_t remaining;
	
	sleep_time.tv_sec = 0;
	sleep_time.tv_nsec = 20000000; /* 20ms */
	
	while (session->playback_running && session->session_active) {
		item = audio_queue_get(session->receive_queue);
		if (!item) {
			usleep(10000); /* 10ms */
			continue;
		}
		
		/* Split audio data into 20ms frames and play them */
		audio_ptr = item->audio_data;
		remaining = item->data_length;
		
		while (remaining > 0 && session->playback_running) {
			size_t chunk_size = (remaining < frame_size) ? remaining : frame_size;
			
			/* Create audio frame */
			frame = ast_calloc(1, sizeof(struct ast_frame) + chunk_size);
			if (!frame) {
				break;
			}
			
			frame->frametype = AST_FRAME_VOICE;
			frame->subclass.format = ast_format_slin;
			frame->data.ptr = frame + 1;
			frame->datalen = chunk_size;
			frame->samples = chunk_size / 2; /* 16-bit samples */
			
			memcpy(frame->data.ptr, audio_ptr, chunk_size);
			
			/* Write frame to channel */
			if (ast_write(session->channel, frame) < 0) {
				ast_log(LOG_WARNING, "Failed to write audio frame\n");
			}
			
			ast_free(frame);
			
			audio_ptr += chunk_size;
			remaining -= chunk_size;
			
			/* Sleep for 20ms */
			nanosleep(&sleep_time, NULL);
		}
		
		/* Cleanup item */
		if (item->audio_data) {
			ast_free(item->audio_data);
		}
		ast_free(item);
	}
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Playback thread exiting\n");
	}
	
	return NULL;
}

/*! \brief Framehook callback for audio capture */
static int framehook_callback(struct ast_channel *chan, struct ast_frame *frame, enum ast_framehook_event event, void *data)
{
	struct elevenlabs_session *session = (struct elevenlabs_session *)data;
	char *encoded_audio;
	cJSON *json;
	char *message;
	size_t buffer_threshold;
	
	if (!session || !session->session_active || !session->ws_connected) {
		return 0;
	}
	
	/* Only process voice frames from read direction (incoming audio) */
	if (frame->frametype != AST_FRAME_VOICE || event != AST_FRAMEHOOK_EVENT_READ) {
		return 0;
	}
	
	/* Add audio data to send buffer */
	if (audio_buffer_append(session->send_buffer, frame->data.ptr, frame->datalen) != 0) {
		return 0;
	}
	
	/* Check if buffer has enough data to send */
	buffer_threshold = (session->config->buffer_duration_ms * 8000 * 2) / 1000; /* 8kHz, 16-bit */
	
	ast_mutex_lock(&session->send_buffer->lock);
	if (session->send_buffer->size >= buffer_threshold) {
		/* Encode and send buffer */
		encoded_audio = base64_encode(session->send_buffer->data, session->send_buffer->size);
		if (encoded_audio) {
			/* Create JSON message */
			json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "user_audio_chunk", encoded_audio);
			
			message = cJSON_Print(json);
			if (message) {
				send_websocket_message(session, message);
				ast_free(message);
			}
			
			cJSON_Delete(json);
			ast_free(encoded_audio);
		}
		
		/* Clear buffer */
		session->send_buffer->size = 0;
	}
	ast_mutex_unlock(&session->send_buffer->lock);
	
	return 0;
}

/*! \brief Attach framehook to channel */
static int attach_framehook(struct elevenlabs_session *session)
{
	struct ast_framehook_interface interface = {
		.version = AST_FRAMEHOOK_INTERFACE_VERSION,
		.event_cb = framehook_callback,
		.destroy_cb = NULL,
		.data = session,
	};
	
	session->framehook_id = ast_framehook_attach(session->channel, &interface);
	if (session->framehook_id < 0) {
		ast_log(LOG_ERROR, "Failed to attach framehook\n");
		return -1;
	}
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Framehook attached (ID: %d)\n", session->framehook_id);
	}
	
	return 0;
}

/*! \brief Detach framehook from channel */
static void detach_framehook(struct elevenlabs_session *session)
{
	if (!session || session->framehook_id < 0) {
		return;
	}
	
	ast_framehook_detach(session->channel, session->framehook_id);
	session->framehook_id = -1;
	
	if (elevenlabs_debug) {
		ast_log(LOG_DEBUG, "Framehook detached\n");
	}
}

/*! \brief Main application function */
static int elevenlabs_exec(struct ast_channel *chan, const char *data)
{
	char *agent_name;
	struct agent_config *config;
	struct elevenlabs_session *session = NULL;
	char *signed_url = NULL;
	
	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "ElevenLabsConvAI requires an agent configuration name\n");
		return -1;
	}
	
	agent_name = ast_strdupa(data);
	ast_strip(agent_name);
	
	if (elevenlabs_debug) {
		ast_verb(2, "ElevenLabsConvAI: Starting session with agent '%s' on channel %s\n", 
			agent_name, ast_channel_name(chan));
	}
	
	/* Find agent configuration */
	config = find_agent_config(agent_name);
	if (!config) {
		ast_log(LOG_ERROR, "Agent configuration '%s' not found\n", agent_name);
		return -1;
	}
	
	/* Get signed URL */
	signed_url = get_signed_url(config);
	if (!signed_url) {
		ast_log(LOG_ERROR, "Failed to get signed URL for agent '%s'\n", agent_name);
		return -1;
	}
	
	/* Create session */
	session = ast_calloc(1, sizeof(struct elevenlabs_session));
	if (!session) {
		ast_log(LOG_ERROR, "Failed to allocate session memory\n");
		ast_free(signed_url);
		return -1;
	}
	
	session->channel = chan;
	session->config = config;
	session->signed_url = signed_url;
	session->session_active = 1;
	session->ws_connected = 0;
	session->playback_running = 0;
	session->framehook_id = -1;
	ast_mutex_init(&session->session_lock);
	
	/* Initialize audio buffers and queues */
	session->send_buffer = create_audio_buffer(8192); /* 8KB initial capacity */
	if (!session->send_buffer) {
		ast_log(LOG_ERROR, "Failed to create send buffer\n");
		cleanup_session(session);
		return -1;
	}
	
	session->receive_queue = create_audio_queue();
	if (!session->receive_queue) {
		ast_log(LOG_ERROR, "Failed to create receive queue\n");
		cleanup_session(session);
		return -1;
	}
	
	/* Initialize WebSocket connection */
	if (init_websocket(session) != 0) {
		ast_log(LOG_ERROR, "Failed to initialize WebSocket connection\n");
		cleanup_session(session);
		return -1;
	}
	
	/* Wait for WebSocket connection */
	int connection_timeout = 30; /* 30 seconds */
	while (!session->ws_connected && connection_timeout > 0 && session->session_active) {
		usleep(100000); /* 100ms */
		lws_service(session->ws_context, 100);
		connection_timeout--;
	}
	
	if (!session->ws_connected) {
		ast_log(LOG_ERROR, "WebSocket connection timeout\n");
		cleanup_session(session);
		return -1;
	}
	
	/* Attach framehook for audio capture */
	if (attach_framehook(session) != 0) {
		ast_log(LOG_ERROR, "Failed to attach framehook\n");
		cleanup_session(session);
		return -1;
	}
	
	/* Start playback thread */
	session->playback_running = 1;
	if (pthread_create(&session->playback_thread, NULL, playback_thread_func, session) != 0) {
		ast_log(LOG_ERROR, "Failed to start playback thread\n");
		session->playback_running = 0;
		cleanup_session(session);
		return -1;
	}
	
	ast_verb(2, "ElevenLabsConvAI: Session started successfully for agent '%s'\n", agent_name);
	
	/* Keep the session active until channel hangs up */
	while (session->session_active && ast_channel_state(chan) == AST_STATE_UP) {
		/* Service WebSocket events */
		lws_service(session->ws_context, 100);
		usleep(100000); /* 100ms */
	}
	
	cleanup_session(session);
	return 0;
}

/*! \brief Cleanup session resources */
static void cleanup_session(struct elevenlabs_session *session)
{
	if (!session) {
		return;
	}
	
	session->session_active = 0;
	
	/* Stop playback thread */
	if (session->playback_running) {
		session->playback_running = 0;
		if (session->receive_queue) {
			ast_cond_signal(&session->receive_queue->cond);
		}
		pthread_join(session->playback_thread, NULL);
	}
	
	/* Detach framehook */
	detach_framehook(session);
	
	/* Close WebSocket connection */
	cleanup_websocket(session);
	
	/* Free audio buffers and queues */
	if (session->send_buffer) {
		destroy_audio_buffer(session->send_buffer);
	}
	
	if (session->receive_queue) {
		destroy_audio_queue(session->receive_queue);
	}
	
	if (session->signed_url) {
		ast_free(session->signed_url);
	}
	
	ast_mutex_destroy(&session->session_lock);
	ast_free(session);
	
	if (elevenlabs_debug) {
		ast_verb(2, "ElevenLabsConvAI: Session cleanup completed\n");
	}
}

/*! \brief CLI command: elevenlabs debug */
static char *handle_elevenlabs_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "elevenlabs debug {on|off}";
		e->usage = "Usage: elevenlabs debug {on|off}\n"
			"       Enable or disable ElevenLabs debug logging\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	
	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	
	if (strcmp(a->argv[2], "on") == 0) {
		elevenlabs_debug = 1;
		ast_cli(a->fd, "ElevenLabs debug enabled\n");
	} else if (strcmp(a->argv[2], "off") == 0) {
		elevenlabs_debug = 0;
		ast_cli(a->fd, "ElevenLabs debug disabled\n");
	} else {
		return CLI_SHOWUSAGE;
	}
	
	return CLI_SUCCESS;
}

/*! \brief CLI command: elevenlabs reload */
static char *handle_elevenlabs_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "elevenlabs reload";
		e->usage = "Usage: elevenlabs reload\n"
			"       Reload ElevenLabs configuration\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	
	if (load_config() == 0) {
		ast_cli(a->fd, "ElevenLabs configuration reloaded successfully\n");
	} else {
		ast_cli(a->fd, "Failed to reload ElevenLabs configuration\n");
	}
	
	return CLI_SUCCESS;
}

/*! \brief CLI command: elevenlabs show agents */
static char *handle_elevenlabs_show_agents(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int i;
	
	switch (cmd) {
	case CLI_INIT:
		e->command = "elevenlabs show agents";
		e->usage = "Usage: elevenlabs show agents\n"
			"       Show configured ElevenLabs agents\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	
	ast_mutex_lock(&config_lock);
	
	ast_cli(a->fd, "ElevenLabs Agent Configurations:\n");
	ast_cli(a->fd, "%-20s %-40s %-20s %s\n", "Name", "Agent ID", "Buffer (ms)", "Caller Number");
	ast_cli(a->fd, "%-20s %-40s %-20s %s\n", "----", "--------", "-----------", "-------------");
	
	for (i = 0; i < agent_count; i++) {
		ast_cli(a->fd, "%-20s %-40s %-20d %s\n",
			agent_configs[i].name,
			agent_configs[i].agent_id,
			agent_configs[i].buffer_duration_ms,
			agent_configs[i].send_caller_number ? "Yes" : "No");
	}
	
	ast_mutex_unlock(&config_lock);
	
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_elevenlabs[] = {
	AST_CLI_DEFINE(handle_elevenlabs_debug, "Enable/disable ElevenLabs debug"),
	AST_CLI_DEFINE(handle_elevenlabs_reload, "Reload ElevenLabs configuration"),
	AST_CLI_DEFINE(handle_elevenlabs_show_agents, "Show ElevenLabs agent configurations"),
};

/*! \brief Module load function */
static int load_module(void)
{
	int res;
	
	/* Initialize CURL */
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		ast_log(LOG_ERROR, "Failed to initialize CURL\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	
	/* Load configuration */
	if (load_config() != 0) {
		ast_log(LOG_ERROR, "Failed to load configuration\n");
		curl_global_cleanup();
		return AST_MODULE_LOAD_DECLINE;
	}
	
	/* Register application */
	res = ast_register_application_xml(app, elevenlabs_exec);
	if (res) {
		ast_log(LOG_ERROR, "Failed to register application %s\n", app);
		curl_global_cleanup();
		return AST_MODULE_LOAD_DECLINE;
	}
	
	/* Register CLI commands */
	ast_cli_register_multiple(cli_elevenlabs, ARRAY_LEN(cli_elevenlabs));
	
	ast_log(LOG_NOTICE, "ElevenLabs ConvAI module loaded successfully\n");
	return AST_MODULE_LOAD_SUCCESS;
}

/*! \brief Module unload function */
static int unload_module(void)
{
	int res = 0;
	
	/* Unregister CLI commands */
	ast_cli_unregister_multiple(cli_elevenlabs, ARRAY_LEN(cli_elevenlabs));
	
	/* Unregister application */
	res = ast_unregister_application(app);
	
	/* Cleanup configuration */
	ast_mutex_lock(&config_lock);
	if (agent_configs) {
		ast_free(agent_configs);
		agent_configs = NULL;
	}
	agent_count = 0;
	ast_mutex_unlock(&config_lock);
	
	/* Cleanup CURL */
	curl_global_cleanup();
	
	ast_log(LOG_NOTICE, "ElevenLabs ConvAI module unloaded\n");
	return res;
}

/*! \brief Module reload function */
static int reload_module(void)
{
	if (load_config() == 0) {
		ast_log(LOG_NOTICE, "ElevenLabs ConvAI configuration reloaded\n");
		return AST_MODULE_LOAD_SUCCESS;
	}
	
	return AST_MODULE_LOAD_DECLINE;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "ElevenLabs ConvAI Application",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);