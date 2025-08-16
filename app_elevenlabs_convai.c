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
	ast_mutex_init(&session->session_lock);
	
	/* TODO: Initialize WebSocket connection */
	/* TODO: Initialize audio buffers and queues */
	/* TODO: Attach framehook for audio capture */
	/* TODO: Start playback thread */
	
	ast_verb(2, "ElevenLabsConvAI: Session started successfully for agent '%s'\n", agent_name);
	
	/* Keep the session active until channel hangs up */
	while (session->session_active && ast_channel_state(chan) == AST_STATE_UP) {
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
	
	/* TODO: Stop playback thread */
	/* TODO: Close WebSocket connection */
	/* TODO: Free audio buffers and queues */
	/* TODO: Detach framehook */
	
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