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

/*
 * 2004-01-28: Added Socks5 proxy support
 *   (Christof Meerwald, http://cmeerw.org)
 */

#ifdef WIN32
#include "config-win32.h"
#else
#include "config.h"
#endif

#include "syshead.h"

#include "buffer.h"
#include "error.h"
#include "common.h"
#include "shaper.h"
#include "crypto.h"
#include "ssl.h"
#include "options.h"
#include "misc.h"
#include "socket.h"
#include "packet_id.h"
#include "win32.h"
#include "push.h"
#include "pool.h"
#include "helper.h"

#include "memdbg.h"

const char title_string[] =
  PACKAGE_STRING
  " " TARGET_ALIAS
#ifdef USE_CRYPTO
#ifdef USE_SSL
  " [SSL]"
#else
  " [CRYPTO]"
#endif
#endif
#ifdef USE_LZO
  " [LZO]"
#endif
#if EPOLL
  " [EPOLL]"
#endif
#ifdef USE_PTHREAD
  " [PTHREAD]"
#endif
  " built on " __DATE__
;

static const char usage_message[] =
  "%s\n"
  "\n"
  "General Options:\n"
  "--config file   : Read configuration options from file.\n"
  "--help          : Show options.\n"
  "--version       : Show copyright and version information.\n"
  "\n"
  "Tunnel Options:\n"
  "--local host    : Local host name or ip address.\n"
  "--remote host [port] : Remote host name or ip address.\n"
  "--remote-random : If multiple --remote options specified, choose one randomly.\n"
  "--mode m        : Major mode, m = 'p2p' (default, point-to-point) or 'server'.\n"
  "--proto p       : Use protocol p for communicating with peer.\n"
  "                  p = udp (default), tcp-server, or tcp-client\n"
  "--connect-retry n : For --proto tcp-client, number of seconds to wait\n"
  "                  between connection retries (default=%d).\n"
  "--http-proxy s p [up] [auth] : Connect to remote host through an HTTP proxy at\n"
  "                  address s and port p.  If proxy authentication is required,\n"
  "                  up is a file containing username/password on 2 lines, or\n"
  "                  'stdin' to prompt from console.  Add auth='ntlm' if\n"
  "                  the proxy requires NTLM authentication.\n"
  "--http-proxy-retry : Retry indefinitely on HTTP proxy errors.\n"
  "--socks-proxy s [p]: Connect to remote host through a Socks5 proxy at address\n"
  "                  s and port p (default port = 1080).\n"
  "--socks-proxy-retry : Retry indefinitely on Socks proxy errors.\n"
  "--resolv-retry n: If hostname resolve fails for --remote, retry\n"
  "                  resolve for n seconds before failing (disabled by default).\n"
  "                  Set n=\"infinite\" to retry indefinitely.\n"
  "--float         : Allow remote to change its IP address/port, such as through\n"
  "                  DHCP (this is the default if --remote is not used).\n"
  "--ipchange cmd  : Execute shell command cmd on remote ip address initial\n"
  "                  setting or change -- execute as: cmd ip-address port#\n"
  "--port port     : TCP/UDP port # for both local and remote.\n"
  "--lport port    : TCP/UDP port # for local (default=%d).\n"
  "--rport port    : TCP/UDP port # for remote (default=%d).\n"
  "--nobind        : Do not bind to local address and port.\n"
  "--dev tunX|tapX : tun/tap device (X can be omitted for dynamic device.\n"
  "--dev-type dt   : Which device type are we using? (dt = tun or tap) Use\n"
  "                  this option only if the tun/tap device used with --dev\n"
  "                  does not begin with \"tun\" or \"tap\".\n"
  "--dev-node node : Explicitly set the device node rather than using\n"
  "                  /dev/net/tun, /dev/tun, /dev/tap, etc.\n"
  "--tun-ipv6      : Build tun link capable of forwarding IPv6 traffic.\n"
  "--ifconfig l rn : TUN: configure device to use IP address l as a local\n"
  "                  endpoint and rn as a remote endpoint.  l & rn should be\n"
  "                  swapped on the other peer.  l & rn must be private\n"
  "                  addresses outside of the subnets used by either peer.\n"
  "                  TAP: configure device to use IP address l as a local\n"
  "                  endpoint and rn as a subnet mask.\n"
  "--ifconfig-noexec : Don't actually execute ifconfig/netsh command, instead\n"
  "                    pass --ifconfig parms by environment to scripts.\n"
  "--ifconfig-nowarn : Don't warn if the --ifconfig option on this side of the\n"
  "                    connection doesn't match the remote side.\n"
  "--route network [netmask] [gateway] [metric] :\n"
  "                  Add route to routing table after connection\n"
  "                  is established.  Multiple routes can be specified.\n"
  "                  netmask default: 255.255.255.255\n"
  "                  gateway default: taken from --route-gateway or --ifconfig\n"
  "                  Specify default by leaving blank or setting to \"nil\".\n"
  "--route-gateway gw : Specify a default gateway for use with --route.\n"
  "--route-delay n [w] : Delay n seconds after connection initiation before\n"
  "                  adding routes (may be 0).  If not specified, routes will\n"
  "                  be added immediately after tun/tap open.  On Windows, wait\n"
  "                  up to w seconds for TUN/TAP adapter to come up.\n"
  "--route-up cmd  : Execute shell cmd after routes are added.\n"
  "--route-noexec  : Don't add routes automatically.  Instead pass routes to\n"
  "                  --route-up script using environmental variables.\n"
  "--redirect-gateway [flags]: (Experimental) Automatically execute routing\n"
  "                  commands to redirect all outgoing IP traffic through the\n"
  "                  VPN.  Add 'local' flag if both OpenVPN servers are directly\n"
  "                  connected via a common subnet, such as with WiFi.\n"
  "                  Add 'def1' flag to set default route using using 0.0.0.0/1\n"
  "                  and 128.0.0.0/1 rather than 0.0.0.0/0.\n"
  "--setenv name value : Set a custom environmental variable to pass to script.\n"
  "--shaper n      : Restrict output to peer to n bytes per second.\n"
  "--keepalive n m : Helper option for setting timeouts in server mode.  Send\n"
  "                  ping once every n seconds, restart if ping not received\n"
  "                  for m seconds.\n"
  "--inactive n    : Exit after n seconds of inactivity on tun/tap device.\n"
  "--ping-exit n   : Exit if n seconds pass without reception of remote ping.\n"
  "--ping-restart n: Restart if n seconds pass without reception of remote ping.\n"
  "--ping-timer-rem: Run the --ping-exit/--ping-restart timer only if we have a\n"
  "                  remote address.\n"
  "--ping n        : Ping remote once every n seconds over TCP/UDP port.\n"
  "--fast-io       : (experimental) Optimize TUN/TAP/UDP writes.\n"
  "--explicit-exit-notify n : (experimental) on exit, send exit signal to remote.\n"
  "--persist-tun   : Keep tun/tap device open across SIGUSR1 or --ping-restart.\n"
  "--persist-remote-ip : Keep remote IP address across SIGUSR1 or --ping-restart.\n"
  "--persist-local-ip  : Keep local IP address across SIGUSR1 or --ping-restart.\n"
  "--persist-key   : Don't re-read key files across SIGUSR1 or --ping-restart.\n"
#if PASSTOS_CAPABILITY
  "--passtos       : TOS passthrough (applies to IPv4 only).\n"
#endif
  "--tun-mtu n     : Take the tun/tap device MTU to be n and derive the\n"
  "                  TCP/UDP MTU from it (default=%d).\n"
  "--tun-mtu-extra n : Assume that tun/tap device might return as many\n"
  "                  as n bytes more than the tun-mtu size on read\n"
  "                  (default TUN=0 TAP=%d).\n"
  "--link-mtu n    : Take the TCP/UDP device MTU to be n and derive the tun MTU\n"
  "                  from it.\n"
  "--mtu-disc type : Should we do Path MTU discovery on TCP/UDP channel?\n"
  "                  'no'    -- Never send DF (Don't Fragment) frames\n"
  "                  'maybe' -- Use per-route hints\n"
  "                  'yes'   -- Always DF (Don't Fragment)\n"
  "--mtu-test      : Empirically measure and report MTU.\n"
  "--fragment max  : Enable internal datagram fragmentation so that no UDP\n"
  "                  datagrams are sent which are larger than max bytes.\n"
  "                  Adds 4 bytes of overhead per datagram.\n"
  "--mssfix [n]    : Set upper bound on TCP MSS, default = tun-mtu size\n"
  "                  or --fragment max value, whichever is lower.\n"
  "--sndbuf size   : Set the TCP/UDP send buffer size.\n"
  "--rcvbuf size   : Set the TCP/UDP receive buffer size.\n"
  "--txqueuelen n  : Set the tun/tap TX queue length to n (Linux only).\n"
  "--mlock         : Disable Paging -- ensures key material and tunnel\n"
  "                  data will never be written to disk.\n"
  "--up cmd        : Shell cmd to execute after successful tun device open.\n"
  "                  Execute as: cmd tun/tap-dev tun-mtu link-mtu \\\n"
  "                              ifconfig-local-ip ifconfig-remote-ip\n"
  "                  (pre --user or --group UID/GID change)\n"
  "--up-delay      : Delay tun/tap open and possible --up script execution\n"
  "                  until after TCP/UDP connection establishment with peer.\n"
  "--down cmd      : Shell cmd to run after tun device close.\n"
  "                  (post --user/--group UID/GID change and/or --chroot)\n"
  "                  (script parameters are same as --up option)\n"
  "--down-pre      : Call --down cmd/script before TUN/TAP close.\n"
  "--up-restart    : Run up/down scripts for all restarts including those\n"
  "                  caused by --ping-restart or SIGUSR1\n"
  "--user user     : Set UID to user after initialization.\n"
  "--group group   : Set GID to group after initialization.\n"
  "--chroot dir    : Chroot to this directory after initialization.\n"
  "--cd dir        : Change to this directory before initialization.\n"
  "--daemon [name] : Become a daemon after initialization.\n"
  "                  The optional 'name' parameter will be passed\n"
  "                  as the program name to the system logger.\n"
  "--inetd [name] ['wait'|'nowait'] : Run as an inetd or xinetd server.\n"
  "                  See --daemon above for a description of the 'name' parm.\n"
  "--log file      : Output log to file which is created/truncated on open.\n"
  "--log-append file : Append log to file, or create file if nonexistent.\n"
  "--suppress-timestamps : Don't log timestamps to stdout/stderr.\n"
  "--writepid file : Write main process ID to file.\n"
  "--nice n        : Change process priority (>0 = lower, <0 = higher).\n"
#ifdef USE_PTHREAD
  "--nice-work n   : Change thread priority of work thread.  The work\n"
  "                  thread is used for background processing such as\n"
  "                  RSA key number crunching.\n"
#endif
  "--verb n        : Set output verbosity to n (default=%d):\n"
  "                  (Level 3 is recommended if you want a good summary\n"
  "                  of what's happening without being swamped by output).\n"
  "                : 0 -- no output except fatal errors\n"
  "                : 1 -- startup info + connection initiated messages +\n"
  "                       non-fatal encryption & net errors\n"
  "                : 2 -- show TLS negotiations\n"
  "                : 3 -- show extra TLS info + --gremlin net outages +\n"
  "                       adaptive compress info\n"
  "                : 4 -- show parameters\n"
  "                : 5 -- show 'RrWw' chars on console for each packet sent\n"
  "                       and received from TCP/UDP (caps) or tun/tap (lc)\n"
  "                : 6 to 11 -- debug messages of increasing verbosity\n"
  "--mute n        : Log at most n consecutive messages in the same category.\n"
  "--status file n : Write operational status to file every n seconds.\n"
  "--disable-occ   : Disable options consistency check between peers.\n"
  "--gremlin       : Simulate dropped & corrupted packets + network outages\n"
  "                  to test robustness of protocol (for debugging only).\n"
#ifdef USE_LZO
  "--comp-lzo      : Use fast LZO compression -- may add up to 1 byte per\n"
  "                  packet for uncompressible data.\n"
  "--comp-noadapt  : Don't use adaptive compression when --comp-lzo\n"
  "                  is specified.\n"
#endif
#if P2MP
  "\n"
  "Multi-Client Server options (when --mode server is used):\n"
  "--server network netmask : Helper option to easily configure server mode.\n"
  "--server-bridge IP netmask pool-start-IP pool-end-IP : Helper option to\n"
  "                    easily configure ethernet bridging server mode.\n"
  "--push \"option\" : Push a config file option back to the peer for remote\n"
  "                  execution.  Peer must specify --pull in its config file.\n"
  "--push-reset    : Don't inherit global push list for specific\n"
  "                  client instance.\n"
  "--ifconfig-pool start-IP end-IP [netmask] : Set aside a pool of subnets\n"
  "                  to be dynamically allocated to connecting clients.\n"
  "--ifconfig-pool-linear : Use individual addresses rather than /30 subnets\n"
  "                  in tun mode.  Not compatible with Windows clients.\n"
  "--ifconfig-pool-persist file [seconds] : Persist/unpersist ifconfig-pool\n"
  "                  data to file, at seconds intervals (default=600).\n"
  "                  If seconds=0, file will be treated as read-only.\n"
  "--ifconfig-push local remote-netmask : Push an ifconfig option to remote,\n"
  "                  overrides --ifconfig-pool dynamic allocation.\n"
  "                  Must be associated with a specific client instance.\n"
  "--iroute network [netmask] : Route subnet to client.\n"
  "                  Sets up internal routes only, and must be\n"
  "                  associated with a specific client instance.\n"
  "--client-cert-not-required : Don't require client certificate, client\n"
  "                  will authenticate using username/password.\n"
  "--username-as-common-name  : For auth-user-pass authentication, use\n"
  "                  the authenticated username as the common name,\n"
  "                  rather than the common name from the client cert.\n"
  "--auth-user-pass-verify cmd : Query client for username/password and run\n"
  "                  script cmd to verify.\n"
  "--client-to-client : Internally route client-to-client traffic.\n"
  "--duplicate-cn  : Allow multiple clients with the same common name to\n"
  "                  concurrently connect.\n"
  "--client-connect cmd : Run script cmd on client connection.\n"
  "--client-disconnect cmd : Run script cmd on client disconnection.\n"
  "--client-config-dir dir : Directory for custom client config files.\n"
  "--ccd-exclusive : Refuse connection unless custom client config is found.\n"
  "--tmp-dir dir   : Temporary directory, used for --client-connect return file.\n"
  "--hash-size r v : Set the size of the real address hash table to r and the\n"
  "                  virtual address table to v.\n"
  "--bcast-buffers n : Allocate n broadcast buffers.\n"
  "--tcp-queue-limit n : Maximum number of queued TCP output packets.\n"
  "--learn-address cmd : Run script cmd to validate client virtual addresses.\n"
  "--connect-freq n s : Allow a maximum of n new connections per s seconds.\n"
  "--max-clients n : Allow a maximum of n simultaneously connected clients.\n"
  "\n"
  "Client options (when connecting to a multi-client server):\n"
  "--client         : Helper option to easily configure client mode.\n"
  "--auth-user-pass [up] : Authenticate with server using username/password.\n"
  "                  up is a file containing username/password on 2 lines,\n"
  "                  or omit to prompt from console.\n"
  "--pull           : Accept certain config file options from the peer as if they\n"
  "                  were part of the local config file.  Must be specified\n"
  "                  when connecting to a '--mode server' remote host.\n"
#endif
#ifdef USE_CRYPTO
  "\n"
  "Data Channel Encryption Options (must be compatible between peers):\n"
  "(These options are meaningful for both Static Key & TLS-mode)\n"
  "--secret f [d]  : Enable Static Key encryption mode (non-TLS).\n"
  "                  Use shared secret file f, generate with --genkey.\n"
  "                  The optional d parameter controls key directionality.\n"
  "                  If d is specified, use separate keys for each\n"
  "                  direction, set d=0 on one side of the connection,\n"
  "                  and d=1 on the other side.\n"
  "--auth alg      : Authenticate packets with HMAC using message\n"
  "                  digest algorithm alg (default=%s).\n"
  "                  (usually adds 16 or 20 bytes per packet)\n"
  "                  Set alg=none to disable authentication.\n"
  "--cipher alg    : Encrypt packets with cipher algorithm alg\n"
  "                  (default=%s).\n"
  "                  Set alg=none to disable encryption.\n"
#ifdef HAVE_EVP_CIPHER_CTX_SET_KEY_LENGTH
  "--keysize n     : Size of cipher key in bits (optional).\n"
  "                  If unspecified, defaults to cipher-specific default.\n"
#endif
  "--engine [name] : Enable OpenSSL hardware crypto engine functionality.\n"
  "--no-replay     : Disable replay protection.\n"
  "--mute-replay-warnings : Silence the output of replay warnings to log file.\n"
  "--replay-window n [t]  : Use a replay protection sliding window of size n\n"
  "                         and a time window of t seconds.\n"
  "                         Default n=%d t=%d\n"
  "--no-iv         : Disable cipher IV -- only allowed with CBC mode ciphers.\n"
  "--replay-persist file : Persist replay-protection state across sessions\n"
  "                  using file.\n"
  "--test-crypto   : Run a self-test of crypto features enabled.\n"
  "                  For debugging only.\n"
#ifdef USE_SSL
  "\n"
  "TLS Key Negotiation Options:\n"
  "(These options are meaningful only for TLS-mode)\n"
  "--tls-server    : Enable TLS and assume server role during TLS handshake.\n"
  "--tls-client    : Enable TLS and assume client role during TLS handshake.\n"
  "--key-method m  : Data channel key exchange method.  m should be a method\n"
  "                  number, such as 1 (default), 2, etc.\n"
  "--ca file       : Certificate authority file in .pem format containing\n"
  "                  root certificate.\n"
  "--dh file       : File containing Diffie Hellman parameters\n"
  "                  in .pem format (for --tls-server only).\n"
  "                  Use \"openssl dhparam -out dh1024.pem 1024\" to generate.\n"
  "--cert file     : Local certificate in .pem format -- must be signed\n"
  "                  by a Certificate Authority in --ca file.\n"
  "--key file      : Local private key in .pem format.\n"
  "--pkcs12 file   : PKCS#12 file containing local private key, local certificate\n"
  "                  and root CA certificate.\n" 
  "--tls-cipher l  : A list l of allowable TLS ciphers separated by : (optional).\n"
  "                : Use --show-tls to see a list of supported TLS ciphers.\n"
  "--tls-timeout n : Packet retransmit timeout on TLS control channel\n"
  "                  if no ACK from remote within n seconds (default=%d).\n"
  "--reneg-bytes n : Renegotiate data chan. key after n bytes sent and recvd.\n"
  "--reneg-pkts n  : Renegotiate data chan. key after n packets sent and recvd.\n"
  "--reneg-sec n   : Renegotiate data chan. key after n seconds (default=%d).\n"
  "--hand-window n : Data channel key exchange must finalize within n seconds\n"
  "                  of handshake initiation by any peer (default=%d).\n"
  "--tran-window n : Transition window -- old key can live this many seconds\n"
  "                  after new key renegotiation begins (default=%d).\n"
  "--single-session: Allow only one session (reset state on restart).\n"
  "--tls-exit      : Exit on TLS negotiation failure.\n"
  "--tls-auth f [d]: Add an additional layer of authentication on top of the TLS\n"
  "                  control channel to protect against DoS attacks.\n"
  "                  f (required) is a shared-secret passphrase file.\n"
  "                  The optional d parameter controls key directionality,\n"
  "                  see --secret option for more info.\n"
  "--askpass [file]: Get PEM password from controlling tty before we daemonize.\n"
  "--crl-verify crl: Check peer certificate against a CRL.\n"
  "--tls-verify cmd: Execute shell command cmd to verify the X509 name of a\n"
  "                  pending TLS connection that has otherwise passed all other\n"
  "                  tests of certification.  cmd should return 0 to allow\n"
  "                  TLS handshake to proceed, or 1 to fail.  (cmd is\n"
  "                  executed as 'cmd certificate_depth X509_NAME_oneline')\n"
  "--tls-remote x509name: Accept connections only from a host with X509 name\n"
  "                  x509name. The remote host must also pass all other tests\n"
  "                  of verification.\n"
#endif				/* USE_SSL */
  "\n"
  "SSL Library information:\n"
  "--show-ciphers  : Show cipher algorithms to use with --cipher option.\n"
  "--show-digests  : Show message digest algorithms to use with --auth option.\n"
  "--show-engines  : Show hardware crypto accelerator engines (if available).\n"
#ifdef USE_SSL
  "--show-tls      : Show all TLS ciphers (TLS used only as a control channel).\n"
#endif
#ifdef WIN32
  "\n"
  "Windows Specific:\n"
  "--ip-win32 method : When using --ifconfig on Windows, set TAP-Win32 adapter\n"
  "                    IP address using method = manual, netsh, ipapi, or\n"
  "                    dynamic (default = ipapi).\n"
  "                    Dynamic method allows two optional parameters:\n"
  "                    offset: DHCP server address offset (> -256 and < 256).\n"
  "                            If 0, use network address, if >0, take nth\n"
  "                            address forward from network address, if <0,\n"
  "                            take nth address backward from broadcast\n"
  "                            address.\n"
  "                            Default is 0.\n"
  "                    lease-time: Lease time in seconds.\n"
  "                                Default is one year.\n"
  "--route-method    : Which method to use for adding routes on Windows?\n"
  "                    ipapi (default) -- Use IP helper API.\n"
  "                    exe -- Call the route.exe shell command.\n"
  "--dhcp-option type [parm] : Set extended TAP-Win32 properties, must\n"
  "                    be used with --ip-win32 dynamic.  For options\n"
  "                    which allow multiple addresses,\n"
  "                    --dhcp-option must be repeated.\n"
  "                    DOMAIN name : Set DNS suffix\n"
  "                    DNS addr    : Set domain name server address(es)\n"
  "                    NTP         : Set NTP server address(es)\n"
  "                    NBDD        : Set NBDD server address(es)\n"
  "                    WINS addr   : Set WINS server address(es)\n"
  "                    NBT type    : Set NetBIOS over TCP/IP Node type\n"
  "                                  1: B, 2: P, 4: M, 8: H\n"
  "                    NBS id      : Set NetBIOS scope ID\n"
  "--dhcp-renew       : Ask Windows to renew the TAP adapter lease on startup.\n"
  "--dhcp-pre-release : Ask Windows to release the previous TAP adapter lease on\n"
"                       startup.\n"
  "--dhcp-release     : Ask Windows to release the TAP adapter lease on shutdown.\n"
  "--tap-sleep n   : Sleep for n seconds after TAP adapter open before\n"
  "                  attempting to set adapter properties.\n"
  "--pause-exit         : When run from a console window, pause before exiting.\n"
  "--service ex [0|1]   : For use when OpenVPN is being instantiated by a\n"
  "                       service, and should not be used directly by end-users.\n"
  "                       ex is the name of an event object which, when\n"
  "                       signaled, will cause OpenVPN to exit.  A second\n"
  "                       optional parameter controls the initial state of ex.\n"
  "--show-net-up   : Show OpenVPN's view of routing table and net adapter list\n"
  "                  after TAP adapter is up and routes have been added.\n"
  "Windows Standalone Options:\n"
  "\n"
  "--show-adapters : Show all TAP-Win32 adapters.\n"
  "--show-net      : Show OpenVPN's view of routing table and net adapter list.\n"
  "--show-valid-subnets : Show valid subnets for --dev tun emulation.\n"
#endif
  "\n"
  "Generate a random key (only for non-TLS static key encryption mode):\n"
  "--genkey        : Generate a random key to be used as a shared secret,\n"
  "                  for use with the --secret option.\n"
  "--secret file   : Write key to file.\n"
#endif				/* USE_CRYPTO */
#ifdef TUNSETPERSIST
  "\n"
  "Tun/tap config mode (available with linux 2.4+):\n"
  "--mktun         : Create a persistent tunnel.\n"
  "--rmtun         : Remove a persistent tunnel.\n"
  "--dev tunX|tapX : tun/tap device\n"
  "--dev-type dt   : Device type.  See tunnel options above for details.\n"
#endif
 ;

/*
 * This is where the options defaults go.
 * Any option not explicitly set here
 * will be set to 0.
 */
void
init_options (struct options *o)
{
  CLEAR (*o);
  gc_init (&o->gc);
  o->mode = MODE_POINT_TO_POINT;
  o->proto = PROTO_UDPv4;
  o->connect_retry_seconds = 5;
  o->local_port = o->remote_port = 5000;
  o->verbosity = 1;
  o->status_file_update_freq = 60;
  o->bind_local = true;
  o->tun_mtu = TUN_MTU_DEFAULT;
  o->link_mtu = LINK_MTU_DEFAULT;
  o->mtu_discover_type = -1;
  o->occ = true;
  o->mssfix = MSSFIX_DEFAULT;
  o->route_delay_window = 30;
  o->resolve_retry_seconds = RESOLV_RETRY_INFINITE;
#ifdef TUNSETPERSIST
  o->persist_mode = 1;
#endif
#ifndef WIN32
  o->rcvbuf = 65536;
  o->sndbuf = 65536;
#endif
#ifdef USE_LZO
  o->comp_lzo_adaptive = true;
#endif
#ifdef TARGET_LINUX
  o->tuntap_options.txqueuelen = 100;
#endif
#ifdef WIN32
  o->tuntap_options.ip_win32_type = IPW32_SET_DHCP_MASQ;
  o->tuntap_options.dhcp_lease_time = 31536000; /* one year */
  o->tuntap_options.dhcp_masq_offset = 0;       /* use network address as internal DHCP server address */
  o->route_method = ROUTE_METHOD_IPAPI;
#endif
#ifdef USE_PTHREAD
  o->n_threads = 1;
#endif
#if P2MP
  o->real_hash_size = 256;
  o->virtual_hash_size = 256;
  o->n_bcast_buf = 256;
  o->tcp_queue_limit = 64;
  o->max_clients = 1024;
  o->ifconfig_pool_persist_refresh_freq = 600;
#endif
#ifdef USE_CRYPTO
  o->ciphername = "BF-CBC";
  o->ciphername_defined = true;
  o->authname = "SHA1";
  o->authname_defined = true;
  o->replay = true;
  o->replay_window = DEFAULT_SEQ_BACKTRACK;
  o->replay_time = DEFAULT_TIME_BACKTRACK;
  o->use_iv = true;
  o->key_direction = KEY_DIRECTION_BIDIRECTIONAL;
#ifdef USE_SSL
  o->key_method = 2;
  o->tls_timeout = 2;
  o->renegotiate_seconds = 3600;
  o->handshake_window = 60;
  o->transition_window = 3600;
#endif
#endif
}

void
uninit_options (struct options *o)
{
  gc_free (&o->gc);
}

#define SHOW_PARM(name, value, format) msg(D_SHOW_PARMS, "  " #name " = " format, (value))
#define SHOW_STR(var)       SHOW_PARM(var, (o->var ? o->var : "[UNDEF]"), "'%s'")
#define SHOW_INT(var)       SHOW_PARM(var, o->var, "%d")
#define SHOW_UINT(var)      SHOW_PARM(var, o->var, "%u")
#define SHOW_UNSIGNED(var)  SHOW_PARM(var, o->var, "0x%08x")
#define SHOW_BOOL(var)      SHOW_PARM(var, (o->var ? "ENABLED" : "DISABLED"), "%s");

void
setenv_settings (struct env_set *es, const struct options *o)
{
  setenv_str (es, "config", o->config);
  setenv_str (es, "proto", proto2ascii (o->proto, false));
  setenv_str (es, "local", o->local);
  setenv_int (es, "local_port", o->local_port);

  if (o->remote_list)
    {
      int i;

      for (i = 0; i < o->remote_list->len; ++i)
	{
	  char remote_string[64];
	  char remote_port_string[64];

	  openvpn_snprintf (remote_string, sizeof (remote_string), "remote_%d", i+1);
	  openvpn_snprintf (remote_port_string, sizeof (remote_port_string), "remote_port_%d", i+1);

	  setenv_str (es, remote_string,      o->remote_list->array[i].hostname);
	  setenv_int (es, remote_port_string, o->remote_list->array[i].port);
	}
    }
}

static in_addr_t
get_ip_addr (const char *ip_string, int msglevel, bool *error)
{
  unsigned int flags = GETADDR_HOST_ORDER;
  bool succeeded = false;
  in_addr_t ret;

  if (msglevel & M_FATAL)
    flags |= GETADDR_FATAL;

  ret = getaddr (flags, ip_string, 0, &succeeded, NULL);
  if (!succeeded && error)
    *error = true;
  return ret;
}

static char *
string_substitute (const char *src, int from, int to, struct gc_arena *gc)
{
  char *ret = (char *) gc_malloc (strlen (src) + 1, true, gc);
  char *dest = ret;
  char c;

  do
    {
      c = *src++;
      if (c == from)
	c = to;
      *dest++ = c;
    }
  while (c);
  return ret;
}

bool
is_persist_option (const struct options *o)
{
  return o->persist_tun
      || o->persist_key
      || o->persist_local_ip
      || o->persist_remote_ip
#ifdef USE_PTHREAD
      || o->n_threads >= 2
#endif
    ;
}

bool
is_stateful_restart (const struct options *o)
{
  return is_persist_option (o) || (o->remote_list && o->remote_list->len > 1);
}

#ifdef WIN32

static void
show_dhcp_option_addrs (const char *name, const in_addr_t *array, int len)
{
  struct gc_arena gc = gc_new ();
  int i;
  for (i = 0; i < len; ++i)
    {
      msg (D_SHOW_PARMS, "  %s[%d] = %s",
	   name,
	   i,
	   print_in_addr_t (array[i], 0, &gc));
    }
  gc_free (&gc);
}

static void
show_tuntap_options (const struct tuntap_options *o)
{
  SHOW_BOOL (ip_win32_defined);
  SHOW_INT (ip_win32_type);
  SHOW_INT (dhcp_masq_offset);
  SHOW_INT (dhcp_lease_time);
  SHOW_INT (tap_sleep);
  SHOW_BOOL (dhcp_options);
  SHOW_BOOL (dhcp_renew);
  SHOW_BOOL (dhcp_pre_release);
  SHOW_BOOL (dhcp_release);
  SHOW_STR (domain);
  SHOW_STR (netbios_scope);
  SHOW_INT (netbios_node_type);

  show_dhcp_option_addrs ("DNS", o->dns, o->dns_len);
  show_dhcp_option_addrs ("WINS", o->wins, o->wins_len);
  show_dhcp_option_addrs ("NTP", o->ntp, o->ntp_len);
  show_dhcp_option_addrs ("NBDD", o->nbdd, o->nbdd_len);
}

static void
dhcp_option_address_parse (const char *name, const char *parm, in_addr_t *array, int *len, int msglevel)
{
  if (*len >= N_DHCP_ADDR)
    {
      msg (msglevel, "--dhcp-option %s: maximum of %d %s servers can be specified",
	   name,
	   N_DHCP_ADDR,
	   name);
    }
  else
    {
      bool error = false;
      const in_addr_t addr = get_ip_addr (parm, msglevel, &error);
      if (!error)
	array[(*len)++] = addr;
    }
}

#endif

#if P2MP
static void
show_p2mp_parms (const struct options *o)
{
  struct gc_arena gc = gc_new ();
  msg (D_SHOW_PARMS, "  server_network = %s", print_in_addr_t (o->server_network, 0, &gc));
  msg (D_SHOW_PARMS, "  server_netmask = %s", print_in_addr_t (o->server_netmask, 0, &gc));
  msg (D_SHOW_PARMS, "  server_bridge_ip = %s", print_in_addr_t (o->server_bridge_ip, 0, &gc));
  msg (D_SHOW_PARMS, "  server_bridge_netmask = %s", print_in_addr_t (o->server_bridge_netmask, 0, &gc));
  msg (D_SHOW_PARMS, "  server_bridge_pool_start = %s", print_in_addr_t (o->server_bridge_pool_start, 0, &gc));
  msg (D_SHOW_PARMS, "  server_bridge_pool_end = %s", print_in_addr_t (o->server_bridge_pool_end, 0, &gc));
  SHOW_BOOL (client);
  if (o->push_list)
    {
      const struct push_list *l = o->push_list;
      const char *printable_push_list = l->options;
      msg (D_SHOW_PARMS, "  push_list = '%s'", printable_push_list);
    }
  SHOW_BOOL (pull);
  SHOW_BOOL (ifconfig_pool_defined);
  msg (D_SHOW_PARMS, "  ifconfig_pool_start = %s", print_in_addr_t (o->ifconfig_pool_start, 0, &gc));
  msg (D_SHOW_PARMS, "  ifconfig_pool_end = %s", print_in_addr_t (o->ifconfig_pool_end, 0, &gc));
  msg (D_SHOW_PARMS, "  ifconfig_pool_netmask = %s", print_in_addr_t (o->ifconfig_pool_netmask, 0, &gc));
  SHOW_STR (ifconfig_pool_persist_filename);
  SHOW_INT (ifconfig_pool_persist_refresh_freq);
  SHOW_BOOL (ifconfig_pool_linear);
  SHOW_INT (n_bcast_buf);
  SHOW_INT (tcp_queue_limit);
  SHOW_INT (real_hash_size);
  SHOW_INT (virtual_hash_size);
  SHOW_STR (client_connect_script);
  SHOW_STR (learn_address_script);
  SHOW_STR (client_disconnect_script);
  SHOW_STR (client_config_dir);
  SHOW_BOOL (ccd_exclusive);
  SHOW_STR (tmp_dir);
  SHOW_BOOL (push_ifconfig_defined);
  msg (D_SHOW_PARMS, "  push_ifconfig_local = %s", print_in_addr_t (o->push_ifconfig_local, 0, &gc));
  msg (D_SHOW_PARMS, "  push_ifconfig_remote_netmask = %s", print_in_addr_t (o->push_ifconfig_remote_netmask, 0, &gc));
  SHOW_BOOL (enable_c2c);
  SHOW_BOOL (duplicate_cn);
  SHOW_INT (cf_max);
  SHOW_INT (cf_per);
  SHOW_INT (max_clients);

  SHOW_BOOL (client_cert_not_required);
  SHOW_BOOL (username_as_common_name)
  SHOW_STR (auth_user_pass_verify_script);
  SHOW_STR (auth_user_pass_file);

  gc_free (&gc);
}

static void
option_iroute (struct options *o,
	       const char *network_str,
	       const char *netmask_str,
	       int msglevel)
{
  struct iroute *ir;

  ALLOC_OBJ_GC (ir, struct iroute, &o->gc);
  ir->network = getaddr (GETADDR_HOST_ORDER, network_str, 0, NULL, NULL);
  ir->netbits = -1;

  if (netmask_str)
    {
      const in_addr_t netmask = getaddr (GETADDR_HOST_ORDER, netmask_str, 0, NULL, NULL);
      if (!netmask_to_netbits (ir->network, netmask, &ir->netbits))
	{
	  msg (msglevel, "Options error: in --iroute %s %s : Bad network/subnet specification",
	       network_str,
	       netmask_str);
	  return;
	}
    }

  ir->next = o->iroutes;
  o->iroutes = ir;
}

#endif

static void
show_remote_list (const struct remote_list *l)
{
  if (l)
    {
      int i;
      for (i = 0; i < l->len; ++i)
	{
	  msg (D_SHOW_PARMS, "  remote_list[%d] = {'%s', %d}",
	       i, l->array[i].hostname, l->array[i].port);
	}
    }
  else
    {
      msg (D_SHOW_PARMS, "  remote_list = NULL");
    }
}

void
options_detach (struct options *o)
{
  gc_detach (&o->gc);
  o->routes = NULL;
#if P2MP
  if (o->push_list) /* clone push_list */
    {
      const struct push_list *old = o->push_list;
      ALLOC_OBJ_GC (o->push_list, struct push_list, &o->gc);
      strcpy (o->push_list->options, old->options);
    }
#endif
}

void
rol_check_alloc (struct options *options)
{
  if (!options->routes)
    options->routes = new_route_option_list (&options->gc);
}

void
show_settings (const struct options *o)
{
  msg (D_SHOW_PARMS, "Current Parameter Settings:");

  SHOW_STR (config);
  
  SHOW_INT (mode);

#ifdef TUNSETPERSIST
  SHOW_BOOL (persist_config);
  SHOW_INT (persist_mode);
#endif

#ifdef USE_CRYPTO
  SHOW_BOOL (show_ciphers);
  SHOW_BOOL (show_digests);
  SHOW_BOOL (show_engines);
  SHOW_BOOL (genkey);
#ifdef USE_SSL
  SHOW_STR (key_pass_file);
  SHOW_BOOL (show_tls_ciphers);
#endif
#endif

  SHOW_INT (proto);
  SHOW_STR (local);
  show_remote_list (o->remote_list);
  SHOW_BOOL (remote_random);

  SHOW_INT (local_port);
  SHOW_INT (remote_port);
  SHOW_BOOL (remote_float);
  SHOW_STR (ipchange);
  SHOW_BOOL (bind_local);
  SHOW_STR (dev);
  SHOW_STR (dev_type);
  SHOW_STR (dev_node);
  SHOW_BOOL (tun_ipv6);
  SHOW_STR (ifconfig_local);
  SHOW_STR (ifconfig_remote_netmask);
  SHOW_BOOL (ifconfig_noexec);
  SHOW_BOOL (ifconfig_nowarn);

#ifdef HAVE_GETTIMEOFDAY
  SHOW_INT (shaper);
#endif
  SHOW_INT (tun_mtu);
  SHOW_BOOL (tun_mtu_defined);
  SHOW_INT (link_mtu);
  SHOW_BOOL (link_mtu_defined);
  SHOW_INT (tun_mtu_extra);
  SHOW_BOOL (tun_mtu_extra_defined);
  SHOW_INT (fragment);
  SHOW_INT (mtu_discover_type);
  SHOW_INT (mtu_test);

  SHOW_BOOL (mlock);

  SHOW_INT (keepalive_ping);
  SHOW_INT (keepalive_timeout);
  SHOW_INT (inactivity_timeout);
  SHOW_INT (ping_send_timeout);
  SHOW_INT (ping_rec_timeout);
  SHOW_INT (ping_rec_timeout_action);
  SHOW_BOOL (ping_timer_remote);
  SHOW_INT (explicit_exit_notification);

  SHOW_BOOL (persist_tun);
  SHOW_BOOL (persist_local_ip);
  SHOW_BOOL (persist_remote_ip);
  SHOW_BOOL (persist_key);

  SHOW_INT (mssfix);
  
#if PASSTOS_CAPABILITY
  SHOW_BOOL (passtos);
#endif

  SHOW_INT (resolve_retry_seconds);
  SHOW_INT (connect_retry_seconds);

  SHOW_STR (username);
  SHOW_STR (groupname);
  SHOW_STR (chroot_dir);
  SHOW_STR (cd_dir);
  SHOW_STR (writepid);
  SHOW_STR (up_script);
  SHOW_STR (down_script);
  SHOW_BOOL (down_pre);
  SHOW_BOOL (up_restart);
  SHOW_BOOL (up_delay);
  SHOW_BOOL (daemon);
  SHOW_INT (inetd);
  SHOW_BOOL (log);
  SHOW_BOOL (suppress_timestamps);
  SHOW_INT (nice);
  SHOW_INT (verbosity);
  SHOW_INT (mute);
  SHOW_BOOL (gremlin);
  SHOW_STR (status_file);
  SHOW_INT (status_file_update_freq);

  SHOW_BOOL (occ);

  SHOW_INT (rcvbuf);
  SHOW_INT (sndbuf);

  SHOW_STR (http_proxy_server);
  SHOW_INT (http_proxy_port);
  SHOW_STR (http_proxy_auth_method);
  SHOW_STR (http_proxy_auth_file);
  SHOW_BOOL (http_proxy_retry);

  SHOW_STR (socks_proxy_server);
  SHOW_INT (socks_proxy_port);
  SHOW_BOOL (socks_proxy_retry);

  SHOW_BOOL (fast_io);

#ifdef USE_LZO
  SHOW_BOOL (comp_lzo);
  SHOW_BOOL (comp_lzo_adaptive);
#endif

  SHOW_STR (route_script);
  SHOW_STR (route_default_gateway);
  SHOW_BOOL (route_noexec);
  SHOW_INT (route_delay);
  SHOW_INT (route_delay_window);
  SHOW_BOOL (route_delay_defined);
  if (o->routes)
    print_route_options (o->routes, D_SHOW_PARMS);

#ifdef USE_CRYPTO
  SHOW_STR (shared_secret_file);
  SHOW_INT (key_direction);
  SHOW_BOOL (ciphername_defined);
  SHOW_STR (ciphername);
  SHOW_BOOL (authname_defined);
  SHOW_STR (authname);
  SHOW_INT (keysize);
  SHOW_BOOL (engine);
  SHOW_BOOL (replay);
  SHOW_BOOL (mute_replay_warnings);
  SHOW_INT (replay_window);
  SHOW_INT (replay_time);
  SHOW_STR (packet_id_file);
  SHOW_BOOL (use_iv);
  SHOW_BOOL (test_crypto);

#ifdef USE_SSL
  SHOW_BOOL (tls_server);
  SHOW_BOOL (tls_client);
  SHOW_INT (key_method);
  SHOW_STR (ca_file);
  SHOW_STR (dh_file);
  SHOW_STR (cert_file);
  SHOW_STR (priv_key_file);
  SHOW_STR (pkcs12_file);
  SHOW_STR (cipher_list);
  SHOW_STR (tls_verify);
  SHOW_STR (tls_remote);
  SHOW_STR (crl_file);

  SHOW_INT (tls_timeout);

  SHOW_INT (renegotiate_bytes);
  SHOW_INT (renegotiate_packets);
  SHOW_INT (renegotiate_seconds);

  SHOW_INT (handshake_window);
  SHOW_INT (transition_window);

  SHOW_BOOL (single_session);
  SHOW_BOOL (tls_exit);

  SHOW_STR (tls_auth_file);
#endif
#endif

#if P2MP
  show_p2mp_parms (o);
#endif

#ifdef WIN32
  SHOW_BOOL (show_net_up);
  SHOW_INT (route_method);
  show_tuntap_options (&o->tuntap_options);
#endif
}

#undef SHOW_PARM
#undef SHOW_STR
#undef SHOW_INT
#undef SHOW_BOOL

/*
 * Sanity check on options.
 * Also set some options based on other
 * options.
 */
void
options_postprocess (struct options *options, bool first_time)
{
  struct options defaults;
  int dev = DEV_TYPE_UNDEF;
  int i;
  bool pull = false;

  init_options (&defaults);

#ifdef USE_CRYPTO
  if (options->test_crypto)
    {
      notnull (options->shared_secret_file, "key file (--secret)");
    }
  else
#endif
    notnull (options->dev, "TUN/TAP device (--dev)");

  /*
   * Get tun/tap/null device type
   */
  dev = dev_type_enum (options->dev, options->dev_type);

  /*
   * Fill in default port number for --remote list
   */
  if (options->remote_list)
    {
      for (i = 0; i < options->remote_list->len; ++i)
	{
	  struct remote_entry *e = &options->remote_list->array[i];
	  if (e->port < 0)
	    e->port = options->remote_port;
	}
    }

  /*
   * Sanity check on daemon/inetd modes
   */

  if (options->daemon && options->inetd)
    msg (M_USAGE, "Options error: only one of --daemon or --inetd may be specified");

  if (options->inetd && (options->local || options->remote_list))
    msg (M_USAGE, "Options error: --local or --remote cannot be used with --inetd");

  if (options->inetd && options->proto == PROTO_TCPv4_CLIENT)
    msg (M_USAGE, "Options error: --proto tcp-client cannot be used with --inetd");

  if (options->inetd == INETD_NOWAIT && options->proto != PROTO_TCPv4_SERVER)
    msg (M_USAGE, "Options error: --inetd nowait can only be used with --proto tcp-server");

  if (options->inetd == INETD_NOWAIT
#if defined(USE_CRYPTO) && defined(USE_SSL)
      && !(options->tls_server || options->tls_client)
#endif
      )
    msg (M_USAGE, "Options error: --inetd nowait can only be used in TLS mode");

  if (options->inetd == INETD_NOWAIT && dev != DEV_TYPE_TAP)
    msg (M_USAGE, "Options error: --inetd nowait only makes sense in --dev tap mode");

  /*
   * In forking TCP server mode, you don't need to ifconfig
   * the tap device (the assumption is that it will be bridged).
   */
  if (options->inetd == INETD_NOWAIT)
    options->ifconfig_noexec = true;

  /*
   * Sanity check on TCP mode options
   */

  if (options->connect_retry_defined && options->proto != PROTO_TCPv4_CLIENT)
    msg (M_USAGE, "Options error: --connect-retry doesn't make sense unless also used with --proto tcp-client");

  /*
   * Sanity check on MTU parameters
   */
  if (options->tun_mtu_defined && options->link_mtu_defined)
    msg (M_USAGE, "Options error: only one of --tun-mtu or --link-mtu may be defined (note that --ifconfig implies --link-mtu %d)", LINK_MTU_DEFAULT);

  if (options->proto != PROTO_UDPv4 && options->mtu_test)
    msg (M_USAGE, "Options error: --mtu-test only makes sense with --proto udp");

  /*
   * Set MTU defaults
   */
  {
    if (!options->tun_mtu_defined && !options->link_mtu_defined)
      {
	options->tun_mtu_defined = true;
      }
    if ((dev == DEV_TYPE_TAP) && !options->tun_mtu_extra_defined)
      {
	options->tun_mtu_extra_defined = true;
	options->tun_mtu_extra = TAP_MTU_EXTRA_DEFAULT;
      }
  }

  /*
   * Process helper-type options which map to other, more complex
   * sequences of options.
   */
  helper_client_server (options);
  helper_keepalive (options);

  /* will we be pulling options from server? */
#if P2MP
  pull = options->pull;
#endif

  /*
   * Sanity check on --local, --remote, and --ifconfig
   */

  if (options->remote_list)
    {
      int i;
      struct remote_list *l = options->remote_list;

      for (i = 0; i < l->len; ++i)
	{
	  const char *remote = l->array[i].hostname;
	  const int remote_port = l->array[i].port;

	  if (string_defined_equal (options->local, remote)
	      && options->local_port == remote_port)
	    msg (M_USAGE, "Options error: --remote and --local addresses are the same");
	
	  if (string_defined_equal (remote, options->ifconfig_local)
	      || string_defined_equal (remote, options->ifconfig_remote_netmask))
	    msg (M_USAGE, "Options error: --local and --remote addresses must be distinct from --ifconfig addresses");
	}
    }

  if (string_defined_equal (options->local, options->ifconfig_local)
      || string_defined_equal (options->local, options->ifconfig_remote_netmask))
    msg (M_USAGE, "Options error: --local addresses must be distinct from --ifconfig addresses");

  if (string_defined_equal (options->ifconfig_local, options->ifconfig_remote_netmask))
    msg (M_USAGE, "Options error: local and remote/netmask --ifconfig addresses must be different");

  if (options->local_port_defined && !options->bind_local)
    msg (M_USAGE, "Options error: --lport and --nobind don't make sense when used together");

  /*
   * Windows-specific options.
   */

#ifdef WIN32
      if (dev == DEV_TYPE_TUN && !(pull || (options->ifconfig_local && options->ifconfig_remote_netmask)))
	msg (M_USAGE, "Options error: On Windows, --ifconfig is required when --dev tun is used");

      if ((options->tuntap_options.ip_win32_defined)
	  && !(pull || (options->ifconfig_local && options->ifconfig_remote_netmask)))
	msg (M_USAGE, "Options error: On Windows, --ip-win32 doesn't make sense unless --ifconfig is also used");

      if (options->tuntap_options.dhcp_options &&
	  options->tuntap_options.ip_win32_type != IPW32_SET_DHCP_MASQ)
	msg (M_USAGE, "Options error: --dhcp-options requires --ip-win32 dynamic");

      if ((dev == DEV_TYPE_TUN || dev == DEV_TYPE_TAP) && !options->route_delay_defined)
	{
	  options->route_delay_defined = true;
	  options->route_delay = 0;
	}

      if (options->ifconfig_noexec)
	{
	  options->tuntap_options.ip_win32_type = IPW32_SET_MANUAL;
	  options->ifconfig_noexec = false;
	}
#endif

  /*
   * Check that protocol options make sense.
   */

  if (options->proto != PROTO_UDPv4 && options->fragment)
    msg (M_USAGE, "Options error: --fragment can only be used with --proto udp");

  if (options->proto != PROTO_UDPv4 && options->explicit_exit_notification)
    msg (M_USAGE, "Options error: --explicit-exit-notify can only be used with --proto udp");

  if (!options->remote_list && options->proto == PROTO_TCPv4_CLIENT)
    msg (M_USAGE, "Options error: --remote MUST be used in TCP Client mode");

  if (options->http_proxy_server && options->proto != PROTO_TCPv4_CLIENT)
    msg (M_USAGE, "Options error: --http-proxy MUST be used in TCP Client mode (i.e. --proto tcp-client)");

  if (options->http_proxy_server && options->socks_proxy_server)
    msg (M_USAGE, "Options error: --http-proxy can not be used together with --socks-proxy");

  if (options->socks_proxy_server && options->proto == PROTO_TCPv4_SERVER)
    msg (M_USAGE, "Options error: --socks-proxy can not be used in TCP Server mode");

  if (options->proto == PROTO_TCPv4_SERVER && remote_list_len (options->remote_list) > 1)
    msg (M_USAGE, "Options error: TCP server mode allows at most one --remote address");

#if P2MP

  /*
   * Check consistency of --mode server options.
   */
  if (options->mode == MODE_SERVER)
    {
      if (!(dev == DEV_TYPE_TUN || dev == DEV_TYPE_TAP))
	msg (M_USAGE, "Options error: --mode server only works with --dev tun or --dev tap");
      if (options->pull)
	msg (M_USAGE, "Options error: --pull cannot be used with --mode server");
      if (!(options->proto == PROTO_UDPv4 || options->proto == PROTO_TCPv4_SERVER))
	msg (M_USAGE, "Options error: --mode server currently only supports --proto udp or --proto tcp-server");
      if (!options->tls_server)
	msg (M_USAGE, "Options error: --mode server requires --tls-server");
      if (options->remote_list)
	msg (M_USAGE, "Options error: --remote cannot be used with --mode server");
      if (!options->bind_local)
	msg (M_USAGE, "Options error: --nobind cannot be used with --mode server");
      if (options->http_proxy_server || options->socks_proxy_server)
	msg (M_USAGE, "Options error: --http-proxy or --socks-proxy cannot be used with --mode server");
      if (options->tun_ipv6)
	msg (M_USAGE, "Options error: --tun-ipv6 cannot be used with --mode server");
      if (options->shaper)
	msg (M_USAGE, "Options error: --shaper cannot be used with --mode server");
      if (options->inetd)
	msg (M_USAGE, "Options error: --inetd cannot be used with --mode server");
      if (options->ipchange)
	msg (M_USAGE, "Options error: --ipchange cannot be used with --mode server (use --client-connect instead)");
      if (!(options->proto == PROTO_UDPv4 || options->proto == PROTO_TCPv4_SERVER))
	msg (M_USAGE, "Options error: --mode server currently only supports --proto udp or --proto tcp-server");
      if (options->proto != PROTO_UDPv4 && (options->cf_max || options->cf_per))
	msg (M_USAGE, "Options error: --connect-freq only works with --mode server --proto udp.  Try --max-clients instead.");
      if (dev != DEV_TYPE_TAP && options->ifconfig_pool_netmask)
	msg (M_USAGE, "Options error: The third parameter to --ifconfig-pool (netmask) is only valid in --dev tap mode");
      if (options->explicit_exit_notification)
	msg (M_USAGE, "Options error: --explicit-exit-notify cannot be used with --mode server");
      if (options->routes && options->routes->redirect_default_gateway)
	msg (M_USAGE, "Options error: --redirect-gateway cannot be used with --mode server (however --push \"redirect-gateway\" is fine)");
      if (options->up_delay)
	msg (M_USAGE, "Options error: --up-delay cannot be used with --mode server");
      if (!options->ifconfig_pool_defined && options->ifconfig_pool_persist_filename)
	msg (M_USAGE, "Options error: --ifconfig-pool-persist must be used with --ifconfig-pool");
      if (options->client_cert_not_required && !options->auth_user_pass_verify_script)
	msg (M_USAGE, "Options error: --client-cert-not-required must be used with an --auth-user-pass-verify script");
      if (options->username_as_common_name && !options->auth_user_pass_verify_script)
	msg (M_USAGE, "Options error: --username-as-common-name must be used with an --auth-user-pass-verify script");
      if (options->auth_user_pass_file)
	msg (M_USAGE, "Options error: --auth-user-pass cannot be used with --mode server (it should be used on the client side only)");
      if (options->ccd_exclusive && !options->client_config_dir)
	msg (M_USAGE, "Options error: --ccd-exclusive must be used with --client-config-dir");

#ifdef WIN32
      /*
       * We need to explicitly set --tap-sleep because
       * we do not schedule event timers in the top-level context.
       */
      options->tuntap_options.tap_sleep = 10;
      if (options->route_delay_defined && options->route_delay)
	options->tuntap_options.tap_sleep = options->route_delay;	
      options->route_delay_defined = false;
#endif

    }
  else
    {
      /*
       * When not in server mode, err if parameters are
       * specified which require --mode server.
       */
      if (options->ifconfig_pool_defined || options->ifconfig_pool_persist_filename)
	msg (M_USAGE, "Options error: --ifconfig-pool/--ifconfig-pool-persist requires --mode server");
      if (options->real_hash_size != defaults.real_hash_size
	  || options->virtual_hash_size != defaults.virtual_hash_size)
	msg (M_USAGE, "Options error: --hash-size requires --mode server");
      if (options->learn_address_script)
	msg (M_USAGE, "Options error: --learn-address requires --mode server");
      if (options->client_connect_script)
	msg (M_USAGE, "Options error: --client-connect requires --mode server");
      if (options->client_disconnect_script)
	msg (M_USAGE, "Options error: --client-disconnect requires --mode server");
      if (options->tmp_dir)
	msg (M_USAGE, "Options error: --tmp-dir requires --mode server");
      if (options->client_config_dir || options->ccd_exclusive)
	msg (M_USAGE, "Options error: --client-config-dir/--ccd-exclusive requires --mode server");
      if (options->enable_c2c)
	msg (M_USAGE, "Options error: --client-to-client requires --mode server");
      if (options->duplicate_cn)
	msg (M_USAGE, "Options error: --duplicate-cn requires --mode server");
      if (options->cf_max || options->cf_per)
	msg (M_USAGE, "Options error: --connect-freq requires --mode server");
      if (options->client_cert_not_required)
	msg (M_USAGE, "Options error: --client-cert-not-required requires --mode server");
      if (options->username_as_common_name)
	msg (M_USAGE, "Options error: --username-as-common-name requires --mode server");
      if (options->auth_user_pass_verify_script)
	msg (M_USAGE, "Options error: --auth-user-pass-verify requires --mode server");
      if (options->ifconfig_pool_linear)
	msg (M_USAGE, "Options error: --ifconfig-pool-linear requires --mode server");
    }
#endif

#ifdef USE_CRYPTO

  /*
   * Check consistency of replay options
   */
  if ((options->proto != PROTO_UDPv4)
      && (options->replay_window != defaults.replay_window
	  || options->replay_time != defaults.replay_time))
    msg (M_USAGE, "Options error: --replay-window only makes sense with --proto udp");

  if (!options->replay
      && (options->replay_window != defaults.replay_window
	  || options->replay_time != defaults.replay_time))
    msg (M_USAGE, "Options error: --replay-window doesn't make sense when replay protection is disabled with --no-replay");

  /* 
   * Don't use replay window for TCP mode (i.e. require that packets
   * be strictly in sequence).
   */
  if (link_socket_proto_connection_oriented (options->proto))
    options->replay_window = options->replay_time = 0;

  /*
   * SSL/TLS mode sanity checks.
   */

#ifdef USE_SSL
  if (options->tls_server + options->tls_client +
      (options->shared_secret_file != NULL) > 1)
    msg (M_USAGE, "Options error: specify only one of --tls-server, --tls-client, or --secret");

  if (options->tls_server)
    {
      notnull (options->dh_file, "DH file (--dh)");
    }
  if (options->tls_server || options->tls_client)
    {
      if (options->pkcs12_file)
        {
          if (options->ca_file)
	    msg(M_USAGE, "Options error: Parameter --ca can not be used when --pkcs12 is also specified.");
          if (options->cert_file)
	    msg(M_USAGE, "Options error: Parameter --cert can not be used when --pkcs12 is also specified.");
          if (options->priv_key_file)
	    msg(M_USAGE, "Options error: Parameter --key can not be used when --pkcs12 is also specified.");
        }
      else
        {
          notnull (options->ca_file, "CA file (--ca) or PKCS#12 file (--pkcs12)");
	  if (pull)
	    {
	      const int sum = (options->cert_file != NULL) + (options->priv_key_file != NULL);
	      if (sum == 0)
		{
#if P2MP
		  if (!options->auth_user_pass_file)
#endif
		    msg (M_USAGE, "Options error: No client-side authentication method is specified.  You must use either --cert/--key, --pkcs12, or --auth-user-pass");
		}
	      else if (sum == 2)
		;
	      else
		{
		  msg (M_USAGE, "Options Error: If you use one of --cert or --key, you must use them both");
		}
	    }
	  else
	    {
	      notnull (options->cert_file, "certificate file (--cert) or PKCS#12 file (--pkcs12)");
	      notnull (options->priv_key_file, "private key file (--key) or PKCS#12 file (--pkcs12)");
	    }
	}
    }
  else
    {
      /*
       * Make sure user doesn't specify any TLS options
       * when in non-TLS mode.
       */

#define MUST_BE_UNDEF(parm) if (options->parm != defaults.parm) msg (M_USAGE, err, #parm);

      const char err[] = "Options error: Parameter %s can only be specified in TLS-mode, i.e. where --tls-server or --tls-client is also specified.";

      MUST_BE_UNDEF (ca_file);
      MUST_BE_UNDEF (dh_file);
      MUST_BE_UNDEF (cert_file);
      MUST_BE_UNDEF (priv_key_file);
      MUST_BE_UNDEF (pkcs12_file);
      MUST_BE_UNDEF (cipher_list);
      MUST_BE_UNDEF (tls_verify);
      MUST_BE_UNDEF (tls_remote);
      MUST_BE_UNDEF (tls_timeout);
      MUST_BE_UNDEF (renegotiate_bytes);
      MUST_BE_UNDEF (renegotiate_packets);
      MUST_BE_UNDEF (renegotiate_seconds);
      MUST_BE_UNDEF (handshake_window);
      MUST_BE_UNDEF (transition_window);
      MUST_BE_UNDEF (tls_auth_file);
      MUST_BE_UNDEF (single_session);
      MUST_BE_UNDEF (tls_exit);
      MUST_BE_UNDEF (crl_file);
      MUST_BE_UNDEF (key_method);
    }
#undef MUST_BE_UNDEF
#endif /* USE_CRYPTO */
#endif /* USE_SSL */

#if P2MP
  /*
   * In pull mode, we usually import --ping/--ping-restart parameters from
   * the server.  However we should also set an initial default --ping-restart
   * for the period of time before we pull the --ping-restart parameter
   * from the server.
   */
  if (options->pull
      && options->ping_rec_timeout_action == PING_UNDEF
      && options->proto == PROTO_UDPv4)
    {
      options->ping_rec_timeout = PRE_PULL_INITIAL_PING_RESTART;
      options->ping_rec_timeout_action = PING_RESTART;
    }

  /*
   * Save certain parms before modifying options via --pull
   */
  pre_pull_save (options);
#endif
}

#if P2MP

/*
 * Save/Restore certain option defaults before --pull is applied.
 */

void
pre_pull_save (struct options *o)
{
  if (o->pull)
    {
      ALLOC_OBJ_CLEAR_GC (o->pre_pull, struct options_pre_pull, &o->gc);
      o->pre_pull->tuntap_options = o->tuntap_options;
      o->pre_pull->tuntap_options_defined = true;
      o->pre_pull->foreign_option_index = o->foreign_option_index;
      if (o->routes)
	{
	  o->pre_pull->routes = *o->routes;
	  o->pre_pull->routes_defined = true;
	}
    }
}

void
pre_pull_restore (struct options *o)
{
  const struct options_pre_pull *pp = o->pre_pull;
  if (pp)
    {
      CLEAR (o->tuntap_options);
      if (pp->tuntap_options_defined)
	  o->tuntap_options = pp->tuntap_options;

      if (pp->routes_defined)
	{
	  rol_check_alloc (o);
	  *o->routes = pp->routes;
	}
      else
	o->routes = NULL;

      o->foreign_option_index = pp->foreign_option_index;
    }
}

#endif

/*
 * Build an options string to represent data channel encryption options.
 * This string must match exactly between peers.  The keysize is checked
 * separately by read_key().
 *
 * The following options must match on both peers:
 *
 * Tunnel options:
 *
 * --dev tun|tap [unit number need not match]
 * --dev-type tun|tap
 * --link-mtu
 * --udp-mtu
 * --tun-mtu
 * --proto udp
 * --proto tcp-client [matched with --proto tcp-server
 *                     on the other end of the connection]
 * --proto tcp-server [matched with --proto tcp-client on
 *                     the other end of the connection]
 * --tun-ipv6
 * --ifconfig x y [matched with --ifconfig y x on
 *                 the other end of the connection]
 *
 * --comp-lzo
 * --fragment
 *
 * Crypto Options:
 *
 * --cipher
 * --auth
 * --keysize
 * --secret
 * --no-replay
 * --no-iv
 *
 * SSL Options:
 *
 * --tls-auth
 * --tls-client [matched with --tls-server on
 *               the other end of the connection]
 * --tls-server [matched with --tls-client on
 *               the other end of the connection]
 */

char *
options_string (const struct options *o,
		const struct frame *frame,
		struct tuntap *tt,
		bool remote,
		struct gc_arena *gc)
{
  struct buffer out = alloc_buf (256);
  bool tt_local = false;

  buf_printf (&out, "V4");

  /*
   * Tunnel Options
   */

  buf_printf (&out, ",dev-type %s", dev_type_string (o->dev, o->dev_type));
  buf_printf (&out, ",link-mtu %d", EXPANDED_SIZE (frame));
  buf_printf (&out, ",tun-mtu %d", PAYLOAD_SIZE (frame));
  buf_printf (&out, ",proto %s", proto2ascii (proto_remote (o->proto, remote), true));
  if (o->tun_ipv6)
    buf_printf (&out, ",tun-ipv6");

  /*
   * Try to get ifconfig parameters into the options string.
   * If tt is undefined, make a temporary instantiation.
   */
  if (!tt)
    {
      tt = init_tun (o->dev,
		     o->dev_type,
		     o->ifconfig_local,
		     o->ifconfig_remote_netmask,
		     (in_addr_t)0,
		     (in_addr_t)0,
		     false,
		     NULL);
      if (tt)
	tt_local = true;
    }

  if (tt && o->mode == MODE_POINT_TO_POINT && !PULL_DEFINED(o))
    {
      const char *ios = ifconfig_options_string (tt, remote, o->ifconfig_nowarn, gc);
      if (ios && strlen (ios))
	buf_printf (&out, ",ifconfig %s", ios);
    }
  if (tt_local)
    {
      free (tt);
      tt = NULL;
    }

#ifdef USE_LZO
  if (o->comp_lzo)
    buf_printf (&out, ",comp-lzo");
#endif

  if (o->fragment)
    buf_printf (&out, ",mtu-dynamic");

#ifdef USE_CRYPTO

#ifdef USE_SSL
#define TLS_CLIENT (o->tls_client)
#define TLS_SERVER (o->tls_server)
#else
#define TLS_CLIENT (false)
#define TLS_SERVER (false)
#endif

  /*
   * Key direction
   */
  {
    const char *kd = keydirection2ascii (o->key_direction, remote);
    if (kd)
      buf_printf (&out, ",keydir %s", kd);
  }

  /*
   * Crypto Options
   */
    if (o->shared_secret_file || TLS_CLIENT || TLS_SERVER)
      {
	struct key_type kt;

	ASSERT ((o->shared_secret_file != NULL)
		+ (TLS_CLIENT == true)
		+ (TLS_SERVER == true)
		<= 1);

	init_key_type (&kt, o->ciphername, o->ciphername_defined,
		       o->authname, o->authname_defined,
		       o->keysize, true, false);

	buf_printf (&out, ",cipher %s", kt_cipher_name (&kt));
	buf_printf (&out, ",auth %s", kt_digest_name (&kt));
	buf_printf (&out, ",keysize %d", kt_key_size (&kt));
	if (o->shared_secret_file)
	  buf_printf (&out, ",secret");
	if (!o->replay)
	  buf_printf (&out, ",no-replay");
	if (!o->use_iv)
	  buf_printf (&out, ",no-iv");
      }

#ifdef USE_SSL
  /*
   * SSL Options
   */
  {
    if (TLS_CLIENT || TLS_SERVER)
      {
	if (o->tls_auth_file)
	  buf_printf (&out, ",tls-auth");

	if (o->key_method > 1)
	  buf_printf (&out, ",key-method %d", o->key_method);
      }

    if (remote)
      {
	if (TLS_CLIENT)
	  buf_printf (&out, ",tls-server");
	else if (TLS_SERVER)
	  buf_printf (&out, ",tls-client");
      }
    else
      {
	if (TLS_CLIENT)
	  buf_printf (&out, ",tls-client");
	else if (TLS_SERVER)
	  buf_printf (&out, ",tls-server");
      }
  }
#endif /* USE_SSL */

#undef TLS_CLIENT
#undef TLS_SERVER

#endif /* USE_CRYPTO */

  return BSTR (&out);
}

/*
 * Compare option strings for equality.
 * If the first two chars of the strings differ, it means that
 * we are looking at different versions of the options string,
 * therefore don't compare them and return true.
 */

bool
options_cmp_equal (char *actual, const char *expected)
{
  return options_cmp_equal_safe (actual, expected, strlen (actual) + 1);
}

void
options_warning (char *actual, const char *expected)
{
  return options_warning_safe (actual, expected, strlen (actual) + 1);
}

bool
options_cmp_equal_safe (char *actual, const char *expected, size_t actual_n)
{
  struct gc_arena gc = gc_new ();
  bool ret = true;

  if (actual_n > 0)
    {
      actual[actual_n - 1] = 0;
#ifndef STRICT_OPTIONS_CHECK
      if (strncmp (actual, expected, 2))
	{
	  msg (D_SHOW_OCC, "NOTE: failed to perform options consistency check between peers because of " PACKAGE_NAME " version differences -- you can disable the options consistency check with --disable-occ (Required for TLS connections between " PACKAGE_NAME " 1.3.x and later versions).  Actual Remote Options: '%s'.  Expected Remote Options: '%s'",
	       safe_print (actual, &gc),
	       safe_print (expected, &gc));
	}
      else
#endif
	ret = !strcmp (actual, expected);
    }

  gc_free (&gc);
  return ret;
}

void
options_warning_safe (char *actual, const char *expected, size_t actual_n)
{
  struct gc_arena gc = gc_new ();
  if (actual_n > 0)
    {
      actual[actual_n - 1] = 0;
      msg (M_WARN,
	   "WARNING: Actual Remote Options ('%s') are inconsistent with Expected Remote Options ('%s')",
	   safe_print (actual, &gc),
	   safe_print (expected, &gc));
    }
  gc_free (&gc);
}

const char *
options_string_version (const char* s, struct gc_arena *gc)
{
  struct buffer out = alloc_buf_gc (4, gc);
  strncpynt (BPTR (&out), s, 3);
  return BSTR (&out);
}

static void
foreign_option (struct options *o, char *argv[], int len, struct env_set *es)
{
  if (len > 0)
    {
      struct gc_arena gc = gc_new();
      struct buffer name = alloc_buf_gc (64, &gc);
      struct buffer value = alloc_buf_gc (256, &gc);
      int i;
      bool first = true;

      buf_printf (&name, "foreign_option_%d", o->foreign_option_index + 1);
      ++o->foreign_option_index;
      for (i = 0; i < len; ++i)
	{
	  if (argv[i])
	    {
	      if (!first)
		buf_printf (&value, " ");
	      buf_printf (&value, argv[i]);
	      first = false;
	    }
	}
      setenv_str (es, BSTR(&name), BSTR(&value));
      gc_free (&gc);
    }
}

static void
usage (void)
{
  struct options o;
  FILE *fp = msg_fp();

  init_options (&o);

#if defined(USE_CRYPTO) && defined(USE_SSL)
  fprintf (fp, usage_message,
	   title_string,
	   o.connect_retry_seconds,
	   o.local_port, o.remote_port,
	   TUN_MTU_DEFAULT, TAP_MTU_EXTRA_DEFAULT,
	   o.verbosity,
	   o.authname, o.ciphername,
           o.replay_window, o.replay_time,
	   o.tls_timeout, o.renegotiate_seconds,
	   o.handshake_window, o.transition_window);
#elif defined(USE_CRYPTO)
  fprintf (fp, usage_message,
	   title_string,
	   o.connect_retry_seconds,
	   o.local_port, o.remote_port,
	   TUN_MTU_DEFAULT, TAP_MTU_EXTRA_DEFAULT,
	   o.verbosity,
	   o.authname, o.ciphername,
           o.replay_window, o.replay_time);
#else
  fprintf (fp, usage_message,
	   title_string,
	   o.connect_retry_seconds,
	   o.local_port, o.remote_port,
	   TUN_MTU_DEFAULT, TAP_MTU_EXTRA_DEFAULT,
	   o.verbosity);
#endif
  fflush(fp);
  
  openvpn_exit (OPENVPN_EXIT_STATUS_USAGE); /* exit point */
}

void
usage_small (void)
{
  msg (M_WARN|M_NOPREFIX, "Use --help for more information.");
  openvpn_exit (OPENVPN_EXIT_STATUS_USAGE); /* exit point */
}

static void
usage_version (void)
{
  msg (M_INFO|M_NOPREFIX, "%s", title_string);
  msg (M_INFO|M_NOPREFIX, "Copyright (C) 2002-2004 James Yonan <jim@yonan.net>");
  openvpn_exit (OPENVPN_EXIT_STATUS_USAGE); /* exit point */
}

void
notnull (const char *arg, const char *description)
{
  if (!arg)
    msg (M_USAGE, "Options error: You must define %s", description);
}

bool
string_defined_equal (const char *s1, const char *s2)
{
  if (s1 && s2)
    return !strcmp (s1, s2);
  else
    return false;
}

#if 0
static void
ping_rec_err (int msglevel)
{
  msg (msglevel, "Options error: only one of --ping-exit or --ping-restart options may be specified");
}
#endif

static inline int
positive (int i)
{
  return i < 0 ? 0 : i;
}

static inline bool
space (char c)
{
  return c == '\0' || isspace (c);
}

int
parse_line (char *line, char *p[], int n, const char *file, int line_num, int msglevel, struct gc_arena *gc)
{
  const int STATE_INITIAL = 0;
  const int STATE_READING_QUOTED_PARM = 1;
  const int STATE_READING_UNQUOTED_PARM = 2;
  const int STATE_DONE = 3;

  int ret = 0;
  char *c = line;
  int state = STATE_INITIAL;
  bool backslash = false;
  char in, out;

  char parm[256];
  unsigned int parm_len = 0;

  do
    {
      in = *c;
      out = 0;

      if (!backslash && in == '\\')
	{
	  backslash = true;
	}
      else
	{
	  if (state == STATE_INITIAL)
	    {
	      if (!space (in))
		{
		  if (in == ';' || in == '#') /* comment */
		    break;
		  if (!backslash && in == '\"')
		    state = STATE_READING_QUOTED_PARM;
		  else
		    {
		      out = in;
		      state = STATE_READING_UNQUOTED_PARM;
		    }
		}
	    }
	  else if (state == STATE_READING_UNQUOTED_PARM)
	    {
	      if (!backslash && space (in))
		state = STATE_DONE;
	      else
		out = in;
	    }
	  else if (state == STATE_READING_QUOTED_PARM)
	    {
	      if (!backslash && in == '\"')
		state = STATE_DONE;
	      else
		out = in;
	    }
	  if (state == STATE_DONE)
	    {
	      ASSERT (parm_len > 0);
	      p[ret] = gc_malloc (parm_len + 1, true, gc);
	      memcpy (p[ret], parm, parm_len);
	      p[ret][parm_len] = '\0';
	      state = STATE_INITIAL;
	      parm_len = 0;
	      ++ret;
	    }

	  if (backslash && out)
	    {
	      if (!(out == '\\' || out == '\"' || space (out)))
		msg (msglevel, "Bad backslash ('\\') usage in %s:%d: remember that backslashes are treated as shell-escapes and if you need to pass backslash characters as part of a Windows filename, you should use double backslashes such as \"c:\\\\openvpn\\\\static.key\"", file, line_num);
	    }
	  backslash = false;
	}

      /* store parameter character */
      if (out)
	{
	  if (parm_len >= SIZE (parm))
	    {
	      parm[SIZE (parm) - 1] = 0;
	      msg (msglevel, "Parameter at %s:%d is too long (%d chars max): %s",
		   file, line_num, (int) SIZE (parm), parm);
	      return 0;
	    }
	  parm[parm_len++] = out;
	}

      /* avoid overflow if too many parms in one config file line */
      if (ret >= n)
	break;

    } while (*c++ != '\0');

  if (state == STATE_READING_QUOTED_PARM)
    {
      msg (msglevel, "No closing quotation (\") in %s:%d", file, line_num);
      return 0;
    }
  if (state != STATE_INITIAL)
    {
      msg (msglevel, "Residual parse state (%d) in %s:%d", state, file, line_num);
      return 0;
    }
#if 0
  {
    int i;
    for (i = 0; i < ret; ++i)
      {
	msg (M_INFO|M_NOPREFIX, "%s:%d ARG[%d] '%s'", file, line_num, i, p[i]);
      }
  }
#endif
    return ret;
}

static int
add_option (struct options *options,
	    int i,
	    char *p[],
	    const char* file,
	    int line,
	    int level,
	    int msglevel,
	    unsigned int permission_mask,
	    unsigned int *option_types_found,
	    struct env_set *es);

static void
read_config_file (struct options *options,
		  const char* file,
		  int level,
		  const char* top_file,
		  int top_line,
		  int msglevel,
		  unsigned int permission_mask,
		  unsigned int *option_types_found,
		  struct env_set *es)
{
  const int max_recursive_levels = 10;
  FILE *fp;
  int line_num;
  char line[256];

  ++level;
  if (level > max_recursive_levels)
    msg (M_FATAL, "In %s:%d: Maximum recursive include levels exceeded in include attempt of file %s -- probably you have a configuration file that tries to include itself.", top_file, top_line, file);

  fp = fopen (file, "r");
  if (!fp)
    msg (M_ERR, "In %s:%d: Error opening configuration file: %s", top_file, top_line, file);

  line_num = 0;
  while (fgets(line, sizeof (line), fp))
    {
      char *p[MAX_PARMS];
      CLEAR (p);
      ++line_num;
      if (parse_line (line, p, SIZE (p), file, line_num, msglevel, &options->gc))
	{
	  if (strlen (p[0]) >= 3 && !strncmp (p[0], "--", 2))
	    p[0] += 2;
	  add_option (options, 0, p, file, line_num, level, msglevel, permission_mask, option_types_found, es);
	}
    }
  fclose (fp);
}

void
parse_argv (struct options* options,
	    int argc,
	    char *argv[],
	    int msglevel,
	    unsigned int permission_mask,
	    unsigned int *option_types_found,
	    struct env_set *es)
{
  int i, j;

  /* usage message */
  if (argc <= 1)
    usage ();

  /* config filename specified only? */
  if (argc == 2 && strncmp (argv[1], "--", 2))
    {
      char *p[MAX_PARMS];
      CLEAR (p);
      p[0] = "config";
      p[1] = argv[1];
      add_option (options, 0, p, NULL, 0, 0, msglevel, permission_mask, option_types_found, es);
    }
  else
    {
      /* parse command line */
      for (i = 1; i < argc; ++i)
	{
	  char *p[MAX_PARMS];
	  CLEAR (p);
	  p[0] = argv[i];
	  if (strncmp(p[0], "--", 2))
	    {
	      msg (msglevel, "I'm trying to parse \"%s\" as an --option parameter but I don't see a leading '--'", p[0]);
	    }
	  else
	    p[0] += 2;

	  for (j = 1; j < MAX_PARMS; ++j)
	    {
	      if (i + j < argc)
		{
		  char *arg = argv[i + j];
		  if (strncmp (arg, "--", 2))
		    p[j] = arg;
		  else
		    break;
		}
	    }
	  i = add_option (options, i, p, NULL, 0, 0, msglevel, permission_mask, option_types_found, es);
	}
    }
}

bool
apply_push_options (struct options *options,
		    struct buffer *buf,
		    unsigned int permission_mask,
		    unsigned int *option_types_found,
		    struct env_set *es)
{
  char line[256];
  int line_num = 0;
  const char *file = "[PUSH-OPTIONS]";
  const int msglevel = D_PUSH_ERRORS;

  while (buf_parse (buf, ',', line, sizeof (line)))
    {
      char *p[MAX_PARMS];
      CLEAR (p);
      ++line_num;
      if (parse_line (line, p, SIZE (p), file, line_num, msglevel, &options->gc))
	{
	  add_option (options, 0, p, file, line_num, 0, msglevel, permission_mask, option_types_found, es);
	}
    }
  return true;
}

void
options_server_import (struct options *o,
		       const char *filename,
		       int msglevel,
		       unsigned int permission_mask,
		       unsigned int *option_types_found,
		       struct env_set *es)
{
  msg (D_PUSH, "OPTIONS IMPORT: reading client specific options from %s", filename);
  read_config_file (o,
		    filename,
		    0,
		    filename,
		    0,
		    msglevel,
		    permission_mask,
		    option_types_found,
		    es);
}

#define VERIFY_PERMISSION(mask) { if (!verify_permission(p[0], (mask), permission_mask, option_types_found, msglevel)) goto err; }

static bool
verify_permission (const char *name,
		   unsigned int type,
		   unsigned int allowed,
		   unsigned int *found,
		   int msglevel)
{
  if (!(type & allowed))
    {
      msg (msglevel, "Options error: option '%s' cannot be used in this context", name);
      return false;
    }
  else
    {
      if (found)
	*found |= type;
      return true;
    }
}

static int
add_option (struct options *options,
	    int i,
	    char *p[],
	    const char* file,
	    int line,
	    int level,
	    int msglevel,
	    unsigned int permission_mask,
	    unsigned int *option_types_found,
	    struct env_set *es)
{
  struct gc_arena gc = gc_new ();
  ASSERT (MAX_PARMS >= 5);

  if (!file)
    {
      file = "[CMD-LINE]";
      line = 1;
    }
  if (streq (p[0], "help"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      usage ();
    }
  if (streq (p[0], "version"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      usage_version ();
    }
  else if (streq (p[0], "config") && p[1])
    {
      ++i;

      VERIFY_PERMISSION (OPT_P_CONFIG);

      /* save first config file only in options */
      if (!options->config)
	options->config = p[1];

      read_config_file (options, p[1], level, file, line, msglevel, permission_mask, option_types_found, es);
    }
  else if (streq (p[0], "mode") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (streq (p[1], "p2p"))
	options->mode = MODE_POINT_TO_POINT;
#if P2MP
      else if (streq (p[1], "server"))
	options->mode = MODE_SERVER;
#endif
      else
	{
	  msg (msglevel, "Options error: Bad --mode parameter: %s", p[1]);
	  goto err;
	}
    }
  else if (streq (p[0], "dev") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->dev = p[1];
    }
  else if (streq (p[0], "dev-type") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->dev_type = p[1];
    }
  else if (streq (p[0], "dev-node") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->dev_node = p[1];
    }
  else if (streq (p[0], "tun-ipv6"))
    {
      VERIFY_PERMISSION (OPT_P_UP);
      options->tun_ipv6 = true;
    }
  else if (streq (p[0], "ifconfig") && p[1] && p[2])
    {
      i += 2;
      VERIFY_PERMISSION (OPT_P_UP);
      options->ifconfig_local = p[1];
      options->ifconfig_remote_netmask = p[2];
    }
  else if (streq (p[0], "ifconfig-noexec"))
    {
      VERIFY_PERMISSION (OPT_P_UP);
      options->ifconfig_noexec = true;
    }
  else if (streq (p[0], "ifconfig-nowarn"))
    {
      VERIFY_PERMISSION (OPT_P_UP);
      options->ifconfig_nowarn = true;
    }
  else if (streq (p[0], "local") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->local = p[1];
    }
  else if (streq (p[0], "remote-random"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->remote_random = true;
    }
  else if (streq (p[0], "remote") && p[1])
    {
      struct remote_list *l;
      struct remote_entry e;
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (!options->remote_list)
	ALLOC_OBJ_CLEAR_GC (options->remote_list, struct remote_list, &options->gc);
      l = options->remote_list;
      if (l->len >= REMOTE_LIST_SIZE)
	msg (msglevel, "Options error: Maximum number of --remote options (%d) exceeded", REMOTE_LIST_SIZE);
      e.hostname = p[1];
      if (p[2])
	{
	  ++i;
	  e.port = atoi (p[2]);
	  if (e.port < 1 || e.port > 65535)
	    msg (msglevel, "Options error: port number associated with host %s is out of range", e.hostname);
	}
      else
	e.port = -1;
      l->array[l->len++] = e;
    }
  else if (streq (p[0], "resolv-retry") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (streq (p[1], "infinite"))
	options->resolve_retry_seconds = RESOLV_RETRY_INFINITE;
      else
	options->resolve_retry_seconds = positive (atoi (p[1]));
    }
  else if (streq (p[0], "connect-retry") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->connect_retry_seconds = positive (atoi (p[1]));
      options->connect_retry_defined = true;
    }
  else if (streq (p[0], "ipchange") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->ipchange = string_substitute (p[1], ',', ' ', &options->gc);
    }
  else if (streq (p[0], "float"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->remote_float = true;
    }
  else if (streq (p[0], "gremlin"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->gremlin = true;
    }
  else if (streq (p[0], "user") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->username = p[1];
    }
  else if (streq (p[0], "group") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->groupname = p[1];
    }
  else if (streq (p[0], "chroot") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->chroot_dir = p[1];
    }
  else if (streq (p[0], "cd") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->cd_dir = p[1];
      if (openvpn_chdir (p[1]))
	{
	  msg (M_ERR, "Options error: cd to '%s' failed", p[1]);
	  goto err;
	}
    }
  else if (streq (p[0], "writepid") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->writepid = p[1];
    }
  else if (streq (p[0], "up") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->up_script = p[1];
    }
  else if (streq (p[0], "down") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->down_script = p[1];
    }
  else if (streq (p[0], "down-pre"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->down_pre = true;
    }
  else if (streq (p[0], "up-delay"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->up_delay = true;
    }
  else if (streq (p[0], "up-restart"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->up_restart = true;
    }
  else if (streq (p[0], "daemon"))
    {
      bool didit = false;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (!options->daemon)
	{
	  options->daemon = didit = true;
	  open_syslog (p[1], false);
	}
      if (p[1])
	{
	  ++i;
	  if (!didit)
	    msg (M_WARN, "WARNING: Multiple --daemon directives specified, ignoring --daemon %s. (Note that initscripts sometimes add their own --daemon directive.)", p[1]);
	}
    }
  else if (streq (p[0], "inetd"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (!options->inetd)
	{
	  int z;
	  const char *name = NULL;
	  const char *opterr = "Options error: when --inetd is used with two parameters, one of them must be 'wait' or 'nowait' and the other must be a daemon name to use for system logging";

	  options->inetd = -1;

	  for (z = 1; z <= 2; ++z)
	    {
	      if (p[z])
		{
		  ++i;
		  if (streq (p[z], "wait"))
		    {
		      if (options->inetd != -1)
			{
			  msg (msglevel, opterr);
			  goto err;
			}
		      else
			options->inetd = INETD_WAIT;
		    }
		  else if (streq (p[z], "nowait"))
		    {
		      if (options->inetd != -1)
			{
			  msg (msglevel, opterr);
			  goto err;
			}
		      else
			options->inetd = INETD_NOWAIT;
		    }
		  else
		    {
		      if (name != NULL)
			{
			  msg (msglevel, opterr);
			  goto err;
			}
		      name = p[z];
		    }
		}
	    }

	  /* default */
	  if (options->inetd == -1)
	    options->inetd = INETD_WAIT;

	  save_inetd_socket_descriptor ();
	  open_syslog (name, true);
	}
    }
  else if (streq (p[0], "log") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->log = true;
      redirect_stdout_stderr (p[1], false);
    }
  else if (streq (p[0], "suppress-timestamps"))
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->suppress_timestamps = true;
      set_suppress_timestamps(true);
    }
  else if (streq (p[0], "log-append") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->log = true;
      redirect_stdout_stderr (p[1], true);
    }
  else if (streq (p[0], "mlock"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->mlock = true;
    }
  else if (streq (p[0], "verb") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MESSAGES);
      options->verbosity = positive (atoi (p[1]));
    }
  else if (streq (p[0], "mute") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MESSAGES);
      options->mute = positive (atoi (p[1]));
    }
  else if (streq (p[0], "status") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->status_file = p[1];
      if (p[2])
	{
	  ++i;
	  options->status_file_update_freq = positive (atoi (p[2]));
	}
    }
  else if ((streq (p[0], "link-mtu") || streq (p[0], "udp-mtu")) && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MTU);
      options->link_mtu = positive (atoi (p[1]));
      options->link_mtu_defined = true;
    }
  else if (streq (p[0], "tun-mtu") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MTU);
      options->tun_mtu = positive (atoi (p[1]));
      options->tun_mtu_defined = true;
    }
  else if (streq (p[0], "tun-mtu-extra") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MTU);
      options->tun_mtu_extra = positive (atoi (p[1]));
      options->tun_mtu_extra_defined = true;
    }
  else if (streq (p[0], "mtu-dynamic"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      msg (msglevel, "Options error: --mtu-dynamic has been replaced by --fragment");
      goto err;
    }
  else if (streq (p[0], "fragment") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MTU);
      options->fragment = positive (atoi (p[1]));
    }
  else if (streq (p[0], "mtu-disc") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_MTU);
      options->mtu_discover_type = translate_mtu_discover_type_name (p[1]);
    }
  else if (streq (p[0], "mtu-test"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->mtu_test = true;
    }
  else if (streq (p[0], "nice") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_NICE);
      options->nice = atoi (p[1]);
    }
  else if (streq (p[0], "rcvbuf") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->rcvbuf = positive (atoi (p[1]));
    }
  else if (streq (p[0], "sndbuf") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->sndbuf = positive (atoi (p[1]));
    }
  else if (streq (p[0], "txqueuelen") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
#ifdef TARGET_LINUX
      options->tuntap_options.txqueuelen = positive (atoi (p[1]));
#else
      msg (msglevel, "Options error: --txqueuelen not supported on this OS");
#endif
    }
#ifdef USE_PTHREAD
  else if (streq (p[0], "nice-work") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_NICE);
      options->nice_work = atoi (p[1]);
    }
  else if (streq (p[0], "threads") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->n_threads = positive (atoi (p[1]));
      if (options->n_threads < 1)
	{
	  msg (msglevel, "Options error: --threads parameter must be at least 1");
	  goto err;
	}
    }
#endif
  else if (streq (p[0], "shaper") && p[1])
    {
#ifdef HAVE_GETTIMEOFDAY
      ++i;
      VERIFY_PERMISSION (OPT_P_SHAPER);
      options->shaper = atoi (p[1]);
      if (options->shaper < SHAPER_MIN || options->shaper > SHAPER_MAX)
	{
	  msg (msglevel, "Options error: Bad shaper value, must be between %d and %d",
	       SHAPER_MIN, SHAPER_MAX);
	  goto err;
	}
#else /* HAVE_GETTIMEOFDAY */
      VERIFY_PERMISSION (OPT_P_GENERAL);
      msg (msglevel, "Options error: --shaper requires the gettimeofday() function which is missing");
      goto err;
#endif /* HAVE_GETTIMEOFDAY */
    }
  else if (streq (p[0], "port") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->local_port = options->remote_port = atoi (p[1]);
      if (!legal_ipv4_port (options->local_port))
	{
	  msg (msglevel, "Options error: Bad port number: %s", p[1]);
	  goto err;
	}
    }
  else if (streq (p[0], "lport") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->local_port = atoi (p[1]);
      options->local_port_defined = true;
      if (!legal_ipv4_port (options->local_port))
	{
	  msg (msglevel, "Options error: Bad local port number: %s", p[1]);
	  goto err;
	}
    }
  else if (streq (p[0], "rport") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->remote_port = atoi (p[1]);
      if (!legal_ipv4_port (options->remote_port))
	{
	  msg (msglevel, "Options error: Bad remote port number: %s", p[1]);
	  goto err;
	}
    }
  else if (streq (p[0], "nobind"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->bind_local = false;
    }
  else if (streq (p[0], "fast-io"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->fast_io = true;
    }
  else if (streq (p[0], "inactive") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TIMER);
      options->inactivity_timeout = positive (atoi (p[1]));
    }
  else if (streq (p[0], "proto") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->proto = ascii2proto (p[1]);
      if (options->proto < 0)
	{
	  msg (msglevel, "Options error: Bad protocol: '%s'.  Allowed protocols with --proto option: %s",
	       p[1],
	       proto2ascii_all (&gc));
	  goto err;
	}
    }
  else if (streq (p[0], "http-proxy") && p[1] && p[2])
    {
      i += 2;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->http_proxy_server = p[1];
      options->http_proxy_port = atoi (p[2]);
      if (options->http_proxy_port <= 0)
	{
	  msg (msglevel, "Options error: Bad http-proxy port number: %s", p[2]);
	  goto err;
	}

      if (p[3])
	{
	  ++i;
	  options->http_proxy_auth_method = "basic";
	  options->http_proxy_auth_file = p[3];

	  if (p[4])
	    {
	      ++i;
	      options->http_proxy_auth_method = p[4];
	    }
	}
      else
	{
	  options->http_proxy_auth_method = "none";
	}
    }
  else if (streq (p[0], "http-proxy-retry"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->http_proxy_retry = true;
    }
  else if (streq (p[0], "socks-proxy") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->socks_proxy_server = p[1];

      if (p[2])
	{
	  ++i;
          options->socks_proxy_port = atoi (p[2]);
          if (options->socks_proxy_port <= 0)
	    {
	      msg (msglevel, "Options error: Bad socks-proxy port number: %s", p[2]);
	      goto err;
	    }
	}
      else
	{
	  options->socks_proxy_port = 1080;
	}
    }
  else if (streq (p[0], "socks-proxy-retry"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->socks_proxy_retry = true;
    }
  else if (streq (p[0], "keepalive") && p[1] && p[2])
    {
      i += 2;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->keepalive_ping = atoi (p[1]);
      options->keepalive_timeout = atoi (p[2]);
    }
  else if (streq (p[0], "ping") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TIMER);
      options->ping_send_timeout = positive (atoi (p[1]));
    }
  else if (streq (p[0], "ping-exit") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TIMER);
      options->ping_rec_timeout = positive (atoi (p[1]));
      options->ping_rec_timeout_action = PING_EXIT;
    }
  else if (streq (p[0], "ping-restart") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TIMER);
      options->ping_rec_timeout = positive (atoi (p[1]));
      options->ping_rec_timeout_action = PING_RESTART;
    }
  else if (streq (p[0], "ping-timer-rem"))
    {
      VERIFY_PERMISSION (OPT_P_TIMER);
      options->ping_timer_remote = true;
    }
  else if (streq (p[0], "explicit-exit-notify") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_EXPLICIT_NOTIFY);
      options->explicit_exit_notification = positive (atoi (p[1]));
    }
  else if (streq (p[0], "persist-tun"))
    {
      VERIFY_PERMISSION (OPT_P_PERSIST);
      options->persist_tun = true;
    }
  else if (streq (p[0], "persist-key"))
    {
      VERIFY_PERMISSION (OPT_P_PERSIST);
      options->persist_key = true;
    }
  else if (streq (p[0], "persist-local-ip"))
    {
      VERIFY_PERMISSION (OPT_P_PERSIST_IP);
      options->persist_local_ip = true;
    }
  else if (streq (p[0], "persist-remote-ip"))
    {
      VERIFY_PERMISSION (OPT_P_PERSIST_IP);
      options->persist_remote_ip = true;
    }
  else if (streq (p[0], "route") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_ROUTE);
      if (p[2])
	++i;
      if (p[3])
	++i;
      if (p[4])
	++i;
      rol_check_alloc (options);
      add_route_to_option_list (options->routes, p[1], p[2], p[3], p[4]);
    }
  else if (streq (p[0], "route-gateway") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_ROUTE);
      options->route_default_gateway = p[1];      
    }
  else if (streq (p[0], "route-delay"))
    {
      VERIFY_PERMISSION (OPT_P_ROUTE);
      options->route_delay_defined = true;
      if (p[1])
	{
	  ++i;
	  options->route_delay = positive (atoi (p[1]));
	  if (p[2])
	    {
	      ++i;
	      options->route_delay_window = positive (atoi (p[2]));
	    }
	}
      else
	{
	  options->route_delay = 0;
	}
    }
  else if (streq (p[0], "route-up") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->route_script = p[1];
    }
  else if (streq (p[0], "route-noexec"))
    {
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->route_noexec = true;
    }
  else if (streq (p[0], "redirect-gateway"))
    {
      int j;
      VERIFY_PERMISSION (OPT_P_ROUTE);
      rol_check_alloc (options);
      options->routes->redirect_default_gateway = true;
      for (j = 1; j < MAX_PARMS && p[j] != NULL; ++j)
	{
	  ++i;
	  if (streq (p[j], "local"))
	    options->routes->redirect_local = true;
	  else if (streq (p[j], "def1"))
	    options->routes->redirect_def1 = true;
	  else
	    msg (msglevel, "Options error: unknown --redirect-gateway flag: %s", p[j]);
	}
    }
  else if (streq (p[0], "setenv") && p[1] && p[2])
    {
      i += 2;
      VERIFY_PERMISSION (OPT_P_SETENV);
      setenv_str (es, p[1], p[2]);
    }
  else if (streq (p[0], "mssfix") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->mssfix = positive (atoi (p[1]));
    }
  else if (streq (p[0], "disable-occ"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->occ = false;
    }
#if P2MP
  else if (streq (p[0], "server") && p[1] && p[2])
    {
      const int lev = M_WARN;
      bool error = false;
      i += 2;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->server_network = get_ip_addr (p[1], lev, &error);
      options->server_netmask = get_ip_addr (p[2], lev, &error);
      if (error || !options->server_network || !options->server_netmask)
	{
	  msg (msglevel, "Options error: error parsing --server parameters");
	  goto err;
	}
      options->server_defined = true;
    }
  else if (streq (p[0], "server-bridge") && p[1] && p[2] && p[3] && p[4])
    {
      const int lev = M_WARN;
      bool error = false;
      i += 4;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->server_bridge_ip = get_ip_addr (p[1], lev, &error);
      options->server_bridge_netmask = get_ip_addr (p[2], lev, &error);
      options->server_bridge_pool_start = get_ip_addr (p[3], lev, &error);
      options->server_bridge_pool_end = get_ip_addr (p[4], lev, &error);
      if (error
	  || !options->server_bridge_ip
	  || !options->server_bridge_netmask
	  || !options->server_bridge_pool_start
	  || !options->server_bridge_pool_end)
	{
	  msg (msglevel, "Options error: error parsing --server-bridge parameters");
	  goto err;
	}
      options->server_bridge_defined = true;
    }
  else if (streq (p[0], "client"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->client = true;
    }
  else if (streq (p[0], "push") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_PUSH);
      push_option (options, p[1], msglevel);
    }
  else if (streq (p[0], "push-reset"))
    {
      VERIFY_PERMISSION (OPT_P_INSTANCE);
      push_reset (options);
    }
  else if (streq (p[0], "pull"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->pull = true;
    }
  else if (streq (p[0], "ifconfig-pool") && p[1] && p[2])
    {
      const int lev = M_WARN;
      bool error = false;
      i += 2;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->ifconfig_pool_defined = true;
      options->ifconfig_pool_start = get_ip_addr (p[1], lev, &error);
      options->ifconfig_pool_end = get_ip_addr (p[2], lev, &error);
      if (p[3])
	{
	  ++i;
	  options->ifconfig_pool_netmask = get_ip_addr (p[3], lev, &error);
	}
      if (error)
	{
	  msg (msglevel, "Options error: error parsing --ifconfig-pool parameters");
	  goto err;
	}
      if (options->ifconfig_pool_start > options->ifconfig_pool_end)
	{
	  msg (msglevel, "Options error: --ifconfig-pool start IP is greater than end IP");
	  goto err;
	}
      if (options->ifconfig_pool_end - options->ifconfig_pool_start >= IFCONFIG_POOL_MAX)
	{
	  msg (msglevel, "Options error: --ifconfig-pool address range is too large.  Current maximum is %d addresses.",
	       IFCONFIG_POOL_MAX);
	  goto err;
	}
    }
  else if (streq (p[0], "ifconfig-pool-persist") && p[1])
    {
      ++i;;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->ifconfig_pool_persist_filename = p[1];
      if (p[2])
	{
	  ++i;
	  options->ifconfig_pool_persist_refresh_freq = atoi (p[2]);
	}
    }
  else if (streq (p[0], "ifconfig-pool-linear"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->ifconfig_pool_linear = true;
    }
  else if (streq (p[0], "hash-size") && p[1] && p[2])
    {
      i += 2;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->real_hash_size = atoi (p[1]);
      options->virtual_hash_size = atoi (p[2]);
      if (options->real_hash_size < 1 || options->virtual_hash_size < 1)
	{
	  msg (msglevel, "Options error: --hash-size sizes must be >= 1 (preferably a power of 2)");
	  goto err;
	}
    }
  else if (streq (p[0], "connect-freq") && p[1] && p[2])
    {
      i += 2;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->cf_max = atoi (p[1]);
      options->cf_per = atoi (p[2]);
      if (options->cf_max < 0 || options->cf_per < 0)
	{
	  msg (msglevel, "Options error: --connect-freq parms must be > 0");
	  goto err;
	}
    }
  else if (streq (p[0], "max-clients") && p[1])
    {
      i += 1;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->max_clients = atoi (p[1]);
      if (options->max_clients < 0)
	{
	  msg (msglevel, "Options error: --max-clients must be at least 1");
	  goto err;
	}
    }
  else if (streq (p[0], "client-cert-not-required"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->client_cert_not_required = true;
    }
  else if (streq (p[0], "username-as-common-name"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->username_as_common_name = true;
    }
  else if (streq (p[0], "auth-user-pass-verify") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->auth_user_pass_verify_script = p[1];
    }
  else if (streq (p[0], "auth-user-pass"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (p[1])
	{
	  ++i;
	  options->auth_user_pass_file = p[1];
	}
      else
	options->auth_user_pass_file = "stdin";
    }
  else if (streq (p[0], "client-connect") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->client_connect_script = p[1];
    }
  else if (streq (p[0], "client-disconnect") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->client_disconnect_script = p[1];
    }
  else if (streq (p[0], "learn-address") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->learn_address_script = p[1];
    }
  else if (streq (p[0], "tmp-dir") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tmp_dir = p[1];
    }
  else if (streq (p[0], "client-config-dir") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->client_config_dir = p[1];
    }
  else if (streq (p[0], "ccd-exclusive"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->ccd_exclusive = true;
    }
  else if (streq (p[0], "bcast-buffers") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->n_bcast_buf = atoi (p[1]);
      if (options->n_bcast_buf < 1)
	msg (msglevel, "Options error: --bcast-buffers parameter must be > 0");
    }
  else if (streq (p[0], "tcp-queue-limit") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tcp_queue_limit = atoi (p[1]);
      if (options->tcp_queue_limit < 1)
	msg (msglevel, "Options error: --tcp-queue-limit parameter must be > 0");
    }
  else if (streq (p[0], "client-to-client"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->enable_c2c = true;
    }
  else if (streq (p[0], "duplicate-cn"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->duplicate_cn = true;
    }
  else if (streq (p[0], "iroute") && p[1])
    {
      const char *netmask = NULL;
      VERIFY_PERMISSION (OPT_P_INSTANCE);
      ++i;
      if (p[2])
	{
	  ++i;
	  netmask = p[2];
	}
      option_iroute (options, p[1], netmask, msglevel);
    }
  else if (streq (p[0], "ifconfig-push") && p[1] && p[2])
    {
      VERIFY_PERMISSION (OPT_P_INSTANCE);
      i += 2;
      options->push_ifconfig_local = getaddr (GETADDR_HOST_ORDER, p[1], 0, NULL, NULL);
      options->push_ifconfig_remote_netmask = getaddr (GETADDR_HOST_ORDER, p[2], 0, NULL, NULL);
      if (options->push_ifconfig_local && options->push_ifconfig_remote_netmask)
	{
	  options->push_ifconfig_defined = true;
	}
      else
	{
	  msg (msglevel, "Options error: cannot parse --ifconfig-push addresses");
	}
    }
#endif
#ifdef WIN32
  else if (streq (p[0], "route-method") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_ROUTE);
      if (streq (p[1], "ipapi"))
	options->route_method = ROUTE_METHOD_IPAPI;
      else if (streq (p[1], "exe"))
	options->route_method = ROUTE_METHOD_EXE;
      else
	{
	  msg (msglevel, "Options error: --route method must be 'ipapi' or 'exe'");
	  goto err;
	}
    }
  else if (streq (p[0], "ip-win32") && p[1])
    {
      const int index = ascii2ipset (p[1]);
      struct tuntap_options *to = &options->tuntap_options;
      ++i;

      VERIFY_PERMISSION (OPT_P_IPWIN32);

      to->ip_win32_defined = true;
 
      if (index < 0)
	{
	  msg (msglevel,
	       "Options error: Bad --ip-win32 method: '%s'.  Allowed methods: %s",
	       p[1],
	       ipset2ascii_all (&gc));
	  goto err;
	}

      to->ip_win32_type = index;

      if (to->ip_win32_type == IPW32_SET_DHCP_MASQ)
	{
	  if (p[2])
	    {
	      ++i;
	      if (!streq (p[2], "default"))
		{
		  int offset = atoi (p[2]);

		  to->dhcp_masq_custom_offset = true;

		  if (!(offset > -256 && offset < 256))
		    {
		      msg (msglevel, "Options error: --ip-win32 dynamic [offset] [lease-time]: offset (%d) must be > -256 and < 256", offset);
		      goto err;
		    }

		  to->dhcp_masq_offset = offset;
		}

	      if (p[3])
		{
		  const int min_lease = 30;
		  int lease_time;
		  ++i;
		  lease_time = atoi (p[3]);
		  if (lease_time < min_lease)
		    {
		      msg (msglevel, "Options error: --ip-win32 dynamic [offset] [lease-time]: lease time parameter (%d) must be at least %d seconds", lease_time, min_lease);
		      goto err;
		    }
		  to->dhcp_lease_time = lease_time;
		}
	    }
	}
    }
  else if (streq (p[0], "dhcp-option") && p[1])
    {
      struct tuntap_options *o = &options->tuntap_options;
      ++i;
      VERIFY_PERMISSION (OPT_P_IPWIN32);

      o->dhcp_options = true;

      if (streq (p[1], "DOMAIN") && p[2])
	{
	  ++i;
	  o->domain = p[2];
	}
      else if (streq (p[1], "NBS") && p[2])
	{
	  ++i;
	  o->netbios_scope = p[2];
	}
      else if (streq (p[1], "NBT") && p[2])
	{
	  int t;
	  ++i;
	  t = atoi (p[2]);
	  if (!(t == 1 || t == 2 || t == 4 || t == 8))
	    {
	      msg (msglevel, "Options error: --dhcp-option NBT: parameter (%d) must be 1, 2, 4, or 8", t);
	      goto err;
	    }
	  o->netbios_node_type = t;
	}
      else if (streq (p[1], "DNS") && p[2])
	{
	  ++i;
	  dhcp_option_address_parse ("DNS", p[2], o->dns, &o->dns_len, msglevel);
	}
      else if (streq (p[1], "WINS") && p[2])
	{
	  ++i;
	  dhcp_option_address_parse ("WINS", p[2], o->wins, &o->wins_len, msglevel);
	}
      else if (streq (p[1], "NTP") && p[2])
	{
	  ++i;
	  dhcp_option_address_parse ("NTP", p[2], o->ntp, &o->ntp_len, msglevel);
	}
      else if (streq (p[1], "NBDD") && p[2])
	{
	  ++i;
	  dhcp_option_address_parse ("NBDD", p[2], o->nbdd, &o->nbdd_len, msglevel);
	}
      else
	{
	  msg (msglevel, "Options error: --dhcp-option: unknown option type '%s' or missing parameter", p[1]);
	  goto err;
	}
    }
  else if (streq (p[0], "show-adapters"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      show_tap_win32_adapters (M_INFO|M_NOPREFIX, M_WARN|M_NOPREFIX);
      openvpn_exit (OPENVPN_EXIT_STATUS_GOOD); /* exit point */
    }
  else if (streq (p[0], "show-net"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      show_routes (M_INFO|M_NOPREFIX);
      show_adapters (M_INFO|M_NOPREFIX);
      openvpn_exit (OPENVPN_EXIT_STATUS_GOOD); /* exit point */
    }
  else if (streq (p[0], "show-net-up"))
    {
      VERIFY_PERMISSION (OPT_P_UP);
      options->show_net_up = true;
    }
  else if (streq (p[0], "tap-sleep") && p[1])
    {
      int s;
      ++i;
      VERIFY_PERMISSION (OPT_P_IPWIN32);
      s = atoi (p[1]);
      if (s < 0 || s >= 256)
	{
	  msg (msglevel, "Options error: --tap-sleep parameter must be between 0 and 255");
	  goto err;
	}
      options->tuntap_options.tap_sleep = s;
    }
  else if (streq (p[0], "dhcp-renew"))
    {
      VERIFY_PERMISSION (OPT_P_IPWIN32);
      options->tuntap_options.dhcp_renew = true;
    }
  else if (streq (p[0], "dhcp-pre-release"))
    {
      VERIFY_PERMISSION (OPT_P_IPWIN32);
      options->tuntap_options.dhcp_pre_release = true;
    }
  else if (streq (p[0], "dhcp-release"))
    {
      VERIFY_PERMISSION (OPT_P_IPWIN32);
      options->tuntap_options.dhcp_release = true;
    }
  else if (streq (p[0], "show-valid-subnets"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      show_valid_win32_tun_subnets ();
      openvpn_exit (OPENVPN_EXIT_STATUS_USAGE); /* exit point */
    }
  else if (streq (p[0], "pause-exit"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      set_pause_exit_win32 ();
    }
  else if (streq (p[0], "service") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->exit_event_name = p[1];
      if (p[2])
	{
	  ++i;
	  options->exit_event_initial_state = (atoi(p[2]) != 0);
	}
    }
#else
  else if (streq (p[0], "dhcp-option") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_IPWIN32);
      if (p[2])
	++i;
      foreign_option (options, p, 3, es);
    }
  else if (streq (p[0], "route-method") && p[1]) /* ignore when pushed to non-Windows OS */
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_ROUTE);
    }
#endif
#if PASSTOS_CAPABILITY
  else if (streq (p[0], "passtos"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->passtos = true;
    }
#endif
#ifdef USE_LZO
  else if (streq (p[0], "comp-lzo"))
    {
      VERIFY_PERMISSION (OPT_P_COMP);
      options->comp_lzo = true;
    }
  else if (streq (p[0], "comp-noadapt"))
    {
      VERIFY_PERMISSION (OPT_P_COMP);
      options->comp_lzo_adaptive = false;
    }
#endif /* USE_LZO */
#ifdef USE_CRYPTO
  else if (streq (p[0], "show-ciphers"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->show_ciphers = true;
    }
  else if (streq (p[0], "show-digests"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->show_digests = true;
    }
  else if (streq (p[0], "show-engines"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->show_engines = true;
    }
  else if (streq (p[0], "secret") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->shared_secret_file = p[1];
      if (p[2])
	{
	  options->key_direction = ascii2keydirection (p[2]);
	  ++i;
	}
    }
  else if (streq (p[0], "genkey"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->genkey = true;
    }
  else if (streq (p[0], "auth") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->authname_defined = true;
      options->authname = p[1];
      if (streq (options->authname, "none"))
	{
	  options->authname_defined = false;
	  options->authname = NULL;
	}
    }
  else if (streq (p[0], "auth"))
    {
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->authname_defined = true;
    }
  else if (streq (p[0], "cipher") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->ciphername_defined = true;
      options->ciphername = p[1];
      if (streq (options->ciphername, "none"))
	{
	  options->ciphername_defined = false;
	  options->ciphername = NULL;
	}
    }
  else if (streq (p[0], "cipher"))
    {
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->ciphername_defined = true;
    }
  else if (streq (p[0], "no-replay"))
    {
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->replay = false;
    }
  else if (streq (p[0], "replay-window"))
    {
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      if (p[1])
	{
	  ++i;
	  options->replay_window = atoi (p[1]);
	  if (!(MIN_SEQ_BACKTRACK <= options->replay_window && options->replay_window <= MAX_SEQ_BACKTRACK))
	    {
	      msg (msglevel, "Options error: replay-window window size parameter (%d) must be between %d and %d",
		   options->replay_window,
		   MIN_SEQ_BACKTRACK,
		   MAX_SEQ_BACKTRACK);
	      goto err;
	    }

	  if (p[2])
	    {
	      ++i;
	      options->replay_time = atoi (p[2]);
	      if (!(MIN_TIME_BACKTRACK <= options->replay_time && options->replay_time <= MAX_TIME_BACKTRACK))
		{
		  msg (msglevel, "Options error: replay-window time window parameter (%d) must be between %d and %d",
		       options->replay_time,
		       MIN_TIME_BACKTRACK,
		       MAX_TIME_BACKTRACK);
		  goto err;
		}
	    }
	}
      else
	{
	  msg (msglevel, "Options error: replay-window option is missing window size parameter");
	  goto err;
	}
    }
  else if (streq (p[0], "mute-replay-warnings"))
    {
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->mute_replay_warnings = true;
    }
  else if (streq (p[0], "no-iv"))
    {
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->use_iv = false;
    }
  else if (streq (p[0], "replay-persist") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->packet_id_file = p[1];
    }
  else if (streq (p[0], "test-crypto"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->test_crypto = true;
    }
  else if (streq (p[0], "engine"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (p[1])
	{
	  ++i;
	  options->engine = p[1];
	}
      else
	options->engine = "auto";
    }  
#ifdef HAVE_EVP_CIPHER_CTX_SET_KEY_LENGTH
  else if (streq (p[0], "keysize") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_CRYPTO);
      options->keysize = atoi (p[1]) / 8;
      if (options->keysize < 0 || options->keysize > MAX_CIPHER_KEY_LENGTH)
	{
	  msg (msglevel, "Options error: Bad keysize: %s", p[1]);
	  goto err;
	}
    }
#endif
#ifdef USE_SSL
  else if (streq (p[0], "show-tls"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->show_tls_ciphers = true;
    }
  else if (streq (p[0], "tls-server"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tls_server = true;
    }
  else if (streq (p[0], "tls-client"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tls_client = true;
    }
  else if (streq (p[0], "ca") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->ca_file = p[1];
    }
  else if (streq (p[0], "dh") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->dh_file = p[1];
    }
  else if (streq (p[0], "cert") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->cert_file = p[1];
    }
  else if (streq (p[0], "key") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->priv_key_file = p[1];
    }
  else if (streq (p[0], "pkcs12") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->pkcs12_file = p[1];
    }
  else if (streq (p[0], "askpass"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      if (p[1])
	{
	  ++i;
	  options->key_pass_file = p[1];
	}
      else
	options->key_pass_file = "stdin";	
    }
  else if (streq (p[0], "single-session"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->single_session = true;
    }
  else if (streq (p[0], "tls-exit"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tls_exit = true;
    }
  else if (streq (p[0], "tls-cipher") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->cipher_list = p[1];
    }
  else if (streq (p[0], "crl-verify") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->crl_file = p[1];
    }
  else if (streq (p[0], "tls-verify") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_SCRIPT);
      options->tls_verify = string_substitute (p[1], ',', ' ', &options->gc);
    }
  else if (streq (p[0], "tls-remote") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tls_remote = p[1];
    }
  else if (streq (p[0], "tls-timeout") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TLS_PARMS);
      options->tls_timeout = positive (atoi (p[1]));
    }
  else if (streq (p[0], "reneg-bytes") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TLS_PARMS);
      options->renegotiate_bytes = positive (atoi (p[1]));
    }
  else if (streq (p[0], "reneg-pkts") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TLS_PARMS);
      options->renegotiate_packets = positive (atoi (p[1]));
    }
  else if (streq (p[0], "reneg-sec") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TLS_PARMS);
      options->renegotiate_seconds = positive (atoi (p[1]));
    }
  else if (streq (p[0], "hand-window") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TLS_PARMS);
      options->handshake_window = positive (atoi (p[1]));
    }
  else if (streq (p[0], "tran-window") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_TLS_PARMS);
      options->transition_window = positive (atoi (p[1]));
    }
  else if (streq (p[0], "tls-auth") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->tls_auth_file = p[1];
      if (p[2])
	{
	  options->key_direction = ascii2keydirection (p[2]);
	  ++i;
	}
    }
  else if (streq (p[0], "key-method") && p[1])
    {
      ++i;
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->key_method = atoi (p[1]);
      if (options->key_method < KEY_METHOD_MIN || options->key_method > KEY_METHOD_MAX)
	{
	  msg (msglevel, "Options error: key_method parameter (%d) must be >= %d and <= %d",
	       options->key_method,
	       KEY_METHOD_MIN,
	       KEY_METHOD_MAX);
	  goto err;
	}
    }
#endif /* USE_SSL */
#endif /* USE_CRYPTO */
#ifdef TUNSETPERSIST
  else if (streq (p[0], "rmtun"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->persist_config = true;
      options->persist_mode = 0;
    }
  else if (streq (p[0], "mktun"))
    {
      VERIFY_PERMISSION (OPT_P_GENERAL);
      options->persist_config = true;
      options->persist_mode = 1;
    }
#endif
  else
    {
      if (file)
	msg (msglevel, "Options error: Unrecognized option or missing parameter(s) in %s:%d: %s", file, line, p[0]);
      else
	msg (msglevel, "Options error: Unrecognized option or missing parameter(s): --%s", p[0]);
    }
 err:
  gc_free (&gc);
  return i;
}
