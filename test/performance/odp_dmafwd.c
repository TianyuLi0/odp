/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023 Nokia
 */

/**
 * DMA forwarder
 *
 * This tester application can be used to profile the performance of an ODP DMA implementation.
 * Tester workflow consists of packet reception, copy and forwarding steps. Packets are first
 * received from configured interfaces after which packets are copied, either with plain SW memory
 * copy or with DMA offload copy. Finally, copied packets are echoed back to the sender(s).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <odp_api.h>
#include <odp/helper/odph_api.h>

#define EXIT_NOT_SUP 2
#define PROG_NAME "odp_dmafwd"
#define DELIMITER ","

enum {
	SW_COPY = 0U,
	DMA_COPY
};

#define DEF_CPY_TYPE SW_COPY
#define DEF_CNT 32768U
#define DEF_LEN 1024U
#define DEF_WORKERS 1U
#define DEF_TIME 0U

#define MAX_IFS 2U
#define MAX_OUT_QS 32U
#define MAX_BURST 32U
#define MAX_WORKERS (ODP_THREAD_COUNT_MAX - 1)
#define MAX_PKTIO_INDEXES 1024U

#define MIN(a, b)  (((a) <= (b)) ? (a) : (b))
#define DIV_IF(a, b) ((b) > 0U ? ((a) / (b)) : 0U)

ODP_STATIC_ASSERT(MAX_IFS < UINT8_MAX, "Too large maximum interface count");
ODP_STATIC_ASSERT(MAX_OUT_QS < UINT8_MAX, "Too large maximum output queue count");

typedef struct {
	uint32_t burst_size;
	uint32_t num_pkts;
	uint32_t pkt_len;
	uint32_t cache_size;
} dynamic_defs_t;

typedef enum {
	PRS_OK,
	PRS_NOK,
	PRS_TERM,
	PRS_NOT_SUP
} parse_result_t;

typedef struct prog_config_s prog_config_t;

typedef struct {
	uint64_t copy_errs;
	uint64_t trs;
	uint64_t start_errs;
	uint64_t trs_errs;
	uint64_t buf_alloc_errs;
	uint64_t compl_alloc_errs;
	uint64_t pkt_alloc_errs;
	uint64_t fwd_pkts;
	uint64_t discards;
	uint64_t sched_cc;
	uint64_t tot_cc;
	uint64_t sched_rounds;
} stats_t;

typedef struct ODP_ALIGNED_CACHE {
	prog_config_t *prog_config;
	odp_dma_t dma_handle;
	odp_pool_t compl_pool;
	odp_pool_t copy_pool;
	odp_pool_t trs_pool;
	odp_queue_t compl_q;
	stats_t stats;
	int thr_idx;
} thread_config_t;

typedef struct pktio_s {
	odp_pktout_queue_t out_qs[MAX_OUT_QS];
	char *name;
	odp_pktio_t handle;
	uint8_t num_out_qs;
} pktio_t;

typedef void (*ev_fn_t)(odp_dma_compl_t compl_ev, thread_config_t *config);
typedef void (*pkt_fn_t)(odp_packet_t pkts[], int num, pktio_t *pktio, thread_config_t *config);

typedef struct prog_config_s {
	uint8_t pktio_idx_map[MAX_PKTIO_INDEXES];
	odph_thread_t thread_tbl[MAX_WORKERS];
	thread_config_t thread_config[MAX_WORKERS];
	pktio_t pktios[MAX_IFS];
	dynamic_defs_t dyn_defs;
	odp_instance_t odp_instance;
	odp_barrier_t init_barrier;
	odp_barrier_t term_barrier;
	odp_atomic_u32_t is_running;
	odp_pool_t pktio_pool;
	odp_pool_t copy_pool;
	odp_pool_t trs_pool;
	ev_fn_t ev_fn;
	pkt_fn_t pkt_fn;
	uint32_t burst_size;
	uint32_t num_pkts;
	uint32_t pkt_len;
	uint32_t cache_size;
	uint32_t time_sec;
	int num_thrs;
	uint8_t num_ifs;
	uint8_t copy_type;
} prog_config_t;

typedef struct {
	odp_packet_t pkts[MAX_BURST * 2U];
	pktio_t *pktio;
	int num;
} pkt_vec_t;

typedef struct {
	odp_packet_t src_pkts[MAX_BURST];
	odp_packet_t dst_pkts[MAX_BURST];
	pktio_t *pktio;
	int num;
} transfer_t;

static prog_config_t *prog_conf;

static void terminate(int signal ODP_UNUSED)
{
	odp_atomic_store_u32(&prog_conf->is_running, 0U);
}

static void init_config(prog_config_t *config)
{
	odp_dma_capability_t dma_capa;
	uint32_t burst_size;
	odp_pool_capability_t pool_capa;
	odp_pool_param_t pool_param;
	thread_config_t *thr;

	memset(config, 0, sizeof(*config));

	if (odp_dma_capability(&dma_capa) == 0) {
		burst_size = MIN(dma_capa.max_src_segs, dma_capa.max_dst_segs);
		burst_size = MIN(burst_size, MAX_BURST);
		config->dyn_defs.burst_size = burst_size;
	}

	if (odp_pool_capability(&pool_capa) == 0) {
		config->dyn_defs.num_pkts = pool_capa.pkt.max_num > 0U ?
						MIN(pool_capa.pkt.max_num, DEF_CNT) : DEF_CNT;
		config->dyn_defs.pkt_len = pool_capa.pkt.max_len > 0U ?
						MIN(pool_capa.pkt.max_len, DEF_LEN) : DEF_LEN;
		odp_pool_param_init(&pool_param);
		config->dyn_defs.cache_size = pool_param.pkt.cache_size;
	}

	config->pktio_pool = ODP_POOL_INVALID;
	config->copy_pool = ODP_POOL_INVALID;
	config->trs_pool = ODP_POOL_INVALID;
	config->burst_size = config->dyn_defs.burst_size;
	config->num_pkts = config->dyn_defs.num_pkts;
	config->pkt_len = config->dyn_defs.pkt_len;
	config->cache_size = config->dyn_defs.cache_size;
	config->time_sec = DEF_TIME;
	config->num_thrs = DEF_WORKERS;
	config->copy_type = DEF_CPY_TYPE;

	for (int i = 0; i < MAX_WORKERS; ++i) {
		thr = &config->thread_config[i];
		thr->dma_handle = ODP_DMA_INVALID;
		thr->compl_pool = ODP_POOL_INVALID;
		thr->compl_q = ODP_QUEUE_INVALID;
	}

	for (uint32_t i = 0U; i < MAX_IFS; ++i)
		config->pktios[i].handle = ODP_PKTIO_INVALID;
}

static void print_usage(dynamic_defs_t *dyn_defs)
{
	printf("\n"
	       "DMA performance tester with packet I/O. Receive and forward packets after\n"
	       "software copy or DMA offload copy.\n"
	       "\n"
	       "Usage: " PROG_NAME " OPTIONS\n"
	       "\n"
	       "  E.g. " PROG_NAME " -i eth0\n"
	       "       " PROG_NAME " -i eth0 -t 0\n"
	       "       " PROG_NAME " -i eth0 -t 1 -b 15 -l 4096 -c 5\n"
	       "\n"
	       "Mandatory OPTIONS:\n"
	       "\n"
	       "  -i, --interfaces   Ethernet interfaces for packet I/O, comma-separated, no\n"
	       "                     spaces.\n"
	       "\n"
	       "Optional OPTIONS:\n"
	       "\n"
	       "  -t, --copy_type    Type of copy. %u by default.\n"
	       "                         0: SW\n"
	       "                         1: DMA\n"
	       "  -b, --burst_size   Copy burst size. This many packets are accumulated before\n"
	       "                     copy. %u by default.\n"
	       "  -n, --num_pkts     Number of packet buffers allocated for packet I/O pool.\n"
	       "                     %u by default.\n"
	       "  -l, --pkt_len      Maximum size of packet buffers in packet I/O pool. %u by\n"
	       "                     default.\n"
	       "  -c, --worker_count Amount of workers. %u by default.\n"
	       "  -C, --cache_size   Packet pool cache size. %u by default.\n"
	       "  -T, --time_sec     Time in seconds to run. 0 means infinite. %u by default.\n"
	       "  -h, --help         This help.\n"
	       "\n", DEF_CPY_TYPE, dyn_defs->burst_size, dyn_defs->num_pkts, dyn_defs->pkt_len,
	       DEF_WORKERS, dyn_defs->cache_size, DEF_TIME);
}

static void parse_interfaces(prog_config_t *config, const char *optarg)
{
	char *tmp_str = strdup(optarg), *tmp;

	if (tmp_str == NULL)
		return;

	tmp = strtok(tmp_str, DELIMITER);

	while (tmp && config->num_ifs < MAX_IFS) {
		config->pktios[config->num_ifs].name = strdup(tmp);

		if (config->pktios[config->num_ifs].name != NULL)
			++config->num_ifs;

		tmp = strtok(NULL, DELIMITER);
	}

	free(tmp_str);
}

static parse_result_t check_options(prog_config_t *config)
{
	unsigned int idx = odp_pktio_max_index();
	odp_dma_capability_t dma_capa;
	uint32_t burst_size;
	odp_pool_capability_t pool_capa;

	if (config->num_ifs == 0U) {
		ODPH_ERR("Invalid number of interfaces: %u (min: 1, max: %u)\n", config->num_ifs,
			 MAX_IFS);
		return PRS_NOK;
	}

	if (idx >= MAX_PKTIO_INDEXES) {
		ODPH_ERR("Invalid packet I/O maximum index: %u (max: %u)\n", idx,
			 MAX_PKTIO_INDEXES);
	}

	if (config->copy_type != SW_COPY && config->copy_type != DMA_COPY) {
		ODPH_ERR("Invalid copy type: %u\n", config->copy_type);
		return PRS_NOK;
	}

	if (config->num_thrs <= 0 || config->num_thrs > MAX_WORKERS) {
		ODPH_ERR("Invalid worker count: %d (min: 1, max: %d)\n", config->num_thrs,
			 MAX_WORKERS);
		return PRS_NOK;
	}

	if (odp_dma_capability(&dma_capa) < 0) {
		ODPH_ERR("Error querying DMA capabilities\n");
		return PRS_NOK;
	}

	if ((uint32_t)config->num_thrs > dma_capa.max_sessions) {
		ODPH_ERR("Not enough DMA sessions supported: %d (max: %u)\n", config->num_thrs,
			 dma_capa.max_sessions);
		return PRS_NOT_SUP;
	}

	burst_size = MIN(dma_capa.max_src_segs, dma_capa.max_dst_segs);
	burst_size = MIN(burst_size, MAX_BURST);

	if (config->burst_size == 0U || config->burst_size > burst_size) {
		ODPH_ERR("Unsupported segment count for DMA: %u (min: 1, max: %u)\n",
			 config->burst_size, burst_size);
		return PRS_NOK;
	}

	if (config->pkt_len > dma_capa.max_seg_len) {
		ODPH_ERR("Unsupported packet length for DMA: %u (max: %u)\n", config->pkt_len,
			 dma_capa.max_seg_len);
		return PRS_NOK;
	}

	if ((dma_capa.compl_mode_mask & ODP_DMA_COMPL_EVENT) == 0U || !dma_capa.queue_type_sched) {
		ODPH_ERR("Unsupported completion mode (mode support: %x, scheduled queue "
			 "support: %u\n", dma_capa.compl_mode_mask, dma_capa.queue_type_sched);
		return PRS_NOT_SUP;
	}

	if ((uint32_t)config->num_thrs > dma_capa.pool.max_pools) {
		ODPH_ERR("Unsupported amount of completion pools: %d (max: %u)\n",
			 config->num_thrs, dma_capa.pool.max_pools);
		return PRS_NOK;
	}

	if (odp_pool_capability(&pool_capa) < 0) {
		ODPH_ERR("Error querying pool capabilities\n");
		return PRS_NOK;
	}

	if (config->num_pkts == 0U ||
	    (pool_capa.pkt.max_num > 0U && config->num_pkts > pool_capa.pkt.max_num)) {
		ODPH_ERR("Invalid pool packet count: %u (min: 1, max: %u)\n", config->num_pkts,
			 pool_capa.pkt.max_num);
		return PRS_NOK;
	}

	if (config->pkt_len == 0U ||
	    (pool_capa.pkt.max_len > 0U && config->pkt_len > pool_capa.pkt.max_len)) {
		ODPH_ERR("Invalid pool packet length: %u (min: 1, max: %u)\n", config->pkt_len,
			 pool_capa.pkt.max_len);
		return PRS_NOK;
	}

	if (config->cache_size < pool_capa.pkt.min_cache_size ||
	    config->cache_size > pool_capa.pkt.max_cache_size) {
		ODPH_ERR("Invalid pool cache size: %u (min: %u, max: %u)\n", config->cache_size,
			 pool_capa.pkt.min_cache_size, pool_capa.pkt.max_cache_size);
		return PRS_NOK;
	}

	return PRS_OK;
}

static parse_result_t parse_options(int argc, char **argv, prog_config_t *config)
{
	int opt, long_index;

	static const struct option longopts[] = {
		{ "interfaces", required_argument, NULL, 'i' },
		{ "copy_type", required_argument, NULL, 't' },
		{ "burst_size", required_argument, NULL, 'b' },
		{ "num_pkts", required_argument, NULL, 'n' },
		{ "pkt_len", required_argument, NULL, 'l' },
		{ "worker_count", required_argument, NULL, 'c' },
		{ "cache_size", required_argument, NULL, 'C' },
		{ "time_sec", required_argument, NULL, 'T' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	static const char *shortopts = "i:t:b:n:l:c:C:T:h";

	init_config(config);

	while (1) {
		opt = getopt_long(argc, argv, shortopts, longopts, &long_index);

		if (opt == -1)
			break;

		switch (opt) {
		case 'i':
			parse_interfaces(config, optarg);
			break;
		case 't':
			config->copy_type = atoi(optarg);
			break;
		case 'b':
			config->burst_size = atoi(optarg);
			break;
		case 'n':
			config->num_pkts = atoi(optarg);
			break;
		case 'l':
			config->pkt_len = atoi(optarg);
			break;
		case 'c':
			config->num_thrs = atoi(optarg);
			break;
		case 'C':
			config->cache_size = atoi(optarg);
			break;
		case 'T':
			config->time_sec = atoi(optarg);
			break;
		case 'h':
			print_usage(&config->dyn_defs);
			return PRS_TERM;
		case '?':
		default:
			print_usage(&config->dyn_defs);
			return PRS_NOK;
		}
	}

	return check_options(config);
}

static parse_result_t setup_program(int argc, char **argv, prog_config_t *config)
{
	struct sigaction action = { .sa_handler = terminate };

	if (sigemptyset(&action.sa_mask) == -1 || sigaddset(&action.sa_mask, SIGINT) == -1 ||
	    sigaddset(&action.sa_mask, SIGTERM) == -1 ||
	    sigaddset(&action.sa_mask, SIGHUP) == -1 || sigaction(SIGINT, &action, NULL) == -1 ||
	    sigaction(SIGTERM, &action, NULL) == -1 || sigaction(SIGHUP, &action, NULL) == -1) {
		ODPH_ERR("Error installing signal handler\n");
		return PRS_NOK;
	}

	return parse_options(argc, argv, config);
}

static inline int send_packets(odp_pktout_queue_t queue, odp_packet_t pkts[], int num)
{
	int ret = odp_pktout_send(queue, pkts, num);

	if (odp_unlikely(ret < num)) {
		ret = ret < 0 ? 0 : ret;
		odp_packet_free_multi(&pkts[ret], num - ret);
	}

	return ret;
}

static void sw_copy_and_send_packets(odp_packet_t pkts[], int num, pktio_t *pktio,
				     thread_config_t *config)
{
	odp_packet_t old_pkt, new_pkt;
	odp_pool_t copy_pool = config->copy_pool;
	odp_packet_t out_pkts[num];
	int num_out_pkts = 0, num_sent;
	stats_t *stats = &config->stats;

	for (int i = 0; i < num; ++i) {
		old_pkt = pkts[i];
		new_pkt = odp_packet_copy(old_pkt, copy_pool);

		if (new_pkt != ODP_PACKET_INVALID)
			out_pkts[num_out_pkts++] = new_pkt;
		else
			++stats->copy_errs;

		odp_packet_free(old_pkt);
	}

	if (num_out_pkts > 0) {
		num_sent = send_packets(pktio->out_qs[config->thr_idx % pktio->num_out_qs],
					out_pkts, num_out_pkts);
		stats->fwd_pkts += num_sent;
		stats->discards += num_out_pkts - num_sent;
	}
}

static inline void send_dma_trs_packets(odp_dma_compl_t compl_ev, thread_config_t *config)
{
	odp_dma_result_t res;
	odp_buffer_t buf;
	transfer_t *trs;
	pktio_t *pktio;
	int num_sent;
	stats_t *stats = &config->stats;

	memset(&res, 0, sizeof(res));
	odp_dma_compl_result(compl_ev, &res);
	buf = (odp_buffer_t)res.user_ptr;
	trs = (transfer_t *)odp_buffer_addr(buf);
	pktio = trs->pktio;

	if (res.success) {
		num_sent = send_packets(pktio->out_qs[config->thr_idx % pktio->num_out_qs],
					trs->dst_pkts, trs->num);
		++stats->trs;
		stats->fwd_pkts += num_sent;
		stats->discards += trs->num - num_sent;
	} else {
		odp_packet_free_multi(trs->dst_pkts, trs->num);
		++stats->trs_errs;
	}

	odp_packet_free_multi(trs->src_pkts, trs->num);
	odp_buffer_free(buf);
	odp_dma_compl_free(compl_ev);
}

static inline transfer_t *init_dma_trs(odp_dma_transfer_param_t *trs_param,
				       odp_dma_compl_param_t *compl_param, odp_dma_seg_t *src_segs,
				       odp_dma_seg_t *dst_segs, pktio_t *pktio,
				       thread_config_t *config)
{
	odp_buffer_t buf;
	stats_t *stats = &config->stats;
	transfer_t *trs;
	odp_dma_compl_t c_ev;

	buf = odp_buffer_alloc(config->trs_pool);

	if (odp_unlikely(buf == ODP_BUFFER_INVALID)) {
		++stats->buf_alloc_errs;
		return NULL;
	}

	trs = (transfer_t *)odp_buffer_addr(buf);
	trs->num = 0;
	trs->pktio = pktio;
	trs_param->src_format = ODP_DMA_FORMAT_PACKET;
	trs_param->dst_format = ODP_DMA_FORMAT_PACKET;
	trs_param->num_src = 0U;
	trs_param->num_dst = 0U;
	trs_param->src_seg = src_segs;
	trs_param->dst_seg = dst_segs;
	compl_param->compl_mode = ODP_DMA_COMPL_EVENT;
	c_ev = odp_dma_compl_alloc(config->compl_pool);

	if (odp_unlikely(c_ev == ODP_DMA_COMPL_INVALID)) {
		odp_buffer_free(buf);
		++stats->compl_alloc_errs;
		return NULL;
	}

	compl_param->event = odp_dma_compl_to_event(c_ev);
	compl_param->queue = config->compl_q;
	compl_param->user_ptr = buf;
	memset(src_segs, 0, sizeof(*src_segs) * MAX_BURST);
	memset(dst_segs, 0, sizeof(*dst_segs) * MAX_BURST);

	return trs;
}

static void dma_copy(odp_packet_t pkts[], int num, pktio_t *pktio, thread_config_t *config)
{
	odp_dma_transfer_param_t trs_param;
	odp_dma_compl_param_t compl_param;
	odp_packet_t pkt;
	transfer_t *trs = NULL;
	odp_dma_seg_t src_segs[MAX_BURST], dst_segs[MAX_BURST];
	uint32_t num_segs = 0U, pkt_len;
	odp_pool_t copy_pool = config->copy_pool;
	stats_t *stats = &config->stats;

	odp_dma_transfer_param_init(&trs_param);
	odp_dma_compl_param_init(&compl_param);

	for (int i = 0; i < num; ++i) {
		pkt = pkts[i];

		if (odp_unlikely(trs == NULL)) {
			trs = init_dma_trs(&trs_param, &compl_param, src_segs, dst_segs, pktio,
					   config);

			if (trs == NULL) {
				odp_packet_free(pkt);
				continue;
			}
		}

		pkt_len = odp_packet_len(pkt);
		src_segs[num_segs].packet = pkt;
		src_segs[num_segs].len = pkt_len;
		dst_segs[num_segs].packet = odp_packet_alloc(copy_pool, pkt_len);

		if (odp_unlikely(dst_segs[num_segs].packet == ODP_PACKET_INVALID)) {
			odp_packet_free(pkt);
			++stats->pkt_alloc_errs;
			continue;
		}

		dst_segs[num_segs].len = pkt_len;
		trs->src_pkts[num_segs] = src_segs[num_segs].packet;
		trs->dst_pkts[num_segs] = dst_segs[num_segs].packet;
		++trs->num;
		++trs_param.num_src;
		++trs_param.num_dst;
		++num_segs;
	}

	if (num_segs > 0U)
		if (odp_dma_transfer_start(config->dma_handle, &trs_param, &compl_param) <= 0) {
			odp_packet_free_multi(trs->src_pkts, trs->num);
			odp_packet_free_multi(trs->dst_pkts, trs->num);
			++stats->start_errs;
		}
}

static odp_bool_t setup_copy(prog_config_t *config)
{
	odp_pool_param_t pool_param;
	thread_config_t *thr;
	const odp_dma_param_t dma_params = {
		.direction = ODP_DMA_MAIN_TO_MAIN,
		.type = ODP_DMA_TYPE_COPY,
		.compl_mode_mask = ODP_DMA_COMPL_EVENT,
		.mt_mode = ODP_DMA_MT_SERIAL,
		.order = ODP_DMA_ORDER_NONE };
	odp_dma_pool_param_t compl_pool_param;
	odp_queue_param_t queue_param;

	odp_pool_param_init(&pool_param);
	pool_param.pkt.seg_len = config->pkt_len;
	pool_param.pkt.len = config->pkt_len;
	pool_param.pkt.num = config->num_pkts;
	pool_param.pkt.cache_size = config->cache_size;
	pool_param.type = ODP_POOL_PACKET;
	config->copy_pool = odp_pool_create(PROG_NAME "_copy", &pool_param);

	if (config->copy_pool == ODP_POOL_INVALID) {
		ODPH_ERR("Error creating packet copy pool\n");
		return false;
	}

	if (config->copy_type == SW_COPY) {
		config->ev_fn = NULL;
		config->pkt_fn = sw_copy_and_send_packets;

		for (int i = 0; i < config->num_thrs; ++i)
			config->thread_config[i].copy_pool = config->copy_pool;

		return true;
	}

	pool_param.buf.num = config->num_pkts;
	pool_param.buf.size = sizeof(transfer_t);
	pool_param.type = ODP_POOL_BUFFER;
	config->trs_pool = odp_pool_create(PROG_NAME "_dma_trs", &pool_param);

	if (config->trs_pool == ODP_POOL_INVALID) {
		ODPH_ERR("Error creating DMA transfer tracking pool\n");
		return false;
	}

	for (int i = 0; i < config->num_thrs; ++i) {
		thr = &config->thread_config[i];
		thr->dma_handle = odp_dma_create(PROG_NAME "_dma", &dma_params);

		if (thr->dma_handle == ODP_DMA_INVALID) {
			ODPH_ERR("Error creating DMA session\n");
			return false;
		}

		odp_dma_pool_param_init(&compl_pool_param);
		compl_pool_param.num = config->num_pkts;
		thr->compl_pool = odp_dma_pool_create(PROG_NAME "_dma_compl", &compl_pool_param);

		if (thr->compl_pool == ODP_POOL_INVALID) {
			ODPH_ERR("Error creating DMA event completion pool\n");
			return false;
		}

		thr->copy_pool = config->copy_pool;
		thr->trs_pool = config->trs_pool;
		odp_queue_param_init(&queue_param);
		queue_param.type = ODP_QUEUE_TYPE_SCHED;
		queue_param.sched.sync = ODP_SCHED_SYNC_PARALLEL;
		queue_param.sched.prio = odp_schedule_max_prio();
		thr->compl_q = odp_queue_create(PROG_NAME "_dma_compl", &queue_param);

		if (thr->compl_q == ODP_QUEUE_INVALID) {
			ODPH_ERR("Error creating DMA completion queue\n");
			return false;
		}
	}

	config->ev_fn = send_dma_trs_packets;
	config->pkt_fn = dma_copy;

	return true;
}

static odp_bool_t setup_pktios(prog_config_t *config)
{
	odp_pool_param_t pool_param;
	pktio_t *pktio;
	odp_pktio_param_t pktio_param;
	odp_pktio_capability_t capa;
	uint32_t num_input_qs, num_output_qs;
	odp_pktin_queue_param_t pktin_param;
	odp_pktout_queue_param_t pktout_param;

	odp_pool_param_init(&pool_param);
	pool_param.pkt.seg_len = config->pkt_len;
	pool_param.pkt.len = config->pkt_len;
	pool_param.pkt.num = config->num_pkts;
	pool_param.pkt.cache_size = config->cache_size;
	pool_param.type = ODP_POOL_PACKET;
	config->pktio_pool = odp_pool_create(PROG_NAME, &pool_param);

	if (config->pktio_pool == ODP_POOL_INVALID) {
		ODPH_ERR("Error creating packet I/O pool\n");
		return false;
	}

	for (uint32_t i = 0U; i < config->num_ifs; ++i) {
		pktio = &config->pktios[i];
		odp_pktio_param_init(&pktio_param);
		pktio_param.in_mode = ODP_PKTIN_MODE_SCHED;
		pktio_param.out_mode = ODP_PKTOUT_MODE_DIRECT;
		pktio->handle = odp_pktio_open(pktio->name, config->pktio_pool, &pktio_param);

		if (pktio->handle == ODP_PKTIO_INVALID) {
			ODPH_ERR("Error opening packet I/O (%s)\n", pktio->name);
			return false;
		}

		config->pktio_idx_map[odp_pktio_index(pktio->handle)] = i;

		if (odp_pktio_capability(pktio->handle, &capa) < 0) {
			ODPH_ERR("Error querying packet I/O capabilities (%s)\n", pktio->name);
			return false;
		}

		num_input_qs = MIN((uint32_t)config->num_thrs, capa.max_input_queues);
		num_output_qs = MIN((uint32_t)config->num_thrs, capa.max_output_queues);
		num_output_qs = MIN(num_output_qs, MAX_OUT_QS);
		odp_pktin_queue_param_init(&pktin_param);

		if (num_input_qs > 1) {
			pktin_param.hash_enable = true;
			pktin_param.hash_proto.proto.ipv4 = 1U;
		}

		pktin_param.num_queues = num_input_qs;
		pktin_param.queue_param.sched.prio = odp_schedule_default_prio();

		if (odp_pktin_queue_config(pktio->handle, &pktin_param) < 0) {
			ODPH_ERR("Error configuring packet I/O input queues (%s)\n", pktio->name);
			return false;
		}

		odp_pktout_queue_param_init(&pktout_param);

		if (num_output_qs == (uint32_t)config->num_thrs)
			pktout_param.op_mode = ODP_PKTIO_OP_MT_UNSAFE;

		pktout_param.num_queues = num_output_qs;
		pktio->num_out_qs = num_output_qs;

		if (odp_pktout_queue_config(pktio->handle, &pktout_param) < 0) {
			ODPH_ERR("Error configuring packet I/O output queues (%s)\n", pktio->name);
			return false;
		}

		if (odp_pktout_queue(pktio->handle, pktio->out_qs, num_output_qs) !=
		    (int)num_output_qs) {
			ODPH_ERR("Error querying packet I/O output queues (%s)\n", pktio->name);
			return false;
		}

		if (odp_pktio_start(pktio->handle) < 0) {
			ODPH_ERR("Error starting packet I/O (%s)\n", pktio->name);
			return false;
		}
	}

	return true;
}

static inline void push_packet(odp_packet_t pkt, pkt_vec_t pkt_vecs[], uint8_t *pktio_idx_map)
{
	uint8_t idx = pktio_idx_map[odp_packet_input_index(pkt)];
	pkt_vec_t *pkt_vec = &pkt_vecs[idx];

	pkt_vec->pkts[pkt_vec->num++] = pkt;
}

static inline void pop_packets(pkt_vec_t *pkt_vec, int num_procd)
{
	pkt_vec->num -= num_procd;

	for (int i = 0, j = num_procd; i < pkt_vec->num; ++i, ++j)
		pkt_vec->pkts[i] = pkt_vec->pkts[j];
}

static void free_pending_packets(pkt_vec_t pkt_vecs[], uint32_t num_ifs)
{
	for (uint32_t i = 0U; i < num_ifs; ++i)
		odp_packet_free_multi(pkt_vecs[i].pkts, pkt_vecs[i].num);
}

static void drain(void)
{
	odp_event_t ev;
	odp_event_type_t type;
	odp_dma_result_t res;
	odp_buffer_t buf;
	transfer_t *trs;

	while (true) {
		ev = odp_schedule(NULL, odp_schedule_wait_time(ODP_TIME_SEC_IN_NS * 2U));

		if (ev == ODP_EVENT_INVALID)
			break;

		type = odp_event_type(ev);

		if (type == ODP_EVENT_DMA_COMPL) {
			memset(&res, 0, sizeof(res));
			odp_dma_compl_result(odp_dma_compl_from_event(ev), &res);
			buf = (odp_buffer_t)res.user_ptr;
			trs = (transfer_t *)odp_buffer_addr(buf);
			odp_packet_free_multi(trs->src_pkts, trs->num);
			odp_packet_free_multi(trs->dst_pkts, trs->num);
			odp_buffer_free(buf);
		}

		odp_event_free(ev);
	}
}

static int process_packets(void *args)
{
	thread_config_t *config = args;
	const uint8_t num_ifs = config->prog_config->num_ifs;
	pkt_vec_t pkt_vecs[num_ifs], *pkt_vec;
	odp_atomic_u32_t *is_running = &config->prog_config->is_running;
	uint64_t c1, c2, c3, c4, cdiff = 0U, rounds = 0U;
	const int burst_size = config->prog_config->burst_size;
	odp_event_t evs[burst_size];
	int num_evs;
	odp_event_t ev;
	odp_event_type_t type;
	ev_fn_t ev_fn = config->prog_config->ev_fn;
	uint8_t *pktio_map = config->prog_config->pktio_idx_map;
	stats_t *stats = &config->stats;
	pkt_fn_t pkt_fn = config->prog_config->pkt_fn;

	for (uint32_t i = 0U; i < num_ifs; ++i) {
		pkt_vecs[i].pktio = &config->prog_config->pktios[i];
		pkt_vecs[i].num = 0;
	}

	config->thr_idx = odp_thread_id();
	odp_barrier_wait(&config->prog_config->init_barrier);
	c1 = odp_cpu_cycles();

	while (odp_atomic_load_u32(is_running)) {
		c3 = odp_cpu_cycles();
		num_evs = odp_schedule_multi_no_wait(NULL, evs, burst_size);
		c4 = odp_cpu_cycles();
		cdiff += odp_cpu_cycles_diff(c4, c3);
		++rounds;

		if (num_evs == 0)
			continue;

		for (int i = 0; i < num_evs; ++i) {
			ev = evs[i];
			type = odp_event_type(ev);

			if (type == ODP_EVENT_DMA_COMPL) {
				if (ev_fn)
					ev_fn(odp_dma_compl_from_event(ev), config);
			} else if (type == ODP_EVENT_PACKET) {
				push_packet(odp_packet_from_event(ev), pkt_vecs, pktio_map);
			} else {
				odp_event_free(ev);
				++stats->discards;
			}
		}

		for (uint32_t i = 0U; i < num_ifs; ++i) {
			pkt_vec = &pkt_vecs[i];

			if (pkt_vec->num >= burst_size) {
				pkt_fn(pkt_vec->pkts, burst_size, pkt_vec->pktio, config);
				pop_packets(pkt_vec, burst_size);
			}
		}
	}

	c2 = odp_cpu_cycles();
	stats->sched_cc = cdiff;
	stats->tot_cc = odp_cpu_cycles_diff(c2, c1);
	stats->sched_rounds = rounds;
	free_pending_packets(pkt_vecs, num_ifs);
	odp_barrier_wait(&config->prog_config->term_barrier);
	drain();

	return 0;
}

static odp_bool_t setup_workers(prog_config_t *config)
{
	odp_cpumask_t cpumask;
	int num_workers;
	odph_thread_common_param_t thr_common;
	odph_thread_param_t thr_param[config->num_thrs];

	num_workers = odp_cpumask_default_worker(&cpumask, config->num_thrs);
	odph_thread_common_param_init(&thr_common);
	thr_common.instance = config->odp_instance;
	thr_common.cpumask = &cpumask;

	for (int i = 0; i < config->num_thrs; ++i) {
		odph_thread_param_init(&thr_param[i]);
		thr_param[i].start = process_packets;
		thr_param[i].thr_type = ODP_THREAD_WORKER;
		config->thread_config[i].prog_config = config;
		thr_param[i].arg = &config->thread_config[i];
	}

	num_workers = odph_thread_create(config->thread_tbl, &thr_common, thr_param, num_workers);

	if (num_workers != config->num_thrs) {
		ODPH_ERR("Error configuring worker threads\n");
		return false;
	}

	return true;
}

static odp_bool_t setup_test(prog_config_t *config)
{
	odp_barrier_init(&config->init_barrier, config->num_thrs + 1);
	odp_barrier_init(&config->term_barrier, config->num_thrs + 1);

	if (!setup_copy(config))
		return false;

	if (!setup_pktios(config))
		return false;

	if (!setup_workers(config))
		return false;

	odp_barrier_wait(&config->init_barrier);

	return true;
}

static void stop_test(prog_config_t *config)
{
	for (uint32_t i = 0U; i < config->num_ifs; ++i)
		if (config->pktios[i].handle != ODP_PKTIO_INVALID)
			(void)odp_pktio_stop(config->pktios[i].handle);

	odp_barrier_wait(&config->term_barrier);
	(void)odph_thread_join(config->thread_tbl, config->num_thrs);
}

static void teardown(prog_config_t *config)
{
	thread_config_t *thr;

	for (uint32_t i = 0U; i < config->num_ifs; ++i) {
		free(config->pktios[i].name);

		if (config->pktios[i].handle != ODP_PKTIO_INVALID)
			(void)odp_pktio_close(config->pktios[i].handle);
	}

	if (config->pktio_pool != ODP_POOL_INVALID)
		(void)odp_pool_destroy(config->pktio_pool);

	for (int i = 0; i < config->num_thrs; ++i) {
		thr = &config->thread_config[i];

		if (thr->compl_q != ODP_QUEUE_INVALID)
			(void)odp_queue_destroy(thr->compl_q);

		if (thr->compl_pool != ODP_POOL_INVALID)
			(void)odp_pool_destroy(thr->compl_pool);

		if (thr->dma_handle != ODP_DMA_INVALID)
			(void)odp_dma_destroy(thr->dma_handle);
	}

	if (config->copy_pool != ODP_POOL_INVALID)
		(void)odp_pool_destroy(config->copy_pool);

	if (config->trs_pool != ODP_POOL_INVALID)
		(void)odp_pool_destroy(config->trs_pool);
}

static void print_stats(const prog_config_t *config)
{
	const stats_t *stats;
	const char *align = config->copy_type == SW_COPY ? "  " : "                  ";

	printf("\n==================\n\n"
	       "DMA forwarder done\n\n"
	       "    copy mode:       %s\n"
	       "    burst size:      %u\n"
	       "    packet length:   %u\n"
	       "    pool cache size: %u\n", config->copy_type == SW_COPY ? "SW" : "DMA",
	       config->burst_size, config->pkt_len, config->cache_size);

	for (int i = 0; i < config->num_thrs; ++i) {
		stats = &config->thread_config[i].stats;

		printf("\n    worker %d:\n", i);

		if (config->copy_type == SW_COPY) {
			printf("        packet copy errors: %" PRIu64 "\n",
			       stats->copy_errs);
		} else {
			printf("        successful DMA transfers:           %" PRIu64 "\n"
			       "        DMA transfer start errors:          %" PRIu64 "\n"
			       "        DMA transfer errors:                %" PRIu64 "\n"
			       "        transfer buffer allocation errors:  %" PRIu64 "\n"
			       "        completion event allocation errors: %" PRIu64 "\n"
			       "        copy packet allocation errors:      %" PRIu64 "\n",
			       stats->trs, stats->start_errs, stats->trs_errs,
			       stats->buf_alloc_errs, stats->compl_alloc_errs,
			       stats->pkt_alloc_errs);
		}

		printf("        packets forwarded:%s%" PRIu64 "\n"
		       "        packets dropped:  %s%" PRIu64 "\n"
		       "        call cycles per schedule round:\n"
		       "            total:    %" PRIu64 "\n"
		       "            schedule: %" PRIu64 "\n"
		       "            rounds:   %" PRIu64 "\n", align, stats->fwd_pkts, align,
		       stats->discards, DIV_IF(stats->tot_cc, stats->sched_rounds),
		       DIV_IF(stats->sched_cc, stats->sched_rounds), stats->sched_rounds);
	}

	printf("\n==================\n");
}

int main(int argc, char **argv)
{
	odph_helper_options_t odph_opts;
	odp_init_t init_param;
	odp_instance_t odp_instance;
	odp_shm_t shm_cfg = ODP_SHM_INVALID;
	int ret = EXIT_SUCCESS;
	parse_result_t parse_res;

	argc = odph_parse_options(argc, argv);

	if (odph_options(&odph_opts) == -1) {
		ODPH_ERR("Error while reading ODP helper options, exiting\n");
		exit(EXIT_FAILURE);
	}

	odp_init_param_init(&init_param);
	init_param.mem_model = odph_opts.mem_model;

	if (odp_init_global(&odp_instance, NULL, NULL)) {
		ODPH_ERR("ODP global init failed, exiting.\n");
		exit(EXIT_FAILURE);
	}

	if (odp_init_local(odp_instance, ODP_THREAD_CONTROL)) {
		ODPH_ERR("ODP local init failed, exiting.\n");
		exit(EXIT_FAILURE);
	}

	shm_cfg = odp_shm_reserve(PROG_NAME "_cfg", sizeof(prog_config_t), ODP_CACHE_LINE_SIZE,
				  0U);

	if (shm_cfg == ODP_SHM_INVALID) {
		ODPH_ERR("Error reserving shared memory\n");
		ret = EXIT_FAILURE;
		goto out;
	}

	prog_conf = odp_shm_addr(shm_cfg);

	if (prog_conf == NULL) {
		ODPH_ERR("Error resolving shared memory address\n");
		ret = EXIT_FAILURE;
		goto out;
	}

	parse_res = setup_program(argc, argv, prog_conf);

	if (parse_res == PRS_NOK) {
		ret = EXIT_FAILURE;
		goto out_test;
	}

	if (parse_res == PRS_TERM) {
		ret = EXIT_SUCCESS;
		goto out_test;
	}

	if (parse_res == PRS_NOT_SUP) {
		ret = EXIT_NOT_SUP;
		goto out_test;
	}

	if (odp_schedule_config(NULL) < 0) {
		ODPH_ERR("Error configuring scheduler\n");
		ret = EXIT_FAILURE;
		goto out_test;
	}

	prog_conf->odp_instance = odp_instance;
	odp_atomic_init_u32(&prog_conf->is_running, 1U);

	if (!setup_test(prog_conf)) {
		ret = EXIT_FAILURE;
		goto out_test;
	}

	if (prog_conf->time_sec) {
		sleep(prog_conf->time_sec);
		odp_atomic_store_u32(&prog_conf->is_running, 0U);
	} else {
		while (odp_atomic_load_u32(&prog_conf->is_running))
			sleep(1U);
	}

	stop_test(prog_conf);
	print_stats(prog_conf);

out_test:
	teardown(prog_conf);

out:
	if (shm_cfg != ODP_SHM_INVALID)
		(void)odp_shm_free(shm_cfg);

	if (odp_term_local()) {
		ODPH_ERR("ODP local terminate failed, exiting.\n");
		exit(EXIT_FAILURE);
	}

	if (odp_term_global(odp_instance)) {
		ODPH_ERR("ODP global terminate failed, exiting.\n");
		exit(EXIT_FAILURE);
	}

	return ret;
}
