/*
 * signal.h - support for setting up signal handlers without using glibc
 */

#pragma once

#include <signal.h>

extern int base_sigaction(int sig, const struct sigaction *act,
                          struct sigaction *oact);
extern int base_sigaction_full(int sig, const struct sigaction *act,
                               struct sigaction *oact);