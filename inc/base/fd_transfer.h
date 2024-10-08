/*
 * fd_transfer.h - utility functions for sending FDs across UNIX sockets.
 */

#pragma once

extern int recv_fd(int controlfd, int *shared_fd_out);
extern int send_fd(int controlfd, int shared_fd);
