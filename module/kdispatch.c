// SPDX-License-Identifier: GPL-2.0
/*
 * kdispatch -- work-conserving in-kernel message dispatch for TCP RPCs.
 *
 * The point of this module is one queue.
 *
 * A userspace RPC server that reassembles a TCP byte stream binds a worker to a
 * connection: whoever reassembles a message is whoever runs it, so a worker busy
 * with a slow message cannot serve a fast one waiting on another connection in
 * its shard. Linux's KCM moves parsing into the kernel but keeps a per-connection
 * reservation, so it inherits the same coupling.
 *
 * Here, strparser recovers message boundaries in the TCP receive path and every
 * completed message -- from every connection -- lands on a single shared queue.
 * Workers read() the device and take whatever is at the head. No worker owns a
 * connection, so a message is delayed only if every worker is genuinely busy.
 *
 * Replies are not our business: the message carries its own connection id and
 * userspace writes the response on the originating socket directly.
 */

#define pr_fmt(fmt) "kdispatch: " fmt

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include <net/sock.h>
#include <net/strparser.h>
#include <net/tcp.h>

#include "kdispatch_uapi.h"

/* Backpressure bounds on the shared queue, in messages. */
static unsigned int queue_high = 4096;
static unsigned int queue_low = 2048;
module_param(queue_high, uint, 0644);
MODULE_PARM_DESC(queue_high, "pause parsers when this many messages are queued");
module_param(queue_low, uint, 0644);
MODULE_PARM_DESC(queue_low, "resume parsers once the queue drains to this");

struct kd_dev;

/* One attached transport socket. */
struct kd_conn {
	struct strparser strp;
	struct socket *sock;
	struct sock *sk;
	struct kd_dev *dev;
	struct list_head list;
	bool paused;

	void (*save_data_ready)(struct sock *sk);
	void (*save_write_space)(struct sock *sk);
	void (*save_state_change)(struct sock *sk);
};

struct kd_dev {
	spinlock_t lock;            /* guards queue, conns and all counters */
	struct sk_buff_head queue;  /* THE shared queue */
	wait_queue_head_t wq;
	struct list_head conns;
	struct mutex attach_lock;   /* serialises attach/teardown */

	u32 qlen, qmax, nconns, pauses, aborts;
	u64 msgs, bytes;
	int opens;
	bool draining;
};

static struct kd_dev kd;

/* ---------------------------------------------------------------- parser */

/*
 * Total message length lives in the first four bytes, big-endian. strparser
 * reads the return value as: >0 complete message length, 0 need more bytes,
 * <0 protocol error (which tears the connection down).
 */
static int kd_parse_msg(struct strparser *strp, struct sk_buff *skb)
{
	__be32 hdr;
	u32 len;

	if (skb->len < sizeof(hdr))
		return 0;
	if (skb_copy_bits(skb, 0, &hdr, sizeof(hdr)) < 0)
		return 0;

	len = be32_to_cpu(hdr);
	if (len < KD_MIN_MSG || len > KD_MAX_MSG) {
		pr_warn_ratelimited("bad message length %u\n", len);
		return -EPROTO;
	}
	return len;
}

/* Called with the transport socket's lock held. */
static void kd_rcv_msg(struct strparser *strp, struct sk_buff *skb)
{
	struct kd_conn *conn = container_of(strp, struct kd_conn, strp);
	struct kd_dev *dev = conn->dev;
	unsigned long flags;
	bool pause = false;

	spin_lock_irqsave(&dev->lock, flags);
	__skb_queue_tail(&dev->queue, skb);
	dev->qlen++;
	dev->msgs++;
	dev->bytes += strp_msg(skb)->full_len;
	if (dev->qlen > dev->qmax)
		dev->qmax = dev->qlen;

	/* Stop reading this transport until readers catch up. Bounded memory
	 * matters more than throughput on an overloaded server.
	 */
	if (dev->qlen >= queue_high && !conn->paused) {
		conn->paused = true;
		dev->pauses++;
		pause = true;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (pause)
		strp_pause(strp);

	wake_up_interruptible(&dev->wq);
}

static void kd_abort_parser(struct strparser *strp, int err)
{
	struct kd_conn *conn = container_of(strp, struct kd_conn, strp);
	unsigned long flags;

	spin_lock_irqsave(&conn->dev->lock, flags);
	conn->dev->aborts++;
	spin_unlock_irqrestore(&conn->dev->lock, flags);
	pr_warn_ratelimited("parser aborted: %d\n", err);
}

static const struct strp_callbacks kd_cb = {
	.parse_msg = kd_parse_msg,
	.rcv_msg = kd_rcv_msg,
	.abort_parser = kd_abort_parser,
};

/* ------------------------------------------------------- socket callbacks */

static void kd_data_ready(struct sock *sk)
{
	struct kd_conn *conn;

	read_lock_bh(&sk->sk_callback_lock);
	conn = sk->sk_user_data;
	if (conn)
		strp_data_ready(&conn->strp);
	read_unlock_bh(&sk->sk_callback_lock);
}

static void kd_state_change(struct sock *sk)
{
	struct kd_conn *conn;

	read_lock_bh(&sk->sk_callback_lock);
	conn = sk->sk_user_data;
	if (conn && (sk->sk_state == TCP_CLOSE || sk->sk_state == TCP_CLOSE_WAIT))
		strp_stop(&conn->strp);
	if (conn && conn->save_state_change)
		conn->save_state_change(sk);
	read_unlock_bh(&sk->sk_callback_lock);
}

/* -------------------------------------------------------- attach / detach */

static int kd_attach(struct kd_dev *dev, int fd)
{
	struct socket *sock;
	struct kd_conn *conn;
	struct sock *sk;
	unsigned long flags;
	int err;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return err;

	sk = sock->sk;
	if (sk->sk_type != SOCK_STREAM || sk->sk_protocol != IPPROTO_TCP) {
		err = -EOPNOTSUPP;
		goto err_put;
	}

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn) {
		err = -ENOMEM;
		goto err_put;
	}
	conn->sock = sock;
	conn->sk = sk;
	conn->dev = dev;

	err = strp_init(&conn->strp, sk, &kd_cb);
	if (err)
		goto err_free;

	write_lock_bh(&sk->sk_callback_lock);
	if (sk->sk_user_data) {          /* KCM or another kdispatch already owns it */
		write_unlock_bh(&sk->sk_callback_lock);
		err = -EBUSY;
		goto err_strp;
	}
	conn->save_data_ready = sk->sk_data_ready;
	conn->save_write_space = sk->sk_write_space;
	conn->save_state_change = sk->sk_state_change;
	sk->sk_user_data = conn;
	sk->sk_data_ready = kd_data_ready;
	sk->sk_state_change = kd_state_change;
	write_unlock_bh(&sk->sk_callback_lock);

	spin_lock_irqsave(&dev->lock, flags);
	list_add(&conn->list, &dev->conns);
	dev->nconns++;
	spin_unlock_irqrestore(&dev->lock, flags);

	/* Bytes may already be sitting in the receive queue. */
	strp_check_rcv(&conn->strp);
	return 0;

err_strp:
	strp_done(&conn->strp);
err_free:
	kfree(conn);
err_put:
	sockfd_put(sock);
	return err;
}

/* Mirrors KCM's unattach ordering: stop the parser under the socket lock, then
 * finish teardown outside it.
 */
static void kd_detach(struct kd_conn *conn)
{
	struct sock *sk = conn->sk;

	lock_sock(sk);
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = NULL;
	sk->sk_data_ready = conn->save_data_ready;
	sk->sk_write_space = conn->save_write_space;
	sk->sk_state_change = conn->save_state_change;
	write_unlock_bh(&sk->sk_callback_lock);
	strp_stop(&conn->strp);
	release_sock(sk);

	strp_done(&conn->strp);
	sockfd_put(conn->sock);
	kfree(conn);
}

/* ------------------------------------------------------------ file ops */

/*
 * strp_unpause() is documented as safe without the socket lock, but nothing
 * promises it is safe in atomic context -- so collect the parsers to resume
 * under the queue lock and call it after dropping the lock. Connections are
 * only freed on last close, when no reader is running, so the pointers stay
 * valid across the unlock.
 *
 * Resuming at most KD_UNPAUSE_BATCH per read keeps this bounded; anything left
 * paused is picked up by the next read that finds the queue drained.
 */
#define KD_UNPAUSE_BATCH 16

static void kd_maybe_unpause(struct kd_dev *dev)
{
	struct kd_conn *batch[KD_UNPAUSE_BATCH];
	struct kd_conn *conn;
	unsigned long flags;
	int n = 0, i;

	spin_lock_irqsave(&dev->lock, flags);
	if (dev->qlen <= queue_low) {
		list_for_each_entry(conn, &dev->conns, list) {
			if (!conn->paused)
				continue;
			conn->paused = false;
			batch[n++] = conn;
			if (n == KD_UNPAUSE_BATCH)
				break;
		}
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	for (i = 0; i < n; i++)
		strp_unpause(&batch[i]->strp);
}

static ssize_t kd_read(struct file *file, char __user *ubuf, size_t count,
		       loff_t *ppos)
{
	struct kd_dev *dev = &kd;
	struct sk_buff *skb;
	struct strp_msg *stm;
	struct iov_iter to;
	unsigned long flags;
	int len, err;

	for (;;) {
		spin_lock_irqsave(&dev->lock, flags);
		skb = __skb_dequeue(&dev->queue);
		if (skb)
			dev->qlen--;
		spin_unlock_irqrestore(&dev->lock, flags);
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		err = wait_event_interruptible(dev->wq,
					       !skb_queue_empty(&dev->queue) ||
					       dev->draining);
		if (err)
			return err;
		if (dev->draining)
			return 0;
	}

	kd_maybe_unpause(dev);

	stm = strp_msg(skb);
	len = min_t(int, stm->full_len, (int)count);

	err = import_ubuf(ITER_DEST, ubuf, len, &to);
	if (!err)
		err = skb_copy_datagram_iter(skb, stm->offset, &to, len);
	kfree_skb(skb);

	return err ? err : len;
}

static long kd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct kd_dev *dev = &kd;
	unsigned long flags;
	int err;

	switch (cmd) {
	case KD_ATTACH: {
		struct kd_attach info;

		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		mutex_lock(&dev->attach_lock);
		err = kd_attach(dev, info.fd);
		mutex_unlock(&dev->attach_lock);
		return err;
	}
	case KD_STATS: {
		struct kd_stats st;

		memset(&st, 0, sizeof(st));
		spin_lock_irqsave(&dev->lock, flags);
		st.msgs = dev->msgs;
		st.bytes = dev->bytes;
		st.qlen = dev->qlen;
		st.qmax = dev->qmax;
		st.conns = dev->nconns;
		st.pauses = dev->pauses;
		st.aborts = dev->aborts;
		spin_unlock_irqrestore(&dev->lock, flags);
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	case KD_SHUTDOWN:
		kd.draining = true;
		smp_wmb();
		wake_up_interruptible_all(&dev->wq);
		return 0;
	default:
		return -ENOTTY;
	}
}

static int kd_open(struct inode *inode, struct file *file)
{
	mutex_lock(&kd.attach_lock);
	kd.opens++;
	kd.draining = false;
	mutex_unlock(&kd.attach_lock);
	return 0;
}

static int kd_release(struct inode *inode, struct file *file)
{
	struct kd_conn *conn, *tmp;
	unsigned long flags;
	LIST_HEAD(dying);

	mutex_lock(&kd.attach_lock);
	if (--kd.opens > 0) {
		mutex_unlock(&kd.attach_lock);
		return 0;
	}

	/* Last close: release every attached transport and drop the backlog. */
	kd.draining = true;
	wake_up_interruptible_all(&kd.wq);

	spin_lock_irqsave(&kd.lock, flags);
	list_splice_init(&kd.conns, &dying);
	kd.nconns = 0;
	spin_unlock_irqrestore(&kd.lock, flags);

	list_for_each_entry_safe(conn, tmp, &dying, list) {
		list_del(&conn->list);
		kd_detach(conn);
	}

	skb_queue_purge(&kd.queue);
	spin_lock_irqsave(&kd.lock, flags);
	kd.qlen = 0;
	spin_unlock_irqrestore(&kd.lock, flags);
	mutex_unlock(&kd.attach_lock);
	return 0;
}

static const struct file_operations kd_fops = {
	.owner = THIS_MODULE,
	.open = kd_open,
	.release = kd_release,
	.read = kd_read,
	.unlocked_ioctl = kd_ioctl,
	.compat_ioctl = kd_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice kd_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = KD_DEVICE_NAME,
	.fops = &kd_fops,
	.mode = 0666,
};

static int __init kd_init(void)
{
	int err;

	spin_lock_init(&kd.lock);
	skb_queue_head_init(&kd.queue);
	init_waitqueue_head(&kd.wq);
	INIT_LIST_HEAD(&kd.conns);
	mutex_init(&kd.attach_lock);

	err = misc_register(&kd_misc);
	if (err) {
		pr_err("misc_register failed: %d\n", err);
		return err;
	}
	pr_info("loaded (queue_high=%u queue_low=%u)\n", queue_high, queue_low);
	return 0;
}

static void __exit kd_exit(void)
{
	misc_deregister(&kd_misc);
	skb_queue_purge(&kd.queue);
	pr_info("unloaded\n");
}

module_init(kd_init);
module_exit(kd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Work-conserving in-kernel message dispatch for TCP RPCs");
