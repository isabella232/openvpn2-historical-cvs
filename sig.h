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

#ifndef SIG_H
#define SIG_H

#include "status.h"
#include "win32.h"

/*
 * Signal information, including signal code
 * and descriptive text.
 */
struct signal_info
{
  volatile int signal_received;
  volatile bool hard;
  const char *signal_text;
};

#define IS_SIG(c) ((c)->sig->signal_received)

struct context;

extern struct signal_info siginfo_static;

void pre_init_signal_catch (void);
void post_init_signal_catch (void);

const char *signal_description (int signum, const char *sigtext);
void print_signal (const struct signal_info *si, const char *title, int msglevel);
void print_status (const struct context *c, struct status_output *so);

bool process_signal (struct context *c);

void process_explicit_exit_notification_timer_wakeup (struct context *c);

#ifdef WIN32

static inline void
get_signal (volatile int *sig)
{
  *sig = win32_signal_get (&win32_signal);
}

static inline void
halt_non_edge_triggered_signals (void)
{
  win32_signal_close (&win32_signal);
}

#else

static inline void
get_signal (volatile int *sig)
{
  const int i = siginfo_static.signal_received;
  if (i)
    *sig = i;
}

static inline void
halt_non_edge_triggered_signals (void)
{
}

#endif

#endif
