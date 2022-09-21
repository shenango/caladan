#ifdef DIRECTPATH

#include "mlx5.h"

static unsigned int queue_assignments[NCPU];

static inline void assign_q(unsigned int qidx, unsigned int kidx)
{
	if (queue_assignments[qidx] == kidx)
		return;

	rcu_hlist_del(&rxqs[qidx].link);
	rcu_hlist_add_head(&rxqs[kidx].head, &rxqs[qidx].link);
	queue_assignments[qidx]  = kidx;

	ACCESS_ONCE(runtime_info->flow_tbl[qidx]) = kidx;
}

static int mlx5_qs_steer(unsigned int *new_fg_assignment)
{
	int i;
	for (i = 0; i < maxks; i++)
		assign_q(i, new_fg_assignment[i]);

	return 0;
}

static int mlx5_qs_gather_rx(struct direct_rxq *rxq, struct mbuf **ms,
                             unsigned int budget)
{
	struct mlx5_rxq *mrxq, *hrxq = container_of(rxq, struct mlx5_rxq, rxq);
	struct rcu_hlist_node *node;

	unsigned int pulled = 0;

	preempt_disable();

	node = hrxq->last_node;
	/* start at the beginning if the last pass pulled from all queues */
	if (!hrxq->last_node) {
		node = hrxq->head.head;
	} else {
		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		/* queue assignment changed, start at the beginning */
		if (ACCESS_ONCE(queue_assignments[mrxq - rxqs]) != hrxq - rxqs) {
			node = hrxq->head.head;
			hrxq->last_node = NULL;
		}
	}

	// TODO: maybe don't walk the list twice

	while (!preempt_cede_needed(myk())) {

		if (!node) {
			/* if we started in the middle, go back to the beginning */
			if (hrxq->last_node) {
				node = hrxq->head.head;
				hrxq->last_node = NULL;
				continue;
			}
			break;
		}

		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);

		if (mlx5_rxq_pending(mrxq) && spin_try_lock(&mrxq->lock)) {
			pulled += mlx5_gather_rx(&mrxq->rxq, ms + pulled, budget - pulled);
			spin_unlock(&mrxq->lock);
			if (pulled == budget)
				break;
		}

		node = rcu_dereference_protected(node->next, !preempt_enabled());
	}

	/* stash the last node we polled so we can start here next time */
	hrxq->last_node = node;
	preempt_enable();
	return pulled;
}

static int mlx5_qs_have_work(struct direct_rxq *rxq)
{
	struct mlx5_rxq *mrxq, *hrxq = container_of(rxq, struct mlx5_rxq, rxq);
	struct rcu_hlist_node *node;

	rcu_hlist_for_each(&hrxq->head, node, !preempt_enabled()) {
		mrxq = rcu_hlist_entry(node, struct mlx5_rxq, link);
		if (mlx5_rxq_pending(mrxq))
			return true;
	}

	return false;
}

int mlx5_init_queue_steering(void)
{
	int i;

	net_ops.rx_batch = mlx5_qs_gather_rx;
	net_ops.rxq_has_work = mlx5_qs_have_work;
	net_ops.steer_flows = mlx5_qs_steer;

	for (i = 0; i < maxks; i++) {
		rcu_hlist_init_head(&rxqs[i].head);
		spin_lock_init(&rxqs[i].lock);
		rcu_hlist_add_head(&rxqs[0].head, &rxqs[i].link);
	}

	return 0;
}

#endif
