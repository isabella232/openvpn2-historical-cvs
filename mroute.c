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

#include "mroute.h"
#include "proto.h"
#include "error.h"

#include "memdbg.h"

bool
mroute_extract_addr_from_packet (struct mroute_addr *addr, const struct buffer *buf, int tunnel_type, bool dest)
{
  if (tunnel_type == DEV_TYPE_TUN)
    {
      if (BLEN (buf) >= (int) (sizeof (struct openvpn_ethhdr) + 1))
	{
	  switch (OPENVPN_IPH_GET_VER (*BPTR(buf)))
	    {
	    case 4:
	      if (BLEN (buf) >= (int) (sizeof (struct openvpn_ethhdr) + sizeof (struct openvpn_iphdr)))
		{
		  const struct openvpn_iphdr *ip = (const struct openvpn_iphdr *) (BPTR (buf) + sizeof (struct openvpn_ethhdr));
		  addr->type = MR_ADDR_IPV4;
		  addr->len = 4;
		  memcpy (addr->addr, dest ? &ip->daddr : &ip->saddr, 4);
		  return true;
		}
	      break;
	    case 6:
	      {
		msg (M_FATAL, "Need IPv6 code in mroute_extract_dest_addr_from_packet"); 
		break;
	      }
	    }
	}
    }
  else if (tunnel_type == DEV_TYPE_TAP)
    {
      if (BLEN (buf) >= (int) sizeof (struct openvpn_ethhdr))
	{
	  const struct openvpn_ethhdr *eth = (const struct openvpn_ethhdr *) BPTR (buf);
	  addr->type = MR_ADDR_ETHER;
	  addr->len = 6;
	  memcpy (addr->addr, dest ? eth->dest : eth->source, 6);
	  return true;
	}
    }
  return false;
}

bool
mroute_extract_sockaddr_in (struct mroute_addr *addr, const struct sockaddr_in *saddr, bool use_port)
{
  if (saddr->sin_family == AF_INET)
    {
      if (use_port)
	{
	  addr->type = MR_ADDR_IPV4 | MR_WITH_PORT;
	  addr->len = 6;
	  memcpy (addr->addr, &saddr->sin_addr.s_addr, 4);
	  memcpy (addr->addr + 4, &saddr->sin_port, 2);
	}
      else
	{
	  addr->type = MR_ADDR_IPV4;
	  addr->len = 4;
	  memcpy (addr->addr, &saddr->sin_addr.s_addr, 4);
	}
      return true;
    }
  return false;
}

static inline void
mroute_addr_init (struct mroute_addr *addr)
{
  CLEAR (*addr);
}

static inline void
mroute_addr_free (struct mroute_addr *addr)
{
  CLEAR (*addr);
}

void
mroute_list_init (struct mroute_list *list)
{
  mroute_addr_init (&list->addr);
}

void
mroute_list_free (struct mroute_list *list)
{
  mroute_addr_free (&list->addr);
}

#else
static void dummy(void) {}
#endif /* P2MP */