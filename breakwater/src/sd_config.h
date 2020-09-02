/*
 * sd_config.h - SEDA configurations
 */

#pragma once

/* maximum client delay */
#define CSD_MAX_CLIENT_DELAY_US		100
/* Token bucket initial rate (reqs/sec) */
#define CSD_TB_INIT_RATE		4
/* Token bucket minimum rate (reqs/sec) */
#define CSD_TB_MIN_RATE			2
/* Token bucket maximum number of token (burstiness) */
#define CSD_TB_MAX_TOKEN		4
/* EWMA filter constant */
#define SEDA_ALPHA			0.7
/* target 90th percentile delay */
#define SEDA_TARGET			220
/* time before controller run */
#define SEDA_TIMEOUT			1000
/* % error to trigger decrease */
#define SEDA_ERR_D			0.0
/* % error to trigger increase */
#define SEDA_ERR_I			-0.5
/* additive rate increase */
#define SEDA_ADJ_I			40.0
/* multiplicative rate decrease */
#define SEDA_ADJ_D			1.1
/* weight on additive increase */
#define SEDA_CI				-0.1
