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

#if P2MP

#include "multi.h"
#include "forward-inline.h"

#include "memdbg.h"

/*
 * TCP States
 */
#define TA_UNDEF                 0
#define TA_SOCKET_READ           1
#define TA_SOCKET_READ_RESIDUAL  2
#define TA_SOCKET_WRITE          3
#define TA_SOCKET_WRITE_READY    4
#define TA_SOCKET_WRITE_DEFERRED 5
#define TA_TUN_READ              6
#define TA_TUN_WRITE             7
#define TA_INITIAL               8
#define TA_TIMEOUT               9

#define MTCP_SOCKET ((void*)1)
#define MTCP_TUN    ((void*)2)
#define MTCP_SIG    ((void*)3) /* Only on Windows */
#define MTCP_N      ((void*)4)

struct ta_iow_flags
{
  unsigned int flags;
  unsigned int ret;
  unsigned int tun;
  unsigned int sock;
};

static const char *
pract (int action)
{
  switch (action)
    {
    case TA_UNDEF:
      return "TA_UNDEF";
    case TA_SOCKET_READ:
      return "TA_SOCKET_READ";
    case TA_SOCKET_READ_RESIDUAL:
      return "TA_SOCKET_READ_RESIDUAL";
    case TA_SOCKET_WRITE:
      return "TA_SOCKET_WRITE";
    case TA_SOCKET_WRITE_READY:
      return "TA_SOCKET_WRITE_READY";
    case TA_SOCKET_WRITE_DEFERRED:
      return "TA_SOCKET_WRITE_DEFERRED";
    case TA_TUN_READ:
      return "TA_TUN_READ";
    case TA_TUN_WRITE:
      return "TA_TUN_WRITE";
    case TA_INITIAL:
      return "TA_INITIAL";
    case TA_TIMEOUT:
      return "TA_TIMEOUT";
    default:
      return "?";
    }
}

static struct multi_instance *
multi_create_instance_tcp (struct multi_context *m)
{
  struct gc_arena gc = gc_new ();
  struct multi_instance *mi = NULL;
  struct hash *hash = m->hash;

  mi = multi_create_instance (m, NULL);
  if (mi)
    {
      struct hash_element *he;
      const uint32_t hv = hash_value (hash, &mi->real);
      struct hash_bucket *bucket = hash_bucket (hash, hv);
  
      hash_bucket_lock (bucket);
      he = hash_lookup_fast (hash, bucket, &mi->real, hv);

      if (he)
	{
	  struct multi_instance *oldmi = (struct multi_instance *) he->value;
	  msg (D_MULTI_LOW, "MULTI TCP: new incoming client address matches existing client address -- new client takes precedence");
	  oldmi->did_real_hash = false;
	  multi_close_instance (m, oldmi, false);
	  he->key = &mi->real;
	  he->value = mi;
	}
      else
	hash_add_fast (hash, bucket, &mi->real, hv, mi);

      mi->did_real_hash = true;

      hash_bucket_unlock (bucket);
    }

#ifdef MULTI_DEBUG
  if (mi)
    msg (D_MULTI_DEBUG, "MULTI TCP: instance added: %s", mroute_addr_print (&mi->real, &gc));
  else
    msg (D_MULTI_DEBUG, "MULTI TCP: new client instance failed");
#endif

  gc_free (&gc);
  ASSERT (!(mi && mi->halt));
  return mi;
}

bool
multi_tcp_instance_specific_init (struct multi_context *m, struct multi_instance *mi)
{
  /* buffer for queued TCP socket output packets */
  mi->tcp_link_out_deferred = mbuf_init (m->top.options.n_bcast_buf);

  ASSERT (mi->context.c2.link_socket);
  ASSERT (mi->context.c2.link_socket->info.lsa);
  ASSERT (mi->context.c2.link_socket->mode == LS_MODE_TCP_ACCEPT_FROM);
  if (!mroute_extract_sockaddr_in (&mi->real, &mi->context.c2.link_socket->info.lsa->actual, true))
    {
      msg (D_MULTI_ERRORS, "MULTI TCP: TCP client address is undefined");
      return false;
    }
  return true;
}

void
multi_tcp_instance_specific_free (struct multi_instance *mi)
{
  mbuf_free (mi->tcp_link_out_deferred);
}

struct multi_tcp *
multi_tcp_init (int maxevents, int *maxclients)
{
  struct multi_tcp *mtcp;
  const int extra_events = BASE_N_EVENTS;

  ASSERT (maxevents >= 1);
  ASSERT (maxclients);

  ALLOC_OBJ_CLEAR (mtcp, struct multi_tcp);
  mtcp->maxevents = maxevents + extra_events;
  mtcp->es = event_set_init (&mtcp->maxevents, EVENT_METHOD_SCALABLE);
  wait_signal (mtcp->es, MTCP_SIG);
  ALLOC_ARRAY (mtcp->esr, struct event_set_return, mtcp->maxevents);
  *maxclients = max_int (min_int (mtcp->maxevents - extra_events, *maxclients), 1);
  msg (D_MULTI_LOW, "MULTI: TCP INIT maxclients=%d maxevents=%d", *maxclients, mtcp->maxevents);
  return mtcp;
}

void
multi_tcp_free (struct multi_tcp *mtcp)
{
  if (mtcp)
    {
      event_free (mtcp->es);
      if (mtcp->esr)
	free (mtcp->esr);
      free (mtcp);
    }
}

void
multi_tcp_dereference_instance (struct multi_tcp *mtcp, struct multi_instance *mi)
{
  struct link_socket *ls = mi->context.c2.link_socket;
  if (ls && mi->socket_set_called)
    event_del (mtcp->es, socket_event_handle (ls));
}

static inline void
multi_tcp_set_global_rw_flags (struct multi_context *m, struct multi_instance *mi)
{
  if (mi)
    {
      mi->socket_set_called = true;
      socket_set (mi->context.c2.link_socket,
		  m->mtcp->es,
		  mbuf_defined (mi->tcp_link_out_deferred) ? EVENT_WRITE : EVENT_READ,
		  mi,
		  &mi->tcp_rwflags);
    }
}

static inline int
multi_tcp_wait (const struct context *c,
		struct multi_tcp *mtcp)
{
  int status;
  socket_set_listen_persistent (c->c2.link_socket, mtcp->es, MTCP_SOCKET);
  tun_set (c->c1.tuntap, mtcp->es, EVENT_READ, MTCP_TUN, &mtcp->tun_rwflags);
  status = event_wait (mtcp->es, &c->c2.timeval, mtcp->esr, mtcp->maxevents);
  update_time ();
  mtcp->n_esr = 0;
  if (status > 0)
    mtcp->n_esr = status;
  return status;
}

static inline struct context *
multi_tcp_context (struct multi_context *m, struct multi_instance *mi)
{
  if (mi)
    return &mi->context;
  else
    return &m->top;
}

static bool
multi_tcp_process_outgoing_link_ready (struct multi_context *m, struct multi_instance *mi, const unsigned int mpp_flags)
{
  struct mbuf_item item;
  bool ret = true;
  ASSERT (mi);

  /* extract from queue */
  if (mbuf_extract_item (mi->tcp_link_out_deferred, &item, true)) /* ciphertext IP packet */
    {
      msg (D_MULTI_TCP, "MULTI TCP: transmitting previously deferred packet");

      ASSERT (mi == item.instance);
      mi->context.c2.to_link = item.buffer->buf;
      ret = multi_process_outgoing_link_dowork (m, mi, mpp_flags);
      if (!ret)
	mi = NULL;
      mbuf_free_buf (item.buffer);
    }
  return ret;
}

static bool
multi_tcp_process_outgoing_link (struct multi_context *m, bool defer, const unsigned int mpp_flags)
{
  struct multi_instance *mi = multi_process_outgoing_link_pre (m);
  bool ret = true;

  if (mi)
    {
      if (defer || mbuf_defined (mi->tcp_link_out_deferred))
	{
	  /* save to queue */
	  struct buffer *buf = &mi->context.c2.to_link;
	  if (BLEN (buf) > 0)
	    {
	      struct mbuf_buffer *mb = mbuf_alloc_buf (buf);
	      struct mbuf_item item;

	      set_prefix (mi);
	      msg (D_MULTI_TCP, "MULTI TCP: queuing deferred packet");
	      item.buffer = mb;
	      item.instance = mi;
	      mbuf_add_item (mi->tcp_link_out_deferred, &item);
	      mbuf_free_buf (mb);
	      buf_reset (buf);
	      ret = multi_process_post (m, mi, mpp_flags);
	      if (!ret)
		mi = NULL;
	      clear_prefix ();
	    }
	}
      else
	{
	  ret = multi_process_outgoing_link_dowork (m, mi, mpp_flags);
	  if (!ret)
	    mi = NULL;
	}
    }
  return ret;
}

static int
multi_tcp_wait_lite (struct multi_context *m, struct multi_instance *mi, const int action, bool *tun_input_pending)
{
  struct context *c = multi_tcp_context (m, mi);
  unsigned int looking_for = 0;

  msg (D_MULTI_DEBUG, "MULTI TCP: multi_tcp_wait_lite a=%s mi=" ptr_format,
       pract(action),
       (ptr_type)mi);

  tv_clear (&c->c2.timeval); /* ZERO-TIMEOUT */

  switch (action)
    {
      case TA_TUN_READ:
	looking_for = TUN_READ;
	tun_input_pending = NULL;
	io_wait (c, IOW_READ_TUN);
	break;
      case TA_SOCKET_READ:
	looking_for = SOCKET_READ;
	tun_input_pending = NULL;
	io_wait (c, IOW_READ_LINK);
	break;
      case TA_TUN_WRITE:
	looking_for = TUN_WRITE;
	tun_input_pending = NULL;
	c->c2.timeval.tv_sec = MULTI_TCP_TUN_WRITE_TIMEOUT;
	io_wait (c, IOW_TO_TUN);
	break;
      case TA_SOCKET_WRITE:
	looking_for = SOCKET_WRITE;
	io_wait (c, IOW_TO_LINK|IOW_READ_TUN_FORCE);
	break;
      default:
	msg (M_FATAL, "MULTI TCP: multi_tcp_wait_lite, unhandled action=%d", action);
    }

  if (tun_input_pending && (c->c2.event_set_status & TUN_READ))
    *tun_input_pending = true;
  if (c->c2.event_set_status & looking_for)
    {
      return action;
    }
  else
    {
      if (action == TA_SOCKET_WRITE) /* TCP socket output buffer is full */
	return TA_SOCKET_WRITE_DEFERRED;
      else
	return TA_UNDEF;
    }
}

static struct multi_instance *
multi_tcp_dispatch (struct multi_context *m, struct multi_instance *mi, const int action)
{
  const unsigned int mpp_flags = MPP_PRE_SELECT|MPP_RECORD_TOUCH;
  struct multi_instance *touched = mi;
  m->mpp_touched = &touched;

  msg (D_MULTI_DEBUG, "MULTI TCP: multi_tcp_dispatch a=%s mi=" ptr_format,
       pract(action),
       (ptr_type)mi);

  switch (action)
    {
    case TA_TUN_READ:
      read_incoming_tun (&m->top);
      if (!IS_SIG (&m->top))
	multi_process_incoming_tun (m, mpp_flags);
      break;
    case TA_SOCKET_READ:
    case TA_SOCKET_READ_RESIDUAL:
      ASSERT (mi);
      ASSERT (mi->context.c2.link_socket);
      set_prefix (mi);
      read_incoming_link (&mi->context);
      clear_prefix ();
      if (!IS_SIG (&mi->context))
	{
	  multi_process_incoming_link (m, mi, mpp_flags);
	  if (!IS_SIG (&mi->context))
	    stream_buf_read_setup (mi->context.c2.link_socket);
	}
      break;
    case TA_TIMEOUT:
      multi_process_timeout (m, mpp_flags);
      break;
    case TA_TUN_WRITE:
      multi_process_outgoing_tun (m, mpp_flags);
      break;
    case TA_SOCKET_WRITE_READY:
      ASSERT (mi);
      multi_tcp_process_outgoing_link_ready (m, mi, mpp_flags);
      break;
    case TA_SOCKET_WRITE:
      multi_tcp_process_outgoing_link (m, false, mpp_flags);
      break;
    case TA_SOCKET_WRITE_DEFERRED:
      multi_tcp_process_outgoing_link (m, true, mpp_flags);
      break;
    case TA_INITIAL:
      ASSERT (mi);
      multi_tcp_set_global_rw_flags (m, mi);
      multi_process_post (m, mi, mpp_flags);
      break;
    default:
      msg (M_FATAL, "MULTI TCP: multi_tcp_dispatch, unhandled action=%d", action);
    }

  m->mpp_touched = NULL;
  return touched;
}

int
multi_tcp_post (struct multi_context *m, struct multi_instance *mi, const int action)
{
  struct context *c = multi_tcp_context (m, mi);
  int newaction = TA_UNDEF;

# define MTP_NONE         0
# define MTP_TUN_OUT      (1<<0)
# define MTP_LINK_OUT     (1<<1)
  unsigned int flags = MTP_NONE;

  if (TUN_OUT(c))
    flags |= MTP_TUN_OUT;
  if (LINK_OUT(c))
    flags |= MTP_LINK_OUT;

  switch (flags)
    {
    case MTP_TUN_OUT|MTP_LINK_OUT:
    case MTP_TUN_OUT:
      newaction = TA_TUN_WRITE;
      break;
    case MTP_LINK_OUT:
      newaction = TA_SOCKET_WRITE;
      break;
    case MTP_NONE:
      if (mi && socket_read_residual (c->c2.link_socket))
	newaction = TA_SOCKET_READ_RESIDUAL;
      else
	multi_tcp_set_global_rw_flags (m, mi);
      break;
    default:
      {
	struct gc_arena gc = gc_new ();
	msg (M_FATAL, "MULTI TCP: multi_tcp_post bad state, mi=%s flags=%d",
	     multi_instance_string (mi, false, &gc),
	     flags);
	gc_free (&gc);
	break;
      }
    }

  msg (D_MULTI_DEBUG, "MULTI TCP: multi_tcp_post %s -> %s",
       pract(action),
       pract(newaction));

  return newaction;
}

static void
multi_tcp_action (struct multi_context *m, struct multi_instance *mi, int action, bool poll)
{
  bool tun_input_pending = false;

  do {
    msg (D_MULTI_DEBUG, "MULTI TCP: multi_tcp_action a=%s p=%d",
	 pract(action),
	 poll);

    /*
     * If TA_SOCKET_READ_RESIDUAL, it means we still have pending
     * input packets which were read by a prior TCP recv.
     *
     * Otherwise do a "lite" wait, which means we wait with 0 timeout
     * on I/O events only related to the current instance, not
     * the big list of events.
     *
     * On our first pass, poll will be false because we already know
     * that input is available, and to call io_wait would be redundant.
     */
    if (poll && action != TA_SOCKET_READ_RESIDUAL)
      {
	const int orig_action = action;
	action = multi_tcp_wait_lite (m, mi, action, &tun_input_pending);
	if (action == TA_UNDEF)
	  msg (M_FATAL, "MULTI TCP: I/O wait required blocking in multi_tcp_action, action=%d", orig_action);
      }

    /*
     * Dispatch the action
     */
    {
      struct multi_instance *touched = multi_tcp_dispatch (m, mi, action);

      /*
       * Signal received or TCP connection
       * reset by peer?
       */
      if (touched && IS_SIG (&touched->context))
	{
	  if (mi == touched)
	    mi = NULL;
	  multi_close_instance_on_signal (m, touched);
	}
    }

    /*
     * If dispatch produced any pending output
     * for a particular instance, point to
     * that instance.
     */
    if (m->pending)
      mi = m->pending;

    /*
     * Based on the effects of the action,
     * such as generating pending output,
     * possibly transition to a new action state.
     */
    action = multi_tcp_post (m, mi, action);

    /*
     * If we are finished processing the original action,
     * check if we have any TUN input.  If so, transition
     * our action state to processing this input.
     */
    if (tun_input_pending && action == TA_UNDEF)
      {
	action = TA_TUN_READ;
	mi = NULL;
	tun_input_pending = false;
	poll = false;
      }
    else
      poll = true;

  } while (action != TA_UNDEF);
}

static void
multi_tcp_process_io (struct multi_context *m)
{
  struct multi_tcp *mtcp = m->mtcp;
  int i;

  for (i = 0; i < mtcp->n_esr; ++i)
    {
      struct event_set_return *e = &mtcp->esr[i];

      /* incoming data for instance? */
      if (e->arg >= MTCP_N)
	{
	  struct multi_instance *mi = (struct multi_instance *) e->arg;
	  if (mi)
	    {
	      if (e->rwflags & EVENT_WRITE)
		multi_tcp_action (m, mi, TA_SOCKET_WRITE_READY, false);
	      else if (e->rwflags & EVENT_READ)
		multi_tcp_action (m, mi, TA_SOCKET_READ, false);
	    }
	}
      else
	{
	  /* incoming data on TUN? */
	  if (e->arg == MTCP_TUN)
	    {
	      if (e->rwflags & EVENT_WRITE)
		multi_tcp_action (m, NULL, TA_TUN_WRITE, false);
	      else if (e->rwflags & EVENT_READ)
		multi_tcp_action (m, NULL, TA_TUN_READ, false);
	    }
	  /* new incoming TCP client attempting to connect? */
	  else if (e->arg == MTCP_SOCKET)
	    {
	      struct multi_instance *mi;
	      ASSERT (m->top.c2.link_socket);
	      socket_reset_listen_persistent (m->top.c2.link_socket);
	      mi = multi_create_instance_tcp (m);
	      if (mi)
		multi_tcp_action (m, mi, TA_INITIAL, false);
	    }
	  /* signal received? */
	  else if (e->arg == MTCP_SIG)
	    {
	      get_signal (&m->top.sig->signal_received);
	    }
	}
      if (IS_SIG (&m->top))
	break;
    }
  mtcp->n_esr = 0;

  /*
   * Process queued mbuf packets destined for TCP socket
   */
  {
    struct multi_instance *mi;
    while (!IS_SIG (&m->top) && (mi = mbuf_peek (m->mbuf)) != NULL)
      {
	multi_tcp_action (m, mi, TA_SOCKET_WRITE, true);
      }
  }
}

/*
 * Top level event loop for single-threaded operation.
 * TCP mode.
 */
void
tunnel_server_tcp (struct context *top)
{
  struct multi_context multi;
  int status;

  top->mode = CM_TOP;
  context_clear_2 (top);

  /* initialize top-tunnel instance */
  init_instance (top, top->es, CC_HARD_USR1_TO_HUP);
  if (IS_SIG (top))
    return;
  
  /* initialize global multi_context object */
  multi_init (&multi, top, true, MC_SINGLE_THREADED);

  /* initialize our cloned top object */
  multi_top_init (&multi, top, true);

  /* finished with initialization */
  initialization_sequence_completed (top, false);

  /* per-packet event loop */
  while (true)
    {
      perf_push (PERF_EVENT_LOOP);

      /* wait on tun/socket list */
      multi_get_timeout (&multi, &multi.top.c2.timeval);
      status = multi_tcp_wait (&multi.top, multi.mtcp);
      MULTI_CHECK_SIG (&multi);

      /* check on status of coarse timers */
      multi_process_per_second_timers (&multi);

      /* timeout? */
      if (status > 0)
	{
	  /* process the I/O which triggered select */
	  multi_tcp_process_io (&multi);
	  MULTI_CHECK_SIG (&multi);
	}
      else if (status == 0)
	{
	  multi_tcp_action (&multi, NULL, TA_TIMEOUT, false);
	}

      perf_pop ();
    }

  /* save ifconfig-pool */
  multi_ifconfig_pool_persist (&multi, true);

  /* tear down tunnel instance (unless --persist-tun) */
  multi_uninit (&multi);
  multi_top_free (&multi);
  close_instance (top);
}

#endif
