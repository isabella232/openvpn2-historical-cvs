/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2003 James Yonan <jim@yonan.net>
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

#ifndef SHAPER_H
#define SHAPER_H

#include "basic.h"
#include "common.h"

/*
 * A simple traffic shaper for
 * the output direction.
 */

#define SHAPER_MIN 100          /* bytes per second */
#define SHAPER_MAX 100000000

#define MAX_TIMEOUT 10          /* seconds */

struct shaper 
{
  int bytes_per_second;
  struct timeval wakeup;
};

void shaper_init (struct shaper *s, int bytes_per_second);
void shaper_msg (struct shaper *s);
int shaper_delay (struct shaper* s);
void shaper_soonest_event (struct timeval *tv, int delay);
void shaper_wrote_bytes (struct shaper* s, int nbytes);
bool shaper_change_pct (struct shaper *s, int pct);

/*
 * inline functions
 */

static inline void
shaper_reset (struct shaper *s, int bytes_per_second)
{
  s->bytes_per_second = bytes_per_second ? constrain_int (bytes_per_second, SHAPER_MIN, SHAPER_MAX) : 0;
}

static inline int
shaper_current_bandwidth (struct shaper *s)
{
  return s->bytes_per_second;
}

#endif
