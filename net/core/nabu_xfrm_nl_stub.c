// SPDX-License-Identifier: GPL-2.0-only
/* nabu stub: register NETLINK_XFRM without CONFIG_XFRM.
 *
 * Android netd XfrmController::Init() opens NETLINK_XFRM and sends
 * XFRM_MSG_FLUSHSA / XFRM_MSG_FLUSHPOLICY. Full XFRM on this tree
 * fails to reach Android. A no-op ACK is enough: there is no IPsec.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/net.h>
#include <linux/netlink.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/sock.h>
#include <uapi/linux/xfrm.h>

#if IS_ENABLED(CONFIG_XFRM_USER)
/* Real xfrm_user already owns NETLINK_XFRM. */
#else

struct nabu_xfrm_nl_net {
	struct sock *nlsk;
};

static unsigned int nabu_xfrm_nl_net_id;

static int nabu_xfrm_nl_doit(struct sk_buff *skb, struct nlmsghdr *nlh,
			     struct netlink_ext_ack *extack)
{
	switch (nlh->nlmsg_type) {
	case XFRM_MSG_FLUSHSA:
	case XFRM_MSG_FLUSHPOLICY:
		pr_info_ratelimited("ack type=%u seq=%u\n",
				    nlh->nlmsg_type, nlh->nlmsg_seq);
		return 0;
	default:
		pr_info_ratelimited("noop type=%u seq=%u\n",
				    nlh->nlmsg_type, nlh->nlmsg_seq);
		return 0;
	}
}

static void nabu_xfrm_nl_rcv(struct sk_buff *skb)
{
	netlink_rcv_skb(skb, nabu_xfrm_nl_doit);
}

static int __net_init nabu_xfrm_nl_init(struct net *net)
{
	struct nabu_xfrm_nl_net *nn = net_generic(net, nabu_xfrm_nl_net_id);
	struct netlink_kernel_cfg cfg = {
		.input = nabu_xfrm_nl_rcv,
	};

	nn->nlsk = netlink_kernel_create(net, NETLINK_XFRM, &cfg);
	if (!nn->nlsk)
		return -ENOMEM;
	pr_info("NETLINK_XFRM stub registered\n");
	return 0;
}

static void __net_exit nabu_xfrm_nl_exit(struct net *net)
{
	struct nabu_xfrm_nl_net *nn = net_generic(net, nabu_xfrm_nl_net_id);

	netlink_kernel_release(nn->nlsk);
	nn->nlsk = NULL;
}

static struct pernet_operations nabu_xfrm_nl_ops = {
	.init = nabu_xfrm_nl_init,
	.exit = nabu_xfrm_nl_exit,
	.id   = &nabu_xfrm_nl_net_id,
	.size = sizeof(struct nabu_xfrm_nl_net),
};

static int __init nabu_xfrm_nl_mod_init(void)
{
	return register_pernet_subsys(&nabu_xfrm_nl_ops);
}

subsys_initcall(nabu_xfrm_nl_mod_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nabu NETLINK_XFRM stub for Android netd");
MODULE_ALIAS_NET_PF_PROTO(PF_NETLINK, NETLINK_XFRM);

#endif
