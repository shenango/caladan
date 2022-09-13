/*
 * netlink.c - methods for registering mac addresses in device bridge table
 * using netlink/linux
 *
 * This file is largely copied from DPDK.
 */

#ifdef MLX5

#include <base/log.h>
#include <base/stddef.h>
#include <net/ethernet.h>

#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <rdma/rdma_netlink.h>
#include <string.h>
#include <unistd.h>

#include "defs.h"
#include "hw_timestamp.h"

static int nl_route_fd = -1;
static unsigned int iface_idx;

#define rte_ether_addr eth_addr
#define RTE_ETHER_ADDR_LEN ETH_ADDR_LEN

/* Size of the buffer to receive kernel messages */
#define MLX5_NL_BUF_SIZE (32 * 1024)
/* Send buffer size for the Netlink socket */
#define MLX5_SEND_BUF_SIZE 32768
/* Receive buffer size for the Netlink socket */
#define MLX5_RECV_BUF_SIZE 32768

#define MLX5_NL_CMD_GET_IB_NAME (1 << 0)
#define MLX5_NL_CMD_GET_IB_INDEX (1 << 1)
#define MLX5_NL_CMD_GET_NET_INDEX (1 << 2)
#define MLX5_NL_CMD_GET_PORT_INDEX (1 << 3)

static int nlerrno;
#define rte_errno nlerrno

/** Data structure used by mlx5_nl_cmdget_cb(). */
struct mlx5_nl_ifindex_data {
	const char *name; /**< IB device name (in). */
	uint32_t flags; /**< found attribute flags (out). */
	uint32_t ibindex; /**< IB device index (out). */
	uint32_t ifindex; /**< Network interface index (out). */
	uint32_t portnum; /**< IB device max port number (out). */
};

static uint32_t atomic_sn;
#define MLX5_NL_SN_GENERATE __atomic_add_fetch(&atomic_sn, 1, __ATOMIC_RELAXED)

/**
 * Process network interface information from Netlink message.
 *
 * @param nh
 *   Pointer to Netlink message header.
 * @param arg
 *   Opaque data pointer for this callback.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_nl_cmdget_cb(struct nlmsghdr *nh, void *arg)
{
	struct mlx5_nl_ifindex_data *data = arg;
	struct mlx5_nl_ifindex_data local = {
		.flags = 0,
	};
	size_t off = NLMSG_HDRLEN;

	if (nh->nlmsg_type !=
	    RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET) &&
	    nh->nlmsg_type !=
	    RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_PORT_GET))
		goto error;
	while (off < nh->nlmsg_len) {
		struct nlattr *na = (void *)((uintptr_t)nh + off);
		void *payload = (void *)((uintptr_t)na + NLA_HDRLEN);

		if (na->nla_len > nh->nlmsg_len - off)
			goto error;
		switch (na->nla_type) {
		case RDMA_NLDEV_ATTR_DEV_INDEX:
			local.ibindex = *(uint32_t *)payload;
			local.flags |= MLX5_NL_CMD_GET_IB_INDEX;
			break;
		case RDMA_NLDEV_ATTR_DEV_NAME:
			if (!strcmp(payload, data->name))
				local.flags |= MLX5_NL_CMD_GET_IB_NAME;
			break;
		case RDMA_NLDEV_ATTR_NDEV_INDEX:
			local.ifindex = *(uint32_t *)payload;
			local.flags |= MLX5_NL_CMD_GET_NET_INDEX;
			break;
		case RDMA_NLDEV_ATTR_PORT_INDEX:
			local.portnum = *(uint32_t *)payload;
			local.flags |= MLX5_NL_CMD_GET_PORT_INDEX;
			break;
		default:
			break;
		}
		off += NLA_ALIGN(na->nla_len);
	}
	/*
	 * It is possible to have multiple messages for all
	 * Infiniband devices in the system with appropriate name.
	 * So we should gather parameters locally and copy to
	 * query context only in case of coinciding device name.
	 */
	if (local.flags & MLX5_NL_CMD_GET_IB_NAME) {
		data->flags = local.flags;
		data->ibindex = local.ibindex;
		data->ifindex = local.ifindex;
		data->portnum = local.portnum;
	}
	return 0;
error:
	rte_errno = EINVAL;
	return -rte_errno;
}

/**
 * Send a message to the kernel on the Netlink socket.
 *
 * @param[in] nlsk_fd
 *   The Netlink socket file descriptor used for communication.
 * @param[in] nh
 *   The Netlink message send to the kernel.
 * @param[in] sn
 *   Sequence number.
 *
 * @return
 *   The number of sent bytes on success, a negative errno value otherwise and
 *   rte_errno is set.
 */
static int
mlx5_nl_send(int nlsk_fd, struct nlmsghdr *nh, uint32_t sn)
{
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
	};
	struct iovec iov = {
		.iov_base = nh,
		.iov_len = nh->nlmsg_len,
	};
	struct msghdr msg = {
		.msg_name = &sa,
		.msg_namelen = sizeof(sa),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	int send_bytes;

	nh->nlmsg_pid = 0; /* communication with the kernel uses pid 0 */
	nh->nlmsg_seq = sn;
	send_bytes = sendmsg(nlsk_fd, &msg, 0);
	if (send_bytes < 0) {
		rte_errno = errno;
		return -rte_errno;
	}
	return send_bytes;
}

/**
 * Receive a message from the kernel on the Netlink socket, following
 * mlx5_nl_send().
 *
 * @param[in] nlsk_fd
 *   The Netlink socket file descriptor used for communication.
 * @param[in] sn
 *   Sequence number.
 * @param[in] cb
 *   The callback function to call for each Netlink message received.
 * @param[in, out] arg
 *   Custom arguments for the callback.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_nl_recv(int nlsk_fd, uint32_t sn, int (*cb)(struct nlmsghdr *, void *arg),
	     void *arg)
{
	struct sockaddr_nl sa;
	char buf[MLX5_RECV_BUF_SIZE];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = MLX5_RECV_BUF_SIZE,
	};
	struct msghdr msg = {
		.msg_name = &sa,
		.msg_namelen = sizeof(sa),
		.msg_iov = &iov,
		/* One message at a time */
		.msg_iovlen = 1,
	};
	int multipart = 0;
	int ret = 0;

	do {
		struct nlmsghdr *nh;
		int recv_bytes = 0;

		do {
			recv_bytes = recvmsg(nlsk_fd, &msg, 0);
			if (recv_bytes == -1) {
				rte_errno = errno;
				ret = -rte_errno;
				goto exit;
			}
			nh = (struct nlmsghdr *)buf;
		} while (nh->nlmsg_seq != sn);
		for (;
		     NLMSG_OK(nh, (unsigned int)recv_bytes);
		     nh = NLMSG_NEXT(nh, recv_bytes)) {
			if (nh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err_data = NLMSG_DATA(nh);

				if (err_data->error < 0) {
					rte_errno = -err_data->error;
					ret = -rte_errno;
					goto exit;
				}
				/* Ack message. */
				ret = 0;
				goto exit;
			}
			/* Multi-part msgs and their trailing DONE message. */
			if (nh->nlmsg_flags & NLM_F_MULTI) {
				if (nh->nlmsg_type == NLMSG_DONE) {
					ret =  0;
					goto exit;
				}
				multipart = 1;
			}
			if (cb) {
				ret = cb(nh, arg);
				if (ret < 0)
					goto exit;
			}
		}
	} while (multipart);
exit:
	return ret;
}

static unsigned int
mlx5_nl_ifindex(int nl, const char *name, uint32_t pindex)
{
	struct mlx5_nl_ifindex_data data = {
		.name = name,
		.flags = 0,
		.ibindex = 0, /* Determined during first pass. */
		.ifindex = 0, /* Determined during second pass. */
	};
	union {
		struct nlmsghdr nh;
		uint8_t buf[NLMSG_HDRLEN +
			    NLA_HDRLEN + NLA_ALIGN(sizeof(data.ibindex)) +
			    NLA_HDRLEN + NLA_ALIGN(sizeof(pindex))];
	} req = {
		.nh = {
			.nlmsg_len = NLMSG_LENGTH(0),
			.nlmsg_type = RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
						       RDMA_NLDEV_CMD_GET),
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP,
		},
	};
	struct nlattr *na;
	uint32_t sn = MLX5_NL_SN_GENERATE;
	int ret;

	ret = mlx5_nl_send(nl, &req.nh, sn);
	if (ret < 0)
		return 0;
	ret = mlx5_nl_recv(nl, sn, mlx5_nl_cmdget_cb, &data);
	if (ret < 0)
		return 0;
	if (!(data.flags & MLX5_NL_CMD_GET_IB_NAME) ||
	    !(data.flags & MLX5_NL_CMD_GET_IB_INDEX))
		goto error;
	data.flags = 0;
	sn = MLX5_NL_SN_GENERATE;
	req.nh.nlmsg_type = RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					     RDMA_NLDEV_CMD_PORT_GET);
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.buf) - NLMSG_HDRLEN);
	na = (void *)((uintptr_t)req.buf + NLMSG_HDRLEN);
	na->nla_len = NLA_HDRLEN + sizeof(data.ibindex);
	na->nla_type = RDMA_NLDEV_ATTR_DEV_INDEX;
	memcpy((void *)((uintptr_t)na + NLA_HDRLEN),
	       &data.ibindex, sizeof(data.ibindex));
	na = (void *)((uintptr_t)na + NLA_ALIGN(na->nla_len));
	na->nla_len = NLA_HDRLEN + sizeof(pindex);
	na->nla_type = RDMA_NLDEV_ATTR_PORT_INDEX;
	memcpy((void *)((uintptr_t)na + NLA_HDRLEN),
	       &pindex, sizeof(pindex));
	ret = mlx5_nl_send(nl, &req.nh, sn);
	if (ret < 0)
		return 0;
	ret = mlx5_nl_recv(nl, sn, mlx5_nl_cmdget_cb, &data);
	if (ret < 0)
		return 0;
	if (!(data.flags & MLX5_NL_CMD_GET_IB_NAME) ||
	    !(data.flags & MLX5_NL_CMD_GET_IB_INDEX) ||
	    !(data.flags & MLX5_NL_CMD_GET_NET_INDEX) ||
	    !data.ifindex)
		goto error;
	return data.ifindex;
error:
	rte_errno = ENODEV;
	return 0;
}


/**
 * Opens a Netlink socket.
 *
 * @param protocol
 *   Netlink protocol (e.g. NETLINK_ROUTE, NETLINK_RDMA).
 *
 * @return
 *   A file descriptor on success, a negative errno value otherwise and
 *   rte_errno is set.
 */
static int
mlx5_nl_init(int protocol)
{
	int fd;
	int sndbuf_size = MLX5_SEND_BUF_SIZE;
	int rcvbuf_size = MLX5_RECV_BUF_SIZE;
	struct sockaddr_nl local = {
		.nl_family = AF_NETLINK,
	};
	int ret;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
	if (fd == -1) {
		rte_errno = errno;
		return -rte_errno;
	}
	ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(int));
	if (ret == -1) {
		rte_errno = errno;
		goto error;
	}
	ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(int));
	if (ret == -1) {
		rte_errno = errno;
		goto error;
	}
	ret = bind(fd, (struct sockaddr *)&local, sizeof(local));
	if (ret == -1) {
		rte_errno = errno;
		goto error;
	}
	return fd;
error:
	close(fd);
	return -rte_errno;
}


/**
 * Modify the MAC address neighbour table with Netlink.
 *
 * @param[in] nlsk_fd
 *   Netlink socket file descriptor.
 * @param[in] iface_idx
 *   Net device interface index.
 * @param mac
 *   MAC address to consider.
 * @param add
 *   1 to add the MAC address, 0 to remove the MAC address.
 *
 * @return
 *   0 on success, a negative errno value otherwise and rte_errno is set.
 */
static int
mlx5_nl_mac_addr_modify(int nlsk_fd, unsigned int iface_idx,
			struct rte_ether_addr *mac, int add)
{
	struct {
		struct nlmsghdr hdr;
		struct ndmsg ndm;
		struct rtattr rta;
		uint8_t buffer[RTE_ETHER_ADDR_LEN];
	} req = {
		.hdr = {
			.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg)),
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE |
				NLM_F_EXCL | NLM_F_ACK,
			.nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH,
		},
		.ndm = {
			.ndm_family = PF_BRIDGE,
			.ndm_state = NUD_NOARP | NUD_PERMANENT,
			.ndm_ifindex = iface_idx,
			.ndm_flags = NTF_SELF,
		},
		.rta = {
			.rta_type = NDA_LLADDR,
			.rta_len = RTA_LENGTH(RTE_ETHER_ADDR_LEN),
		},
	};
	uint32_t sn = MLX5_NL_SN_GENERATE;
	int ret;

	if (nlsk_fd == -1)
		return -ENODEV;
	memcpy(RTA_DATA(&req.rta), mac, RTE_ETHER_ADDR_LEN);
	req.hdr.nlmsg_len = NLMSG_ALIGN(req.hdr.nlmsg_len) +
		RTA_ALIGN(req.rta.rta_len);
	ret = mlx5_nl_send(nlsk_fd, &req.hdr, sn);
	if (ret < 0)
		goto error;
	ret = mlx5_nl_recv(nlsk_fd, sn, NULL, NULL);
	if (ret < 0)
		goto error;
	return 0;
error:
	return -rte_errno;
}

int nl_init(void)
{
	int nl_rdma_fd;

	nl_rdma_fd = mlx5_nl_init(NETLINK_RDMA);
	if (nl_rdma_fd < 0)
		return nl_rdma_fd;

	iface_idx =  mlx5_nl_ifindex(nl_rdma_fd, device_name, 1); // TODO dynamic port number
	if (iface_idx == 0)
		return rte_errno ? rte_errno : -1;

	close(nl_rdma_fd);

	nl_route_fd = mlx5_nl_init(NETLINK_ROUTE);
	if (nl_route_fd < 0)
		return errno ? -errno : -1;

	return 0;
}

int nl_remove_mac_address(struct eth_addr *mac)
{
	if (cfg.no_hw_qdel)
		return 0;

	return mlx5_nl_mac_addr_modify(nl_route_fd, iface_idx, mac, 0);
}

int nl_register_mac_address(struct eth_addr *mac)
{
	int ret;

	/* functionality is currently associated with hw_timestamp */
	if (cfg.no_hw_qdel)
		return 0;

	ret = mlx5_nl_mac_addr_modify(nl_route_fd, iface_idx, mac, 1);
	/* not a problem if mac is already registered */
	if (ret == -EEXIST)
		return 0;

	return ret;
}

#endif  // MLX5
