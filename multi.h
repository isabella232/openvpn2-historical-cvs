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

#ifndef MULTI_H
#define MULTI_H

#if P2MP

#include "init.h"
#include "forward.h"
#include "mroute.h"
#include "mbuf.h"
#include "list.h"
#include "schedule.h"
#include "pool.h"
#include "mudp.h"
#include "mtcp.h"
#include "perf.h"

/*
 * Walk (don't run) through the routing table,
 * deleting old entries, and possibly multi_instance
 * structs as well which have been marked for deletion.
 */
struct multi_reap
{
  int bucket_base;
  int buckets_per_pass;
  time_t last_call;
};

/*
 * One multi_instance object per client instance.
 */
struct multi_instance {
  struct schedule_entry se;    /* this must be the first element of the structure */
  struct gc_arena gc;
  //MUTEX_DEFINE (mutex);
  bool defined;
  bool halt;
  int refcount;
  time_t created;
  struct timeval wakeup;       /* absolute time */
  struct mroute_addr real;
  ifconfig_pool_handle vaddr_handle;
  const char *msg_prefix;

  /* queued outgoing data in Server/TCP mode */
  unsigned int tcp_rwflags;
  struct mbuf_set *tcp_link_out_deferred;

  bool did_open_context;
  bool did_real_hash;
  bool did_iter;
  bool connection_established_flag;

  struct context context;
};

/*
 * One multi_context object per server daemon.
 */
struct multi_context {
  struct hash *hash;   /* client instances indexed by real address */
  struct hash *vhash;  /* client instances indexed by virtual address */
  struct hash *iter;   /* like real address hash but optimized for iteration */
  struct schedule *schedule;
  struct mbuf_set *mbuf;
  struct multi_tcp *mtcp;
  struct ifconfig_pool *ifconfig_pool;
  struct frequency_limit *new_connection_limiter;
  struct mroute_helper *route_helper;
  struct multi_reap *reaper;
  struct mroute_addr local;
  const char *learn_address_script;
  bool enable_c2c;
  int max_clients;
  int tcp_queue_limit;

  struct multi_instance *pending;
  struct multi_instance *earliest_wakeup;
  struct multi_instance **mpp_touched;
  struct context_buffers *context_buffers;
  time_t per_second_trigger;

  struct context top;
};

/*
 * Host route
 */
struct multi_route
{
  struct mroute_addr addr;
  struct multi_instance *instance;

# define MULTI_ROUTE_CACHE   (1<<0)
# define MULTI_ROUTE_AGEABLE (1<<1)
  unsigned int flags;

  unsigned int cache_generation;
  time_t last_reference;
};

/*
 * top level function, called by openvpn.c
 */
void tunnel_server (struct context *top);

const char *multi_instance_string (struct multi_instance *mi, bool null, struct gc_arena *gc);

void multi_bcast (struct multi_context *m,
		  const struct buffer *buf,
		  struct multi_instance *omit);

/*
 * Called by mtcp.c, mudp.c, or other (to be written) protocol drivers
 */

void multi_init (struct multi_context *m, struct context *t, bool tcp_mode);
void multi_uninit (struct multi_context *m);

void multi_top_init (struct multi_context *m, const struct context *top);
void multi_top_free (struct multi_context *m);

struct multi_instance *multi_create_instance (struct multi_context *m, const struct mroute_addr *real);
void multi_close_instance (struct multi_context *m, struct multi_instance *mi, bool shutdown);

bool multi_process_timeout (struct multi_context *m, const unsigned int mpp_flags);

#define MPP_PRE_SELECT             (1<<0)
#define MPP_CONDITIONAL_PRE_SELECT (1<<1)
#define MPP_CLOSE_ON_SIGNAL        (1<<2)
#define MPP_RECORD_TOUCH           (1<<3)
bool multi_process_post (struct multi_context *m, struct multi_instance *mi, const unsigned int flags);

bool multi_process_incoming_link (struct multi_context *m, struct multi_instance *instance, const unsigned int mpp_flags);
bool multi_process_incoming_tun (struct multi_context *m, const unsigned int mpp_flags);

void multi_print_status (struct multi_context *m, struct status_output *so);

struct multi_instance *multi_get_queue (struct mbuf_set *ms);

void multi_add_mbuf (struct multi_context *m,
		     struct multi_instance *mi,
		     struct mbuf_buffer *mb);

void multi_ifconfig_pool_persist (struct multi_context *m, bool force);

/*
 * Return true if our output queue is not full
 */
static inline bool
multi_output_queue_ready (const struct multi_context *m,
			  const struct multi_instance *mi)
{
  if (mi->tcp_link_out_deferred)
    return mbuf_len (mi->tcp_link_out_deferred) <= m->tcp_queue_limit;
  else
    return true;
}

/*
 * Determine which instance has pending output
 * and prepare the output for sending in
 * the to_link buffer.
 */
static inline struct multi_instance *
multi_process_outgoing_link_pre (struct multi_context *m)
{
  struct multi_instance *mi = NULL;

  if (m->pending)
    mi = m->pending;
  else if (mbuf_defined (m->mbuf))
    mi = multi_get_queue (m->mbuf);
  return mi;
}

/*
 * Instance reference counting
 */

static inline void
multi_instance_inc_refcount (struct multi_instance *mi)
{
  ++mi->refcount;
}

static inline void
multi_instance_dec_refcount (struct multi_instance *mi)
{
  if (--mi->refcount <= 0)
    {
      gc_free (&mi->gc);
      free (mi);
    }
}

static inline void
multi_route_del (struct multi_route *route)
{
  multi_instance_dec_refcount (route->instance);
  free (route);
}

static inline bool
multi_route_defined (const struct multi_context *m,
		     const struct multi_route *r)
{
  if (r->instance->halt)
    return false;
  else if ((r->flags & MULTI_ROUTE_CACHE)
	   && r->cache_generation != m->route_helper->cache_generation)
    return false;
  else if ((r->flags & MULTI_ROUTE_AGEABLE)
	   && r->last_reference + m->route_helper->ageable_ttl_secs < now)
    return false;
  else
    return true;
}

/*
 * Set a msg() function prefix with our current client instance ID.
 */

static inline void
set_prefix (struct multi_instance *mi)
{
#ifdef MULTI_DEBUG_EVENT_LOOP
  if (mi->msg_prefix)
    printf ("[%s]\n", mi->msg_prefix);
#endif
  msg_set_prefix (mi->msg_prefix);
}

static inline void
clear_prefix (void)
{
#ifdef MULTI_DEBUG_EVENT_LOOP
  printf ("[NULL]\n");
#endif
  msg_set_prefix (NULL);
}

/*
 * Instance Reaper
 *
 * Reaper constants.  The reaper is the process where the virtual address
 * and virtual route hash table is scanned for dead entries which are
 * then removed.  The hash table could potentially be quite large, so we
 * don't want to reap in a single pass.
 */

#define REAP_MAX_WAKEUP   10  /* Do reap pass at least once per n seconds */
#define REAP_DIVISOR     256  /* How many passes to cover whole hash table */
#define REAP_MIN          16  /* Minimum number of buckets per pass */
#define REAP_MAX        1024  /* Maximum number of buckets per pass */

/*
 * Mark a cached host route for deletion after this
 * many seconds without any references.
 */
#define MULTI_CACHE_ROUTE_TTL 60

static inline void
multi_reap_process (const struct multi_context *m)
{
  void multi_reap_process_dowork (const struct multi_context *m);
  if (m->reaper->last_call != now)
    multi_reap_process_dowork (m);
}

static inline void
multi_process_per_second_timers (struct multi_context *m)
{
  if (m->per_second_trigger != now)
    {
      void multi_process_per_second_timers_dowork (struct multi_context *m);
      multi_process_per_second_timers_dowork (m);
      m->per_second_trigger = now;
    }
}

/*
 * Compute earliest timeout expiry from the set of
 * all instances.  Output:
 *
 * m->earliest_wakeup : instance needing the earliest service.
 * dest               : earliest timeout as a delta in relation
 *                      to current time.
 */
static inline void
multi_get_timeout (struct multi_context *m, struct timeval *dest)
{
  struct timeval tv, current;

  m->earliest_wakeup = (struct multi_instance *) schedule_get_earliest_wakeup (m->schedule, &tv);
  if (m->earliest_wakeup)
    {
      ASSERT (!gettimeofday (&current, NULL));
      tv_delta (dest, &current, &tv);
      if (dest->tv_sec >= REAP_MAX_WAKEUP)
	{
	  m->earliest_wakeup = NULL;
	  dest->tv_sec = REAP_MAX_WAKEUP;
	  dest->tv_usec = 0;
	}
    }
  else
    {
      dest->tv_sec = REAP_MAX_WAKEUP;
      dest->tv_usec = 0;
    }
}

/*
 * Send a packet to TUN/TAP interface.
 */
static inline bool
multi_process_outgoing_tun (struct multi_context *m, const unsigned int mpp_flags)
{
  struct multi_instance *mi = m->pending;
  bool ret = true;

  ASSERT (mi);
#ifdef MULTI_DEBUG_EVENT_LOOP
  printf ("%s -> TUN len=%d\n",
	  id(mi),
	  mi->context.c2.to_tun.len);
#endif
  set_prefix (mi);
  process_outgoing_tun (&mi->context);
  ret = multi_process_post (m, mi, mpp_flags);
  clear_prefix ();
  return ret;
}

static inline bool
multi_process_outgoing_link_dowork (struct multi_context *m, struct multi_instance *mi, const unsigned int mpp_flags)
{
  bool ret = true;
  set_prefix (mi);
  process_outgoing_link (&mi->context);
  ret = multi_process_post (m, mi, mpp_flags);
  clear_prefix ();
  return ret;
}

/*
 * Check for signals.
 */
#define MULTI_CHECK_SIG() \
  if (IS_SIG (&multi.top)) \
  { \
    if (multi.top.sig->signal_received == SIGUSR2) \
      { \
        struct status_output *so = status_open (NULL, 0, M_INFO, 0); \
        multi_print_status (&multi, so); \
        status_close (so); \
        multi.top.sig->signal_received = 0; \
        perf_pop (); \
        continue; \
      } \
    perf_pop (); \
    break; \
  }

#endif /* P2MP */
#endif /* MULTI_H */
