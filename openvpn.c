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

#include "init.h"
#include "forward.h"
#include "multi.h"

#include "memdbg.h"

#include "forward-inline.h"

#define PROCESS_SIGNAL_P2P(c)                   \
      if (IS_SIG (c))                           \
	{                                       \
	  const int brk = process_signal (c);   \
	  perf_pop ();                          \
	  if (brk)                              \
	    break;                              \
	  else                                  \
	    continue;                           \
	}

static void
tunnel_point_to_point (struct context *c)
{
  context_clear_2 (c);

  /* set point-to-point mode */
  c->mode = CM_P2P;

  /* initialize tunnel instance */
  init_instance (c, CC_HARD_USR1_TO_HUP);
  if (IS_SIG (c))
    return;

  /* main event loop */
  while (true)
    {
      perf_push (PERF_EVENT_LOOP);

      /* process timers, TLS, etc. */
      pre_select (c);
      PROCESS_SIGNAL_P2P (c);

      /* set up and do the I/O wait */
      io_wait (c, p2p_iow_flags (c));
      PROCESS_SIGNAL_P2P (c);

      /* timeout? */
      if (c->c2.event_set_status == ES_TIMEOUT)
	{
	  perf_pop ();
	  continue;
	}

      /* process the I/O which triggered select */
      process_io (c);
      PROCESS_SIGNAL_P2P (c);

      perf_pop ();
    }

  /* tear down tunnel instance (unless --persist-tun) */
  close_instance (c);
  c->first_time = false;
}

#undef PROCESS_SIGNAL_P2P

int
main (int argc, char *argv[])
{
  struct context c;

  CLEAR (c);

  /* signify first time for components which can
     only be initialized once per program instantiation. */
  c.first_time = true;

  /* initialize program-wide statics */
  if (init_static ())
    {
      /*
       * This loop is initially executed on startup and then
       * once per SIGHUP.
       */
      do
	{
	  /* zero context struct but leave first_time member alone */
	  context_clear_all_except_first_time (&c);

	  /* initialize garbage collector scoped to context object */
	  gc_init (&c.gc);

	  /* static signal info object */
	  c.sig = &siginfo_static;

	  /* initialize options to default state */
	  init_options (&c.options);

	  /* parse command line options, and read configuration file */
	  parse_argv (&c.options, argc, argv, M_USAGE, OPT_P_DEFAULT, NULL);

	  /* init verbosity and mute levels */
	  init_verb_mute (&c, IVM_LEVEL_1);

	  /* set dev options */
	  init_options_dev (&c.options);

	  /* openssl print info? */
	  if (print_openssl_info (&c.options))
	    break;

	  /* --genkey mode? */
	  if (do_genkey (&c.options))
	    break;

	  /* tun/tap persist command? */
	  if (do_persist_tuntap (&c.options))
	    break;

	  /* sanity check on options */
	  options_postprocess (&c.options, c.first_time);

	  /* misc stuff */
	  pre_setup (&c.options);

	  /* test crypto? */
	  if (do_test_crypto (&c.options))
	    break;

	  /* finish context init */
	  context_init_1 (&c);

	  do
	    {
	      /* run tunnel depending on mode */
	      switch (c.options.mode)
		{
		case MODE_POINT_TO_POINT:
		  tunnel_point_to_point (&c);
		  break;
#if P2MP
		case MODE_SERVER:
		  tunnel_server (&c);
		  break;
#endif
		default:
		  ASSERT (0);
		}

	      /* any signals received? */
	      if (IS_SIG (&c))
		print_signal (c.sig, NULL, M_INFO);

	      /* Convert SIGUSR1 -> SIGHUP if no --persist options (or other options
		 which hold state across restarts) specified */
	      if (!is_stateful_restart (&c.options) && c.sig->signal_received == SIGUSR1)
		c.sig->signal_received = SIGHUP;
	    }
	  while (c.sig->signal_received == SIGUSR1);

	  uninit_options (&c.options);
	  gc_reset (&c.gc);
	}
      while (c.sig->signal_received == SIGHUP);
    }

  /* uninitialize program-wide statics */
  uninit_static ();

  context_gc_free (&c);

  openvpn_exit (OPENVPN_EXIT_STATUS_GOOD);  /* exit point */
  return 0;			            /* NOTREACHED */
}
