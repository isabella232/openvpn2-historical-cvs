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
 * Plug-in types.  These types correspond to the set of script callbacks
 * supported by OpenVPN.
 */
#define OPENVPN_PLUGIN_UP                    0
#define OPENVPN_PLUGIN_DOWN                  1
#define OPENVPN_PLUGIN_ROUTE_UP              2
#define OPENVPN_PLUGIN_IPCHANGE              3
#define OPENVPN_PLUGIN_TLS_VERIFY            4
#define OPENVPN_PLUGIN_AUTH_USER_PASS_VERIFY 5
#define OPENVPN_PLUGIN_CLIENT_CONNECT        6
#define OPENVPN_PLUGIN_CLIENT_DISCONNECT     7
#define OPENVPN_PLUGIN_LEARN_ADDRESS         8
#define OPENVPN_PLUGIN_N                     9

/*
 * Build a mask out of a set of plug-in types.
 */
#define OPENVPN_PLUGIN_MASK(x) (1<<(x))

/*
 * A pointer to a plugin-defined object which contains
 * the object state.
 */
typedef void *openvpn_plugin_handle_t;

/*
 * Return value for openvpn_plugin_func_v1 function
 */
#define OPENVPN_PLUGIN_FUNC_SUCCESS  0
#define OPENVPN_PLUGIN_FUNC_ERROR    1

/*
 * For Windows (needs to be modified for MSVC)
 */
#if defined(__MINGW32_VERSION) && !defined(OPENVPN_PLUGIN_H)
# define OPENVPN_EXPORT __declspec(dllexport)
#else
# define OPENVPN_EXPORT
#endif

/*
 * If OPENVPN_PLUGIN_H is defined, we know that we are being
 * included in an OpenVPN compile, rather than a plugin compile.
 */
#ifdef OPENVPN_PLUGIN_H

/*
 * We are compiling OpenVPN.
 */
#define OPENVPN_PLUGIN_DEF        typedef
#define OPENVPN_PLUGIN_FUNC(name) (*name)

#else

/*
 * We are compiling plugin.
 */
#define OPENVPN_PLUGIN_DEF        OPENVPN_EXPORT
#define OPENVPN_PLUGIN_FUNC(name) name

#endif

/*
 * Multiple plugin modules can be cascaded, and modules can be
 * used in tandem with scripts.  The order of operation is that
 * the module func() functions are called in the order that
 * the modules were specified in the config file.  If a script
 * was specified as well, it will be called last.  If the
 * return code of the module/script controls an authentication
 * function (such as tls-verify or auth-user-pass-verify), then
 * every module and script must return success (0) in order for
 * the connection to be authenticated.
 */

/*
 * Prototypes for functions which OpenVPN plug-ins must define.
 */

/*
 * FUNCTION: openvpn_plugin_open_v1
 * 
 * Called on initial plug-in load.  OpenVPN will preserve plug-in state
 * across SIGUSR1 restarts but not across SIGHUP restarts.  A SIGHUP reset
 * will cause the plugin to be closed and reopened.
 *
 * ARGUMENTS
 *
 * *type_mask : Set by OpenVPN to the logical OR of all script
 *              types which this version of OpenVPN supports.  The plug-in
 *              should set this value to the logical OR of all script types
 *              which the plug-in wants to intercept.  For example, if the
 *              script wants to intercept the client-connect and
 *              client-disconnect script types:
 *
 *              *type_mask = OPENVPN_PLUGIN_MASK(OPENVPN_PLUGIN_CLIENT_CONNECT)
 *                         | OPENVPN_PLUGIN_MASK(OPENVPN_PLUGIN_CLIENT_DISCONNECT)
 *
 * argv : a NULL-terminated array of options provided to the OpenVPN
 *        "plug-in" directive.  argv[0] is the dynamic library pathname.
 *
 * envp : a NULL-terminated array of OpenVPN-set environmental
 *        variables in "name=value" format.  Note that for security reasons,
 *        these variables are not actually written to the "official"
 *        environmental variable store of the process.
 *
 * RETURN VALUE
 *
 * An openvpn_plugin_handle_t value on success, NULL on failure
 */
OPENVPN_PLUGIN_DEF openvpn_plugin_handle_t OPENVPN_PLUGIN_FUNC(openvpn_plugin_open_v1)
     (unsigned int *type_mask, const char *argv[], const char *envp[]);

/*
 * FUNCTION: openvpn_plugin_func_v1
 *
 * Called to perform the work of a given script type.
 *
 * ARGUMENTS
 *
 * handle : the openvpn_plugin_handle_t value which was returned by
 *          openvpn_plugin_open_v1.
 *
 * type : one of the PLUGIN_x types
 *
 * argv : a NULL-terminated array of "command line" options which
 *        would normally be passed to the script.  argv[0] is the dynamic
 *        library pathname.
 *
 * envp : a NULL-terminated array of OpenVPN-set environmental
 *        variables in "name=value" format.  Note that for security reasons,
 *        these variables are not actually written to the "official"
 *        environmental variable store of the process.
 *
 * RETURN VALUE
 *
 * OPENVPN_PLUGIN_FUNC_SUCCESS on success, OPENVPN_PLUGIN_FUNC_ERROR on failure
 */
OPENVPN_PLUGIN_DEF int OPENVPN_PLUGIN_FUNC(openvpn_plugin_func_v1)
     (openvpn_plugin_handle_t handle, const int type, const char *argv[], const char *envp[]);

/*
 * FUNCTION: openvpn_plugin_close_v1
 *
 * ARGUMENTS
 *
 * handle : the openvpn_plugin_handle_t value which was returned by
 *          openvpn_plugin_open_v1.
 *
 * Called immediately prior to plug-in unload.
 */
OPENVPN_PLUGIN_DEF void OPENVPN_PLUGIN_FUNC(openvpn_plugin_close_v1)
     (openvpn_plugin_handle_t handle);