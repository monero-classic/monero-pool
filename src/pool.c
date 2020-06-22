/*
Copyright (c) 2014-2019, The Monero Project

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Parts of the project are originally copyright (c) 2012-2013 The Cryptonote
developers.
*/

#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>

#include <lmdb.h>

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>
#include <uuid/uuid.h>
#include <getopt.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <json-c/json.h>
#include <openssl/bn.h>
#include <pthread.h>

#include "bstack.h"
#include "util.h"
#include "xmr.h"
#include "log.h"
#include "webui.h"
#include "pooldb.h"

#define MAX_LINE 8192
#define POOL_CLIENTS_GROW 1024
#define RPC_BODY_MAX 8192
#define JOB_BODY_MAX 8192
#define ERROR_BODY_MAX 512
#define STATUS_BODY_MAX 256
#define CLIENT_JOBS_MAX 4
#define BLOCK_HEADERS_MAX 4
#define BLOCK_TEMPLATES_MAX 4
#define MAINNET_ADDRESS_PREFIX 18
#define TESTNET_ADDRESS_PREFIX 53
#define BLOCK_HEADERS_RANGE 10
#define MAX_PATH 1024
#define RPC_PATH "/json_rpc"
#define ADDRESS_MAX 128
#define BLOCK_TIME 120
#define HR_BLOCK_COUNT 5
#define TEMLATE_HEIGHT_VARIANCE 5
#define MAX_BAD_SHARES 5

#define uint128_t unsigned __int128

/*
  A block is initially locked.
  After height + 60 blocks, we check to see if its on the chain.
  If not, it becomes orphaned, otherwise we unlock it for payouts.
*/

/*
  Tables:

  Shares
  ------
  height <-> share_t

  Blocks
  ------
  height <-> block_t

  Balance
  -------
  wallet addr <-> balance

  Payments
  --------
  wallet addr <-> payment_t
*/

enum block_status { BLOCK_LOCKED=0, BLOCK_UNLOCKED=1, BLOCK_ORPHANED=2 };
enum stratum_mode { MODE_NORMAL=0, MODE_SELF_SELECT=1 };

typedef struct config_t
{
    char rpc_host[256];
    uint32_t rpc_port;
    uint32_t rpc_timeout;
    char wallet_rpc_host[256];
    uint32_t wallet_rpc_port;
    char pool_wallet[ADDRESS_MAX];
    uint64_t pool_start_diff;
    double share_mul;
    double pool_fee;
    double payment_threshold;
    uint32_t pool_port;
    uint32_t log_level;
    uint32_t webui_port;
    char log_file[MAX_PATH];
    bool block_notified;
    bool disable_self_select;
    char data_dir[MAX_PATH];
} config_t;

typedef struct block_template_t
{
    char *blockhashing_blob;
    char *blocktemplate_blob;
    uint64_t difficulty;
    uint64_t height;
    char prev_hash[64];
    uint32_t reserved_offset;
    char seed_hash[64];
    char next_seed_hash[64];
} block_template_t;

typedef struct job_t
{
    uuid_t id;
    char *blob;
    block_template_t *block_template;
    uint32_t extra_nonce;
    uint64_t target;
    uint128_t *submissions;
    size_t submissions_count;
    block_template_t *miner_template;
} job_t;

typedef struct client_t
{
    int fd;
    int json_id;
    struct bufferevent *bev;
    char address[ADDRESS_MAX];
    char worker_id[64];
    char client_id[32];
    char agent[256];
    job_t active_jobs[CLIENT_JOBS_MAX];
    uint64_t hashes;
    time_t connected_since;
    bool is_proxy;
    uint32_t mode;
    uint8_t bad_shares;
} client_t;

typedef struct pool_clients_t
{
    client_t *clients;
    size_t count;
} pool_clients_t;

typedef struct share_t
{
    uint64_t height;
    uint64_t difficulty;
    char address[ADDRESS_MAX];
    time_t timestamp;
} share_t;

typedef struct block_t
{
    uint64_t height;
    char hash[64];
    char prev_hash[64];
    uint64_t difficulty;
    uint32_t status;
    uint64_t reward;
    time_t timestamp;
} block_t;

typedef struct rpc_callback_t rpc_callback_t;
typedef void (*rpc_callback_fun)(const char*, rpc_callback_t*);
struct rpc_callback_t
{
    rpc_callback_fun f;
    void *data;
};

static config_t config;
static pool_clients_t pool_clients;
static bstack_t *bst;
static bstack_t *bsh;
static struct event_base *base;
static struct event *listener_event;
static struct event *timer_120s;
static struct event *timer_1m;
static struct event *signal_usr1;
static uint32_t extra_nonce;
static uint32_t instance_id;
static block_t block_headers_range[BLOCK_HEADERS_RANGE];
static BN_CTX *bn_ctx;
static BIGNUM *base_diff;
static pool_stats_t pool_stats;
static pthread_mutex_t mutex_clients = PTHREAD_MUTEX_INITIALIZER;
static FILE *fd_log;
static unsigned char sec_view[32];
static unsigned char pub_spend[32];

#ifdef HAVE_RX
extern void rx_stop_mining();
extern void rx_slow_hash_free_state();
#else
void rx_stop_mining(){}
void rx_slow_hash_free_state(){}
#endif

#define JSON_GET_OR_ERROR(name, parent, type, client)                \
    json_object *name = NULL;                                        \
    if (!json_object_object_get_ex(parent, #name, &name)) {          \
        send_validation_error(client, #name " not found");           \
        return;                                                      \
    }                                                                \
    if (!json_object_is_type(name, type)) {                          \
        send_validation_error(client, #name " not a " #type);        \
        return;                                                      \
    }

#define JSON_GET_OR_WARN(name, parent, type)                         \
    json_object *name = NULL;                                        \
    if (!json_object_object_get_ex(parent, #name, &name)) {          \
        log_warn(#name " not found");                                \
    } else {                                                         \
        if (!json_object_is_type(name, type)) {                      \
            log_warn(#name " not a " #type);                         \
        }                                                            \
    }

static inline rpc_callback_t *
rpc_callback_new(rpc_callback_fun f, void *data)
{
    rpc_callback_t *c = calloc(1, sizeof(rpc_callback_t));
    c->f = f;
    c->data = data;
    return c;
}

static inline void
rpc_callback_free(rpc_callback_t *callback)
{
    if (!callback)
        return;
    if (callback->data)
        free(callback->data);
    free(callback);
}

static int
store_share(uint64_t height, share_t *share)
{
	uint64_t timestamp = share->timestamp;
	add_share_to_db(share->height, share->difficulty, share->address, timestamp);
    return 0;
}

static int
store_block(uint64_t height, block_t *block)
{
    uint64_t timestamp = block->timestamp;
	add_block_to_db(block->height, block->hash, block->prev_hash, block->difficulty, block->status, block->reward, timestamp);
    return 0;
}

uint64_t
miner_hr(const char *address)
{
    pthread_mutex_lock(&mutex_clients);
    client_t *c = pool_clients.clients;
    uint64_t hr = 0;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->connected_since != 0
                && strncmp(c->address, address, ADDRESS_MAX) == 0)
        {
            double d = difftime(time(NULL), c->connected_since);
            if (d == 0.0)
                continue;
            hr += c->hashes / d;
            continue;
        }
    }
    pthread_mutex_unlock(&mutex_clients);
    return hr;
}

static void
update_pool_hr(void)
{
    //log_trace("update pool hash rate");
    uint64_t hr = 0;
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->connected_since != 0)
        {
            double d = difftime(time(NULL), c->connected_since);
            if (d == 0.0)
                continue;
            //log_info("client hash %"PRIu64, c->hashes);

            hr += c->hashes / d;
        }
    }
		
    pool_stats.pool_hashrate = hr;
}

static void
template_recycle(void *item)
{
    block_template_t *bt = (block_template_t*) item;
    log_trace("Recycle block template at height: %"PRIu64, bt->height);
/*    if (bt->blockhashing_blob)
    {
        free(bt->blockhashing_blob);
        bt->blockhashing_blob = NULL;
    }*/
    if (bt->blocktemplate_blob)
    {
        free(bt->blocktemplate_blob);
        bt->blocktemplate_blob = NULL;
    }
}

static void
retarget(client_t *client, job_t *job)
{
    double duration = difftime(time(NULL), client->connected_since);
    duration = client->is_proxy ? (duration/2) : duration; 
    uint8_t retarget_time = client->is_proxy ? 5 : 120;
    uint64_t target = fmax((double)client->hashes /
            duration * retarget_time, config.pool_start_diff);
    job->target = target;
    log_debug("client duraion %f ", duration);
    log_debug("client retarget_time %"PRIu64, retarget_time);
    log_debug("client hash %"PRIu64, client->hashes);
    log_debug("Client %.32s target now %"PRIu64, client->client_id, target);
}

static void
target_to_hex(uint64_t target, char *target_hex)
{
    if (target & 0xFFFFFFFF00000000)
    {
        log_debug("High target requested: %"PRIu64, target);
        bin_to_hex((const unsigned char*)&target, 8, &target_hex[0], 16);
        return;
    }
    BIGNUM *diff = BN_new();
    BIGNUM *bnt = BN_new();
#ifdef SIXTY_FOUR_BIT_LONG
    BN_set_word(bnt, target);
#else
    char tmp[24];
    snprintf(tmp, 24, "%"PRIu64, target);
    BN_dec2bn(&bnt, tmp);
#endif
    BN_div(diff, NULL, base_diff, bnt, bn_ctx);
    BN_rshift(diff, diff, 224);
    uint32_t w = BN_get_word(diff);
    bin_to_hex((const unsigned char*)&w, 4, &target_hex[0], 8);
    BN_free(bnt);
    BN_free(diff);
}

static void
stratum_get_proxy_job_body(char *body, const client_t *client,
        const char *block_hex, bool response)
{

    int json_id = client->json_id;
    const char *client_id = client->client_id;
    const job_t *job = &client->active_jobs[0];
		char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), job_id, 32);

    uint64_t target = job->target;
    char target_hex[17] = {0};
    target_to_hex(target, &target_hex[0]);
    const block_template_t *bt = job->block_template;

    if (response)
    {
        snprintf(body, JOB_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
                "\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{\"blocktemplate_blob\":\"%s\","
                "\"job_id\":\"%.32s\","
                "\"difficulty\":%"PRIu64",\"height\":%"PRIu64","
                "\"reserved_offset\":%u,"
                "\"client_nonce_offset\":%u,\"client_pool_offset\":%u,"
                "\"target_diff\":%"PRIu64",\"target_diff_hex\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n",
                json_id, client_id, block_hex, job_id,
                bt->difficulty, bt->height, bt->reserved_offset,
                bt->reserved_offset + 12,
                bt->reserved_offset + 8, target, target_hex,
                bt->seed_hash, bt->next_seed_hash);
    }
    else
    {
		log_info("stratum get proxy job body response false" );
        snprintf(body, JOB_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":"
                "\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"job\":{\"blocktemplate_blob\":\"%s\","
                "\"job_id\":\"%.32s\","
                "\"difficulty\":%"PRIu64",\"height\":%"PRIu64","
                "\"reserved_offset\":%u,"
                "\"client_nonce_offset\":%u,\"client_pool_offset\":%u,"
                "\"target_diff\":%"PRIu64",\"target_diff_hex\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n", client_id, block_hex, job_id,
                bt->difficulty, bt->height,
                bt->reserved_offset, bt->reserved_offset + 12,
                bt->reserved_offset + 8, target, target_hex,
                bt->seed_hash, bt->next_seed_hash);
    }
}

static void
stratum_get_job_body_ss(char *body, const client_t *client, bool response)
{
    /* job_id, target, pool_wallet, extra_nonce */
    int json_id = client->json_id;
    const char *client_id = client->client_id;
    const job_t *job = &client->active_jobs[0];
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), job_id, 32);
    uint64_t target = job->target;
    char target_hex[17] = {0};
    target_to_hex(target, &target_hex[0]);
    char empty[] = "";
    char *seed_hash = empty;
    char *next_seed_hash = empty;
    if (job->miner_template)
    {
        seed_hash = job->miner_template->seed_hash;
        next_seed_hash = job->miner_template->next_seed_hash;
    }
    unsigned char extra_bin[8];
    memcpy(extra_bin, &job->extra_nonce, 4);
    memcpy(extra_bin+4, &instance_id, 4);
    char extra_hex[17] = {0};
    bin_to_hex(extra_bin, 8, extra_hex, 16);

    if (response)
    {
        snprintf(body, JOB_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
                "\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{"
                "\"job_id\":\"%.32s\",\"target\":\"%s\","
                "\"extra_nonce\":\"%s\", \"pool_wallet\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n",
                json_id, client_id, job_id, target_hex, extra_hex,
                config.pool_wallet, seed_hash, next_seed_hash);
    }
    else
    {
        snprintf(body, JOB_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":"
                "\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"job_id\":\"%.32s\","
                "\"target\":\"%s\","
                "\"extra_nonce\":\"%s\", \"pool_wallet\":\"%s\","
                "\"seed_hash\":\"%.64s\",\"next_seed_hash\":\"%.64s\"}}\n",
                client_id, job_id, target_hex, extra_hex, config.pool_wallet,
                seed_hash, next_seed_hash);
    }
}

static void
stratum_get_job_body(char *body, const client_t *client, bool response)
{
    int json_id = client->json_id;
    const char *client_id = client->client_id;
    const job_t *job = &client->active_jobs[0];
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), job_id, 32);
    const char *blob = job->blob;
    uint64_t target = job->target;
    uint64_t height = job->block_template->height;
    char target_hex[17] = {0};
    target_to_hex(target, &target_hex[0]);
    char *seed_hash = job->block_template->seed_hash;
    char *next_seed_hash = job->block_template->next_seed_hash;

    if (response)
    {
        snprintf(body, JOB_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
                "\"error\":null,\"result\""
                ":{\"id\":\"%.32s\",\"job\":{"
                "\"blob\":\"%s\",\"job_id\":\"%.32s\",\"target\":\"%s\","
                "\"height\":%"PRIu64",\"seed_hash\":\"%.64s\","
                "\"next_seed_hash\":\"%.64s\"},"
                "\"status\":\"OK\"}}\n",
                json_id, client_id, blob, job_id, target_hex, height,
                seed_hash, next_seed_hash);
    }
    else
    {
        snprintf(body, JOB_BODY_MAX, "{\"jsonrpc\":\"2.0\",\"method\":"
                "\"job\",\"params\""
                ":{\"id\":\"%.32s\",\"blob\":\"%s\",\"job_id\":\"%.32s\","
                "\"target\":\"%s\","
                "\"height\":%"PRIu64",\"seed_hash\":\"%.64s\","
                "\"next_seed_hash\":\"%.64s\"}}\n",
                client_id, blob, job_id, target_hex, height,
                seed_hash, next_seed_hash);
    }
}

static inline void
stratum_get_error_body(char *body, int json_id, const char *error)
{
    snprintf(body, ERROR_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
            "\"error\":"
            "{\"code\":-1, \"message\":\"%s\"}}\n", json_id, error);
}

static inline void
stratum_get_status_body(char *body, int json_id, const char *status)
{
    snprintf(body, STATUS_BODY_MAX, "{\"id\":%d,\"jsonrpc\":\"2.0\","
            "\"error\":null,\"result\":{\"status\":\"%s\"}}\n",
            json_id, status);
}

static void
send_validation_error(const client_t *client, const char *message)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);
    char body[ERROR_BODY_MAX];
    stratum_get_error_body(body, client->json_id, message);
    evbuffer_add(output, body, strlen(body));
    log_debug("Validation error: %s", message);
}

static void
client_clear_jobs(client_t *client)
{
    for (size_t i=0; i<CLIENT_JOBS_MAX; i++)
    {
        job_t *job = &client->active_jobs[i];
        if (job->blob)
        {
            free(job->blob);
            job->blob = NULL;
        }
        if (job->submissions)
        {
            free(job->submissions);
            job->submissions = NULL;
            job->submissions_count = 0;
        }
        if (job->miner_template)
        {
            block_template_t *bt = job->miner_template;
            if (bt->blocktemplate_blob)
            {
                free(bt->blocktemplate_blob);
                bt->blocktemplate_blob = NULL;
            }
            free(job->miner_template);
            job->miner_template = NULL;
        }
    }
}

static job_t *
client_find_job(client_t *client, const char *job_id)
{
    uuid_t jid;
    hex_to_bin(job_id, strlen(job_id), (unsigned char*)&jid, sizeof(uuid_t));
    for (size_t i=0; i<CLIENT_JOBS_MAX; i++)
    {
        job_t *job = &client->active_jobs[i];
        if (memcmp(job->id, jid, sizeof(uuid_t)) == 0)
            return job;
    }
    return NULL;
}

static void
client_send_job(client_t *client, bool response)
{
    /* First cycle jobs */
    job_t *last = &client->active_jobs[CLIENT_JOBS_MAX-1];
    if (last->blob)
    {
        free(last->blob);
        last->blob = NULL;
    }
    if (last->submissions)
    {
        free(last->submissions);
        last->submissions = NULL;
        last->submissions_count = 0;
    }
    if (last->miner_template)
    {
        block_template_t *bt = last->miner_template;
        if (bt->blocktemplate_blob)
        {
            free(bt->blocktemplate_blob);
            bt->blocktemplate_blob = NULL;
        }
        free(last->miner_template);
        last->miner_template = NULL;
    }
    for (size_t i=CLIENT_JOBS_MAX-1; i>0; i--)
    {
        job_t *current = &client->active_jobs[i];
        job_t *prev = &client->active_jobs[i-1];
        memcpy(current, prev, sizeof(job_t));
    }
    job_t *job = &client->active_jobs[0];
    memset(job, 0, sizeof(job_t));

    if (client->mode == MODE_SELF_SELECT)
    {
        uuid_generate(job->id);
        retarget(client, job);
        ++extra_nonce;
        job->extra_nonce = extra_nonce;
        char body[JOB_BODY_MAX];
        stratum_get_job_body_ss(body, client, response);
        log_trace("Client job: %s", body);
        struct evbuffer *output = bufferevent_get_output(client->bev);
        evbuffer_add(output, body, strlen(body));
        return;
    }

    /* Quick check we actually have a block template */
    block_template_t *bt = bstack_peek(bst);
    if (!bt)
    {
        log_warn("Cannot send client a job: No block template");
        return;
    }

    /*
      1. Convert blocktemplate_blob to binary
      2. Update bytes in reserved space at reserved_offset
      3. Get block hashing blob for job
      4. Send
    */

    /* Convert template to blob */
//    size_t bin_size = strlen(bt->blocktemplate_blob) >> 1;
//    unsigned char *block = calloc(bin_size, sizeof(char));
//    hex_to_bin(bt->blocktemplate_blob, bin_size << 1, block, bin_size);
	size_t hex_size = strlen(bt->blocktemplate_blob);
	size_t bin_size = hex_size >> 1;
    unsigned char *block = calloc(bin_size, sizeof(char));
	hex_to_bin(bt->blocktemplate_blob, hex_size, block, bin_size);


    /* Set the extra nonce in our reserved space */
    unsigned char *p = block;
    p += bt->reserved_offset;
    ++extra_nonce;
    memcpy(p, &extra_nonce, sizeof(extra_nonce));
    job->extra_nonce = extra_nonce;

    /* Add our instance ID */
    p += 4;
    memcpy(p, &instance_id, sizeof(instance_id));

    /* Get hashong blob */
    size_t hashing_blob_size;
    unsigned char *hashing_blob = NULL;
    get_hashing_blob(block, bin_size, &hashing_blob, &hashing_blob_size);

    /* Make hex */
    job->blob = calloc((hashing_blob_size << 1) +1, sizeof(char));
    bin_to_hex(hashing_blob, hashing_blob_size, job->blob,
            hashing_blob_size << 1);
    log_trace("Miner hashing blob: %s", job->blob);

    /* Save a job id */
    uuid_generate(job->id);

    /* Hold reference to block template */
    job->block_template = bt;

    /* Send */
    char job_id[33] = {0};
    bin_to_hex((const unsigned char*)job->id, sizeof(uuid_t), &job_id[0], 32);

    /* Retarget */
    retarget(client, job);
	log_info("retarget");

    char body[JOB_BODY_MAX];
    if (!client->is_proxy)
    {
        stratum_get_job_body(body, client, response);
    }
    else
    {	
				
        stratum_get_job_body(body, client, response);
       // char *block_hex = calloc(hex_size+1, sizeof(char));
       // bin_to_hex(block, bin_size, block_hex, hex_size);
       // stratum_get_proxy_job_body(body, client, block_hex, response);
       // free(block_hex);
    }
    //log_trace("Client job: %s", body);
    struct evbuffer *output = bufferevent_get_output(client->bev);
    evbuffer_add(output, body, strlen(body));
    free(block);
    free(hashing_blob);
}

static void
pool_clients_send_job(void)
{
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->fd == 0)
            continue;
        client_send_job(c, false);
    }
}

static void
pool_clients_init(void)
{
    assert(pool_clients.count == 0);
    pool_clients.count = POOL_CLIENTS_GROW;
    pool_clients.clients = (client_t*) calloc(pool_clients.count,
            sizeof(client_t));
}

static void
pool_clients_free(void)
{
    assert(pool_clients.count != 0);
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        client_clear_jobs(c);
    }
    free(pool_clients.clients);
}

static void
response_to_block_template(json_object *result,
        block_template_t *block_template)
{
    //JSON_GET_OR_WARN(blockhashing_blob, result, json_type_string);
    JSON_GET_OR_WARN(blocktemplate_blob, result, json_type_string);
    JSON_GET_OR_WARN(difficulty, result, json_type_int);
    JSON_GET_OR_WARN(height, result, json_type_int);
    //JSON_GET_OR_WARN(prev_hash, result, json_type_string);
    JSON_GET_OR_WARN(reserved_offset, result, json_type_int);
    //block_template->blockhashing_blob = strdup(
    //        json_object_get_string(blockhashing_blob));
    block_template->blocktemplate_blob = strdup(
            json_object_get_string(blocktemplate_blob));
    block_template->difficulty = json_object_get_int64(difficulty);
    block_template->height = json_object_get_int64(height);
    //memcpy(block_template->prev_hash, json_object_get_string(prev_hash), 64);
    block_template->reserved_offset = json_object_get_int(reserved_offset);

    unsigned int major_version = 0;
    sscanf(block_template->blocktemplate_blob, "%2x", &major_version);
    uint8_t pow_variant = 0;// major_version >= 7 ? major_version - 6 : 0;
    log_trace("Variant: %u", pow_variant);

    if (pow_variant >= 6)
    {
        JSON_GET_OR_WARN(seed_hash, result, json_type_string);
        JSON_GET_OR_WARN(next_seed_hash, result, json_type_string);
        assert(seed_hash != NULL);
        assert(next_seed_hash != NULL);
        memcpy(block_template->seed_hash,
                json_object_get_string(seed_hash), 64);
        memcpy(block_template->next_seed_hash,
                json_object_get_string(next_seed_hash), 64);
    }
}

static void
response_to_block(json_object *block_header, block_t *block)
{
    memset(block, 0, sizeof(block_t));
    JSON_GET_OR_WARN(height, block_header, json_type_int);
    JSON_GET_OR_WARN(difficulty, block_header, json_type_int);
    JSON_GET_OR_WARN(hash, block_header, json_type_string);
    JSON_GET_OR_WARN(prev_hash, block_header, json_type_string);
    JSON_GET_OR_WARN(timestamp, block_header, json_type_int);
    JSON_GET_OR_WARN(reward, block_header, json_type_int);
    JSON_GET_OR_WARN(orphan_status, block_header, json_type_boolean);
    block->height = json_object_get_int64(height);
    block->difficulty = json_object_get_int64(difficulty);
    memcpy(block->hash, json_object_get_string(hash), 64);
    memcpy(block->prev_hash, json_object_get_string(prev_hash), 64);
    block->timestamp = json_object_get_int64(timestamp);
    block->reward = json_object_get_int64(reward);
    if (json_object_get_int(orphan_status))
        block->status |= BLOCK_ORPHANED;
}

static void
rpc_on_response(struct evhttp_request *req, void *arg)
{
    struct evbuffer *input;
    rpc_callback_t *callback = (rpc_callback_t*) arg;

    if (!req)
    {
        log_error("Request failure. Aborting.");
        rpc_callback_free(callback);
        return;
    }

    int rc = evhttp_request_get_response_code(req);
    if (rc < 200 || rc >= 300)
    {
        log_error("HTTP status code %d for %s. Aborting.",
                rc, evhttp_request_get_uri(req));
        rpc_callback_free(callback);
        return;
    }

    input = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(input);
    char body[len+1];
    evbuffer_remove(input, body, len);
    body[len] = '\0';
    callback->f(body, callback);
    rpc_callback_free(callback);
}

static void
rpc_request(struct event_base *base, const char *body, rpc_callback_t *callback)
{
    struct evhttp_connection *con;
    struct evhttp_request *req;
    struct evkeyvalq *headers;
    struct evbuffer *output;

    con = evhttp_connection_base_new(base, NULL,
            config.rpc_host, config.rpc_port);
    evhttp_connection_free_on_completion(con);
    evhttp_connection_set_timeout(con, config.rpc_timeout);
    req = evhttp_request_new(rpc_on_response, callback);
    output = evhttp_request_get_output_buffer(req);
    evbuffer_add(output, body, strlen(body));
    headers = evhttp_request_get_output_headers(req);
    evhttp_add_header(headers, "Content-Type", "application/json");
    evhttp_add_header(headers, "Connection", "close");
    evhttp_make_request(con, req, EVHTTP_REQ_POST, RPC_PATH);
}

static void
rpc_get_request_body(char *body, const char *method, char *fmt, ...)
{
    char *pb = body;
    char *end = body + RPC_BODY_MAX;

    pb = stecpy(pb, "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":\"", end);
    pb = stecpy(pb, method, end);
    pb = stecpy(pb, "\"", end);

    if (fmt && *fmt)
    {
        char *s;
        uint64_t d;
        pb = stecpy(pb, ",\"params\":{", end);
        va_list args;
        va_start(args, fmt);
        uint8_t count = 0;
        while (*fmt)
        {
            switch (*fmt++)
            {
                case 's':
                    s = va_arg(args, char *);
                    pb = stecpy(pb, "\"", end);
                    pb = stecpy(pb, s, end);
                    pb = stecpy(pb, "\"", end);
                    break;
                case 'd':
                    d = va_arg(args, uint64_t);
                    char tmp[24];
                    snprintf(tmp, 24, "%"PRIu64, d);
                    pb = stecpy(pb, tmp, end);
                    break;
            }
            *pb++ = count++ % 2 ? ',' : ':';
        }
        va_end(args);
        *--pb = '}';
        pb++;
    }
    *pb++ = '}';
    *pb = '\0';
    log_trace("Payload: %s", body);
}

static void
rpc_on_block_header_by_height(const char* data, rpc_callback_t *callback)
{
    log_trace("Got block header by height: \n%s", data);
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting block header by height: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_error("Error getting block header by height: %s", ss);
        json_object_put(root);
        return;
    }
    block_t rb;
    JSON_GET_OR_WARN(block_header, result, json_type_object);
    response_to_block(block_header, &rb);
    json_object_put(root);
}

static void
rpc_on_block_template(const char* data, rpc_callback_t *callback)
{
    log_trace("Got block template: \n%s", data);
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting block template: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_error("Error getting block template: %s", ss);
        json_object_put(root);
        return;
    }
    pool_stats.last_template_fetched = time(NULL);
    block_template_t *front = (block_template_t*) bstack_push(bst, NULL);
    response_to_block_template(result, front);
    pool_clients_send_job();
    json_object_put(root);
}

static void
rpc_on_last_block_header(const char* data, rpc_callback_t *callback)
{
    log_trace("Got last block header: \n%s", data);
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_error("Error (%d) getting last block header: %s", ec, em);
        json_object_put(root);
        return;
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_error("Error getting last block header: %s", ss);
        json_object_put(root);
        return;
    }

    JSON_GET_OR_WARN(block_header, result, json_type_object);
    JSON_GET_OR_WARN(height, block_header, json_type_int);
    uint64_t bh = json_object_get_int64(height);
    bool need_new_template = false;
    block_t *front = bstack_peek(bsh);
    if (front && bh > front->height)
    {
        need_new_template = true;
        block_t *block = bstack_push(bsh, NULL);
        response_to_block(block_header, block);
    }
    else if (!front)
    {
        block_t *block = bstack_push(bsh, NULL);
        response_to_block(block_header, block);
        need_new_template = true;
    }

    front = bstack_peek(bsh);
    pool_stats.network_difficulty = front->difficulty;
    pool_stats.network_hashrate = front->difficulty / BLOCK_TIME;
    pool_stats.network_height = front->height;
    update_pool_hr();

    if (need_new_template)
    {
        log_info("Fetching new block template");
        char body[RPC_BODY_MAX];
        uint64_t reserve = 17;
        rpc_get_request_body(body, "getblocktemplate", "sssd",
                "wallet_address", config.pool_wallet, "reserve_size", reserve);
        rpc_callback_t *cb1 = rpc_callback_new(rpc_on_block_template, NULL);
        rpc_request(base, body, cb1);

    }

    json_object_put(root);
}

static void
rpc_on_block_submitted(const char* data, rpc_callback_t *callback)
{
    json_object *root = json_tokener_parse(data);
    JSON_GET_OR_WARN(result, root, json_type_object);
    JSON_GET_OR_WARN(status, result, json_type_string);
    const char *ss = json_object_get_string(status);
    json_object *error = NULL;
    json_object_object_get_ex(root, "error", &error);
    /*
      The RPC reports submission as an error even when it's added as
      an alternative block. Thus, still store it. This doesn't matter
      as upon payout, blocks are checked whether they are orphaned or not.
    */
    if (error)
    {
        JSON_GET_OR_WARN(code, error, json_type_object);
        JSON_GET_OR_WARN(message, error, json_type_string);
        int ec = json_object_get_int(code);
        const char *em = json_object_get_string(message);
        log_warn("Error (%d) with block submission: %s", ec, em);
    }
    if (!status || strcmp(ss, "OK") != 0)
    {
        log_warn("Error submitting block: %s", ss);
    }
    pool_stats.pool_blocks_found++;
    block_t *b = (block_t*)callback->data;
    pool_stats.last_block_found = b->timestamp;
    log_info("Block submitted at height: %"PRIu64, b->height);
    int rc = store_block(b->height, b);
    if (rc != 0)
        log_warn("Failed to store block: %s", mdb_strerror(rc));
    json_object_put(root);
}

static void
fetch_last_block_header(void)
{
    log_info("Fetching last block header");
    char body[RPC_BODY_MAX];
    rpc_get_request_body(body, "getlastblockheader", NULL);
    rpc_callback_t *cb = rpc_callback_new(rpc_on_last_block_header, NULL);
    rpc_request(base, body, cb);
}

static void
timer_on_120s(int fd, short kind, void *ctx)
{
    log_trace("Fetching last block header from timer");
    fetch_last_block_header();
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    evtimer_add(timer_120s, &timeout);
}

static void
timer_on_1m(int fd, short kind, void *ctx)
{
    log_info("flush data to db");
	batch_sql();	
    struct timeval timeout = { .tv_sec = 60, .tv_usec = 0 };
    evtimer_add(timer_1m, &timeout);
}

static void
client_add(int fd, struct bufferevent *bev)
{
    log_info("New client connected");
    client_t *c = pool_clients.clients;
    bool resize = true;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->connected_since == 0)
        {
            resize = false;
            break;
        }
    }
    if (resize)
    {
        pthread_mutex_lock(&mutex_clients);
        pool_clients.count += POOL_CLIENTS_GROW;
        c = realloc(pool_clients.clients, sizeof(client_t) *
                pool_clients.count);
        pool_clients.clients = c;
        c += pool_clients.count - POOL_CLIENTS_GROW;
        pthread_mutex_unlock(&mutex_clients);
        log_debug("Client pool can now hold %zu clients", pool_clients.count);
    }
    memset(c, 0, sizeof(client_t));
    c->fd = fd;
    c->bev = bev;
    c->connected_since = time(NULL);
    pool_stats.connected_miners++;
}

static void
client_find(struct bufferevent *bev, client_t **client)
{
    int fd = bufferevent_getfd(bev);
    client_t *c = pool_clients.clients;
    for (size_t i = 0; i < pool_clients.count; i++, c++)
    {
        if (c->fd == fd)
        {
            *client = c;
            return;
        }
    }
    *client = NULL;
}

static void
client_clear(struct bufferevent *bev)
{
    client_t *client = NULL;
    client_find(bev, &client);
    if (!client)
        return;
    client_clear_jobs(client);
    memset(client, 0, sizeof(client_t));
    bufferevent_free(bev);
    pool_stats.connected_miners--;
}

static void
client_on_login(json_object *message, client_t *client)
{
    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(login, params, json_type_string, client);
    JSON_GET_OR_ERROR(pass, params, json_type_string, client);
    client->mode = MODE_NORMAL;
    json_object *mode = NULL;
    if (json_object_object_get_ex(params, "mode", &mode))
    {
        if (!json_object_is_type(mode, json_type_string))
            log_warn("mode not a json_type_string");
        else
        {
            const char *modestr = json_object_get_string(mode);
            if (strcmp(modestr, "self-select") == 0)
            {
                if (config.disable_self_select)
                {
                    send_validation_error(client,
                            "pool disabled self-select");
                    return;
                }
                client->mode = MODE_SELF_SELECT;
                log_trace("Client login for mode: self-select");
            }
        }
    }

    const char *address = json_object_get_string(login);
    uint64_t prefix;
    parse_address(address, &prefix, NULL);
    log_trace("Address: %s", address);
    const char *worker_id = json_object_get_string(pass);

    json_object *agent = NULL;
    if (json_object_object_get_ex(params, "agent", &agent))
    {
        const char *user_agent = json_object_get_string(agent);
        if (user_agent)
        {
            strncpy(client->agent, user_agent, 255);
            client->is_proxy = strstr(user_agent, "proxy") != NULL
                ? true : false;
        }
    }

    if (!client->is_proxy) 
    {
        if (prefix != MAINNET_ADDRESS_PREFIX && prefix != TESTNET_ADDRESS_PREFIX)
        {
	   log_trace("Variant: %u", prefix);

            send_validation_error(client,
                    "login only main wallet addresses are supported");
            return;
        }

    }
  
    if (client->is_proxy && client->mode == MODE_SELF_SELECT)
    {
        send_validation_error(client,
                "login mode self-select and proxy not yet supported");
        return;
    }

    strncpy(client->address, address, sizeof(client->address));
    strncpy(client->worker_id, worker_id, sizeof(client->worker_id));
    uuid_t cid;
    uuid_generate(cid);
    bin_to_hex((const unsigned char*)cid, sizeof(uuid_t), client->client_id, 32);
    client_send_job(client, true);
}

static void
client_on_block_template(json_object *message, client_t *client)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);

    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(id, params, json_type_string, client);
    JSON_GET_OR_ERROR(job_id, params, json_type_string, client);
    JSON_GET_OR_ERROR(blob, params, json_type_string, client);
    JSON_GET_OR_ERROR(difficulty, params, json_type_int, client);
    JSON_GET_OR_ERROR(height, params, json_type_int, client);
    JSON_GET_OR_ERROR(prev_hash, params, json_type_string, client);

    const char *jid = json_object_get_string(job_id);
    if (strlen(jid) != 32)
    {
        send_validation_error(client, "job_id invalid length");
        return;
    }

    int64_t h = json_object_get_int64(height);
    int64_t dh = llabs(h - (int64_t)pool_stats.network_height);
    if (dh > TEMLATE_HEIGHT_VARIANCE)
    {
        char m[64];
        snprintf(m, 64, "Bad height. Differs to pool by %"PRIu64" blocks.", dh);
        send_validation_error(client, m);
        return;
    }

    const char *btb = json_object_get_string(blob);
    int rc = validate_block_from_blob(btb, &sec_view[0], &pub_spend[0]);
    if (rc != 0)
    {
        log_warn("Bad template submitted: %d", rc);
        send_validation_error(client, "block template blob invalid");
        return;
    }

    job_t *job = client_find_job(client, jid);
    if (!job)
    {
        send_validation_error(client, "cannot find job with job_id");
        return;
    }

    if (job->miner_template)
    {
        send_validation_error(client, "job already has block template");
        return;
    }

    job->miner_template = calloc(1, sizeof(block_template_t));
    job->miner_template->blocktemplate_blob = strdup(btb);
    job->miner_template->difficulty = json_object_get_int64(difficulty);
    job->miner_template->height = json_object_get_int64(height);
    memcpy(job->miner_template->prev_hash,
            json_object_get_string(prev_hash), 64);

    unsigned int major_version = 0;
    sscanf(btb, "%2x", &major_version);
    uint8_t pow_variant = 0;//major_version >= 7 ? major_version - 6 : 0;
    log_trace("Variant: %u", pow_variant);

    if (pow_variant >= 6)
    {
        JSON_GET_OR_WARN(seed_hash, params, json_type_string);
        JSON_GET_OR_WARN(next_seed_hash, params, json_type_string);
        assert(seed_hash != NULL);
        assert(next_seed_hash != NULL);
        memcpy(job->miner_template->seed_hash,
                json_object_get_string(seed_hash), 64);
        memcpy(job->miner_template->next_seed_hash,
                json_object_get_string(next_seed_hash), 64);
    }

    log_trace("Client set template: %s", btb);
    char body[STATUS_BODY_MAX];
    stratum_get_status_body(body, client->json_id, "OK");
    evbuffer_add(output, body, strlen(body));
}

static void
client_on_submit(json_object *message, client_t *client)
{
    struct evbuffer *output = bufferevent_get_output(client->bev);

    JSON_GET_OR_ERROR(params, message, json_type_object, client);
    JSON_GET_OR_ERROR(nonce, params, json_type_string, client);
    JSON_GET_OR_ERROR(result, params, json_type_string, client);
    JSON_GET_OR_ERROR(job_id, params, json_type_string, client);

    char *endptr = NULL;
    const char *nptr = json_object_get_string(nonce);
    errno = 0;
    unsigned long int uli = strtoul(nptr, &endptr, 16);
    if (errno != 0 || nptr == endptr)
    {
        send_validation_error(client, "nonce not an unsigned long int");
        return;
    }
    const uint32_t result_nonce = ntohl(uli);

    const char *result_hex = json_object_get_string(result);
    if (strlen(result_hex) != 64)
    {
        send_validation_error(client, "result invalid length");
        return;
    }
    if (is_hex_string(result_hex) != 0)
    {
        send_validation_error(client, "result not hex string");
        return;
    }

    const char *jid = json_object_get_string(job_id);
    if (strlen(jid) != 32)
    {
        send_validation_error(client, "job_id invalid length");
        return;
    }

    job_t *job = client_find_job(client, jid);
    if (!job)
    {
        send_validation_error(client, "cannot find job with job_id");
        return;
    }

    log_trace("Client submitted nonce=%u, result=%s",
            result_nonce, result_hex);
    /*
      1. Validate submission
         active_job->blocktemplate_blob to bin
         add extra_nonce at reserved offset
         add nonce
         get hashing blob
         hash
         compare result
         check result hash against block difficulty (if ge then mined block)
         check result hash against target difficulty (if not ge, invalid share)
      2. Process share
         check result hash against template difficulty
         (submit to network if good) add share to db

      Note reserved space is: extra_nonce, instance_id, pool_nonce, worker_nonce
       4 bytes each.
    */

    /* Convert template to blob */
    if (client->mode == MODE_SELF_SELECT && !job->miner_template)
    {
        send_validation_error(client, "mode self-selct and no template");
        return;
    }
    block_template_t *bt;
    if (job->miner_template)
        bt = job->miner_template;
    else
        bt = job->block_template;
    char *btb = bt->blocktemplate_blob;
    size_t bin_size = strlen(btb) >> 1;
    unsigned char *block = calloc(bin_size, sizeof(char));
    hex_to_bin(bt->blocktemplate_blob, bin_size << 1, block, bin_size);

    unsigned char *p = block;
    uint32_t pool_nonce = 0;
    uint32_t worker_nonce = 0;

    if (client->mode != MODE_SELF_SELECT)
    {
        /* Set the extra nonce and instance_id in our reserved space */
        p += bt->reserved_offset;
        memcpy(p, &job->extra_nonce, sizeof(extra_nonce));
        p += 4;
        memcpy(p, &instance_id, sizeof(instance_id));
        if (client->is_proxy)
        {
            /*
              A proxy supplies pool_nonce and worker_nonce
              so add them in the resrved space too.
            */
          /*  JSON_GET_OR_WARN(poolNonce, params, json_type_int);
            JSON_GET_OR_WARN(workerNonce, params, json_type_int);
            pool_nonce = json_object_get_int(poolNonce);
            worker_nonce = json_object_get_int(workerNonce);
            p += 4;
            memcpy(p, &pool_nonce, sizeof(pool_nonce));
            p += 4;
            memcpy(p, &worker_nonce, sizeof(worker_nonce));*/
        }
    }

    uint128_t sub = 0;
    uint32_t *psub = (uint32_t*) &sub;
    *psub++ = result_nonce;
    *psub++ = job->extra_nonce;
    *psub++ = pool_nonce;
    *psub++ = worker_nonce;

    psub -= 4;
    /*log_trace("Submission reserved values: %u %u %u %u",
            *psub, *(psub+1), *(psub+2), *(psub+3));
    */

    /* Check not already submitted */
    uint128_t *submissions = job->submissions;
    for (size_t i=0; i<job->submissions_count; i++)
    {
        if (submissions[i] == sub)
        {
            char body[ERROR_BODY_MAX];
            stratum_get_error_body(body, client->json_id, "Duplicate share");
            evbuffer_add(output, body, strlen(body));
            log_debug("Duplicate share");
            free(block);
            return;
        }
    }
    job->submissions = realloc((void*)submissions,
            sizeof(uint128_t) * ++job->submissions_count);
    job->submissions[job->submissions_count-1] = sub;

    /* And the supplied nonce */
    p = block;
    p += 39;
    memcpy(p, &result_nonce, sizeof(result_nonce));

    /* Get hashong blob */
    size_t hashing_blob_size;
    unsigned char *hashing_blob = NULL;
    if (get_hashing_blob(block, bin_size,
                &hashing_blob, &hashing_blob_size) != 0)
    {
        char body[ERROR_BODY_MAX];
        stratum_get_error_body(body, client->json_id, "Invalid block");
        evbuffer_add(output, body, strlen(body));
        log_debug("Invalid block");
        free(block);
        return;
    }

    /* Hash and compare */
    unsigned char result_hash[32] = {0};
    unsigned char submitted_hash[32] = {0};
    uint8_t major_version = (uint8_t)block[0];
    uint8_t pow_variant = 0; //major_version >= 7 ? major_version - 6 : 0;
    if (pow_variant >= 6)
    {
        unsigned char seed_hash[32];
        hex_to_bin(bt->seed_hash, 64, seed_hash, 32);
        get_rx_hash(hashing_blob, hashing_blob_size,
                (unsigned char*)result_hash, seed_hash, bt->height);
    }
    else
    {
        get_hash(hashing_blob, hashing_blob_size,
                (unsigned char*)result_hash, pow_variant, bt->height);
    }
    hex_to_bin(result_hex, 64, submitted_hash, 32);

    if (memcmp(submitted_hash, result_hash, 32) != 0)
    {
        char body[ERROR_BODY_MAX];
        stratum_get_error_body(body, client->json_id, "Invalid share");
        evbuffer_add(output, body, strlen(body));
        log_debug("Invalid share");
        client->bad_shares++;
        free(block);
        free(hashing_blob);
        return;
    }

    BIGNUM *hd = BN_new();
    BIGNUM *jd = BN_new();
    BIGNUM *bd = BN_new();
    BIGNUM *rh = NULL;
    BN_set_word(jd, job->target);
    BN_set_word(bd, bt->difficulty);
    reverse_bin(result_hash, 32);
    rh = BN_bin2bn((const unsigned char*)result_hash, 32, NULL);
    BN_div(hd, NULL, base_diff, rh, bn_ctx);
    BN_free(rh);

    /* Process share */
    client->hashes += job->target;
    time_t now = time(NULL);
    bool can_store = true;
    /*log_trace("Checking hash against blobk difficulty: "
            "%lu, job difficulty: %lu",
            BN_get_word(bd), BN_get_word(jd));
    */

    if (BN_cmp(hd, bd) >= 0)
    {
        /* Yay! Mined a block so submit to network */
        log_info("+++ MINED A BLOCK +++");
        char *block_hex = calloc((bin_size << 1)+1, sizeof(char));
        bin_to_hex(block, bin_size, block_hex, bin_size << 1);
        char body[RPC_BODY_MAX];
        snprintf(body, RPC_BODY_MAX,
                "{\"jsonrpc\":\"2.0\",\"id\":\"0\",\"method\":"
                "\"submitblock\", \"params\":[\"%s\"]}",
                block_hex);

        rpc_callback_t *cb = rpc_callback_new(rpc_on_block_submitted, NULL);
        cb->data = calloc(1, sizeof(block_t));
        block_t* b = (block_t*) cb->data;
        b->height = bt->height;
        unsigned char block_hash[32] = {0};
        if (get_block_hash(block, bin_size, block_hash) != 0)
            log_error("Error getting block hash!");
        bin_to_hex(block_hash, 32, b->hash, 64);
        memcpy(b->prev_hash, bt->prev_hash, 64);
        b->difficulty = bt->difficulty;
        b->status = BLOCK_LOCKED;
        b->timestamp = now;

        rpc_request(base, body, cb);
        free(block_hex);
    }
    else if (BN_cmp(hd, jd) < 0)
    {
        can_store = false;
        char body[ERROR_BODY_MAX];
        stratum_get_error_body(body, client->json_id, "Low difficulty share");
        evbuffer_add(output, body, strlen(body));
        log_debug("Low difficulty (%lu) share", BN_get_word(jd));
        client->bad_shares++;
    }

    BN_free(hd);
    BN_free(jd);
    BN_free(bd);
    free(block);
    free(hashing_blob);

    if (can_store)
    {
        if (client->bad_shares)
            client->bad_shares--;
        share_t share;
        share.height = bt->height;
        share.difficulty = job->target;
        strncpy(share.address, client->address, sizeof(share.address));
        share.timestamp = now;
        log_debug("Storing share with difficulty: %"PRIu64, share.difficulty);
        int rc = store_share(share.height, &share);
        if (rc != 0)
            log_warn("Failed to store share: %s", mdb_strerror(rc));
        char body[STATUS_BODY_MAX];
        stratum_get_status_body(body, client->json_id, "OK");
        evbuffer_add(output, body, strlen(body));
    }
}

static void
client_on_read(struct bufferevent *bev, void *ctx)
{
    const char *unknown_method = "Removing client. Unknown method called.";
    const char *too_bad = "Removing client. Too many bad shares.";
    const char *too_long = "Removing client. Message too long.";
    struct evbuffer *input, *output;
    char *line;
    size_t n;
    client_t *client = NULL;

    client_find(bev, &client);
    if (!client)
        return;

    input = bufferevent_get_input(bev);
    output = bufferevent_get_output(bev);

    size_t len = evbuffer_get_length(input);
    if (len > MAX_LINE)
    {
        char body[ERROR_BODY_MAX];
        stratum_get_error_body(body, client->json_id, too_long);
        evbuffer_add(output, body, strlen(body));
        log_info(too_long);
        evbuffer_drain(input, len);
        client_clear(bev);
        return;
    }

    if (client->bad_shares > MAX_BAD_SHARES)
    {
        char body[ERROR_BODY_MAX];
        stratum_get_error_body(body, client->json_id, too_bad);
        evbuffer_add(output, body, strlen(body));
        log_info(too_bad);
        evbuffer_drain(input, len);
        client_clear(bev);
        return;
    }

    while ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_LF)))
    {
        json_object *message = json_tokener_parse(line);
        JSON_GET_OR_WARN(method, message, json_type_string);
        JSON_GET_OR_WARN(id, message, json_type_int);
        const char *method_name = json_object_get_string(method);
        client->json_id = json_object_get_int(id);

        bool unknown = false;

        if (!method || !method_name)
        {
            unknown = true;
        }
        else if (strcmp(method_name, "login") == 0)
        {
            client_on_login(message, client);
        }
        else if (strcmp(method_name, "block_template") == 0)
        {
            client_on_block_template(message, client);
        }
        else if (strcmp(method_name, "submit") == 0)
        {
            client_on_submit(message, client);
        }
        else if (strcmp(method_name, "getjob") == 0)
        {
            client_send_job(client, false);
        }
        else if (strcmp(method_name, "keepalived") == 0)
        {
            char body[STATUS_BODY_MAX];
            stratum_get_status_body(body, client->json_id, "KEEPALIVED");
            evbuffer_add(output, body, strlen(body));
        }
        else
        {
            unknown = true;
        }

        json_object_put(message);
        free(line);

        if (unknown)
        {
            char body[ERROR_BODY_MAX];
            stratum_get_error_body(body, client->json_id, unknown_method);
            evbuffer_add(output, body, strlen(body));
            log_info(unknown_method);
            evbuffer_drain(input, len);
            client_clear(bev);
            return;
        }
    }
}

static void
client_on_error(struct bufferevent *bev, short error, void *ctx)
{
    if (error & BEV_EVENT_EOF)
    {
        /* connection has been closed */
        log_debug("Client disconnected. Removing.");
    }
    else if (error & BEV_EVENT_ERROR)
    {
        /* check errno to see what error occurred */
        log_debug("Client error: %d. Removing.", errno);
    }
    else if (error & BEV_EVENT_TIMEOUT)
    {
        /* must be a timeout event handle, handle it */
        log_debug("Client timeout. Removing.");
    }
    client_clear(bev);
}

static void
client_on_accept(evutil_socket_t listener, short event, void *arg)
{
    struct event_base *base = (struct event_base*)arg;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listener, (struct sockaddr*)&ss, &slen);
    if (fd < 0)
    {
        perror("accept");
    }
    else if (fd > FD_SETSIZE)
    {
        close(fd);
    }
    else
    {
        struct bufferevent *bev;
        evutil_make_socket_nonblocking(fd);
        bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        bufferevent_setcb(bev, client_on_read, NULL, client_on_error, NULL);
        bufferevent_setwatermark(bev, EV_READ, 0, MAX_LINE);
        bufferevent_enable(bev, EV_READ|EV_WRITE);
        client_add(fd, bev);
    }
}

static void
read_config(const char *config_file, const char *log_file, bool block_notified,
        const char *data_dir)
{
    /* Start with some defaults for any missing... */
    strncpy(config.rpc_host, "127.0.0.1", 10);
    config.rpc_port = 18081;
    config.rpc_timeout = 15;
    config.pool_start_diff = 100;
    config.share_mul = 2.0;
    config.pool_fee = 0.01;
    config.payment_threshold = 0.33;
    config.pool_port = 4242;
    config.log_level = 5;
    config.webui_port = 4243;
    config.block_notified = block_notified;
    config.disable_self_select = false;
    strncpy(config.data_dir, "./data", 7);

    char path[MAX_PATH] = {0};
    if (config_file)
    {
        strncpy(path, config_file, MAX_PATH);
    }
    else
    {
        if (!getcwd(path, MAX_PATH))
        {
            log_fatal("Cannot getcwd (%s). Aborting.", errno);
            abort();
        }
        strcat(path, "/pool.conf");
        if (access(path, R_OK) != 0)
        {
            strncpy(path, getenv("HOME"), MAX_PATH);
            strcat(path, "/pool.conf");
            if (access(path, R_OK) != 0)
            {
                log_fatal("Cannot find a config file in ./ or ~/ "
                        "and no option supplied. Aborting.");
                abort();
            }
        }
    }
    log_info("Reading config at: %s", path);

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        log_fatal("Cannot open config file. Aborting.");
        abort();
    }
    char line[256];
    char *key;
    char *val;
    const char *tok = " =";
    while (fgets(line, sizeof(line), fp))
    {
        key = strtok(line, tok);
        if (!key)
            continue;
        val = strtok(NULL, tok);
        if (!val)
            continue;
        val[strcspn(val, "\r\n")] = 0;
        if (strcmp(key, "rpc-host") == 0)
        {
            strncpy(config.rpc_host, val, sizeof(config.rpc_host));
        }
        else if (strcmp(key, "rpc-port") == 0)
        {
            config.rpc_port = atoi(val);
        }
        else if (strcmp(key, "rpc-timeout") == 0)
        {
            config.rpc_timeout = atoi(val);
        }
        else if (strcmp(key, "pool-wallet") == 0)
        {
            strncpy(config.pool_wallet, val, sizeof(config.pool_wallet));
        }
        else if (strcmp(key, "pool-start-diff") == 0)
        {
            config.pool_start_diff = strtoumax(val, NULL, 10);
        }
        else if (strcmp(key, "share-mul") == 0)
        {
            config.share_mul = atof(val);
        }
        else if (strcmp(key, "pool-fee") == 0)
        {
            config.pool_fee = atof(val);
        }
        else if (strcmp(key, "payment-threshold") == 0)
        {
            config.payment_threshold = atof(val);
        }
        else if (strcmp(key, "pool-port") == 0)
        {
            config.pool_port = atoi(val);
        }
        else if (strcmp(key, "log-level") == 0)
        {
            config.log_level = atoi(val);
        }
        else if (strcmp(key, "webui-port") == 0)
        {
            config.webui_port = atoi(val);
        }
        else if (strcmp(key, "log-file") == 0)
        {
            strncpy(config.log_file, val, sizeof(config.log_file));
        }
        else if (strcmp(key, "block-notified") == 0)
        {
            config.block_notified = atoi(val);
        }
        else if (strcmp(key, "disable-self-select") == 0)
        {
            config.disable_self_select = atoi(val);
        }
        else if (strcmp(key, "data-dir") == 0)
        {
            strncpy(config.data_dir, val,  sizeof(config.data_dir));
        }
    }
    fclose(fp);

    if (log_file)
        strncpy(config.log_file, log_file, sizeof(config.log_file));
    if (data_dir)
        strncpy(config.data_dir, data_dir, sizeof(config.data_dir));

    if (!config.pool_wallet[0])
    {
        log_fatal("No pool wallet supplied. Aborting.");
        abort();
    }

    log_info("\nCONFIG:\n  rpc_host = %s\n  rpc_port = %u\n  "
            "rpc_timeout = %u\n  pool_wallet = %s\n  "
            "pool_start_diff = %"PRIu64"\n  share_mul = %.2f\n  "
            "pool_fee = %.3f\n  payment_threshold = %.2f\n  "
            "pool_port = %u\n  "
            "log_level = %u\n  webui_port=%u\n  "
            "log-file = %s\n  block-notified = %u\n  "
            "disable-self-select = %u\n  "
            "data-dir = %s\n",
            config.rpc_host, config.rpc_port, config.rpc_timeout,
            config.pool_wallet, config.pool_start_diff, config.share_mul,
            config.pool_fee, config.payment_threshold,
            config.pool_port,
            config.log_level, config.webui_port,
            config.log_file, config.block_notified, config.disable_self_select,
            config.data_dir);
}

static void
sigusr1_handler(evutil_socket_t fd, short event, void *arg)
{
    log_trace("Fetching last block header from signal");
    fetch_last_block_header();
}

static void
sigint_handler(int sig)
{
    signal(SIGINT, SIG_DFL);
    exit(0);
}

static void
run(void)
{
    evutil_socket_t listener;
    struct sockaddr_in sin;

    base = event_base_new();
    if (!base)
    {
        log_fatal("Failed to create event base");
        return;
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(config.pool_port);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    evutil_make_socket_nonblocking(listener);

#ifndef WIN32
    {
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#endif

    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0)
    {
        perror("bind");
        return;
    }

    if (listen(listener, 16)<0)
    {
        perror("listen");
        return;
    }

    listener_event = event_new(base, listener, EV_READ|EV_PERSIST,
            client_on_accept, (void*)base);
    if (event_add(listener_event, NULL) != 0)
    {
        log_fatal("Failed to add socket listener event");
        return;
    }

    signal_usr1 = evsignal_new(base, SIGUSR1, sigusr1_handler, NULL);
    event_add(signal_usr1, NULL);
    if (!config.block_notified)
    {
        timer_120s = evtimer_new(base, timer_on_120s, NULL);
        timer_on_120s(-1, EV_TIMEOUT, NULL);
    }
    else
        fetch_last_block_header();

    timer_1m = evtimer_new(base, timer_on_1m, NULL);
    timer_on_1m(-1, EV_TIMEOUT, NULL);

    event_base_dispatch(base);
}

static void
cleanup(void)
{
    log_info("\nPerforming cleanup");
    if (listener_event)
        event_free(listener_event);
    stop_web_ui();
    if (signal_usr1)
        event_free(signal_usr1);
    if (timer_120s)
        event_free(timer_120s);
    if (timer_1m)
        event_free(timer_1m);
    event_base_free(base);
    pool_clients_free();
    bstack_free(bsh);
    bstack_free(bst);
    rx_stop_mining();
    rx_slow_hash_free_state();
    pthread_mutex_destroy(&mutex_clients);
    log_info("Pool shutdown successfully");
    if (fd_log)
        fclose(fd_log);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, sigint_handler);
    atexit(cleanup);

    log_set_level(LOG_INFO);
    log_info("Starting pool");

    static struct option options[] =
    {
        {"config-file", required_argument, 0, 'c'},
        {"log-file", required_argument, 0, 'l'},
        {"block-notified", optional_argument, 0, 'b'},
        {"data-dir", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };
    char *config_file = NULL;
    char *log_file = NULL;
    bool block_notified = false;
    char *data_dir = NULL;
    int c;
    while (1)
    {
        int option_index = 0;
        c = getopt_long (argc, argv, "c:l:b:d:",
                       options, &option_index);
        if (c == -1)
            break;
        switch (c)
        {
            case 'c':
                config_file = strdup(optarg);
                break;
            case 'l':
                log_file = strdup(optarg);
                break;
            case 'b':
                block_notified = true;
                if (optarg)
                    block_notified = atoi(optarg);
                break;
            case 'd':
                data_dir = strdup(optarg);
                break;
        }
    }
    read_config(config_file, log_file, block_notified, data_dir);

    if (config_file)
        free(config_file);
    if (log_file)
        free(log_file);
    if (data_dir)
        free(data_dir);

    log_set_level(LOG_FATAL - config.log_level);
    if (config.log_file[0])
    {
        fd_log = fopen(config.log_file, "a");
        if (!fd_log)
            log_info("Failed to open log file: %s", config.log_file);
        else
            log_set_fp(fd_log);
    }

    bstack_new(&bst, BLOCK_TEMPLATES_MAX, sizeof(block_template_t),
            template_recycle);
    bstack_new(&bsh, BLOCK_HEADERS_MAX, sizeof(block_t), NULL);

    bn_ctx = BN_CTX_new();
    base_diff = NULL;
    BN_hex2bn(&base_diff,
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");

    uuid_t iid;
    uuid_generate(iid);
    memcpy(&instance_id, iid, 4);

    pool_clients_init();

    wui_context_t uic;
    uic.port = config.webui_port;
    uic.pool_stats = &pool_stats;
    uic.pool_fee = config.pool_fee;
    uic.pool_port = config.pool_port;
    uic.allow_self_select = !config.disable_self_select;
    uic.payment_threshold = config.payment_threshold;
    start_web_ui(&uic);

    run();

cleanup:
    return 0;
}
