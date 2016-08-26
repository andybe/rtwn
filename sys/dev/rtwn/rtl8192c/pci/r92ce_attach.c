/*	$OpenBSD: if_rtwn.c,v 1.6 2015/08/28 00:03:53 deraadt Exp $	*/

/*-
 * Copyright (c) 2010 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2015 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2016 Andriy Voskoboinyk <avos@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/linker.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_nop.h>

#include <dev/rtwn/pci/rtwn_pci_var.h>

#include <dev/rtwn/rtl8192c/r92c_var.h>

#include <dev/rtwn/rtl8192c/pci/r92ce.h>
#include <dev/rtwn/rtl8192c/pci/r92ce_priv.h>
#include <dev/rtwn/rtl8192c/pci/r92ce_reg.h>
#include <dev/rtwn/rtl8192c/pci/r92ce_tx_desc.h>


void	r92ce_attach(struct rtwn_pci_softc *);

static void
r92ce_postattach(struct rtwn_softc *sc)
{
	struct r92c_softc *rs = sc->sc_priv;
	struct ieee80211com *ic = &sc->sc_ic;

	if (!(rs->chip & R92C_CHIP_92C) &&
	    rs->board_type == R92C_BOARD_TYPE_HIGHPA)
		rs->rs_txagc = &rtl8188ru_txagc[0];
	else
		rs->rs_txagc = &rtl8192cu_txagc[0];

	if ((rs->chip & (R92C_CHIP_UMC_A_CUT | R92C_CHIP_92C)) ==
	    R92C_CHIP_UMC_A_CUT)
		sc->fwname = "rtwn-rtl8192cfwE";
	else
		sc->fwname = "rtwn-rtl8192cfwE_B";
	sc->fwsig = 0x88c;

	rs->rs_scan_start = ic->ic_scan_start;
	ic->ic_scan_start = r92c_scan_start;
	rs->rs_scan_end = ic->ic_scan_end;
	ic->ic_scan_end = r92c_scan_end;
}

static void
r92ce_attach_private(struct rtwn_softc *sc)
{
	struct r92c_softc *rs;

	rs = malloc(sizeof(struct r92c_softc), M_TEMP, M_WAITOK | M_ZERO);

	rs->rs_set_bw20			= r92c_set_bw20;
        rs->rs_get_txpower		= r92c_get_txpower;
        rs->rs_set_gain			= r92c_set_gain;
        rs->rs_tx_setup_ampdu		= r92c_tx_setup_ampdu;
        rs->rs_tx_setup_hwseq		= r92c_tx_setup_hwseq;
        rs->rs_tx_setup_macid		= r92c_tx_setup_macid;

	/* XXX TODO: test with net80211 ratectl! */
#ifndef RTWN_WITHOUT_UCODE
	rs->rs_c2h_timeout		= hz;
                
	callout_init_mtx(&rs->rs_c2h_report, &sc->sc_mtx, 0);
#endif

	rs->rf_read_delay[0]		= 1000;
	rs->rf_read_delay[1]		= 1000;
	rs->rf_read_delay[2]		= 1000;

	sc->sc_priv = rs;
}

static void
r92ce_adj_devcaps(struct rtwn_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;

	/* XXX TODO: test everything that removed here before enabling. */
	/* XX do NOT enable PMGT until RSVD_PAGE command will not be fixed. */
	ic->ic_caps &= ~(
		  IEEE80211_C_IBSS	/* check beaconing / tsf */
		| IEEE80211_C_HOSTAP	/* the same */
		| IEEE80211_C_PMGT	/* check null frame / device usability */
		| IEEE80211_C_SWAMSDUTX
		| IEEE80211_C_FF
	);

	ic->ic_htcaps = 0;
}

void
r92ce_attach(struct rtwn_pci_softc *pc)
{
	struct rtwn_softc *sc		= &pc->pc_sc;

	pc->pc_setup_tx_desc		= r92ce_setup_tx_desc;
	pc->pc_tx_postsetup		= r92ce_tx_postsetup;
	pc->pc_reset_tx_desc		= r92ce_reset_tx_desc;
	pc->pc_enable_intr		= r92ce_enable_intr;

	pc->pc_qmap			= 0xf771;
	pc->tcr				=
	    R92C_TCR_CFENDFORM | (1 << 12) | (1 << 13);

	sc->sc_flags			= 0;

	sc->sc_start_xfers		= r92ce_start_xfers;
	sc->sc_set_chan			= r92c_set_chan;
	sc->sc_fill_tx_desc		= r92c_fill_tx_desc;
	sc->sc_fill_tx_desc_raw		= r92c_fill_tx_desc_raw;
	sc->sc_fill_tx_desc_null	= r92c_fill_tx_desc_null; /* XXX recheck */
	sc->sc_get_rssi_cck		= r92c_get_rssi_cck;
	sc->sc_get_rssi_ofdm		= r92c_get_rssi_ofdm;
	sc->sc_classify_intr		= r92ce_classify_intr;
	sc->sc_check_frame		= rtwn_nop_int_softc_mbuf;
	sc->sc_rf_read			= r92c_rf_read;
	sc->sc_rf_write			= r92c_rf_write;
	sc->sc_check_condition		= r92c_check_condition;	/* XXX TODO */
	sc->sc_efuse_postread		= r92c_efuse_postread;
	sc->sc_parse_rom		= r92c_parse_rom;
	sc->sc_set_led			= r92ce_set_led;
	sc->sc_power_on			= r92ce_power_on;
	sc->sc_power_off		= r92ce_power_off;
#ifndef RTWN_WITHOUT_UCODE
	sc->sc_fw_reset			= r92ce_fw_reset;
#endif
	sc->sc_set_page_size		= r92c_set_page_size;
	sc->sc_lc_calib			= r92c_lc_calib;
	sc->sc_iq_calib			= r92ce_iq_calib;
	sc->sc_read_chipid_vendor	= r92c_read_chipid_vendor;
	sc->sc_adj_devcaps		= r92ce_adj_devcaps;
	sc->sc_vap_preattach		= rtwn_nop_softc_vap;
	sc->sc_postattach		= r92ce_postattach;
	sc->sc_detach_private		= r92c_detach_private;
	sc->sc_set_media_status		= r92c_joinbss_rpt;
#ifndef RTWN_WITHOUT_UCODE
	sc->sc_set_rsvd_page		= r92c_set_rsvd_page;
	sc->sc_set_pwrmode		= r92c_set_pwrmode;
#endif
	sc->sc_beacon_init		= r92c_beacon_init;
	sc->sc_beacon_enable		= r92c_beacon_enable;
	sc->sc_beacon_set_rate		= rtwn_nop_void_int;
	sc->sc_beacon_select		= rtwn_nop_softc_int;
	sc->sc_temp_measure		= r92c_temp_measure;
	sc->sc_temp_read		= r92c_temp_read;
	sc->sc_init_tx_agg		= rtwn_nop_softc;
	sc->sc_init_rx_agg		= rtwn_nop_softc;
	sc->sc_init_ampdu		= r92ce_init_ampdu;
	sc->sc_init_intr		= r92ce_init_intr;
	sc->sc_init_rf_workaround	= rtwn_nop_softc;
	sc->sc_init_edca		= r92ce_init_edca;
	sc->sc_init_bb			= r92ce_init_bb;
	sc->sc_init_rf			= r92c_init_rf;
	sc->sc_init_antsel		= rtwn_nop_softc;
	sc->sc_post_init		= r92ce_post_init;
	sc->sc_init_bcnq1_boundary	= rtwn_nop_int_softc;

	sc->chan_list_5ghz		= NULL;
	sc->chan_num_5ghz		= 0;

	sc->mac_prog			= &rtl8192ce_mac[0];
	sc->mac_size			= nitems(rtl8192ce_mac);
	sc->bb_prog			= &rtl8192ce_bb[0];
	sc->bb_size			= nitems(rtl8192ce_bb);
	sc->agc_prog			= &rtl8192ce_agc[0];
	sc->agc_size			= nitems(rtl8192ce_agc);
	sc->rf_prog			= &rtl8192c_rf[0];

	sc->ntx				= 2;	/* XXX TODO: static configuration */
	sc->page_count			= R92CE_TX_PAGE_COUNT;
	sc->pktbuf_count		= R92C_TXPKTBUF_COUNT;
	sc->tx_boundary			= R92CE_TX_PAGE_BOUNDARY;
	sc->ackto			= 0x40;
	sc->npubqpages			= R92CE_PUBQ_NPAGES;
	sc->page_size			= R92C_TX_PAGE_SIZE;

	sc->txdesc_len			= sizeof(struct r92ce_tx_desc);
	sc->efuse_maxlen		= R92C_EFUSE_MAX_LEN;
	sc->efuse_maplen		= R92C_EFUSE_MAP_LEN;
	sc->rx_dma_size			= R92C_RX_DMA_BUFFER_SIZE;

	sc->macid_limit			= R92C_MACID_MAX + 1;
	sc->cam_entry_limit		= R92C_CAM_ENTRY_COUNT;
	sc->fwsize_limit		= R92C_MAX_FW_SIZE;

	sc->tx_agg_desc_num		= 6;	/* XXX move to usb_softc */

	sc->bcn_status_reg[0]		= R92C_TDECTRL;
	sc->bcn_status_reg[1]		= R92C_TDECTRL;
	sc->rcr				= 0;

	if (sc->sc_hwcrypto != RTWN_CRYPTO_SW) {
		/* XXX untested */
		device_printf(sc->sc_dev,
		    "%s: warning: hardware crypto enabled\n",
		    __func__);
	}

	r92ce_attach_private(sc);	
}
