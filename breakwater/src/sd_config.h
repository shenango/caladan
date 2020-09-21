/*
 * sd_config.h - SEDA configurations
 */

#pragma once

/* Recommended parameters with 1,000 clinets
*  in XL170 environment
* - 1 us average service time
* #define CSD_MAX_CLIENT_DELAY_US	10
* #define CSD_TB_INIT_RATE		4
* #define CSD_TB_MIN_RATE		2
* #define SEDA_TARGET			50
* #define SEDA_ADJ_I			120
* #define SEDA_ADJ_D			1.04
*
* - 10 us average service time
* #define CSD_MAX_CLIENT_DELAY_US	100
* #define CSD_TB_INIT_RATE		4
* #define CSD_TB_MIN_RATE		2
* #define SEDA_TARGET			80
* #define SEDA_ADJ_I			40
* #define SEDA_ADJ_D			1.04
*
* - 100 us average service time
* #define CSD_MAX_CLIENT_DELAY_US	100
* #define CSD_TB_INIT_RATE		4
* #define CSD_TB_MIN_RATE		2
* #define SEDA_TARGET			720
* #define SEDA_ADJ_I			10
* #define SEDA_ADJ_D			1.3
*/

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
#define SEDA_TARGET			80
/* time before controller run */
#define SEDA_TIMEOUT			1000
/* % error to trigger decrease */
#define SEDA_ERR_D			0.0
/* % error to trigger increase */
#define SEDA_ERR_I			-0.5
/* additive rate increase */
#define SEDA_ADJ_I			4.0
/* multiplicative rate decrease */
#define SEDA_ADJ_D			1.1
/* weight on additive increase */
#define SEDA_CI				-0.1
