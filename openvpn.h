/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2004 James Yonan <jim@yonan.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef OPENVPN_H
#define OPENVPN_H

#include "buffer.h"
#include "options.h"
#include "socket.h"
#include "crypto.h"
#include "ssl.h"
#include "packet_id.h"
#include "lzo.h"
#include "tun.h"
#include "interval.h"
#include "fragment.h"
#include "route.h"
#include "proxy.h"
#include "socks.h"
#include "sig.h"

/*
 * Signal information, including signal code
 * and descriptive text.
 */
struct signal_info
{
  volatile int signal_received;
  const char *signal_text;
};

/*
 * Our global key schedules, packaged thusly
 * to facilitate --persist-key.
 */

struct key_schedule
{
#ifdef USE_CRYPTO
  /* which cipher, HMAC digest, and key sizes are we using? */
  struct key_type key_type;

  /* pre-shared static key, read from a file */
  struct key_ctx_bi static_key;

#ifdef USE_SSL
  /* our global SSL context */
  SSL_CTX *ssl_ctx;

  /* optional authentication HMAC key for TLS control channel */
  struct key_ctx_bi tls_auth_key;

#endif				/* USE_SSL */
#else				/* USE_CRYPTO */
  int dummy;
#endif				/* USE_CRYPTO */
};

/*
 * struct packet_id_persist should be empty if we are not
 * building with crypto.
 */
#ifndef PACKET_ID_H
struct packet_id_persist
{
  int dummy;
};
static inline void
packet_id_persist_init (struct packet_id_persist *p)
{
}
#endif

/*
 * Contains the persist-across-restart OpenVPN tunnel instance state.
 * Reset only for SIGHUP restarts.
 */
struct context_1
{
  struct link_socket_addr link_socket_addr;
  struct tuntap tuntap;
  struct key_schedule ks;
  struct packet_id_persist pid_persist;
  struct route_list route_list;
  struct http_proxy_info http_proxy;
  struct socks_proxy_info socks_proxy;
};

/*
 * Contains the OpenVPN tunnel instance state, wiped across
 * SIGUSR1 and SIGHUP restarts.
 */
struct context_2
{
  /* our global wait event */
  struct event_wait event_wait;

#if PASSTOS_CAPABILITY
  /* used to get/set TOS. */
  uint8_t ptos;
  bool ptos_defined;
#endif

  /* declare various buffers */
  struct buffer to_tun;
  struct buffer to_link;
  struct buffer buf;
  struct buffer aux_buf;
  struct buffer nullbuf;

  /* tells us to free to_link buffer after it has been written to TCP/UDP port */
  bool free_to_link;

  struct link_socket link_socket;	/* socket used for TCP/UDP connection to remote */
  struct sockaddr_in to_link_addr;	/* IP address of remote */

  /* MTU frame parameters */
  struct frame frame;

  /* Object to handle advanced MTU negotiation and datagram fragmentation */
  struct fragment_master *fragment;
  struct frame frame_fragment;
  struct frame frame_fragment_omit;

#ifdef HAVE_GETTIMEOFDAY
  /*
   * Traffic shaper object.
   */
  struct shaper shaper;
#endif

  /*
   * Statistics
   */
  counter_type tun_read_bytes;
  counter_type tun_write_bytes;
  counter_type link_read_bytes;
  counter_type link_read_bytes_auth;
  counter_type link_write_bytes;

  /*
   * Timer objects for ping and inactivity
   * timeout features.
   */
  struct event_timeout wait_for_connect;
  struct event_timeout inactivity_interval;
  struct event_timeout ping_send_interval;
  struct event_timeout ping_rec_interval;

  /* the option strings must match across peers */
  char *options_string_local;
  char *options_string_remote;

  int occ_op;			/* INIT to -1 */
  int occ_n_tries;
  struct event_timeout occ_interval;

  /*
   * Keep track of maximum packet size received so far
   * (of authenticated packets).
   */
  int original_recv_size;	/* temporary */
  int max_recv_size_local;	/* max packet size received */
  int max_recv_size_remote;	/* max packet size received by remote */
  int max_send_size_local;	/* max packet size sent */
  int max_send_size_remote;	/* max packet size sent by remote */

  /* remote wants us to send back a load test packet of this size */
  int occ_mtu_load_size;

  struct event_timeout occ_mtu_load_test_interval;
  int occ_mtu_load_n_tries;

#ifdef USE_CRYPTO

  /*
   * TLS-mode crypto objects.
   */
#ifdef USE_SSL

  /* master OpenVPN SSL/TLS object */
  struct tls_multi *tls_multi;

#ifdef USE_PTHREAD

  /* object containing TLS thread state */
  struct thread_parms thread_parms;

  /* object sent to us by TLS thread */
  struct tt_ret tt_ret;

  /* did we open TLS thread? */
  bool thread_opened;

#endif

  /* used to optimize calls to tls_multi_process
     in single-threaded mode */
  struct interval tmp_int;

#endif

  /* workspace buffers used by crypto routines */
  struct buffer encrypt_buf;
  struct buffer decrypt_buf;

  /* passed to encrypt or decrypt, contains all
     crypto-related command line options related
     to data channel encryption/decryption */
  struct crypto_options crypto_options;

  /* used to keep track of data channel packet sequence numbers */
  struct packet_id packet_id;
#endif

  /*
   * LZO compression library objects.
   */
#ifdef USE_LZO
  struct buffer lzo_compress_buf;
  struct buffer lzo_decompress_buf;
  struct lzo_compress_workspace lzo_compwork;
#endif

  /*
   * Buffers used to read from TUN device
   * and TCP/UDP port.
   */
  struct buffer read_link_buf;
  struct buffer read_tun_buf;

  /*
   * IPv4 TUN device?
   */
  bool ipv4_tun;

  /* workspace for get_pid_file/write_pid */
  struct pid_state pid_state;

  /* workspace for --user/--group */
  struct user_state user_state;
  struct group_state group_state;

  /* temporary variable */
  bool did_we_daemonize;

  /* should we print R|W|r|w to console on packet transfers? */
  bool log_rw;

  /* route stuff */
  struct event_timeout route_wakeup;

  /* did we open tun/tap dev during this cycle? */
  bool did_open_tun;

  /*
   * Event loop info
   */

  /* Always set to current time. */
  time_t current;

  /* how long to wait before we will need to be serviced */
  struct timeval timeval;

  /* return from main event loop select (or windows equivalent) */
  int select_status;
};

/*
 * Contains all state information for one tunnel.
 */
struct context
{
  /* command line or config file options */
  struct options options;

  /* true on initial VPN iteration */
  bool first_time;

  /* signal info */
  struct signal_info *sig;

  /* level 1 context is preserved for
     SIGUSR1 restarts, but initialized
     for SIGHUP restarts */
  struct context_1 c1;

  /* level 2 context is initialized for all
     restarts (SIGUSR1 and SIGHUP) */
  struct context_2 c2;
};

/*
 * Macros for referencing objects which may not
 * have been compiled in.
 */
#if defined(USE_CRYPTO) && defined(USE_SSL)
#define TLS_MODE (c->c2.tls_multi != NULL)
#define PROTO_DUMP_FLAGS (check_debug_level (D_LINK_RW_VERBOSE) ? (PD_SHOW_DATA|PD_VERBOSE) : 0)
#define PROTO_DUMP(buf) protocol_dump(buf, \
				      PROTO_DUMP_FLAGS | \
				      (c->c2.tls_multi ? PD_TLS : 0) | \
				      (c->options.tls_auth_file ? c->c1.ks.key_type.hmac_length : 0) \
				      )
#else
#define TLS_MODE (false)
#define PROTO_DUMP(buf) format_hex (BPTR (buf), BLEN (buf), 80)
#endif

#ifdef USE_CRYPTO
#define MD5SUM(buf, len) md5sum(buf, len, 0)
#else
#define MD5SUM(buf, len) "[unavailable]"
#endif

#endif
