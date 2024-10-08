
#include <base/fd_transfer.h>
#include <base/log.h>
#include <base/stddef.h>

#include <sys/socket.h>

int recv_fd(int fd, int *fd_out)
{
	struct msghdr msg;
	char buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov[1];
	char iobuf[1];
	ssize_t ret;
	struct cmsghdr *cmptr;

	/* init message header and buffs for control message and iovec */
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov[0].iov_base = iobuf;
	iov[0].iov_len = sizeof(iobuf);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	ret = recvmsg(fd, &msg, 0);
	if (ret < 0) {
		log_debug("control: error with recvmsg %ld", ret);
		return ret;
	}

	/* check validity of control message */
	cmptr = CMSG_FIRSTHDR(&msg);
	if (cmptr == NULL) {
		log_debug("control: no cmsg %p", cmptr);
		return -1;
	} else if (cmptr->cmsg_len != CMSG_LEN(sizeof(int))) {
		log_debug("control: cmsg is too long %ld", cmptr->cmsg_len);
		return -1;
	} else if (cmptr->cmsg_level != SOL_SOCKET) {
		log_debug("control: unrecognized cmsg level %d", cmptr->cmsg_level);
		return -1;
	} else if (cmptr->cmsg_type != SCM_RIGHTS) {
		log_debug("control: unrecognized cmsg type %d", cmptr->cmsg_type);
		return -1;
	}

	*fd_out = *(int *)CMSG_DATA(cmptr);
	return 0;
}

int send_fd(int controlfd, int shared_fd)
{
	struct msghdr msg;
	char buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov[1];
	char iobuf[1];
	struct cmsghdr *cmptr;

	/* init message header, iovec is necessary even though it's unused */
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);
	iov[0].iov_base = iobuf;
	iov[0].iov_len = sizeof(iobuf);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	/* init control message */
	cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len = CMSG_LEN(sizeof(int));
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	*(int *)CMSG_DATA(cmptr) = shared_fd;

	if (sendmsg(controlfd, &msg, 0) != sizeof(iobuf)) {
		log_err("failed to send cmsg");
		return -1;
	}

	return 0;
}