// SPDX-License-Identifier: GPL-2.0-only
/* nabu stub: register xt_policy revision 0 without CONFIG_XFRM.
 *
 * Android netd BandwidthController always installs
 *   -m policy --pol ipsec --dir in/out -j RETURN
 * Full XFRM on this mainline tree fails to reach Android. Without IPsec
 * the match never hits, so returning false is the correct no-XFRM behavior.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/netfilter/xt_policy.h>
#include <linux/netfilter/x_tables.h>

static bool policy_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	return false;
}

static int policy_mt_check(const struct xt_mtchk_param *par)
{
	const struct xt_policy_info *info = par->matchinfo;
	const char *errmsg = "neither incoming nor outgoing policy selected";

	if (!(info->flags & (XT_POLICY_MATCH_IN | XT_POLICY_MATCH_OUT)))
		goto err;

	if (par->hook_mask & ((1 << NF_INET_PRE_ROUTING) |
	    (1 << NF_INET_LOCAL_IN)) && info->flags & XT_POLICY_MATCH_OUT) {
		errmsg = "output policy not valid in PREROUTING and INPUT";
		goto err;
	}
	if (par->hook_mask & ((1 << NF_INET_POST_ROUTING) |
	    (1 << NF_INET_LOCAL_OUT)) && info->flags & XT_POLICY_MATCH_IN) {
		errmsg = "input policy not valid in POSTROUTING and OUTPUT";
		goto err;
	}
	if (info->len > XT_POLICY_MAX_ELEM) {
		errmsg = "too many policy elements";
		goto err;
	}
	return 0;
err:
	pr_info_ratelimited("%s\n", errmsg);
	return -EINVAL;
}

static struct xt_match policy_mt_reg[] __read_mostly = {
	{
		.name		= "policy",
		.family		= NFPROTO_IPV4,
		.checkentry	= policy_mt_check,
		.match		= policy_mt,
		.matchsize	= sizeof(struct xt_policy_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "policy",
		.family		= NFPROTO_IPV6,
		.checkentry	= policy_mt_check,
		.match		= policy_mt,
		.matchsize	= sizeof(struct xt_policy_info),
		.me		= THIS_MODULE,
	},
};

static int __init policy_mt_init(void)
{
	return xt_register_matches(policy_mt_reg, ARRAY_SIZE(policy_mt_reg));
}

static void __exit policy_mt_exit(void)
{
	xt_unregister_matches(policy_mt_reg, ARRAY_SIZE(policy_mt_reg));
}

module_init(policy_mt_init);
module_exit(policy_mt_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xtables: IPsec policy match stub (no XFRM)");
MODULE_ALIAS("ipt_policy");
MODULE_ALIAS("ip6t_policy");
