// SPDX-License-Identifier: GPL-2.0-only
/* nabu stub: register PF_KEY without CONFIG_NET_KEY / CONFIG_XFRM.
 *
 * Android BpfNetMaps.synchronizeKernelRCU() does
 *   socket(AF_KEY, SOCK_RAW, PF_KEY_V2) then close().
 * Real af_key.c calls synchronize_rcu() in pfkey_release(). Full
 * NET_KEY selects XFRM, and full XFRM on this tree does not reach
 * Android. A socket that succeeds and syncs RCU on close is enough.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/net.h>
#include <linux/rcupdate.h>
#include <linux/socket.h>
#include <net/sock.h>
#include <uapi/linux/pfkeyv2.h>

#if IS_ENABLED(CONFIG_NET_KEY)
/* Real af_key already owns PF_KEY. */
#else

static struct proto nabu_pfkey_proto = {
	.name	  = "KEY",
	.owner	  = THIS_MODULE,
	.obj_size = sizeof(struct sock),
};

static int nabu_pfkey_release(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (!sk)
		return 0;

	sock_orphan(sk);
	sock->sk = NULL;
	skb_queue_purge(&sk->sk_write_queue);
	synchronize_rcu();
	sock_put(sk);
	return 0;
}

static const struct proto_ops nabu_pfkey_ops = {
	.family		= PF_KEY,
	.owner		= THIS_MODULE,
	.release	= nabu_pfkey_release,
	.bind		= sock_no_bind,
	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= sock_no_getname,
	.poll		= datagram_poll,
	.ioctl		= sock_no_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.sendmsg	= sock_no_sendmsg,
	.recvmsg	= sock_no_recvmsg,
	.mmap		= sock_no_mmap,
};

static int nabu_pfkey_create(struct net *net, struct socket *sock, int protocol,
			     int kern)
{
	struct sock *sk;

	if (sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;
	if (protocol != PF_KEY_V2)
		return -EPROTONOSUPPORT;

	sk = sk_alloc(net, PF_KEY, GFP_KERNEL, &nabu_pfkey_proto, kern);
	if (!sk)
		return -ENOMEM;

	sock->ops = &nabu_pfkey_ops;
	sock_init_data(sock, sk);
	sk->sk_family = PF_KEY;
	return 0;
}

static const struct net_proto_family nabu_pfkey_family = {
	.family	= PF_KEY,
	.create	= nabu_pfkey_create,
	.owner	= THIS_MODULE,
};

static int __init nabu_pfkey_init(void)
{
	int err = proto_register(&nabu_pfkey_proto, 0);

	if (err)
		return err;
	err = sock_register(&nabu_pfkey_family);
	if (err) {
		proto_unregister(&nabu_pfkey_proto);
		return err;
	}
	pr_info("PF_KEY stub registered\n");
	return 0;
}

subsys_initcall(nabu_pfkey_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nabu PF_KEY stub for Android synchronizeKernelRCU");
MODULE_ALIAS_NET_PF_PROTO(PF_KEY, PF_KEY_V2);

#endif
