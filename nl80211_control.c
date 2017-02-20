/*
    This file is part of LORCON

	The majority of the functions in this file were derived directly from the 
	'iw' tool (and are marked as such), (c) Johannes Berg

    LOROCN is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    LORCON is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LORCON; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#if defined(HAVE_LIBNL20) || defined(HAVE_LIBNL30)
#define HAVE_LIBNL_NG
#endif

#ifdef SYS_LINUX

#ifdef HAVE_LINUX_NETLINK
#include <sys/types.h>
#include <asm/types.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <netlink/netlink.h>
#include "nl80211.h"
#include <net/if.h>
#endif

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "lorcon.h"
#include "nl80211.h"
#include "nl80211_control.h"

// Libnl1->Libnl2 compatability mode since the API changed, cribbed from 'iw'
#if !defined(HAVE_LIBNL_NG)
#define nl_sock nl_handle

static inline struct nl_handle *nl_socket_alloc(void) {
#ifdef HAVE_LINUX_NETLINK
	return (struct nl_handle *) nl_handle_alloc();
#else
    return NULL;
#endif
}

static inline void nl_socket_free(struct nl_sock *h) {
#ifdef HAVE_LINUX_NETLINK
	nl_handle_destroy(h);
#endif
}

static inline int __genl_ctrl_alloc_cache(struct nl_sock *h, struct nl_cache **cache) {
#ifdef HAVE_LINUX_NETLINK
	struct nl_cache *tmp = genl_ctrl_alloc_cache(h);
	if (!tmp)
		return -1;
	*cache = tmp;
#else
    *cache = NULL;
#endif
	return 0;
}
#define genl_ctrl_alloc_cache __genl_ctrl_alloc_cache
#endif

int ChanToFreq(int in_chan) {
   // 80211 frequencies to channels
   // Stolen from Linux net/wireless/util.c
   if (in_chan == 14)
       return 2484;
   else if (in_chan < 14)
       return 2407 + in_chan * 5;
   if (in_chan >= 182 && in_chan <= 196)
       return 4000 + in_chan * 5;
   else
       return 5000 + in_chan * 5;

   return in_chan;
}

int FreqToChan(int in_freq) {
	// 80211 frequencies to channels
	// Stolen from Linux net/wireless/util.c
	/* see 802.11 17.3.8.3.2 and Annex J */
	if (in_freq == 2484)
		return 14;
	else if (in_freq < 2484)
		return (in_freq - 2407) / 5;
	else if (in_freq >= 4910 && in_freq <= 4980)
		return (in_freq - 4000) / 5;
	else if (in_freq <= 45000) /* DMG band lower limit */
		return (in_freq - 5000) / 5;
	else if (in_freq >= 58320 && in_freq <= 64800)
		return (in_freq - 56160) / 2160;
	else
		return in_freq;
}

int nl80211_connect(const char *interface, void **handle, void **cache,
					 void **family, char *errstr) {
#ifndef HAVE_LINUX_NETLINK
	snprintf(errstr, LORCON_STATUS_MAX, "LORCON was not compiled with netlink/nl80211 "
			 "support, check the output of ./configure for why");
	return -1;
#else
	struct nl_sock *nl_handle;
	struct nl_cache *nl_cache;
	struct genl_family *nl80211;

    if (*handle == NULL) {
        if ((nl_handle = nl_socket_alloc()) == NULL) {
            snprintf(errstr, LORCON_STATUS_MAX, "%s failed to allocate nlhandle",
                    __FUNCTION__);
            return -1;
        }

        if (genl_connect(nl_handle)) {
            snprintf(errstr, LORCON_STATUS_MAX, "%s failed to connect to generic netlink",
                    __FUNCTION__);
            nl_socket_free(nl_handle);
            return -1;
        } 
    } else {
        nl_handle = (struct nl_sock *) (*handle);
    }

	if (genl_ctrl_alloc_cache(nl_handle, &nl_cache) != 0) {
		snprintf(errstr, LORCON_STATUS_MAX, "%s failed to allocate "
				 "generic netlink cache", __FUNCTION__);
		nl_socket_free(nl_handle);
		return -1;
	}

	if ((nl80211 = genl_ctrl_search_by_name(nl_cache, "nl80211")) == NULL) {
		snprintf(errstr, LORCON_STATUS_MAX, "%s failed to find "
				 "nl80211 controls, kernel may be too old", __FUNCTION__);
		nl_socket_free(nl_handle);
		return -1;
	}

	(*handle) = (void *) nl_handle;
	(*cache) = (void *) nl_cache;
	(*family) = (void *) nl80211;

	return 1;
#endif
}

void nl80211_disconnect(void *handle) {
#ifdef HAVE_LINUX_NETLINK
	nl_socket_free((struct nl_sock *) handle);
#endif
}

int nl80211_createvap(const char *interface, const char *newinterface, char *errstr) {
#ifndef HAVE_LINUX_NETLINK
	snprintf(errstr, LORCON_STATUS_MAX, "LORCON was not compiled with "
			 "netlink/nl80211 support, check the output of ./configure for why");
	return -1;
#else

	struct nl_sock *nl_handle;
	struct nl_cache *nl_cache;
	struct genl_family *nl80211;
	struct nl_msg *msg;

	if (if_nametoindex(newinterface) > 0) 
		return 1;

	if (nl80211_connect(interface, (void **) &nl_handle, 
						 (void **) &nl_cache, (void **) &nl80211, errstr) < 0)
		return -1;

	if ((msg = nlmsg_alloc()) == NULL) {
		snprintf(errstr, LORCON_STATUS_MAX, "nl80211_createvap() failed to allocate "
				 "message");
		nl80211_disconnect(nl_handle);
		return -1;
	}

	genlmsg_put(msg, 0, 0, genl_family_get_id(nl80211), 0, 0, 
				NL80211_CMD_NEW_INTERFACE, 0);
	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, if_nametoindex(interface));
	NLA_PUT_STRING(msg, NL80211_ATTR_IFNAME, newinterface);
	NLA_PUT_U32(msg, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MONITOR);

	if (nl_send_auto_complete(nl_handle, msg) < 0 || nl_wait_for_ack(nl_handle) < 0) {
nla_put_failure:
		snprintf(errstr, LORCON_STATUS_MAX, "nl80211_createvap() failed to create "
				 "interface '%s'", newinterface);
		nlmsg_free(msg);
		nl80211_disconnect(nl_handle);
		return -1;
	}

	nlmsg_free(msg);

	nl80211_disconnect(nl_handle);

	if (if_nametoindex(newinterface) <= 0) {
		snprintf(errstr, LORCON_STATUS_MAX, "nl80211_createvap() thought we made a "
				 "vap, but it wasn't there when we looked");
		return -1;
	}

	return 0;
#endif
}

// Has to be a separate function because of gotos, ew
void nl80211_parseflags(int nflags, int *in_flags, struct nl_msg *msg) {
#ifdef HAVE_LINUX_NETLINK
	struct nl_msg *flags;
	unsigned int x;
	enum nl80211_mntr_flags flag = NL80211_MNTR_FLAG_MAX;

	if ((flags = nlmsg_alloc()) == NULL) {
		return;
	}

	for (x = 0; x < nflags; x++) {
		switch (in_flags[x]) {
			case nl80211_mntr_flag_none:
				continue;
				break;
			case nl80211_mntr_flag_fcsfail:
				flag = NL80211_MNTR_FLAG_FCSFAIL;
				break;
			case nl80211_mntr_flag_plcpfail:
				flag = NL80211_MNTR_FLAG_PLCPFAIL;
				break;
			case nl80211_mntr_flag_control:
				flag = NL80211_MNTR_FLAG_CONTROL;
				break;
			case nl80211_mntr_flag_otherbss:
				flag = NL80211_MNTR_FLAG_OTHER_BSS;
				break;
			case nl80211_mntr_flag_cookframe:
				flag = NL80211_MNTR_FLAG_COOK_FRAMES;
				break;
		}

		NLA_PUT_FLAG(flags, flag);
	}

	nla_put_nested(msg, NL80211_ATTR_MNTR_FLAGS, flags);

nla_put_failure:
	nlmsg_free(flags);
#endif
}

int nl80211_setvapflag(const char *interface, char *errstr, int nflags, int *in_flags) {
#ifndef HAVE_LINUX_NETLINK
	snprintf(errstr, LORCON_STATUS_MAX, "LORCON was not compiled with netlink/nl80211 "
			 "support, check the output of ./configure for why");
	return -1;
#else

	struct nl_sock *nl_handle = NULL;
	struct nl_cache *nl_cache = NULL;
	struct genl_family *nl80211 = NULL;
	struct nl_msg *msg = NULL;

	if (nl80211_connect(interface, (void **) &nl_handle, 
						 (void **) &nl_cache, (void **) &nl80211, errstr) < 0)
		return -1;

	if ((msg = nlmsg_alloc()) == NULL) {
		snprintf(errstr, LORCON_STATUS_MAX, "%s failed to allocate message",
				 __FUNCTION__);
		nl80211_disconnect(nl_handle);
		return -1;
	}

	genlmsg_put(msg, 0, 0, genl_family_get_id(nl80211), 0, 0, 
				NL80211_CMD_SET_INTERFACE, 0);

	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, if_nametoindex(interface));
	NLA_PUT_U32(msg, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MONITOR);

	nl80211_parseflags(nflags, in_flags, msg);

	if (nl_send_auto_complete(nl_handle, msg) >= 0) { 
		if (nl_wait_for_ack(nl_handle) < 0) {
			goto nla_put_failure;
		}
	} else {
nla_put_failure:
		snprintf(errstr, LORCON_STATUS_MAX, "%s failed to set flags on "
				 "interface '%s': %s", __FUNCTION__, interface,
				 strerror(errno));
		nlmsg_free(msg);
		nl80211_disconnect(nl_handle);
		return -1;
	}

	nlmsg_free(msg);

	nl80211_disconnect(nl_handle);

	return 0;
#endif
}

int nl80211_setchannel_cache(const char *interface, void *handle,
							  void *family, int channel,
							  unsigned int chmode, char *errstr) {
#ifndef HAVE_LINUX_NETLINK
	snprintf(errstr, LORCON_STATUS_MAX, "LORCON was not compiled with netlink/nl80211 "
			 "support, check the output of ./configure for why");
	// Return the same error as we get if the device doesn't support nlfreq
	return -22;
#else
	struct nl_sock *nl_handle = (struct nl_sock *) handle;
	struct genl_family *nl80211 = (struct genl_family *) family;
	struct nl_msg *msg;
	int ret = 0;

	int chanmode[] = {
		NL80211_CHAN_NO_HT, NL80211_CHAN_HT20, 
		NL80211_CHAN_HT40PLUS, NL80211_CHAN_HT40MINUS
	};

	if (chmode > 4) {
		snprintf(errstr, LORCON_STATUS_MAX, "Invalid channel mode\n");
		return -1;
	}

	if ((msg = nlmsg_alloc()) == NULL) {
		snprintf(errstr, LORCON_STATUS_MAX, "nl80211_setchannel() failed to allocate "
				 "message");
		return -1;
	}

	genlmsg_put(msg, 0, 0, genl_family_get_id(nl80211), 0, 0, 
				NL80211_CMD_SET_WIPHY, 0);
	NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, if_nametoindex(interface));
	NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_FREQ, ChanToFreq(channel));
	NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_CHANNEL_TYPE, chanmode[chmode]);

	if ((ret = nl_send_auto_complete(nl_handle, msg)) >= 0) {
		if ((ret = nl_wait_for_ack(nl_handle)) < 0) 
			goto nla_put_failure;
	}

	nlmsg_free(msg);

	return 0;

nla_put_failure:
	snprintf(errstr, LORCON_STATUS_MAX, "nl80211_setchannel() could not set channel "
			 "%d/%d on interface '%s' err %d", channel, ChanToFreq(channel), 
			 interface, ret);
	nlmsg_free(msg);
	return ret;
#endif
}

int nl80211_setchannel(const char *interface, int channel, 
						unsigned int chmode, char *errstr) {
#ifndef HAVE_LINUX_NETLINK
	snprintf(errstr, LORCON_STATUS_MAX, "LORCON was not compiled with netlink/nl80211 "
			 "support, check the output of ./configure for why");
	// Return the same error as if the device doesn't support nl freq control
	// so we catch it elsewhere
	return -22;
#else
	struct nl_sock *nl_handle = NULL;
	struct nl_cache *nl_cache = NULL;
	struct genl_family *nl80211 = NULL;

	if (nl80211_connect(interface, (void **) &nl_handle, 
						 (void **) &nl_cache, (void **) &nl80211, errstr) < 0)
		return -1;

	int ret = 
		nl80211_setchannel_cache(interface, (void *) nl_handle,
								  (void *) nl80211, channel, chmode, errstr);

	nl80211_disconnect(nl_handle);

	return ret;
#endif
}

#ifdef HAVE_LINUX_NETLINK
static int nl80211_freqlist_cb(struct nl_msg *msg, void *arg) {
	struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = (struct genlmsghdr *) nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *tb_band[NL80211_BAND_ATTR_MAX + 1];
	struct nlattr *tb_freq[NL80211_FREQUENCY_ATTR_MAX + 1];
	struct nlattr *nl_band, *nl_freq;
	int rem_band, rem_freq, num_freq = 0;
	uint32_t freq;
	struct nl80211_channel_block *chanb = (struct nl80211_channel_block *) arg;

	nla_parse(tb_msg, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
			  genlmsg_attrlen(gnlh, 0), NULL);

	if (!tb_msg[NL80211_ATTR_WIPHY_BANDS]) {
		return NL_SKIP;
	}

	if (tb_msg[NL80211_ATTR_WIPHY_NAME]) {
		if (strcmp(nla_get_string(tb_msg[NL80211_ATTR_WIPHY_NAME]), 
				   chanb->phyname) != 0) {
			return NL_SKIP;
		}
	}

	// Count the number of channels
	for (nl_band = (struct nlattr *) nla_data(tb_msg[NL80211_ATTR_WIPHY_BANDS]),
		 rem_band = nla_len(tb_msg[NL80211_ATTR_WIPHY_BANDS]);
		 nla_ok(nl_band, rem_band); 
		 nl_band = (struct nlattr *) nla_next(nl_band, &rem_band)) {

		nla_parse(tb_band, NL80211_BAND_ATTR_MAX, (struct nlattr *) nla_data(nl_band),
				  nla_len(nl_band), NULL);

		for (nl_freq = (struct nlattr *) nla_data(tb_band[NL80211_BAND_ATTR_FREQS]),
			 rem_freq = nla_len(tb_band[NL80211_BAND_ATTR_FREQS]);
			 nla_ok(nl_freq, rem_freq); 
			 nl_freq = (struct nlattr *) nla_next(nl_freq, &rem_freq)) {

			nla_parse(tb_freq, NL80211_FREQUENCY_ATTR_MAX, 
					  (struct nlattr *) nla_data(nl_freq),
					  nla_len(nl_freq), NULL);

			if (!tb_freq[NL80211_FREQUENCY_ATTR_FREQ])
				continue;

			if (tb_freq[NL80211_FREQUENCY_ATTR_DISABLED])
				continue;

			num_freq++;
		}
	}

	chanb->nfreqs = num_freq;
	chanb->channel_list = malloc(sizeof(int) * num_freq);
	num_freq = 0;

	// Assemble a return
	for (nl_band = (struct nlattr *) nla_data(tb_msg[NL80211_ATTR_WIPHY_BANDS]),
		 rem_band = nla_len(tb_msg[NL80211_ATTR_WIPHY_BANDS]);
		 nla_ok(nl_band, rem_band); 
		 nl_band = (struct nlattr *) nla_next(nl_band, &rem_band)) {

		nla_parse(tb_band, NL80211_BAND_ATTR_MAX, (struct nlattr *) nla_data(nl_band),
				  nla_len(nl_band), NULL);

		for (nl_freq = (struct nlattr *) nla_data(tb_band[NL80211_BAND_ATTR_FREQS]),
			 rem_freq = nla_len(tb_band[NL80211_BAND_ATTR_FREQS]);
			 nla_ok(nl_freq, rem_freq); 
			 nl_freq = (struct nlattr *) nla_next(nl_freq, &rem_freq)) {

			nla_parse(tb_freq, NL80211_FREQUENCY_ATTR_MAX, 
					  (struct nlattr *) nla_data(nl_freq),
					  nla_len(nl_freq), NULL);

			if (!tb_freq[NL80211_FREQUENCY_ATTR_FREQ])
				continue;

			if (tb_freq[NL80211_FREQUENCY_ATTR_DISABLED])
				continue;

			freq = nla_get_u32(tb_freq[NL80211_FREQUENCY_ATTR_FREQ]);

			chanb->channel_list[num_freq++] = FreqToChan(freq);
		}
	}

	return NL_SKIP;
}
#endif

#ifdef HAVE_LINUX_NETLINK
static int nl80211_error_cb(struct sockaddr_nl *nla, struct nlmsgerr *err,
			 void *arg) {
	int *ret = (int *) arg;
	*ret = err->error;
	return NL_STOP;
}

static int nl80211_finish_cb(struct nl_msg *msg, void *arg) {
	int *ret = (int *) arg;
	*ret = 0;
	return NL_SKIP;
}
#endif

int nl80211_get_chanlist(const char *interface, int *ret_num_chans, int **ret_chan_list,
						  char *errstr) {
	nl80211_channel_block_t cblock;

#ifndef HAVE_LINUX_NETLINK
	snprintf(errstr, LORCON_STATUS_MAX, "LORCON was not compiled with netlink/nl80211 "
			 "support, check the output of ./configure for why");
	return NL80211_CHANLIST_NOT_NL80211;
#else
	void *handle = NULL, *cache = NULL, *family = NULL;
	struct nl_cb *cb;
	int err;
	struct nl_msg *msg;

	cblock.phyname = nl80211_find_parent(interface);
	if (strlen(cblock.phyname) == 0) {
		if (if_nametoindex(interface) <= 0) {
			snprintf(errstr, LORCON_STATUS_MAX, "Interface %s doesn't exist", interface);
			return NL80211_CHANLIST_NO_INTERFACE;
		} 

		snprintf(errstr, LORCON_STATUS_MAX, "LORCON could not find a parent phy device "
				 "for interface %s, it isn't nl80211?", interface);
		return NL80211_CHANLIST_NOT_NL80211;
	}

	if (nl80211_connect(interface, &handle, &cache, &family, errstr) < 0) {
		return NL80211_CHANLIST_GENERIC;
	}

	msg = nlmsg_alloc();
	cb = nl_cb_alloc(NL_CB_DEFAULT);

	err = 1;

	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, nl80211_freqlist_cb, &cblock);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, nl80211_finish_cb, &err);
	nl_cb_err(cb, NL_CB_CUSTOM, nl80211_error_cb, &err);

	genlmsg_put(msg, 0, 0, genl_family_get_id((struct genl_family *) family),
			  0, NLM_F_DUMP, NL80211_CMD_GET_WIPHY, 0);

	if (nl_send_auto_complete((struct nl_sock *) handle, msg) < 0) {
		snprintf(errstr, LORCON_STATUS_MAX, "%s: Failed to write nl80211 message",
				__FUNCTION__);
		nl80211_disconnect(handle);
		return NL80211_CHANLIST_GENERIC;
	}

	while (err)
		nl_recvmsgs((struct nl_sock *) handle, cb);

	nl80211_disconnect(handle);
	(*ret_num_chans) = cblock.nfreqs;
	(*ret_chan_list) = (int *) malloc(sizeof(int) * cblock.nfreqs);
	memcpy(*ret_chan_list, cblock.channel_list, sizeof(int) * cblock.nfreqs);

	free(cblock.channel_list);
	free(cblock.phyname);

	return (*ret_num_chans);
#endif
}

char *nl80211_find_parent(const char *interface) {
	DIR *devdir;
	struct dirent *devfile;
	char dirpath[1024];
	char *ret;

	DIR *ieeedir;
	struct dirent *ieeefile;
	char ieeedirpath[1024];

	snprintf(dirpath, 1024, "/sys/class/net/%s/phy80211/device", interface);

	if ((devdir = opendir(dirpath)) == NULL)
		return NULL;

	while ((devfile = readdir(devdir)) != NULL) {
		if (strlen(devfile->d_name) < 9)
			continue;

		if (strncmp("ieee80211:phy", devfile->d_name, 13) == 0) {
			ret = strdup(devfile->d_name + 10);
			closedir(devdir);
			return ret;
		}

        if (strncmp("ieee80211", devfile->d_name, 9) == 0) {
			snprintf(ieeedirpath, 1024, "%s/ieee80211", dirpath);

            if ((ieeedir = opendir(ieeedirpath)) != NULL) {
                while ((ieeefile = readdir(ieeedir)) != NULL) {
                    if (strncmp("phy", ieeefile->d_name, 3) == 0) {
                        ret = strdup(ieeefile->d_name);

                        closedir(ieeedir);
                        closedir(devdir);

                        return ret;
                    }
                }
            }

            closedir(ieeedir);
        }
	}

	closedir(devdir);

	return NULL;
}

#endif /* linux */

