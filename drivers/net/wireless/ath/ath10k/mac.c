/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2013 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "mac.h"

#include <net/mac80211.h>
#include <linux/etherdevice.h>

#include "core.h"
#include "debug.h"
#include "wmi.h"
#include "htt.h"
#include "txrx.h"

/**********/
/* Crypto */
/**********/

static int ath10k_send_key(struct ath10k_vif *arvif,
			   struct ieee80211_key_conf *key,
			   enum set_key_cmd cmd,
			   const u8 *macaddr)
{
	struct wmi_vdev_install_key_arg arg = {
		.vdev_id = arvif->vdev_id,
		.key_idx = key->keyidx,
		.key_len = key->keylen,
		.key_data = key->key,
		.macaddr = macaddr,
	};

	if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE)
		arg.key_flags = WMI_KEY_PAIRWISE;
	else
		arg.key_flags = WMI_KEY_GROUP;

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_CCMP:
		arg.key_cipher = WMI_CIPHER_AES_CCM;
		key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
		arg.key_cipher = WMI_CIPHER_TKIP;
		arg.key_txmic_len = 8;
		arg.key_rxmic_len = 8;
		break;
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
		arg.key_cipher = WMI_CIPHER_WEP;
		/* AP/IBSS mode requires self-key to be groupwise
		 * Otherwise pairwise key must be set */
		if (memcmp(macaddr, arvif->vif->addr, ETH_ALEN))
			arg.key_flags = WMI_KEY_PAIRWISE;
		break;
	default:
		ath10k_warn("cipher %d is not supported\n", key->cipher);
		return -EOPNOTSUPP;
	}

	if (cmd == DISABLE_KEY) {
		arg.key_cipher = WMI_CIPHER_NONE;
		arg.key_data = NULL;
	}

	return wmi_vdev_install_key(arvif->ar, &arg);
}

static int ath10k_install_key(struct ath10k_vif *arvif,
			      struct ieee80211_key_conf *key,
			      enum set_key_cmd cmd,
			      const u8 *macaddr)
{
	struct ath10k *ar = arvif->ar;
	int ret;

	INIT_COMPLETION(ar->install_key_done);

	ret = ath10k_send_key(arvif, key, cmd, macaddr);
	if (ret)
		return ret;

	ret = wait_for_completion_timeout(&ar->install_key_done, 3*HZ);
	if (ret == 0)
		ret = -ETIMEDOUT;
	if (ret > 0)
		ret = 0;

	return ret;
}

static int ath10k_install_peer_wep_keys(struct ath10k_vif *arvif,
					const u8 *addr)
{
	struct ath10k *ar = arvif->ar;
	struct ath10k_peer *peer;
	int ret;
	int i;

	lockdep_assert_held(&ar->conf_mutex);

	spin_lock_bh(&ar->data_lock);
	peer = ath10k_peer_find(ar, arvif->vdev_id, addr);
	spin_unlock_bh(&ar->data_lock);

	if (!peer)
		return -ENOENT;

	for (i = 0; i < ARRAY_SIZE(arvif->wep_keys); i++) {
		if (arvif->wep_keys[i] == NULL)
			continue;

		ret = ath10k_install_key(arvif, arvif->wep_keys[i], SET_KEY,
					 addr);
		if (ret)
			return ret;

		peer->keys[i] = arvif->wep_keys[i];
	}

	return 0;
}

static int ath10k_clear_peer_keys(struct ath10k_vif *arvif,
				  const u8 *addr)
{
	struct ath10k *ar = arvif->ar;
	struct ath10k_peer *peer;
	int first_errno = 0;
	int ret;
	int i;

	lockdep_assert_held(&ar->conf_mutex);

	spin_lock_bh(&ar->data_lock);
	peer = ath10k_peer_find(ar, arvif->vdev_id, addr);
	spin_unlock_bh(&ar->data_lock);

	if (!peer)
		return -ENOENT;

	for (i = 0; i < ARRAY_SIZE(peer->keys); i++) {
		if (peer->keys[i] == NULL)
			continue;

		ret = ath10k_install_key(arvif, peer->keys[i], DISABLE_KEY, addr);
		if (ret && first_errno == 0)
			first_errno = ret;

		if (ret)
			ath10k_warn("could not remove peer wep key %d (%d)\n", i, ret);

		peer->keys[i] = NULL;
	}

	return first_errno;
}

static int ath10k_clear_vdev_key(struct ath10k_vif *arvif,
				 struct ieee80211_key_conf *key)
{
	struct ath10k *ar = arvif->ar;
	struct ath10k_peer *peer;
	u8 addr[ETH_ALEN];
	int first_errno = 0;
	int ret;
	int i;

	lockdep_assert_held(&ar->conf_mutex);

	for (;;) {
		/* since ath10k_install_key we can't hold data_lock all the
		 * time, so we try to remove the keys incrementally */
		spin_lock_bh(&ar->data_lock);
		i = 0;
		list_for_each_entry(peer, &ar->peers, list) {
			for (i = 0; i < ARRAY_SIZE(peer->keys); i++)
				if (peer->keys[i] == key) {
					memcpy(addr, peer->addr, ETH_ALEN);
					peer->keys[i] = NULL;
					break;
				}
			if (i < ARRAY_SIZE(peer->keys))
				break;
		}
		spin_unlock_bh(&ar->data_lock);

		if (i == ARRAY_SIZE(peer->keys))
			break;

		ret = ath10k_install_key(arvif, key, DISABLE_KEY, addr);
		if (ret && first_errno == 0)
			first_errno = ret;

		if (ret)
			ath10k_warn("could not remove key for %pM\n", addr);
	}

	return first_errno;
}


/*********************/
/* General utilities */
/*********************/

static inline enum wmi_phy_mode chan_to_phymode(struct ieee80211_channel *chan,
					enum nl80211_channel_type channel_type)
{
	enum wmi_phy_mode phymode = MODE_UNKNOWN;

	switch (chan->band) {
	case IEEE80211_BAND_2GHZ:
		switch (channel_type) {
		case NL80211_CHAN_NO_HT:
			phymode = MODE_11G;
			break;
		case NL80211_CHAN_HT20:
			phymode = MODE_11NG_HT20;
			break;
		case NL80211_CHAN_HT40PLUS:
		case NL80211_CHAN_HT40MINUS:
			phymode = MODE_11NG_HT40;
			break;
		}
		break;
	case IEEE80211_BAND_5GHZ:
		switch (channel_type) {
		case NL80211_CHAN_NO_HT:
			phymode = MODE_11A;
			break;
		case NL80211_CHAN_HT20:
			phymode = MODE_11NA_HT20;
			break;
		case NL80211_CHAN_HT40PLUS:
		case NL80211_CHAN_HT40MINUS:
			phymode = MODE_11NA_HT40;
			break;
		}
		break;
	default:
		break;
	}

	WARN_ON(phymode == MODE_UNKNOWN);
	return phymode;
}

static int band_center_freq(struct ieee80211_channel *chan,
			    enum nl80211_channel_type channel_type)
{
	u32 center_freq = chan->center_freq;

	switch (channel_type) {
	case NL80211_CHAN_HT40PLUS:
		center_freq = chan->center_freq + 10;
		break;
	case NL80211_CHAN_HT40MINUS:
		center_freq = chan->center_freq - 10;
		break;
	default:
		break;
	}

	return center_freq;
}

static u8 ath10k_parse_mpdudensity(u8 mpdudensity)
{
/*
 * 802.11n D2.0 defined values for "Minimum MPDU Start Spacing":
 *   0 for no restriction
 *   1 for 1/4 us
 *   2 for 1/2 us
 *   3 for 1 us
 *   4 for 2 us
 *   5 for 4 us
 *   6 for 8 us
 *   7 for 16 us
 */
	switch (mpdudensity) {
	case 0:
		return 0;
	case 1:
	case 2:
	case 3:
	/* Our lower layer calculations limit our precision to
	   1 microsecond */
		return 1;
	case 4:
		return 2;
	case 5:
		return 4;
	case 6:
		return 8;
	case 7:
		return 16;
	default:
		return 0;
	}
}

static int ath10k_peer_create(struct ath10k *ar, u32 vdev_id, const u8 *addr)
{
	int ret;

	lockdep_assert_held(&ar->conf_mutex);

	ret = wmi_peer_create(ar, vdev_id, addr);
	if (ret)
		return ret;

	ret = ath10k_wait_for_peer_created(ar, vdev_id, addr);
	if (ret)
		return ret;

	return 0;
}

static int ath10k_peer_delete(struct ath10k *ar, u32 vdev_id, const u8 *addr)
{
	int ret;

	lockdep_assert_held(&ar->conf_mutex);

	ret = wmi_peer_delete(ar, vdev_id, addr);
	if (ret)
		return ret;

	ret = ath10k_wait_for_peer_deleted(ar, vdev_id, addr);
	if (ret)
		return ret;

	return 0;
}

/************************/
/* Interface management */
/************************/

static inline int ath10k_vdev_setup_sync(struct ath10k *ar)
{
	int ret;

	ret = wait_for_completion_timeout(&ar->vdev_setup_done,
					  ATH10K_VDEV_SETUP_TIMEOUT_HZ);
	if (ret == 0)
		ret = -ETIMEDOUT;

	if (ret > 0)
		ret = 0; /* success */

	return ret;
}

static int ath10k_vdev_start(struct ath10k_vif *arvif)
{
	struct ath10k *ar = arvif->ar;
	struct ieee80211_conf *conf = &ar->hw->conf;
	struct ieee80211_channel *channel = conf->channel;
	struct wmi_vdev_start_request_arg arg = {
		.vdev_id = arvif->vdev_id,
		.channel = {
			.freq = channel->center_freq,
			.band_center_freq1 = band_center_freq(conf->channel,
							      conf->channel_type),
			.mode = chan_to_phymode(conf->channel,
						conf->channel_type),
			.min_power = channel->max_power * 3,
			.max_power = channel->max_power * 4,
			.max_reg_power = channel->max_reg_power * 4,
			.max_antenna_gain = channel->max_antenna_gain,
		},
		.bcn_intval = arvif->beacon_interval,
		.dtim_period = arvif->dtim_period,
	};
	int ret = 0;

	mutex_lock(&ar->vdev_mtx);

	INIT_COMPLETION(ar->vdev_setup_done);

	if (arvif->vdev_type == WMI_VDEV_TYPE_AP) {
		arg.ssid = arvif->u.ap.ssid;
		arg.ssid_len = arvif->u.ap.ssid_len;
		arg.hidden_ssid = arvif->u.ap.hidden_ssid;
	} else if (arvif->vdev_type == WMI_VDEV_TYPE_IBSS) {
		/* TODO: IBSS ssid and ssid_len is needed from mac80211
		 * small patch in mac80211 is needed
		 */
	}

	ret = wmi_vdev_start(ar, &arg);
	if (ret) {
		ath10k_warn("WMI vdev start failed: ret %d\n", ret);
		goto unlock;
	}

	ret = ath10k_vdev_setup_sync(ar);
	if (ret) {
		ath10k_warn("vdev setup failed %d\n", ret);
		goto unlock;
	}

unlock:
	mutex_unlock(&ar->vdev_mtx);
	return ret;
}

static int ath10k_vdev_stop(struct ath10k_vif *arvif)
{
	struct ath10k *ar = arvif->ar;
	int ret;

	mutex_lock(&ar->vdev_mtx);

	INIT_COMPLETION(ar->vdev_setup_done);

	ret = wmi_vdev_stop(ar, arvif->vdev_id);
	if (ret) {
		ath10k_warn("WMI vdev stop failed: ret %d\n", ret);
		goto unlock;
	}

	ret = ath10k_vdev_setup_sync(ar);
	if (ret) {
		ath10k_warn("vdev setup failed %d\n", ret);
		goto unlock;
	}

unlock:
	mutex_unlock(&ar->vdev_mtx);
	return ret;
}

static int ath10k_monitor_start(struct ath10k *ar, int vdev_id)
{
	struct ieee80211_channel *channel = ar->hw->conf.channel;
	struct ieee80211_conf *conf = &ar->hw->conf;
	struct wmi_vdev_start_request_arg arg = {
		.vdev_id = vdev_id,
		.channel = {
			.freq = channel->center_freq,
			.band_center_freq1 = band_center_freq(conf->channel,
							      conf->channel_type),
			/* TODO setup this dynamically, what in case we
			   don't have any vifs? */
			.mode = chan_to_phymode(conf->channel,
						conf->channel_type),
			.min_power = channel->max_power * 3,
			.max_power = channel->max_power * 4,
			.max_reg_power = channel->max_reg_power * 4,
			.max_antenna_gain = channel->max_antenna_gain,
		},
	};
	int ret = 0;

	lockdep_assert_held(&ar->vdev_mtx);

	ret = wmi_vdev_start(ar, &arg);
	if (ret) {
		ath10k_warn("Monitor vdev start failed: ret %d\n", ret);
		return ret;
	}

	ret = ath10k_vdev_setup_sync(ar);
	if (ret) {
		ath10k_warn("Monitor vdev setup failed %d\n", ret);
		return ret;
	}

	ret = wmi_vdev_up(ar, vdev_id, 0, ar->mac_addr);
	if (ret) {
		ath10k_warn("Monitor vdev up failed: %d\n", ret);
		goto vdev_stop;
	}

	ar->monitor_vdev_id = vdev_id;
	ar->monitor_enabled = true;

	return 0;

vdev_stop:
	ret = wmi_vdev_stop(ar, ar->monitor_vdev_id);
	if (ret)
		ath10k_warn("Monitor vdev stop failed: %d\n", ret);

	return ret;
}

static int ath10k_monitor_stop(struct ath10k *ar)
{
	int ret = 0;

	lockdep_assert_held(&ar->vdev_mtx);

	/* For some reasons, wmi_vdev_down() here couse
	 * often wmi_vdev_stop() to fail. Next we could
	 * not run monitor vdev and driver reload
	 * required. Don't see such problems we skip
	 * wmi_vdev_down() here.
	 */

	ret = wmi_vdev_stop(ar, ar->monitor_vdev_id);
	if (ret)
		ath10k_warn("Monitor vdev stop failed: %d\n", ret);

	ret = ath10k_vdev_setup_sync(ar);
	if (ret)
		ath10k_warn("Monitor_down sync failed: %d\n", ret);

	ar->monitor_enabled = false;
	return ret;
}

static int ath10k_monitor_create(struct ath10k *ar)
{
	int bit, ret = 0;

	mutex_lock(&ar->vdev_mtx);

	if (ar->monitor_present) {
		ath10k_warn("Monitor mode already enabled\n");
		goto unlock;
	}

	bit = ffs(ar->free_vdev_map);
	if (bit == 0) {
		ath10k_warn("No free VDEV slots\n");
		ret = -ENOMEM;
		goto unlock;
	}

	ar->monitor_vdev_id = bit - 1;
	ar->free_vdev_map &= ~(1 << ar->monitor_vdev_id);

	ret = wmi_vdev_create(ar, ar->monitor_vdev_id,
			      WMI_VDEV_TYPE_MONITOR,
			      0, ar->mac_addr);
	if (ret) {
		ath10k_warn("WMI vdev monitor create failed: ret %d\n", ret);
		goto vdev_fail;
	}

	ath10k_dbg(ATH10K_DBG_MAC, "Monitor interface created, vdev id: %d\n",
		   ar->monitor_vdev_id);

	ar->monitor_present = true;
	goto unlock;

vdev_fail:
	/*
	 * Restore the ID to the global map.
	 */
	ar->free_vdev_map |= 1 << (ar->monitor_vdev_id);
unlock:
	mutex_unlock(&ar->vdev_mtx);
	return ret;
}

static int ath10k_monitor_destroy(struct ath10k *ar)
{
	int ret = 0;

	mutex_lock(&ar->vdev_mtx);

	if (!ar->monitor_present)
		goto unlock;

	ret = wmi_vdev_delete(ar, ar->monitor_vdev_id);
	if (ret) {
		ath10k_warn("WMI vdev monitor delete failed: %d\n", ret);
		goto unlock;
	}

	ar->free_vdev_map |= 1 << (ar->monitor_vdev_id);
	ar->monitor_present = false;

	ath10k_dbg(ATH10K_DBG_MAC, "Monitor interface destroyed, vdev id: %d\n",
		   ar->monitor_vdev_id);
unlock:
	mutex_unlock(&ar->vdev_mtx);
	return ret;
}

static void ath10k_control_beaconing(struct ath10k_vif *arvif,
				struct ieee80211_bss_conf *info)
{
	int ret = 0;

	if (!info->enable_beacon) {
		ath10k_vdev_stop(arvif);
		return;
	}

	arvif->tx_seq_no = 0x1000;

	ret = ath10k_vdev_start(arvif);
	if (ret)
		return;

	ret = wmi_vdev_up(arvif->ar, arvif->vdev_id, 0, info->bssid);
	if (ret) {
		ath10k_warn("Failed to bring up VDEV: %d\n",
			    arvif->vdev_id);
		return;
	}
	ath10k_dbg(ATH10K_DBG_MAC, "VDEV: %d up\n", arvif->vdev_id);
}

static void ath10k_control_ibss(struct ath10k_vif *arvif,
				struct ieee80211_bss_conf *info,
				const u8 self_peer[ETH_ALEN])
{
	int ret = 0;

	if (!info->ibss_joined) {
		ret = ath10k_peer_delete(arvif->ar, arvif->vdev_id, self_peer);
		if (ret)
			ath10k_warn("Failed to delete IBSS self peer:%pM "
				    "for VDEV:%d ret:%d\n", self_peer,
				    arvif->vdev_id, ret);

		if (is_zero_ether_addr(arvif->u.ibss.bssid))
			return;

		ret = ath10k_peer_delete(arvif->ar, arvif->vdev_id,
					 arvif->u.ibss.bssid);
		if (ret) {
			ath10k_warn("Failed to delete IBSS BSSID peer:%pM "
				    "for VDEV:%d ret:%d\n", arvif->u.ibss.bssid,
				     arvif->vdev_id, ret);
			return;
		}

		memset(arvif->u.ibss.bssid, 0, ETH_ALEN);

		return;
	}

	ret = ath10k_peer_create(arvif->ar, arvif->vdev_id, self_peer);
	if (ret) {
		ath10k_warn("Failed to create IBSS self peer:%pM for VDEV:%d"
			    "ret:%d\n", self_peer, arvif->vdev_id, ret);
		return;
	}

	ret = wmi_vdev_set_param(arvif->ar, arvif->vdev_id,
				 WMI_VDEV_PARAM_ATIM_WINDOW,
				 ATH10K_DEFAULT_ATIM);
	if (ret)
		ath10k_warn("Failed to set IBSS ATIM for VDEV:%d ret:%d\n",
			    arvif->vdev_id, ret);
}

/*
 * Review this when mac80211 gains per-interface powersave support.
 */
static void ath10k_config_ps_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct ath10k_generic_iter *ar_iter = data;
	struct ieee80211_conf *conf = &ar_iter->ar->hw->conf;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	enum wmi_sta_ps_mode psmode;

	if (vif->type != NL80211_IFTYPE_STATION)
		return;

	if (conf->flags & IEEE80211_CONF_PS) {
		psmode = WMI_STA_PS_MODE_ENABLED;
		ar_iter->ret = wmi_set_sta_ps_param(ar_iter->ar, arvif->vdev_id,
					    WMI_STA_PS_PARAM_INACTIVITY_TIME,
					    conf->dynamic_ps_timeout);
		if (ar_iter->ret) {
			ath10k_warn("Failed to set inactivity time for VDEV: %d\n",
				    arvif->vdev_id);
			return;
		}
	} else
		psmode = WMI_STA_PS_MODE_DISABLED;

	ar_iter->ret = wmi_set_psmode(ar_iter->ar, arvif->vdev_id, psmode);
	if (ar_iter->ret)
		ath10k_warn("Failed to set PS Mode: %d for VDEV: %d\n",
			    psmode, arvif->vdev_id);
	else
		ath10k_dbg(ATH10K_DBG_MAC, "Set PS Mode: %d for VDEV: %d\n",
			   psmode, arvif->vdev_id);
}

/**********************/
/* Station management */
/**********************/

static void ath10k_peer_assoc_h_basic(struct ath10k *ar,
				      struct ath10k_vif *arvif,
				      struct ieee80211_sta *sta,
				      struct ieee80211_bss_conf *bss_conf,
				      struct wmi_peer_assoc_complete_arg *arg)
{
	memcpy(arg->addr, sta->addr, ETH_ALEN);
	arg->vdev_id = arvif->vdev_id;
	arg->peer_aid = sta->aid;
	arg->peer_flags |= WMI_PEER_AUTH;

	if (arvif->vdev_type == WMI_VDEV_TYPE_STA)
		/*
		 * Seems FW have problems with Power Save in STA
		 * mode when we setup this parameter to high (eg. 5).
		 * Often we see that FW don't send NULL (with clean P flags)
		 * frame even there is info about buffered frames in beacons.
		 * Sometimes we have to wait more than 10 seconds before FW
		 * will wakeup. Often sending one ping from AP to our device
		 * just fail (more than 50%).
		 *
		 * Seems setting this FW parameter to 1 couse FW
		 * will check every beacon and will wakup immediately
		 * after detection buffered data.
		 */
		arg->peer_listen_intval = 1;
	else
		arg->peer_listen_intval = ar->hw->conf.listen_interval;

	arg->peer_num_spatial_streams = 1;

	/*
	 * The assoc capabilities are available only in managed mode.
	 */
	if (arvif->vdev_type == WMI_VDEV_TYPE_STA && bss_conf)
		arg->peer_caps = bss_conf->assoc_capability;
}

static void ath10k_peer_assoc_h_crypto(struct ath10k *ar,
				       struct ath10k_vif *arvif,
				       struct wmi_peer_assoc_complete_arg *arg)
{
	struct ieee80211_vif *vif = arvif->vif;
	struct ieee80211_bss_conf *info = &vif->bss_conf;
	struct cfg80211_bss *bss;
	const u8 *rsnie = NULL;
	const u8 *wpaie = NULL;

	bss = cfg80211_get_bss(ar->hw->wiphy, ar->hw->conf.channel,
			       info->bssid, NULL, 0, 0, 0);
	if (bss) {
		const struct cfg80211_bss_ies *ies;

		rcu_read_lock();
		rsnie = ieee80211_bss_get_ie(bss, WLAN_EID_RSN);

		ies = rcu_dereference(bss->ies);

		wpaie = cfg80211_find_vendor_ie(WLAN_OUI_MICROSOFT,
				WLAN_OUI_TYPE_MICROSOFT_WPA,
				ies->data,
				ies->len);
		rcu_read_unlock();
		cfg80211_put_bss(ar->hw->wiphy, bss);
	}

	/* FIXME: base on RSN IE/WPA IE is a correct idea? */
	if (rsnie || wpaie) {
		ath10k_dbg(ATH10K_DBG_WMI, "%s: rsn ie found\n", __func__);
		arg->peer_flags |= WMI_PEER_NEED_PTK_4_WAY;
	}

	if (wpaie) {
		ath10k_dbg(ATH10K_DBG_WMI, "%s: wpa ie found\n", __func__);
		arg->peer_flags |= WMI_PEER_NEED_GTK_2_WAY;
	}
}

static void ath10k_peer_assoc_h_rates(struct ath10k *ar,
				      struct ieee80211_sta *sta,
				      struct wmi_peer_assoc_complete_arg *arg)
{
	struct wmi_rate_set_arg *rateset = &arg->peer_legacy_rates;
	const struct ieee80211_supported_band *sband;
	const struct ieee80211_rate *rates;
	u32 ratemask;
	int i;

	sband = ar->hw->wiphy->bands[ar->hw->conf.channel->band];
	ratemask = sta->supp_rates[ar->hw->conf.channel->band];
	rates = sband->bitrates;

	rateset->num_rates = 0;

	for (i = 0; i < 32; i++, ratemask >>= 1, rates++) {
		if (!(ratemask & 1))
			continue;

		rateset->rates[rateset->num_rates] = rates->hw_value;
		rateset->num_rates++;
	}
}

static void ath10k_peer_assoc_h_ht(struct ath10k *ar,
				   struct ieee80211_sta *sta,
				   struct wmi_peer_assoc_complete_arg *arg)
{
	const struct ieee80211_sta_ht_cap *ht_cap = &sta->ht_cap;
	int smps;
	int i, n;

	if (!ht_cap->ht_supported)
		return;

	arg->peer_flags |= WMI_PEER_HT;
	arg->peer_max_mpdu = (1 << (IEEE80211_HT_MAX_AMPDU_FACTOR +
				    ht_cap->ampdu_factor)) - 1;
	arg->peer_mpdu_density = ath10k_parse_mpdudensity(ht_cap->ampdu_density);
	arg->peer_ht_caps = ht_cap->cap;
	arg->peer_rate_caps |= WMI_RC_HT_FLAG;

	if (ht_cap->cap & IEEE80211_HT_CAP_LDPC_CODING)
		arg->peer_flags |= WMI_PEER_LDPC;

	if (sta->bandwidth >= IEEE80211_STA_RX_BW_40) {
		arg->peer_flags |= WMI_PEER_40MHZ;
		arg->peer_rate_caps |= WMI_RC_CW40_FLAG;
	}

	if (ht_cap->cap & IEEE80211_HT_CAP_SGI_20)
		arg->peer_rate_caps |= WMI_RC_SGI_FLAG;

	if (ht_cap->cap & IEEE80211_HT_CAP_SGI_40)
		arg->peer_rate_caps |= WMI_RC_SGI_FLAG;

	if (ht_cap->cap & IEEE80211_HT_CAP_TX_STBC) {
		arg->peer_rate_caps |= WMI_RC_TX_STBC_FLAG;
		arg->peer_flags |= WMI_PEER_STBC;
	}

	if (ht_cap->cap & IEEE80211_HT_CAP_RX_STBC) {
		u32 stbc;
		stbc = ht_cap->cap & IEEE80211_HT_CAP_RX_STBC;
		stbc = stbc >> IEEE80211_HT_CAP_RX_STBC_SHIFT;
		stbc = stbc << WMI_RC_RX_STBC_FLAG_S;
		arg->peer_rate_caps |= stbc;
		arg->peer_flags |= WMI_PEER_STBC;
	}

	smps = ht_cap->cap & IEEE80211_HT_CAP_SM_PS;
	smps >>= IEEE80211_HT_CAP_SM_PS_SHIFT;

	if (smps == WLAN_HT_CAP_SM_PS_STATIC) {
		arg->peer_flags |= WMI_PEER_SPATIAL_MUX;
		arg->peer_flags |= WMI_PEER_STATIC_MIMOPS;
	} else if (smps == WLAN_HT_CAP_SM_PS_DYNAMIC) {
		arg->peer_flags |= WMI_PEER_SPATIAL_MUX;
		arg->peer_flags |= WMI_PEER_DYN_MIMOPS;
	}

	if (ht_cap->mcs.rx_mask[1] && ht_cap->mcs.rx_mask[2])
		arg->peer_rate_caps |= WMI_RC_TS_FLAG;
	else if (ht_cap->mcs.rx_mask[1])
		arg->peer_rate_caps |= WMI_RC_DS_FLAG;

	for (i = 0, n = 0; i < IEEE80211_HT_MCS_MASK_LEN*8; i++)
		if (ht_cap->mcs.rx_mask[i/8] & (1 << i%8))
			arg->peer_ht_rates.rates[n++] = i;

	arg->peer_ht_rates.num_rates = n;
	arg->peer_num_spatial_streams = max((n+7) / 8, 1);

	ath10k_dbg(ATH10K_DBG_MAC, "mcs cnt %d nss %d\n",
		   arg->peer_ht_rates.num_rates,
		   arg->peer_num_spatial_streams);
}

/*
 * FIXME: Handle UAPSD later.
 */
static void ath10k_peer_assoc_h_qos(struct ath10k *ar,
				    struct ath10k_vif *arvif,
				    struct ieee80211_sta *sta,
				    struct ieee80211_bss_conf *bss_conf,
				    struct wmi_peer_assoc_complete_arg *arg)
{
	if (arvif->vdev_type == WMI_VDEV_TYPE_AP) {
		if (sta->wme)
			arg->peer_flags |= WMI_PEER_QOS;
	} else if (arvif->vdev_type == WMI_VDEV_TYPE_STA) {
		if (bss_conf->qos)
			arg->peer_flags |= WMI_PEER_QOS;
	}
}

static void ath10k_peer_assoc_h_phymode(struct ath10k *ar,
					struct ath10k_vif *arvif,
					struct ieee80211_sta *sta,
					struct wmi_peer_assoc_complete_arg *arg)
{
	enum wmi_phy_mode phymode = MODE_UNKNOWN;

	switch (ar->hw->conf.channel->band) {
	case IEEE80211_BAND_2GHZ:
		if (sta->ht_cap.ht_supported) {
			if (sta->bandwidth == IEEE80211_STA_RX_BW_40)
				phymode = MODE_11NG_HT40;
			else
				phymode = MODE_11NG_HT20;
		} else
			phymode = MODE_11G;
		break;
	case IEEE80211_BAND_5GHZ:
		if (sta->ht_cap.ht_supported) {
			if (sta->bandwidth == IEEE80211_STA_RX_BW_40)
				phymode = MODE_11NA_HT40;
			else
				phymode = MODE_11NA_HT20;
		} else
			phymode = MODE_11A;
		break;
	default:
		break;
	}

	arg->peer_phymode = phymode;
	WARN_ON(phymode == MODE_UNKNOWN);
}

static int ath10k_peer_assoc(struct ath10k *ar,
			     struct ath10k_vif *arvif,
			     struct ieee80211_sta *sta,
			     struct ieee80211_bss_conf *bss_conf)
{
	struct wmi_peer_assoc_complete_arg arg;

	memset(&arg, 0, sizeof(struct wmi_peer_assoc_complete_arg));

	ath10k_peer_assoc_h_basic(ar, arvif, sta, bss_conf, &arg);
	ath10k_peer_assoc_h_crypto(ar, arvif, &arg);
	ath10k_peer_assoc_h_rates(ar, sta, &arg);
	ath10k_peer_assoc_h_ht(ar, sta, &arg);
	ath10k_peer_assoc_h_qos(ar, arvif, sta, bss_conf, &arg);
	ath10k_peer_assoc_h_phymode(ar, arvif, sta, &arg);

	return wmi_peer_assoc(ar, &arg);
}

/* can be called only in mac80211 callbacks due to `key_count` usage */
static void ath10k_bss_assoc(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif,
			     struct ieee80211_bss_conf *bss_conf)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	struct ieee80211_sta *ap_sta;
	int ret;

	rcu_read_lock();

	ap_sta = ieee80211_find_sta(vif, bss_conf->bssid);
	if (!ap_sta) {
		ath10k_warn("Failed to find station entry for %pM\n",
			    bss_conf->bssid);
		rcu_read_unlock();
		return;
	}

	ret = ath10k_peer_assoc(ar, arvif, ap_sta, bss_conf);
	if (ret) {
		ath10k_warn("Peer assoc failed for %pM\n", bss_conf->bssid);
		rcu_read_unlock();
		return;
	}

	rcu_read_unlock();

	ret = wmi_vdev_up(ar, arvif->vdev_id, bss_conf->aid, bss_conf->bssid);
	if (ret)
		ath10k_warn("VDEV: %d up failed: ret %d\n",
			    arvif->vdev_id, ret);
	else
		ath10k_dbg(ATH10K_DBG_MAC,
			   "VDEV: %d associated, BSSID: %pM, AID: %d\n",
			   arvif->vdev_id, bss_conf->bssid, bss_conf->aid);
}

/*
 * FIXME: flush TIDs
 */
static void ath10k_bss_disassoc(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	int ret;

	/*
	 * For some reason, calling VDEV-DOWN before VDEV-STOP
	 * makes the FW to send frames via HTT after disassociation.
	 * No idea why this happens, even though VDEV-DOWN is supposed
	 * to be analogous to link down, so just stop the VDEV.
	 */
	ret = ath10k_vdev_stop(arvif);
	if (!ret)
		ath10k_dbg(ATH10K_DBG_MAC, "VDEV: %d stopped\n",
			   arvif->vdev_id);

	/*
	 * If we don't call VDEV-DOWN after VDEV-STOP FW will remain active and
	 * report beacons from previously associated network through HTT.
	 * This in turn would spam mac80211 WARN_ON if we bring down all
	 * interfaces as it expects there is no rx when no interface is
	 * running.
	 */
	ret = wmi_vdev_down(ar, arvif->vdev_id);
	if (ret)
		ath10k_dbg(ATH10K_DBG_MAC, "VDEV: %d wmi_vdev_down failed (%d)\n",
			   arvif->vdev_id, ret);

	wmi_flush_tx(ar);

	arvif->def_wep_key_index = 0;
}

static int ath10k_station_assoc(struct ath10k *ar, struct ath10k_vif *arvif,
				struct ieee80211_sta *sta)
{
	int ret = 0;

	ret = ath10k_peer_assoc(ar, arvif, sta, NULL);
	if (ret) {
		ath10k_warn("WMI peer assoc failed for %pM\n", sta->addr);
		return ret;
	}

	ret = ath10k_install_peer_wep_keys(arvif, sta->addr);
	if (ret) {
		ath10k_warn("could not install peer wep keys (%d)\n", ret);
		return ret;
	}

	return ret;
}

static int ath10k_station_disassoc(struct ath10k *ar, struct ath10k_vif *arvif,
				   struct ieee80211_sta *sta)
{
	int ret = 0;

	ret = ath10k_clear_peer_keys(arvif, sta->addr);
	if (ret) {
		ath10k_warn("could not clear all peer wep keys (%d)\n", ret);
		return ret;
	}

	return ret;
}

/**************/
/* Regulatory */
/**************/

static void ath10k_update_channel_list(struct ath10k *ar)
{
	struct ieee80211_hw *hw = ar->hw;
	struct ieee80211_supported_band **bands;
	enum ieee80211_band band;
	struct ieee80211_channel *channel;
	struct wmi_scan_chan_list_arg arg = {0};
	struct wmi_channel_arg *ch;
	int len;
	int i;

	bands = hw->wiphy->bands;
	for (band = 0; band < IEEE80211_NUM_BANDS; band++) {
		if (!bands[band])
			continue;

		for (i = 0; i < bands[band]->n_channels; i++) {
			if (bands[band]->channels[i].flags & IEEE80211_CHAN_DISABLED)
				continue;

			arg.n_channels++;
		}
	}

	len = sizeof(struct wmi_channel_arg) * arg.n_channels;
	arg.channels = kzalloc(len, GFP_KERNEL);
	if (!arg.channels) {
		ath10k_warn("%s alloc failed\n", __func__);
		return;
	}

	ch = arg.channels;
	for (band = 0; band < IEEE80211_NUM_BANDS; band++) {
		if (!bands[band])
			continue;

		for (i = 0; i < bands[band]->n_channels; i++) {
			channel = &bands[band]->channels[i];

			if (channel->flags & IEEE80211_CHAN_DISABLED)
				continue;

			ch->allow_ht   = true;
			ch->allow_ibss = !(channel->flags & IEEE80211_CHAN_NO_IBSS);
			ch->ht40plus   = !(channel->flags & IEEE80211_CHAN_NO_HT40PLUS);
			ch->passive    = !!(channel->flags & IEEE80211_CHAN_PASSIVE_SCAN);

			ch->freq             = channel->center_freq;
			ch->min_power        = channel->max_power * 3;
			ch->max_power        = channel->max_power * 4;
			ch->max_reg_power    = channel->max_reg_power * 4;
			ch->max_antenna_gain = channel->max_antenna_gain;
			ch->reg_class_id     = 0; /* FIXME */
			ch->mode             = chan_to_phymode(channel, NL80211_CHAN_NO_HT);

			if (WARN_ON_ONCE(ch->mode == MODE_UNKNOWN))
				continue;

			ath10k_dbg(ATH10K_DBG_WMI, "%s: [%zd/%d] freq %d "
				   "maxpower %d regpower %d antenna %d "
				   "mode %d\n",
				   __func__, ch - arg.channels, arg.n_channels,
				   ch->freq, ch->max_power, ch->max_reg_power,
				   ch->max_antenna_gain, ch->mode);

			ch++;
		}
	}

	wmi_scan_chan_list(ar, &arg);
	kfree(arg.channels);
}

static void ath10k_reg_notifier(struct wiphy *wiphy,
				struct regulatory_request *request)
{
	struct ieee80211_hw *hw = wiphy_to_ieee80211_hw(wiphy);
	struct ath10k *ar = hw->priv;

	ath_reg_notifier_apply(wiphy, request, &ath10k_common(ar)->regulatory);
	ath10k_update_channel_list(ar);
}

/***************/
/* TX handlers */
/***************/

static void ath10k_tx_h_qos_workaround(struct ieee80211_hw *hw,
				       struct ieee80211_tx_control *control,
				       struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (void *)skb->data;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	u8 *qos_ctl;

	/*
	 * FIXME: we don't know the AP mode behaviour yet
	 *        this has been tested with STA mode so far
	 *        for AP mode control->sta->wme should be used probably
	 */

	if (!control->sta)
		return;

	if (!info->control.vif)
		return;

	if (info->control.vif->type != NL80211_IFTYPE_STATION &&
	    info->control.vif->type != NL80211_IFTYPE_ADHOC)
		return;

	if (!info->control.vif->bss_conf.qos)
		return;

	/* FW adds QoS Control even to data frames.. */
	if (WARN_ON_ONCE(ieee80211_is_data(hdr->frame_control) &&
			!ieee80211_is_data_qos(hdr->frame_control)))
		return;

	if (!ieee80211_is_data_qos(hdr->frame_control))
		return;

	/*
	 * FW append QoS Control on its own,
	 * so we need to get rid of the one mac80211 is * generating
	 */
	qos_ctl = ieee80211_get_qos_ctl(hdr);
	memmove(qos_ctl, qos_ctl + IEEE80211_QOS_CTL_LEN,
		skb->len - (qos_ctl + IEEE80211_QOS_CTL_LEN - skb->data));
	skb_trim(skb, skb->len - IEEE80211_QOS_CTL_LEN);
}

static void ath10k_tx_h_update_wep_key(struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_vif *vif = info->control.vif;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	struct ath10k *ar = arvif->ar;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_key_conf *key = info->control.hw_key;
	int ret;

	/* TODO AP mode should be implemented */
	if (vif->type != NL80211_IFTYPE_STATION)
		return;

	if (!ieee80211_has_protected(hdr->frame_control))
		return;

	if (!key)
		return;

	if (key->cipher != WLAN_CIPHER_SUITE_WEP40 &&
	    key->cipher != WLAN_CIPHER_SUITE_WEP104)
		return;

	if (key->keyidx == arvif->def_wep_key_index)
		return;

	ath10k_dbg(ATH10K_DBG_MAC, "new wep keyidx will be %d\n", key->keyidx);

	ret = wmi_vdev_set_param(ar, arvif->vdev_id, WMI_VDEV_PARAM_DEF_KEYID,
				 key->keyidx);
	if (!ret)
		arvif->def_wep_key_index = key->keyidx;
	else
		ath10k_warn("could not update wep keyidx (%d)\n", ret);
}

static void ath10k_tx_h_add_p2p_noa_ie(struct ath10k *ar, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_vif *vif = info->control.vif;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);

	/* This is case only for P2P_GO */
	if (arvif->vdev_type != WMI_VDEV_TYPE_AP ||
	    arvif->vdev_subtype != WMI_VDEV_SUBTYPE_P2P_GO)
		return;

	if (unlikely(ieee80211_is_probe_resp(hdr->frame_control))) {
		spin_lock_bh(&ar->data_lock);
		if (arvif->u.ap.noa_data)
			if (!pskb_expand_head(skb, 0, arvif->u.ap.noa_len, GFP_ATOMIC))
				memcpy(skb_put(skb, arvif->u.ap.noa_len),
				       arvif->u.ap.noa_data,
				       arvif->u.ap.noa_len);
		spin_unlock_bh(&ar->data_lock);
	}
}

static void ath10k_tx_htt(struct ath10k *ar, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	int ret;

	if (ieee80211_is_mgmt(hdr->frame_control))
		ret = htt_mgmt_tx(ar->htt, skb);
	else
		ret = htt_tx(ar->htt, skb);

	if (ret) {
		ath10k_warn("tx failed (%d). dropping packet.\n", ret);
		ieee80211_free_txskb(ar->hw, skb);
	}
}

void ath10k_offchan_tx_purge(struct ath10k *ar)
{
	struct sk_buff *skb;

	for (;;) {
		skb = skb_dequeue(&ar->offchan_tx_queue);
		if (!skb)
			break;

		ieee80211_free_txskb(ar->hw, skb);
	}
}

void ath10k_offchan_tx_work(struct work_struct *work)
{
	struct ath10k *ar = container_of(work, struct ath10k, offchan_tx_work);
	struct ath10k_peer *peer;
	struct ieee80211_hdr *hdr;
	struct sk_buff *skb;
	const u8 *peer_addr;
	int vdev_id;
	int ret;

	/* FW requirement: We must create a peer before FW will send out
	 * an offchannel frame. Otherwise the frame will be stuck and
	 * never transmitted. We delete the peer upon tx completion.
	 * It is unlikely that a peer for offchannel tx will already be
	 * present. However it may be in some rare cases so account for that.
	 * Otherwise we might remove a legitimate peer and break stuff. */

	for (;;) {
		skb = skb_dequeue(&ar->offchan_tx_queue);
		if (!skb)
			break;

		mutex_lock(&ar->conf_mutex);

		ath10k_dbg(ATH10K_DBG_MAC, "processing offchannel skb %p\n", skb);

		hdr = (struct ieee80211_hdr *)skb->data;
		peer_addr = ieee80211_get_DA(hdr);
		vdev_id = ATH10K_SKB_CB(skb)->htt.vdev_id;

		spin_lock_bh(&ar->data_lock);
		peer = ath10k_peer_find(ar, vdev_id, peer_addr);
		spin_unlock_bh(&ar->data_lock);

		if (peer)
			ath10k_info("peer %pM on vdev %d already present\n",
				    peer_addr, vdev_id);

		if (!peer) {
			ret = ath10k_peer_create(ar, vdev_id, peer_addr);
			if (ret)
				ath10k_warn("peer %pM on vdev %d not created (%d)\n",
					    peer_addr, vdev_id, ret);
		}

		spin_lock_bh(&ar->data_lock);
		INIT_COMPLETION(ar->offchan_tx_completed);
		ar->offchan_tx_skb = skb;
		spin_unlock_bh(&ar->data_lock);

		ath10k_tx_htt(ar, skb);

		ret = wait_for_completion_timeout(&ar->offchan_tx_completed, 3*HZ);
		if (ret <= 0)
			ath10k_warn("timed out waiting for offchannel skb %p\n", skb);

		if (!peer) {
			ret = ath10k_peer_delete(ar, vdev_id, peer_addr);
			if (ret)
				ath10k_warn("peer %pM on vdev %d not deleted (%d)\n",
					    peer_addr, vdev_id, ret);
		}

		mutex_unlock(&ar->conf_mutex);
	}
}

/************/
/* Scanning */
/************/

/*
 * This gets called if we dont get a heart-beat during scan.
 * This may indicate the FW has hung and we need to abort the
 * scan manually to prevent cancel_hw_scan() from deadlocking
 */
void ath10k_reset_scan(unsigned long ptr)
{
	struct ath10k *ar = (struct ath10k *)ptr;

	spin_lock_bh(&ar->scan.lock);

	if (!ar->scan.in_progress) {
		/* lucky! scan must've completed right before timeout */
		spin_unlock_bh(&ar->scan.lock);
		return;
	}

	ath10k_warn("scan timeout. resetting. fw issue?\n");

	if (ar->scan.is_roc)
		ieee80211_remain_on_channel_expired(ar->hw);
	else
		ieee80211_scan_completed(ar->hw, 1 /* aborted */);

	ar->scan.in_progress = false;
	complete_all(&ar->scan.completed);
	spin_unlock_bh(&ar->scan.lock);
}

static void ath10k_abort_scan(struct ath10k *ar)
{
	struct wmi_stop_scan_arg arg = {
		.req_id = 1, /* FIXME */
		.req_type = WMI_SCAN_STOP_ONE,
		.u.scan_id = ATH10K_SCAN_ID,
	};
	int ret;

	lockdep_assert_held(&ar->conf_mutex);

	del_timer_sync(&ar->scan.timeout);

	spin_lock_bh(&ar->scan.lock);
	if (!ar->scan.in_progress) {
		spin_unlock_bh(&ar->scan.lock);
		return;
	}

	ar->scan.aborting = true;
	spin_unlock_bh(&ar->scan.lock);

	ret = wmi_stop_scan(ar, &arg);
	if (ret)
		ath10k_warn("%s: wmi_stop_scan failed (%d)\n", __func__, ret);

	ret = wait_for_completion_timeout(&ar->scan.completed, 3*HZ);
	if (ret == 0)
		ret = -ETIMEDOUT;

	spin_lock_bh(&ar->scan.lock);
	if (ar->scan.in_progress) {
		ath10k_warn("%s: could not stop scan (%d)\n", __func__, ret);
		ar->scan.in_progress = false;
		ath10k_offchan_tx_purge(ar);
	}
	spin_unlock_bh(&ar->scan.lock);
}

static int ath10k_start_scan(struct ath10k *ar,
			     const struct wmi_start_scan_arg *arg)
{
	int ret;

	ret = wmi_start_scan(ar, arg);
	if (ret)
		goto abort;

	/* make sure we submit the command so the completion
	* timeout makes sense */
	wmi_flush_tx(ar);

	ret = wait_for_completion_timeout(&ar->scan.started, 1*HZ);
	if (ret == 0)
		ret = -ETIMEDOUT;
	if (ret < 0)
		goto abort;

	/* the scan can complete earlier, before we even
	 * start the timer. in that case the timer handler
	 * checks ar->scan.in_progress and bails out if its
	 * false. */
	mod_timer(&ar->scan.timeout, jiffies + (arg->max_scan_time*HZ)/1000);
	return 0;
abort:
	spin_lock_bh(&ar->scan.lock);
	ar->scan.in_progress = false;
	spin_unlock_bh(&ar->scan.lock);
	return ret;
}

/**********************/
/* mac80211 callbacks */
/**********************/

static void ath10k_tx(struct ieee80211_hw *hw,
		      struct ieee80211_tx_control *control,
		      struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = NULL;
	u32 vdev_id = 0;

	if (info->control.vif) {
		arvif = ath10k_vif_to_arvif(info->control.vif);
		vdev_id = arvif->vdev_id;
	} else if (ar->monitor_enabled)
		vdev_id = ar->monitor_vdev_id;

	/* We should disable CCK RATE due to P2P */
	if (info->flags & IEEE80211_TX_CTL_NO_CCK_RATE)
		ath10k_dbg(ATH10K_DBG_MAC, "IEEE80211_TX_CTL_NO_CCK_RATE\n");

	ath10k_tx_h_qos_workaround(hw, control, skb);
	ath10k_tx_h_update_wep_key(skb);
	ath10k_tx_h_add_p2p_noa_ie(ar, skb);
	ath10k_tx_h_seq_no(skb);

	memset(ATH10K_SKB_CB(skb), 0, sizeof(*ATH10K_SKB_CB(skb)));
	ATH10K_SKB_CB(skb)->htt.vdev_id = vdev_id;

	if (info->flags & IEEE80211_TX_CTL_TX_OFFCHAN) {
		spin_lock_bh(&ar->scan.lock);
		ATH10K_SKB_CB(skb)->htt.is_offchan = true;
		ATH10K_SKB_CB(skb)->htt.vdev_id = ar->scan.vdev_id;
		spin_unlock_bh(&ar->scan.lock);

		ath10k_dbg(ATH10K_DBG_MAC, "queued offchannel skb %p\n", skb);

		skb_queue_tail(&ar->offchan_tx_queue, skb);
		ieee80211_queue_work(hw, &ar->offchan_tx_work);
		return;
	}

	ath10k_tx_htt(ar, skb);
}

static int ath10k_start(struct ieee80211_hw *hw)
{
	struct ath10k *ar = hw->priv;
	int ret;

	ret = wmi_pdev_set_param(ar, WMI_PDEV_PARAM_PMF_QOS, 1);
	if (ret)
		ath10k_warn("could not enable WMI_PDEV_PARAM_PMF_QOS (%d)\n", ret);

	return 0;
}

static void ath10k_stop(struct ieee80211_hw *hw)
{
	struct ath10k *ar = hw->priv;

	/* avoid leaks in case FW never confirms scan for offchannel */
	cancel_work_sync(&ar->offchan_tx_work);
	ath10k_offchan_tx_purge(ar);
}

static int ath10k_config(struct ieee80211_hw *hw, u32 changed)
{
	struct ath10k_generic_iter ar_iter;
	struct ath10k *ar = hw->priv;
	struct ieee80211_conf *conf = &hw->conf;
	int ret = 0;

	mutex_lock(&ar->conf_mutex);

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		ath10k_dbg(ATH10K_DBG_MAC, "Config channel %d mhz\n",
			   conf->channel->center_freq);
		rcu_assign_pointer(ar->rx_channel, conf->channel);
	}

	if (changed & IEEE80211_CONF_CHANGE_PS) {
		memset(&ar_iter, 0, sizeof(struct ath10k_generic_iter));
		ar_iter.ar = ar;
		ieee80211_iterate_active_interfaces_atomic(hw,
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    ath10k_config_ps_iter, &ar_iter);
		ret = ar_iter.ret;
	}

	if (changed & IEEE80211_CONF_CHANGE_MONITOR) {
		if (conf->flags & IEEE80211_CONF_MONITOR)
			ret = ath10k_monitor_create(ar);
		else
			ret = ath10k_monitor_destroy(ar);
	}

	mutex_unlock(&ar->conf_mutex);
	return ret;
}

/*
 * TODO:
 * Figure out how to handle WMI_VDEV_SUBTYPE_P2P_DEVICE,
 * because we will send mgmt frames without CCK. This requirement
 * for P2P_FIND/GO_NEG should be handled by checking CCK flag
 * in the TX packet.
 */
static int ath10k_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	int ret = 0;
	int bit;

	mutex_lock(&ar->conf_mutex);

	arvif->ar = ar;
	arvif->vif = vif;

	mutex_lock(&ar->vdev_mtx);
	if ((vif->type == NL80211_IFTYPE_MONITOR) && ar->monitor_present) {
		ath10k_warn("Only one monitor interface allowed\n");
		mutex_unlock(&ar->vdev_mtx);
		ret = -EBUSY;
		goto exit;
	}
	mutex_unlock(&ar->vdev_mtx);

	bit = ffs(ar->free_vdev_map);
	if (bit == 0) {
		ret = -EBUSY;
		goto exit;
	}

	arvif->vdev_id = bit - 1;
	arvif->vdev_subtype = WMI_VDEV_SUBTYPE_NONE;
	ar->free_vdev_map &= ~(1 << arvif->vdev_id);

	if (ar->p2p)
		arvif->vdev_subtype = WMI_VDEV_SUBTYPE_P2P_DEVICE;

	switch (vif->type) {
	case NL80211_IFTYPE_UNSPECIFIED:
	case NL80211_IFTYPE_STATION:
		arvif->vdev_type = WMI_VDEV_TYPE_STA;
		if (vif->p2p)
			arvif->vdev_subtype = WMI_VDEV_SUBTYPE_P2P_CLIENT;
		break;
	case NL80211_IFTYPE_ADHOC:
		arvif->vdev_type = WMI_VDEV_TYPE_IBSS;
		break;
	case NL80211_IFTYPE_AP:
		arvif->vdev_type = WMI_VDEV_TYPE_AP;

		if (vif->p2p)
			arvif->vdev_subtype = WMI_VDEV_SUBTYPE_P2P_GO;
		break;
	case NL80211_IFTYPE_MONITOR:
		arvif->vdev_type = WMI_VDEV_TYPE_MONITOR;
		break;
	default:
		WARN_ON(1);
		break;
	}

	ath10k_dbg(ATH10K_DBG_MAC, "Add interface: id %d type %d subtype %d\n",
		   arvif->vdev_id, arvif->vdev_type, arvif->vdev_subtype);

	ret = wmi_vdev_create(ar, arvif->vdev_id, arvif->vdev_type,
			      arvif->vdev_subtype, vif->addr);
	if (ret) {
		ath10k_warn("WMI vdev create failed: ret %d\n", ret);
		goto exit;
	}

	ret = wmi_vdev_set_param(ar, 0, WMI_VDEV_PARAM_DEF_KEYID,
				 arvif->def_wep_key_index);
	if (ret)
		ath10k_warn("Failed to set default keyid: %d\n", ret);

	if (arvif->vdev_type == WMI_VDEV_TYPE_AP) {
		ret = ath10k_peer_create(ar, arvif->vdev_id, vif->addr);
		if (ret) {
			ath10k_warn("Failed to create peer for AP: %d\n", ret);
			goto exit;
		}
	}

	if (arvif->vdev_type == WMI_VDEV_TYPE_STA) {
		ret = wmi_set_sta_ps_param(ar, arvif->vdev_id,
					   WMI_STA_PS_PARAM_RX_WAKE_POLICY,
					   WMI_STA_PS_RX_WAKE_POLICY_WAKE);
		if (ret)
			ath10k_warn("Failed to set RX wake policy: %d\n", ret);

		ret = wmi_set_sta_ps_param(ar, arvif->vdev_id,
					   WMI_STA_PS_PARAM_TX_WAKE_THRESHOLD,
					   WMI_STA_PS_TX_WAKE_THRESHOLD_ALWAYS);
		if (ret)
			ath10k_warn("Failed to set TX wake thresh: %d\n", ret);

		ret = wmi_set_sta_ps_param(ar, arvif->vdev_id,
					   WMI_STA_PS_PARAM_PSPOLL_COUNT,
					   WMI_STA_PS_PSPOLL_COUNT_NO_MAX);
		if (ret)
			ath10k_warn("Failed to set PSPOLL count: %d\n", ret);
	}

	if (arvif->vdev_type == WMI_VDEV_TYPE_MONITOR) {
		mutex_lock(&ar->vdev_mtx);
		ar->monitor_present = true;
		mutex_unlock(&ar->vdev_mtx);
	}

exit:
	mutex_unlock(&ar->conf_mutex);
	return ret;
}

static void ath10k_remove_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	int ret;

	mutex_lock(&ar->conf_mutex);

	ath10k_dbg(ATH10K_DBG_MAC, "Remove interface: id %d\n", arvif->vdev_id);

	ar->free_vdev_map |= 1 << (arvif->vdev_id);

	if (arvif->vdev_type == WMI_VDEV_TYPE_AP) {
		ret = ath10k_peer_delete(arvif->ar, arvif->vdev_id, vif->addr);
		if (ret)
			ath10k_warn("Failed to remove peer for AP: %d\n", ret);

		kfree(arvif->u.ap.noa_data);
	}

	ret = wmi_vdev_delete(ar, arvif->vdev_id);
	if (ret)
		ath10k_warn("WMI vdev delete failed: %d\n", ret);

	if (arvif->vdev_type == WMI_VDEV_TYPE_MONITOR) {
		mutex_lock(&ar->vdev_mtx);
		ar->monitor_present = false;
		mutex_unlock(&ar->vdev_mtx);
	}

	mutex_unlock(&ar->conf_mutex);
}

/*
 * FIXME: Has to be verified.
 */
#define SUPPORTED_FILTERS			\
	(FIF_PROMISC_IN_BSS |			\
	FIF_ALLMULTI |				\
	FIF_CONTROL |				\
	FIF_PSPOLL |				\
	FIF_OTHER_BSS |				\
	FIF_BCN_PRBRESP_PROMISC |		\
	FIF_PROBE_REQ |				\
	FIF_FCSFAIL)

static void ath10k_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed_flags,
				    unsigned int *total_flags,
				    u64 multicast)
{
	struct ath10k *ar = hw->priv;
	int ret;

	mutex_lock(&ar->conf_mutex);

	changed_flags &= SUPPORTED_FILTERS;
	*total_flags &= SUPPORTED_FILTERS;
	ar->filter_flags = *total_flags;

	mutex_lock(&ar->vdev_mtx);

	if ((ar->filter_flags & FIF_PROMISC_IN_BSS) &&
	    !ar->monitor_enabled) {
		ret = ath10k_monitor_start(ar, ar->monitor_vdev_id);
		if (ret)
			ath10k_warn("Unable to start monitor mode\n");
		else
			ath10k_dbg(ATH10K_DBG_MAC, "Monitor mode started\n");
	} else if (!(ar->filter_flags & FIF_PROMISC_IN_BSS) &&
		   ar->monitor_enabled) {
		ret = ath10k_monitor_stop(ar);
		if (ret)
			ath10k_warn("Unable to stop monitor mode\n");
		else
			ath10k_dbg(ATH10K_DBG_MAC, "Monitor mode stopped\n");
	}

	mutex_unlock(&ar->vdev_mtx);
	mutex_unlock(&ar->conf_mutex);
}

static void ath10k_bss_info_changed(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_bss_conf *info,
				    u32 changed)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	int ret = 0;

	mutex_lock(&ar->conf_mutex);

	if (changed & BSS_CHANGED_IBSS)
		ath10k_control_ibss(arvif, info, vif->addr);

	if (changed & BSS_CHANGED_BEACON_INT) {
		arvif->beacon_interval = info->beacon_int;
		ret = wmi_vdev_set_param(ar, arvif->vdev_id,
					 WMI_VDEV_PARAM_BEACON_INTERVAL,
					 arvif->beacon_interval);
		if (ret)
			ath10k_warn("Failed to set beacon interval for VDEV: %d\n",
				    arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Beacon interval: %d set for VDEV: %d\n",
				   arvif->beacon_interval, arvif->vdev_id);
	}

	if (changed & BSS_CHANGED_BEACON) {
		ret = wmi_pdev_set_param(ar, WMI_PDEV_PARAM_BEACON_TX_MODE,
					 WMI_BEACON_STAGGERED_MODE);
		if (ret)
			ath10k_warn("Failed to set beacon mode for VDEV: %d\n",
				    arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Set staggered beacon mode for VDEV: %d\n",
				   arvif->vdev_id);
	}

	if (changed & BSS_CHANGED_DTIM_PERIOD) {
		arvif->dtim_period = info->dtim_period;

		ret = wmi_vdev_set_param(ar, arvif->vdev_id,
					 WMI_VDEV_PARAM_DTIM_PERIOD,
					 arvif->dtim_period);
		if (ret)
			ath10k_warn("Failed to set dtim period for VDEV: %d\n",
				    arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Set dtim period: %d for VDEV: %d\n",
				   arvif->dtim_period, arvif->vdev_id);
	}

	if (changed & BSS_CHANGED_SSID) {
		arvif->u.ap.ssid_len = info->ssid_len;
		if (info->ssid_len)
			memcpy(arvif->u.ap.ssid, info->ssid, info->ssid_len);
		arvif->u.ap.hidden_ssid = info->hidden_ssid;
	}

	if (changed & BSS_CHANGED_BSSID) {
		if (!is_zero_ether_addr(info->bssid)) {
			ret = ath10k_peer_create(ar, arvif->vdev_id, info->bssid);
			if (ret)
				ath10k_warn("Failed to add peer: %pM for VDEV: %d\n",
					    info->bssid, arvif->vdev_id);
			else
				ath10k_dbg(ATH10K_DBG_MAC,
					   "Added peer: %pM for VDEV: %d\n",
					   info->bssid, arvif->vdev_id);


			if (vif->type == NL80211_IFTYPE_STATION) {
				/*
				 * this is never erased as we it for crypto key
				 * clearing; this is FW requirement
				 */
				memcpy(arvif->u.sta.bssid, info->bssid, ETH_ALEN);

				ret = ath10k_vdev_start(arvif);
				if (!ret)
					ath10k_dbg(ATH10K_DBG_MAC,
						   "VDEV: %d started with BSSID: %pM\n",
						   arvif->vdev_id, info->bssid);
			}

			/*
			 * Mac80211 does not keep IBSS bssid when leaving IBSS,
			 * so driver need to store it. It is needed when leaving
			 * IBSS in order to remove BSSID peer.
			 */
			if (vif->type == NL80211_IFTYPE_ADHOC)
				memcpy(arvif->u.ibss.bssid, info->bssid, ETH_ALEN);
		}
	}

	if (changed & BSS_CHANGED_BEACON_ENABLED)
		ath10k_control_beaconing(arvif, info);

	if (changed & BSS_CHANGED_ERP_CTS_PROT) {
		u32 cts_prot;
		if (info->use_cts_prot)
			cts_prot = 1;
		else
			cts_prot = 0;

		ret = wmi_vdev_set_param(ar, arvif->vdev_id,
					 WMI_VDEV_PARAM_ENABLE_RTSCTS,
					 cts_prot);
		if (ret)
			ath10k_warn("Failed to set CTS prot for VDEV: %d\n",
				    arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Set CTS prot: %d for VDEV: %d\n",
				   cts_prot, arvif->vdev_id);
	}

	if (changed & BSS_CHANGED_ERP_SLOT) {
		u32 slottime;
		if (info->use_short_slot)
			slottime = WMI_VDEV_SLOT_TIME_SHORT; /* 9us */

		else
			slottime = WMI_VDEV_SLOT_TIME_LONG; /* 20us */

		ret = wmi_vdev_set_param(ar, arvif->vdev_id,
					WMI_VDEV_PARAM_SLOT_TIME,
					slottime);
		if (ret)
			ath10k_warn("Failed to set erp slot for VDEV: %d\n",
				    arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Set slottime: %d for VDEV: %d\n",
				   slottime, arvif->vdev_id);
	}

	if (changed & BSS_CHANGED_ERP_PREAMBLE) {
		u32 preamble;
		if (info->use_short_preamble)
			preamble = WMI_VDEV_PREAMBLE_SHORT;
		else
			preamble = WMI_VDEV_PREAMBLE_LONG;

		ret = wmi_vdev_set_param(ar, arvif->vdev_id,
					WMI_VDEV_PARAM_PREAMBLE,
					preamble);
		if (ret)
			ath10k_warn("Failed to set preamble for VDEV: %d\n",
				    arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Set preamble: %d for VDEV: %d\n",
				   preamble, arvif->vdev_id);
	}

	if (changed & BSS_CHANGED_ASSOC) {
		if (info->assoc)
			ath10k_bss_assoc(hw, vif, info);
	}

	mutex_unlock(&ar->conf_mutex);
}

static int ath10k_hw_scan(struct ieee80211_hw *hw,
			  struct ieee80211_vif *vif,
			  struct cfg80211_scan_request *req)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	struct wmi_start_scan_arg arg;
	int ret = 0;
	int i;

	mutex_lock(&ar->conf_mutex);

	spin_lock_bh(&ar->scan.lock);
	if (ar->scan.in_progress) {
		spin_unlock_bh(&ar->scan.lock);
		ret = -EBUSY;
		goto exit;
	}

	INIT_COMPLETION(ar->scan.started);
	INIT_COMPLETION(ar->scan.completed);
	ar->scan.in_progress = true;
	ar->scan.aborting = false;
	ar->scan.is_roc = false;
	ar->scan.vdev_id = arvif->vdev_id;
	spin_unlock_bh(&ar->scan.lock);

	memset(&arg, 0, sizeof(arg));
	wmi_start_scan_init(ar, &arg);
	arg.vdev_id = arvif->vdev_id;
	arg.scan_id = ATH10K_SCAN_ID;

	if (!req->no_cck)
		arg.scan_ctrl_flags |= WMI_SCAN_ADD_CCK_RATES;

	if (req->ie_len) {
		arg.ie_len = req->ie_len;
		memcpy(arg.ie, req->ie, arg.ie_len);
	}

	if (req->n_ssids) {
		arg.n_ssids = req->n_ssids;
		for (i = 0; i < arg.n_ssids; i++) {
			arg.ssids[i].len  = req->ssids[i].ssid_len;
			arg.ssids[i].ssid = req->ssids[i].ssid;
		}
	}

	if (req->n_channels) {
		arg.n_channels = req->n_channels;
		for (i = 0; i < arg.n_channels; i++)
			arg.channels[i] = req->channels[i]->center_freq;
	}

	ret = ath10k_start_scan(ar, &arg);
	if (ret) {
		spin_lock_bh(&ar->scan.lock);
		ar->scan.in_progress = false;
		spin_unlock_bh(&ar->scan.lock);
	}

exit:
	mutex_unlock(&ar->conf_mutex);
	return ret;
}

static void ath10k_cancel_hw_scan(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif)
{
	struct ath10k *ar = hw->priv;

	mutex_lock(&ar->conf_mutex);
	ath10k_abort_scan(ar);
	mutex_unlock(&ar->conf_mutex);
}

static int ath10k_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			  struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			  struct ieee80211_key_conf *key)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	struct ath10k_peer *peer;
	const u8 *peer_addr;
	bool is_wep = key->cipher == WLAN_CIPHER_SUITE_WEP40 ||
		      key->cipher == WLAN_CIPHER_SUITE_WEP104;
	int ret = 0;

	if (key->keyidx > WMI_MAX_KEY_INDEX)
		return -ENOSPC;

	mutex_lock(&ar->conf_mutex);

	if (sta)
		peer_addr = sta->addr;
	else if (arvif->vdev_type == WMI_VDEV_TYPE_STA)
		peer_addr = vif->bss_conf.bssid;
	else
		peer_addr = vif->addr;

	key->hw_key_idx = key->keyidx;

	/* the peer should not disappear in mid-way (unless FW goes awry) since
	 * we already hold conf_mutex. we just make sure its there now. */
	spin_lock_bh(&ar->data_lock);
	peer = ath10k_peer_find(ar, arvif->vdev_id, peer_addr);
	spin_unlock_bh(&ar->data_lock);

	if (!peer) {
		if (cmd == SET_KEY) {
			ath10k_warn("cannot install key for non-existent peer %pM\n",
				    peer_addr);
			ret = -EOPNOTSUPP;
			goto exit;
		} else {
			/* if the peer doesn't exist there is no key to disable
			 * anymore */
			goto exit;
		}
	}

	if (is_wep) {
		if (cmd == SET_KEY)
			arvif->wep_keys[key->keyidx] = key;
		else
			arvif->wep_keys[key->keyidx] = NULL;

		if (cmd == DISABLE_KEY)
			ath10k_clear_vdev_key(arvif, key);
	}

	ret = ath10k_install_key(arvif, key, cmd, peer_addr);
	if (ret) {
		ath10k_warn("ath10k_install_key failed (%d)\n", ret);
		goto exit;
	}

	spin_lock_bh(&ar->data_lock);
	peer = ath10k_peer_find(ar, arvif->vdev_id, peer_addr);
	if (peer && cmd == SET_KEY)
		peer->keys[key->keyidx] = key;
	else if (peer && cmd == DISABLE_KEY)
		peer->keys[key->keyidx] = NULL;
	else if (peer == NULL)
		/* impossible unless FW goes crazy */
		ath10k_warn("peer %pM disappeared!\n", peer_addr);
	spin_unlock_bh(&ar->data_lock);

exit:
	mutex_unlock(&ar->conf_mutex);
	return ret;
}

static int ath10k_sta_state(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta,
			    enum ieee80211_sta_state old_state,
			    enum ieee80211_sta_state new_state)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	int ret = 0;

	mutex_lock(&ar->conf_mutex);

	if (old_state == IEEE80211_STA_NOTEXIST &&
	    new_state == IEEE80211_STA_NONE &&
	    vif->type != NL80211_IFTYPE_STATION) {
		/*
		 * New station addition.
		 */
		ret = ath10k_peer_create(ar, arvif->vdev_id, sta->addr);
		if (ret)
			ath10k_warn("Failed to add peer: %pM for VDEV: %d\n",
				    sta->addr, arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Added peer: %pM for VDEV: %d\n",
				   sta->addr, arvif->vdev_id);
	} else if ((old_state == IEEE80211_STA_NONE &&
		    new_state == IEEE80211_STA_NOTEXIST)) {
		/*
		 * Existing station deletion.
		 */
		ret = ath10k_peer_delete(ar, arvif->vdev_id, sta->addr);
		if (ret)
			ath10k_warn("Failed to delete peer: %pM for VDEV: %d\n",
				    sta->addr, arvif->vdev_id);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Removed peer: %pM for VDEV: %d\n",
				   sta->addr, arvif->vdev_id);

		if (vif->type == NL80211_IFTYPE_STATION)
			ath10k_bss_disassoc(hw, vif);
	} else if (old_state == IEEE80211_STA_AUTH &&
		   new_state == IEEE80211_STA_ASSOC &&
		   (vif->type == NL80211_IFTYPE_AP ||
		    vif->type == NL80211_IFTYPE_ADHOC)) {
		/*
		 * New association.
		 */
		ret = ath10k_station_assoc(ar, arvif, sta);
		if (ret)
			ath10k_warn("Failed to associate station: %pM\n",
				    sta->addr);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Station %pM moved to assoc state\n",
				   sta->addr);
	} else if (old_state == IEEE80211_STA_ASSOC &&
		   new_state == IEEE80211_STA_AUTH &&
		   (vif->type == NL80211_IFTYPE_AP ||
		    vif->type == NL80211_IFTYPE_ADHOC)) {
		/*
		 * Disassociation.
		 */
		ret = ath10k_station_disassoc(ar, arvif, sta);
		if (ret)
			ath10k_warn("Failed to disassociate station: %pM\n",
				    sta->addr);
		else
			ath10k_dbg(ATH10K_DBG_MAC,
				   "Station %pM moved to disassociated state\n",
				   sta->addr);
	}

	mutex_unlock(&ar->conf_mutex);
	return ret;
}

static int ath10k_conf_tx(struct ieee80211_hw *hw,
			  struct ieee80211_vif *vif, u16 ac,
			  const struct ieee80211_tx_queue_params *params)
{
	struct ath10k *ar = hw->priv;
	struct wmi_wmm_params_arg *p = NULL;
	int ret;

	mutex_lock(&ar->conf_mutex);

	switch (ac) {
	case IEEE80211_AC_VO:
		p = &ar->wmm_params.ac_vo;
		break;
	case IEEE80211_AC_VI:
		p = &ar->wmm_params.ac_vi;
		break;
	case IEEE80211_AC_BE:
		p = &ar->wmm_params.ac_be;
		break;
	case IEEE80211_AC_BK:
		p = &ar->wmm_params.ac_bk;
		break;
	}

	if (WARN_ON(!p))
		return -EINVAL;

	p->cwmin = params->cw_min;
	p->cwmax = params->cw_max;
	p->aifs = params->aifs;
	p->txop = params->txop;
	/* FIXME: can we pass the params->uapsd to the FW? */
	/* FIXME: FW accepts wmm params per hw, not per vif */

	ret = wmi_pdev_set_wmm_params(ar, &ar->wmm_params);
	if (ret)
		ath10k_warn("could not set wmm params (%d)\n", ret);

	mutex_unlock(&ar->conf_mutex);
	return ret;
}

#define ATH10K_ROC_TIMEOUT_HZ (2*HZ)

static int ath10k_remain_on_channel(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_channel *chan,
				    int duration,
				    enum ieee80211_roc_type type)
{
	struct ath10k *ar = hw->priv;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	struct wmi_start_scan_arg arg;
	int ret;

	mutex_lock(&ar->conf_mutex);

	spin_lock_bh(&ar->scan.lock);
	if (ar->scan.in_progress) {
		spin_unlock_bh(&ar->scan.lock);
		ret = -EBUSY;
		goto exit;
	}

	INIT_COMPLETION(ar->scan.started);
	INIT_COMPLETION(ar->scan.completed);
	ar->scan.in_progress = true;
	ar->scan.aborting = false;
	ar->scan.is_roc = true;
	ar->scan.vdev_id = arvif->vdev_id;
	spin_unlock_bh(&ar->scan.lock);

	memset(&arg, 0, sizeof(arg));
	wmi_start_scan_init(ar, &arg);
	arg.vdev_id = arvif->vdev_id;
	arg.scan_id = ATH10K_SCAN_ID;
	arg.n_channels = 1;
	arg.channels[0] = chan->center_freq;
	arg.dwell_time_active = duration;
	arg.dwell_time_passive = duration;
	arg.max_scan_time = 2 * duration;
	arg.scan_ctrl_flags |= WMI_SCAN_FLAG_PASSIVE;
	arg.scan_ctrl_flags |= WMI_SCAN_FILTER_PROBE_REQ;

	ret = ath10k_start_scan(ar, &arg);
	if (ret) {
		ath10k_warn("could not start roc scan (%d)\n", ret);
		spin_lock_bh(&ar->scan.lock);
		ar->scan.in_progress = false;
		spin_unlock_bh(&ar->scan.lock);
	}

exit:
	mutex_unlock(&ar->conf_mutex);
	return ret;
}

static int ath10k_cancel_remain_on_channel(struct ieee80211_hw *hw)
{
	struct ath10k *ar = hw->priv;

	mutex_lock(&ar->conf_mutex);
	ath10k_abort_scan(ar);
	mutex_unlock(&ar->conf_mutex);

	return 0;
}

/*
 * Both RTS and Fragmentation threshold are interface-specific
 * in ath10k, but device-specific in mac80211.
 */
static void ath10k_set_rts_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct ath10k_generic_iter *ar_iter = data;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	u32 rts = ar_iter->ar->hw->wiphy->rts_threshold;

	if (rts > ATH10K_RTS_MAX)
		rts = ATH10K_RTS_MAX;

	ar_iter->ret = wmi_vdev_set_param(ar_iter->ar, arvif->vdev_id,
					  WMI_VDEV_PARAM_RTS_THRESHOLD, rts);
	if (ar_iter->ret)
		ath10k_warn("Failed to set RTS threshold for VDEV: %d\n",
			    arvif->vdev_id);
	else
		ath10k_dbg(ATH10K_DBG_MAC,
			   "Set RTS threshold: %d for VDEV: %d\n",
			   rts, arvif->vdev_id);
}

static int ath10k_set_rts_threshold(struct ieee80211_hw *hw, u32 value)
{
	struct ath10k_generic_iter ar_iter;
	struct ath10k *ar = hw->priv;

	memset(&ar_iter, 0, sizeof(struct ath10k_generic_iter));
	ar_iter.ar = ar;

	mutex_lock(&ar->conf_mutex);
	ieee80211_iterate_active_interfaces(hw, IEEE80211_IFACE_ITER_RESUME_ALL,
					    ath10k_set_rts_iter, &ar_iter);
	mutex_unlock(&ar->conf_mutex);

	return ar_iter.ret;
}

static void ath10k_set_frag_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct ath10k_generic_iter *ar_iter = data;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);
	u32 frag = ar_iter->ar->hw->wiphy->frag_threshold;

	if (frag > ATH10K_FRAGMT_THRESHOLD_MAX)
		frag = ATH10K_FRAGMT_THRESHOLD_MAX;
	else if (frag < ATH10K_FRAGMT_THRESHOLD_MIN)
		frag = ATH10K_FRAGMT_THRESHOLD_MIN;

	ar_iter->ret = wmi_vdev_set_param(ar_iter->ar, arvif->vdev_id,
				  WMI_VDEV_PARAM_FRAGMENTATION_THRESHOLD, frag);
	if (ar_iter->ret)
		ath10k_warn("Failed to set frag threshold for VDEV: %d\n",
			    arvif->vdev_id);
	else
		ath10k_dbg(ATH10K_DBG_MAC,
			   "Set frag threshold: %d for VDEV: %d\n",
			   frag, arvif->vdev_id);
}

static int ath10k_set_frag_threshold(struct ieee80211_hw *hw, u32 value)
{
	struct ath10k_generic_iter ar_iter;
	struct ath10k *ar = hw->priv;

	memset(&ar_iter, 0, sizeof(struct ath10k_generic_iter));
	ar_iter.ar = ar;

	mutex_lock(&ar->conf_mutex);
	ieee80211_iterate_active_interfaces(hw, IEEE80211_IFACE_ITER_RESUME_ALL,
					   ath10k_set_frag_iter, &ar_iter);
	mutex_unlock(&ar->conf_mutex);

	return ar_iter.ret;
}

static void ath10k_flush(struct ieee80211_hw *hw, u32 queues, bool drop)
{
	struct ath10k *ar = hw->priv;
	int ret;

	/* mac80211 doesn't care if we really xmit queued frames or not
	 * we'll collect those frames either way if we stop/delete vdevs */
	if (drop)
		return;

	ret = wait_event_timeout(ar->htt->empty_tx_wq,
				 atomic_read(&ar->htt->num_used_txi) == 0,
				 ATH10K_FLUSH_TIMEOUT_HZ);
	if (ret <= 0)
		ath10k_warn("tx not flushed (%d frames still pending)\n",
			    atomic_read(&ar->htt->num_used_txi));
}

/* TODO: Implement this function properly
 * For now it is needed to reply to Probe Requests in IBSS mode.
 * Propably we need this information from FW.
 */
static int ath10k_tx_last_beacon(struct ieee80211_hw *hw)
{
	return 1;
}

static const struct ieee80211_ops ath10k_ops = {
	.tx				= ath10k_tx,
	.start				= ath10k_start,
	.stop				= ath10k_stop,
	.config				= ath10k_config,
	.add_interface			= ath10k_add_interface,
	.remove_interface		= ath10k_remove_interface,
	.configure_filter		= ath10k_configure_filter,
	.bss_info_changed		= ath10k_bss_info_changed,
	.hw_scan			= ath10k_hw_scan,
	.cancel_hw_scan			= ath10k_cancel_hw_scan,
	.set_key			= ath10k_set_key,
	.sta_state			= ath10k_sta_state,
	.conf_tx			= ath10k_conf_tx,
	.remain_on_channel		= ath10k_remain_on_channel,
	.cancel_remain_on_channel	= ath10k_cancel_remain_on_channel,
	.set_rts_threshold		= ath10k_set_rts_threshold,
	.set_frag_threshold		= ath10k_set_frag_threshold,
	.flush				= ath10k_flush,
	.tx_last_beacon			= ath10k_tx_last_beacon,
};

#define RATETAB_ENT(_rate, _rateid, _flags) { \
	.bitrate		= (_rate), \
	.flags			= (_flags), \
	.hw_value		= (_rateid), \
}

#define CHAN2G(_channel, _freq, _flags) { \
	.band			= IEEE80211_BAND_2GHZ, \
	.hw_value		= (_channel), \
	.center_freq		= (_freq), \
	.flags			= (_flags), \
	.max_antenna_gain	= 0, \
	.max_power		= 30, \
}

#define CHAN5G(_channel, _freq, _flags) { \
	.band			= IEEE80211_BAND_5GHZ, \
	.hw_value		= (_channel), \
	.center_freq		= (_freq), \
	.flags			= (_flags), \
	.max_antenna_gain	= 0, \
	.max_power		= 30, \
}

static struct ieee80211_channel ath10k_2ghz_channels[] = {
	CHAN2G(1, 2412, 0),
	CHAN2G(2, 2417, 0),
	CHAN2G(3, 2422, 0),
	CHAN2G(4, 2427, 0),
	CHAN2G(5, 2432, 0),
	CHAN2G(6, 2437, 0),
	CHAN2G(7, 2442, 0),
	CHAN2G(8, 2447, 0),
	CHAN2G(9, 2452, 0),
	CHAN2G(10, 2457, 0),
	CHAN2G(11, 2462, 0),
	CHAN2G(12, 2467, 0),
	CHAN2G(13, 2472, 0),
	CHAN2G(14, 2484, 0),
};

static struct ieee80211_channel ath10k_5ghz_channels[] = {
	CHAN5G(36, 5180, 14),
	CHAN5G(40, 5200, 15),
	CHAN5G(44, 5220, 16),
	CHAN5G(48, 5240, 17),
	CHAN5G(52, 5260, 18),
	CHAN5G(56, 5280, 19),
	CHAN5G(60, 5300, 20),
	CHAN5G(64, 5320, 21),
	CHAN5G(100, 5500, 22),
	CHAN5G(104, 5520, 23),
	CHAN5G(108, 5540, 24),
	CHAN5G(112, 5560, 25),
	CHAN5G(116, 5580, 26),
	CHAN5G(120, 5600, 27),
	CHAN5G(124, 5620, 28),
	CHAN5G(128, 5640, 29),
	CHAN5G(132, 5660, 30),
	CHAN5G(136, 5680, 31),
	CHAN5G(140, 5700, 32),
	CHAN5G(149, 5745, 33),
	CHAN5G(153, 5765, 34),
	CHAN5G(157, 5785, 35),
	CHAN5G(161, 5805, 36),
	CHAN5G(165, 5825, 37),
};

static struct ieee80211_rate ath10k_rates[] = {
	/* CCK */
	RATETAB_ENT(10,  0x82, 0),
	RATETAB_ENT(20,  0x84, 0),
	RATETAB_ENT(55,  0x8b, 0),
	RATETAB_ENT(110, 0x96, 0),
	/* OFDM */
	RATETAB_ENT(60,  0x0c, 0),
	RATETAB_ENT(90,  0x12, 0),
	RATETAB_ENT(120, 0x18, 0),
	RATETAB_ENT(180, 0x24, 0),
	RATETAB_ENT(240, 0x30, 0),
	RATETAB_ENT(360, 0x48, 0),
	RATETAB_ENT(480, 0x60, 0),
	RATETAB_ENT(540, 0x6c, 0),
};

#define ath10k_a_rates (ath10k_rates + 4)
#define ath10k_a_rates_size (ARRAY_SIZE(ath10k_rates) - 4)
#define ath10k_g_rates (ath10k_rates + 0)
#define ath10k_g_rates_size (ARRAY_SIZE(ath10k_rates))

static struct ieee80211_supported_band ath10k_band_2ghz = {
	.n_channels = ARRAY_SIZE(ath10k_2ghz_channels),
	.channels = ath10k_2ghz_channels,
	.n_bitrates = ath10k_g_rates_size,
	.bitrates = ath10k_g_rates,
	/* .ht_cap depends on FW capabilities */
};

static struct ieee80211_supported_band ath10k_band_5ghz = {
	.n_channels = ARRAY_SIZE(ath10k_5ghz_channels),
	.channels = ath10k_5ghz_channels,
	.n_bitrates = ath10k_a_rates_size,
	.bitrates = ath10k_a_rates,
	/* .ht_cap depends on FW capabilities */
};

struct ath10k *ath10k_mac_create(void)
{
	struct ieee80211_hw *hw;
	struct ath10k *ar;

	hw = ieee80211_alloc_hw(sizeof(struct ath10k), &ath10k_ops);
	if (!hw)
		return NULL;

	ar = hw->priv;
	ar->hw = hw;

	return ar;
}

void ath10k_mac_destroy(struct ath10k *ar)
{
	ieee80211_free_hw(ar->hw);
}

static struct ieee80211_iface_limit ath10k_if_limits[] = {
	{
	.max	= 8,
	.types	= BIT(NL80211_IFTYPE_STATION)
		| BIT(NL80211_IFTYPE_P2P_CLIENT)
		| BIT(NL80211_IFTYPE_P2P_GO)
		| BIT(NL80211_IFTYPE_AP)
	}
};

static struct ieee80211_iface_combination ath10k_if_comb = {
	.limits = ath10k_if_limits,
	.n_limits = ARRAY_SIZE(ath10k_if_limits),
	.max_interfaces = 8,
	.num_different_channels = 1,
	.beacon_int_infra_match = true,
};

static struct ieee80211_sta_ht_cap ath10k_get_ht_cap(struct ath10k *ar)
{
	int i;
	struct ieee80211_sta_ht_cap ht_cap = {0};

	if (!(ar->ht_cap_info & WMI_HT_CAP_ENABLED))
		return ht_cap;

	ht_cap.ht_supported = 1;
	ht_cap.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K;
	ht_cap.cap |= IEEE80211_HT_CAP_SUP_WIDTH_20_40;
	ht_cap.cap |= IEEE80211_HT_CAP_DSSSCCK40;
	ht_cap.cap |= WLAN_HT_CAP_SM_PS_STATIC << IEEE80211_HT_CAP_SM_PS_SHIFT;

	if (ar->ht_cap_info & WMI_HT_CAP_HT20_SGI)
		ht_cap.cap |= IEEE80211_HT_CAP_SGI_20;

	if (ar->ht_cap_info & WMI_HT_CAP_HT40_SGI)
		ht_cap.cap |= IEEE80211_HT_CAP_SGI_40;

	if (ar->ht_cap_info & WMI_HT_CAP_DYNAMIC_SMPS) {
		u32 smps;

		smps   = WLAN_HT_CAP_SM_PS_DYNAMIC;
		smps <<= IEEE80211_HT_CAP_SM_PS_SHIFT;

		ht_cap.cap |= smps;
	}

	if (ar->ht_cap_info & WMI_HT_CAP_TX_STBC)
		ht_cap.cap |= IEEE80211_HT_CAP_TX_STBC;

	if (ar->ht_cap_info & WMI_HT_CAP_RX_STBC) {
		u32 stbc;

		stbc   = ar->ht_cap_info;
		stbc  &= WMI_HT_CAP_RX_STBC;
		stbc >>= WMI_HT_CAP_RX_STBC_MASK_SHIFT;
		stbc <<= IEEE80211_HT_CAP_RX_STBC_SHIFT;
		stbc  &= IEEE80211_HT_CAP_RX_STBC;

		ht_cap.cap |= stbc;
	}

	if (ar->ht_cap_info & WMI_HT_CAP_LDPC)
		ht_cap.cap |= IEEE80211_HT_CAP_LDPC_CODING;

	if (ar->ht_cap_info & WMI_HT_CAP_L_SIG_TXOP_PROT)
		ht_cap.cap |= IEEE80211_HT_CAP_LSIG_TXOP_PROT;

	if (ar->ht_cap_info & WMI_HT_CAP_MPDU_DENSITY) {
		u32 density;

		density   = ar->ht_cap_info;
		density  &= WMI_HT_CAP_MPDU_DENSITY;
		density >>= WMI_HT_CAP_MPDU_DENSITY_MASK_SHIFT;

		ht_cap.ampdu_density = density;
	}

	/* max AMSDU is implicitly taken from vht_cap_info */
	if (ar->vht_cap_info & WMI_VHT_CAP_MAX_MPDU_LEN_MASK)
		ht_cap.cap |= IEEE80211_HT_CAP_MAX_AMSDU;

	for (i = 0; i < WMI_MAX_SPATIAL_STREAM; i++)
		ht_cap.mcs.rx_mask[i] = 0xFF;

	ht_cap.mcs.tx_params |= IEEE80211_HT_MCS_TX_DEFINED;

	return ht_cap;
}


static void ath10k_get_arvif_iter(void *data, u8 *mac, struct ieee80211_vif *vif)
{
	struct ath10k_vif_iter *arvif_iter = data;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vif);

	if (arvif->vdev_id == arvif_iter->vdev_id)
		arvif_iter->arvif = arvif;
}

struct ath10k_vif *ath10k_get_arvif(struct ath10k *ar, u32 vdev_id)
{
	struct ath10k_vif_iter arvif_iter;

	memset(&arvif_iter, 0, sizeof(struct ath10k_vif_iter));
	arvif_iter.vdev_id = vdev_id;

	ieee80211_iterate_active_interfaces_atomic(ar->hw,
					   IEEE80211_IFACE_ITER_RESUME_ALL,
					   ath10k_get_arvif_iter, &arvif_iter);
	if (!arvif_iter.arvif) {
		ath10k_warn("No VIF found for VDEV: %d\n", vdev_id);
		return NULL;
	}

	return arvif_iter.arvif;
}

int ath10k_mac_register(struct ath10k *ar)
{
	struct ath_common *common = ath10k_common(ar);
	struct ieee80211_sta_ht_cap ht_cap;
	int ret;

	SET_IEEE80211_PERM_ADDR(ar->hw, ar->mac_addr);

	SET_IEEE80211_DEV(ar->hw, ar->dev);

	ht_cap = ath10k_get_ht_cap(ar);
	ath10k_band_2ghz.ht_cap = ht_cap;
	ath10k_band_5ghz.ht_cap = ht_cap;

	if (ar->phy_capability & WHAL_WLAN_11G_CAPABILITY)
		ar->hw->wiphy->bands[IEEE80211_BAND_2GHZ] = &ath10k_band_2ghz;

	if (ar->phy_capability & WHAL_WLAN_11A_CAPABILITY)
		ar->hw->wiphy->bands[IEEE80211_BAND_5GHZ] = &ath10k_band_5ghz;

	ar->hw->wiphy->interface_modes =
		BIT(NL80211_IFTYPE_STATION) |
		BIT(NL80211_IFTYPE_ADHOC) |
		BIT(NL80211_IFTYPE_AP) |
		BIT(NL80211_IFTYPE_P2P_CLIENT) |
		BIT(NL80211_IFTYPE_P2P_GO);

	ar->hw->flags = IEEE80211_HW_SIGNAL_DBM |
			IEEE80211_HW_SUPPORTS_PS |
			IEEE80211_HW_SUPPORTS_DYNAMIC_PS |
			IEEE80211_HW_MFP_CAPABLE |
			IEEE80211_HW_REPORTS_TX_ACK_STATUS |
			IEEE80211_HW_HAS_RATE_CONTROL |
			IEEE80211_HW_SUPPORTS_STATIC_SMPS |
			IEEE80211_HW_WANT_MONITOR_VIF |
			IEEE80211_HW_AP_LINK_PS;

	if (ar->ht_cap_info & WMI_HT_CAP_DYNAMIC_SMPS)
		ar->hw->flags |= IEEE80211_HW_SUPPORTS_DYNAMIC_SMPS;

	if (ar->ht_cap_info & WMI_HT_CAP_ENABLED) {
		ar->hw->flags |= IEEE80211_HW_AMPDU_AGGREGATION;
		ar->hw->flags |= IEEE80211_HW_TX_AMPDU_SETUP_IN_HW;
	}

	ar->hw->wiphy->max_scan_ssids = WLAN_SCAN_PARAMS_MAX_SSID;
	ar->hw->wiphy->max_scan_ie_len = WLAN_SCAN_PARAMS_MAX_IE_LEN;

	ar->hw->vif_data_size = sizeof(struct ath10k_vif);

	ar->hw->channel_change_time = 5000;
	ar->hw->max_listen_interval = ATH10K_MAX_HW_LISTEN_INTERVAL;

	ar->hw->wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
	ar->hw->wiphy->max_remain_on_channel_duration = 5000;

	/*
	 * on LL hardware queues are managed entirely by the FW
	 * so we only advertise to mac we can do the queues thing
	 */
	ar->hw->queues = 4;

	ar->hw->wiphy->iface_combinations = &ath10k_if_comb;
	ar->hw->wiphy->n_iface_combinations = 1;

	common->regulatory.country_code = CTRY_DEFAULT;
	ret = ath_regd_init(&common->regulatory, ar->hw->wiphy,
			    ath10k_reg_notifier);
	if (ret) {
		ath10k_err("Regulatory initialization failed\n");
		return ret;
	}

	ret = ieee80211_register_hw(ar->hw);
	if (ret) {
		ath10k_err("ieee80211 registration failed: %d\n", ret);
		return ret;
	}

	if (!ath_is_world_regd(&common->regulatory)) {
		ret = regulatory_hint(ar->hw->wiphy,
				      common->regulatory.alpha2);
		if (ret)
			goto exit;
	}

	return 0;
exit:
	ieee80211_unregister_hw(ar->hw);
	return ret;
}

void ath10k_mac_unregister(struct ath10k *ar)
{
	ieee80211_unregister_hw(ar->hw);

	SET_IEEE80211_DEV(ar->hw, NULL);
}
