// SPDX-License-Identifier: GPL-2.0
/*
 * iwl_ratefix: puts the Intel 7265's host-rate frames back on the rate
 * mac80211 asked for.
 *
 * Linux 6.18's iwlmvm builds rate_n_flags in the v3 format, where a legacy
 * rate is an index, but for TX_CMD version <= 8 firmware (7265D-29) it
 * stores the PLCP value instead. iwl_mvm_v3_rate_to_fw() then reads that
 * PLCP as an index (& 7), so 1 Mbit/s goes out at 5.5 Mbit/s, 2 Mbit/s as
 * garbage, and so on. Phones don't mind; a DS (1 and 2 Mbit/s only) never
 * hears anything but the beacons, which take a different path.
 *
 * This module sits on iwl_trans_tx() with a kprobe and undoes the mapping
 * in the TX command before the PCIe layer copies it to the card.
 */
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#define TX_CMD_FLG_STA_RATE	(1u << 4)
#define RATE_CCK_V1		(1u << 9)
#define RATE_HT_V1		(1u << 8)
#define RATE_VHT_V1		(1u << 26)

/* dev_cmd = 4-byte command header + TX command v6 */
#define OFF_TX_FLAGS		(4 + 4)
#define OFF_RATE		(4 + 12)

static unsigned long fixed;
module_param(fixed, ulong, 0444);
MODULE_PARM_DESC(fixed, "frames whose rate was corrected");

static u8 fix_plcp(u8 plcp, bool cck)
{
	if (cck) {
		switch (plcp) {
		case 55: return 10;	/* asked 1 Mbit/s */
		case 13: return 20;	/* asked 2 Mbit/s */
		case 7: return 55;	/* asked 5.5 Mbit/s */
		case 5: return 110;	/* asked 11 Mbit/s */
		}
	} else if (plcp == 11) {
		return 13;		/* most likely asked 6 Mbit/s */
	}
	return plcp;
}

static int pre(struct kprobe *p, struct pt_regs *regs)
{
	u8 *dev_cmd = (u8 *)regs_get_kernel_argument(regs, 2);
	u32 flags, rate;
	u8 plcp, want;

	if (!dev_cmd)
		return 0;
	flags = get_unaligned_le32(dev_cmd + OFF_TX_FLAGS);
	rate = get_unaligned_le32(dev_cmd + OFF_RATE);
	if ((flags & TX_CMD_FLG_STA_RATE) || !rate ||
	    (rate & (RATE_HT_V1 | RATE_VHT_V1)))
		return 0;
	plcp = rate & 0xff;
	want = fix_plcp(plcp, rate & RATE_CCK_V1);
	if (want != plcp) {
		put_unaligned_le32((rate & ~0xffu) | want, dev_cmd + OFF_RATE);
		fixed++;
	}
	return 0;
}

static struct kprobe kp = {
	.symbol_name = "iwl_trans_tx",
	.pre_handler = pre,
};

static int __init ratefix_init(void)
{
	int ret = register_kprobe(&kp);

	if (ret)
		pr_err("iwl_ratefix: can't probe iwl_trans_tx: %d\n", ret);
	else
		pr_info("iwl_ratefix: correcting iwlmvm legacy TX rates\n");
	return ret;
}

static void __exit ratefix_exit(void)
{
	unregister_kprobe(&kp);
	pr_info("iwl_ratefix: removed after %lu fixes\n", fixed);
}

module_init(ratefix_init);
module_exit(ratefix_exit);
MODULE_DESCRIPTION("Correct iwlmvm v1-firmware legacy TX rates (Sion Link)");
MODULE_LICENSE("GPL");
