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

#ifdef WIN32
#include "config-win32.h"
#else
#include "config.h"
#endif

#include "syshead.h"

#include "forward.h"
#include "init.h"
#include "push.h"
#include "gremlin.h"
#include "mss.h"
#include "event.h"

#include "memdbg.h"

#include "forward-inline.h"
#include "occ-inline.h"
#include "ping-inline.h"

/* show event wait debugging info */

const char *
wait_status_string (struct context *c, struct gc_arena *gc)
{
  struct buffer out = alloc_buf_gc (64, gc);
  buf_printf (&out, "I/O WAIT %s|%s|%s|%s %s",
	      tun_stat (c->c1.tuntap, EVENT_READ, gc),
	      tun_stat (c->c1.tuntap, EVENT_WRITE, gc),
	      socket_stat (c->c2.link_socket, EVENT_READ, gc),
	      socket_stat (c->c2.link_socket, EVENT_WRITE, gc),
	      tv_string (&c->c2.timeval, gc));
  return BSTR (&out);
}

void
show_wait_status (struct context *c)
{
  struct gc_arena gc = gc_new ();
  msg (D_EVENT_WAIT, "%s", wait_status_string (c, &gc));
  gc_free (&gc);
}

/*
 * In TLS mode, let TLS level respond to any control-channel
 * packets which were received, or prepare any packets for
 * transmission.
 *
 * tmp_int is purely an optimization that allows us to call
 * tls_multi_process less frequently when there's not much
 * traffic on the control-channel.
 *
 */
#if defined(USE_CRYPTO) && defined(USE_SSL)
void
check_tls_dowork (struct context *c)
{
  interval_t wakeup = BIG_TIMEOUT;

  if (interval_test (&c->c2.tmp_int))
    {
      if (tls_multi_process
	  (c->c2.tls_multi, &c->c2.to_link, &c->c2.to_link_addr,
	   get_link_socket_info (c), &wakeup))
	{
	  update_time ();
	  interval_action (&c->c2.tmp_int);
	}

      interval_future_trigger (&c->c2.tmp_int, wakeup);
    }

  interval_schedule_wakeup (&c->c2.tmp_int, &wakeup);

  if (wakeup)
    context_reschedule_sec (c, wakeup);
}
#endif

#if defined(USE_CRYPTO) && defined(USE_SSL)
void
check_tls_errors_dowork (struct context *c)
{
  /* TLS errors are fatal in TCP mode */
  c->sig->signal_received = SIGUSR1;
  msg (D_STREAM_ERRORS, "Fatal decryption error (check_tls_errors_dowork), restarting");
  c->sig->signal_text = "tls-error";
}
#endif

/*
 * Handle incoming configuration
 * messages on the control channel.
 */
#if P2MP
void
check_incoming_control_channel_dowork (struct context *c)
{
  const int len = tls_test_payload_len (c->c2.tls_multi);
  if (len)
    {
      struct gc_arena gc = gc_new ();
      struct buffer buf = alloc_buf_gc (len, &gc);
      if (tls_rec_payload (c->c2.tls_multi, &buf))
	{
	  msg (D_PUSH, "PUSH: Received control message: '%s'", BSTR (&buf));
	  if (buf_string_match_head_str (&buf, "PUSH_"))
	    {
	      unsigned int option_types_found = 0;
	      const int status = process_incoming_push_msg (c,
							    &buf,
							    c->options.pull,
							    pull_permission_mask (),
							    &option_types_found);
	      if (status == PUSH_MSG_ERROR)
		msg (D_PUSH_ERRORS, "WARNING: Received bad push/pull message: %s", BSTR (&buf));
	      else if (status == PUSH_MSG_REPLY)
		{
		  do_up (c, true, option_types_found); /* delay bringing tun/tap up until --push parms received from remote */
		  event_timeout_clear (&c->c2.push_request_interval);
		}
	      else if (status == PUSH_MSG_REQUEST)
		{
		  //msg (D_PUSH, "PUSH: PUSH_MSG_REQUEST Replied");
		}
	      else if (status == PUSH_MSG_REQUEST_DEFERRED)
		{
		  //msg (D_PUSH, "PUSH: PUSH_MSG_REQUEST Deferred");
		}
	    }
	  else
	    {
	      msg (D_PUSH_ERRORS, "WARNING: Received unknown control message: %s", BSTR (&buf));
	    }
	}
      else
	{
	  msg (D_PUSH_ERRORS, "WARNING: Receive control message failed");
	}
      gc_free (&gc);
    }
}

/*
 * Periodically resend PUSH_REQUEST until PUSH message received
 */
void
check_push_request_dowork (struct context *c)
{
  send_push_request (c);
}

#endif

/*
 * Things that need to happen immediately after connection initiation should go here.
 */
void
check_connection_established_dowork (struct context *c)
{
  if (event_timeout_trigger (&c->c2.wait_for_connect, &c->c2.timeval, ETT_DEFAULT))
    {
      if (CONNECTION_ESTABLISHED (c))
	{
#if P2MP
	  /* if --pull was specified, send a push request to server */
	  if (c->c2.tls_multi && c->options.pull)
	    {
	      send_push_request (c);

	      /* if no reply, try again in 5 sec */
	      event_timeout_init (&c->c2.push_request_interval, 5, now);
	      reset_coarse_timers (c);
	    }
	  else
#endif
	    {
	      do_up (c, false, 0);
	    }

	  event_timeout_clear (&c->c2.wait_for_connect);
	}
    }
}

bool
send_control_channel_string (struct context *c, char *str)
{
#if defined(USE_CRYPTO) && defined(USE_SSL)
  if (c->c2.tls_multi) {
    struct buffer buf;
    bool stat;
    buf_set_read (&buf, str, strlen (str) + 1);
    stat = tls_send_payload (c->c2.tls_multi, &buf);
    interval_action (&c->c2.tmp_int);
    context_immediate_reschedule (c);
    msg (D_PUSH, "SENT CONTROL [%s]: '%s' (status=%d)",
	 tls_common_name (c->c2.tls_multi, false),
	 str,
	 (int) stat);
    return stat;
  }
#endif
  return true;
}

/*
 * Add routes.
 */
void
check_add_routes_dowork (struct context *c)
{
  if (test_routes (c->c1.route_list, c->c1.tuntap)
      || event_timeout_trigger (&c->c2.route_wakeup_expire, &c->c2.timeval, ETT_DEFAULT))
    {
      do_route (&c->options, c->c1.route_list, c->c1.tuntap);
      update_time ();
      event_timeout_clear (&c->c2.route_wakeup);
      event_timeout_clear (&c->c2.route_wakeup_expire);
    }
  else
    {
      msg (D_ROUTE, "Route: Waiting for TAP-Win32 interface to come up...");
      if (c->c2.route_wakeup.n != 1)
	event_timeout_init (&c->c2.route_wakeup, 1, now);
    }
}

/*
 * Should we exit due to inactivity timeout?
 */
void
check_inactivity_timeout_dowork (struct context *c)
{
  struct gc_arena gc = gc_new ();
  msg (M_INFO, "Inactivity timeout (--inactive), exiting");
  c->sig->signal_received = SIGTERM;
  c->sig->signal_text = "inactive";
  gc_free (&gc);
}

/*
 * Should we write timer-triggered status file.
 */
void
check_status_file_dowork (struct context *c)
{
  if (c->c1.status_output)
    print_status (c, c->c1.status_output);
}

/*
 * Should we deliver a datagram fragment to remote?
 */
void
check_fragment_dowork (struct context *c)
{
  struct link_socket_info *lsi = get_link_socket_info (c);

  /* OS MTU Hint? */
  if (lsi->mtu_changed && c->c2.ipv4_tun)
    {
      frame_adjust_path_mtu (&c->c2.frame_fragment, c->c2.link_socket->mtu,
			     c->options.proto);
      lsi->mtu_changed = false;
    }

  if (fragment_outgoing_defined (c->c2.fragment))
    {
      if (!c->c2.to_link.len)
	{
	  /* encrypt a fragment for output to TCP/UDP port */
	  ASSERT (fragment_ready_to_send (c->c2.fragment, &c->c2.buf, &c->c2.frame_fragment));
	  encrypt_sign (c, false);
	}
    }

  fragment_housekeeping (c->c2.fragment, &c->c2.frame_fragment, &c->c2.timeval);
}

/*
 * Compress, fragment, encrypt and HMAC-sign an outgoing packet.
 */
void
encrypt_sign (struct context *c, bool comp_frag)
{
  struct context_buffers *b = c->c2.buffers;
  if (comp_frag)
    {
#ifdef USE_LZO
      /* Compress the packet. */
      if (c->options.comp_lzo)
	lzo_compress (&c->c2.buf, b->lzo_compress_buf, &c->c2.lzo_compwork, &c->c2.frame);
#endif
      if (c->c2.fragment)
	fragment_outgoing (c->c2.fragment, &c->c2.buf, &c->c2.frame_fragment);
    }

#ifdef USE_CRYPTO
#ifdef USE_SSL
  /*
   * If TLS mode, get the key we will use to encrypt
   * the packet.
   */
  if (c->c2.tls_multi)
    {
      //tls_mutex_lock (c->c2.tls_multi);
      tls_pre_encrypt (c->c2.tls_multi, &c->c2.buf, &c->c2.crypto_options);
    }
#endif

  /*
   * Encrypt the packet and write an optional
   * HMAC signature.
   */
  openvpn_encrypt (&c->c2.buf, b->encrypt_buf, &c->c2.crypto_options, &c->c2.frame);
#endif
  /*
   * Get the address we will be sending the packet to.
   */
  link_socket_get_outgoing_addr (&c->c2.buf, get_link_socket_info (c),
				 &c->c2.to_link_addr);
#ifdef USE_CRYPTO
#ifdef USE_SSL
  /*
   * In TLS mode, prepend the appropriate one-byte opcode
   * to the packet which identifies it as a data channel
   * packet and gives the low-permutation version of
   * the key-id to the recipient so it knows which
   * decrypt key to use.
   */
  if (c->c2.tls_multi)
    {
      tls_post_encrypt (c->c2.tls_multi, &c->c2.buf);
      //tls_mutex_unlock (c->c2.tls_multi);
    }
#endif
#endif
  c->c2.to_link = c->c2.buf;
}

/*
 * Coarse timers work to 1 second resolution.
 */
static void
process_coarse_timers (struct context *c)
{
#ifdef USE_CRYPTO
  /* flush current packet-id to file once per 60
     seconds if --replay-persist was specified */
  check_packet_id_persist_flush (c);
#endif

  /* should we update status file? */
  check_status_file (c);

  /* process connection establishment items */
  check_connection_established (c);

#if P2MP
  /* see if we should send a push_request in response to --pull */
  check_push_request (c);
#endif

  /* process --route options */
  check_add_routes (c);

  /* possibly exit due to --inactive */
  check_inactivity_timeout (c);
  if (c->sig->signal_received)
    return;

  /* restart if ping not received */
  check_ping_restart (c);
  if (c->sig->signal_received)
    return;

  /* Should we send an OCC_REQUEST message? */
  check_send_occ_req (c);

  /* Should we send an MTU load test? */
  check_send_occ_load_test (c);

  /* Should we ping the remote? */
  check_ping_send (c);
}

static void
check_coarse_timers_dowork (struct context *c)
{
  const struct timeval save = c->c2.timeval;
  c->c2.timeval.tv_sec = BIG_TIMEOUT;
  c->c2.timeval.tv_usec = 0;
  process_coarse_timers (c);
  c->c2.coarse_timer_wakeup = now + c->c2.timeval.tv_sec; 

  msg (D_INTERVAL, "TIMER: coarse timer wakeup %d seconds", (int) c->c2.timeval.tv_sec);

  /* Is the coarse timeout NOT the earliest one? */
  if (c->c2.timeval.tv_sec > save.tv_sec)
    c->c2.timeval = save;
}

static inline void
check_coarse_timers (struct context *c)
{
  const time_t local_now = now;
  if (local_now >= c->c2.coarse_timer_wakeup)
    check_coarse_timers_dowork (c);
  else
    context_reschedule_sec (c, c->c2.coarse_timer_wakeup - local_now);
}

static void
check_timeout_random_component_dowork (struct context *c)
{
  const int update_interval = 10; /* seconds */
  c->c2.update_timeout_random_component = now + update_interval;
  c->c2.timeout_random_component.tv_usec = (time_t) get_random () & 0x000FFFFF;
  c->c2.timeout_random_component.tv_sec = 0;

  msg (D_INTERVAL, "RANDOM USEC=%d", (int) c->c2.timeout_random_component.tv_usec);
}

static inline void
check_timeout_random_component (struct context *c)
{
  if (now >= c->c2.update_timeout_random_component)
    check_timeout_random_component_dowork (c);
  if (c->c2.timeval.tv_sec >= 1)
    tv_add (&c->c2.timeval, &c->c2.timeout_random_component);
}

/*
 * Handle addition and removal of the 10-byte Socks5 header
 * in UDP packets.
 */

static inline void
socks_postprocess_incoming_link (struct context *c)
{
  if (c->c2.link_socket->socks_proxy && c->c2.link_socket->info.proto == PROTO_UDPv4)
    socks_process_incoming_udp (&c->c2.buf, &c->c2.from);
}

static inline void
socks_preprocess_outgoing_link (struct context *c,
				struct sockaddr_in **to_addr,
				int *size_delta)
{
  if (c->c2.link_socket->socks_proxy && c->c2.link_socket->info.proto == PROTO_UDPv4)
    {
      *size_delta += socks_process_outgoing_udp (&c->c2.to_link, &c->c2.to_link_addr);
      *to_addr = &c->c2.link_socket->socks_relay;
    }
}

/* undo effect of socks_preprocess_outgoing_link */
static inline void
link_socket_write_post_size_adjust (int *size,
				    int size_delta,
				    struct buffer *buf)
{
  if (size_delta > 0 && *size > size_delta)
    {
      *size -= size_delta;
      if (!buf_advance (buf, size_delta))
	*size = 0;
    }
}

void
read_incoming_link (struct context *c)
{
  /*
   * Set up for recvfrom call to read datagram
   * sent to our TCP/UDP port.
   */
  int status;

  perf_push (PERF_READ_IN_LINK);

  ASSERT (!c->c2.to_tun.len);

  c->c2.buf = c->c2.buffers->read_link_buf;
  ASSERT (buf_init (&c->c2.buf, FRAME_HEADROOM (&c->c2.frame)));
  status = link_socket_read (c->c2.link_socket, &c->c2.buf, MAX_RW_SIZE_LINK (&c->c2.frame), &c->c2.from);

  if (socket_connection_reset (c->c2.link_socket, status))
    {
      /* received a disconnect from a connection-oriented protocol */
      if (c->options.inetd)
	{
	  c->sig->signal_received = SIGTERM;
	  msg (D_STREAM_ERRORS, "Connection reset, inetd/xinetd exit [%d]", status);
	}
      else
	{
	  c->sig->signal_received = SIGUSR1;
	  msg (D_STREAM_ERRORS, "Connection reset, restarting [%d]", status);
	}
      c->sig->signal_text = "connection-reset";
      perf_pop ();
      return;
    }

  /* check recvfrom status */
  check_status (status, "read", c->c2.link_socket, NULL);

  /* Remove socks header if applicable */
  socks_postprocess_incoming_link (c);

  perf_pop ();
}

void
process_incoming_link (struct context *c)
{
  struct gc_arena gc = gc_new ();
  bool decrypt_status;
  struct link_socket_info *lsi = get_link_socket_info (c);

  perf_push (PERF_PROC_IN_LINK);

  if (c->c2.buf.len > 0)
    {
      c->c2.link_read_bytes += c->c2.buf.len;
      c->c2.original_recv_size = c->c2.buf.len;
    }
  else
    c->c2.original_recv_size = 0;

  /* take action to corrupt packet if we are in gremlin test mode */
  if (c->options.gremlin) {
    if (!ask_gremlin())
      c->c2.buf.len = 0;
    corrupt_gremlin (&c->c2.buf);
  }

  /* log incoming packet */
#ifdef LOG_RW
  if (c->c2.log_rw)
    fprintf (stderr, "R");
#endif
  msg (D_LINK_RW, "%s READ [%d] from %s: %s",
       proto2ascii (lsi->proto, true),
       BLEN (&c->c2.buf),
       print_sockaddr (&c->c2.from, &gc),
       PROTO_DUMP (&c->c2.buf, &gc));

  /*
   * Good, non-zero length packet received.
   * Commence multi-stage processing of packet,
   * such as authenticate, decrypt, decompress.
   * If any stage fails, it sets buf.len to 0 or -1,
   * telling downstream stages to ignore the packet.
   */
  if (c->c2.buf.len > 0)
    {
      if (!link_socket_verify_incoming_addr (&c->c2.buf, lsi, &c->c2.from))
	link_socket_bad_incoming_addr (&c->c2.buf, lsi, &c->c2.from);

#ifdef USE_CRYPTO
#ifdef USE_SSL
      if (c->c2.tls_multi)
	{
	  /*
	   * If tls_pre_decrypt returns true, it means the incoming
	   * packet was a good TLS control channel packet.  If so, TLS code
	   * will deal with the packet and set buf.len to 0 so downstream
	   * stages ignore it.
	   *
	   * If the packet is a data channel packet, tls_pre_decrypt
	   * will load crypto_options with the correct encryption key
	   * and return false.
	   */
	  //tls_mutex_lock (c->c2.tls_multi);
	  if (tls_pre_decrypt (c->c2.tls_multi, &c->c2.from, &c->c2.buf, &c->c2.crypto_options))
	    {
	      interval_action (&c->c2.tmp_int);

	      /* reset packet received timer if TLS packet */
	      if (c->options.ping_rec_timeout)
		event_timeout_reset (&c->c2.ping_rec_interval);
	    }
	}
#endif /* USE_SSL */

      /* authenticate and decrypt the incoming packet */
      decrypt_status = openvpn_decrypt (&c->c2.buf, c->c2.buffers->decrypt_buf, &c->c2.crypto_options, &c->c2.frame);

#ifdef USE_SSL
      if (c->c2.tls_multi)
	{
	  //tls_mutex_unlock (c->c2.tls_multi);
	}
#endif
      
      if (!decrypt_status && link_socket_connection_oriented (c->c2.link_socket))
	{
	  /* decryption errors are fatal in TCP mode */
	  c->sig->signal_received = SIGUSR1;
	  msg (D_STREAM_ERRORS, "Fatal decryption error (process_incoming_link), restarting");
	  c->sig->signal_text = "decryption-error";
	  goto done;
	}

#endif /* USE_CRYPTO */

      if (c->c2.fragment)
	fragment_incoming (c->c2.fragment, &c->c2.buf, &c->c2.frame_fragment);

#ifdef USE_LZO
      /* decompress the incoming packet */
      if (c->options.comp_lzo)
	lzo_decompress (&c->c2.buf, c->c2.buffers->lzo_decompress_buf, &c->c2.lzo_compwork, &c->c2.frame);
#endif
      /*
       * Set our "official" outgoing address, since
       * if buf.len is non-zero, we know the packet
       * authenticated.  In TLS mode we do nothing
       * because TLS mode takes care of source address
       * authentication.
       *
       * Also, update the persisted version of our packet-id.
       */
      if (!TLS_MODE)
	link_socket_set_outgoing_addr (&c->c2.buf, lsi, &c->c2.from, NULL);

      /* reset packet received timer */
      if (c->options.ping_rec_timeout && c->c2.buf.len > 0)
	event_timeout_reset (&c->c2.ping_rec_interval);

      /* increment authenticated receive byte count */
      if (c->c2.buf.len > 0)
	{
	  c->c2.link_read_bytes_auth += c->c2.buf.len;
	  c->c2.max_recv_size_local = max_int (c->c2.original_recv_size, c->c2.max_recv_size_local);
	}

      /* Did we just receive an openvpn ping packet? */
      if (is_ping_msg (&c->c2.buf))
	{
	  msg (D_PACKET_CONTENT, "RECEIVED PING PACKET");
	  c->c2.buf.len = 0; /* drop packet */
	}

      /* Did we just receive an OCC packet? */
      if (is_occ_msg (&c->c2.buf))
	process_received_occ_msg (c);
      
      c->c2.to_tun = c->c2.buf;

      /* to_tun defined + unopened tuntap can cause deadlock */
      if (!tuntap_defined (c->c1.tuntap))
	c->c2.to_tun.len = 0;
    }
  else
    {
      buf_reset (&c->c2.to_tun);
    }
 done:
  perf_pop ();
  gc_free (&gc);
}

void
read_incoming_tun (struct context *c)
{
  perf_push (PERF_READ_IN_TUN);

  /*
   * Setup for read() call on TUN/TAP device.
   */
  ASSERT (!c->c2.to_link.len);

  c->c2.buf = c->c2.buffers->read_tun_buf;
#ifdef TUN_PASS_BUFFER
  read_tun_buffered (c->c1.tuntap, &c->c2.buf, MAX_RW_SIZE_TUN (&c->c2.frame));
#else
  ASSERT (buf_init (&c->c2.buf, FRAME_HEADROOM (&c->c2.frame)));
  ASSERT (buf_safe (&c->c2.buf, MAX_RW_SIZE_TUN (&c->c2.frame)));
  c->c2.buf.len = read_tun (c->c1.tuntap, BPTR (&c->c2.buf), MAX_RW_SIZE_TUN (&c->c2.frame));
#endif

  /* Was TUN/TAP interface stopped? */
  if (tuntap_stop (c->c2.buf.len))
    {
      c->sig->signal_received = SIGTERM;
      c->sig->signal_text = "tun-stop";
      msg (M_INFO, "TUN/TAP interface has been stopped, exiting");
      perf_pop ();
      return;		  
    }

  /* Check the status return from read() */
  check_status (c->c2.buf.len, "read from TUN/TAP", NULL, c->c1.tuntap);

  perf_pop ();
}

void
process_incoming_tun (struct context *c)
{
  struct gc_arena gc = gc_new ();

  perf_push (PERF_PROC_IN_TUN);

  if (c->c2.buf.len > 0)
    c->c2.tun_read_bytes += c->c2.buf.len;

#ifdef LOG_RW
  if (c->c2.log_rw)
    fprintf (stderr, "r");
#endif

  /* Show packet content */
  msg (D_TUN_RW, "TUN READ [%d]: %s md5=%s",
       BLEN (&c->c2.buf),
       format_hex (BPTR (&c->c2.buf), BLEN (&c->c2.buf), 80, &gc),
       MD5SUM (BPTR (&c->c2.buf), BLEN (&c->c2.buf), &gc));

  if (c->c2.buf.len > 0)
    {
      /*
       * The --passtos and --mssfix options require
       * us to examine the IPv4 header.
       */
      process_ipv4_header (c, PIPV4_PASSTOS|PIPV4_MSSFIX, &c->c2.buf);
      encrypt_sign (c, true);
    }
  else
    {
      buf_reset (&c->c2.to_link);
    }
  perf_pop ();
  gc_free (&gc);
}

void
process_ipv4_header (struct context *c, unsigned int flags, struct buffer *buf)
{
  if (!c->options.mssfix)
    flags &= ~PIPV4_MSSFIX;
#if PASSTOS_CAPABILITY
  if (!c->options.passtos)
    flags &= ~PIPV4_PASSTOS;
#endif

  if (buf->len > 0)
    {
      /*
       * The --passtos and --mssfix options require
       * us to examine the IPv4 header.
       */
#if PASSTOS_CAPABILITY
      if (flags & (PIPV4_PASSTOS|PIPV4_MSSFIX))
#else
      if (flags & PIPV4_MSSFIX)
#endif
	{
	  struct buffer ipbuf = *buf;
	  if (is_ipv4 (TUNNEL_TYPE (c->c1.tuntap), &ipbuf))
	    {
#if PASSTOS_CAPABILITY
	      /* extract TOS from IP header */
	      if (flags & PIPV4_PASSTOS)
		link_socket_extract_tos (c->c2.link_socket, &ipbuf);
#endif
			  
	      /* possibly alter the TCP MSS */
	      if (flags & PIPV4_MSSFIX)
		mss_fixup (&ipbuf, MTU_TO_MSS (TUN_MTU_SIZE_DYNAMIC (&c->c2.frame)));
	    }
	}
    }
}

void
process_outgoing_link (struct context *c)
{
  struct gc_arena gc = gc_new ();

  perf_push (PERF_PROC_OUT_LINK);

  if (c->c2.to_link.len > 0 && c->c2.to_link.len <= EXPANDED_SIZE (&c->c2.frame))
    {
      /*
       * Setup for call to send/sendto which will send
       * packet to remote over the TCP/UDP port.
       */
      int size;
      ASSERT (addr_defined (&c->c2.to_link_addr));

      /* In gremlin-test mode, we may choose to drop this packet */
      if (!c->options.gremlin || ask_gremlin())
	{
	  /*
	   * Let the traffic shaper know how many bytes
	   * we wrote.
	   */
#ifdef HAVE_GETTIMEOFDAY
	  if (c->options.shaper)
	    shaper_wrote_bytes (&c->c2.shaper, BLEN (&c->c2.to_link)
				+ datagram_overhead (c->options.proto));
#endif
	  /*
	   * Let the pinger know that we sent a packet.
	   */
	  if (c->options.ping_send_timeout)
	    event_timeout_reset (&c->c2.ping_send_interval);

#if PASSTOS_CAPABILITY
	  /* Set TOS */
	  link_socket_set_tos (c->c2.link_socket);
#endif

	  /* Log packet send */
#ifdef LOG_RW
	  if (c->c2.log_rw)
	    fprintf (stderr, "W");
#endif
	  msg (D_LINK_RW, "%s WRITE [%d] to %s: %s",
	       proto2ascii (c->c2.link_socket->info.proto, true),
	       BLEN (&c->c2.to_link),
	       print_sockaddr (&c->c2.to_link_addr, &gc),
	       PROTO_DUMP (&c->c2.to_link, &gc));

	  /* Packet send complexified by possible Socks5 usage */
	  {
	    struct sockaddr_in *to_addr = &c->c2.to_link_addr;
	    int size_delta = 0;

	    /* If Socks5 over UDP, prepend header */
	    socks_preprocess_outgoing_link (c, &to_addr, &size_delta);

	    /* Send packet */
	    size = link_socket_write (c->c2.link_socket, &c->c2.to_link, to_addr);

	    /* Undo effect of prepend */
	    link_socket_write_post_size_adjust (&size, size_delta, &c->c2.to_link);
	  }

	  if (size > 0)
	    {
	      c->c2.max_send_size_local = max_int (size, c->c2.max_send_size_local);
	      c->c2.link_write_bytes += size;
	    }
	}
      else
	size = 0;

      /* Check return status */
      check_status (size, "write", c->c2.link_socket, NULL);

      if (size > 0)
	{
	  /* Did we write a different size packet than we intended? */
	  if (size != BLEN (&c->c2.to_link))
	    msg (D_LINK_ERRORS,
		 "TCP/UDP packet was truncated/expanded on write to %s (tried=%d,actual=%d)",
		 print_sockaddr (&c->c2.to_link_addr, &gc),
		 BLEN (&c->c2.to_link),
		 size);
	}
    }
  else
    {
      if (c->c2.to_link.len > 0)
	msg (D_LINK_ERRORS, "TCP/UDP packet too large on write to %s (tried=%d,max=%d)",
	     print_sockaddr (&c->c2.to_link_addr, &gc),
	     c->c2.to_link.len,
	     EXPANDED_SIZE (&c->c2.frame));
    }

  buf_reset (&c->c2.to_link);

  perf_pop ();
  gc_free (&gc);
}

void
process_outgoing_tun (struct context *c)
{
  struct gc_arena gc = gc_new ();

  perf_push (PERF_PROC_OUT_TUN);

  /*
   * Set up for write() call to TUN/TAP
   * device.
   */
  ASSERT (c->c2.to_tun.len > 0);

  /*
   * The --mssfix option requires
   * us to examine the IPv4 header.
   */
  process_ipv4_header (c, PIPV4_MSSFIX, &c->c2.to_tun);

  if (c->c2.to_tun.len <= MAX_RW_SIZE_TUN (&c->c2.frame))
    {
      /*
       * Write to TUN/TAP device.
       */
      int size;

#ifdef LOG_RW
      if (c->c2.log_rw)
	fprintf (stderr, "w");
#endif
      msg (D_TUN_RW, "TUN WRITE [%d]: %s md5=%s",
	   BLEN (&c->c2.to_tun),
	   format_hex (BPTR (&c->c2.to_tun), BLEN (&c->c2.to_tun), 80, &gc),
	   MD5SUM (BPTR (&c->c2.to_tun), BLEN (&c->c2.to_tun), &gc));

#ifdef TUN_PASS_BUFFER
      size = write_tun_buffered (c->c1.tuntap, &c->c2.to_tun);
#else
      size = write_tun (c->c1.tuntap, BPTR (&c->c2.to_tun), BLEN (&c->c2.to_tun));
#endif

      if (size > 0)
	c->c2.tun_write_bytes += size;
      check_status (size, "write to TUN/TAP", NULL, c->c1.tuntap);

      /* check written packet size */
      if (size > 0)
	{
	  /* Did we write a different size packet than we intended? */
	  if (size != BLEN (&c->c2.to_tun))
	    msg (D_LINK_ERRORS,
		 "TUN/TAP packet was destructively fragmented on write to %s (tried=%d,actual=%d)",
		 c->c1.tuntap->actual_name,
		 BLEN (&c->c2.to_tun),
		 size);
	}
    }
  else
    {
      /*
       * This should never happen, probably indicates some kind
       * of MTU mismatch.
       */
      msg (D_LINK_ERRORS, "tun packet too large on write (tried=%d,max=%d)",
	   c->c2.to_tun.len,
	   MAX_RW_SIZE_TUN (&c->c2.frame));
    }

  /*
   * Putting the --inactive timeout reset here, ensures that we will timeout
   * if the remote goes away, even if we are trying to send data to the
   * remote and failing.
   */
  register_activity (c);

  buf_reset (&c->c2.to_tun);

  perf_pop ();
  gc_free (&gc);
}

void
pre_select (struct context *c)
{
  /* make sure current time (now) is updated on function entry */

  /*
   * Start with an effectively infinite timeout, then let it
   * reduce to a timeout that reflects the component which
   * needs the earliest service.
   */
  c->c2.timeval.tv_sec = BIG_TIMEOUT;
  c->c2.timeval.tv_usec = 0;

#if defined(WIN32)
  if (check_debug_level (D_TAP_WIN32_DEBUG))
    {
      c->c2.timeval.tv_sec = 1;
      if (tuntap_defined (c->c1.tuntap))
	tun_show_debug (c->c1.tuntap);
    }
#endif

  /* check coarse timers? */
  check_coarse_timers (c);
  if (c->sig->signal_received)
    return;

  /* Does TLS need service? */
  check_tls (c);

  /* In certain cases, TLS errors will require a restart */
  check_tls_errors (c);
  if (c->sig->signal_received)
    return;

  /* check for incoming configuration info on the control channel */
  check_incoming_control_channel (c);

  /* Should we send an OCC message? */
  check_send_occ_msg (c);

  /* Should we deliver a datagram fragment to remote? */
  check_fragment (c);

  /* Update random component of timeout */
  check_timeout_random_component (c);
}

/*
 * Wait for I/O events.  Used for both TCP & UDP sockets
 * in point-to-point mode and for UDP sockets in
 * point-to-multipoint mode.
 */

void
io_wait (struct context *c,
	 const unsigned int flags)
{
  unsigned int socket = 0;
  unsigned int tuntap = 0;
  struct event_set_return esr[3];

  /* These shifts all depend on EVENT_READ and EVENT_WRITE */
  static const int socket_shift = 0; /* depends on SOCKET_READ and SOCKET_WRITE */
  static const int tun_shift = 2;    /* depends on TUN_READ and TUN_WRITE */
  static const int err_shift = 4;    /* depends on ES_ERROR */

  /*
   * Decide what kind of events we want to wait for.
   */
  event_reset (c->c2.event_set);

  /*
   * On win32 we use the keyboard or an event object as a source
   * of asynchronous signals.
   */
  if (flags & IOW_WAIT_SIGNAL)
    wait_signal (c->c2.event_set, (void*)&err_shift);

  /*
   * If outgoing data (for TCP/UDP port) pending, wait for ready-to-send
   * status from TCP/UDP port. Otherwise, wait for incoming data on
   * TUN/TAP device.
   */
  if (flags & IOW_TO_LINK)
    {
      if (flags & IOW_SHAPER)
	{
	  /*
	   * If sending this packet would put us over our traffic shaping
	   * quota, don't send -- instead compute the delay we must wait
	   * until it will be OK to send the packet.
	   */
#ifdef HAVE_GETTIMEOFDAY
	  int delay = 0;

	  /* set traffic shaping delay in microseconds */
	  if (c->options.shaper)
	    delay = max_int (delay, shaper_delay (&c->c2.shaper));
	  
	  if (delay < 1000)
	    {
	      socket |= EVENT_WRITE;
	    }
	  else
	    {
	      shaper_soonest_event (&c->c2.timeval, delay);
	    }
#else /* HAVE_GETTIMEOFDAY */
	  socket |= EVENT_WRITE;
#endif /* HAVE_GETTIMEOFDAY */
	}
      else
	{
	  socket |= EVENT_WRITE;
	}
    }
  else if (!((flags & IOW_FRAG) && TO_LINK_FRAG (c)))
    {
      if (flags & IOW_READ_TUN)
	tuntap |= EVENT_READ;
    }

  /*
   * If outgoing data (for TUN/TAP device) pending, wait for ready-to-send status
   * from device.  Otherwise, wait for incoming data on TCP/UDP port.
   */
  if (flags & IOW_TO_TUN)
    {
      tuntap |= EVENT_WRITE;
    }
  else
    {
      if (flags & IOW_READ_LINK)
	socket |= EVENT_READ;
    }

  /*
   * outgoing bcast buffer waiting to be sent?
   */
  if (flags & IOW_MBUF)
    socket |= EVENT_WRITE;

  /*
   * Force wait on TUN input, even if also waiting on TCP/UDP output
   */
  if (flags & IOW_READ_TUN_FORCE)
    tuntap |= EVENT_READ;

  /*
   * Configure event wait based on socket, tuntap flags.
   */
  socket_set (c->c2.link_socket, c->c2.event_set, socket, (void*)&socket_shift, NULL);
  tun_set (c->c1.tuntap, c->c2.event_set, tuntap, (void*)&tun_shift, NULL);

  /*
   * Possible scenarios:
   *  (1) tcp/udp port has data available to read
   *  (2) tcp/udp port is ready to accept more data to write
   *  (3) tun dev has data available to read
   *  (4) tun dev is ready to accept more data to write
   *  (5) we received a signal (handler sets signal_received)
   *  (6) timeout (tv) expired
   */

  c->c2.event_set_status = ES_ERROR;

  if (!c->sig->signal_received)
    {
      if (!(flags & IOW_CHECK_RESIDUAL) || !socket_read_residual (c->c2.link_socket))
	{
	  int status;

	  if (check_debug_level (D_EVENT_WAIT))
	    show_wait_status (c);

	  /*
	   * Wait for something to happen.
	   */
	  status = event_wait (c->c2.event_set, &c->c2.timeval, esr, SIZE(esr));

	  check_status (status, "event_wait", NULL, NULL);

	  if (status > 0)
	    {
	      int i;
	      c->c2.event_set_status = 0;
	      for (i = 0; i < status; ++i)
		{
		  const struct event_set_return *e = &esr[i];
		  c->c2.event_set_status |= ((e->rwflags & 3) << *((int*)e->arg));
		}
	    }
	  else if (status == 0)
	    {
	      c->c2.event_set_status = ES_TIMEOUT;
	    }
	}
      else
	{
	  c->c2.event_set_status = SOCKET_READ;
	}
    }

  /* 'now' should always be a reasonably up-to-date timestamp */
  update_time ();

  /* set signal_received if a signal was received */
  if (c->c2.event_set_status & ES_ERROR)
    get_signal (&c->sig->signal_received);

  msg (D_EVENT_WAIT, "I/O WAIT status=0x%04x", c->c2.event_set_status);
}

void
process_io (struct context *c)
{
  const unsigned int status = c->c2.event_set_status;

  /* TCP/UDP port ready to accept write */
  if (status & SOCKET_WRITE)
    {
      process_outgoing_link (c);
    }
  /* TUN device ready to accept write */
  else if (status & TUN_WRITE)
    {
      process_outgoing_tun (c);
    }
  /* Incoming data on TCP/UDP port */
  else if (status & SOCKET_READ)
    {
      read_incoming_link (c);
      if (!IS_SIG (c))
	process_incoming_link (c);
    }
  /* Incoming data on TUN device */
  else if (status & TUN_READ)
    {
      read_incoming_tun (c);
      if (!IS_SIG (c))
	process_incoming_tun (c);
    }
}
