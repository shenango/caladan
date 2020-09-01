/*
 * dg_config.h - Dagor configurations
 */

#pragma once

/* delay threshold to detect congestion */
#define DAGOR_OVERLOAD_THRESH	120	// in us
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

#define CDG_BATCH_WAIT_US		0
