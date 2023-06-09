// SPDX-License-Identifier: GPL-2.0
/* Copyright 2011-2014 Autronica Fire and Security AS
 *
 * Author(s):
 *	2011-2014 Arvid Brodin, arvid.brodin@alten.se
 * This file contains device methods for creating, using and destroying
 * virtual HSR or PRP devices.
 */

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>
#include <linux/net_tstamp.h>
#include "hsr_device.h"
#include "hsr_slave.h"
#include "hsr_framereg.h"
#include "hsr_main.h"
#include "hsr_forward.h"

static inline bool is_slave_port(struct hsr_port *p)
{
	return (p->type == HSR_PT_SLAVE_A) ||
	       (p->type == HSR_PT_SLAVE_B);
}

static bool is_admin_up(struct net_device *dev)
{
	return dev && (dev->flags & IFF_UP);
}

static bool is_slave_up(struct net_device *dev)
{
	return dev && is_admin_up(dev) && netif_oper_up(dev);
}

static void __hsr_set_operstate(struct net_device *dev, int transition)
{
	write_lock_bh(&dev_base_lock);
	if (dev->operstate != transition) {
		dev->operstate = transition;
		write_unlock_bh(&dev_base_lock);
		netdev_state_change(dev);
	} else {
		write_unlock_bh(&dev_base_lock);
	}
}

static void hsr_set_operstate(struct hsr_port *master, bool has_carrier)
{
	if (!is_admin_up(master->dev)) {
		__hsr_set_operstate(master->dev, IF_OPER_DOWN);
		return;
	}

	if (has_carrier)
		__hsr_set_operstate(master->dev, IF_OPER_UP);
	else
		__hsr_set_operstate(master->dev, IF_OPER_LOWERLAYERDOWN);
}

static bool hsr_check_carrier(struct hsr_port *master)
{
	struct hsr_port *port;

	ASSERT_RTNL();

	hsr_for_each_port(master->hsr, port) {
		if (port->type != HSR_PT_MASTER && is_slave_up(port->dev)) {
			netif_carrier_on(master->dev);
			return true;
		}
	}

	netif_carrier_off(master->dev);

	return false;
}

static void hsr_check_announce(struct net_device *hsr_dev,
			       unsigned char old_operstate)
{
	struct hsr_priv *hsr;

	hsr = netdev_priv(hsr_dev);

	if (hsr_dev->operstate == IF_OPER_UP && old_operstate != IF_OPER_UP) {
		/* Went up */
		hsr->announce_count = 0;
		mod_timer(&hsr->announce_timer,
			  jiffies + msecs_to_jiffies(HSR_ANNOUNCE_INTERVAL));
	}

	if (hsr_dev->operstate != IF_OPER_UP && old_operstate == IF_OPER_UP)
		/* Went down */
		del_timer(&hsr->announce_timer);
}

void hsr_check_carrier_and_operstate(struct hsr_priv *hsr)
{
	struct hsr_port *master;
	unsigned char old_operstate;
	bool has_carrier;

	master = hsr_port_get_hsr(hsr, HSR_PT_MASTER);
	/* netif_stacked_transfer_operstate() cannot be used here since
	 * it doesn't set IF_OPER_LOWERLAYERDOWN (?)
	 */
	old_operstate = master->dev->operstate;
	has_carrier = hsr_check_carrier(master);
	hsr_set_operstate(master, has_carrier);
	hsr_check_announce(master->dev, old_operstate);
}

int hsr_get_max_mtu(struct hsr_priv *hsr)
{
	unsigned int mtu_max;
	struct hsr_port *port;

	mtu_max = ETH_DATA_LEN;
	hsr_for_each_port(hsr, port)
		if (port->type != HSR_PT_MASTER)
			mtu_max = min(port->dev->mtu, mtu_max);

	if (mtu_max < HSR_HLEN)
		return 0;

	/* For offloaded keep the mtu same as ETH_DATA_LEN as
	 * h/w is expected to extend the frame to accommodate RCT
	 * or TAG
	 */
	if (!hsr->rx_offloaded)
		return mtu_max - HSR_HLEN;

	return mtu_max;
}

int hsr_lredev_attr_get(struct hsr_priv *hsr, struct lredev_attr *attr)
{
	struct hsr_port *port_a = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	struct net_device *slave_a_dev;

	if (!port_a)
		return -EINVAL;

	slave_a_dev = port_a->dev;
	if (slave_a_dev && slave_a_dev->lredev_ops &&
	    slave_a_dev->lredev_ops->lredev_attr_get)
		return slave_a_dev->lredev_ops->lredev_attr_get(slave_a_dev,
								attr);
	return -EINVAL;
}

int hsr_lredev_attr_set(struct hsr_priv *hsr, struct lredev_attr *attr)
{
	struct hsr_port *port_a = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	struct net_device *slave_a_dev;

	if (!port_a)
		return -EINVAL;

	slave_a_dev = port_a->dev;
	if (slave_a_dev && slave_a_dev->lredev_ops &&
	    slave_a_dev->lredev_ops->lredev_attr_set)
		return slave_a_dev->lredev_ops->lredev_attr_set(slave_a_dev,
								attr);
	return -EINVAL;
}

static int _hsr_lredev_get_node_table(struct hsr_priv *hsr,
				      struct lre_node_table_entry table[],
				      int size)
{
	struct hsr_node *node;
	int i = 0;

	rcu_read_lock();

	list_for_each_entry_rcu(node, &hsr->node_db, mac_list) {
		if (hsr_addr_is_self(hsr, node->macaddress_A))
			continue;
		/* SANs are not shown as part of Node Table */
		if (node->san_a || node->san_b)
			continue;
		memcpy(&table[i].mac_address[0],
		       &node->macaddress_A[0], ETH_ALEN);
		table[i].time_last_seen_a = node->time_in[HSR_PT_SLAVE_A];
		table[i].time_last_seen_b = node->time_in[HSR_PT_SLAVE_B];
		if (hsr->prot_version == PRP_V1)
			table[i].node_type = IEC62439_3_DANP;
		else if (hsr->prot_version <= HSR_V1)
			table[i].node_type = IEC62439_3_DANH;
		else
			continue;
		i++;
	}
	rcu_read_unlock();

	return i;
}

int hsr_lredev_get_node_table(struct hsr_priv *hsr,
			      struct lre_node_table_entry table[],
			      int size)
{
	struct hsr_port *port_a = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	struct net_device *slave_a_dev;
	int ret = -EINVAL;

	if (!port_a)
		return ret;

	if (!hsr->rx_offloaded)
		return _hsr_lredev_get_node_table(hsr, table, size);

	slave_a_dev = port_a->dev;

	if (slave_a_dev && slave_a_dev->lredev_ops &&
	    slave_a_dev->lredev_ops->lredev_get_node_table)
		ret =
		slave_a_dev->lredev_ops->lredev_get_node_table(slave_a_dev,
							       table,
							       size);
	return ret;
}

static int hsr_set_sv_frame_vid(struct hsr_priv *hsr, u16 vid)
{
	struct hsr_port *port_a = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	struct net_device *slave_a_dev;
	int ret = -EINVAL;

	if (!port_a)
		return ret;

	slave_a_dev = port_a->dev;

	/* TODO can we use vlan_vid_add() here?? */
	if (slave_a_dev && slave_a_dev->lredev_ops &&
	    slave_a_dev->lredev_ops->lredev_set_sv_vlan_id)
		slave_a_dev->lredev_ops->lredev_set_sv_vlan_id(slave_a_dev,
							       vid);
	return 0;
}

int hsr_lredev_get_lre_stats(struct hsr_priv *hsr, struct lre_stats *stats)
{
	struct hsr_port *port_a = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	struct net_device *slave_a_dev;
	int ret = -EINVAL;

	if (!port_a)
		return ret;

	slave_a_dev = port_a->dev;

	if (slave_a_dev && slave_a_dev->lredev_ops &&
	    slave_a_dev->lredev_ops->lredev_get_stats)
		ret =
		slave_a_dev->lredev_ops->lredev_get_stats(slave_a_dev, stats);
	return ret;
}
static int hsr_dev_change_mtu(struct net_device *dev, int new_mtu)
{
	struct hsr_priv *hsr;

	hsr = netdev_priv(dev);

	if (new_mtu > hsr_get_max_mtu(hsr)) {
		netdev_info(dev, "A HSR master's MTU cannot be greater than the smallest MTU of its slaves minus the HSR Tag length (%d octets).\n",
			    HSR_HLEN);
		return -EINVAL;
	}

	dev->mtu = new_mtu;

	return 0;
}

static int hsr_dev_open(struct net_device *dev)
{
	struct hsr_priv *hsr;
	struct hsr_port *port;
	char designation;

	hsr = netdev_priv(dev);
	designation = '\0';

	hsr_for_each_port(hsr, port) {
		if (port->type == HSR_PT_MASTER)
			continue;
		switch (port->type) {
		case HSR_PT_SLAVE_A:
			designation = 'A';
			break;
		case HSR_PT_SLAVE_B:
			designation = 'B';
			break;
		default:
			designation = '?';
		}
		if (!is_slave_up(port->dev))
			netdev_warn(dev, "Slave %c (%s) is not up; please bring it up to get a fully working HSR network\n",
				    designation, port->dev->name);
	}

	if (designation == '\0')
		netdev_warn(dev, "No slave devices configured\n");

	return 0;
}

static int hsr_dev_close(struct net_device *dev)
{
	struct hsr_port *port;
	struct hsr_priv *hsr;

	hsr = netdev_priv(dev);
	hsr_for_each_port(hsr, port) {
		if (port->type == HSR_PT_MASTER)
			continue;
		switch (port->type) {
		case HSR_PT_SLAVE_A:
		case HSR_PT_SLAVE_B:
			dev_uc_unsync(port->dev, dev);
			dev_mc_unsync(port->dev, dev);
			break;
		default:
			break;
		}
	}

	return 0;
}

static netdev_features_t hsr_features_recompute(struct hsr_priv *hsr,
						netdev_features_t features)
{
	netdev_features_t mask;
	struct hsr_port *port;

	mask = features;

	/* Mask out all features that, if supported by one device, should be
	 * enabled for all devices (see NETIF_F_ONE_FOR_ALL).
	 *
	 * Anything that's off in mask will not be enabled - so only things
	 * that were in features originally, and also is in NETIF_F_ONE_FOR_ALL,
	 * may become enabled.
	 */
	features &= ~NETIF_F_ONE_FOR_ALL;
	hsr_for_each_port(hsr, port)
		features = netdev_increment_features(features,
						     port->dev->features,
						     mask);

	return features;
}

static netdev_features_t hsr_fix_features(struct net_device *dev,
					  netdev_features_t features)
{
	struct hsr_priv *hsr = netdev_priv(dev);

	return hsr_features_recompute(hsr, features);
}

static netdev_tx_t hsr_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct hsr_priv *hsr = netdev_priv(dev);
	struct hsr_port *master;

	master = hsr_port_get_hsr(hsr, HSR_PT_MASTER);
	if (master) {
		skb->dev = master->dev;
		skb_reset_mac_header(skb);
		skb_reset_mac_len(skb);
		spin_lock_bh(&hsr->seqnr_lock);
		hsr_forward_skb(skb, master);
		INC_CNT_RX_C(hsr);
		spin_unlock_bh(&hsr->seqnr_lock);
	} else {
		atomic_long_inc(&dev->tx_dropped);
		dev_kfree_skb_any(skb);
	}
	return NETDEV_TX_OK;
}

static const struct header_ops hsr_header_ops = {
	.create	 = eth_header,
	.parse	 = eth_header_parse,
};

static struct sk_buff *hsr_init_skb(struct hsr_port *master)
{
	struct hsr_priv *hsr = master->hsr;
	struct sk_buff *skb;
	int hlen, tlen;
	u16 proto;
	int len;

	hsr = master->hsr;

	if (hsr->disable_sv_frame)
		return NULL;

	hlen = LL_RESERVED_SPACE(master->dev);
	tlen = master->dev->needed_tailroom;
	len = sizeof(struct hsr_tag) +
	      sizeof(struct hsr_sup_tag) +
	      sizeof(struct hsr_sup_payload) + hlen + tlen;

	if (hsr->use_vlan_for_sv)
		len += VLAN_HLEN;

	/* skb size is same for PRP/HSR frames, only difference
	 * being, for PRP it is a trailer and for HSR it is a
	 * header
	 */
	skb = dev_alloc_skb(len);
	if (!skb)
		return skb;

	skb_reserve(skb, hlen);
	skb->dev = master->dev;
	proto = ETH_P_PRP;

	if (hsr->use_vlan_for_sv) {
		proto = ETH_P_8021Q;
		skb->priority = hsr->sv_frame_pcp;
	} else {
		skb->priority = TC_PRIO_CONTROL;
	}
	skb->protocol = htons(proto);

	if (dev_hard_header(skb, skb->dev, proto,
			    hsr->sup_multicast_addr,
			    skb->dev->dev_addr, skb->len) <= 0)
		goto out;

	skb_reset_mac_header(skb);
	skb_reset_mac_len(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);

	return skb;
out:
	kfree_skb(skb);

	return NULL;
}

static void send_hsr_supervision_frame(struct hsr_port *master,
				       unsigned long *interval)
{
	struct hsr_priv *hsr = master->hsr;
	__u8 type = HSR_TLV_LIFE_CHECK;
	struct hsr_tag *hsr_tag = NULL;
	struct hsr_sup_payload *hsr_sp;
	struct hsr_sup_tag *hsr_stag;
	struct sk_buff *skb;
	struct vlan_hdr *vhdr;
	u16 vlan_tci = 0;

	*interval = msecs_to_jiffies(HSR_LIFE_CHECK_INTERVAL);
	if (hsr->announce_count < 3 && hsr->prot_version == 0) {
		type = HSR_TLV_ANNOUNCE;
		*interval = msecs_to_jiffies(HSR_ANNOUNCE_INTERVAL);
		hsr->announce_count++;
	}

	skb = hsr_init_skb(master);
	if (!skb) {
		WARN_ONCE(1, "HSR: Could not send supervision frame\n");
		return;
	}

	if (hsr->use_vlan_for_sv) {
		vhdr = skb_put(skb, VLAN_HLEN);
		vlan_tci = hsr->sv_frame_vid;
		vlan_tci |= (hsr->sv_frame_pcp	<< VLAN_PRIO_SHIFT);
		if (hsr->sv_frame_dei)
			vlan_tci |= VLAN_CFI_MASK;
		vhdr->h_vlan_TCI = htons(vlan_tci);
		vhdr->h_vlan_encapsulated_proto = htons(ETH_P_PRP);
	}

	if (hsr->prot_version > 0) {
		hsr_tag = skb_put(skb, sizeof(struct hsr_tag));
		hsr_tag->encap_proto = htons(ETH_P_PRP);
		set_hsr_tag_LSDU_size(hsr_tag, HSR_V1_SUP_LSDUSIZE);
	}

	hsr_stag = skb_put(skb, sizeof(struct hsr_sup_tag));
	set_hsr_stag_path(hsr_stag, (hsr->prot_version ? 0x0 : 0xf));
	set_hsr_stag_HSR_ver(hsr_stag, hsr->prot_version);

	/* From HSRv1 on we have separate supervision sequence numbers. */
	spin_lock_bh(&hsr->seqnr_lock);
	if (hsr->prot_version > 0) {
		hsr_stag->sequence_nr = htons(hsr->sup_sequence_nr);
		hsr->sup_sequence_nr++;
	} else {
		hsr_stag->sequence_nr = htons(hsr->sequence_nr);
		hsr->sequence_nr++;
	}

	hsr_stag->HSR_TLV_type = type;
	/* TODO: Why 12 in HSRv0? */
	hsr_stag->HSR_TLV_length = hsr->prot_version ?
				sizeof(struct hsr_sup_payload) : 12;

	/* Payload: MacAddressA */
	hsr_sp = skb_put(skb, sizeof(struct hsr_sup_payload));
	ether_addr_copy(hsr_sp->macaddress_A, master->dev->dev_addr);

	if (!hsr->use_vlan_for_sv) {
		if (skb_put_padto(skb, ETH_ZLEN)) {
			spin_unlock_bh(&hsr->seqnr_lock);
			return;
		}
	} else {
		if (skb_put_padto(skb, ETH_ZLEN + VLAN_HLEN)) {
			spin_unlock_bh(&hsr->seqnr_lock);
			return;
		}
	}

	hsr_forward_skb(skb, master);
	INC_CNT_TX_SUP(hsr);
	spin_unlock_bh(&hsr->seqnr_lock);
	return;
}

static void send_prp_supervision_frame(struct hsr_port *master,
				       unsigned long *interval)
{
	struct hsr_priv *hsr = master->hsr;
	struct hsr_sup_payload *hsr_sp;
	struct hsr_sup_tag *hsr_stag;
	struct sk_buff *skb;

	skb = hsr_init_skb(master);
	if (!skb) {
		WARN_ONCE(1, "PRP: Could not send supervision frame\n");
		return;
	}

	*interval = msecs_to_jiffies(HSR_LIFE_CHECK_INTERVAL);
	hsr_stag = skb_put(skb, sizeof(struct hsr_sup_tag));
	set_hsr_stag_path(hsr_stag, (hsr->prot_version ? 0x0 : 0xf));
	set_hsr_stag_HSR_ver(hsr_stag, (hsr->prot_version ? 1 : 0));

	/* From HSRv1 on we have separate supervision sequence numbers. */
	spin_lock_bh(&hsr->seqnr_lock);
	hsr_stag->sequence_nr = htons(hsr->sup_sequence_nr);
	hsr->sup_sequence_nr++;
	hsr_stag->HSR_TLV_type = PRP_TLV_LIFE_CHECK_DD;
	hsr_stag->HSR_TLV_length = sizeof(struct hsr_sup_payload);

	/* Payload: MacAddressA */
	hsr_sp = skb_put(skb, sizeof(struct hsr_sup_payload));
	ether_addr_copy(hsr_sp->macaddress_A, master->dev->dev_addr);

	if (skb_put_padto(skb, ETH_ZLEN)) {
		spin_unlock_bh(&hsr->seqnr_lock);
		return;
	}

	hsr_forward_skb(skb, master);
	INC_CNT_TX_SUP(hsr);
	spin_unlock_bh(&hsr->seqnr_lock);
}

/* Announce (supervision frame) timer function
 */
static void hsr_announce(struct timer_list *t)
{
	struct hsr_priv *hsr;
	struct hsr_port *master;
	unsigned long interval;

	hsr = from_timer(hsr, t, announce_timer);

	rcu_read_lock();
	master = hsr_port_get_hsr(hsr, HSR_PT_MASTER);
	hsr->proto_ops->send_sv_frame(master, &interval);

	if (is_admin_up(master->dev))
		mod_timer(&hsr->announce_timer, jiffies + interval);

	rcu_read_unlock();
}

void hsr_del_ports(struct hsr_priv *hsr, struct net_device *hsr_dev)
{
	struct hsr_port *port;

	hsr_remove_procfs(hsr, hsr_dev);
	hsr_debugfs_term(hsr);

	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_A);
	if (port)
		hsr_del_port(port);

	port = hsr_port_get_hsr(hsr, HSR_PT_SLAVE_B);
	if (port)
		hsr_del_port(port);

	port = hsr_port_get_hsr(hsr, HSR_PT_MASTER);
	if (port)
		hsr_del_port(port);
}

static void hsr_ndo_set_rx_mode(struct net_device *dev)
{
	struct hsr_port *port;
	struct hsr_priv *hsr;

	hsr = netdev_priv(dev);

	hsr_for_each_port(hsr, port) {
		if (port->type == HSR_PT_MASTER)
			continue;
		switch (port->type) {
		case HSR_PT_SLAVE_A:
		case HSR_PT_SLAVE_B:
			dev_mc_sync_multiple(port->dev, dev);
			dev_uc_sync_multiple(port->dev, dev);
			break;
		default:
			break;
		}
	}
}

static void hsr_change_rx_flags(struct net_device *dev, int change)
{
	struct hsr_port *port;
	struct hsr_priv *hsr;

	hsr = netdev_priv(dev);

	hsr_for_each_port(hsr, port) {
		if (port->type == HSR_PT_MASTER)
			continue;
		switch (port->type) {
		case HSR_PT_SLAVE_A:
		case HSR_PT_SLAVE_B:
			if (change & IFF_ALLMULTI)
				dev_set_allmulti(port->dev,
						 dev->flags &
						 IFF_ALLMULTI ? 1 : -1);
			break;
		default:
			break;
		}
	}
}

static int hsr_ndo_vlan_rx_add_vid(struct net_device *dev,
				   __be16 proto, u16 vid)
{
	struct hsr_port *port;
	struct hsr_priv *hsr;
	int ret = 0;

	hsr = netdev_priv(dev);

	hsr_for_each_port(hsr, port) {
		if (port->type == HSR_PT_MASTER)
			continue;

		ret = vlan_vid_add(port->dev, proto, vid);
		switch (port->type) {
		case HSR_PT_SLAVE_A:
			if (ret) {
				netdev_err(dev, "add vid failed for Slave-A\n");
				return ret;
			}
			break;

		case HSR_PT_SLAVE_B:
			if (ret) {
				/* clean up Slave-A */
				netdev_err(dev, "add vid failed for Slave-B\n");
				vlan_vid_del(port->dev, proto, vid);
				return ret;
			}
			break;
		default:
			break;
		};
	}

	return 0;
}

static int hsr_dev_ioctl(struct net_device *hsr_dev, struct ifreq *req, int cmd)
{
	struct hsr_priv *priv = netdev_priv(hsr_dev);
	const struct net_device_ops *ops;
	struct hsr_port *port;
	int ret = -EOPNOTSUPP;

	if (cmd != SIOCSHWTSTAMP && cmd != SIOCGHWTSTAMP)
		return ret;

	hsr_for_each_port(priv, port) {
		if (is_slave_port(port)) {
			ops = port->dev->netdev_ops;
			if (ops && ops->ndo_do_ioctl) {
				ret = ops->ndo_do_ioctl(port->dev, req, cmd);

				if (cmd == SIOCGHWTSTAMP || cmd < 0)
					return ret;
			}
		}
	}

	return ret;
}

static int hsr_ndo_vlan_rx_kill_vid(struct net_device *dev,
				    __be16 proto, u16 vid)
{
	struct hsr_port *port;
	struct hsr_priv *hsr;

	hsr = netdev_priv(dev);

	hsr_for_each_port(hsr, port) {
		if (port->type == HSR_PT_MASTER)
			continue;
		switch (port->type) {
		case HSR_PT_SLAVE_A:
		case HSR_PT_SLAVE_B:
			vlan_vid_del(port->dev, proto, vid);
			break;
		default:
			break;
		};
	}

	return 0;
}

static const struct net_device_ops hsr_device_ops = {
	.ndo_change_mtu = hsr_dev_change_mtu,
	.ndo_open = hsr_dev_open,
	.ndo_stop = hsr_dev_close,
	.ndo_start_xmit = hsr_dev_xmit,
	.ndo_change_rx_flags = hsr_change_rx_flags,
	.ndo_fix_features = hsr_fix_features,
	.ndo_set_rx_mode = hsr_ndo_set_rx_mode,
	.ndo_vlan_rx_add_vid = hsr_ndo_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = hsr_ndo_vlan_rx_kill_vid,
	.ndo_do_ioctl = hsr_dev_ioctl,
};

static int hsr_get_ts_info(struct net_device *dev, struct ethtool_ts_info *info)
{
	struct hsr_priv *priv = netdev_priv(dev);
	struct hsr_port *port;
	const struct ethtool_ops *ops;
	int ret = -EOPNOTSUPP;

	hsr_for_each_port(priv, port) {
		if (is_slave_port(port)) {
			ops = port->dev->ethtool_ops;
			if (ops && ops->get_ts_info) {
				ret = ops->get_ts_info(port->dev, info);
				return ret;
			}
		}
	}

	return ret;
}

static const struct ethtool_ops hsr_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_ts_info = hsr_get_ts_info,
};

static struct device_type hsr_type = {
	.name = "hsr",
};

static struct hsr_proto_ops hsr_ops = {
	.send_sv_frame = send_hsr_supervision_frame,
	.create_tagged_frame = hsr_create_tagged_frame,
	.get_untagged_frame = hsr_get_untagged_frame,
	.drop_frame = hsr_drop_frame,
	.fill_frame_info = hsr_fill_frame_info,
	.invalid_dan_ingress_frame = hsr_invalid_dan_ingress_frame,
};

static struct hsr_proto_ops prp_ops = {
	.send_sv_frame = send_prp_supervision_frame,
	.create_tagged_frame = prp_create_tagged_frame,
	.get_untagged_frame = prp_get_untagged_frame,
	.drop_frame = prp_drop_frame,
	.fill_frame_info = prp_fill_frame_info,
	.handle_san_frame = prp_handle_san_frame,
	.update_san_info = prp_update_san_info,
};

void hsr_dev_setup(struct net_device *dev)
{
	eth_hw_addr_random(dev);

	ether_setup(dev);
	dev->min_mtu = 0;
	dev->header_ops = &hsr_header_ops;
	dev->netdev_ops = &hsr_device_ops;
	dev->ethtool_ops = &hsr_ethtool_ops;
	SET_NETDEV_DEVTYPE(dev, &hsr_type);
	dev->priv_flags |= IFF_NO_QUEUE | IFF_DISABLE_NETPOLL;

	dev->needs_free_netdev = true;

	dev->hw_features = NETIF_F_SG | NETIF_F_FRAGLIST | NETIF_F_HIGHDMA |
			   NETIF_F_GSO_MASK | NETIF_F_HW_CSUM |
			   NETIF_F_HW_VLAN_CTAG_TX |
			   NETIF_F_HW_VLAN_CTAG_FILTER;

	dev->features = dev->hw_features;

	/* Prevent recursive tx locking */
	dev->features |= NETIF_F_LLTX;
	/* Not sure about this. Taken from bridge code. netdev_features.h says
	 * it means "Does not change network namespaces".
	 */
	dev->features |= NETIF_F_NETNS_LOCAL;
}

/* Return true if dev is a HSR master; return false otherwise.
 */
bool is_hsr_master(struct net_device *dev)
{
	return (dev->netdev_ops->ndo_start_xmit == hsr_dev_xmit);
}
EXPORT_SYMBOL(is_hsr_master);

/* Default multicast address for HSR Supervision frames */
static const unsigned char def_multicast_addr[ETH_ALEN] __aligned(2) = {
	0x01, 0x15, 0x4e, 0x00, 0x01, 0x00
};

int hsr_dev_finalize(struct net_device *hsr_dev, struct net_device *slave[2],
		     unsigned char multicast_spec, u8 protocol_version,
		     struct netlink_ext_ack *extack, bool sv_vlan_tag_needed, unsigned short vid,
		     unsigned char pcp, unsigned char dei)
{
	bool unregister = false;
	struct hsr_priv *hsr;
	int res;

	hsr = netdev_priv(hsr_dev);
	INIT_LIST_HEAD(&hsr->ports);
	INIT_LIST_HEAD(&hsr->node_db);
	INIT_LIST_HEAD(&hsr->self_node_db);
	spin_lock_init(&hsr->list_lock);

	ether_addr_copy(hsr_dev->dev_addr, slave[0]->dev_addr);

	/* initialize protocol specific functions */
	if (protocol_version == PRP_V1) {
		/* For PRP, lan_id has most significant 3 bits holding
		 * the net_id of PRP_LAN_ID and also duplicate discard
		 * mode set.
		 */
		hsr->net_id = PRP_LAN_ID << 1;
		hsr->proto_ops = &prp_ops;
		hsr->dd_mode = IEC62439_3_DD;
	} else {
		hsr->proto_ops = &hsr_ops;
		hsr->hsr_mode = IEC62439_3_HSR_MODE_H;
	}

	/* Make sure we recognize frames from ourselves in hsr_rcv() */
	res = hsr_create_self_node(hsr, hsr_dev->dev_addr,
				   slave[1]->dev_addr);
	if (res < 0)
		return res;

	spin_lock_init(&hsr->seqnr_lock);
	/* Overflow soon to find bugs easier: */
	hsr->sequence_nr = HSR_SEQNR_START;
	hsr->sup_sequence_nr = HSR_SUP_SEQNR_START;

	timer_setup(&hsr->announce_timer, hsr_announce, 0);
	if (!hsr->rx_offloaded)
		timer_setup(&hsr->prune_timer, hsr_prune_nodes, 0);

	ether_addr_copy(hsr->sup_multicast_addr, def_multicast_addr);
	hsr->sup_multicast_addr[ETH_ALEN - 1] = multicast_spec;

	hsr->prot_version = protocol_version;
	/* update vlan tag infor for SV frames */
	hsr->use_vlan_for_sv = sv_vlan_tag_needed;
	hsr->sv_frame_vid = vid;
	hsr->sv_frame_dei = dei;
	hsr->sv_frame_pcp = pcp;

	/* FIXME: should I modify the value of these?
	 *
	 * - hsr_dev->flags - i.e.
	 *			IFF_MASTER/SLAVE?
	 * - hsr_dev->priv_flags - i.e.
	 *			IFF_EBRIDGE?
	 *			IFF_TX_SKB_SHARING?
	 *			IFF_HSR_MASTER/SLAVE?
	 */

	/* Make sure the 1st call to netif_carrier_on() gets through */
	netif_carrier_off(hsr_dev);

	res = hsr_add_port(hsr, hsr_dev, HSR_PT_MASTER, extack);
	if (res)
		goto err_add_master;

	/* HSR/PRP LRE Rx offload supported in lower device? */
	if ((slave[0]->features & NETIF_F_HW_HSR_TAG_RM) &&
	    (slave[1]->features & NETIF_F_HW_HSR_TAG_RM))
		hsr->rx_offloaded = true;

	if ((slave[0]->features & NETIF_F_HW_VLAN_CTAG_FILTER) &&
	    (slave[1]->features & NETIF_F_HW_VLAN_CTAG_FILTER))
		hsr_dev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;

	res = register_netdevice(hsr_dev);
	if (res)
		goto err_unregister;

	unregister = true;

	res = hsr_add_port(hsr, slave[0], HSR_PT_SLAVE_A, extack);
	if (res)
		goto err_unregister;

	res = hsr_add_port(hsr, slave[1], HSR_PT_SLAVE_B, extack);
	if (res)
		goto err_unregister;

	hsr_debugfs_init(hsr, hsr_dev);

	/* For LRE rx offload, pruning is expected to happen
	 * at the hardware or firmware . So don't do this in software
	 */
	if (!hsr->rx_offloaded)
		mod_timer(&hsr->prune_timer,
			  jiffies + msecs_to_jiffies(PRUNE_PERIOD));
	/* for offloaded case, expect both slaves have the
	 * same MAC address configured. If not fail.
	 */
	if (hsr->rx_offloaded &&
	    !ether_addr_equal(slave[0]->dev_addr,
			      slave[1]->dev_addr)) {
		netdev_err(hsr_dev,
			   "Slave's MAC addr must be same. So change it\n");
		res = -EINVAL;
		goto err_add_slaves;
	}

	res = hsr_create_procfs(hsr, hsr_dev);
	if (res)
		goto err_add_slaves;

	if (hsr->use_vlan_for_sv)
		res = hsr_set_sv_frame_vid(hsr, hsr->sv_frame_vid);

	if (res)
		goto err_procfs;

	return 0;
err_procfs:
	hsr_remove_procfs(hsr, hsr_dev);
err_unregister:
	hsr_del_ports(hsr, hsr_dev);
err_add_master:
	hsr_del_self_node(hsr);
err_add_slaves:
	if (unregister)
		unregister_netdevice(hsr_dev);
	return res;
}
