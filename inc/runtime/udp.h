/*
 * udp.h - UDP sockets
 */

#pragma once

#include <base/types.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <net/udp.h>
#include <runtime/net.h>
#include <sys/uio.h>

/* the maximum possible payload size (for the largest possible MTU) */
#define UDP_MAX_PAYLOAD \
	(ETH_MAX_MTU - sizeof(struct ip_hdr) - sizeof(struct udp_hdr))

extern unsigned int udp_payload_size;

/**
 * udp_get_payload_size - returns the MTU-limited payload size
 */
static inline unsigned int udp_get_payload_size(void)
{
	return udp_payload_size;
}


/*
 * UDP Socket API
 */

struct udpconn;
typedef struct udpconn udpconn_t;

extern int udp_dial(struct netaddr laddr, struct netaddr raddr,
		    udpconn_t **c_out);
extern int udp_listen(struct netaddr laddr, udpconn_t **c_out);
extern struct netaddr udp_local_addr(udpconn_t *c);
extern struct netaddr udp_remote_addr(udpconn_t *c);
extern int udp_set_buffers(udpconn_t *c, int read_mbufs, int write_mbufs);
extern ssize_t udp_read_from(udpconn_t *c, void *buf, size_t len,
			     struct netaddr *raddr);
extern ssize_t udp_write_to(udpconn_t *c, const void *buf, size_t len,
			    const struct netaddr *raddr);
extern ssize_t udp_read(udpconn_t *c, void *buf, size_t len);
extern ssize_t udp_write(udpconn_t *c, const void *buf, size_t len);
extern void udp_shutdown(udpconn_t *c);
extern void udp_close(udpconn_t *c);

extern void udp_set_nonblocking(udpconn_t *c, bool nonblocking);

/*
 * UDP Parallel API
 */

struct udpspawner;
typedef struct udpspawner udpspawner_t;

struct udp_spawn_data {
	const void	*buf;
	size_t		len;
	struct netaddr	laddr;
	struct netaddr	raddr;
	void		*release_data;
};

typedef void (*udpspawn_fn_t)(struct udp_spawn_data *d);

extern int udp_create_spawner(struct netaddr laddr, udpspawn_fn_t fn,
			      udpspawner_t **s_out);
extern void udp_destroy_spawner(udpspawner_t *s);
extern ssize_t udp_send(const void *buf, size_t len,
			struct netaddr laddr, struct netaddr raddr);
extern ssize_t udp_sendv(const struct iovec *iov, int iovcnt,
			 struct netaddr laddr, struct netaddr raddr);
extern void udp_spawn_data_release(void *release_data);

/**
 * udp_respond - sends a response datagram to a spawner datagram
 * @buf: a buffer containing the datagram
 * @len: the length of the datagram
 * @d: the UDP spawner data
 *
 * Returns @len if successful, otherwise fail.
 */
static inline ssize_t udp_respond(const void *buf, size_t len,
				  struct udp_spawn_data *d)
{
	return udp_send(buf, len, d->laddr, d->raddr);
}

static inline ssize_t udp_respondv(const struct iovec *iov, int iovcnt,
				   struct udp_spawn_data *d)
{
	return udp_sendv(iov, iovcnt, d->laddr, d->raddr);
}
