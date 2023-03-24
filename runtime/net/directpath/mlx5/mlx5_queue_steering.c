#ifdef DIRECTPATH

#include "mlx5.h"

static uint8_t queue_assignments[NCPU];
BUILD_ASSERT(NCPU - 1 <= UINT8_MAX);

static inline void assign_q(unsigned int qidx, unsigned int kidx)
{
	if (queue_assignments[qidx] == kidx)
		return;

	rcu_hlist_del(&rxqs[qidx].link);
	rcu_hlist_add_head(&rxqs[kidx].head, &rxqs[qidx].link);
	queue_assignments[qidx] = kidx;
}

static int mlx5_qs_steer(unsigned int *new_fg_assignment)
{
	int i;
	for (i = 0; i < maxks; i++)
		assign_q(i, new_fg_assignment[i]);

	return 0;
}

static bool mlx5_qs_rx_poll(unsigned int q_index)
{
	bool work_done;
	struct mlx5_rxq *mrxq, *hrxq;
	struct rcu_hlist_node *node;

	/* if work stealing, just poll the single queue */
	if (q_index != myk()->kthread_idx)
		return mlx5_rx_poll_locked(q_index);

	work_done = false;
	hrxq = &rxqs[q_index];

	rcu_hlist_for_each(&hrxq->head, node, true) {
		prefetch(container_of(node->next, struct mlx5_rxq, link));
		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		work_done |= mlx5_rx_poll(mrxq - rxqs);
	}

	return work_done;
}

static bool mlx5_qs_rx_poll_locked(unsigned int q_index)
{
	bool work_done;
	struct mlx5_rxq *mrxq, *hrxq;
	struct rcu_hlist_node *node;

	/* if work stealing, just poll the single queue */
	if (q_index != myk()->kthread_idx)
		return mlx5_rx_poll_locked(q_index);

	work_done = false;
	hrxq = &rxqs[q_index];

	rcu_hlist_for_each(&hrxq->head, node, true) {
		prefetch(container_of(node->next, struct mlx5_rxq, link));
		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		work_done |= mlx5_rx_poll_locked(mrxq - rxqs);
	}

	return work_done;
}

int mlx5_init_queue_steering(void)
{
	int i;

	net_ops.rx_poll = mlx5_qs_rx_poll;
	net_ops.rx_poll_locked = mlx5_qs_rx_poll_locked;
	net_ops.steer_flows = mlx5_qs_steer;

	for (i = 0; i < maxks; i++) {
		rcu_hlist_init_head(&rxqs[i].head);
		rcu_hlist_add_head(&rxqs[0].head, &rxqs[i].link);
	}

	return 0;
}

#endif
