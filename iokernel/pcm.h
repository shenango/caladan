#pragma once

/* Declarations of relevant functions in patched PCM library */
extern uint32_t pcm_caladan_get_cas_count(uint32_t channel);
extern uint32_t pcm_caladan_get_active_channel_count(void);
extern int pcm_caladan_init(int socket);

