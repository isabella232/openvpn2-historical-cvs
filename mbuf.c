/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2005 OpenVPN Solutions LLC <info@openvpn.net>
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

#include "buffer.h"
#include "error.h"
#include "misc.h"
#include "mbuf.h"

#include "memdbg.h"

struct mbuf_set *
mbuf_init (unsigned int size)
{
  struct mbuf_set *ret;
  ALLOC_OBJ_CLEAR (ret, struct mbuf_set);
  ret->capacity = adjust_power_of_2 (size);
  ALLOC_ARRAY (ret->array, struct mbuf_item, ret->capacity);
  return ret;
}

void
mbuf_free (struct mbuf_set *ms)
{
  if (ms)
    {
      int i;
      for (i = 0; i < (int) ms->len; ++i)
	{
	  struct mbuf_item *item = &ms->array[MBUF_INDEX(ms->head, i, ms->capacity)];
	  mbuf_free_buf (item->buffer);
	}
      free (ms->array);
      free (ms);
    }
}

struct mbuf_buffer *
mbuf_alloc_buf (const struct buffer *buf)
{
  struct mbuf_buffer *ret;
  ALLOC_OBJ (ret, struct mbuf_buffer);
  ret->buf = clone_buf (buf);
  ret->refcount = 1;
  ret->flags = 0;
  return ret;
}

struct mbuf_buffer *
mbuf_alloc_string (const char *str, const unsigned int flags)
{
  struct mbuf_buffer *ret;
  ALLOC_OBJ (ret, struct mbuf_buffer);
  ret->buf = string_alloc_buf (str, flags, NULL);
  ret->refcount = 1;
  ret->flags = 0;
  return ret;
}

void
mbuf_free_buf (struct mbuf_buffer *mb)
{
  if (mb)
    {
      if (--mb->refcount <= 0)
	{
	  free_buf (&mb->buf);
	  free (mb);
	}
    }
}

bool
mbuf_add_item (struct mbuf_set *ms, const struct mbuf_item *item)
{
  bool ret = true;
  ASSERT (ms);
  if (ms->len == ms->capacity)
    {
      struct mbuf_item rm;
      ASSERT (mbuf_extract_item (ms, &rm));
      mbuf_free_buf (rm.buffer);
      msg (D_MULTI_DROPPED, "MBUF: mbuf packet dropped");

      ret = false;
    }

  ASSERT (ms->len < ms->capacity);

  ms->array[MBUF_INDEX(ms->head, ms->len, ms->capacity)] = *item;
  if (++ms->len > ms->max_queued)
    ms->max_queued = ms->len;
  ++item->buffer->refcount;
  return ret;
}

bool
mbuf_extract_item (struct mbuf_set *ms, struct mbuf_item *item)
{
  bool ret = false;
  if (ms)
    {
      while (ms->len)
	{
	  *item = ms->array[ms->head];
	  ms->head = MBUF_INDEX(ms->head, 1, ms->capacity);
	  --ms->len;
	  if (item->arg) /* ignore dereferenced entries */
	    {
	      ret = true;
	      break;
	    }
	}
    }
  return ret;
}

void *
mbuf_peek_dowork (struct mbuf_set *ms)
{
  void *ret = NULL;
  if (ms)
    {
      int i;
      for (i = 0; i < (int) ms->len; ++i)
	{
	  struct mbuf_item *item = &ms->array[MBUF_INDEX(ms->head, i, ms->capacity)];
	  if (item->arg)
	    {
	      ret = item->arg;
	      break;
	    }
	}
    }
  return ret;
}

void
mbuf_dereference (struct mbuf_set *ms, void *arg)
{
  if (ms)
    {
      int i;
      for (i = 0; i < (int) ms->len; ++i)
	{
	  struct mbuf_item *item = &ms->array[MBUF_INDEX(ms->head, i, ms->capacity)];
	  if (item->arg == arg)
	    {
	      mbuf_free_buf (item->buffer);
	      item->buffer = NULL;
	      item->arg = NULL;
	      msg (D_MBUF, "MBUF: dereferenced queued packet");
	    }
	}
    }
}

#else
static void dummy(void) {}
#endif /* P2MP */
