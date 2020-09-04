/*
 * bw_config.h - Breakwater configurations
 */

#pragma once

/* Recommended parameters (in XL170 environment)
* - 1 us average service time
* #define SBW_MIN_DELAY_US		40
* #define SBW_DROP_THRESH		80
*
* - 10 us average service time
* #define SBW_MIN_DELAY_US		80
* #define SBW_DROP_THRESH		180
*
* - 100 us average service time
* #define SBW_MIN_DELAY_US		300
* #define SBW_DROP_THRESH		1000
*/

/* delay threshold to detect congestion */
#define SBW_MIN_DELAY_US		80
/* delay threshold for AQM */
#define SBW_DROP_THRESH			180

/* round trip time in us */
#define SBW_RTT_US			10

#define SBW_AI				0.002
#define SBW_MD				0.008
#define CBW_MAX_CLIENT_DELAY_US		10
