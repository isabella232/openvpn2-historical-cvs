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

/*
 * I/O functionality used by both the sockets and TUN/TAP I/O layers.
 *
 * We also try to abstract away the differences between Posix and Win32.
 */

#ifndef OPENVPN_IO_H
#define OPENVPN_IO_H

#include "common.h"
#include "integer.h"
#include "error.h"
#include "basic.h"
#include "mtu.h"
#include "buffer.h"

/* Maximum number of events we will wait for */
#define MAX_EVENTS 5

/* flags for wait_init() */

#define WAIT_READ  0x01
#define WAIT_WRITE 0x02

/* allocate a buffer for socket or tun layer */
void alloc_buf_sock_tun (struct buffer *buf, const struct frame *frame, bool tuntap_buffer);

#ifdef WIN32

/*
 * Use keyboard input or events
 * to simulate incoming signals
 */

#define SIGUSR1   1
#define SIGUSR2   2
#define SIGHUP    3
#define SIGTERM   4
#define SIGINT    5

/*
 * If we are being run as a win32 service,
 * use this event as our exit trigger.
 */
#define EXIT_EVENT_NAME PACKAGE "_exit"

struct win32_signal {
  /*
   * service is true if we are being run as a win32 service.
   * if service == true, in is an event handle which will be
   *   signaled when we should exit.
   * if service == false, in is a keyboard handle which we will
   *   use as a source of asynchronous signals.
   */
  bool service;
  HANDLE in;
};

extern struct win32_signal win32_signal; /* static/global */

void win32_signal_init (void); 
void win32_signal_close (void);
int win32_signal_get (struct win32_signal *ws);
void win32_pause (void);

/*
 * Set the text on the window title bar
 */
void generate_window_title (const char *title);
void save_window_title ();
void restore_window_title ();

/* 
 * We try to do all Win32 I/O using overlapped
 * (i.e. asynchronous) I/O for a performance win.
 */
struct overlapped_io {
# define IOSTATE_INITIAL          0
# define IOSTATE_QUEUED           1 /* overlapped I/O has been queued */
# define IOSTATE_IMMEDIATE_RETURN 2 /* I/O function returned immediately without queueing */
  int iostate;
  OVERLAPPED overlapped;
  DWORD size;
  DWORD flags;
  int status;
  bool addr_defined;
  struct sockaddr_in addr;
  int addrlen;
  struct buffer buf_init;
  struct buffer buf;
};

void overlapped_io_init (struct overlapped_io *o,
			 const struct frame *frame,
			 BOOL event_state,
			 bool tuntap_buffer);

void overlapped_io_close (struct overlapped_io *o);

static inline bool
overlapped_io_active (struct overlapped_io *o)
{
  return o->iostate == IOSTATE_QUEUED || o->iostate == IOSTATE_IMMEDIATE_RETURN;
}

const char *
overlapped_io_state_ascii (const struct overlapped_io *o,
			   const char* prefix,
			   struct gc_arena *gc);

/*
 * On Win32, use WSAWaitForMultipleEvents instead of select as our main event
 * loop I/O wait mechanism.  This is done for efficiency and also for the simple
 * reason that Win32 select() can only wait on sockets, not on the TAP-Win32 file
 * handle which we must also wait on.
 */

struct event_wait {
  HANDLE events[MAX_EVENTS];
  DWORD n_events;
  HANDLE trigger;  /* handle that satisfied the most recent wait */
};

#define SELECT(w, t)       my_select ((w), (t))
#define SELECT_READ(w, t)  my_select ((w), (t))
#define SELECT_WRITE(w, t) my_select ((w), (t))

#define WAIT_SIGNAL(w) \
  { if (win32_signal.in != INVALID_HANDLE_VALUE) \
    wait_add ((w), win32_signal.in); }

#define GET_SIGNAL(sig) \
  { (sig) = win32_signal_get (&win32_signal); }

#define SELECT_SIGNAL_RECEIVED(w, sig) \
  { if (win32_signal.in != INVALID_HANDLE_VALUE \
       && wait_trigger ((w), win32_signal.in)) \
         GET_SIGNAL (sig); }

static inline int
my_select (struct event_wait *ew, const struct timeval *tv)
{
#if 0
  {
    int i;
    msg (D_SELECT, "WSAWaitForMultipleEvents n=%d", (int) ew->n_events);
    for (i = 0; i < ew->n_events; ++i)
      msg (D_SELECT, "WSAWaitForMultipleEvents [%d] 0x%08x state=%d",
	   i, (unsigned int) ew->events[i], (int) WaitForSingleObject (ew->events[i], 0));
  }
#endif

  const DWORD status = WSAWaitForMultipleEvents(
    ew->n_events,
    ew->events,
    FALSE,
    tv ? (DWORD) (tv->tv_sec * 1000 + tv->tv_usec / 1000) : WSA_INFINITE,
    FALSE);

  if (status >= WSA_WAIT_EVENT_0 && status < WSA_WAIT_EVENT_0 + ew->n_events)
    {
      const int n = status - WSA_WAIT_EVENT_0;
      ew->trigger = ew->events [n];
      return n + 1;
    }
  else if (status == WSA_WAIT_TIMEOUT)
    return 0;
  else
    return -1;
}

static inline void
wait_reset (struct event_wait *ew)
{
  ew->n_events = 0;
  ew->trigger = NULL;
}

static inline void
wait_add (struct event_wait *ew, HANDLE h)
{
  ASSERT (ew->n_events < MAX_EVENTS);
  ew->events[ew->n_events++] = h;
}

static inline bool
wait_trigger (const struct event_wait *ew, HANDLE h)
{
  return ew->trigger == h;
}

static inline void
wait_free (struct event_wait *ew)
{
}

/*
 * Use to control access to resources that only one
 * OpenVPN process on a given machine can access at
 * a given time.
 */

struct semaphore
{
  const char *name;
  bool locked;
  HANDLE hand;
};

void semaphore_clear (struct semaphore *s);
void semaphore_open (struct semaphore *s, const char *name);
bool semaphore_lock (struct semaphore *s, int timeout_milliseconds);
void semaphore_release (struct semaphore *s);
void semaphore_close (struct semaphore *s);

/*
 * Special global semaphore used to protect network
 * shell commands from simultaneous instantiation.
 *
 * Not kidding -- you can't run more than one instance
 * of netsh on the same machine at the same time.
 */

extern struct semaphore netcmd_semaphore;
void netcmd_semaphore_init (void);
void netcmd_semaphore_close (void);
void netcmd_semaphore_lock (void);
void netcmd_semaphore_release (void);

char *getpass (const char *prompt);

#else /* Posix stuff here */

struct event_wait {
  int max_fd_plus_one;
  fd_set *reads;
  fd_set *writes;
};

#ifdef ENABLE_PROFILING
int profile_select(int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
#define SELECT(w, t)       profile_select ((w)->max_fd_plus_one, (w)->reads, (w)->writes, NULL, (t))
#else
#define SELECT(w, t)       select ((w)->max_fd_plus_one, (w)->reads, (w)->writes, NULL, (t))
#endif

#define SELECT_SIGNAL_RECEIVED(w, sig)
#define GET_SIGNAL(sig)
#define WAIT_SIGNAL(w)

void wait_free (struct event_wait *ew);

#define ASSUME_FD_SET_IS_BITFIELD // JYFIXME

static inline void
wait_reset (struct event_wait *ew)
{
#ifdef ASSUME_FD_SET_IS_BITFIELD
  if (ew->max_fd_plus_one <= (int) sizeof (unsigned long))
    {
      if (ew->reads)
	*((unsigned long *) ew->reads) = 0;
      if (ew->writes)
	*((unsigned long *) ew->writes) = 0;
    }
  else
#endif
    {
      if (ew->reads)
	FD_ZERO (ew->reads);
      if (ew->writes)
	FD_ZERO (ew->writes);
    }
  ew->max_fd_plus_one = -1;
}

static inline void
wait_add (struct event_wait *ew, int fd, const unsigned int rwflag)
{
  if (fd >= 0)
    {
      ew->max_fd_plus_one = max_int (ew->max_fd_plus_one, fd + 1);
      if ((rwflag & WAIT_READ) && ew->reads)
	FD_SET (fd, ew->reads);
      if ((rwflag & WAIT_WRITE) && ew->writes)
	FD_SET (fd, ew->writes);
    }
}

static inline void
wait_add_reads (struct event_wait *ew, int fd)
{
  if (fd >= 0)
    {
      ew->max_fd_plus_one = max_int (ew->max_fd_plus_one, fd + 1);
      if (ew->reads)
	FD_SET (fd, ew->reads);
    }
}

static inline void
wait_add_writes (struct event_wait *ew, int fd)
{
  if (fd >= 0)
    {
      ew->max_fd_plus_one = max_int (ew->max_fd_plus_one, fd + 1);
      if (ew->writes)
	FD_SET (fd, ew->writes);
    }
}

static inline bool
wait_test (struct event_wait *ew, int fd, const unsigned int rwflag)
{
  if (fd >= 0)
    {
      if (rwflag == WAIT_READ && ew->reads)
	return FD_ISSET (fd, ew->reads);
      if (rwflag == WAIT_WRITE && ew->writes)
	return FD_ISSET (fd, ew->writes);
    }
  else
    return false;
}

static inline bool
wait_test_reads (struct event_wait *ew, int fd)
{
  if (fd >= 0 && ew->reads)
    return FD_ISSET (fd, ew->reads);
  else
    return false;
}

static inline bool
wait_test_writes (struct event_wait *ew, int fd)
{
  if (fd >= 0 && ew->writes)
    return FD_ISSET (fd, ew->writes);
  else
    return false;
}

static inline bool
wait_read_enabled (const struct event_wait *ew)
{
  return ew->reads != NULL;
}

static inline bool
wait_write_enabled (const struct event_wait *ew)
{
  return ew->writes != NULL;
}

#endif /* WIN32 */

void wait_init (struct event_wait *ew, unsigned int rwflags);

#endif
