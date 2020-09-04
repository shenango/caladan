/*
 * dg_config.h - Dagor configurations
 */

#pragma once

/* Recommended parameters with 1,000 clinets
*  in XL170 environment
* - 1 us average service time
* (bimod) #define DAGOR_OVERLOAD_THRESH		20
* (exp) #define DAGOR_OVERLOAD_THRESH		30
* (const) #define DAGOR_OVERLOAD_THRESH		30
*
* - 10 us average service time
* (bimod) #define DAGOR_OVERLOAD_THRESH		100
* (exp) #define DAGOR_OVERLOAD_THRESH		100
* (const) #define DAGOR_OVERLOAD_THRESH		100
*
* - 100 us average service time
* (bimod) #define DAGOR_OVERLOAD_THRESH		820
* (exp) #define DAGOR_OVERLOAD_THRESH		820
* (const) #define DAGOR_OVERLOAD_THRESH		820
*/

/* delay threshold to detect congestion */
#define DAGOR_OVERLOAD_THRESH	100	// in us
/* max priority update interval */
#define DAGOR_PRIO_UPDATE_INT	1000	// in us
/* max # requests for priority update */
#define DAGOR_PRIO_UPDATE_REQS	2000	// in # reqs
/* queueing delay monitor interval */
#define DAGOR_PRIO_MONITOR	10
/* decrement factor when congested */
#define DAGOR_ALPHA		0.95
/* increment factor when uncongested */
#define DAGOR_BETA		0.01

#define CDG_BATCH_WAIT_US	0
