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
 * \brief ElevenLabs ConvAI dialplan application - Proper Architecture
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
#include "asterisk/format_cache.h"

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include <time.h>

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
			for real-time bidirectional audio streaming. The application sets up
			the connection and returns immediately, with audio processing handled
			asynchronously via framehooks.</para>
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

/*! \brief Audio queue item for buffering */
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

/*! \brief Main session state - persists throughout call */
struct elevenlabs_session {
	/* Channel and configuration */
	struct ast_channel *channel;
	struct agent_config *config;
	char channel_name[64];
	
	/* WebSocket connection */
	struct lws_context *ws_context;
	struct lws *ws_connection;
	char ws_host[256];
	char ws_path[512];
	int ws_port;
	int ws_connected;
	char *signed_url;
	
	/* Audio processing */
	struct audio_queue *receive_queue;
	
	/* Audio buffering for transmission */
	unsigned char *send_buffer;
	size_t send_buffer_size;
	size_t send_buffer_capacity;
	int send_buffer_duration_ms;
	struct timeval last_send_time;
	ast_mutex_t send_buffer_lock;
	
	/* Threading and lifecycle */
	pthread_t websocket_thread;
	pthread_t playback_thread;
	int websocket_running;
	int playback_running;
	int session_active;
	int interruption_pending; /* Flag to immediately stop current playback */
	
	/* Framehook */
	int framehook_id;
	
	/* Synchronization */
	ast_mutex_t session_lock;
	
	/* Sequence counters */
	int current_event_id;
	int read_frame_count;
	int write_frame_count;
	
	/* WebSocket message reassembly */
	char *message_buffer;
	size_t message_buffer_size;
	size_t message_buffer_capacity;
	ast_mutex_t message_buffer_lock;
};

/*! \brief Global agent configurations */
static struct agent_config *agent_configs = NULL;
static int agent_count = 0;
static ast_mutex_t config_lock = AST_MUTEX_INIT_VALUE;

/*! \brief Forward declarations */
static int elevenlabs_exec(struct ast_channel *chan, const char *data);
static int load_config(void);
static struct agent_config *find_agent_config(const char *name);

/* HTTP and WebSocket functions */
static char *get_signed_url(struct agent_config *config);
static int parse_signed_url(const char *signed_url, char *host, char *path, int *port);

/* Session management */
static struct elevenlabs_session *create_session(struct ast_channel *chan, struct agent_config *config);
static void destroy_session(struct elevenlabs_session *session);
static void session_cleanup_cb(void *data);

/* Audio processing */
static struct audio_queue *create_audio_queue(void);
static void destroy_audio_queue(struct audio_queue *queue);
static int audio_queue_add(struct audio_queue *queue, const unsigned char *data, size_t length, int event_id);
static struct audio_queue_item *audio_queue_get(struct audio_queue *queue);

/* Framehook */
static struct ast_frame *framehook_callback(struct ast_channel *chan, struct ast_frame *frame, 
	enum ast_framehook_event event, void *data);
static int attach_framehook(struct elevenlabs_session *session);
static void detach_framehook(struct elevenlabs_session *session);

/* Threading */
static void *websocket_thread_func(void *arg);
static void *playback_thread_func(void *arg);

/* WebSocket protocol */
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static int send_websocket_message(struct elevenlabs_session *session, const char *message);
static void handle_websocket_message(struct elevenlabs_session *session, const char *message);
static void send_conversation_initiation(struct elevenlabs_session *session);
static int send_audio_to_websocket(struct elevenlabs_session *session, const unsigned char *audio_data, size_t length);
static int add_to_message_buffer(struct elevenlabs_session *session, const char *data, size_t len, int is_final);
static void process_complete_message(struct elevenlabs_session *session);

/* Base64 encoding */
static char *base64_encode(const unsigned char *data, size_t input_length);
static unsigned char *base64_decode(const char *data, size_t *output_length);

/* Audio buffer management */
static int add_audio_to_send_buffer(struct elevenlabs_session *session, const unsigned char *data, size_t length);
static int should_send_buffer(struct elevenlabs_session *session);
static int send_buffered_audio(struct elevenlabs_session *session);

/* WebSocket protocol structure */
static struct lws_protocols protocols[] = {
	{
		"convai",
		websocket_callback,
		0,
		4096,
	},
	{ NULL, NULL, 0, 0 } /* terminator */
};

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

/*! \brief Main application function - BLOCKING until session ends */
static int elevenlabs_exec(struct ast_channel *chan, const char *data)
{
	char *agent_name;
	struct agent_config *config;
	struct elevenlabs_session *session = NULL;
	
	ast_log(LOG_NOTICE, "ElevenLabsConvAI: Starting session with agent '%s' on channel %s\n", 
		data ? data : "(null)", ast_channel_name(chan));
	
	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "ElevenLabsConvAI requires an agent configuration name\n");
		return -1;
	}
	
	agent_name = ast_strdupa(data);
	ast_strip(agent_name);
	
	/* Find agent configuration */
	config = find_agent_config(agent_name);
	if (!config) {
		ast_log(LOG_ERROR, "Agent configuration '%s' not found\n", agent_name);
		return -1;
	}
	
	/* Answer the channel */
	if (ast_answer(chan) != 0) {
		ast_log(LOG_ERROR, "Failed to answer channel\n");
		return -1;
	}
	
	/* Force channel to use μ-law format for consistent audio processing */
	struct ast_format *ulaw_format = ast_format_cache_get("ulaw");
	if (ulaw_format) {
		ast_log(LOG_NOTICE, "Forcing channel to μ-law format\n");
		
		int read_result = ast_set_read_format(chan, ulaw_format);
		int write_result = ast_set_write_format(chan, ulaw_format);
		
		ast_log(LOG_NOTICE, "Format forcing results: read=%d, write=%d\n", read_result, write_result);
		
		if (read_result != 0) {
			ast_log(LOG_ERROR, "Failed to set read format to μ-law (error: %d)\n", read_result);
		}
		if (write_result != 0) {
			ast_log(LOG_ERROR, "Failed to set write format to μ-law (error: %d)\n", write_result);
		}
		
		/* Verify the format was actually set */
		struct ast_format *current_read = ast_channel_readformat(chan);
		struct ast_format *current_write = ast_channel_writeformat(chan);
		
		ast_log(LOG_NOTICE, "Current formats after forcing: read=%s, write=%s\n", 
			ast_format_get_name(current_read), ast_format_get_name(current_write));
		
		ao2_ref(ulaw_format, -1); /* Release reference */
	} else {
		ast_log(LOG_ERROR, "Could not get μ-law format from cache\n");
	}
	
	/* Create session - this handles everything asynchronously */
	session = create_session(chan, config);
	if (!session) {
		ast_log(LOG_ERROR, "Failed to create ElevenLabs session\n");
		return -1;
	}
	
	ast_verb(2, "ElevenLabsConvAI: Session started for agent '%s' - processing in background\n", agent_name);
	
	/* 
	 * Return immediately - this is the proper Asterisk way.
	 * The framehook will handle all audio processing and session management.
	 * Session cleanup will happen automatically when the channel hangs up.
	 */
	return 0;
}

/*! \brief Create and initialize session */
static struct elevenlabs_session *create_session(struct ast_channel *chan, struct agent_config *config)
{
	struct elevenlabs_session *session;
	char *signed_url;
	
	/* Allocate session */
	session = ast_calloc(1, sizeof(struct elevenlabs_session));
	if (!session) {
		ast_log(LOG_ERROR, "Failed to allocate session memory\n");
		return NULL;
	}
	
	/* Initialize basic fields */
	session->channel = ast_channel_ref(chan); /* Take a reference to prevent channel destruction */
	session->config = config;
	ast_copy_string(session->channel_name, ast_channel_name(chan), sizeof(session->channel_name));
	session->framehook_id = -1;
	session->session_active = 1;
	session->interruption_pending = 0;
	session->current_event_id = 1;
	session->read_frame_count = 0;
	session->write_frame_count = 0;
	ast_mutex_init(&session->session_lock);
	
	/* Initialize message buffer for WebSocket reassembly */
	session->message_buffer_capacity = 64 * 1024; /* 64KB buffer for large audio messages */
	session->message_buffer = ast_calloc(1, session->message_buffer_capacity);
	if (!session->message_buffer) {
		ast_log(LOG_ERROR, "Failed to allocate message buffer\n");
		destroy_session(session);
		return NULL;
	}
	session->message_buffer_size = 0;
	ast_mutex_init(&session->message_buffer_lock);
	
	/* Initialize send buffer based on config */
	session->send_buffer_duration_ms = config->buffer_duration_ms;
	/* Calculate buffer capacity: 8kHz * 8-bit μ-law = 8 bytes per ms */
	session->send_buffer_capacity = session->send_buffer_duration_ms * 8;
	session->send_buffer = ast_calloc(1, session->send_buffer_capacity);
	if (!session->send_buffer) {
		ast_log(LOG_ERROR, "Failed to allocate send buffer\n");
		destroy_session(session);
		return NULL;
	}
	session->send_buffer_size = 0;
	gettimeofday(&session->last_send_time, NULL);
	ast_mutex_init(&session->send_buffer_lock);
	
	/* Get signed URL */
	ast_log(LOG_NOTICE, "Getting signed URL for agent '%s'\n", config->name);
	signed_url = get_signed_url(config);
	if (!signed_url) {
		ast_log(LOG_ERROR, "Failed to get signed URL\n");
		destroy_session(session);
		return NULL;
	}
	session->signed_url = signed_url;
	
	/* Create audio queue */
	session->receive_queue = create_audio_queue();
	if (!session->receive_queue) {
		ast_log(LOG_ERROR, "Failed to create audio queue\n");
		destroy_session(session);
		return NULL;
	}
	
	/* Attach framehook */
	if (attach_framehook(session) != 0) {
		ast_log(LOG_ERROR, "Failed to attach framehook\n");
		destroy_session(session);
		return NULL;
	}
	
	/* Start WebSocket thread */
	session->websocket_running = 1;
	if (pthread_create(&session->websocket_thread, NULL, websocket_thread_func, session) != 0) {
		ast_log(LOG_ERROR, "Failed to create WebSocket thread\n");
		destroy_session(session);
		return NULL;
	}
	
	/* Start playback thread */
	session->playback_running = 1;
	if (pthread_create(&session->playback_thread, NULL, playback_thread_func, session) != 0) {
		ast_log(LOG_ERROR, "Failed to create playback thread\n");
		session->websocket_running = 0;
		pthread_join(session->websocket_thread, NULL);
		destroy_session(session);
		return NULL;
	}
	
	ast_log(LOG_NOTICE, "ElevenLabs session created successfully\n");
	return session;
}

/*! \brief Session cleanup callback - called when channel hangs up */
static void session_cleanup_cb(void *data)
{
	struct elevenlabs_session *session = data;
	
	ast_log(LOG_NOTICE, "=== SESSION CLEANUP START ===\n");
	
	if (!session) {
		ast_log(LOG_ERROR, "Session cleanup called with NULL session\n");
		return;
	}
	
	ast_log(LOG_NOTICE, "Cleaning up ElevenLabs session for channel %s\n", session->channel_name);
	
	/* Add session state logging */
	ast_mutex_lock(&session->session_lock);
	ast_log(LOG_NOTICE, "Session state: active=%d, ws_connected=%d, websocket_running=%d, playback_running=%d\n",
		session->session_active, session->ws_connected, session->websocket_running, session->playback_running);
	ast_mutex_unlock(&session->session_lock);
	
	destroy_session(session);
	ast_log(LOG_NOTICE, "=== SESSION CLEANUP END ===\n");
}

/*! \brief Destroy session and cleanup resources */
static void destroy_session(struct elevenlabs_session *session)
{
	if (!session) {
		ast_log(LOG_ERROR, "destroy_session called with NULL session\n");
		return;
	}
	
	ast_log(LOG_NOTICE, "=== DESTROY SESSION START ===\n");
	
	/* Mark session as inactive first */
	ast_log(LOG_NOTICE, "Step 1: Marking session as inactive\n");
	ast_mutex_lock(&session->session_lock);
	session->session_active = 0;
	session->ws_connected = 0;
	ast_mutex_unlock(&session->session_lock);
	
	/* Stop WebSocket thread */
	ast_log(LOG_NOTICE, "Step 2: Stopping WebSocket thread (running=%d)\n", session->websocket_running);
	if (session->websocket_running) {
		session->websocket_running = 0;
		ast_log(LOG_NOTICE, "Waiting for WebSocket thread to stop\n");
		pthread_join(session->websocket_thread, NULL);
		ast_log(LOG_NOTICE, "WebSocket thread stopped\n");
	}
	
	/* Stop playback thread */
	ast_log(LOG_NOTICE, "Step 3: Stopping playback thread (running=%d)\n", session->playback_running);
	if (session->playback_running) {
		session->playback_running = 0;
		if (session->receive_queue) {
			ast_log(LOG_NOTICE, "Signaling playback thread condition\n");
			ast_cond_signal(&session->receive_queue->cond);
		}
		ast_log(LOG_NOTICE, "Waiting for playback thread to stop\n");
		pthread_join(session->playback_thread, NULL);
		ast_log(LOG_NOTICE, "Playback thread stopped\n");
	}
	
	/* Detach framehook */
	ast_log(LOG_NOTICE, "Step 4: Detaching framehook (ID=%d)\n", session->framehook_id);
	detach_framehook(session);
	
	/* Cleanup WebSocket safely */
	ast_log(LOG_NOTICE, "Step 5: Cleaning up WebSocket connection\n");
	ast_mutex_lock(&session->session_lock);
	if (session->ws_connection) {
		ast_log(LOG_NOTICE, "Closing WebSocket connection\n");
		lws_close_reason(session->ws_connection, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
		session->ws_connection = NULL;
		session->ws_connected = 0;
		/* Small delay to allow WebSocket cleanup to complete */
		ast_mutex_unlock(&session->session_lock);
		ast_log(LOG_NOTICE, "Sleeping 100ms for WebSocket cleanup\n");
		usleep(100000); /* 100ms delay to allow cleanup */
	} else {
		session->ws_connected = 0;
		ast_mutex_unlock(&session->session_lock);
		ast_log(LOG_NOTICE, "WebSocket connection already NULL\n");
	}
	
	ast_log(LOG_NOTICE, "Step 6: Destroying WebSocket context\n");
	if (session->ws_context) {
		ast_log(LOG_NOTICE, "Destroying WebSocket context\n");
		lws_context_destroy(session->ws_context);
		session->ws_context = NULL;
		ast_log(LOG_NOTICE, "WebSocket context destroyed\n");
	} else {
		ast_log(LOG_NOTICE, "WebSocket context already NULL\n");
	}
	
	/* Cleanup audio queue */
	ast_log(LOG_NOTICE, "Step 7: Cleaning up audio queue\n");
	if (session->receive_queue) {
		destroy_audio_queue(session->receive_queue);
		session->receive_queue = NULL;
		ast_log(LOG_NOTICE, "Audio queue destroyed\n");
	}
	
	/* Free signed URL */
	ast_log(LOG_NOTICE, "Step 8: Freeing signed URL\n");
	if (session->signed_url) {
		ast_free(session->signed_url);
		session->signed_url = NULL;
	}
	
	/* Cleanup send buffer */
	ast_log(LOG_NOTICE, "Step 9: Cleaning up send buffer\n");
	if (session->send_buffer) {
		ast_free(session->send_buffer);
		session->send_buffer = NULL;
	}
	
	/* Cleanup message buffer */
	ast_log(LOG_NOTICE, "Step 10: Cleaning up message buffer\n");
	if (session->message_buffer) {
		ast_free(session->message_buffer);
		session->message_buffer = NULL;
	}
	
	/* Release channel reference */
	ast_log(LOG_NOTICE, "Step 11: Releasing channel reference\n");
	if (session->channel) {
		ast_channel_unref(session->channel);
		session->channel = NULL;
		ast_log(LOG_NOTICE, "Channel reference released\n");
	}
	
	/* Destroy mutexes */
	ast_log(LOG_NOTICE, "Step 12: Destroying mutexes\n");
	ast_mutex_destroy(&session->message_buffer_lock);
	ast_mutex_destroy(&session->send_buffer_lock);
	ast_mutex_destroy(&session->session_lock);
	
	/* Free session */
	ast_log(LOG_NOTICE, "Step 13: Freeing session structure\n");
	ast_free(session);
	
	ast_log(LOG_NOTICE, "=== DESTROY SESSION END ===\n");
}

/*! \brief Framehook callback - processes audio frames in real-time */
static struct ast_frame *framehook_callback(struct ast_channel *chan, struct ast_frame *frame, 
	enum ast_framehook_event event, void *data)
{
	struct elevenlabs_session *session = data;
	
	if (!session || !session->session_active || !frame) {
		return frame;
	}
	
	/* Only process voice frames */
	if (frame->frametype != AST_FRAME_VOICE) {
		return frame;
	}
	
	switch (event) {
	case AST_FRAMEHOOK_EVENT_READ:
		/* Incoming audio from caller - buffer and send to ElevenLabs */
		if (frame->data.ptr && frame->datalen > 0) {
			int should_process = 0;
			
			/* Check connection state under lock */
			ast_mutex_lock(&session->session_lock);
			should_process = (session->session_active && session->ws_connected);
			ast_mutex_unlock(&session->session_lock);
			
			if (should_process) {
				/* Log frame details only in debug mode */
				if (elevenlabs_debug) {
					const char *format_name = ast_format_get_name(frame->subclass.format);
					session->read_frame_count++;
					ast_log(LOG_DEBUG, "Framehook READ #%d: %s frame - %d bytes, %d samples\n", 
						session->read_frame_count, format_name, frame->datalen, frame->samples);
				}
				
				add_audio_to_send_buffer(session, frame->data.ptr, frame->datalen);
				if (should_send_buffer(session)) {
					send_buffered_audio(session);
				}
			}
		}
		break;
		
	case AST_FRAMEHOOK_EVENT_WRITE:
		/* Outgoing audio to caller - normal processing for now */
		if (frame->data.ptr && frame->datalen > 0) {
			const char *format_name = ast_format_get_name(frame->subclass.format);
			session->write_frame_count++;
			ast_log(LOG_NOTICE, "Framehook WRITE #%d: %s frame - %d bytes, %d samples\n", 
				session->write_frame_count, format_name, frame->datalen, frame->samples);
		}
		/* Note: ElevenLabs audio is injected separately via ast_write in playback thread */
		break;
		
	default:
		break;
	}
	
	return frame;
}

/*! \brief Attach framehook to channel */
static int attach_framehook(struct elevenlabs_session *session)
{
	struct ast_framehook_interface interface = {
		.version = AST_FRAMEHOOK_INTERFACE_VERSION,
		.event_cb = framehook_callback,
		.destroy_cb = session_cleanup_cb,
		.data = session,
	};
	
	session->framehook_id = ast_framehook_attach(session->channel, &interface);
	if (session->framehook_id < 0) {
		ast_log(LOG_ERROR, "Failed to attach framehook\n");
		return -1;
	}
	
	ast_log(LOG_NOTICE, "Framehook attached (ID: %d)\n", session->framehook_id);
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
	
	ast_log(LOG_NOTICE, "Framehook detached\n");
}

/*! \brief WebSocket thread function - runs in background */
static void *websocket_thread_func(void *arg)
{
	struct elevenlabs_session *session = arg;
	struct lws_context_creation_info info;
	struct lws_client_connect_info connect_info;
	int n;
	
	ast_log(LOG_NOTICE, "WebSocket thread starting for channel %s\n", session->channel_name);
	
	/* Parse signed URL */
	if (parse_signed_url(session->signed_url, session->ws_host, 
			session->ws_path, &session->ws_port) != 0) {
		ast_log(LOG_ERROR, "Failed to parse signed URL: %s\n", session->signed_url);
		return NULL;
	}
	
	ast_log(LOG_NOTICE, "Parsed URL - host=%s, port=%d, path=%s\n", 
		session->ws_host, session->ws_port, session->ws_path);
	
	/* Create WebSocket context */
	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.user = session;
	
	session->ws_context = lws_create_context(&info);
	if (!session->ws_context) {
		ast_log(LOG_ERROR, "Failed to create WebSocket context\n");
		return NULL;
	}
	
	/* Connect to WebSocket */
	memset(&connect_info, 0, sizeof(connect_info));
	connect_info.context = session->ws_context;
	connect_info.address = session->ws_host;
	connect_info.port = session->ws_port;
	connect_info.path = session->ws_path;
	connect_info.host = session->ws_host;
	connect_info.origin = session->ws_host;
	connect_info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	connect_info.protocol = "convai";
	connect_info.pwsi = &session->ws_connection;
	
	session->ws_connection = lws_client_connect_via_info(&connect_info);
	if (!session->ws_connection) {
		ast_log(LOG_ERROR, "Failed to initiate WebSocket connection\n");
		lws_context_destroy(session->ws_context);
		session->ws_context = NULL;
		return NULL;
	}
	
	ast_log(LOG_NOTICE, "WebSocket connection initiated\n");
	
	/* Main WebSocket service loop */
	ast_log(LOG_NOTICE, "=== WEBSOCKET SERVICE LOOP START ===\n");
	while (session->websocket_running && session->session_active) {
		n = lws_service(session->ws_context, 100); /* 100ms timeout */
		if (n < 0) {
			ast_log(LOG_ERROR, "WebSocket service error: %d - EXITING LOOP\n", n);
			break;
		}
	}
	
	ast_log(LOG_NOTICE, "=== WEBSOCKET SERVICE LOOP END ===\n");
	ast_log(LOG_NOTICE, "Loop exit reason: websocket_running=%d, session_active=%d\n", 
		session->websocket_running, session->session_active);
	ast_log(LOG_NOTICE, "WebSocket thread exiting for channel %s\n", session->channel_name);
	return NULL;
}

/*! \brief Send audio data to WebSocket */
static int send_audio_to_websocket(struct elevenlabs_session *session, const unsigned char *audio_data, size_t length)
{
	cJSON *json;
	char *base64_audio;
	char *message;
	int connection_ok = 0;
	
	if (!session || !audio_data || length == 0) {
		return -1;
	}
	
	/* Check connection state under lock */
	ast_mutex_lock(&session->session_lock);
	connection_ok = (session->session_active && session->ws_connected && session->ws_connection);
	ast_mutex_unlock(&session->session_lock);
	
	if (!connection_ok) {
		ast_log(LOG_DEBUG, "WebSocket not connected, skipping audio send\n");
		return -1;
	}
	
	/* Encode audio as base64 */
	base64_audio = base64_encode(audio_data, length);
	if (!base64_audio) {
		ast_log(LOG_ERROR, "Failed to encode audio data\n");
		return -1;
	}
	
	/* Create audio message with correct format */
	json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "user_audio_chunk", base64_audio);
	
	message = cJSON_Print(json);
	if (message) {
		ast_log(LOG_NOTICE, "Sending audio chunk to ElevenLabs: %zu bytes raw audio, %zu bytes base64\n", 
			length, strlen(base64_audio));
		
		/* send_websocket_message now has its own connection validation */
		if (send_websocket_message(session, message) < 0) {
			ast_log(LOG_DEBUG, "Failed to send audio message - WebSocket may be disconnected\n");
		}
		ast_free(message);
	}
	
	cJSON_Delete(json);
	ast_free(base64_audio);
	return 0;
}

/*! \brief Audio playback thread function */
static void *playback_thread_func(void *arg)
{
	struct elevenlabs_session *session = arg;
	struct audio_queue_item *item;
	struct ast_frame *frame;
	size_t frame_size = 160; /* 20ms at 8kHz, μ-law = 160 bytes */
	unsigned char *audio_ptr;
	size_t remaining;
	struct timespec sleep_time;
	
	sleep_time.tv_sec = 0;
	sleep_time.tv_nsec = 20000000; /* 20ms */
	
	ast_log(LOG_NOTICE, "Playback thread starting for channel %s\n", session->channel_name);
	
	ast_log(LOG_NOTICE, "=== PLAYBACK THREAD MAIN LOOP START ===\n");
	while (session->playback_running && session->session_active) {
		/* Wait for audio data */
		ast_mutex_lock(&session->receive_queue->lock);
		while (session->receive_queue->count == 0 && session->playback_running) {
			struct timespec timeout;
			timeout.tv_sec = time(NULL) + 1;
			timeout.tv_nsec = 0;
			ast_cond_timedwait(&session->receive_queue->cond, &session->receive_queue->lock, &timeout);
		}
		ast_mutex_unlock(&session->receive_queue->lock);
		
		if (!session->playback_running) {
			ast_log(LOG_NOTICE, "Playback thread exiting - playback_running=false\n");
			break;
		}
		
		item = audio_queue_get(session->receive_queue);
		if (!item) {
			continue;
		}
		
		/* Check for interruption before starting to play this chunk */
		ast_mutex_lock(&session->session_lock);
		if (session->interruption_pending) {
			ast_log(LOG_NOTICE, "Interruption detected - skipping audio chunk with event_id=%d\n", item->event_id);
			session->interruption_pending = 0; /* Reset flag */
			ast_mutex_unlock(&session->session_lock);
			
			/* Free the current audio item and continue */
			ast_free(item->audio_data);
			ast_free(item);
			continue;
		}
		ast_mutex_unlock(&session->session_lock);
		
		ast_log(LOG_NOTICE, "Playing audio chunk: %zu bytes (event_id: %d)\n", 
			item->data_length, item->event_id);
		
		/* Split audio data into 20ms frames and play them */
		audio_ptr = item->audio_data;
		remaining = item->data_length;
		
		while (remaining > 0 && session->playback_running) {
			/* Check for interruption during frame playback */
			ast_mutex_lock(&session->session_lock);
			if (session->interruption_pending) {
				ast_log(LOG_NOTICE, "Interruption detected during frame playback - stopping audio chunk\n");
				session->interruption_pending = 0; /* Reset flag */
				ast_mutex_unlock(&session->session_lock);
				break; /* Exit the frame loop */
			}
			ast_mutex_unlock(&session->session_lock);
			
			size_t chunk_size = (remaining < frame_size) ? remaining : frame_size;
			
			/* Create audio frame */
			frame = ast_calloc(1, sizeof(struct ast_frame) + chunk_size);
			if (!frame) {
				break;
			}
			
			frame->frametype = AST_FRAME_VOICE;
			frame->subclass.format = ast_format_cache_get("ulaw");
			frame->data.ptr = frame + 1;
			frame->datalen = chunk_size;
			frame->samples = chunk_size; /* μ-law: 1 byte = 1 sample */
			
			memcpy(frame->data.ptr, audio_ptr, chunk_size);
			
			/* Write frame to channel - check if session is still active and channel is valid */
			ast_mutex_lock(&session->session_lock);
			int session_ok = session->session_active && session->channel;
			ast_mutex_unlock(&session->session_lock);
			
			if (session_ok) {
				/* Check channel state before writing */
				if (ast_channel_state(session->channel) == AST_STATE_UP && !ast_check_hangup(session->channel)) {
					if (ast_write(session->channel, frame) < 0) {
						ast_log(LOG_WARNING, "Failed to write audio frame to channel\n");
						/* Stop playback on write error */
						session->playback_running = 0;
					} else {
						ast_log(LOG_NOTICE, "Injected audio frame: %zu bytes, %zu samples\n", 
							chunk_size, chunk_size);
					}
				} else {
					ast_log(LOG_NOTICE, "Channel not available for writing, stopping audio playback\n");
					session->playback_running = 0;
				}
			} else {
				ast_log(LOG_NOTICE, "Session inactive, stopping audio playback\n");
				session->playback_running = 0;
			}
			
			/* Release format reference and free frame */
			if (frame->subclass.format) {
				ao2_ref(frame->subclass.format, -1);
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
	
	ast_log(LOG_NOTICE, "=== PLAYBACK THREAD MAIN LOOP END ===\n");
	ast_log(LOG_NOTICE, "Loop exit reason: playback_running=%d, session_active=%d\n", 
		session->playback_running, session->session_active);
	ast_log(LOG_NOTICE, "Playback thread exiting for channel %s\n", session->channel_name);
	return NULL;
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

/*! \brief WebSocket callback handler */
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	struct elevenlabs_session *session = NULL;
	
	ast_log(LOG_NOTICE, "=== WEBSOCKET CALLBACK: reason=%d (LWS_CALLBACK_CLOSED=%d) ===\n", reason, LWS_CALLBACK_CLOSED);
	
	/* Add safety check for critical callbacks */
	if (reason == LWS_CALLBACK_CLOSED) { 
		ast_log(LOG_NOTICE, "WebSocket CLOSED callback - entering safe handling (reason=%d)\n", reason);
	} else if (reason == 30) {
		ast_log(LOG_NOTICE, "WebSocket ACTUAL CLOSE callback - this is the real close event (reason=30)\n");
	} else if (reason == 38) {
		ast_log(LOG_NOTICE, "WebSocket callback reason=38 (unknown)\n");
	} else if (reason == 75) {
		ast_log(LOG_NOTICE, "WebSocket callback reason=75 (unknown)\n");
	}
	
	/* Get session for relevant callbacks */
	if (reason == LWS_CALLBACK_CLIENT_ESTABLISHED || 
	    reason == LWS_CALLBACK_CLIENT_RECEIVE ||
	    reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR ||
	    reason == LWS_CALLBACK_CLOSED ||
	    reason == 30) {  /* Add reason 30 for session retrieval */
		
		ast_log(LOG_NOTICE, "Getting session context for callback reason=%d\n", reason);
		
		if (wsi && lws_get_context(wsi)) {
			ast_log(LOG_NOTICE, "WSI and context valid, getting session\n");
			session = (struct elevenlabs_session *)lws_context_user(lws_get_context(wsi));
			
			/* Validate session pointer - allow callbacks during cleanup */
			if (!session) {
				ast_log(LOG_ERROR, "WebSocket callback called with no session context (reason=%d)\n", reason);
				return -1;
			}
			
			ast_log(LOG_NOTICE, "Session found: %p, checking state\n", session);
			
			/* For ping/pong and closed callbacks, don't require active session */
			if (reason != LWS_CALLBACK_CLOSED && reason != LWS_CALLBACK_CLIENT_CONNECTION_ERROR && reason != 30) {
				if (!session->session_active) {
					ast_log(LOG_DEBUG, "WebSocket callback called with inactive session (cleanup in progress)\n");
					return 0; /* Allow cleanup to continue */
				}
			}
		} else {
			ast_log(LOG_ERROR, "WSI or context is NULL for callback reason=%d\n", reason);
			return -1;
		}
	}
	
	ast_log(LOG_NOTICE, "Session validation complete for reason=%d, entering switch\n", reason);
	
	/* CRITICAL: Move emergency deactivation BEFORE switch statement */
	if ((reason == LWS_CALLBACK_CLOSED || reason == 30) && session) { 
		ast_log(LOG_NOTICE, "PRE-SWITCH: Detected CLOSE callback - emergency deactivation (reason=%d)\n", reason);
		
		/* IMMEDIATELY stop all other threads to prevent race conditions */
		ast_log(LOG_NOTICE, "EMERGENCY: Immediately deactivating session to stop race conditions\n");
		ast_mutex_lock(&session->session_lock);
		session->session_active = 0;  /* Stop framehook immediately */
		session->ws_connected = 0;    /* Stop sending to WebSocket immediately */
		ast_mutex_unlock(&session->session_lock);
		ast_log(LOG_NOTICE, "Session deactivated - other threads should stop now\n");
		
		/* Give other threads a moment to see the deactivation */
		usleep(10000); /* 10ms delay */
		ast_log(LOG_NOTICE, "Delay complete, about to enter switch for reason=30\n");
		ast_log(LOG_NOTICE, "Session pointer after emergency deactivation: %p\n", session);
	}
	
	ast_log(LOG_NOTICE, "Entering switch statement for reason=%d\n", reason);
	
	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		ast_log(LOG_NOTICE, "SWITCH: Matched LWS_CALLBACK_CLIENT_ESTABLISHED\n");
		if (session) {
			session->ws_connected = 1;
			ast_log(LOG_NOTICE, "WebSocket connection established for channel %s\n", session->channel_name);
			
			/* Send conversation initiation after 2 seconds */
			sleep(2);
			send_conversation_initiation(session);
		}
		break;
		
	case LWS_CALLBACK_CLIENT_RECEIVE:
		ast_log(LOG_NOTICE, "SWITCH: Matched LWS_CALLBACK_CLIENT_RECEIVE\n");
		if (session && in && len > 0 && session->session_active) {
			/* Check if this is the final fragment */
			int is_final = lws_is_final_fragment(wsi);
			add_to_message_buffer(session, (char *)in, len, is_final);
		}
		break;
		
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		ast_log(LOG_ERROR, "SWITCH: Matched LWS_CALLBACK_CLIENT_CONNECTION_ERROR\n");
		if (session) {
			ast_log(LOG_NOTICE, "Marking session as disconnected due to connection error\n");
			ast_mutex_lock(&session->session_lock);
			session->ws_connected = 0;
			session->websocket_running = 0;
			session->session_active = 0;
			/* Clear connection pointer to prevent further use */
			session->ws_connection = NULL;
			ast_mutex_unlock(&session->session_lock);
			
			/* Signal playback thread to stop */
			if (session->receive_queue) {
				ast_log(LOG_NOTICE, "Signaling playback thread due to connection error\n");
				ast_cond_signal(&session->receive_queue->cond);
			}
		}
		ast_log(LOG_ERROR, "=== CONNECTION ERROR HANDLING COMPLETE ===\n");
		break;
		
	case 30: /* The ACTUAL close event in this libwebsockets version */
		ast_log(LOG_NOTICE, "SWITCH: Matched ACTUAL CLOSE EVENT (reason=30)\n");
		ast_log(LOG_NOTICE, "Session pointer at case 30: %p\n", session);
		if (session) {
			ast_log(LOG_NOTICE, "Session pointer valid: %p for channel: %s\n", session, session->channel_name);
			
			/* Session already deactivated above, just finalize cleanup */
			ast_log(LOG_NOTICE, "Finalizing actual close - setting websocket_running=0\n");
			ast_mutex_lock(&session->session_lock);
			session->websocket_running = 0;
			session->ws_connection = NULL;
			ast_mutex_unlock(&session->session_lock);
			
			/* Signal playback thread to stop */
			ast_log(LOG_NOTICE, "Signaling playback thread to stop\n");
			if (session->receive_queue) {
				ast_cond_signal(&session->receive_queue->cond);
				ast_log(LOG_NOTICE, "Playback thread signaled\n");
			} else {
				ast_log(LOG_WARNING, "Receive queue is NULL during close\n");
			}
			
			/* Hang up the call since conversation is over */
			if (session->channel && !ast_check_hangup(session->channel)) {
				ast_log(LOG_NOTICE, "WebSocket closed - hanging up call\n");
				ast_softhangup(session->channel, AST_SOFTHANGUP_EXPLICIT);
			}
		} else {
			ast_log(LOG_ERROR, "Session is NULL in ACTUAL CLOSE callback!\n");
		}
		ast_log(LOG_NOTICE, "=== ACTUAL CLOSE HANDLING COMPLETE ===\n");
		break;
		
	case LWS_CALLBACK_CLOSED:
		ast_log(LOG_NOTICE, "SWITCH: Matched LWS_CALLBACK_CLOSED (reason=%d - standard close)\n", reason);
		if (session) {
			ast_log(LOG_NOTICE, "Session pointer valid: %p for channel: %s\n", session, session->channel_name);
			
			/* Session already deactivated above, just finalize cleanup */
			ast_log(LOG_NOTICE, "Finalizing close - setting websocket_running=0\n");
			ast_mutex_lock(&session->session_lock);
			session->websocket_running = 0;
			session->ws_connection = NULL;
			ast_mutex_unlock(&session->session_lock);
			
			/* Signal playback thread to stop */
			ast_log(LOG_NOTICE, "Signaling playback thread to stop\n");
			if (session->receive_queue) {
				ast_cond_signal(&session->receive_queue->cond);
				ast_log(LOG_NOTICE, "Playback thread signaled\n");
			} else {
				ast_log(LOG_WARNING, "Receive queue is NULL during close\n");
			}
			
			/* Hang up the call since conversation is over */
			if (session->channel && !ast_check_hangup(session->channel)) {
				ast_log(LOG_NOTICE, "Standard WebSocket closed - hanging up call\n");
				ast_softhangup(session->channel, AST_SOFTHANGUP_EXPLICIT);
			}
		} else {
			ast_log(LOG_ERROR, "Session is NULL in CLOSED callback!\n");
		}
		ast_log(LOG_NOTICE, "=== CLOSE HANDLING COMPLETE ===\n");
		break;
		
	default:
		ast_log(LOG_NOTICE, "SWITCH: Matched default case for reason=%d\n", reason);
		break;
	}
	
	ast_log(LOG_NOTICE, "Exiting websocket_callback for reason=%d\n", reason);
	return 0;
}

/*! \brief Send WebSocket message */
static int send_websocket_message(struct elevenlabs_session *session, const char *message)
{
	size_t len;
	unsigned char *buf;
	int result = -1;
	struct lws *ws_conn;
	
	if (!session || !message) {
		return -1;
	}
	
	/* Lock session to check connection state atomically */
	ast_mutex_lock(&session->session_lock);
	
	/* Double-check connection state under lock */
	if (!session->session_active || !session->ws_connected || !session->ws_connection) {
		ast_mutex_unlock(&session->session_lock);
		ast_log(LOG_DEBUG, "WebSocket not connected, skipping message send\n");
		return -1;
	}
	
	/* Store connection pointer while locked */
	ws_conn = session->ws_connection;
	ast_mutex_unlock(&session->session_lock);
	
	len = strlen(message);
	buf = ast_calloc(1, LWS_PRE + len + 1);
	if (!buf) {
		return -1;
	}
	
	memcpy(buf + LWS_PRE, message, len);
	
	/* Use stored connection pointer - additional validation */
	if (ws_conn && lws_write(ws_conn, buf + LWS_PRE, len, LWS_WRITE_TEXT) >= 0) {
		result = 0;
		/* Log sent messages only in debug mode */
		if (elevenlabs_debug) {
			ast_log(LOG_DEBUG, "Sent to ElevenLabs: %s\n", message);
		}
	} else {
		ast_log(LOG_WARNING, "Failed to send WebSocket message - connection may be closed\n");
		
		/* Mark connection as disconnected on write failure */
		ast_mutex_lock(&session->session_lock);
		session->ws_connected = 0;
		ast_mutex_unlock(&session->session_lock);
	}
	
	ast_free(buf);
	return result;
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
		ast_log(LOG_WARNING, "Failed to parse JSON message: %s\n", message);
		return;
	}
	
	type_item = cJSON_GetObjectItem(json, "type");
	if (!type_item || !cJSON_IsString(type_item)) {
		ast_log(LOG_WARNING, "Message missing 'type' field: %s\n", message);
		cJSON_Delete(json);
		return;
	}
	
	ast_log(LOG_NOTICE, "Received %s message\n", type_item->valuestring);
	
	/* Handle vad_score messages with detailed logging */
	if (strcmp(type_item->valuestring, "vad_score") == 0) {
		cJSON *vad_score_event = cJSON_GetObjectItem(json, "vad_score_event");
		
		if (vad_score_event && cJSON_IsObject(vad_score_event)) {
			cJSON *score_item = cJSON_GetObjectItem(vad_score_event, "vad_score");
			cJSON *timestamp_item = cJSON_GetObjectItem(vad_score_event, "timestamp");
			
			if (score_item && cJSON_IsNumber(score_item)) {
				double score = score_item->valuedouble;
				ast_log(LOG_NOTICE, "=== VAD SCORE MESSAGE ===\n");
				ast_log(LOG_NOTICE, "VAD Score: %.3f\n", score);
				
				if (timestamp_item && cJSON_IsString(timestamp_item)) {
					ast_log(LOG_NOTICE, "Timestamp: %s\n", timestamp_item->valuestring);
				}
				
				if (score > 0.5) {
					ast_log(LOG_NOTICE, "HIGH voice activity detected (score=%.3f)\n", score);
				} else {
					ast_log(LOG_NOTICE, "LOW voice activity detected (score=%.3f)\n", score);
				}
				ast_log(LOG_NOTICE, "=== END VAD SCORE ===\n");
			} else {
				ast_log(LOG_WARNING, "vad_score_event missing valid vad_score field\n");
			}
		} else {
			ast_log(LOG_WARNING, "vad_score message missing vad_score_event object\n");
		}
	}
	/* Handle interruption messages with detailed logging */
	else if (strcmp(type_item->valuestring, "interruption") == 0) {
		cJSON *interruption_event = cJSON_GetObjectItem(json, "interruption_event");
		
		ast_log(LOG_NOTICE, "=== INTERRUPTION MESSAGE ===\n");
		
		if (interruption_event && cJSON_IsObject(interruption_event)) {
			cJSON *event_id_item = cJSON_GetObjectItem(interruption_event, "event_id");
			cJSON *timestamp_item = cJSON_GetObjectItem(interruption_event, "timestamp");
			
			if (event_id_item && cJSON_IsNumber(event_id_item)) {
				ast_log(LOG_NOTICE, "Event ID: %d\n", event_id_item->valueint);
			}
			
			if (timestamp_item && cJSON_IsString(timestamp_item)) {
				ast_log(LOG_NOTICE, "Timestamp: %s\n", timestamp_item->valuestring);
			}
		} else {
			ast_log(LOG_WARNING, "interruption message missing interruption_event object\n");
		}
		
		ast_log(LOG_NOTICE, "User interrupted AI speech - stopping playback\n");
		
		/* Process interruption based on event_id */
		if (interruption_event && cJSON_IsObject(interruption_event)) {
			cJSON *event_id_item = cJSON_GetObjectItem(interruption_event, "event_id");
			int interrupt_event_id = event_id_item ? event_id_item->valueint : -1;
			
			if (interrupt_event_id >= 0) {
				ast_log(LOG_NOTICE, "Processing interruption for event_id=%d - clearing older audio\n", interrupt_event_id);
				
				/* Set interruption flag to immediately stop current playback */
				ast_mutex_lock(&session->session_lock);
				session->interruption_pending = 1;
				ast_mutex_unlock(&session->session_lock);
				
				/* Clear audio queue items with event_id <= interrupt_event_id */
				if (session->receive_queue) {
					ast_mutex_lock(&session->receive_queue->lock);
					
					struct audio_queue_item **current = &session->receive_queue->head;
					int removed_count = 0;
					
					while (*current) {
						struct audio_queue_item *item = *current;
						
						if (item->event_id <= interrupt_event_id) {
							/* Remove this item */
							*current = item->next;
							
							/* Update tail if we're removing the last item */
							if (item == session->receive_queue->tail) {
								session->receive_queue->tail = NULL;
							}
							
							ast_log(LOG_NOTICE, "Removing audio chunk with event_id=%d (≤%d)\n", 
								item->event_id, interrupt_event_id);
							
							ast_free(item->audio_data);
							ast_free(item);
							removed_count++;
							session->receive_queue->count--;
						} else {
							/* Keep this item, move to next */
							current = &item->next;
						}
					}
					
					/* Fix tail pointer if queue is not empty */
					if (session->receive_queue->head && !session->receive_queue->tail) {
						struct audio_queue_item *item = session->receive_queue->head;
						while (item->next) {
							item = item->next;
						}
						session->receive_queue->tail = item;
					}
					
					ast_mutex_unlock(&session->receive_queue->lock);
					
					ast_log(LOG_NOTICE, "Interruption processing complete: removed %d audio chunks with event_id ≤ %d\n", 
						removed_count, interrupt_event_id);
					ast_log(LOG_NOTICE, "Remaining audio chunks in queue: %d\n", session->receive_queue->count);
				}
				
				/* Signal playback thread to check for interruption */
				if (session->receive_queue) {
					ast_cond_signal(&session->receive_queue->cond);
				}
			} else {
				ast_log(LOG_WARNING, "Invalid interrupt event_id, clearing entire queue\n");
				
				/* Fallback: clear entire queue */
				if (session->receive_queue) {
					ast_mutex_lock(&session->receive_queue->lock);
					while (session->receive_queue->head) {
						struct audio_queue_item *item = session->receive_queue->head;
						session->receive_queue->head = item->next;
						ast_free(item->audio_data);
						ast_free(item);
					}
					session->receive_queue->tail = NULL;
					session->receive_queue->count = 0;
					ast_mutex_unlock(&session->receive_queue->lock);
					ast_log(LOG_NOTICE, "Entire audio playback queue cleared due to interruption\n");
				}
			}
		}
		
		ast_log(LOG_NOTICE, "=== END INTERRUPTION ===\n");
	}
	/* Handle audio messages */
	else if (strcmp(type_item->valuestring, "audio") == 0) {
		cJSON *audio_event = cJSON_GetObjectItem(json, "audio_event");
		if (audio_event) {
			cJSON *audio_base64_item = cJSON_GetObjectItem(audio_event, "audio_base_64");
			cJSON *event_id_item = cJSON_GetObjectItem(audio_event, "event_id");
			
			if (audio_base64_item && cJSON_IsString(audio_base64_item) &&
			    event_id_item && cJSON_IsNumber(event_id_item)) {
				
				/* Decode base64 audio */
				size_t audio_length;
				unsigned char *audio_data = base64_decode(audio_base64_item->valuestring, &audio_length);
				if (audio_data) {
					ast_log(LOG_NOTICE, "Received audio from ElevenLabs: event_id=%d, base64_size=%zu, raw_size=%zu\n", 
						event_id_item->valueint, strlen(audio_base64_item->valuestring), audio_length);
					
					/* Add to receive queue for playback */
					audio_queue_add(session->receive_queue, audio_data, audio_length, event_id_item->valueint);
					ast_free(audio_data);
				} else {
					ast_log(LOG_ERROR, "Failed to decode base64 audio data\n");
				}
			}
		}
	}
	
	/* Check for other audio message formats from ElevenLabs */
	cJSON *audio_chunk = cJSON_GetObjectItem(json, "audio_chunk");
	if (audio_chunk && cJSON_IsString(audio_chunk)) {
		/* Handle direct audio_chunk format */
		size_t audio_length;
		unsigned char *audio_data = base64_decode(audio_chunk->valuestring, &audio_length);
		if (audio_data) {
			ast_log(LOG_NOTICE, "Received audio_chunk from ElevenLabs: base64_size=%zu, raw_size=%zu\n", 
				strlen(audio_chunk->valuestring), audio_length);
			
			/* Add to receive queue for playback */
			audio_queue_add(session->receive_queue, audio_data, audio_length, session->current_event_id++);
			ast_free(audio_data);
		} else {
			ast_log(LOG_ERROR, "Failed to decode base64 audio_chunk data\n");
		}
	}
	
	cJSON_Delete(json);
}

/*! \brief Add data to message buffer and process when complete */
static int add_to_message_buffer(struct elevenlabs_session *session, const char *data, size_t len, int is_final)
{
	if (!session || !data || len == 0) {
		return -1;
	}
	
	/* Check if session is still active */
	if (!session->session_active) {
		ast_log(LOG_WARNING, "Ignoring message buffer add - session inactive\n");
		return -1;
	}
	
	ast_mutex_lock(&session->message_buffer_lock);
	
	/* Check if buffer has space */
	if (session->message_buffer_size + len > session->message_buffer_capacity) {
		ast_log(LOG_ERROR, "Message buffer overflow, resetting buffer\n");
		session->message_buffer_size = 0;
		ast_mutex_unlock(&session->message_buffer_lock);
		return -1;
	}
	
	/* Add data to buffer */
	memcpy(session->message_buffer + session->message_buffer_size, data, len);
	session->message_buffer_size += len;
	
	ast_log(LOG_NOTICE, "Added %zu bytes to message buffer (total: %zu, final: %s)\n", 
		len, session->message_buffer_size, is_final ? "yes" : "no");
	
	/* If this is the final fragment, process the complete message */
	if (is_final) {
		process_complete_message(session);
	}
	
	ast_mutex_unlock(&session->message_buffer_lock);
	return 0;
}

/*! \brief Process complete reassembled message */
static void process_complete_message(struct elevenlabs_session *session)
{
	char *complete_message;
	
	if (!session || session->message_buffer_size == 0) {
		return;
	}
	
	/* Null-terminate the message */
	session->message_buffer[session->message_buffer_size] = '\0';
	
	/* Create a copy for processing */
	complete_message = ast_strdupa(session->message_buffer);
	
	ast_log(LOG_NOTICE, "Processing complete message: %zu bytes\n", session->message_buffer_size);
	
	/* Reset buffer for next message */
	session->message_buffer_size = 0;
	
	/* Process the complete message */
	handle_websocket_message(session, complete_message);
}

/*! \brief Send conversation initiation message */
static void send_conversation_initiation(struct elevenlabs_session *session)
{
	cJSON *json;
	char *message;
	int connection_ok = 0;
	
	if (!session) {
		return;
	}
	
	/* Check connection state under lock */
	ast_mutex_lock(&session->session_lock);
	connection_ok = (session->session_active && session->ws_connected && session->ws_connection);
	ast_mutex_unlock(&session->session_lock);
	
	if (!connection_ok) {
		ast_log(LOG_DEBUG, "WebSocket not connected, skipping conversation initiation\n");
		return;
	}
	
	/* Create conversation initiation message */
	json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "type", "conversation_initiation_client_data");
	
	/* Add empty dynamic variables */
	cJSON *dynamic_vars = cJSON_CreateObject();
	cJSON_AddItemToObject(json, "dynamic_variables", dynamic_vars);
	
	message = cJSON_Print(json);
	if (message) {
		ast_log(LOG_NOTICE, "Sending conversation initiation message\n");
		if (send_websocket_message(session, message) < 0) {
			ast_log(LOG_WARNING, "Failed to send conversation initiation message\n");
		}
		ast_free(message);
	}
	
	cJSON_Delete(json);
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

/*! \brief Add audio data to send buffer */
static int add_audio_to_send_buffer(struct elevenlabs_session *session, const unsigned char *data, size_t length)
{
	if (!session || !data || length == 0) {
		return -1;
	}
	
	ast_mutex_lock(&session->send_buffer_lock);
	
	/* Check if buffer has enough space */
	if (session->send_buffer_size + length > session->send_buffer_capacity) {
		/* Buffer full - send current buffer first */
		ast_mutex_unlock(&session->send_buffer_lock);
		send_buffered_audio(session);
		ast_mutex_lock(&session->send_buffer_lock);
	}
	
	/* Add data to buffer */
	if (session->send_buffer_size + length <= session->send_buffer_capacity) {
		memcpy(session->send_buffer + session->send_buffer_size, data, length);
		session->send_buffer_size += length;
	}
	
	ast_mutex_unlock(&session->send_buffer_lock);
	return 0;
}

/*! \brief Check if buffer should be sent (based on duration or fullness) */
static int should_send_buffer(struct elevenlabs_session *session)
{
	struct timeval now, diff;
	int elapsed_ms;
	
	if (!session) {
		return 0;
	}
	
	ast_mutex_lock(&session->send_buffer_lock);
	
	/* Check if buffer is full */
	if (session->send_buffer_size >= session->send_buffer_capacity) {
		ast_mutex_unlock(&session->send_buffer_lock);
		return 1;
	}
	
	/* Check if enough time has passed */
	gettimeofday(&now, NULL);
	timersub(&now, &session->last_send_time, &diff);
	elapsed_ms = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
	
	ast_mutex_unlock(&session->send_buffer_lock);
	
	return (elapsed_ms >= session->send_buffer_duration_ms);
}

/*! \brief Send buffered audio to ElevenLabs */
static int send_buffered_audio(struct elevenlabs_session *session)
{
	unsigned char *buffer_copy;
	size_t buffer_size;
	int connection_ok = 0;
	
	if (!session) {
		return -1;
	}
	
	/* Check connection state before proceeding */
	ast_mutex_lock(&session->session_lock);
	connection_ok = (session->session_active && session->ws_connected && session->ws_connection);
	ast_mutex_unlock(&session->session_lock);
	
	if (!connection_ok) {
		ast_log(LOG_DEBUG, "WebSocket not connected, skipping buffered audio send\n");
		return -1;
	}
	
	ast_mutex_lock(&session->send_buffer_lock);
	
	if (session->send_buffer_size == 0) {
		ast_mutex_unlock(&session->send_buffer_lock);
		return 0;
	}
	
	/* Make a copy of the buffer */
	buffer_size = session->send_buffer_size;
	buffer_copy = ast_calloc(1, buffer_size);
	if (!buffer_copy) {
		ast_mutex_unlock(&session->send_buffer_lock);
		return -1;
	}
	
	memcpy(buffer_copy, session->send_buffer, buffer_size);
	
	/* Reset buffer */
	session->send_buffer_size = 0;
	gettimeofday(&session->last_send_time, NULL);
	
	ast_mutex_unlock(&session->send_buffer_lock);
	
	/* Send to ElevenLabs */
	ast_log(LOG_NOTICE, "Sending buffered audio: %zu bytes (%d ms worth of μ-law audio)\n", 
		buffer_size, (int)(buffer_size / 8)); /* 8 bytes per ms at 8kHz μ-law */
	int result = send_audio_to_websocket(session, buffer_copy, buffer_size);
	
	ast_free(buffer_copy);
	return result;
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
		
		i++;
	}
	
	ast_mutex_unlock(&config_lock);
	ast_config_destroy(cfg);
	
	ast_log(LOG_NOTICE, "Loaded %d agent configurations\n", agent_count);
	return 0;
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

static struct ast_cli_entry cli_elevenlabs[] = {
	AST_CLI_DEFINE(handle_elevenlabs_debug, "Enable/disable ElevenLabs debug"),
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
	
	ast_log(LOG_NOTICE, "ElevenLabs ConvAI Proper module loaded successfully\n");
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
	
	ast_log(LOG_NOTICE, "ElevenLabs ConvAI Proper module unloaded\n");
	return res;
}

/*! \brief Module reload function */
static int reload_module(void)
{
	if (load_config() == 0) {
		ast_log(LOG_NOTICE, "ElevenLabs ConvAI Proper configuration reloaded\n");
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