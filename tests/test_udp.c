/*
 * test_ping.c - sends ping echo requests
 */

#include <stdio.h>
#include <stdlib.h>

#include <runtime/udp.h>
#include <runtime/runtime.h>
#include <runtime/timer.h>

#define DEST_IP_ADDR 3232235778 // 192.168.1.2

static void main_handler(void *arg)
{
    int err;
    udpconnspawn_t *s;
    udpconn_t *c;
    char buf[16];
    ssize_t read_len;
    struct netaddr raddr;
    struct netaddr laddr = { .ip = 0, .port = 4242 };

    err = udp_conn_listen(laddr, 40, &s);
    if (err) {
        printf("Could not make udp listen\n");
        return;
    }

    printf("listening\n");

    err = udp_accept(s, &c);
    if (err) {
        printf("udp accept error\n");
        return;
    }

    raddr = udp_remote_addr(c);
    printf("got connection from %u:%u\n", raddr.ip, raddr.port);
    read_len = udp_read_from(c, buf, 16, &raddr);
    printf("[");
    for (int i = 0 ; i < read_len ; i++) {
        printf("%u, ", buf[i]);
    }
    printf("]\n");


    printf("ok\n");
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc < 2) {
		printf("arg must be config file\n");
		return -EINVAL;
	}

	ret = runtime_init(argv[1], main_handler, NULL);
	if (ret) {
		printf("failed to start runtime\n");
		return ret;
	}

	return 0;
}
