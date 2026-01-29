/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2025, serhii.kravchenko@adelina.solutions
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
 * \brief ElevenLabs Conversational AI dialplan application
 *
 * \author serhii.kravchenko@adelina.solutions
 *
 * \ingroup applications
 */

#include "asterisk.h"

#include "asterisk/app.h"
#include "asterisk/bridge.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/format_cache.h"
#include "asterisk/frame.h"
#include "asterisk/framehook.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/translate.h"
#include "asterisk/utils.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <hiredis/hiredis.h>
#include <libwebsockets.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*** DOCUMENTATION
        <application name="ElevenLabsConvAI" language="en_US">
                <synopsis>
                        Connect channel to ElevenLabs ConvAI agent for real-time
 conversation
                </synopsis>
                <syntax>
                        <parameter name="agent_config" required="true">
                                <para>The agent configuration section name from
 elevenlabs.conf</para>
                        </parameter>
                </syntax>
                <description>
                        <para>This application connects a channel to an
 ElevenLabs ConvAI agent for real-time bidirectional audio streaming. The
 application sets up the connection and returns immediately, with audio
 processing handled asynchronously via framehooks.</para>
                </description>
        </application>
 ***/

static const char app[] = "ElevenLabsConvAI";

/*! \brief Log level enum */
enum elevenlabs_loglevel {
  ELEVENLABS_LOG_NONE = 0,    /*!< No logging */
  ELEVENLABS_LOG_ERROR = 1,   /*!< Only errors */
  ELEVENLABS_LOG_WARNING = 2, /*!< Errors and warnings */
  ELEVENLABS_LOG_NOTICE = 3,  /*!< Errors, warnings, and notices */
  ELEVENLABS_LOG_DEBUG = 4    /*!< All messages including debug */
};

/*! \brief Global log level (default: WARNING) */
static int elevenlabs_loglevel = ELEVENLABS_LOG_WARNING;

/*! \brief Conditional logging macro */
#define ELEVENLABS_LOG(level, ast_level, ...)                                  \
  do {                                                                         \
    if (elevenlabs_loglevel >= (level)) {                                      \
      ast_log(ast_level, __VA_ARGS__);                                         \
    }                                                                          \
  } while (0)

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
  int max_connection_attempts;
  int connection_timeout_seconds;

  /* STT (Speech-to-Text) configuration */
  int enable_stt;
  char stt_websocket_url[512];
  int stt_buffer_duration_ms;
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

/*! \brief Custom SIP header for transfer */
struct custom_header {
  char name[128];
  char value[512];
  struct custom_header *next;
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
  char caller_id[128];
  char called_id[128];

  /* WebSocket connection (ElevenLabs) */
  struct lws_context *ws_context;
  struct lws *ws_connection;
  char ws_host[256];
  char ws_path[512];
  int ws_port;
  int ws_connected;
  char *signed_url;

  /* Audio processing */
  struct audio_queue *receive_queue;

  /* Audio buffering for transmission (8kHz to ElevenLabs) */
  unsigned char *send_buffer;
  size_t send_buffer_size;
  size_t send_buffer_capacity;
  int send_buffer_duration_ms;
  struct timeval last_send_time;
  ast_mutex_t send_buffer_lock;

  /* Threading and lifecycle */
  pthread_t websocket_thread;
  pthread_t playback_thread;
  pthread_t hangup_monitor_thread;
  int websocket_running;
  int websocket_thread_started;
  int playback_running;
  int playback_thread_started;
  int session_active;
  int transfer_in_progress;
  int hangup_monitor_running;
  int hangup_monitor_started;
  int bridge_monitor_running; /* For monitoring when transfer call is bridged */
  pthread_t bridge_monitor_thread;
  int ws_close_requested; /* Flag set by bridge monitor to request WebSocket
                             close */
  int dial_completed;     /* Flag set when Dial thread finishes */
  int dial_result;        /* Result from Dial */
  char dial_status[32];   /* DIALSTATUS from Dial */
  pthread_t dial_thread;
  int dial_thread_running;
  char dial_args[256];              /* Arguments for Dial */
  char dial_number[64];             /* Number being dialed for transfer */
  char dial_reason[256];            /* Reason for transfer */
  struct ast_channel *dial_channel; /* Channel reference for Dial thread */
  int cleanup_started;
  int cleanup_needed; /* Flag for deferred cleanup after websocket thread exits
                       */
  int interruption_pending;  /* Flag to immediately stop current playback */
  int hangup_after_playback; /* Flag to hang up after all buffered audio is
                                played */

  /* Error handling and retry */
  int connection_attempts;
  int max_connection_attempts;
  time_t last_connection_attempt;
  int connection_timeout_seconds;
  int last_error_code;

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

  /* STT (Speech-to-Text) WebSocket connection - 16kHz audio */
  int stt_enabled;
  struct lws_context *stt_ws_context;
  struct lws *stt_ws_connection;
  char stt_ws_host[256];
  char stt_ws_path[512];
  int stt_ws_port;
  int stt_ws_connected;
  int stt_use_ssl;

  /* STT audio translation (8kHz ulaw -> 16kHz slin16) */
  struct ast_trans_pvt *stt_translator;
  struct ast_format *stt_src_format; /* ulaw 8kHz */
  struct ast_format *stt_dst_format; /* slin16 16kHz */

  /* STT audio buffering (16kHz linear) */
  unsigned char *stt_send_buffer;
  size_t stt_send_buffer_size;
  size_t stt_send_buffer_capacity;
  int stt_send_buffer_duration_ms;
  struct timeval stt_last_send_time;
  ast_mutex_t stt_send_buffer_lock;

  /* STT threading */
  pthread_t stt_websocket_thread;
  int stt_websocket_running;
  int stt_connection_attempts;

  /* Redis connection (optional) */
  void *redis_context; /* redisContext*, as void* to avoid hiredis include
                          dependency issues */

  /* ElevenLabs conversation tracking for ingester events */
  char conversation_id[128]; /* Conversation ID from ElevenLabs */
  int started_event_sent;    /* Flag to track if 'started' event was sent */
  int ended_event_sent;      /* Flag to track if 'ended' event was sent */

  /* Custom X-* headers from incoming call for transfer */
  struct custom_header *custom_headers;
};

/*! \brief Global agent configurations */
static struct agent_config *agent_configs = NULL;
static int agent_count = 0;
static ast_mutex_t config_lock = AST_MUTEX_INIT_VALUE;

/*! \brief Global Redis configuration */
static char redis_host[256] = "127.0.0.1";
static int redis_port = 6379;
static int redis_db = 0;
static char redis_password[256] = "";
static int redis_enabled = 0;
static char redis_ingester_queue[128] =
    "ingester:events"; /* Queue for ingester events */

/*! \brief Forward declarations */
static int elevenlabs_exec(struct ast_channel *chan, const char *data);
static int load_config(void);
static struct agent_config *find_agent_config(const char *name);

/* HTTP and WebSocket functions */
static char *get_signed_url(struct agent_config *config);
static int parse_signed_url(const char *signed_url, char *host, char *path,
                            int *port);

/* Session management */
static struct elevenlabs_session *create_session(struct ast_channel *chan,
                                                 struct agent_config *config);
static void destroy_session(struct elevenlabs_session *session);
static void session_cleanup_cb(void *data);
static void capture_custom_headers(struct elevenlabs_session *session,
                                   struct ast_channel *chan);
static void free_custom_headers(struct custom_header *headers);

/* Redis connection management */
static int connect_redis(struct elevenlabs_session *session);
static void disconnect_redis(struct elevenlabs_session *session);

/* Ingester event publishing */
static int publish_ingester_event(struct elevenlabs_session *session,
                                  const char *event_type,
                                  const char *payload_json);

/* Audio processing */
static struct audio_queue *create_audio_queue(void);
static void destroy_audio_queue(struct audio_queue *queue);
static int audio_queue_add(struct audio_queue *queue, const unsigned char *data,
                           size_t length, int event_id);
static struct audio_queue_item *audio_queue_get(struct audio_queue *queue);

/* Framehook */
static struct ast_frame *framehook_callback(struct ast_channel *chan,
                                            struct ast_frame *frame,
                                            enum ast_framehook_event event,
                                            void *data);
static int attach_framehook(struct elevenlabs_session *session);
static void detach_framehook(struct elevenlabs_session *session);

/* Threading */
static void *websocket_thread_func(void *arg);
static void *playback_thread_func(void *arg);
static void *hangup_monitor_thread_func(void *arg);

/* WebSocket protocol */
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len);
static int send_websocket_message(struct elevenlabs_session *session,
                                  const char *message);
static void handle_websocket_message(struct elevenlabs_session *session,
                                     const char *message);
static void handle_client_tool_call(struct elevenlabs_session *session,
                                    cJSON *tool_call);
static void quiesce_session_for_transfer(struct elevenlabs_session *session,
                                         pthread_t current_thread);
static void send_tool_result(struct elevenlabs_session *session,
                             const char *tool_call_id, int success,
                             const char *detail);
static void fail_call_with_error(struct elevenlabs_session *session,
                                 const char *tool_call_id, const char *reason);
static void send_conversation_initiation(struct elevenlabs_session *session);
static int send_audio_to_websocket(struct elevenlabs_session *session,
                                   const unsigned char *audio_data,
                                   size_t length);
static int add_to_message_buffer(struct elevenlabs_session *session,
                                 const char *data, size_t len, int is_final);
static void process_complete_message(struct elevenlabs_session *session);

/* Error handling and recovery */
static int establish_websocket_connection(struct elevenlabs_session *session);
static int should_retry_connection(struct elevenlabs_session *session);
static void handle_connection_failure(struct elevenlabs_session *session,
                                      const char *error_msg);
static const char *get_lws_error_string(int error_code);

/* Base64 encoding */
static char *base64_encode(const unsigned char *data, size_t input_length);
static unsigned char *base64_decode(const char *data, size_t *output_length);

/* Audio buffer management */
static int add_audio_to_send_buffer(struct elevenlabs_session *session,
                                    const unsigned char *data, size_t length);
static int should_send_buffer(struct elevenlabs_session *session);
static int send_buffered_audio(struct elevenlabs_session *session);

/* STT (Speech-to-Text) functions */
static int init_stt_translator(struct elevenlabs_session *session);
static void destroy_stt_translator(struct elevenlabs_session *session);
static int parse_stt_websocket_url(const char *url, char *host, char *path,
                                   int *port, int *use_ssl);
static int
establish_stt_websocket_connection(struct elevenlabs_session *session);
static void *stt_websocket_thread_func(void *arg);
static int stt_websocket_callback(struct lws *wsi,
                                  enum lws_callback_reasons reason, void *user,
                                  void *in, size_t len);
static int add_audio_to_stt_buffer(struct elevenlabs_session *session,
                                   const unsigned char *data, size_t length);
static int should_send_stt_buffer(struct elevenlabs_session *session);
static int send_stt_buffered_audio(struct elevenlabs_session *session);
static int translate_and_buffer_for_stt(struct elevenlabs_session *session,
                                        struct ast_frame *frame);
static void handle_stt_transcription(struct elevenlabs_session *session,
                                     const char *message);
static int
send_contextual_update_to_elevenlabs(struct elevenlabs_session *session,
                                     const char *text);

/* WebSocket protocol structure for ElevenLabs */
static struct lws_protocols protocols[] = {
    {
        "convai",
        websocket_callback,
        0,
        4096,
    },
    {NULL, NULL, 0, 0} /* terminator */
};

/* WebSocket protocol structure for STT */
static struct lws_protocols stt_protocols[] = {
    {
        "stt", stt_websocket_callback, 0,
        16384, /* Larger buffer for 16kHz audio */
    },
    {NULL, NULL, 0, 0} /* terminator */
};

/*! \brief HTTP response callback */
static size_t http_response_callback(void *contents, size_t size, size_t nmemb,
                                     struct http_response *response) {
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
static char *get_signed_url(struct agent_config *config) {
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

  snprintf(url, sizeof(url), "%s?agent_id=%s", config->endpoint_url,
           config->agent_id);
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
          if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                           "Got signed URL: %s\n", signed_url);
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
static struct agent_config *find_agent_config(const char *name) {
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

/*! \brief Connect to Redis server (optional, non-blocking) */
static int connect_redis(struct elevenlabs_session *session) {
  struct timeval timeout = {5, 0}; /* 5 seconds timeout */
  redisContext *redis = NULL;
  redisReply *reply = NULL;

  if (!session) {
    return -1;
  }

  /* Skip if Redis is disabled */
  if (!redis_enabled) {
    return 0;
  }

  /* Disconnect existing connection if any */
  if (session->redis_context) {
    redisFree((redisContext *)session->redis_context);
    session->redis_context = NULL;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Connecting to Redis at %s:%d (db=%d)\n", redis_host,
                 redis_port, redis_db);

  /* Connect to Redis with timeout */
  redis = redisConnectWithTimeout(redis_host, redis_port, timeout);
  if (!redis || redis->err) {
    if (redis) {
      ast_log(LOG_WARNING, "Redis connection error: %s\n", redis->errstr);
      redisFree(redis);
    } else {
      ast_log(LOG_WARNING, "Redis connection error: cannot allocate context\n");
    }
    session->redis_context = NULL;
    return -1;
  }

  /* Authenticate if password is set */
  if (!ast_strlen_zero(redis_password)) {
    reply = redisCommand(redis, "AUTH %s", redis_password);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      ast_log(LOG_WARNING, "Redis authentication failed: %s\n",
              reply ? reply->str : "no reply");
      if (reply) {
        freeReplyObject(reply);
      }
      redisFree(redis);
      session->redis_context = NULL;
      return -1;
    }
    freeReplyObject(reply);
  }

  /* Select database */
  if (redis_db != 0) {
    reply = redisCommand(redis, "SELECT %d", redis_db);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      ast_log(LOG_WARNING, "Redis SELECT failed: %s\n",
              reply ? reply->str : "no reply");
      if (reply) {
        freeReplyObject(reply);
      }
      redisFree(redis);
      session->redis_context = NULL;
      return -1;
    }
    freeReplyObject(reply);
  }

  session->redis_context = redis;

  /* Test: Send a connection message with caller ID to verify connection works
   */
  char test_msg[256];
  snprintf(test_msg, sizeof(test_msg),
           "{\"event\":\"test\",\"caller_id\":\"unknown\"}");
  reply = redisCommand(redis, "PUBLISH elevenlabs:test %s", test_msg);
  if (reply) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Redis test message published, subscribers: %lld\n",
                   reply->integer);
    freeReplyObject(reply);
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Successfully connected to Redis\n");
  return 0;
}

/*! \brief Disconnect from Redis server (safe) */
static void disconnect_redis(struct elevenlabs_session *session) {
  if (!session) {
    return;
  }

  if (session->redis_context) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Disconnecting from Redis\n");
    redisFree((redisContext *)session->redis_context);
    session->redis_context = NULL;
  }
}

/*! \brief Get current UTC timestamp in ISO-8601 format */
static void get_utc_timestamp(char *buffer, size_t size) {
  time_t now;
  struct tm tm_utc;

  time(&now);
  gmtime_r(&now, &tm_utc);
  strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/*! \brief Publish ingester event to Redis queue
 *
 * Publishes events according to the Ingester Service specification:
 * - event: "started", "transcript_chunk", "ended"
 * - timestamp: UTC ISO-8601 format
 * - elevenlabs_conversation_id: Session ID from ElevenLabs
 * - elevenlabs_agent_id: Agent ID from configuration
 * - asterisk_meta: Caller ID and Asterisk unique ID
 * - payload: Event-specific data
 */
static int publish_ingester_event(struct elevenlabs_session *session,
                                  const char *event_type,
                                  const char *payload_json) {
  redisContext *redis;
  redisReply *reply;
  cJSON *json;
  cJSON *asterisk_meta;
  cJSON *payload;
  char *message;
  char timestamp[64];
  const char *uniqueid = NULL;

  if (!session || !event_type) {
    return -1;
  }

  /* Check if Redis is enabled and connected */
  if (!redis_enabled || !session->redis_context) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Ingester: Skipping '%s' event - Redis %s\n", event_type,
                   !redis_enabled ? "not enabled" : "not connected");
    return 0;
  }

  redis = (redisContext *)session->redis_context;

  /* Get current UTC timestamp */
  get_utc_timestamp(timestamp, sizeof(timestamp));

  /* Build JSON message according to Ingester Service spec */
  json = cJSON_CreateObject();
  if (!json) {
    ast_log(LOG_ERROR, "Failed to create JSON object for ingester event\n");
    return -1;
  }

  /* Required fields */
  cJSON_AddStringToObject(json, "event", event_type);
  cJSON_AddStringToObject(json, "timestamp", timestamp);

  /* Use conversation_id if available, otherwise generate from Asterisk uniqueid
   */
  if (!ast_strlen_zero(session->conversation_id)) {
    cJSON_AddStringToObject(json, "elevenlabs_conversation_id",
                            session->conversation_id);
  } else if (session->channel) {
    uniqueid = ast_channel_uniqueid(session->channel);
    if (!ast_strlen_zero(uniqueid)) {
      cJSON_AddStringToObject(json, "elevenlabs_conversation_id", uniqueid);
    } else {
      cJSON_AddStringToObject(json, "elevenlabs_conversation_id", "unknown");
    }
  } else {
    cJSON_AddStringToObject(json, "elevenlabs_conversation_id", "unknown");
  }

  /* Agent ID from configuration */
  if (session->config && !ast_strlen_zero(session->config->agent_id)) {
    cJSON_AddStringToObject(json, "elevenlabs_agent_id",
                            session->config->agent_id);
  } else {
    cJSON_AddStringToObject(json, "elevenlabs_agent_id", "unknown");
  }

  /* Asterisk metadata (optional but recommended) */
  asterisk_meta = cJSON_CreateObject();
  if (asterisk_meta) {
    if (!ast_strlen_zero(session->caller_id)) {
      cJSON_AddStringToObject(asterisk_meta, "caller_id", session->caller_id);
    }
    if (session->channel) {
      uniqueid = ast_channel_uniqueid(session->channel);
      if (!ast_strlen_zero(uniqueid)) {
        cJSON_AddStringToObject(asterisk_meta, "unique_id", uniqueid);
      }
    }
    cJSON_AddItemToObject(json, "asterisk_meta", asterisk_meta);
  }

  /* Payload - parse from JSON string if provided, otherwise create empty object
   */
  if (payload_json && !ast_strlen_zero(payload_json)) {
    payload = cJSON_Parse(payload_json);
    if (payload) {
      cJSON_AddItemToObject(json, "payload", payload);
    } else {
      ast_log(LOG_WARNING, "Failed to parse payload JSON: %s\n", payload_json);
      cJSON_AddObjectToObject(json, "payload");
    }
  } else {
    cJSON_AddObjectToObject(json, "payload");
  }

  /* Serialize JSON */
  message = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);

  if (!message) {
    ast_log(LOG_ERROR, "Failed to serialize ingester event JSON\n");
    return -1;
  }

  /* 1. Push to Redis list (queue) for reliable processing */
  reply = redisCommand(redis, "RPUSH %s %s", redis_ingester_queue, message);

  if (!reply) {
    ast_log(LOG_ERROR, "Failed to RPUSH ingester event to Redis: %s\n",
            redis->errstr);
    ast_std_free(message);
    return -1;
  }

  if (reply->type == REDIS_REPLY_ERROR) {
    ast_log(LOG_ERROR, "Redis RPUSH error: %s\n", reply->str);
    freeReplyObject(reply);
    ast_std_free(message);
    return -1;
  }
  freeReplyObject(reply);

  /* 2. Also PUBLISH for real-time subscribers (monitoring) */
  reply = redisCommand(redis, "PUBLISH %s %s", redis_ingester_queue, message);

  if (reply) {
    if (reply->type == REDIS_REPLY_ERROR) {
      ast_log(LOG_WARNING, "Redis PUBLISH error (non-fatal): %s\n", reply->str);
    }
    freeReplyObject(reply);
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Ingester: Published '%s' event to Redis queue '%s'\n",
                 event_type, redis_ingester_queue);

  ast_std_free(message); /* cJSON uses standard malloc */
  return 0;
}

/*! \brief Publish 'started' event to ingester */
static int publish_started_event(struct elevenlabs_session *session) {
  if (!session) {
    return -1;
  }

  /* Avoid sending duplicate started events */
  if (session->started_event_sent) {
    return 0;
  }

  session->started_event_sent = 1;
  return publish_ingester_event(session, "started", "{\"type\":\"call\"}");
}

/*! \brief Publish 'transcript_chunk' event to ingester */
static int publish_transcript_chunk(struct elevenlabs_session *session,
                                    const char *text, const char *speaker,
                                    int is_final) {
  cJSON *payload;
  char *payload_str;
  int result;

  if (!session || !text || !speaker) {
    return -1;
  }

  /* Build payload JSON using cJSON for proper escaping */
  payload = cJSON_CreateObject();
  if (!payload) {
    ast_log(LOG_ERROR, "Failed to create payload JSON for transcript_chunk\n");
    return -1;
  }

  cJSON_AddStringToObject(payload, "text", text);
  cJSON_AddStringToObject(payload, "speaker", speaker);
  cJSON_AddBoolToObject(payload, "is_final", is_final ? 1 : 0);

  payload_str = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);

  if (!payload_str) {
    ast_log(LOG_ERROR, "Failed to serialize transcript_chunk payload\n");
    return -1;
  }

  result = publish_ingester_event(session, "transcript_chunk", payload_str);
  ast_std_free(payload_str); /* cJSON uses standard malloc */

  return result;
}

/*! \brief Publish 'ended' event to ingester */
static int publish_ended_event(struct elevenlabs_session *session) {
  if (!session) {
    return -1;
  }

  /* Avoid sending duplicate ended events */
  if (session->ended_event_sent) {
    return 0;
  }

  session->ended_event_sent = 1;
  return publish_ingester_event(session, "ended", "{}");
}

/*! \brief Main application function - BLOCKING until session ends */
static int elevenlabs_exec(struct ast_channel *chan, const char *data) {
  char *agent_name;
  struct agent_config *config;
  struct elevenlabs_session *session = NULL;

  ELEVENLABS_LOG(
      ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
      "ElevenLabsConvAI: Starting session with agent '%s' on channel %s\n",
      data ? data : "(null)", ast_channel_name(chan));

  if (ast_strlen_zero(data)) {
    ast_log(LOG_ERROR,
            "ElevenLabsConvAI requires an agent configuration name\n");
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
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Forcing channel to μ-law format\n");

    int read_result = ast_set_read_format(chan, ulaw_format);
    int write_result = ast_set_write_format(chan, ulaw_format);

    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Format forcing results: read=%d, write=%d\n", read_result,
                   write_result);

    if (read_result != 0) {
      ast_log(LOG_ERROR, "Failed to set read format to μ-law (error: %d)\n",
              read_result);
    }
    if (write_result != 0) {
      ast_log(LOG_ERROR, "Failed to set write format to μ-law (error: %d)\n",
              write_result);
    }

    /* Verify the format was actually set */
    struct ast_format *current_read = ast_channel_readformat(chan);
    struct ast_format *current_write = ast_channel_writeformat(chan);

    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Current formats after forcing: read=%s, write=%s\n",
                   ast_format_get_name(current_read),
                   ast_format_get_name(current_write));

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

  ast_verb(2,
           "ElevenLabsConvAI: Session started for agent '%s' - processing in "
           "background\n",
           agent_name);

  /*
   * Return immediately - this is the proper Asterisk way.
   * The framehook will handle all audio processing and session management.
   * Session cleanup will happen automatically when the channel hangs up.
   */
  return 0;
}

/*! \brief Create and initialize session */
static struct elevenlabs_session *create_session(struct ast_channel *chan,
                                                 struct agent_config *config) {
  struct elevenlabs_session *session;
  char *signed_url;

  /* Allocate session */
  session = ast_calloc(1, sizeof(struct elevenlabs_session));
  if (!session) {
    ast_log(LOG_ERROR, "Failed to allocate session memory\n");
    return NULL;
  }

  /* Initialize basic fields */
  session->channel = ast_channel_ref(
      chan); /* Take a reference to prevent channel destruction */
  session->config = config;
  ast_copy_string(session->channel_name, ast_channel_name(chan),
                  sizeof(session->channel_name));

  /* Extract caller ID */
  const char *caller_id = ast_channel_caller(chan)->id.number.str;
  if (caller_id && !ast_strlen_zero(caller_id)) {
    ast_copy_string(session->caller_id, caller_id, sizeof(session->caller_id));
  } else {
    ast_copy_string(session->caller_id, "Unknown", sizeof(session->caller_id));
  }
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE, "Caller ID: %s\n",
                 session->caller_id);

  /* Extract called ID (DNID - Dialed Number Identification) */
  char dnid_buf[128] = "";
  if (ast_func_read(chan, "CALLERID(dnid)", dnid_buf, sizeof(dnid_buf)) == 0 &&
      !ast_strlen_zero(dnid_buf)) {
    ast_copy_string(session->called_id, dnid_buf, sizeof(session->called_id));
  } else {
    ast_copy_string(session->called_id, "Unknown", sizeof(session->called_id));
  }
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE, "Called ID (DNID): %s\n",
                 session->called_id);

  /* Capture X-* headers from incoming call for transfer */
  session->custom_headers = NULL;
  capture_custom_headers(session, chan);

  session->framehook_id = -1;
  session->session_active = 1;
  session->transfer_in_progress = 0;
  session->hangup_monitor_running = 0;
  session->bridge_monitor_running = 0;
  session->ws_close_requested = 0;
  session->dial_completed = 0;
  session->dial_result = 0;
  session->dial_status[0] = '\0';
  session->dial_args[0] = '\0';
  session->dial_number[0] = '\0';
  session->dial_reason[0] = '\0';
  session->dial_channel = NULL;
  session->dial_thread_running = 0;
  session->cleanup_started = 0;
  session->cleanup_needed = 0;
  session->websocket_thread_started = 0;
  session->playback_thread_started = 0;
  session->hangup_monitor_started = 0;
  session->interruption_pending = 0;
  session->hangup_after_playback = 0;
  session->current_event_id = 1;
  session->read_frame_count = 0;
  session->write_frame_count = 0;
  session->redis_context = NULL; /* Initialize Redis context */
  session->conversation_id[0] =
      '\0'; /* Will be set when we receive conversation_initiation_metadata */
  session->started_event_sent = 0;
  session->ended_event_sent = 0;

  /* Initialize error handling from config */
  session->connection_attempts = 0;
  session->max_connection_attempts = config->max_connection_attempts;
  session->last_connection_attempt = 0;
  session->connection_timeout_seconds = config->connection_timeout_seconds;
  session->last_error_code = 0;

  ast_mutex_init(&session->session_lock);

  /* Initialize message buffer for WebSocket reassembly */
  session->message_buffer_capacity =
      1024 * 1024; /* 1MB buffer for large audio messages */
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
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Getting signed URL for agent '%s'\n", config->name);
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
  if (pthread_create(&session->websocket_thread, NULL, websocket_thread_func,
                     session) != 0) {
    ast_log(LOG_ERROR, "Failed to create WebSocket thread\n");
    destroy_session(session);
    return NULL;
  }
  session->websocket_thread_started = 1;

  /* Start playback thread */
  session->playback_running = 1;
  if (pthread_create(&session->playback_thread, NULL, playback_thread_func,
                     session) != 0) {
    ast_log(LOG_ERROR, "Failed to create playback thread\n");
    session->websocket_running = 0;
    pthread_join(session->websocket_thread, NULL);
    destroy_session(session);
    return NULL;
  }
  session->playback_thread_started = 1;

  /* Start hangup monitor thread to immediately react to caller hangup */
  session->hangup_monitor_running = 1;
  if (pthread_create(&session->hangup_monitor_thread, NULL,
                     hangup_monitor_thread_func, session) != 0) {
    ast_log(LOG_ERROR, "Failed to create hangup monitor thread\n");
    session->hangup_monitor_running = 0;
    /* Cleanup to avoid leaking resources if we cannot monitor hangups */
    destroy_session(session);
    return NULL;
  }
  session->hangup_monitor_started = 1;

  /* Initialize STT if enabled */
  if (config->enable_stt && !ast_strlen_zero(config->stt_websocket_url)) {
    session->stt_enabled = 1;

    /* Initialize STT translator (8kHz ulaw -> 16kHz slin16) */
    if (init_stt_translator(session) != 0) {
      ast_log(LOG_WARNING,
              "Failed to initialize STT translator - STT disabled\n");
      session->stt_enabled = 0;
    } else {
      /* Initialize STT send buffer */
      session->stt_send_buffer_duration_ms =
          config->stt_buffer_duration_ms > 0 ? config->stt_buffer_duration_ms
                                             : config->buffer_duration_ms;
      /* 16kHz * 2 bytes (16-bit) = 32 bytes per ms */
      session->stt_send_buffer_capacity =
          session->stt_send_buffer_duration_ms * 32;
      session->stt_send_buffer =
          ast_calloc(1, session->stt_send_buffer_capacity);
      if (!session->stt_send_buffer) {
        ast_log(LOG_WARNING,
                "Failed to allocate STT send buffer - STT disabled\n");
        destroy_stt_translator(session);
        session->stt_enabled = 0;
      } else {
        session->stt_send_buffer_size = 0;
        gettimeofday(&session->stt_last_send_time, NULL);
        ast_mutex_init(&session->stt_send_buffer_lock);

        /* Parse STT WebSocket URL */
        if (parse_stt_websocket_url(config->stt_websocket_url,
                                    session->stt_ws_host, session->stt_ws_path,
                                    &session->stt_ws_port,
                                    &session->stt_use_ssl) != 0) {
          ast_log(LOG_WARNING,
                  "Failed to parse STT WebSocket URL - STT disabled\n");
          ast_free(session->stt_send_buffer);
          session->stt_send_buffer = NULL;
          destroy_stt_translator(session);
          session->stt_enabled = 0;
        } else {
          /* Start STT WebSocket thread */
          session->stt_websocket_running = 1;
          session->stt_connection_attempts = 0;
          if (pthread_create(&session->stt_websocket_thread, NULL,
                             stt_websocket_thread_func, session) != 0) {
            ast_log(LOG_WARNING,
                    "Failed to create STT WebSocket thread - STT disabled\n");
            ast_free(session->stt_send_buffer);
            session->stt_send_buffer = NULL;
            ast_mutex_destroy(&session->stt_send_buffer_lock);
            destroy_stt_translator(session);
            session->stt_enabled = 0;
          } else {
            ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                           "STT enabled: 16kHz audio will be sent to %s\n",
                           config->stt_websocket_url);
          }
        }
      }
    }
  } else {
    session->stt_enabled = 0;
  }

  /* Connect to Redis if enabled (non-blocking, errors ignored) */
  if (redis_enabled) {
    if (connect_redis(session) != 0) {
      ast_log(LOG_WARNING,
              "Failed to connect to Redis - continuing without it\n");
    }
  }

  ELEVENLABS_LOG(
      ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
      "ElevenLabs session created successfully (STT: %s, Redis: %s)\n",
      session->stt_enabled ? "enabled" : "disabled",
      session->redis_context ? "connected" : "disabled");
  return session;
}

/*! \brief Session cleanup callback - called when channel hangs up */
static void session_cleanup_cb(void *data) {
  struct elevenlabs_session *session = data;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "=== SESSION CLEANUP START ===\n");

  if (!session) {
    ast_log(LOG_ERROR, "Session cleanup called with NULL session\n");
    return;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Cleaning up ElevenLabs session for channel %s\n",
                 session->channel_name);

  /* Add session state logging */
  ast_mutex_lock(&session->session_lock);
  ELEVENLABS_LOG(
      ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
      "Session state: active=%d, ws_connected=%d, websocket_running=%d, "
      "playback_running=%d, transfer_in_progress=%d\n",
      session->session_active, session->ws_connected,
      session->websocket_running, session->playback_running,
      session->transfer_in_progress);

  /* Skip cleanup if transfer is in progress - it will be done after Dial
   * completes */
  if (session->transfer_in_progress) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
        "Transfer in progress - skipping cleanup, will be done after Dial\n");
    ast_mutex_unlock(&session->session_lock);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "=== SESSION CLEANUP END (skipped) ===\n");
    return;
  }
  ast_mutex_unlock(&session->session_lock);

  destroy_session(session);
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "=== SESSION CLEANUP END ===\n");
}

/*! \brief Destroy session and cleanup resources */
static void destroy_session(struct elevenlabs_session *session) {
  if (!session) {
    ast_log(LOG_ERROR, "destroy_session called with NULL session\n");
    return;
  }

  /* Prevent double cleanup */
  ast_mutex_lock(&session->session_lock);
  if (session->cleanup_started) {
    ast_mutex_unlock(&session->session_lock);
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
        "destroy_session already in progress - skipping duplicate call\n");
    return;
  }
  session->cleanup_started = 1;
  ast_mutex_unlock(&session->session_lock);

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "=== DESTROY SESSION START ===\n");

  /* Mark session as inactive first */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 1: Marking session as inactive\n");
  ast_mutex_lock(&session->session_lock);
  session->session_active = 0;
  session->ws_connected = 0;
  ast_mutex_unlock(&session->session_lock);

  /* Stop hangup monitor thread early to avoid races */
  ELEVENLABS_LOG(
      ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
      "Step 1a: Stopping hangup monitor thread (started=%d, running=%d)\n",
      session->hangup_monitor_started, session->hangup_monitor_running);
  session->hangup_monitor_running = 0;
  if (session->hangup_monitor_started &&
      !pthread_equal(pthread_self(), session->hangup_monitor_thread)) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Waiting for hangup monitor thread to stop\n");
    pthread_join(session->hangup_monitor_thread, NULL);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Hangup monitor thread stopped\n");
  }

  /* Stop WebSocket thread */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 2: Stopping WebSocket thread (started=%d, running=%d)\n",
                 session->websocket_thread_started, session->websocket_running);
  session->websocket_running = 0;
  if (session->websocket_thread_started &&
      !pthread_equal(pthread_self(), session->websocket_thread)) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Waiting for WebSocket thread to stop\n");
    pthread_join(session->websocket_thread, NULL);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "WebSocket thread stopped\n");
  }

  /* Stop playback thread */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 3: Stopping playback thread (started=%d, running=%d)\n",
                 session->playback_thread_started, session->playback_running);
  session->playback_running = 0;
  if (session->receive_queue) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Signaling playback thread condition\n");
    ast_cond_signal(&session->receive_queue->cond);
  }
  if (session->playback_thread_started &&
      !pthread_equal(pthread_self(), session->playback_thread)) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Waiting for playback thread to stop\n");
    pthread_join(session->playback_thread, NULL);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Playback thread stopped\n");
  }

  /* Stop STT WebSocket thread */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 3a: Stopping STT WebSocket thread (running=%d)\n",
                 session->stt_websocket_running);
  if (session->stt_websocket_running) {
    session->stt_websocket_running = 0;
    session->stt_ws_connected = 0;
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Waiting for STT WebSocket thread to stop\n");
    pthread_join(session->stt_websocket_thread, NULL);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "STT WebSocket thread stopped\n");
  }

  /* Detach framehook */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 4: Detaching framehook (ID=%d)\n",
                 session->framehook_id);
  detach_framehook(session);

  /* Cleanup WebSocket safely */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 5: Cleaning up WebSocket connection\n");
  ast_mutex_lock(&session->session_lock);
  if (session->ws_connection) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Closing WebSocket connection\n");
    lws_close_reason(session->ws_connection, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
    session->ws_connection = NULL;
    session->ws_connected = 0;
    /* Small delay to allow WebSocket cleanup to complete */
    ast_mutex_unlock(&session->session_lock);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Sleeping 100ms for WebSocket cleanup\n");
    usleep(100000); /* 100ms delay to allow cleanup */
  } else {
    session->ws_connected = 0;
    ast_mutex_unlock(&session->session_lock);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "WebSocket connection already NULL\n");
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 6: Destroying WebSocket context\n");
  if (session->ws_context) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Destroying WebSocket context\n");
    lws_context_destroy(session->ws_context);
    session->ws_context = NULL;
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "WebSocket context destroyed\n");
  } else {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "WebSocket context already NULL\n");
  }

  /* Cleanup STT WebSocket context */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 6a: Destroying STT WebSocket context\n");
  if (session->stt_ws_context) {
    if (session->stt_ws_connection) {
      lws_close_reason(session->stt_ws_connection, LWS_CLOSE_STATUS_NORMAL,
                       NULL, 0);
      session->stt_ws_connection = NULL;
    }
    lws_context_destroy(session->stt_ws_context);
    session->stt_ws_context = NULL;
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "STT WebSocket context destroyed\n");
  }

  /* Cleanup STT translator */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 6b: Destroying STT translator\n");
  destroy_stt_translator(session);

  /* Disconnect from Redis */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 6c: Disconnecting from Redis\n");
  disconnect_redis(session);

  /* Cleanup audio queue */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 7: Cleaning up audio queue\n");
  if (session->receive_queue) {
    destroy_audio_queue(session->receive_queue);
    session->receive_queue = NULL;
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Audio queue destroyed\n");
  }

  /* Free signed URL */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 8: Freeing signed URL\n");
  if (session->signed_url) {
    ast_free(session->signed_url);
    session->signed_url = NULL;
  }

  /* Cleanup send buffer */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 9: Cleaning up send buffer\n");
  if (session->send_buffer) {
    ast_free(session->send_buffer);
    session->send_buffer = NULL;
  }

  /* Cleanup STT send buffer */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 9a: Cleaning up STT send buffer\n");
  if (session->stt_send_buffer) {
    ast_free(session->stt_send_buffer);
    session->stt_send_buffer = NULL;
    ast_mutex_destroy(&session->stt_send_buffer_lock);
  }

  /* Free custom headers */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 9b: Freeing custom headers\n");
  if (session->custom_headers) {
    free_custom_headers(session->custom_headers);
    session->custom_headers = NULL;
  }

  /* Cleanup message buffer */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 10: Cleaning up message buffer\n");
  if (session->message_buffer) {
    ast_free(session->message_buffer);
    session->message_buffer = NULL;
  }

  /* Release channel reference */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 11: Releasing channel reference\n");
  if (session->channel) {
    ast_channel_unref(session->channel);
    session->channel = NULL;
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Channel reference released\n");
  }

  /* Destroy mutexes */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 12: Destroying mutexes\n");
  ast_mutex_destroy(&session->message_buffer_lock);
  ast_mutex_destroy(&session->send_buffer_lock);
  ast_mutex_destroy(&session->session_lock);

  /* Free session */
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Step 13: Freeing session structure\n");
  ast_free(session);

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "=== DESTROY SESSION END ===\n");
}

/*! \brief Free custom headers linked list */
static void free_custom_headers(struct custom_header *headers) {
  struct custom_header *current = headers;
  struct custom_header *next;

  while (current) {
    next = current->next;
    ast_free(current);
    current = next;
  }
}

/*! \brief Capture X-* SIP headers from incoming channel for transfer */
static void capture_custom_headers(struct elevenlabs_session *session,
                                   struct ast_channel *chan) {
  char header_list[2048] = "";
  char header_value[512] = "";
  char func_name[256];
  struct custom_header *head = NULL;
  struct custom_header *tail = NULL;
  char *header_name;
  char *saveptr;
  int count = 0;

  if (!session || !chan) {
    return;
  }

  /* Get list of all X-* headers using PJSIP_HEADERS(X-) */
  snprintf(func_name, sizeof(func_name), "PJSIP_HEADERS(X-)");
  if (ast_func_read(chan, func_name, header_list, sizeof(header_list)) != 0) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                   "No X-* headers found or PJSIP_HEADERS not available\n");
    return;
  }

  if (ast_strlen_zero(header_list)) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                   "No X-* headers in incoming call\n");
    return;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "Found X-* headers: %s\n",
                 header_list);

  /* Parse comma-separated header names and read each value */
  header_name = strtok_r(header_list, ",", &saveptr);
  while (header_name) {
    struct custom_header *hdr;

    /* Skip leading/trailing whitespace */
    while (*header_name == ' ') {
      header_name++;
    }
    char *end = header_name + strlen(header_name) - 1;
    while (end > header_name && *end == ' ') {
      *end-- = '\0';
    }

    if (ast_strlen_zero(header_name)) {
      header_name = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    /* Read header value */
    snprintf(func_name, sizeof(func_name), "PJSIP_HEADER(read,%s)",
             header_name);
    header_value[0] = '\0';
    if (ast_func_read(chan, func_name, header_value, sizeof(header_value)) !=
            0 ||
        ast_strlen_zero(header_value)) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Could not read value for header %s\n", header_name);
      header_name = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    /* Allocate and populate header structure */
    hdr = ast_calloc(1, sizeof(struct custom_header));
    if (!hdr) {
      ast_log(LOG_ERROR, "Failed to allocate memory for custom header\n");
      header_name = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    ast_copy_string(hdr->name, header_name, sizeof(hdr->name));
    ast_copy_string(hdr->value, header_value, sizeof(hdr->value));
    hdr->next = NULL;

    /* Add to linked list */
    if (!head) {
      head = hdr;
      tail = hdr;
    } else {
      tail->next = hdr;
      tail = hdr;
    }
    count++;

    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                   "Captured header: %s = %s\n", header_name, header_value);

    header_name = strtok_r(NULL, ",", &saveptr);
  }

  session->custom_headers = head;
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Captured %d custom X-* headers for transfer\n", count);
}

/*! \brief Framehook callback - processes audio frames in real-time */
static struct ast_frame *framehook_callback(struct ast_channel *chan,
                                            struct ast_frame *frame,
                                            enum ast_framehook_event event,
                                            void *data) {
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
      int should_process_stt = 0;

      /* Check connection state under lock */
      ast_mutex_lock(&session->session_lock);
      should_process = (session->session_active && session->ws_connected);
      should_process_stt = (session->session_active && session->stt_enabled &&
                            session->stt_ws_connected);
      ast_mutex_unlock(&session->session_lock);

      if (should_process) {
        /* Log frame details only in debug mode */
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          const char *format_name = ast_format_get_name(frame->subclass.format);
          session->read_frame_count++;
          ELEVENLABS_LOG(
              ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
              "Framehook READ #%d: %s frame - %d bytes, %d samples\n",
              session->read_frame_count, format_name, frame->datalen,
              frame->samples);
        }

        add_audio_to_send_buffer(session, frame->data.ptr, frame->datalen);
        if (should_send_buffer(session)) {
          send_buffered_audio(session);
        }
      }

      /* Also send 16kHz audio to STT if enabled */
      if (should_process_stt) {
        translate_and_buffer_for_stt(session, frame);
        if (should_send_stt_buffer(session)) {
          send_stt_buffered_audio(session);
        }
      }
    }
    break;

  case AST_FRAMEHOOK_EVENT_WRITE:
    /* Outgoing audio to caller - normal processing for now */
    if (frame->data.ptr && frame->datalen > 0) {
      if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
        const char *format_name = ast_format_get_name(frame->subclass.format);
        session->write_frame_count++;
        ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                       "Framehook WRITE #%d: %s frame - %d bytes, %d samples\n",
                       session->write_frame_count, format_name, frame->datalen,
                       frame->samples);
      }
    }
    /* Note: ElevenLabs audio is injected separately via ast_write in playback
     * thread */
    break;

  default:
    break;
  }

  return frame;
}

/*! \brief Attach framehook to channel */
static int attach_framehook(struct elevenlabs_session *session) {
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

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Framehook attached (ID: %d)\n", session->framehook_id);
  return 0;
}

/*! \brief Detach framehook from channel */
static void detach_framehook(struct elevenlabs_session *session) {
  if (!session || session->framehook_id < 0) {
    return;
  }

  ast_framehook_detach(session->channel, session->framehook_id);
  session->framehook_id = -1;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE, "Framehook detached\n");
}

/*! \brief WebSocket thread function - runs in background */
static void *websocket_thread_func(void *arg) {
  struct elevenlabs_session *session = arg;
  struct lws_context_creation_info info;
  int n;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "WebSocket thread starting for channel %s\n",
                 session->channel_name);

  /* Parse signed URL */
  if (parse_signed_url(session->signed_url, session->ws_host, session->ws_path,
                       &session->ws_port) != 0) {
    ast_log(LOG_ERROR, "Failed to parse signed URL: %s\n", session->signed_url);
    return NULL;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Parsed URL - host=%s, port=%d, path=%s\n", session->ws_host,
                 session->ws_port, session->ws_path);

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

  /* Establish WebSocket connection with retry logic */
  if (establish_websocket_connection(session) != 0) {
    ast_log(LOG_ERROR,
            "Failed to establish WebSocket connection after retries\n");
    lws_context_destroy(session->ws_context);
    session->ws_context = NULL;
    return NULL;
  }

  /* Main WebSocket service loop */
  while (session->websocket_running && session->session_active) {
    /* Check if WebSocket close was requested (e.g., by bridge monitor after
     * transfer answered) */
    ast_mutex_lock(&session->session_lock);
    int close_requested = session->ws_close_requested;
    int dial_done = session->dial_completed;
    ast_mutex_unlock(&session->session_lock);

    if (close_requested) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "WebSocket close requested - forcing immediate context destroy\n");

      /* Forcefully destroy context immediately - graceful close doesn't work
       * reliably */
      ast_mutex_lock(&session->session_lock);
      session->ws_close_requested = 0;
      session->ws_connected = 0;
      session->websocket_running = 0;
      struct lws_context *ctx = session->ws_context;
      session->ws_context = NULL;
      session->ws_connection = NULL;
      ast_mutex_unlock(&session->session_lock);

      if (ctx) {
        lws_context_destroy(ctx);
        ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                       "WebSocket context destroyed - connection closed\n");
      }

      /* Exit loop - WebSocket is done */
      break;
    }

    /* If dial thread completed, we're done */
    if (dial_done && session->transfer_in_progress) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "Dial completed - exiting WebSocket loop\n");
      break;
    }

    n = lws_service(session->ws_context, 100); /* 100ms timeout */
    if (n < 0) {
      session->last_error_code = n;
      ast_log(LOG_ERROR, "WebSocket service error: %d (%s)\n", n,
              get_lws_error_string(n));

      /* Try to reconnect if it's a recoverable error */
      if (should_retry_connection(session)) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "Attempting to reconnect WebSocket...\n");
        }
        if (establish_websocket_connection(session) == 0) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "WebSocket reconnection successful\n");
          continue; /* Resume service loop */
        } else {
          ast_log(LOG_ERROR, "WebSocket reconnection failed\n");
        }
      }
      break;
    }

    /* Check connection health periodically */
    static time_t last_health_check = 0;
    time_t now = time(NULL);
    if (now - last_health_check >= 30) { /* Check every 30 seconds */
      last_health_check = now;
      if (!session->ws_connected && session->ws_connection) {
        ast_log(
            LOG_WARNING,
            "WebSocket connection appears stale - attempting reconnection\n");
        if (establish_websocket_connection(session) != 0) {
          ast_log(LOG_ERROR, "Health check reconnection failed\n");
          break;
        }
      }
    }
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "WebSocket thread exiting for channel %s\n",
                 session->channel_name);

  /* If transfer was in progress, wait for dial thread to complete and do
   * cleanup */
  if (session->transfer_in_progress) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
        "Transfer was in progress - waiting for dial thread to complete\n");

    /* Wait for dial thread */
    if (session->dial_thread_running) {
      pthread_join(session->dial_thread, NULL);
      session->dial_thread_running = 0;
    }

    /* Wait for bridge monitor thread */
    session->bridge_monitor_running = 0;
    if (session->bridge_monitor_thread) {
      pthread_join(session->bridge_monitor_thread, NULL);
    }

    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Dial completed with result %d, DIALSTATUS=%s\n",
                   session->dial_result, session->dial_status);

    /* Hang up leg 1 after transfer ended */
    if (session->dial_channel && !ast_check_hangup(session->dial_channel)) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "Hanging up leg 1 after transfer bridge ended\n");
      ast_softhangup(session->dial_channel, AST_SOFTHANGUP_EXPLICIT);
    }

    /* Release channel reference */
    if (session->dial_channel) {
      ast_channel_unref(session->dial_channel);
      session->dial_channel = NULL;
    }

    /* Mark for cleanup */
    session->session_active = 0;
    session->transfer_in_progress = 0;
    session->cleanup_needed = 1;
  }

  /* Check if deferred cleanup is needed (e.g., after transfer) */
  ast_mutex_lock(&session->session_lock);
  int need_cleanup = session->cleanup_needed;
  session->cleanup_needed = 0; /* Clear the flag */
  ast_mutex_unlock(&session->session_lock);

  if (need_cleanup) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Performing deferred cleanup after websocket thread exit\n");
    destroy_session(session);
  }

  return NULL;
}

/*! \brief Send audio data to WebSocket */
static int send_audio_to_websocket(struct elevenlabs_session *session,
                                   const unsigned char *audio_data,
                                   size_t length) {
  cJSON *json;
  char *base64_audio;
  char *message;
  int connection_ok = 0;

  if (!session || !audio_data || length == 0) {
    return -1;
  }

  /* Check connection state under lock */
  ast_mutex_lock(&session->session_lock);
  connection_ok = (session->session_active && session->ws_connected &&
                   session->ws_connection);
  ast_mutex_unlock(&session->session_lock);

  if (!connection_ok) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "WebSocket not connected, skipping audio send\n");
    }
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
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
          "Sending audio chunk to ElevenLabs: %zu bytes raw audio, %zu "
          "bytes base64\n",
          length, strlen(base64_audio));
    }

    /* send_websocket_message now has its own connection validation and recovery
     */
    int send_result = send_websocket_message(session, message);
    if (send_result < 0) {
      if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
        ast_log(
            LOG_DEBUG,
            "Failed to send audio message - WebSocket may be disconnected\n");
      }
      /* Audio send failures are handled gracefully - don't terminate session */
    }
    ast_free(message);
  }

  cJSON_Delete(json);
  ast_free(base64_audio);
  return 0;
}

/*! \brief Audio playback thread function */
static void *playback_thread_func(void *arg) {
  struct elevenlabs_session *session = arg;
  struct audio_queue_item *item;
  struct ast_frame *frame;
  size_t frame_size = 160; /* 20ms at 8kHz, μ-law = 160 bytes */
  unsigned char *audio_ptr;
  size_t remaining;
  struct timespec sleep_time;

  sleep_time.tv_sec = 0;
  sleep_time.tv_nsec = 20000000; /* 20ms */

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Playback thread starting for channel %s\n",
                 session->channel_name);

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "=== PLAYBACK THREAD MAIN LOOP START ===\n");
  while (1) {
    int should_continue = 0;
    int should_hangup = 0;

    ast_mutex_lock(&session->session_lock);
    should_hangup = session->hangup_after_playback;
    should_continue = session->playback_running && session->session_active;
    ast_mutex_unlock(&session->session_lock);

    /* Continue if normal operation OR if we need to finish buffered audio
     * before hangup */
    if (!should_continue && !should_hangup) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "Playback thread exiting - no more work to do\n");
      break;
    }

    /* Wait for audio data */
    ast_mutex_lock(&session->receive_queue->lock);
    while (session->receive_queue->count == 0 && should_continue &&
           !should_hangup) {
      struct timespec timeout;
      timeout.tv_sec = time(NULL) + 1;
      timeout.tv_nsec = 0;
      ast_cond_timedwait(&session->receive_queue->cond,
                         &session->receive_queue->lock, &timeout);

      /* Re-check flags after waiting */
      ast_mutex_unlock(&session->receive_queue->lock);
      ast_mutex_lock(&session->session_lock);
      should_hangup = session->hangup_after_playback;
      should_continue = session->playback_running && session->session_active;
      ast_mutex_unlock(&session->session_lock);
      ast_mutex_lock(&session->receive_queue->lock);
    }

    /* Check if we should hang up after playback when queue is empty */
    if (session->receive_queue->count == 0) {
      ast_mutex_unlock(&session->receive_queue->lock);

      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "Queue is empty - should_hangup=%d, should_continue=%d\n",
                     should_hangup, should_continue);

      if (should_hangup) {
        ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                       "All buffered audio played, hanging up call now!\n");
        if (session->channel && !ast_check_hangup(session->channel)) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "Calling ast_softhangup on channel %s\n",
                         session->channel_name);
          ast_softhangup(session->channel, AST_SOFTHANGUP_EXPLICIT);
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "ast_softhangup completed\n");
        } else {
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "Channel already hung up or NULL\n");
        }
        break;
      }

      /* No audio and not in hangup mode, continue waiting */
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "Queue empty but hangup_after_playback not set, "
                     "continuing to wait\n");
      continue;
    } else {
      ast_mutex_unlock(&session->receive_queue->lock);
    }

    item = audio_queue_get(session->receive_queue);
    if (!item) {
      continue;
    }

    /* Check for interruption before starting to play this chunk */
    ast_mutex_lock(&session->session_lock);
    if (session->interruption_pending) {
      if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
        ELEVENLABS_LOG(
            ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
            "Interruption detected - skipping audio chunk with event_id=%d\n",
            item->event_id);
      }
      session->interruption_pending = 0; /* Reset flag */
      ast_mutex_unlock(&session->session_lock);

      /* Free the current audio item and continue */
      ast_free(item->audio_data);
      ast_free(item);
      continue;
    }
    ast_mutex_unlock(&session->session_lock);

    /* Check remaining queue size */
    ast_mutex_lock(&session->receive_queue->lock);
    int remaining_chunks = session->receive_queue->count;
    ast_mutex_unlock(&session->receive_queue->lock);

    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Playing audio chunk: %zu bytes (event_id: %d), %d chunks "
                   "remaining in queue\n",
                   item->data_length, item->event_id, remaining_chunks);

    /* Split audio data into 20ms frames and play them */
    audio_ptr = item->audio_data;
    remaining = item->data_length;

    while (remaining > 0) {
      /* Check for interruption during frame playback */
      ast_mutex_lock(&session->session_lock);
      if (session->interruption_pending) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "Interruption detected during frame playback - "
                         "stopping audio chunk\n");
        }
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

      /* Write frame to channel - check if session is still active and channel
       * is valid */
      ast_mutex_lock(&session->session_lock);
      int session_ok = session->session_active && session->channel;
      ast_mutex_unlock(&session->session_lock);

      if (session_ok) {
        /* Check channel state before writing */
        if (ast_channel_state(session->channel) == AST_STATE_UP &&
            !ast_check_hangup(session->channel)) {
          if (ast_write(session->channel, frame) < 0) {
            ast_log(
                LOG_WARNING,
                "Failed to write audio frame to channel - ending playback\n");
            /* Stop playback on write error and trigger hangup */
            ast_mutex_lock(&session->session_lock);
            session->playback_running = 0;
            session->hangup_after_playback =
                0; /* Clear flag since we can't play anymore */
            ast_mutex_unlock(&session->session_lock);
          } else {
            if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
              ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                             "Injected audio frame: %zu bytes, %zu samples\n",
                             chunk_size, chunk_size);
            }
          }
        } else {
          if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
            ELEVENLABS_LOG(
                ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                "Channel not available for writing - ending playback\n");
          }
          /* Channel gone, can't play anymore */
          ast_mutex_lock(&session->session_lock);
          session->playback_running = 0;
          session->hangup_after_playback = 0;
          ast_mutex_unlock(&session->session_lock);
        }
      } else {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "Session inactive - ending playback\n");
        }
        /* Session inactive, can't play anymore */
        ast_mutex_lock(&session->session_lock);
        session->playback_running = 0;
        session->hangup_after_playback = 0;
        ast_mutex_unlock(&session->session_lock);
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
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Finished playing audio chunk (event_id: %d)\n",
                   item->event_id);

    if (item->audio_data) {
      ast_free(item->audio_data);
    }
    ast_free(item);

    /* Check if queue is now empty and we should hang up */
    ast_mutex_lock(&session->receive_queue->lock);
    int queue_empty = (session->receive_queue->count == 0);
    ast_mutex_unlock(&session->receive_queue->lock);

    if (queue_empty) {
      ast_mutex_lock(&session->session_lock);
      int should_hangup_now = session->hangup_after_playback;
      ast_mutex_unlock(&session->session_lock);

      if (should_hangup_now) {
        ELEVENLABS_LOG(
            ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
            "Queue empty after playing chunk - checking hangup condition\n");
        ELEVENLABS_LOG(
            ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
            "All buffered audio has been played, hanging up call now!\n");
        if (session->channel && !ast_check_hangup(session->channel)) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "Calling ast_softhangup on channel %s\n",
                         session->channel_name);
          ast_softhangup(session->channel, AST_SOFTHANGUP_EXPLICIT);
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "Call hangup initiated successfully\n");
        }
        break;
      }
    }
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Playback thread exiting for channel %s\n",
                 session->channel_name);
  return NULL;
}

/*! \brief Monitor thread to detect caller hangup and trigger immediate cleanup
 */
static void *hangup_monitor_thread_func(void *arg) {
  struct elevenlabs_session *session = arg;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Hangup monitor thread started for channel %s\n",
                 session->channel_name);

  while (1) {
    int running = 0;
    int active = 0;
    int transfer = 0;

    ast_mutex_lock(&session->session_lock);
    running = session->hangup_monitor_running;
    active = session->session_active;
    transfer = session->transfer_in_progress;
    ast_mutex_unlock(&session->session_lock);

    if (!running || !active) {
      break;
    }

    /* Don't interfere with transfer - cleanup will be handled by transfer code
     */
    if (transfer) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "Transfer in progress - hangup monitor exiting for channel %s\n",
          session->channel_name);
      break;
    }

    if (session->channel && ast_check_hangup(session->channel)) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "Caller hangup detected by monitor for channel %s - initiating "
          "cleanup\n",
          session->channel_name);

      ast_mutex_lock(&session->session_lock);
      session->session_active = 0;
      session->ws_connected = 0;
      session->stt_ws_connected = 0;
      session->hangup_after_playback = 0;
      ast_mutex_unlock(&session->session_lock);

      if (session->receive_queue) {
        ast_cond_signal(&session->receive_queue->cond);
      }

      destroy_session(session);
      return NULL;
    }

    usleep(50000); /* 50ms poll */
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Hangup monitor thread exiting for channel %s\n",
                 session->channel_name);
  return NULL;
}

/*! \brief Dial thread - runs Dial in a separate thread so websocket can
 * continue */
static void *dial_thread_func(void *arg) {
  struct elevenlabs_session *session = arg;
  struct ast_app *dial_app;
  int res;
  const char *dialstatus;
  const char *uniqueid;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Dial thread started for channel %s\n", session->channel_name);

  dial_app = pbx_findapp("Dial");
  if (!dial_app) {
    ast_log(LOG_ERROR, "Dial application not found\n");
    ast_mutex_lock(&session->session_lock);
    session->dial_completed = 1;
    session->dial_result = -1;
    ast_copy_string(session->dial_status, "FAILED",
                    sizeof(session->dial_status));
    ast_mutex_unlock(&session->session_lock);
    return NULL;
  }

  /* Set caller ID for outbound call to match original incoming call */
  if (!ast_strlen_zero(session->caller_id)) {
    ast_func_write(session->dial_channel, "CALLERID(num)", session->caller_id);
    ast_func_write(session->dial_channel, "CALLERID(name)", session->caller_id);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Set outbound CALLERID(num) and CALLERID(name) to: %s\n",
                   session->caller_id);
  }

  /* Set called ID (DNID) for outbound call to match original incoming call */
  if (!ast_strlen_zero(session->called_id) &&
      strcmp(session->called_id, "Unknown") != 0) {
    ast_func_write(session->dial_channel, "CALLERID(dnid)", session->called_id);
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Set outbound CALLERID(dnid) to: %s\n", session->called_id);
  }

  /* Set SIP headers for outbound call */
  ast_channel_lock(session->dial_channel);
  uniqueid = ast_channel_uniqueid(session->dial_channel);
  ast_channel_unlock(session->dial_channel);

  /* Add custom SIP headers: X-Original-CallerID, X-Original-CallID,
   * X-Transfer-Number, X-Transfer-Reason */
  pbx_builtin_setvar_helper(session->dial_channel,
                            "PJSIP_HEADER(add,X-Original-CallerID)",
                            session->caller_id);
  pbx_builtin_setvar_helper(session->dial_channel,
                            "PJSIP_HEADER(add,X-Original-CallID)",
                            uniqueid ? uniqueid : "");
  pbx_builtin_setvar_helper(session->dial_channel,
                            "PJSIP_HEADER(add,X-Transfer-Number)",
                            session->dial_number);
  if (!ast_strlen_zero(session->dial_reason)) {
    pbx_builtin_setvar_helper(session->dial_channel,
                              "PJSIP_HEADER(add,X-Transfer-Reason)",
                              session->dial_reason);
  }

  /* Pass through all X-* headers from the original incoming call */
  if (session->custom_headers) {
    struct custom_header *hdr = session->custom_headers;
    int header_count = 0;
    char func_name[256];

    while (hdr) {
      snprintf(func_name, sizeof(func_name), "PJSIP_HEADER(add,%s)", hdr->name);
      pbx_builtin_setvar_helper(session->dial_channel, func_name, hdr->value);
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Passing through header: %s = %s\n", hdr->name,
                     hdr->value);
      header_count++;
      hdr = hdr->next;
    }

    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Passed through %d custom X-* headers from incoming call\n",
                   header_count);
  }

  ELEVENLABS_LOG(
      ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
      "Set SIP headers: CallerID=%s, CallID=%s, Number=%s, Reason=%s\n",
      session->caller_id, uniqueid ? uniqueid : "(null)", session->dial_number,
      session->dial_reason[0] ? session->dial_reason : "(none)");

  /* Run Dial - this blocks until call ends */
  res = pbx_exec(session->dial_channel, dial_app, session->dial_args);

  /* Get DIALSTATUS */
  dialstatus = pbx_builtin_getvar_helper(session->dial_channel, "DIALSTATUS");

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Dial completed with result %d, DIALSTATUS=%s\n", res,
                 dialstatus ? dialstatus : "(null)");

  ast_mutex_lock(&session->session_lock);
  session->dial_completed = 1;
  session->dial_result = res;
  if (dialstatus) {
    ast_copy_string(session->dial_status, dialstatus,
                    sizeof(session->dial_status));
  } else {
    session->dial_status[0] = '\0';
  }
  ast_mutex_unlock(&session->session_lock);

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Dial thread exiting for channel %s\n", session->channel_name);
  return NULL;
}

/*! \brief Bridge monitor thread - detects when transfer call is bridged and
 * signals WebSocket close */
static void *bridge_monitor_thread_func(void *arg) {
  struct elevenlabs_session *session = arg;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Bridge monitor thread started for channel %s\n",
                 session->channel_name);

  while (1) {
    int running = 0;
    RAII_VAR(struct ast_bridge *, bridge, NULL, ao2_cleanup);

    ast_mutex_lock(&session->session_lock);
    running = session->bridge_monitor_running;
    ast_mutex_unlock(&session->session_lock);

    if (!running) {
      break;
    }

    /* Check if channel is now in a bridge */
    if (session->channel) {
      ast_channel_lock(session->channel);
      bridge = ast_channel_get_bridge(session->channel);
      ast_channel_unlock(session->channel);

      if (bridge) {
        /* Channel is bridged - transfer was answered! Signal WebSocket to close
         */
        ELEVENLABS_LOG(
            ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
            "Transfer call bridged - signaling WebSocket to close\n");

        ast_mutex_lock(&session->session_lock);
        session->ws_close_requested = 1;
        session->bridge_monitor_running = 0;
        /* Wake up lws_service if it's waiting */
        if (session->ws_context) {
          lws_cancel_service(session->ws_context);
        }
        ast_mutex_unlock(&session->session_lock);

        break;
      }
    }

    usleep(50000); /* 50ms poll */
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Bridge monitor thread exiting for channel %s\n",
                 session->channel_name);
  return NULL;
}

/*! \brief Parse WebSocket signed URL */
static int parse_signed_url(const char *signed_url, char *host, char *path,
                            int *port) {
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
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
  struct elevenlabs_session *session = NULL;
  int debug_enabled = 0;

  /* Get session for relevant callbacks */
  if (reason == LWS_CALLBACK_CLIENT_ESTABLISHED ||
      reason == LWS_CALLBACK_CLIENT_RECEIVE ||
      reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR ||
      reason == LWS_CALLBACK_CLOSED ||
      reason == 30) { /* Add reason 30 for session retrieval */

    if (wsi && lws_get_context(wsi)) {
      session =
          (struct elevenlabs_session *)lws_context_user(lws_get_context(wsi));

      /* Validate session pointer - allow callbacks during cleanup */
      if (!session) {
        ELEVENLABS_LOG(
            ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
            "WebSocket callback called with NULL session (reason=%d)\n",
            reason);
        return -1;
      }

      /* Get debug flag */
      if (session->config) {
        debug_enabled = elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG;
      }

      /* For ping/pong and closed callbacks, don't require active session */
      if (reason != LWS_CALLBACK_CLOSED &&
          reason != LWS_CALLBACK_CLIENT_CONNECTION_ERROR && reason != 30) {
        if (!session->session_active) {
          if (debug_enabled) {
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                           "WebSocket callback called with inactive "
                           "session (cleanup in progress)\n");
          }
          return 0; /* Allow cleanup to continue */
        }
      }
    } else {
      ast_log(LOG_ERROR, "WSI or context is NULL for callback reason=%d\n",
              reason);
      return -1;
    }
  }

  /* CRITICAL: Move emergency deactivation BEFORE switch statement */
  if ((reason == LWS_CALLBACK_CLOSED || reason == 30) && session) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
          "PRE-SWITCH: Detected CLOSE callback - emergency deactivation "
          "(reason=%d)\n",
          reason);
    }

    /* Stop WebSocket communication immediately, but let playback finish */
    ast_mutex_lock(&session->session_lock);
    session->ws_connected = 0; /* Stop sending to WebSocket immediately */
    ast_mutex_unlock(&session->session_lock);

    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "WebSocket disconnected - playback thread will finish "
                     "buffered audio\n");
    }
  }

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    if (session) {
      session->ws_connected = 1;
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "WebSocket connection established for channel %s\n",
                     session->channel_name);
      /* Send conversation_initiation_client_data message with dynamic_variables
       */
      send_conversation_initiation(session);
    }
    break;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    if (session && in && len > 0 && session->session_active) {
      /* Check if this is the final fragment */
      int is_final = lws_is_final_fragment(wsi);
      add_to_message_buffer(session, (char *)in, len, is_final);
    }
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    if (session) {
      if (debug_enabled) {
        ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                       "Processing connection error for session %p\n", session);
      }

      /* Handle connection failure with retry logic */
      handle_connection_failure(session,
                                "WebSocket client connection error callback");

      /* Only terminate session if we've exhausted retries */
      if (session->connection_attempts >= session->max_connection_attempts) {
        ast_log(LOG_ERROR,
                "Max connection attempts exceeded - terminating session\n");
        ast_mutex_lock(&session->session_lock);
        session->websocket_running = 0;
        session->session_active = 0;
        ast_mutex_unlock(&session->session_lock);

        /* Signal playback thread to stop */
        if (session->receive_queue) {
          if (debug_enabled) {
            ELEVENLABS_LOG(
                ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                "Signaling playback thread due to connection error\n");
          }
          ast_cond_signal(&session->receive_queue->cond);
        }

        /* Hang up the call since we can't establish connection */
        if (session->channel && !ast_check_hangup(session->channel)) {
          ELEVENLABS_LOG(
              ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
              "Hanging up call due to persistent connection failures\n");
          ast_softhangup(session->channel, AST_SOFTHANGUP_EXPLICIT);
        }
      } else {
        if (debug_enabled) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "Will retry connection (%d/%d attempts used)\n",
                         session->connection_attempts,
                         session->max_connection_attempts);
        }
      }
    }
    break;

  case 30: /* The ACTUAL close event in this libwebsockets version */
    if (session) {
      /* Check how many audio chunks are still buffered */
      int buffered_chunks = 0;
      if (session->receive_queue) {
        ast_mutex_lock(&session->receive_queue->lock);
        buffered_chunks = session->receive_queue->count;
        ast_mutex_unlock(&session->receive_queue->lock);
      }

      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "WebSocket closed for channel: %s - %d audio chunks still buffered\n",
          session->channel_name, buffered_chunks);

      /* Publish 'ended' event to ingester */
      publish_ended_event(session);

      /* Stop websocket thread but let playback continue */
      ast_mutex_lock(&session->session_lock);
      session->websocket_running = 0;
      session->ws_connection = NULL;
      /* Set flag to hang up after all buffered audio is played */
      session->hangup_after_playback = 1;
      /* Also stop STT since ElevenLabs connection is closed */
      session->stt_websocket_running = 0;
      session->stt_ws_connected = 0;
      ast_mutex_unlock(&session->session_lock);

      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "Set hangup_after_playback flag - will hang up after playing %d "
          "remaining chunks\n",
          buffered_chunks);

      /* Signal playback thread to check the queue and hangup flag */
      if (session->receive_queue) {
        ast_cond_signal(&session->receive_queue->cond);
      }
    }
    break;

  case LWS_CALLBACK_CLOSED:
    if (session) {
      /* Check how many audio chunks are still buffered */
      int buffered_chunks = 0;
      if (session->receive_queue) {
        ast_mutex_lock(&session->receive_queue->lock);
        buffered_chunks = session->receive_queue->count;
        ast_mutex_unlock(&session->receive_queue->lock);
      }

      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "WebSocket closed for channel: %s - %d audio chunks still buffered\n",
          session->channel_name, buffered_chunks);

      /* Publish 'ended' event to ingester (will be no-op if already sent) */
      publish_ended_event(session);

      /* Stop websocket thread but let playback continue */
      ast_mutex_lock(&session->session_lock);
      session->websocket_running = 0;
      session->ws_connection = NULL;
      /* Set flag to hang up after all buffered audio is played */
      session->hangup_after_playback = 1;
      /* Also stop STT since ElevenLabs connection is closed */
      session->stt_websocket_running = 0;
      session->stt_ws_connected = 0;
      ast_mutex_unlock(&session->session_lock);

      ELEVENLABS_LOG(
          ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
          "Set hangup_after_playback flag - will hang up after playing %d "
          "remaining chunks\n",
          buffered_chunks);

      /* Signal playback thread to check the queue and hangup flag */
      if (session->receive_queue) {
        ast_cond_signal(&session->receive_queue->cond);
      }
    }
    break;

  default:
    break;
  }

  return 0;
}

/*! \brief Send WebSocket message */
static int send_websocket_message(struct elevenlabs_session *session,
                                  const char *message) {
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
  if ((!session->session_active && !session->transfer_in_progress) ||
      !session->ws_connected || !session->ws_connection) {
    ast_mutex_unlock(&session->session_lock);
    /* Note: session access is safe here since we're in send_websocket_message
     */
    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                   "WebSocket not connected, skipping message send\n");
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
    if (session && elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Sent to ElevenLabs: %s\n", message);
    }
  } else {
    ast_log(LOG_WARNING,
            "Failed to send WebSocket message - connection may be closed\n");

    /* Mark connection as disconnected on write failure */
    ast_mutex_lock(&session->session_lock);
    session->ws_connected = 0;
    ast_mutex_unlock(&session->session_lock);
  }

  ast_free(buf);
  return result;
}

/*! \brief Handle incoming WebSocket message */
static void handle_websocket_message(struct elevenlabs_session *session,
                                     const char *message) {
  cJSON *json;
  cJSON *type_item;

  if (!session || !message) {
    return;
  }

  if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                   "Received WebSocket message: %s\n", message);
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

  if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "Received %s message\n",
                   type_item->valuestring);
  }

  /* Handle vad_score messages with detailed logging */
  if (strcmp(type_item->valuestring, "vad_score") == 0) {
    cJSON *vad_score_event = cJSON_GetObjectItem(json, "vad_score_event");

    if (vad_score_event && cJSON_IsObject(vad_score_event)) {
      cJSON *score_item = cJSON_GetObjectItem(vad_score_event, "vad_score");
      cJSON *timestamp_item = cJSON_GetObjectItem(vad_score_event, "timestamp");

      if (score_item && cJSON_IsNumber(score_item)) {
        double score = score_item->valuedouble;
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "=== VAD SCORE MESSAGE ===\n");
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "VAD Score: %.3f\n",
                         score);

          if (timestamp_item && cJSON_IsString(timestamp_item)) {
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "Timestamp: %s\n",
                           timestamp_item->valuestring);
          }

          if (score > 0.5) {
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                           "HIGH voice activity detected (score=%.3f)\n",
                           score);
          } else {
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                           "LOW voice activity detected (score=%.3f)\n", score);
          }
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "=== END VAD SCORE ===\n");
        }
      } else {
        ast_log(LOG_WARNING, "vad_score_event missing valid vad_score field\n");
      }
    } else {
      ast_log(LOG_WARNING,
              "vad_score message missing vad_score_event object\n");
    }
  }
  /* Handle conversation_initiation_metadata - capture conversation_id */
  else if (strcmp(type_item->valuestring, "conversation_initiation_metadata") ==
           0) {
    cJSON *conversation_id_item = NULL;
    cJSON *metadata_event = NULL;

    /* Try to find conversation_id in nested structure first */
    metadata_event =
        cJSON_GetObjectItem(json, "conversation_initiation_metadata_event");
    if (metadata_event && cJSON_IsObject(metadata_event)) {
      conversation_id_item =
          cJSON_GetObjectItem(metadata_event, "conversation_id");
    }

    /* Fallback to root level */
    if (!conversation_id_item || !cJSON_IsString(conversation_id_item)) {
      conversation_id_item = cJSON_GetObjectItem(json, "conversation_id");
    }

    if (conversation_id_item && cJSON_IsString(conversation_id_item)) {
      ast_copy_string(session->conversation_id,
                      conversation_id_item->valuestring,
                      sizeof(session->conversation_id));
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "Received conversation_id from ElevenLabs: %s\n",
                     session->conversation_id);
    } else {
      /* Log the full message for debugging */
      char *msg_str = cJSON_PrintUnformatted(json);
      ast_log(LOG_WARNING,
              "conversation_initiation_metadata missing conversation_id. "
              "Message: %s\n",
              msg_str ? msg_str : "(null)");
      if (msg_str) {
        ast_std_free(msg_str);
      }
    }

    /* Publish 'started' event (will use Asterisk uniqueid as fallback) */
    publish_started_event(session);
  }
  /* Handle user_transcript - user speech transcription */
  else if (strcmp(type_item->valuestring, "user_transcript") == 0) {
    cJSON *transcript_event =
        cJSON_GetObjectItem(json, "user_transcription_event");
    if (!transcript_event) {
      /* Try alternate key */
      transcript_event = cJSON_GetObjectItem(json, "user_transcript_event");
    }

    if (transcript_event && cJSON_IsObject(transcript_event)) {
      cJSON *text_item =
          cJSON_GetObjectItem(transcript_event, "user_transcript");
      cJSON *is_final_item = cJSON_GetObjectItem(transcript_event, "is_final");

      if (text_item && cJSON_IsString(text_item)) {
        int is_final = (is_final_item && cJSON_IsBool(is_final_item))
                           ? cJSON_IsTrue(is_final_item)
                           : 1;

        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "User transcript (is_final=%d): %s\n", is_final,
                         text_item->valuestring);
        }

        /* Publish transcript_chunk event for user (client) */
        publish_transcript_chunk(session, text_item->valuestring, "client",
                                 is_final);
      }
    } else {
      /* Handle simple format where transcript is at root level */
      cJSON *text_item = cJSON_GetObjectItem(json, "user_transcript");
      if (text_item && cJSON_IsString(text_item)) {
        cJSON *is_final_item = cJSON_GetObjectItem(json, "is_final");
        int is_final = (is_final_item && cJSON_IsBool(is_final_item))
                           ? cJSON_IsTrue(is_final_item)
                           : 1;

        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "User transcript (simple format, is_final=%d): %s\n",
                         is_final, text_item->valuestring);
        }

        publish_transcript_chunk(session, text_item->valuestring, "client",
                                 is_final);
      }
    }
  }
  /* Handle agent_response - agent speech text */
  else if (strcmp(type_item->valuestring, "agent_response") == 0) {
    cJSON *response_event = cJSON_GetObjectItem(json, "agent_response_event");
    if (!response_event) {
      /* Try alternate key */
      response_event = cJSON_GetObjectItem(json, "agent_response");
    }

    if (response_event && cJSON_IsObject(response_event)) {
      cJSON *text_item = cJSON_GetObjectItem(response_event, "agent_response");
      if (!text_item) {
        text_item = cJSON_GetObjectItem(response_event, "text");
      }
      cJSON *is_final_item =
          cJSON_GetObjectItem(response_event, "end_of_response");
      if (!is_final_item) {
        is_final_item = cJSON_GetObjectItem(response_event, "is_final");
      }

      if (text_item && cJSON_IsString(text_item)) {
        int is_final = (is_final_item && cJSON_IsBool(is_final_item))
                           ? cJSON_IsTrue(is_final_item)
                           : 0;

        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "Agent response (is_final=%d): %s\n", is_final,
                         text_item->valuestring);
        }

        /* Publish transcript_chunk event for agent */
        publish_transcript_chunk(session, text_item->valuestring, "agent",
                                 is_final);
      }
    } else {
      /* Handle simple format */
      cJSON *text_item = cJSON_GetObjectItem(json, "agent_response");
      if (text_item && cJSON_IsString(text_item)) {
        cJSON *is_final_item = cJSON_GetObjectItem(json, "end_of_response");
        int is_final = (is_final_item && cJSON_IsBool(is_final_item))
                           ? cJSON_IsTrue(is_final_item)
                           : 0;

        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "Agent response (simple format, is_final=%d): %s\n",
                         is_final, text_item->valuestring);
        }

        publish_transcript_chunk(session, text_item->valuestring, "agent",
                                 is_final);
      }
    }
  }
  /* Handle interruption messages with detailed logging */
  else if (strcmp(type_item->valuestring, "interruption") == 0) {
    cJSON *interruption_event = cJSON_GetObjectItem(json, "interruption_event");

    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "=== INTERRUPTION MESSAGE ===\n");
    }

    if (interruption_event && cJSON_IsObject(interruption_event)) {
      cJSON *event_id_item =
          cJSON_GetObjectItem(interruption_event, "event_id");
      cJSON *timestamp_item =
          cJSON_GetObjectItem(interruption_event, "timestamp");

      if (event_id_item && cJSON_IsNumber(event_id_item)) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "Event ID: %d\n",
                         event_id_item->valueint);
        }
      }

      if (timestamp_item && cJSON_IsString(timestamp_item)) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "Timestamp: %s\n",
                         timestamp_item->valuestring);
        }
      }
    } else {
      ast_log(LOG_WARNING,
              "interruption message missing interruption_event object\n");
    }

    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "User interrupted AI speech - stopping playback\n");
    }

    /* Process interruption based on event_id */
    if (interruption_event && cJSON_IsObject(interruption_event)) {
      cJSON *event_id_item =
          cJSON_GetObjectItem(interruption_event, "event_id");
      int interrupt_event_id = event_id_item ? event_id_item->valueint : -1;

      if (interrupt_event_id >= 0) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(
              ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
              "Processing interruption for event_id=%d - clearing older "
              "audio\n",
              interrupt_event_id);
        }

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

              if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
                ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                               "Removing audio chunk with event_id=%d (≤%d)\n",
                               item->event_id, interrupt_event_id);
              }

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

          if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
            ELEVENLABS_LOG(
                ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                "Interruption processing complete: removed %d audio chunks "
                "with event_id ≤ %d\n",
                removed_count, interrupt_event_id);
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                           "Remaining audio chunks in queue: %d\n",
                           session->receive_queue->count);
          }
        }

        /* Signal playback thread to check for interruption */
        if (session->receive_queue) {
          ast_cond_signal(&session->receive_queue->cond);
        }
      } else {
        ast_log(LOG_WARNING,
                "Invalid interrupt event_id, clearing entire queue\n");

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
          if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
            ELEVENLABS_LOG(
                ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                "Entire audio playback queue cleared due to interruption\n");
          }
        }
      }
    }

    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "=== END INTERRUPTION ===\n");
    }
  }
  /* Handle tool calls from ElevenLabs client */
  else if (strcmp(type_item->valuestring, "client_tool_call") == 0) {
    cJSON *tool_call = cJSON_GetObjectItem(json, "client_tool_call");
    if (tool_call && cJSON_IsObject(tool_call)) {
      handle_client_tool_call(session, tool_call);
      cJSON_Delete(json);
      return;
    } else {
      ast_log(LOG_WARNING,
              "client_tool_call message missing client_tool_call object\n");
    }
  }
  /* Log agent tool requests for debugging/visibility */
  else if (strcmp(type_item->valuestring, "agent_tool_request") == 0) {
    cJSON *tool_req = cJSON_GetObjectItem(json, "agent_tool_request");
    const char *tool_name = NULL;
    int expects_response = -1;
    int event_id = -1;

    if (tool_req && cJSON_IsObject(tool_req)) {
      cJSON *name_item = cJSON_GetObjectItem(tool_req, "tool_name");
      cJSON *expects_item = cJSON_GetObjectItem(tool_req, "expects_response");
      cJSON *event_id_item = cJSON_GetObjectItem(tool_req, "event_id");

      if (name_item && cJSON_IsString(name_item)) {
        tool_name = name_item->valuestring;
      }
      if (expects_item && cJSON_IsBool(expects_item)) {
        expects_response = cJSON_IsTrue(expects_item) ? 1 : 0;
      }
      if (event_id_item && cJSON_IsNumber(event_id_item)) {
        event_id = event_id_item->valueint;
      }
    }

    ELEVENLABS_LOG(
        ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
        "Agent tool request: tool_name=%s expects_response=%d event_id=%d "
        "payload=%s\n",
        tool_name ? tool_name : "(null)", expects_response, event_id, message);

    /* Handle transfer_to_human agent tool as a client tool call */
    if (tool_req && tool_name && strcmp(tool_name, "transfer_to_human") == 0) {
      handle_client_tool_call(session, tool_req);
      cJSON_Delete(json);
      return;
    }
  }
  /* Handle audio messages */
  else if (strcmp(type_item->valuestring, "audio") == 0) {
    cJSON *audio_event = cJSON_GetObjectItem(json, "audio_event");
    if (audio_event) {
      cJSON *audio_base64_item =
          cJSON_GetObjectItem(audio_event, "audio_base_64");
      cJSON *event_id_item = cJSON_GetObjectItem(audio_event, "event_id");

      if (audio_base64_item && cJSON_IsString(audio_base64_item) &&
          event_id_item && cJSON_IsNumber(event_id_item)) {

        /* Decode base64 audio */
        size_t audio_length;
        unsigned char *audio_data =
            base64_decode(audio_base64_item->valuestring, &audio_length);
        if (audio_data) {
          if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
            ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                           "Received audio from ElevenLabs: event_id=%d, "
                           "base64_size=%zu, raw_size=%zu\n",
                           event_id_item->valueint,
                           strlen(audio_base64_item->valuestring),
                           audio_length);
          }

          /* Add to receive queue for playback */
          audio_queue_add(session->receive_queue, audio_data, audio_length,
                          event_id_item->valueint);
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
    unsigned char *audio_data =
        base64_decode(audio_chunk->valuestring, &audio_length);
    if (audio_data) {
      if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
        ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                       "Received audio_chunk from ElevenLabs: base64_size=%zu, "
                       "raw_size=%zu\n",
                       strlen(audio_chunk->valuestring), audio_length);
      }

      /* Add to receive queue for playback */
      audio_queue_add(session->receive_queue, audio_data, audio_length,
                      session->current_event_id++);
      ast_free(audio_data);
    } else {
      ast_log(LOG_ERROR, "Failed to decode base64 audio_chunk data\n");
    }
  }

  cJSON_Delete(json);
}

/*! \brief Stop session activity before transferring, without hanging up */
static void quiesce_session_for_transfer(struct elevenlabs_session *session,
                                         pthread_t current_thread) {
  if (!session) {
    return;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Quiescing session before transfer\n");

  pthread_t playback_thread = session->playback_thread;
  pthread_t stt_thread = session->stt_websocket_thread;

  ast_mutex_lock(&session->session_lock);
  session->transfer_in_progress = 1;
  session->playback_running = 0;
  session->stt_websocket_running = 0;
  session->hangup_after_playback = 0;
  session->hangup_monitor_running = 0;
  ast_mutex_unlock(&session->session_lock);

  /* Wake playback thread if waiting */
  if (session->receive_queue) {
    ast_cond_signal(&session->receive_queue->cond);
  }

  /* Join playback thread */
  if (playback_thread && !pthread_equal(current_thread, playback_thread)) {
    pthread_join(playback_thread, NULL);
  }

  /* Join STT thread */
  if (stt_thread && !pthread_equal(current_thread, stt_thread)) {
    pthread_join(stt_thread, NULL);
  }

  /* Close WebSocket connection/context if present */
  ast_mutex_lock(&session->session_lock);
  /* Do NOT close WebSocket here; we need it to send tool result */
  ast_mutex_unlock(&session->session_lock);

  /* Detach framehook to stop callbacks */
  detach_framehook(session);
}

/*! \brief Send tool result back to ElevenLabs after Dial attempt */
static void send_tool_result(struct elevenlabs_session *session,
                             const char *tool_call_id, int success,
                             const char *detail) {
  cJSON *json;
  char *message;
  int connection_ok = 0;

  if (!session || ast_strlen_zero(tool_call_id)) {
    return;
  }

  ast_mutex_lock(&session->session_lock);
  connection_ok = (session->ws_connected && session->ws_connection);
  ast_mutex_unlock(&session->session_lock);

  if (!connection_ok) {
    ast_log(
        LOG_WARNING,
        "Cannot send tool result (tool_call_id=%s) - WebSocket not connected\n",
        tool_call_id);
    return;
  }

  json = cJSON_CreateObject();
  if (!json) {
    ast_log(LOG_ERROR, "Failed to allocate JSON for tool result\n");
    return;
  }

  cJSON_AddStringToObject(json, "type", "client_tool_result");
  cJSON_AddStringToObject(json, "tool_call_id", tool_call_id);
  cJSON_AddBoolToObject(json, "success", success ? 1 : 0);
  if (detail && !ast_strlen_zero(detail)) {
    cJSON_AddStringToObject(json, "message", detail);
  }

  message = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);

  if (!message) {
    ast_log(LOG_ERROR, "Failed to serialize tool result JSON\n");
    return;
  }

  if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "Sending tool result: %s\n",
                   message);
  }

  send_websocket_message(session, message);
  ast_free(message);
}

/*! \brief Fail the current call/session safely without impacting Asterisk */
static void fail_call_with_error(struct elevenlabs_session *session,
                                 const char *tool_call_id, const char *reason) {
  int was_transfer_in_progress;

  if (!session) {
    return;
  }

  ast_log(LOG_WARNING, "Failing current call/session: %s\n",
          reason ? reason : "(no reason)");

  /* Try to report failure to ElevenLabs if possible */
  if (tool_call_id) {
    send_tool_result(session, tool_call_id, 0, reason ? reason : "failure");
  }

  ast_mutex_lock(&session->session_lock);
  was_transfer_in_progress = session->transfer_in_progress;
  session->session_active = 0;
  session->transfer_in_progress = 0;
  session->websocket_running = 0;
  session->ws_connected = 0;
  session->playback_running = 0;
  session->stt_websocket_running = 0;
  session->hangup_after_playback = 0;
  /* If we were in transfer mode, mark for deferred cleanup */
  if (was_transfer_in_progress) {
    session->cleanup_needed = 1;
  }
  ast_mutex_unlock(&session->session_lock);

  if (session->receive_queue) {
    ast_cond_signal(&session->receive_queue->cond);
  }

  /* NOTE: Don't call destroy_session here - we're inside the websocket thread.
   * The cleanup will be done by websocket_thread_func after it exits the main
   * loop. */
}

/*! \brief Handle client tool calls from ElevenLabs (e.g., transfer_to_human) */
static void handle_client_tool_call(struct elevenlabs_session *session,
                                    cJSON *tool_call) {
  cJSON *tool_name_item;
  cJSON *params_item;
  const char *tool_name;
  const char *dial_number;
  const char *tool_call_id = NULL;
  cJSON *tool_call_id_item;
  char dial_arg[128];
  struct ast_channel *chan_ref = NULL;

  if (!session || !tool_call) {
    return;
  }

  tool_name_item = cJSON_GetObjectItem(tool_call, "tool_name");
  params_item = cJSON_GetObjectItem(tool_call, "parameters");
  if (!params_item || !cJSON_IsObject(params_item)) {
    /* Fallback: some messages might place parameters at the top level */
    params_item = tool_call;
  }
  tool_call_id_item = cJSON_GetObjectItem(tool_call, "tool_call_id");
  if (tool_call_id_item && cJSON_IsString(tool_call_id_item)) {
    tool_call_id = tool_call_id_item->valuestring;
  }

  if (!tool_name_item || !cJSON_IsString(tool_name_item) || !params_item ||
      !cJSON_IsObject(params_item)) {
    ast_log(LOG_WARNING, "client_tool_call missing tool_name or parameters\n");
    return;
  }

  tool_name = tool_name_item->valuestring;

  if (strcmp(tool_name, "transfer_to_human") != 0) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Unsupported client_tool_call '%s' - ignoring\n",
                     tool_name);
    }
    return;
  }

  dial_number = NULL;
  const char *dial_reason = NULL;
  if (params_item && cJSON_IsObject(params_item)) {
    cJSON *number_item = cJSON_GetObjectItem(params_item, "number");
    if (number_item && cJSON_IsString(number_item) &&
        !ast_strlen_zero(number_item->valuestring)) {
      dial_number = number_item->valuestring;
    }
    cJSON *reason_item = cJSON_GetObjectItem(params_item, "reason");
    if (reason_item && cJSON_IsString(reason_item) &&
        !ast_strlen_zero(reason_item->valuestring)) {
      dial_reason = reason_item->valuestring;
    }
  }

  if (ast_strlen_zero(dial_number)) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
        "transfer_to_human missing 'number' parameter - ignoring request\n");
    return;
  }

  snprintf(dial_arg, sizeof(dial_arg), "PJSIP/%s@trunk_asterisk112,60",
           dial_number);

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Processing transfer_to_human to %s, reason=%s (Dial(%s))\n",
                 dial_number, dial_reason ? dial_reason : "(none)", dial_arg);

  /* Keep channel alive during cleanup */
  if (session->channel) {
    chan_ref = ast_channel_ref(session->channel);
  }

  if (!chan_ref) {
    fail_call_with_error(session, tool_call_id, "no channel reference");
    return;
  }

  /* Stop playback and other activity but keep websocket running */
  quiesce_session_for_transfer(session, pthread_self());

  /* Store dial arguments for the dial thread */
  ast_copy_string(session->dial_args, dial_arg, sizeof(session->dial_args));
  ast_copy_string(session->dial_number, dial_number,
                  sizeof(session->dial_number));
  if (dial_reason) {
    ast_copy_string(session->dial_reason, dial_reason,
                    sizeof(session->dial_reason));
  } else {
    session->dial_reason[0] = '\0';
  }
  session->dial_channel = chan_ref;
  session->dial_completed = 0;
  session->dial_result = 0;
  session->dial_status[0] = '\0';

  /* Start bridge monitor thread - it will signal WebSocket to close when call
   * is bridged */
  session->bridge_monitor_running = 1;
  if (pthread_create(&session->bridge_monitor_thread, NULL,
                     bridge_monitor_thread_func, session) != 0) {
    ast_log(LOG_WARNING, "Failed to create bridge monitor thread\n");
    session->bridge_monitor_running = 0;
  }

  /* Start dial thread - runs Dial in background so websocket loop can continue
   */
  session->dial_thread_running = 1;
  if (pthread_create(&session->dial_thread, NULL, dial_thread_func, session) !=
      0) {
    ast_log(LOG_ERROR, "Failed to create dial thread - cannot transfer\n");
    session->dial_thread_running = 0;
    session->bridge_monitor_running = 0;
    fail_call_with_error(session, tool_call_id, "dial thread failed");
    ast_channel_unref(chan_ref);
    return;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Transfer initiated - dial thread started, returning to "
                 "WebSocket loop\n");

  /* Return immediately - websocket loop will continue and handle:
   * 1. ws_close_requested flag from bridge_monitor when call is bridged
   * 2. dial_completed flag from dial_thread when call ends
   * Cleanup will happen when websocket loop exits */
}

/*! \brief Add data to message buffer and process when complete */
static int add_to_message_buffer(struct elevenlabs_session *session,
                                 const char *data, size_t len, int is_final) {
  if (!session || !data || len == 0) {
    return -1;
  }

  /* Check if session is still active */
  if (!session->session_active) {
    ast_log(LOG_WARNING, "Ignoring message buffer add - session inactive\n");
    return -1;
  }

  ast_mutex_lock(&session->message_buffer_lock);

  /* Check if buffer has space, expand if needed */
  if (session->message_buffer_size + len > session->message_buffer_capacity) {
    size_t new_capacity = session->message_buffer_capacity * 2;
    while (new_capacity < session->message_buffer_size + len) {
      new_capacity *= 2;
    }

    char *new_buffer = ast_realloc(session->message_buffer, new_capacity);
    if (!new_buffer) {
      ast_log(LOG_ERROR,
              "Failed to expand message buffer from %zu to %zu bytes\n",
              session->message_buffer_capacity, new_capacity);
      session->message_buffer_size = 0;
      ast_mutex_unlock(&session->message_buffer_lock);
      return -1;
    }

    session->message_buffer = new_buffer;
    session->message_buffer_capacity = new_capacity;
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "Expanded message buffer to %zu bytes\n", new_capacity);
  }

  /* Add data to buffer */
  memcpy(session->message_buffer + session->message_buffer_size, data, len);
  session->message_buffer_size += len;

  if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
        "Added %zu bytes to message buffer (total: %zu, final: %s)\n", len,
        session->message_buffer_size, is_final ? "yes" : "no");
  }

  /* If this is the final fragment, process the complete message */
  if (is_final) {
    process_complete_message(session);
  }

  ast_mutex_unlock(&session->message_buffer_lock);
  return 0;
}

/*! \brief Process complete reassembled message */
static void process_complete_message(struct elevenlabs_session *session) {
  char *complete_message;

  if (!session || session->message_buffer_size == 0) {
    return;
  }

  /* Null-terminate the message */
  session->message_buffer[session->message_buffer_size] = '\0';

  /* Create a copy for processing */
  complete_message = ast_strdupa(session->message_buffer);

  /* Note: We don't check session->config here since we're in a critical path
   * and this is informational only. The handle_websocket_message will do proper
   * logging */

  /* Reset buffer for next message */
  session->message_buffer_size = 0;

  /* Process the complete message */
  handle_websocket_message(session, complete_message);
}

/*! \brief Send conversation initiation message with dynamic_variables
 * Sends client_caller_id via dynamic_variables in
 * conversation_initiation_client_data message
 */
static void send_conversation_initiation(struct elevenlabs_session *session) {
  cJSON *json;
  cJSON *dynamic_variables;
  char *message;
  int connection_ok = 0;

  if (!session) {
    return;
  }

  /* Check connection state under lock */
  ast_mutex_lock(&session->session_lock);
  connection_ok = (session->session_active && session->ws_connected &&
                   session->ws_connection);
  ast_mutex_unlock(&session->session_lock);

  if (!connection_ok) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
          "WebSocket not connected, skipping conversation initiation\n");
    }
    return;
  }

  /* Create conversation_initiation_client_data message */
  json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "type", "conversation_initiation_client_data");

  /* Add dynamic_variables with client_caller_id, client_called_id, callid, and
   * X-* headers */
  dynamic_variables = cJSON_CreateObject();

  if (session->config->send_caller_number &&
      !ast_strlen_zero(session->caller_id)) {
    cJSON_AddStringToObject(dynamic_variables, "client_caller_id",
                            session->caller_id);
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Adding client_caller_id to dynamic_variables: %s\n",
                     session->caller_id);
    }
  } else {
    cJSON_AddStringToObject(dynamic_variables, "client_caller_id", "Unknown");
  }

  /* Add client_called_id (DNID) */
  if (!ast_strlen_zero(session->called_id)) {
    cJSON_AddStringToObject(dynamic_variables, "client_called_id",
                            session->called_id);
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Adding client_called_id to dynamic_variables: %s\n",
                     session->called_id);
    }
  } else {
    cJSON_AddStringToObject(dynamic_variables, "client_called_id", "Unknown");
  }

  /* Add callid (Asterisk unique channel identifier) */
  if (session->channel) {
    const char *uniqueid = ast_channel_uniqueid(session->channel);
    if (!ast_strlen_zero(uniqueid)) {
      cJSON_AddStringToObject(dynamic_variables, "callid", uniqueid);
      if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
        ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                       "Adding callid to dynamic_variables: %s\n", uniqueid);
      }
    }
  }

  /* Add X-* custom headers */
  if (session->custom_headers) {
    struct custom_header *hdr = session->custom_headers;
    while (hdr) {
      cJSON_AddStringToObject(dynamic_variables, hdr->name, hdr->value);
      if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
        ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                       "Adding custom header to dynamic_variables: %s = %s\n",
                       hdr->name, hdr->value);
      }
      hdr = hdr->next;
    }
  }

  cJSON_AddItemToObject(json, "dynamic_variables", dynamic_variables);

  message = cJSON_PrintUnformatted(json);
  if (message) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
        "Sending conversation initiation message with dynamic_variables\n");
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ast_log(
          LOG_DEBUG,
          "=== CONVERSATION INITIATION MESSAGE ===\n%s\n=== END MESSAGE ===\n",
          message);
    }
    if (send_websocket_message(session, message) < 0) {
      ast_log(LOG_WARNING, "Failed to send conversation initiation message\n");
    }
    ast_std_free(
        message); /* cJSON_PrintUnformatted uses malloc, not ast_malloc */
  }

  cJSON_Delete(json);
}

/*! \brief Create audio queue */
static struct audio_queue *create_audio_queue(void) {
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
static void destroy_audio_queue(struct audio_queue *queue) {
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
static int audio_queue_add(struct audio_queue *queue, const unsigned char *data,
                           size_t length, int event_id) {
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
static struct audio_queue_item *audio_queue_get(struct audio_queue *queue) {
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
static char *base64_encode(const unsigned char *data, size_t input_length) {
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
static unsigned char *base64_decode(const char *data, size_t *output_length) {
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

/*! \brief Get human-readable error string for libwebsockets error codes */
static const char *get_lws_error_string(int error_code) {
  switch (error_code) {
  case -1:
    return "Generic error";
  case -2:
    return "Connection refused";
  case -3:
    return "Connection timeout";
  case -4:
    return "SSL/TLS handshake failed";
  default:
    return "Unknown error";
  }
}

/*! \brief Check if connection should be retried based on error and attempt
 * count */
static int should_retry_connection(struct elevenlabs_session *session) {
  if (!session) {
    return 0;
  }

  /* Don't retry if session is being shut down */
  if (!session->session_active || !session->websocket_running) {
    return 0;
  }

  /* Don't retry if we've exceeded max attempts */
  if (session->connection_attempts >= session->max_connection_attempts) {
    ast_log(LOG_ERROR, "Maximum connection attempts (%d) exceeded\n",
            session->max_connection_attempts);
    return 0;
  }

  /* Don't retry too frequently */
  time_t now = time(NULL);
  if (now - session->last_connection_attempt <
      5) { /* Wait at least 5 seconds */
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(
          ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
          "Too soon to retry connection (last attempt %ld seconds ago)\n",
          now - session->last_connection_attempt);
    }
    return 0;
  }

  return 1;
}

/*! \brief Handle connection failure with appropriate logging and state updates
 */
static void handle_connection_failure(struct elevenlabs_session *session,
                                      const char *error_msg) {
  if (!session || !error_msg) {
    return;
  }

  ast_log(LOG_ERROR, "WebSocket connection failure: %s (attempt %d/%d)\n",
          error_msg, session->connection_attempts + 1,
          session->max_connection_attempts);

  /* Update session state */
  ast_mutex_lock(&session->session_lock);
  session->ws_connected = 0;
  if (session->ws_connection) {
    session->ws_connection = NULL;
  }
  session->connection_attempts++;
  session->last_connection_attempt = time(NULL);
  ast_mutex_unlock(&session->session_lock);

  /* Signal other threads about the failure */
  if (session->receive_queue) {
    ast_cond_signal(&session->receive_queue->cond);
  }
}

/*! \brief Establish WebSocket connection with timeout and retry handling */
static int establish_websocket_connection(struct elevenlabs_session *session) {
  struct lws_client_connect_info connect_info;
  time_t connection_start, now;
  int n;
  int connection_established = 0;

  if (!session || !session->ws_context) {
    return -1;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Establishing WebSocket connection (attempt %d/%d)\n",
                 session->connection_attempts + 1,
                 session->max_connection_attempts);

  /* Clean up any existing connection */
  ast_mutex_lock(&session->session_lock);
  if (session->ws_connection) {
    lws_close_reason(session->ws_connection, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
    session->ws_connection = NULL;
  }
  session->ws_connected = 0;
  ast_mutex_unlock(&session->session_lock);

  /* Use the path as-is - client_caller_id is now sent via dynamic_variables */
  char full_path[1024];
  ast_copy_string(full_path, session->ws_path, sizeof(full_path));

  /* Set up connection info */
  memset(&connect_info, 0, sizeof(connect_info));
  connect_info.context = session->ws_context;
  connect_info.address = session->ws_host;
  connect_info.port = session->ws_port;
  connect_info.path = full_path;
  connect_info.host = session->ws_host;
  connect_info.origin = session->ws_host;
  connect_info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED |
                                LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
  connect_info.protocol = "convai";
  connect_info.pwsi = &session->ws_connection;

  /* Initiate connection */
  session->ws_connection = lws_client_connect_via_info(&connect_info);
  if (!session->ws_connection) {
    handle_connection_failure(session,
                              "Failed to initiate WebSocket connection");
    return -1;
  }

  ELEVENLABS_LOG(
      ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
      "WebSocket connection initiated, waiting for establishment...\n");

  /* Wait for connection with timeout */
  connection_start = time(NULL);
  while (!connection_established && session->websocket_running &&
         session->session_active) {
    n = lws_service(session->ws_context, 100); /* 100ms timeout per iteration */
    if (n < 0) {
      handle_connection_failure(session, get_lws_error_string(n));
      return -1;
    }

    /* Check if connection is established */
    ast_mutex_lock(&session->session_lock);
    connection_established = session->ws_connected;
    ast_mutex_unlock(&session->session_lock);

    /* Check for timeout */
    now = time(NULL);
    if (now - connection_start > session->connection_timeout_seconds) {
      handle_connection_failure(session, "Connection establishment timeout");
      return -1;
    }
  }

  if (connection_established) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "WebSocket connection established successfully\n");
    session->connection_attempts = 0; /* Reset counter on success */
    return 0;
  } else {
    handle_connection_failure(
        session, "Connection establishment failed - session terminated");
    return -1;
  }
}

/*! \brief Add audio data to send buffer */
static int add_audio_to_send_buffer(struct elevenlabs_session *session,
                                    const unsigned char *data, size_t length) {
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
static int should_send_buffer(struct elevenlabs_session *session) {
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
static int send_buffered_audio(struct elevenlabs_session *session) {
  unsigned char *buffer_copy;
  size_t buffer_size;
  int connection_ok = 0;

  if (!session) {
    return -1;
  }

  /* Check connection state before proceeding */
  ast_mutex_lock(&session->session_lock);
  connection_ok = (session->session_active && session->ws_connected &&
                   session->ws_connection);
  ast_mutex_unlock(&session->session_lock);

  if (!connection_ok) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "WebSocket not connected, skipping buffered audio send\n");
    }
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
  if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
        "Sending buffered audio: %zu bytes (%d ms worth of μ-law audio)\n",
        buffer_size, (int)(buffer_size / 8)); /* 8 bytes per ms at 8kHz μ-law */
  }
  int result = send_audio_to_websocket(session, buffer_copy, buffer_size);

  ast_free(buffer_copy);
  return result;
}

/* ============================================================================
 * STT (Speech-to-Text) Implementation - 16kHz audio streaming
 * ============================================================================
 */

/*! \brief Initialize STT translator (8kHz ulaw -> 16kHz slin16) */
static int init_stt_translator(struct elevenlabs_session *session) {
  if (!session) {
    return -1;
  }

  /* Get source format (ulaw 8kHz) */
  session->stt_src_format = ast_format_cache_get("ulaw");
  if (!session->stt_src_format) {
    ast_log(LOG_ERROR, "Failed to get ulaw format for STT translator\n");
    return -1;
  }

  /* Get destination format (slin16 - 16kHz signed linear) */
  session->stt_dst_format = ast_format_cache_get("slin16");
  if (!session->stt_dst_format) {
    ast_log(LOG_ERROR, "Failed to get slin16 format for STT translator\n");
    ao2_ref(session->stt_src_format, -1);
    session->stt_src_format = NULL;
    return -1;
  }

  /* Build translation path from ulaw to slin16 */
  session->stt_translator = ast_translator_build_path(session->stt_dst_format,
                                                      session->stt_src_format);
  if (!session->stt_translator) {
    ast_log(LOG_ERROR, "Failed to build translator path from ulaw to slin16 - "
                       "check that required codec modules are loaded\n");
    ao2_ref(session->stt_src_format, -1);
    ao2_ref(session->stt_dst_format, -1);
    session->stt_src_format = NULL;
    session->stt_dst_format = NULL;
    return -1;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "STT translator initialized: ulaw (8kHz) -> slin16 (16kHz)\n");
  return 0;
}

/*! \brief Destroy STT translator */
static void destroy_stt_translator(struct elevenlabs_session *session) {
  if (!session) {
    return;
  }

  if (session->stt_translator) {
    ast_translator_free_path(session->stt_translator);
    session->stt_translator = NULL;
  }

  if (session->stt_src_format) {
    ao2_ref(session->stt_src_format, -1);
    session->stt_src_format = NULL;
  }

  if (session->stt_dst_format) {
    ao2_ref(session->stt_dst_format, -1);
    session->stt_dst_format = NULL;
  }
}

/*! \brief Parse STT WebSocket URL into components */
static int parse_stt_websocket_url(const char *url, char *host, char *path,
                                   int *port, int *use_ssl) {
  const char *protocol_end, *host_start, *host_end, *port_start, *path_start;
  char port_str[16];

  if (!url || !host || !path || !port || !use_ssl) {
    return -1;
  }

  /* Determine protocol and SSL usage */
  if (strncmp(url, "wss://", 6) == 0) {
    *use_ssl = 1;
    protocol_end = url + 6;
    *port = 443; /* Default WSS port */
  } else if (strncmp(url, "ws://", 5) == 0) {
    *use_ssl = 0;
    protocol_end = url + 5;
    *port = 80; /* Default WS port */
  } else {
    ast_log(
        LOG_ERROR,
        "Invalid STT WebSocket URL protocol (must be ws:// or wss://): %s\n",
        url);
    return -1;
  }

  host_start = protocol_end;

  /* Find end of host (either : for port or / for path) */
  host_end = strchr(host_start, ':');
  if (!host_end) {
    host_end = strchr(host_start, '/');
  }

  if (!host_end) {
    /* No port and no path - just host */
    strcpy(host, host_start);
    strcpy(path, "/");
    return 0;
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
      /* Port but no path */
      *port = atoi(port_start);
      strcpy(path, "/");
      return 0;
    }

    size_t port_len = path_start - port_start;
    if (port_len >= sizeof(port_str)) {
      return -1;
    }
    memcpy(port_str, port_start, port_len);
    port_str[port_len] = '\0';
    *port = atoi(port_str);
  } else {
    path_start = host_end;
  }

  /* Extract path */
  strcpy(path, path_start);

  return 0;
}

/*! \brief STT WebSocket callback handler */
static int stt_websocket_callback(struct lws *wsi,
                                  enum lws_callback_reasons reason, void *user,
                                  void *in, size_t len) {
  struct elevenlabs_session *session = NULL;

  /* Get session for relevant callbacks */
  if (wsi && lws_get_context(wsi)) {
    session =
        (struct elevenlabs_session *)lws_context_user(lws_get_context(wsi));
  }

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    if (session) {
      ast_mutex_lock(&session->session_lock);
      session->stt_ws_connected = 1;
      ast_mutex_unlock(&session->session_lock);
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "STT WebSocket connection established for channel %s\n",
                     session->channel_name);
    }
    break;

  case LWS_CALLBACK_CLIENT_RECEIVE:
    if (session && in && len > 0) {
      /* Create null-terminated copy of received message */
      char *message = ast_strndup(in, len);
      if (message) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG, "STT response: %s\n",
                         message);
        }

        /* Handle transcription message and forward to ElevenLabs */
        handle_stt_transcription(session, message);

        ast_free(message);
      }
    }
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    if (session) {
      ast_log(LOG_WARNING, "STT WebSocket connection error for channel %s\n",
              session->channel_name);
      ast_mutex_lock(&session->session_lock);
      session->stt_ws_connected = 0;
      session->stt_connection_attempts++;
      ast_mutex_unlock(&session->session_lock);
    }
    break;

  case LWS_CALLBACK_CLOSED:
  case 30: /* The close event in some libwebsockets versions */
    if (session) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "STT WebSocket closed for channel %s\n",
                     session->channel_name);
      ast_mutex_lock(&session->session_lock);
      session->stt_ws_connected = 0;
      session->stt_ws_connection = NULL;
      ast_mutex_unlock(&session->session_lock);
    }
    break;

  default:
    break;
  }

  return 0;
}

/*! \brief STT WebSocket thread function */
static void *stt_websocket_thread_func(void *arg) {
  struct elevenlabs_session *session = arg;
  struct lws_context_creation_info info;
  int n;

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "STT WebSocket thread starting for channel %s\n",
                 session->channel_name);

  /* Create WebSocket context for STT */
  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = stt_protocols;
  info.gid = -1;
  info.uid = -1;
  if (session->stt_use_ssl) {
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  }
  info.user = session;

  session->stt_ws_context = lws_create_context(&info);
  if (!session->stt_ws_context) {
    ast_log(LOG_ERROR, "Failed to create STT WebSocket context\n");
    return NULL;
  }

  /* Establish connection */
  if (establish_stt_websocket_connection(session) != 0) {
    ast_log(LOG_ERROR, "Failed to establish STT WebSocket connection\n");
    lws_context_destroy(session->stt_ws_context);
    session->stt_ws_context = NULL;
    return NULL;
  }

  /* Main service loop */
  while (session->stt_websocket_running && session->session_active) {
    n = lws_service(session->stt_ws_context, 100);
    if (n < 0) {
      ast_log(LOG_WARNING, "STT WebSocket service error: %d\n", n);

      /* Try to reconnect if session is still active */
      if (session->session_active && session->stt_websocket_running) {
        if (session->stt_connection_attempts <
            session->max_connection_attempts) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                         "Attempting STT WebSocket reconnection...\n");
          usleep(1000000); /* Wait 1 second before retry */
          if (establish_stt_websocket_connection(session) == 0) {
            ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                           "STT WebSocket reconnected\n");
            continue;
          }
        }
      }
      break;
    }
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "STT WebSocket thread exiting for channel %s\n",
                 session->channel_name);
  return NULL;
}

/*! \brief Establish STT WebSocket connection */
static int
establish_stt_websocket_connection(struct elevenlabs_session *session) {
  struct lws_client_connect_info connect_info;
  time_t connection_start, now;
  int n;
  int connection_established = 0;

  if (!session || !session->stt_ws_context) {
    return -1;
  }

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Establishing STT WebSocket connection to %s:%d%s (SSL: %s)\n",
                 session->stt_ws_host, session->stt_ws_port,
                 session->stt_ws_path, session->stt_use_ssl ? "yes" : "no");

  /* Clean up existing connection */
  ast_mutex_lock(&session->session_lock);
  if (session->stt_ws_connection) {
    lws_close_reason(session->stt_ws_connection, LWS_CLOSE_STATUS_NORMAL, NULL,
                     0);
    session->stt_ws_connection = NULL;
  }
  session->stt_ws_connected = 0;
  ast_mutex_unlock(&session->session_lock);

  /* Set up connection info */
  memset(&connect_info, 0, sizeof(connect_info));
  connect_info.context = session->stt_ws_context;
  connect_info.address = session->stt_ws_host;
  connect_info.port = session->stt_ws_port;
  connect_info.path = session->stt_ws_path;
  connect_info.host = session->stt_ws_host;
  connect_info.origin = session->stt_ws_host;
  if (session->stt_use_ssl) {
    connect_info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED |
                                  LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
  }
  connect_info.protocol = "stt";
  connect_info.pwsi = &session->stt_ws_connection;

  /* Initiate connection */
  session->stt_ws_connection = lws_client_connect_via_info(&connect_info);
  if (!session->stt_ws_connection) {
    ast_log(LOG_ERROR, "Failed to initiate STT WebSocket connection\n");
    return -1;
  }

  /* Wait for connection with timeout */
  connection_start = time(NULL);
  while (!connection_established && session->stt_websocket_running &&
         session->session_active) {
    n = lws_service(session->stt_ws_context, 100);
    if (n < 0) {
      ast_log(LOG_ERROR, "STT WebSocket service error during connection: %d\n",
              n);
      return -1;
    }

    ast_mutex_lock(&session->session_lock);
    connection_established = session->stt_ws_connected;
    ast_mutex_unlock(&session->session_lock);

    now = time(NULL);
    if (now - connection_start > session->connection_timeout_seconds) {
      ast_log(LOG_ERROR, "STT WebSocket connection timeout\n");
      return -1;
    }
  }

  if (connection_established) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "STT WebSocket connection established successfully\n");
    session->stt_connection_attempts = 0;
    return 0;
  }

  return -1;
}

/*! \brief Translate audio frame and buffer for STT (8kHz -> 16kHz) */
static int translate_and_buffer_for_stt(struct elevenlabs_session *session,
                                        struct ast_frame *frame) {
  struct ast_frame *translated_frame;

  if (!session || !frame || !session->stt_translator) {
    return -1;
  }

  /* Translate the frame from ulaw (8kHz) to slin16 (16kHz) */
  translated_frame = ast_translate(session->stt_translator, frame, 0);
  if (!translated_frame) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Failed to translate frame for STT\n");
    }
    return -1;
  }

  /* Add translated audio to STT buffer */
  if (translated_frame->data.ptr && translated_frame->datalen > 0) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Translated frame for STT: %d bytes (16kHz slin16)\n",
                     translated_frame->datalen);
    }
    add_audio_to_stt_buffer(session, translated_frame->data.ptr,
                            translated_frame->datalen);
  }

  /* Free the translated frame */
  ast_frfree(translated_frame);

  return 0;
}

/*! \brief Add audio data to STT send buffer */
static int add_audio_to_stt_buffer(struct elevenlabs_session *session,
                                   const unsigned char *data, size_t length) {
  if (!session || !data || length == 0 || !session->stt_send_buffer) {
    return -1;
  }

  ast_mutex_lock(&session->stt_send_buffer_lock);

  /* Check if buffer has enough space */
  if (session->stt_send_buffer_size + length >
      session->stt_send_buffer_capacity) {
    /* Buffer full - send current buffer first */
    ast_mutex_unlock(&session->stt_send_buffer_lock);
    send_stt_buffered_audio(session);
    ast_mutex_lock(&session->stt_send_buffer_lock);
  }

  /* Add data to buffer */
  if (session->stt_send_buffer_size + length <=
      session->stt_send_buffer_capacity) {
    memcpy(session->stt_send_buffer + session->stt_send_buffer_size, data,
           length);
    session->stt_send_buffer_size += length;
  }

  ast_mutex_unlock(&session->stt_send_buffer_lock);
  return 0;
}

/*! \brief Check if STT buffer should be sent */
static int should_send_stt_buffer(struct elevenlabs_session *session) {
  struct timeval now, diff;
  int elapsed_ms;

  if (!session || !session->stt_send_buffer) {
    return 0;
  }

  ast_mutex_lock(&session->stt_send_buffer_lock);

  /* Check if buffer is full */
  if (session->stt_send_buffer_size >= session->stt_send_buffer_capacity) {
    ast_mutex_unlock(&session->stt_send_buffer_lock);
    return 1;
  }

  /* Check if enough time has passed */
  gettimeofday(&now, NULL);
  timersub(&now, &session->stt_last_send_time, &diff);
  elapsed_ms = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);

  ast_mutex_unlock(&session->stt_send_buffer_lock);

  return (elapsed_ms >= session->stt_send_buffer_duration_ms);
}

/*! \brief Handle STT transcription and forward to ElevenLabs as
 * contextual_update */
static void handle_stt_transcription(struct elevenlabs_session *session,
                                     const char *message) {
  cJSON *json;
  cJSON *type_item;
  cJSON *text_item;
  cJSON *segment_id_item;

  if (!session || !message) {
    return;
  }

  json = cJSON_Parse(message);
  if (!json) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "STT: Failed to parse JSON message: %s\n", message);
    }
    return;
  }

  /* Check message type */
  type_item = cJSON_GetObjectItem(json, "type");
  if (!type_item || !cJSON_IsString(type_item)) {
    cJSON_Delete(json);
    return;
  }

  /* Handle transcription messages */
  if (strcmp(type_item->valuestring, "transcription") == 0) {
    text_item = cJSON_GetObjectItem(json, "text");
    segment_id_item = cJSON_GetObjectItem(json, "segment_id");

    if (text_item && cJSON_IsString(text_item) &&
        !ast_strlen_zero(text_item->valuestring)) {
      int segment_id = segment_id_item && cJSON_IsNumber(segment_id_item)
                           ? segment_id_item->valueint
                           : -1;

      ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                     "STT transcription received (segment_id=%d): %s\n",
                     segment_id, text_item->valuestring);

      /* Forward transcription to ElevenLabs as contextual_update */
      if (send_contextual_update_to_elevenlabs(session,
                                               text_item->valuestring) == 0) {
        if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
          ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                         "STT transcription forwarded to ElevenLabs as "
                         "contextual_update\n");
        }
      } else {
        ast_log(LOG_WARNING,
                "Failed to forward STT transcription to ElevenLabs\n");
      }
    }
  }

  cJSON_Delete(json);
}

/*! \brief Send contextual_update message to ElevenLabs WebSocket */
static int
send_contextual_update_to_elevenlabs(struct elevenlabs_session *session,
                                     const char *text) {
  cJSON *json;
  char *message;
  char *formatted_text;
  int result = -1;
  int connection_ok = 0;

  if (!session || !text || ast_strlen_zero(text)) {
    return -1;
  }

  /* Check ElevenLabs connection state */
  ast_mutex_lock(&session->session_lock);
  connection_ok = (session->session_active && session->ws_connected &&
                   session->ws_connection);
  ast_mutex_unlock(&session->session_lock);

  if (!connection_ok) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "ElevenLabs WebSocket not connected, cannot send "
                     "contextual_update\n");
    }
    return -1;
  }

  /* Format text as "alternative transcript: <text>" */
  if (ast_asprintf(&formatted_text, "alternative transcript: %s", text) < 0) {
    ast_log(LOG_ERROR, "Failed to allocate memory for formatted text\n");
    return -1;
  }

  /* Create contextual_update message */
  json = cJSON_CreateObject();
  if (!json) {
    ast_log(LOG_ERROR, "Failed to create JSON object for contextual_update\n");
    ast_free(formatted_text);
    return -1;
  }

  cJSON_AddStringToObject(json, "type", "contextual_update");
  cJSON_AddStringToObject(json, "text", formatted_text);

  message = cJSON_Print(json);
  if (message) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "Sending contextual_update to ElevenLabs: %s\n", message);
    }

    result = send_websocket_message(session, message);
    ast_free(message);
  }

  cJSON_Delete(json);
  ast_free(formatted_text);
  return result;
}

/*! \brief Send buffered 16kHz audio to STT WebSocket */
static int send_stt_buffered_audio(struct elevenlabs_session *session) {
  unsigned char *buffer_copy;
  size_t buffer_size;
  int connection_ok = 0;
  unsigned char *ws_buf;

  if (!session || !session->stt_send_buffer) {
    return -1;
  }

  /* Check connection state */
  ast_mutex_lock(&session->session_lock);
  connection_ok = (session->session_active && session->stt_ws_connected &&
                   session->stt_ws_connection);
  ast_mutex_unlock(&session->session_lock);

  if (!connection_ok) {
    if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
      ELEVENLABS_LOG(ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
                     "STT WebSocket not connected, skipping audio send\n");
    }
    return -1;
  }

  ast_mutex_lock(&session->stt_send_buffer_lock);

  if (session->stt_send_buffer_size == 0) {
    ast_mutex_unlock(&session->stt_send_buffer_lock);
    return 0;
  }

  /* Make a copy of the buffer */
  buffer_size = session->stt_send_buffer_size;
  buffer_copy = ast_calloc(1, buffer_size);
  if (!buffer_copy) {
    ast_mutex_unlock(&session->stt_send_buffer_lock);
    return -1;
  }

  memcpy(buffer_copy, session->stt_send_buffer, buffer_size);

  /* Reset buffer */
  session->stt_send_buffer_size = 0;
  gettimeofday(&session->stt_last_send_time, NULL);

  ast_mutex_unlock(&session->stt_send_buffer_lock);

  /* Send raw binary audio (16-bit PCM at 16kHz) to STT WebSocket */
  if (elevenlabs_loglevel >= ELEVENLABS_LOG_DEBUG) {
    ELEVENLABS_LOG(
        ELEVENLABS_LOG_DEBUG, LOG_DEBUG,
        "Sending STT audio: %zu bytes (16kHz, 16-bit PCM, %d ms)\n",
        buffer_size,
        (int)(buffer_size / 32)); /* 32 bytes per ms at 16kHz 16-bit */
  }

  /* Allocate buffer with LWS_PRE space */
  ws_buf = ast_calloc(1, LWS_PRE + buffer_size);
  if (!ws_buf) {
    ast_free(buffer_copy);
    return -1;
  }

  memcpy(ws_buf + LWS_PRE, buffer_copy, buffer_size);

  /* Send as binary WebSocket message */
  ast_mutex_lock(&session->session_lock);
  if (session->stt_ws_connection && session->stt_ws_connected) {
    if (lws_write(session->stt_ws_connection, ws_buf + LWS_PRE, buffer_size,
                  LWS_WRITE_BINARY) < 0) {
      ast_log(LOG_WARNING, "Failed to send audio to STT WebSocket\n");
      session->stt_ws_connected = 0;
    }
  }
  ast_mutex_unlock(&session->session_lock);

  ast_free(ws_buf);
  ast_free(buffer_copy);

  return 0;
}

/* ============================================================================
 * End of STT Implementation
 * ============================================================================
 */

/*! \brief Load configuration from elevenlabs.conf */
static int load_config(void) {
  struct ast_config *cfg;
  struct ast_variable *var;
  struct ast_flags config_flags = {0};
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
    if (strcmp(var->name, "loglevel") == 0) {
      if (strcasecmp(var->value, "off") == 0 ||
          strcasecmp(var->value, "none") == 0) {
        elevenlabs_loglevel = ELEVENLABS_LOG_NONE;
      } else if (strcasecmp(var->value, "error") == 0) {
        elevenlabs_loglevel = ELEVENLABS_LOG_ERROR;
      } else if (strcasecmp(var->value, "warning") == 0 ||
                 strcasecmp(var->value, "warn") == 0) {
        elevenlabs_loglevel = ELEVENLABS_LOG_WARNING;
      } else if (strcasecmp(var->value, "notice") == 0) {
        elevenlabs_loglevel = ELEVENLABS_LOG_NOTICE;
      } else if (strcasecmp(var->value, "debug") == 0) {
        elevenlabs_loglevel = ELEVENLABS_LOG_DEBUG;

      } else {
        ast_log(LOG_WARNING, "Unknown loglevel '%s', using 'warning'\n",
                var->value);
        elevenlabs_loglevel = ELEVENLABS_LOG_WARNING;
      }
    } else if (strcmp(var->name, "redis_host") == 0) {
      ast_copy_string(redis_host, var->value, sizeof(redis_host));
    } else if (strcmp(var->name, "redis_port") == 0) {
      int port = atoi(var->value);
      if (port > 0 && port <= 65535) {
        redis_port = port;
      } else {
        ast_log(LOG_WARNING, "Invalid redis_port '%s', using default 6379\n",
                var->value);
      }
    } else if (strcmp(var->name, "redis_db") == 0) {
      int db = atoi(var->value);
      if (db >= 0 && db <= 15) {
        redis_db = db;
      } else {
        ast_log(LOG_WARNING, "Invalid redis_db '%s', using default 0\n",
                var->value);
      }
    } else if (strcmp(var->name, "redis_password") == 0) {
      ast_copy_string(redis_password, var->value, sizeof(redis_password));
    } else if (strcmp(var->name, "redis_enabled") == 0) {
      redis_enabled = ast_true(var->value);
    } else if (strcmp(var->name, "redis_ingester_queue") == 0) {
      ast_copy_string(redis_ingester_queue, var->value,
                      sizeof(redis_ingester_queue));
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

    ast_copy_string(agent_configs[i].name, category,
                    sizeof(agent_configs[i].name));

    /* Set defaults */
    agent_configs[i].send_caller_number = 0;
    agent_configs[i].buffer_duration_ms = 240;
    agent_configs[i].max_connection_attempts = 3;
    agent_configs[i].connection_timeout_seconds = 10;

    /* STT defaults */
    agent_configs[i].enable_stt = 0;
    agent_configs[i].stt_websocket_url[0] = '\0';
    agent_configs[i].stt_buffer_duration_ms =
        0; /* 0 = same as elevenlabs buffer */

    var = ast_variable_browse(cfg, category);
    while (var) {
      if (strcmp(var->name, "elevenlabs_api_key") == 0) {
        ast_copy_string(agent_configs[i].api_key, var->value,
                        sizeof(agent_configs[i].api_key));
      } else if (strcmp(var->name, "elevenlabs_get_signed_url_endpoint") == 0) {
        ast_copy_string(agent_configs[i].endpoint_url, var->value,
                        sizeof(agent_configs[i].endpoint_url));
      } else if (strcmp(var->name, "elevenlabs_agent_id") == 0) {
        ast_copy_string(agent_configs[i].agent_id, var->value,
                        sizeof(agent_configs[i].agent_id));
      } else if (strcmp(var->name, "elevenlabs_send_caller_number") == 0) {
        agent_configs[i].send_caller_number = ast_true(var->value);
      } else if (strcmp(var->name, "elevenlabs_audio_frames_buffer_ms") == 0) {
        agent_configs[i].buffer_duration_ms = atoi(var->value);
      } else if (strcmp(var->name, "max_connection_attempts") == 0) {
        int attempts = atoi(var->value);
        if (attempts > 0 && attempts <= 10) {
          agent_configs[i].max_connection_attempts = attempts;
        } else {
          ast_log(LOG_WARNING,
                  "Invalid max_connection_attempts '%s' for agent '%s', using "
                  "default\n",
                  var->value, agent_configs[i].name);
        }
      } else if (strcmp(var->name, "connection_timeout_seconds") == 0) {
        int timeout = atoi(var->value);
        if (timeout > 0 && timeout <= 60) {
          agent_configs[i].connection_timeout_seconds = timeout;
        } else {
          ast_log(LOG_WARNING,
                  "Invalid connection_timeout_seconds '%s' for agent '%s', "
                  "using default\n",
                  var->value, agent_configs[i].name);
        }
      } else if (strcmp(var->name, "enable_stt") == 0) {
        agent_configs[i].enable_stt = ast_true(var->value);
      } else if (strcmp(var->name, "stt_websocket_url") == 0) {
        ast_copy_string(agent_configs[i].stt_websocket_url, var->value,
                        sizeof(agent_configs[i].stt_websocket_url));
      } else if (strcmp(var->name, "stt_buffer_duration_ms") == 0) {
        agent_configs[i].stt_buffer_duration_ms = atoi(var->value);
      }
      var = var->next;
    }

    /* Validate required parameters */
    if (ast_strlen_zero(agent_configs[i].api_key) ||
        ast_strlen_zero(agent_configs[i].endpoint_url) ||
        ast_strlen_zero(agent_configs[i].agent_id)) {
      ast_log(LOG_ERROR, "Agent '%s' missing required parameters\n",
              agent_configs[i].name);
    }

    i++;
  }

  ast_mutex_unlock(&config_lock);
  ast_config_destroy(cfg);

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Redis: %s (host=%s, port=%d, db=%d, ingester_queue=%s)\n",
                 redis_enabled ? "enabled" : "disabled", redis_host, redis_port,
                 redis_db, redis_ingester_queue);
  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "Loaded %d agent configurations\n", agent_count);
  return 0;
}

/*! \\brief Helper function to get loglevel name */
static const char *loglevel_to_string(int level) {
  switch (level) {
  case ELEVENLABS_LOG_NONE:
    return "off";
  case ELEVENLABS_LOG_ERROR:
    return "error";
  case ELEVENLABS_LOG_WARNING:
    return "warning";
  case ELEVENLABS_LOG_NOTICE:
    return "notice";
  case ELEVENLABS_LOG_DEBUG:
    return "debug";
  default:
    return "unknown";
  }
}

/*! \\brief CLI command: elevenlabs loglevel */
static char *handle_elevenlabs_loglevel(struct ast_cli_entry *e, int cmd,
                                        struct ast_cli_args *a) {
  switch (cmd) {
  case CLI_INIT:
    e->command = "elevenlabs loglevel";
    e->usage = "Usage: elevenlabs loglevel [off|error|warning|notice|debug]\n"
               "       Set or show ElevenLabs module log level\n"
               "       Levels (from least to most verbose):\n"
               "         off     - No logging from module\n"
               "         error   - Only errors\n"
               "         warning - Errors and warnings\n"
               "         notice  - Errors, warnings, and notices (default)\n"
               "         debug   - All messages including debug\n";
    return NULL;
  case CLI_GENERATE:
    if (a->pos == 2) {
      static const char *const levels[] = {"off",    "error", "warning",
                                           "notice", "debug", NULL};
      return ast_cli_complete(a->word, levels, a->n);
    }
    return NULL;
  }

  if (a->argc == 2) {
    /* Show current level */
    ast_cli(a->fd, "ElevenLabs loglevel: %s\n",
            loglevel_to_string(elevenlabs_loglevel));
    return CLI_SUCCESS;
  }

  if (a->argc != 3) {
    return CLI_SHOWUSAGE;
  }

  if (strcasecmp(a->argv[2], "off") == 0 ||
      strcasecmp(a->argv[2], "none") == 0) {
    elevenlabs_loglevel = ELEVENLABS_LOG_NONE;
  } else if (strcasecmp(a->argv[2], "error") == 0) {
    elevenlabs_loglevel = ELEVENLABS_LOG_ERROR;
  } else if (strcasecmp(a->argv[2], "warning") == 0 ||
             strcasecmp(a->argv[2], "warn") == 0) {
    elevenlabs_loglevel = ELEVENLABS_LOG_WARNING;
  } else if (strcasecmp(a->argv[2], "notice") == 0) {
    elevenlabs_loglevel = ELEVENLABS_LOG_NOTICE;
  } else if (strcasecmp(a->argv[2], "debug") == 0) {
    elevenlabs_loglevel = ELEVENLABS_LOG_DEBUG;
  } else {
    return CLI_SHOWUSAGE;
  }

  ast_cli(a->fd, "ElevenLabs loglevel set to: %s\n",
          loglevel_to_string(elevenlabs_loglevel));
  return CLI_SUCCESS;
}

/*! \\brief CLI command: elevenlabs show settings */
static char *handle_elevenlabs_show(struct ast_cli_entry *e, int cmd,
                                    struct ast_cli_args *a) {
  int i;

  switch (cmd) {
  case CLI_INIT:
    e->command = "elevenlabs show settings";
    e->usage = "Usage: elevenlabs show settings\n"
               "       Show ElevenLabs module settings\n";
    return NULL;
  case CLI_GENERATE:
    return NULL;
  }

  if (a->argc != 3) {
    return CLI_SHOWUSAGE;
  }

  ast_cli(a->fd, "\n=== ElevenLabs Module Settings ===\n");
  ast_cli(a->fd, "Log level: %s\n", loglevel_to_string(elevenlabs_loglevel));

  ast_mutex_lock(&config_lock);
  ast_cli(a->fd, "\nConfigured agents: %d\n", agent_count);
  for (i = 0; i < agent_count; i++) {
    ast_cli(a->fd, "  [%s] agent_id=%s\n", agent_configs[i].name,
            agent_configs[i].agent_id);
  }
  ast_mutex_unlock(&config_lock);

  ast_cli(a->fd, "\n");
  return CLI_SUCCESS;
}

static struct ast_cli_entry cli_elevenlabs[] = {

    AST_CLI_DEFINE(handle_elevenlabs_loglevel, "Set/show ElevenLabs log level"),
    AST_CLI_DEFINE(handle_elevenlabs_show, "Show ElevenLabs module settings"),
};

/*! \brief Module load function */
static int load_module(void) {
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

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "ElevenLabs ConvAI Proper module loaded successfully\n");
  return AST_MODULE_LOAD_SUCCESS;
}

/*! \brief Module unload function */
static int unload_module(void) {
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

  /* Cleanup mutex */
  ast_mutex_destroy(&config_lock);

  /* Cleanup CURL */
  curl_global_cleanup();

  ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                 "ElevenLabs ConvAI Proper module unloaded\n");
  return res;
}

/*! \brief Module reload function */
static int reload_module(void) {
  if (load_config() == 0) {
    ELEVENLABS_LOG(ELEVENLABS_LOG_NOTICE, LOG_NOTICE,
                   "ElevenLabs ConvAI Proper configuration reloaded\n");
    return AST_MODULE_LOAD_SUCCESS;
  }

  return AST_MODULE_LOAD_DECLINE;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT,
                "ElevenLabs ConvAI Application",
                .support_level = AST_MODULE_SUPPORT_EXTENDED,
                .load = load_module, .unload = unload_module,
                .reload = reload_module, );