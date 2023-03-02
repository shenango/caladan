/*
 * udp.c - support for User Datagram Protocol (UDP)
 */

#include <string.h>

#include <base/hash.h>
#include <base/kref.h>
#include <runtime/smalloc.h>
#include <runtime/rculist.h>
#include <runtime/sync.h>
#include <runtime/thread.h>
#include <runtime/udp.h>

#include "defs.h"
#include "waitq.h"

#define UDP_IN_DEFAULT_CAP	512
#define UDP_OUT_DEFAULT_CAP	2048

unsigned int udp_payload_size;

static int udp_send_raw(struct mbuf *m, size_t len,
			struct netaddr laddr, struct netaddr raddr)
{
	struct udp_hdr *udphdr;

	/* rewrite loopback address */
	if (raddr.ip == MAKE_IP_ADDR(127, 0, 0, 1))
		raddr.ip = netcfg.addr;

	/* write UDP header */
	udphdr = mbuf_push_hdr(m, *udphdr);
	udphdr->src_port = hton16(laddr.port);
	udphdr->dst_port = hton16(raddr.port);
	udphdr->len = hton16(len + sizeof(*udphdr));
	udphdr->chksum = 0;

	/* send the IP packet */
	return net_tx_ip(m, IPPROTO_UDP, raddr.ip);
}


/*
 * UDP Socket Support
 */

struct udpconn {
	struct trans_entry	e;
	bool			shutdown;
	bool			nonblocking;

	/* ingress support */
	spinlock_t		inq_lock;
	int			inq_cap;
	int			inq_len;
	int			inq_err;
	waitq_t			inq_wq;
	struct mbufq		inq;

	/* egress support */
	spinlock_t		outq_lock;
	bool			outq_free;
	int			outq_cap;
	int			outq_len;
	waitq_t			outq_wq;

	struct kref		ref;
	struct flow_registration		flow;
};

/* handles ingress packets for UDP sockets */
static void udp_conn_recv(struct trans_entry *e, struct mbuf *m)
{
	udpconn_t *c = container_of(e, udpconn_t, e);
	thread_t *th;

	if (unlikely(!mbuf_pull_hdr_or_null(m, sizeof(struct udp_hdr)))) {
		mbuf_free(m);
		return;
	}

	spin_lock_np(&c->inq_lock);
	/* drop packet if the ingress queue is full */
	if (c->inq_len >= c->inq_cap || c->inq_err || c->shutdown) {
		spin_unlock_np(&c->inq_lock);
		mbuf_drop(m);
		return;
	}

	/* enqueue the packet on the ingress queue */
	mbufq_push_tail(&c->inq, m);
	c->inq_len++;

	/* wake up a waiter */
	th = waitq_signal(&c->inq_wq, &c->inq_lock);
	spin_unlock_np(&c->inq_lock);

	waitq_signal_finish(th);
}

/* handles network errors for UDP sockets */
static void udp_conn_err(struct trans_entry *e, int err)
{
	udpconn_t *c = container_of(e, udpconn_t, e);

	bool do_release;

	spin_lock_np(&c->inq_lock);
	do_release = !c->inq_err && !c->shutdown;
	c->inq_err = err;
	spin_unlock_np(&c->inq_lock);

	if (do_release)
		waitq_release(&c->inq_wq);
}

/* operations for UDP sockets */
static const struct trans_ops udp_conn_ops = {
	.recv = udp_conn_recv,
	.err = udp_conn_err,
};

static void udp_init_conn(udpconn_t *c)
{
	c->shutdown = false;
	c->nonblocking = false;

	/* initialize ingress fields */
	spin_lock_init(&c->inq_lock);
	c->inq_cap = UDP_IN_DEFAULT_CAP;
	c->inq_len = 0;
	c->inq_err = 0;
	waitq_init(&c->inq_wq);
	mbufq_init(&c->inq);

	/* initialize egress fields */
	spin_lock_init(&c->outq_lock);
	c->outq_free = false;
	c->outq_cap = UDP_OUT_DEFAULT_CAP;
	c->outq_len = 0;
	waitq_init(&c->outq_wq);

	kref_init(&c->ref);
}

static void udp_finish_release_conn(struct rcu_head *h)
{
	udpconn_t *c = container_of(h, udpconn_t, e.rcu);
	sfree(c);
}

static void udp_release_conn_ref(struct kref *ref)
{
	udpconn_t *c = container_of(ref, udpconn_t, ref);
	assert(waitq_empty(&c->inq_wq) && waitq_empty(&c->outq_wq));
	assert(mbufq_empty(&c->inq));
	rcu_free(&c->e.rcu, udp_finish_release_conn);
}

static inline void udp_conn_put(udpconn_t *c)
{
	kref_put(&c->ref, udp_release_conn_ref);
}

/**
 * udp_dial - creates a UDP socket between a local and remote address
 * @laddr: the local UDP address
 * @raddr: the remote UDP address
 * @c_out: a pointer to store the UDP socket (if successful)
 *
 * Returns 0 if success, otherwise fail.
 */
int udp_dial(struct netaddr laddr, struct netaddr raddr, udpconn_t **c_out)
{
	udpconn_t *c;
	int ret;

	/* only can support one local IP so far */
	if (laddr.ip == 0)
		laddr.ip = netcfg.addr;
	else if (laddr.ip != netcfg.addr)
		return -EINVAL;

	/* rewrite loopback address */
	if (raddr.ip == MAKE_IP_ADDR(127, 0, 0, 1))
		raddr.ip = netcfg.addr;

	c = smalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;

	udp_init_conn(c);
	trans_init_5tuple(&c->e, IPPROTO_UDP, &udp_conn_ops, laddr, raddr);

	if (laddr.port == 0)
		ret = trans_table_add_with_ephemeral_port(&c->e);
	else
		ret = trans_table_add(&c->e);
	if (ret) {
		sfree(c);
		return ret;
	}

	*c_out = c;
	return 0;
}

/**
 * udp_listen - creates a UDP socket listening to a local address
 * @laddr: the local UDP address
 * @c_out: a pointer to store the UDP socket (if successful)
 *
 * Returns 0 if success, otherwise fail.
 */
int udp_listen(struct netaddr laddr, udpconn_t **c_out)
{
	udpconn_t *c;
	int ret;

	/* only can support one local IP so far */
	if (laddr.ip == 0)
		laddr.ip = netcfg.addr;
	else if (laddr.ip != netcfg.addr)
		return -EINVAL;

	c = smalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;

	udp_init_conn(c);
	trans_init_3tuple(&c->e, IPPROTO_UDP, &udp_conn_ops, laddr);

	if (laddr.port == 0)
		ret = trans_table_add_with_ephemeral_port(&c->e);
	else
		ret = trans_table_add(&c->e);
	if (ret) {
		sfree(c);
		return ret;
	}

	c->flow.kthread_affinity = 0;
	c->flow.e = &c->e;
	c->flow.ref = &c->ref;
	c->flow.release = udp_release_conn_ref;
	register_flow(&c->flow);

	*c_out = c;
	return 0;
}

/**
 * udp_local_addr - gets the local address of the socket
 * @c: the UDP socket
 */
struct netaddr udp_local_addr(udpconn_t *c)
{
	return c->e.laddr;
}

/**
 * udp_remote_addr - gets the remote address of the socket
 * @c: the UDP socket
 *
 * A UDP socket may not have a remote address attached. If so, the IP and
 * port will be set to zero.
 */
struct netaddr udp_remote_addr(udpconn_t *c)
{
	return c->e.raddr;
}

/**
 * udp_set_buffers - changes send and receive buffer sizes
 * @c: the UDP socket
 * @read_mbufs: the maximum number of read mbufs to buffer
 * @write_mbufs: the maximum number of write mbufs to buffer
 *
 * Returns 0 if the inputs were valid.
 */
int udp_set_buffers(udpconn_t *c, int read_mbufs, int write_mbufs)
{
	c->inq_cap = read_mbufs;
	c->outq_cap = write_mbufs;

	/* TODO: free mbufs that go over new limits? */
	return 0;
}

/**
 * udp_read_from - reads from a UDP socket
 * @c: the UDP socket
 * @buf: a buffer to store the datagram
 * @len: the size of @buf
 * @raddr: a pointer to store the remote address of the datagram (if not NULL)
 *
 * WARNING: This a blocking function. It will wait until a datagram is
 * available, an error occurs, or the socket is shutdown.
 *
 * Returns the number of bytes in the datagram, or @len if the datagram
 * is >= @len in size. If the socket has been shutdown, returns 0.
 */
ssize_t udp_read_from(udpconn_t *c, void *buf, size_t len,
                      struct netaddr *raddr)
{
	ssize_t ret;
	struct mbuf *m;

	spin_lock_np(&c->inq_lock);

	/* block until there is an actionable event */
	while (mbufq_empty(&c->inq) && !c->inq_err && !c->shutdown) {
		if (c->nonblocking) {
			spin_unlock_np(&c->inq_lock);
			return -EAGAIN;
		}
		waitq_wait(&c->inq_wq, &c->inq_lock);
	}

	/* is the socket drained and shutdown? */
	if (mbufq_empty(&c->inq) && c->shutdown) {
		spin_unlock_np(&c->inq_lock);
		return 0;
	}

	/* propagate error status code if an error was detected */
	if (c->inq_err) {
		spin_unlock_np(&c->inq_lock);
		return -c->inq_err;
	}

	/* pop an mbuf and deliver the payload */
	m = mbufq_pop_head(&c->inq);
	c->inq_len--;
	spin_unlock_np(&c->inq_lock);

	ret = MIN(len, mbuf_length(m));
	memcpy(buf, mbuf_data(m), ret);
	if (raddr) {
		struct ip_hdr *iphdr = mbuf_network_hdr(m, *iphdr);
		struct udp_hdr *udphdr = mbuf_transport_hdr(m, *udphdr);
		raddr->ip = ntoh32(iphdr->saddr);
		raddr->port = ntoh16(udphdr->src_port);
		if (c->e.match == TRANS_MATCH_5TUPLE) {
			assert(c->e.raddr.ip == raddr->ip &&
			       c->e.raddr.port == raddr->port);
		}
	}
	mbuf_free(m);
	return ret;
}

static void udp_tx_release_mbuf(struct mbuf *m)
{
	udpconn_t *c = (udpconn_t *)m->release_data;
	thread_t *th = NULL;
	bool free_conn;

	spin_lock_np(&c->outq_lock);
	c->outq_len--;
	free_conn = (c->outq_free && c->outq_len == 0);
	if (!c->shutdown)
		th = waitq_signal(&c->outq_wq, &c->outq_lock);
	spin_unlock_np(&c->outq_lock);
	waitq_signal_finish(th);

	net_tx_release_mbuf(m);
	if (free_conn)
		udp_conn_put(c);
}

/**
 * udp_write_to - writes to a UDP socket
 * @c: the UDP socket
 * @buf: a buffer from which to load the payload
 * @len: the length of the payload
 * @raddr: the remote address of the datagram (if not NULL)
 *
 * WARNING: This a blocking function. It will wait until space in the transmit
 * buffer is available or the socket is shutdown.
 *
 * Returns the number of payload bytes sent in the datagram. If an error
 * occurs, returns < 0 to indicate the error code.
 */
ssize_t udp_write_to(udpconn_t *c, const void *buf, size_t len,
                     const struct netaddr *raddr)
{
	struct netaddr addr;
	ssize_t ret;
	struct mbuf *m;
	void *payload;

	if (len > udp_get_payload_size())
		return -EMSGSIZE;
	if (!raddr) {
		if (c->e.match == TRANS_MATCH_3TUPLE)
			return -EDESTADDRREQ;
		addr = c->e.raddr;
	} else {
		addr = *raddr;
		/* rewrite loopback address */
		if (addr.ip == MAKE_IP_ADDR(127, 0, 0, 1))
			addr.ip = netcfg.addr;
	}

	spin_lock_np(&c->outq_lock);

	/* block until there is an actionable event */
	while (c->outq_len >= c->outq_cap && !c->shutdown) {
		if (c->nonblocking) {
			spin_unlock_np(&c->outq_lock);
			return -EAGAIN;
		}
		waitq_wait(&c->outq_wq, &c->outq_lock);
	}

	/* is the socket shutdown? */
	if (c->shutdown) {
		spin_unlock_np(&c->outq_lock);
		return -EPIPE;
	}

	c->outq_len++;
	spin_unlock_np(&c->outq_lock);

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return -ENOBUFS;

	/* write datagram payload */
	payload = mbuf_put(m, len);
	memcpy(payload, buf, len);

	/* override mbuf release method */
	m->release = udp_tx_release_mbuf;
	m->release_data = (unsigned long)c;

	ret = udp_send_raw(m, len, c->e.laddr, addr);
	if (unlikely(ret)) {
		net_tx_release_mbuf(m);
		return ret;
	}

	return len;
}

/**
 * udp_read - reads from a UDP socket
 * @c: the UDP socket
 * @buf: a buffer to store the datagram
 * @len: the size of @buf
 *
 * WARNING: This a blocking function. It will wait until a datagram is
 * available, an error occurs, or the socket is shutdown.
 *
 * Returns the number of bytes in the datagram, or @len if the datagram
 * is >= @len in size. If the socket has been shutdown, returns 0.
 */
ssize_t udp_read(udpconn_t *c, void *buf, size_t len)
{
	return udp_read_from(c, buf, len, NULL);
}

/**
 * udp_write - writes to a UDP socket
 * @c: the UDP socket
 * @buf: the payload to send
 * @len: the length of the payload
 *
 * WARNING: This a blocking function. It will wait until space in the transmit
 * buffer is available or the socket is shutdown.
 *
 * Returns the number of payload bytes sent in the datagram. If an error
 * occurs, returns < 0 to indicate the error code.
 */
ssize_t udp_write(udpconn_t *c, const void *buf, size_t len)
{
	return udp_write_to(c, buf, len, NULL);
}

static void __udp_shutdown(udpconn_t *c)
{
	spin_lock_np(&c->inq_lock);
	spin_lock_np(&c->outq_lock);
	BUG_ON(c->shutdown);
	c->shutdown = true;
	spin_unlock_np(&c->outq_lock);
	spin_unlock_np(&c->inq_lock);

	/* prevent ingress receive and error dispatch (after RCU period) */
	trans_table_remove(&c->e);
}

/**
 * udp_shutdown - disables a UDP socket
 * @c: the socket to disable
 *
 * All blocking requests on the socket will return a failure.
 */
void udp_shutdown(udpconn_t *c)
{
	/* shutdown the UDP socket */
	__udp_shutdown(c);

	/* wake all blocked threads */
	if (!c->inq_err)
		waitq_release(&c->inq_wq);
	waitq_release(&c->outq_wq);
}

/**
 * udp_close - frees a UDP socket
 * @c: the socket to free
 *
 * WARNING: Only the last reference can safely call this method. Call
 * udp_shutdown() first if any threads are sleeping on the socket.
 */
void udp_close(udpconn_t *c)
{
	bool free_conn;

	if (!c->shutdown)
		__udp_shutdown(c);

	BUG_ON(!waitq_empty(&c->inq_wq));
	BUG_ON(!waitq_empty(&c->outq_wq));

	/* free all in-flight mbufs */
	while (true) {
		struct mbuf *m = mbufq_pop_head(&c->inq);
		if (!m)
			break;
		mbuf_free(m);
	}

	spin_lock_np(&c->outq_lock);
	free_conn = c->outq_len == 0;
	c->outq_free = true;
	spin_unlock_np(&c->outq_lock);

	if (c->e.match == TRANS_MATCH_3TUPLE)
		deregister_flow(&c->flow);

	if (free_conn)
		udp_conn_put(c);
}

void udp_set_nonblocking(udpconn_t *c, bool nonblocking)
{
	spin_lock_np(&c->outq_lock);
	spin_lock(&c->inq_lock);
	c->nonblocking = nonblocking;
	if (nonblocking) {
		waitq_release(&c->inq_wq);
		waitq_release(&c->outq_wq);
	}
	spin_unlock(&c->inq_lock);
	spin_unlock_np(&c->outq_lock);
}


/*
 * Parallel API
 */

struct udpspawner {
	struct trans_entry	e;
	udpspawn_fn_t		fn;

	struct kref ref;
	struct flow_registration flow;
};

/* handles ingress packets with parallel threads */
static void udp_par_recv(struct trans_entry *e, struct mbuf *m)
{
	udpspawner_t *s = container_of(e, udpspawner_t, e);
	const struct ip_hdr *iphdr;
	const struct udp_hdr *udphdr;
	struct udp_spawn_data *d;
	thread_t *th;

	iphdr = mbuf_network_hdr(m, *iphdr);
	udphdr = mbuf_pull_hdr_or_null(m, *udphdr);
	if (unlikely(!udphdr)) {
		mbuf_free(m);
		return;
	}

	th = thread_create_with_buf((thread_fn_t)s->fn,
				    (void **)&d, sizeof(*d));
	if (unlikely(!th)) {
		mbuf_drop(m);
		return;
	}

	d->buf = mbuf_data(m);
	d->len = mbuf_length(m);
	d->laddr = e->laddr;
	d->raddr.ip = ntoh32(iphdr->saddr);
	d->raddr.port = ntoh16(udphdr->src_port);
	d->release_data = m;
	thread_ready(th);
}

/* operations for UDP spawners */
static const struct trans_ops udp_par_ops = {
	.recv = udp_par_recv,
};

static void udp_release_spawner(struct rcu_head *h)
{
	udpspawner_t *s = container_of(h, udpspawner_t, e.rcu);
	sfree(s);
}

static void udp_release_spawner_ref(struct kref *ref)
{
	udpspawner_t *s = container_of(ref, udpspawner_t, ref);
	rcu_free(&s->e.rcu, udp_release_spawner);
}


/**
 * udp_create_spawner - creates a UDP spawner for ingress datagrams
 * @laddr: the local address to bind to
 * @fn: a handler function for each datagram
 * @s_out: if successful, set to a pointer to the spawner
 *
 * Returns 0 if successful, otherwise fail.
 */
int udp_create_spawner(struct netaddr laddr, udpspawn_fn_t fn,
		       udpspawner_t **s_out)
{
	udpspawner_t *s;
	int ret;

	/* only can support one local IP so far */
	if (laddr.ip == 0)
		laddr.ip = netcfg.addr;
	else if (laddr.ip != netcfg.addr)
		return -EINVAL;

	s = smalloc(sizeof(*s));
	if (!s)
		return -ENOMEM;

	kref_init(&s->ref);
	trans_init_3tuple(&s->e, IPPROTO_UDP, &udp_par_ops, laddr);
	s->fn = fn;
	ret = trans_table_add(&s->e);
	if (ret) {
		sfree(s);
		return ret;
	}

	s->flow.kthread_affinity = 0;
	s->flow.e = &s->e;
	s->flow.ref = &s->ref;
	s->flow.release = udp_release_spawner_ref;
	register_flow(&s->flow);

	*s_out = s;
	return 0;
}

/**
 * udp_destroy_spawner - unregisters and frees a UDP spawner
 * @s: the spawner to free
 */
void udp_destroy_spawner(udpspawner_t *s)
{
	trans_table_remove(&s->e);
	deregister_flow(&s->flow);
	kref_put(&s->ref, udp_release_spawner_ref);
}

/**
 * udp_send - sends a UDP datagram
 * @buf: the payload to send
 * @len: the length of the payload
 * @laddr: the local UDP address
 * @raddr: the remote UDP address
 *
 * Returns the number of payload bytes sent in the datagram. If an error
 * occurs, returns < 0 to indicate the error code.
 */
ssize_t udp_send(const void *buf, size_t len,
		 struct netaddr laddr, struct netaddr raddr)
{
	void *payload;
	struct mbuf *m;
	int ret;

	if (len > udp_get_payload_size())
		return -EMSGSIZE;
	if (laddr.ip == 0)
		laddr.ip = netcfg.addr;
	else if (laddr.ip != netcfg.addr)
		return -EINVAL;
	if (laddr.port == 0)
		return -EINVAL;

	/* rewrite loopback address */
	if (raddr.ip == MAKE_IP_ADDR(127, 0, 0, 1))
		raddr.ip = netcfg.addr;

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return -ENOBUFS;

	/* write datagram payload */
	payload = mbuf_put(m, len);
	memcpy(payload, buf, len);

	ret = udp_send_raw(m, len, laddr, raddr);
	if (unlikely(ret)) {
		mbuf_free(m);
		return ret;
	}

	return len;
}

ssize_t udp_sendv(const struct iovec *iov, int iovcnt,
		  struct netaddr laddr, struct netaddr raddr)
{
	struct mbuf *m;
	int i, ret;
	ssize_t len = 0;

	if (laddr.ip == 0)
		laddr.ip = netcfg.addr;
	else if (laddr.ip != netcfg.addr)
		return -EINVAL;
	if (laddr.port == 0)
		return -EINVAL;

	/* rewrite loopback address */
	if (raddr.ip == MAKE_IP_ADDR(127, 0, 0, 1))
		raddr.ip = netcfg.addr;

	m = net_tx_alloc_mbuf();
	if (unlikely(!m))
		return -ENOBUFS;

	/* write datagram payload */
	for (i = 0; i < iovcnt; i++) {
		len += iov[i].iov_len;
		if (unlikely(len > udp_get_payload_size())) {
			mbuf_free(m);
			return -EMSGSIZE;
		}
		memcpy(mbuf_put(m, iov[i].iov_len),
		       iov[i].iov_base, iov[i].iov_len);
	}

	ret = udp_send_raw(m, len, laddr, raddr);
	if (unlikely(ret)) {
		mbuf_free(m);
		return ret;
	}

	return len;
}

/**
 * udp_spawn_data_release - frees the datagram buffer for a spawner thread
 * @release_data: the release data pointer
 *
 * Must be called when finished with the buffer passed to the spawner thread.
 */
void udp_spawn_data_release(void *release_data)
{
	struct mbuf *m = release_data;
	mbuf_free(m);
}

/**
 * udp_init - initializes the UDP stack
 *
 * Returns 0 if successful.
 */
int udp_init(void)
{
	/* calculate MTU-limited UDP payload size */
	udp_payload_size = net_get_mtu();
	udp_payload_size -= sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
	return 0;
}
