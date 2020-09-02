/*
 * bw_config.h - Breakwater configurations
 */

#pragma once

/* delay threshold */
#define SBW_MIN_DELAY_US		80
#define SBW_DROP_THRESH			180

/* round trip time in us */
#define SBW_RTT_US			10

#define SBW_AI				0.002
#define SBW_MD				0.008
#define CBW_MAX_CLIENT_DELAY_US		10
