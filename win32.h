/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single UDP port, with support for SSL/TLS-based
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
#ifndef OPENVPN_WIN32_H
#define OPENVPN_WIN32_H

#include "mtu.h"

/*
 * Win32-specific OpenVPN code, targetted at the mingw
 * development environment.
 */

void init_win32 (void);
void uninit_win32 (void);

void set_pause_exit_win32 (void);

/*
 * Use keyboard input or events
 * to simulate incoming signals
 */

#define SIGUSR1   1
#define SIGUSR2   2
#define SIGHUP    3
#define SIGTERM   4
#define SIGINT    5

struct security_attributes
{
  SECURITY_ATTRIBUTES sa;
  SECURITY_DESCRIPTOR sd;
};

#define HANDLE_DEFINED(h) ((h) != NULL && (h) != INVALID_HANDLE_VALUE)

/*
 * Save old window title.
 */
struct window_title
{
  bool saved;
  char old_window_title [256];
};

struct rw_handle {
  HANDLE read;
  HANDLE write;
};

struct win32_signal {
# define WSO_MODE_UNDEF   0
# define WSO_MODE_SERVICE 1
# define WSO_MODE_CONSOLE 2
  int mode;
  struct rw_handle in;
  DWORD console_mode_save;
  bool console_mode_save_defined;
};

extern struct win32_signal win32_signal; /* static/global */
extern struct window_title window_title; /* static/global */

void win32_signal_clear (struct win32_signal *ws);

/* win32_signal_open startup type */
#define WSO_NOFORCE       0
#define WSO_FORCE_SERVICE 1
#define WSO_FORCE_CONSOLE 2

void win32_signal_open (struct win32_signal *ws,
			int force, /* set to WSO force parm */
			const char *exit_event_name,
			bool exit_event_initial_state);

void win32_signal_close (struct win32_signal *ws);

int win32_signal_get (struct win32_signal *ws);

void win32_pause (struct win32_signal *ws);

/*
 * Set the text on the window title bar
 */

void window_title_clear (struct window_title *wt);
void window_title_save (struct window_title *wt);
void window_title_restore (const struct window_title *wt);
void window_title_generate (const char *title);

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

char *overlapped_io_state_ascii (const struct overlapped_io *o);

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
 * It seems you can't run more than one instance
 * of netsh on the same machine at the same time.
 */

extern struct semaphore netcmd_semaphore;
void netcmd_semaphore_init (void);
void netcmd_semaphore_close (void);
void netcmd_semaphore_lock (void);
void netcmd_semaphore_release (void);

char *getpass (const char *prompt);

/* Set Win32 security attributes structure to allow all access */
bool init_security_attributes_allow_all (struct security_attributes *obj);

#endif
#endif
