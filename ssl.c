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
 * The routines in this file deal with dynamically negotiating
 * the data channel HMAC and cipher keys through a TLS session.
 *
 * Both the TLS session and the data channel are multiplexed
 * over the same TCP/UDP port.
 */

#ifdef WIN32
#include "config-win32.h"
#else
#include "config.h"
#endif

#if defined(USE_CRYPTO) && defined(USE_SSL)

#include "syshead.h"

#include "ssl.h"
#include "error.h"
#include "common.h"
#include "integer.h"
#include "socket.h"
#include "thread.h"
#include "misc.h"
#include "fdmisc.h"
#include "interval.h"
#include "options.h"

#include "memdbg.h"

#ifdef MEASURE_TLS_HANDSHAKE_STATS

static int tls_handshake_success; /* GLOBAL */
static int tls_handshake_error;   /* GLOBAL */
static int tls_packets_generated; /* GLOBAL */
static int tls_packets_sent;      /* GLOBAL */

#define INCR_SENT       ++tls_packets_sent
#define INCR_GENERATED  ++tls_packets_generated
#define INCR_SUCCESS    ++tls_handshake_success
#define INCR_ERROR      ++tls_handshake_error

void
show_tls_performance_stats(void)
{
  msg (D_TLS_DEBUG_LOW, "TLS Handshakes, success=%f%% (good=%d, bad=%d), retransmits=%f%%",
       (double) tls_handshake_success / (tls_handshake_success + tls_handshake_error) * 100.0,
       tls_handshake_success, tls_handshake_error,
       (double) (tls_packets_sent - tls_packets_generated) / tls_packets_generated * 100.0);
}
#else

#define INCR_SENT
#define INCR_GENERATED
#define INCR_SUCCESS
#define INCR_ERROR

#endif

#ifdef BIO_DEBUG

static FILE *biofp;                            /* GLOBAL */
static bool biofp_toggle;                      /* GLOBAL */
static time_t biofp_last_open;                 /* GLOBAL */
static const int biofp_reopen_interval = 600;  /* GLOBAL */

static void
close_biofp()
{
  if (biofp)
    {
      ASSERT (!fclose (biofp));
      biofp = NULL;
    }
}

static void
open_biofp()
{
  const time_t current = time (NULL);
  const pid_t pid = getpid ();

  if (biofp_last_open + biofp_reopen_interval < current)
    close_biofp();
  if (!biofp)
    {
      char fn[256];
      openvpn_snprintf(fn, sizeof(fn), "bio/%d-%d.log", pid, biofp_toggle);
      biofp = fopen (fn, "w");
      ASSERT (biofp);
      biofp_last_open = time (NULL);
      biofp_toggle ^= 1;
    }
}

static void
bio_debug_data (const char *mode, BIO *bio, uint8_t *buf, int len, const char *desc)
{
  if (len > 0)
    {
      open_biofp();
      fprintf(biofp, "BIO_%s %s time=" time_format " bio=" ptr_format " len=%d data=%s\n",
	      mode, desc, time (NULL), bio, len, format_hex (buf, len, 0));
      fflush (biofp);
    }
}

static void
bio_debug_oc (const char *mode, BIO *bio)
{
  open_biofp();
  fprintf(biofp, "BIO %s time=" time_format " bio=" ptr_format "\n",
	  mode, time (NULL), bio);
  fflush (biofp);
}

#endif

/*
 * Max number of bytes we will add
 * for data structures common to both
 * data and control channel packets.
 * (opcode only). 
 */
void
tls_adjust_frame_parameters(struct frame *frame)
{
  frame_add_to_extra_frame (frame, 1); /* space for opcode */
}

/*
 * Max number of bytes we will add
 * to control channel packet. 
 */
static void
tls_init_control_channel_frame_parameters(const struct frame *data_channel_frame,
					  struct frame *frame)
{
  /*
   * frame->extra_frame is already initialized with tls_auth buffer requirements,
   * if --tls-auth is enabled.
   */

  /* inherit link MTU and extra_link from data channel */
  frame->link_mtu = data_channel_frame->link_mtu;
  frame->extra_link = data_channel_frame->extra_link;

  /* set extra_frame */
  tls_adjust_frame_parameters (frame);
  reliable_ack_adjust_frame_parameters (frame, CONTROL_SEND_ACK_MAX);
  frame_add_to_extra_frame (frame, SID_SIZE + sizeof (packet_id_type));

  /* set dynamic link MTU to minimum value */
  frame_set_mtu_dynamic (frame, 0, SET_MTU_TUN);
}

/*
 * Allocate space in SSL objects
 * in which to store a struct tls_session
 * pointer back to parent.
 */

static int mydata_index; /* GLOBAL */

static void
ssl_set_mydata_index ()
{
  mydata_index = SSL_get_ex_new_index (0, "struct session *", NULL, NULL, NULL);
  ASSERT (mydata_index >= 0);
}

void
init_ssl_lib ()
{
  SSL_library_init ();
  SSL_load_error_strings ();
  OpenSSL_add_all_algorithms ();

  init_crypto_lib();

  /*
   * If you build the OpenSSL library and OpenVPN with
   * CRYPTO_MDEBUG, you will get a listing of OpenSSL
   * memory leaks on program termination.
   */
#ifdef CRYPTO_MDEBUG
  CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ON);
#endif

  ssl_set_mydata_index ();
}

void
free_ssl_lib ()
{
#ifdef CRYPTO_MDEBUG
  FILE* fp = fopen ("sdlog", "w");
  ASSERT (fp);
  CRYPTO_mem_leaks_fp (fp);
  fclose (fp);
#endif

  EVP_cleanup ();
  ERR_free_strings ();
}

/*
 * OpenSSL library calls back here if the private key
 * is protected by a password.
 */
int
pem_password_callback (char *buf, int size, int rwflag, void *u)
{
#ifdef HAVE_GETPASS
  static char passbuf[256];

  if (!strlen (passbuf))
    {
      char *gp = getpass ("Enter PEM pass phrase:");
      if (!gp)
	msg (M_FATAL, "TLS Error: Error reading PEM pass phrase for private key");
      strncpynt (passbuf, gp, sizeof (passbuf));
      memset (gp, 0, strlen (gp));
    }

  if (buf)
    {
      if (!strlen (passbuf))
	msg (M_FATAL, "TLS Error: Need PEM pass phrase for private key");
      strncpynt (buf, passbuf, size);
      CLEAR (passbuf);
      return strlen (buf);
    }
#else
  msg (M_FATAL, "Sorry but I can't read a password from the console because this operating system or C library doesn't support the getpass() function");
#endif
  return 0;
}

/*
 * OpenSSL callback to get a temporary RSA key, mostly
 * used for export ciphers.
 */
static RSA *
tmp_rsa_cb (SSL * s, int is_export, int keylength)
{
  static RSA *rsa_tmp = NULL;
  if (rsa_tmp == NULL)
    {
      msg (D_HANDSHAKE, "Generating temp (%d bit) RSA key", keylength);
      rsa_tmp = RSA_generate_key (keylength, RSA_F4, NULL, NULL);
    }
  return (rsa_tmp);
}

/*
 * Extract common name from an X509 subject name.
 */

static void
extract_common_name (char *out, int size, const char *subject)
{
  /*
   * Example subject:
   *
   * /C=US/ST=CO/L=Denver/O=NTLP/CN=Test-CA/Email=jim@yonan.net
   *
   * The common name is 'Test-CA'
   */

  char c;
  int state = 0;

  ASSERT (size > 0);
  out[--size] = '\0';

  do {
    c = *subject++;
    if (state == 4)
      {
	if (c == '/')
	  c = '\0';
	if (size > 0)
	  {
	    *out++ = c;
	    --size;
	  }
	else
	  break;
      }
    else if (c == '/')
      state = 1;
    else if (state == 1)
      {
	if (c == 'C')
	  state = 2;
	else
	  state = 0;
      }
    else if (state == 2)
      {
	if (c == 'N')
	  state = 3;
	else
	  state = 0;
      }
    else if (state == 3)
      {
	if (c == '=')
	  state = 4;
	else
	  state = 0;
      }
  } while (c != '\0');
}

static void
setenv_untrusted (struct tls_session *session)
{
  setenv_sockaddr ("untrusted", &session->untrusted_sockaddr);
}

/*
 * Our verify callback function -- check
 * that an incoming peer certificate is good.
 */

static int
verify_callback (int preverify_ok, X509_STORE_CTX * ctx)
{
  char subject[256];
  char envname[64];
  SSL *ssl;
  struct tls_session *session;
  const struct tls_options *opt;
  const int max_depth = 8;

  /* acquire script mutex */
  mutex_lock_static (L_SCRIPT);

  /*
   * Retrieve the pointer to the SSL of the connection currently treated
   * and the application specific data stored into the SSL object.
   */
  ssl = X509_STORE_CTX_get_ex_data (ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
  ASSERT (ssl);
  session = (struct tls_session *) SSL_get_ex_data (ssl, mydata_index);
  ASSERT (session);
  opt = session->opt;
  ASSERT (opt);

  X509_NAME_oneline (X509_get_subject_name (ctx->current_cert), subject,
		     sizeof (subject));
  subject[sizeof (subject) - 1] = '\0';
  safe_string (subject);

#if 0 /* print some debugging info */
  msg (D_LOW, "LOCAL OPT: %s", opt->local_options);
  msg (D_LOW, "X509: %s", subject);
#endif

  if (!preverify_ok)
    {
      /* Remote site specified a certificate, but it's not correct */
      msg (D_TLS_ERRORS, "VERIFY ERROR: depth=%d, error=%s: %s",
	   ctx->error_depth, X509_verify_cert_error_string (ctx->error), subject);
      goto err;			/* Reject connection */
    }

  if (ctx->error_depth >= max_depth)
    msg (M_WARN, "TLS Warning: Convoluted certificate chain detected with depth [%d] greater than %d", ctx->error_depth, max_depth);

  /* export subject name string as environmental variable */
  session->verify_maxlevel = max_int (session->verify_maxlevel, ctx->error_depth);
  openvpn_snprintf (envname, sizeof(envname), "tls_id_%d", ctx->error_depth);
  setenv_str (envname, subject);

  /* export serial number as environmental variable */
  {
    const int serial = (int) ASN1_INTEGER_get (X509_get_serialNumber (ctx->current_cert));
    openvpn_snprintf (envname, sizeof(envname), "tls_serial_%d", ctx->error_depth);
    setenv_int (envname, serial);
  }

  /* export current untrusted IP */
  setenv_untrusted (session);
  
  if (opt->verify_x509name && ctx->error_depth == 0)
    {
      if (strcmp (opt->verify_x509name, subject) == 0)
	msg (D_HANDSHAKE, "VERIFY X509NAME OK: %s", subject);
      else
	{
	  msg (D_HANDSHAKE, "VERIFY X509NAME ERROR: %s, must be %s",
	       subject, opt->verify_x509name);
	  goto err;		/* Reject connection */
	}
    }

  if (opt->verify_command)
    {
      char command[512];
      struct buffer out;
      int ret;

      setenv_str ("script_type", "tls-verify");

      buf_set_write (&out, (uint8_t*)command, sizeof (command));
      buf_printf (&out, "%s %d %s",
		  opt->verify_command,
		  ctx->error_depth,
		  subject);
      msg (D_TLS_DEBUG, "TLS: executing verify command: %s", command);
      ret = openvpn_system (command);

      if (system_ok (ret))
	{
	  msg (D_HANDSHAKE, "VERIFY SCRIPT OK: depth=%d, %s",
	       ctx->error_depth, subject);
	}
      else
	{
	  if (!system_executed (ret))
	    msg (M_ERR, "Verify command failed to execute: %s", command);
	  msg (D_HANDSHAKE, "VERIFY SCRIPT ERROR: depth=%d, %s",
	       ctx->error_depth, subject);
	  goto err;		/* Reject connection */
	}
    }
  
  if (opt->crl_file)
    {
      X509_CRL *crl=NULL;
      X509_REVOKED *revoked;
      BIO *in=NULL;
      int n,i,retval = 0;

      in=BIO_new(BIO_s_file());

      if (in == NULL) {
	msg (M_ERR, "CRL: BIO err");
	goto end;
      }
      if (BIO_read_filename(in, opt->crl_file) <= 0) {
	msg (M_ERR, "CRL: cannot read: %s", opt->crl_file);
	goto end;
      }
      crl=PEM_read_bio_X509_CRL(in,NULL,NULL,NULL);
      if (crl == NULL) {
	msg (M_ERR, "CRL: cannot read CRL from file %s", opt->crl_file);
	goto end;
      }

      n = sk_num(X509_CRL_get_REVOKED(crl));

      for (i = 0; i < n; i++) {
	revoked = (X509_REVOKED *)sk_value(X509_CRL_get_REVOKED(crl), i);
	if (ASN1_INTEGER_cmp(revoked->serialNumber, X509_get_serialNumber(ctx->current_cert)) == 0) {
	  msg (D_HANDSHAKE, "CRL CHECK FAILED: %s is REVOKED",subject);
	  goto end;
	}
      }

      retval = 1;
      msg (D_HANDSHAKE, "CRL CHECK OK: %s",subject);

    end:

      BIO_free(in);
      if (!retval)
	goto err;
    }

  msg (D_HANDSHAKE, "VERIFY OK: depth=%d, %s", ctx->error_depth, subject);

  /* save common name in session object */
  if (ctx->error_depth == 0)
    {
      char common_name[TLS_CN_LEN];
      extract_common_name (common_name, TLS_CN_LEN, subject);
      if (session->common_name)
	free (session->common_name);
      session->common_name = string_alloc (common_name, NULL);
    }
  
  mutex_unlock_static (L_SCRIPT);
  return 1;			/* Accept connection */

 err:
  mutex_unlock_static (L_SCRIPT);
  return 0;                     /* Reject connection */
}

int
get_max_tls_verify_id (struct tls_multi* multi)
{
  if (multi)
    return multi->session[TM_ACTIVE].verify_maxlevel;
  else
    return 0;
}

const char *
tls_common_name (struct tls_multi* multi, bool null)
{
  const char *ret = NULL;
  if (multi)
    ret = multi->session[TM_ACTIVE].common_name;
  if (ret)
    return ret;
  else if (null)
    return NULL;
  else
    return "UNDEF";
}

/*
 * Print debugging information on SSL/TLS session negotiation.
 */
static void
info_callback (INFO_CALLBACK_SSL_CONST SSL * s, int where, int ret)
{
  if (where & SSL_CB_LOOP)
    {
      msg (D_HANDSHAKE_VERBOSE, "SSL state (%s): %s",
	   where & SSL_ST_CONNECT ? "connect" :
	   where & SSL_ST_ACCEPT ? "accept" :
	   "undefined", SSL_state_string_long (s));
    }
  else if (where & SSL_CB_ALERT)
    {
      msg (D_HANDSHAKE_VERBOSE, "SSL alert (%s): %s: %s",
	   where & SSL_CB_READ ? "read" : "write",
	   SSL_alert_type_string_long (ret),
	   SSL_alert_desc_string_long (ret));
    }
}

/*
 * Initialize SSL context.
 * All files are in PEM format.
 */
SSL_CTX *
init_ssl (bool server,
	  const char *ca_file,
	  const char *dh_file,
	  const char *cert_file,
	  const char *priv_key_file,
	  const char *cipher_list)
{
  SSL_CTX *ctx;
  DH *dh;
  BIO *bio;

  if (server)
    {
      ctx = SSL_CTX_new (TLSv1_server_method ());
      if (ctx == NULL)
	msg (M_SSLERR, "SSL_CTX_new TLSv1_server_method");

      SSL_CTX_set_tmp_rsa_callback (ctx, tmp_rsa_cb);

      /* Get Diffie Hellman Parameters */
      if (!(bio = BIO_new_file (dh_file, "r")))
	msg (M_SSLERR, "Cannot open %s for DH parameters", dh_file);
      dh = PEM_read_bio_DHparams (bio, NULL, NULL, NULL);
      BIO_free (bio);
      if (!dh)
	msg (M_SSLERR, "Cannot load DH parameters from %s", dh_file);
      if (!SSL_CTX_set_tmp_dh (ctx, dh))
	msg (M_SSLERR, "SSL_CTX_set_tmp_dh");
      msg (D_TLS_DEBUG_LOW, "Diffie-Hellman initialized with %d bit key",
	   8 * DH_size (dh));
      DH_free (dh);
    }
  else				/* if client */
    {
      ctx = SSL_CTX_new (TLSv1_client_method ());
      if (ctx == NULL)
	msg (M_SSLERR, "SSL_CTX_new TLSv1_client_method");
    }

  /* Set SSL options */
  SSL_CTX_set_session_cache_mode (ctx, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_options (ctx, SSL_OP_SINGLE_DH_USE);

  /* Set callback for getting password from user to decrypt private key */
  SSL_CTX_set_default_passwd_cb (ctx, pem_password_callback);

  /* Load Certificate */
  if (!SSL_CTX_use_certificate_file (ctx, cert_file, SSL_FILETYPE_PEM))
    msg (M_SSLERR, "Cannot load certificate file %s", cert_file);

  /* Load Private Key */
  if (!SSL_CTX_use_PrivateKey_file (ctx, priv_key_file, SSL_FILETYPE_PEM))
    msg (M_SSLERR, "Cannot load private key file %s", priv_key_file);
  warn_if_group_others_accessible (priv_key_file);

  /* Check Private Key */
  if (!SSL_CTX_check_private_key (ctx))
    msg (M_SSLERR, "Private key does not match the certificate");

  /* Load CA file for verifying peer supplied certificate */
  if (!SSL_CTX_load_verify_locations (ctx, ca_file, NULL))
    msg (M_SSLERR, "Cannot load CA certificate file %s (SSL_CTX_load_verify_locations)", ca_file);

  /* Load names of CAs from file and use it as a client CA list */
  {
    STACK_OF(X509_NAME) *cert_names;
    cert_names = SSL_load_client_CA_file (ca_file);
    if (!cert_names)
      msg (M_SSLERR, "Cannot load CA certificate file %s (SSL_load_client_CA_file)", ca_file);
    SSL_CTX_set_client_CA_list (ctx, cert_names);
  }

  /* Enable the use of certificate chains */
  if (!SSL_CTX_use_certificate_chain_file (ctx, cert_file))
    msg (M_SSLERR, "Cannot load certificate chain file %s (SSL_use_certificate_chain_file)", cert_file);

  /* Require peer certificate verification */
  SSL_CTX_set_verify (ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
		      verify_callback);

  /* Connection information callback */
  SSL_CTX_set_info_callback (ctx, info_callback);

  /* Allowable ciphers */
  if (cipher_list)
    {
      if (!SSL_CTX_set_cipher_list (ctx, cipher_list))
	msg (M_SSLERR, "Problem with cipher list: %s", cipher_list);
    }

  return ctx;
}

/*
 * Print a one line summary of SSL/TLS session handshake.
 */
static void
print_details (SSL * c_ssl, const char *prefix)
{
  SSL_CIPHER *ciph;
  X509 *cert;
  char s1[256];
  char s2[256];

  s1[0] = s2[0] = 0;
  ciph = SSL_get_current_cipher (c_ssl);
  openvpn_snprintf (s1, sizeof (s1), "%s %s, cipher %s %s",
		    prefix,
		    SSL_get_version (c_ssl),
		    SSL_CIPHER_get_version (ciph),
		    SSL_CIPHER_get_name (ciph));
  cert = SSL_get_peer_certificate (c_ssl);
  if (cert != NULL)
    {
      EVP_PKEY *pkey = X509_get_pubkey (cert);
      if (pkey != NULL)
	{
	  if (pkey->type == EVP_PKEY_RSA && pkey->pkey.rsa != NULL
	      && pkey->pkey.rsa->n != NULL)
	    {
	      openvpn_snprintf (s2, sizeof (s2), ", %d bit RSA",
				BN_num_bits (pkey->pkey.rsa->n));
	    }
	  else if (pkey->type == EVP_PKEY_DSA && pkey->pkey.dsa != NULL
		   && pkey->pkey.dsa->p != NULL)
	    {
	      openvpn_snprintf (s2, sizeof (s2), ", %d bit DSA",
				BN_num_bits (pkey->pkey.dsa->p));
	    }
	  EVP_PKEY_free (pkey);
	}
      X509_free (cert);
    }
  /* The SSL API does not allow us to look at temporary RSA/DH keys,
   * otherwise we should print their lengths too */
  msg (D_HANDSHAKE, "%s%s", s1, s2);
}

/*
 * Show the TLS ciphers that are available for us to use
 * in the OpenSSL library.
 */
void
show_available_tls_ciphers ()
{
  SSL_CTX *ctx;
  SSL *ssl;
  const char *cipher_name;
  int priority = 0;

  ctx = SSL_CTX_new (TLSv1_method ());
  if (!ctx)
    msg (M_SSLERR, "Cannot create SSL_CTX object");
  ssl = SSL_new (ctx);
  if (!ssl)
    msg (M_SSLERR, "Cannot create SSL object");

  printf ("Available TLS Ciphers,\n");
  printf ("listed in order of preference:\n\n");
  while ((cipher_name = SSL_get_cipher_list (ssl, priority++)))
    printf ("%s\n", cipher_name);
  printf ("\n");

  SSL_free (ssl);
  SSL_CTX_free (ctx);
}

/*
 * The OpenSSL library has a notion of preference in TLS
 * ciphers.  Higher preference == more secure.
 * Return the highest preference cipher.
 */
void
get_highest_preference_tls_cipher (char *buf, int size)
{
  SSL_CTX *ctx;
  SSL *ssl;
  const char *cipher_name;

  ctx = SSL_CTX_new (TLSv1_method ());
  if (!ctx)
    msg (M_SSLERR, "Cannot create SSL_CTX object");
  ssl = SSL_new (ctx);
  if (!ssl)
    msg (M_SSLERR, "Cannot create SSL object");

  cipher_name = SSL_get_cipher_list (ssl, 0);
  strncpynt (buf, cipher_name, size);

  SSL_free (ssl);
  SSL_CTX_free (ctx);
}

/*
 * Map internal constants to ascii names.
 */
static const char *
state_name (int state)
{
  switch (state)
    {
    case S_UNDEF:
      return "S_UNDEF";
    case S_INITIAL:
      return "S_INITIAL";
    case S_PRE_START:
      return "S_PRE_START";
    case S_START:
      return "S_START";
    case S_SENT_KEY:
      return "S_SENT_KEY";
    case S_GOT_KEY:
      return "S_GOT_KEY";
    case S_ACTIVE:
      return "S_ACTIVE";
    case S_NORMAL:
      return "S_NORMAL";
    case S_ERROR:
      return "S_ERROR";
    default:
      return "S_???";
    }
}

static const char *
packet_opcode_name (int op)
{
  switch (op)
    {
    case P_CONTROL_HARD_RESET_CLIENT_V1:
      return "P_CONTROL_HARD_RESET_CLIENT_V1";
    case P_CONTROL_HARD_RESET_SERVER_V1:
      return "P_CONTROL_HARD_RESET_SERVER_V1";
    case P_CONTROL_HARD_RESET_CLIENT_V2:
      return "P_CONTROL_HARD_RESET_CLIENT_V2";
    case P_CONTROL_HARD_RESET_SERVER_V2:
      return "P_CONTROL_HARD_RESET_SERVER_V2";
    case P_CONTROL_SOFT_RESET_V1:
      return "P_CONTROL_SOFT_RESET_V1";
    case P_CONTROL_V1:
      return "P_CONTROL_V1";
    case P_ACK_V1:
      return "P_ACK_V1";
    case P_DATA_V1:
      return "P_DATA_V1";
    default:
      return "P_???";
    }
}

static const char *
session_index_name (int index)
{
  switch (index)
    {
    case TM_ACTIVE:
      return "TM_ACTIVE";
    case TM_UNTRUSTED:
      return "TM_UNTRUSTED";
    case TM_LAME_DUCK:
      return "TM_LAME_DUCK";
    default:
      return "TM_???";
    }
}

/*
 * For debugging.
 */
static const char *
print_key_id (struct tls_multi *multi, struct gc_arena *gc)
{
  int i;
  struct buffer out = alloc_buf_gc (256, gc);

  for (i = 0; i < KEY_SCAN_SIZE; ++i)
    {
      struct key_state *ks = multi->key_scan[i];
      buf_printf (&out, " [key#%d state=%s id=%d sid=%s]", i,
		  state_name (ks->state), ks->key_id,
		  session_id_print (&ks->session_id_remote, gc));
    }

  return BSTR (&out);
}

/*
 * Given a key_method, return true if op
 * represents the required form of hard_reset.
 *
 * If key_method = 0, return true if any
 * form of hard reset is used.
 */
static bool
is_hard_reset (int op, int key_method)
{
  if (!key_method || key_method == 1)
    if (op == P_CONTROL_HARD_RESET_CLIENT_V1 || op == P_CONTROL_HARD_RESET_SERVER_V1)
      return true;

  if (!key_method || key_method >= 2)
    if (op == P_CONTROL_HARD_RESET_CLIENT_V2 || op == P_CONTROL_HARD_RESET_SERVER_V2)
      return true;

  return false;
}

/*
 * OpenVPN's interface to SSL/TLS authentication,
 * encryption, and decryption is exclusively
 * through "memory BIOs".
 */
static BIO *
getbio (BIO_METHOD * type, const char *desc)
{
  BIO *ret;
  ret = BIO_new (type);
  if (!ret)
    msg (M_SSLERR, "Error creating %s BIO", desc);
  return ret;
}

/*
 * Write to an OpenSSL BIO in non-blocking mode.
 */
static int
bio_write (struct tls_multi* multi, BIO *bio, struct buffer *buf, const char *desc)
{
  int i;
  int ret = 0;
  ASSERT (buf->len >= 0);
  if (buf->len)
    {
      /*
       * Free the L_TLS lock prior to calling BIO routines
       * so that foreground thread can still call
       * tls_pre_decrypt or tls_pre_encrypt,
       * allowing tunnel packet forwarding to continue.
       */
#ifdef BIO_DEBUG
      bio_debug_data ("write", bio, BPTR (buf), BLEN (buf), desc);
#endif
      //mutex_unlock (multi->mutex);
      i = BIO_write (bio, BPTR (buf), BLEN (buf));
      //mutex_lock (multi->mutex);
      if (i < 0)
	{
	  if (BIO_should_retry (bio))
	    {
	      ;
	    }
	  else
	    {
	      msg (D_TLS_ERRORS | M_SSL, "TLS ERROR: BIO write %s error",
		   desc);
	      ret = -1;
	    }
	}
      else if (i != buf->len)
	{
	  msg (D_TLS_ERRORS | M_SSL,
	       "TLS ERROR: BIO write %s incomplete %d/%d", desc, i, buf->len);
	  ret = -1;
	}
      else
	{			/* successful write */
	  msg (D_HANDSHAKE_VERBOSE, "BIO write %s %d bytes", desc, i);
	  memset (BPTR (buf), 0, BLEN (buf)); /* erase data just written */
	  buf->len = 0;
	  ret = 1;
	}
    }
  return ret;
}

/*
 * Read from an OpenSSL BIO in non-blocking mode.
 */
static int
bio_read (struct tls_multi* multi, BIO *bio, struct buffer *buf, int maxlen, const char *desc)
{
  int i;
  int ret = 0;
  ASSERT (buf->len >= 0);
  if (buf->len)
    {
      ;
    }
  else
    {
      int len = buf_forward_capacity (buf);
      if (maxlen < len)
	len = maxlen;
      /*
       * Free the L_TLS lock prior to calling BIO routines
       * so that foreground thread can still call
       * tls_pre_decrypt or tls_pre_encrypt,
       * allowing tunnel packet forwarding to continue.
       *
       * BIO_read brackets most of the serious RSA
       * key negotiation number crunching.
       */
      //mutex_unlock (multi->mutex);
      i = BIO_read (bio, BPTR (buf), len);
      //mutex_lock (multi->mutex);
#ifdef BIO_DEBUG
      bio_debug_data ("read", bio, BPTR (buf), i, desc);
#endif
      if (i < 0)
	{
	  if (BIO_should_retry (bio))
	    {
	      ;
	    }
	  else
	    {
	      msg (D_TLS_ERRORS | M_SSL, "TLS_ERROR: BIO read %s error",
		   desc);
	      buf->len = 0;
	      ret = -1;
	    }
	}
      else if (!i)
	{
	  buf->len = 0;
	}
      else
	{			/* successful read */
	  msg (D_HANDSHAKE_VERBOSE, "BIO read %s %d bytes", desc, i);
	  buf->len = i;
	  ret = 1;
	}
    }
  return ret;
}

/*
 * Inline functions for reading from and writing
 * to BIOs.
 */

static inline int
key_state_write_plaintext (struct tls_multi *multi, struct key_state *ks, struct buffer *buf)
{
  return bio_write (multi, ks->ssl_bio, buf, "tls_write_plaintext");
}

static inline int
key_state_write_ciphertext (struct tls_multi *multi, struct key_state *ks, struct buffer *buf)
{
  return bio_write (multi, ks->ct_in, buf, "tls_write_ciphertext");
}

static inline int
key_state_read_plaintext (struct tls_multi *multi, struct key_state *ks, struct buffer *buf,
			  int maxlen)
{
  return bio_read (multi, ks->ssl_bio, buf, maxlen, "tls_read_plaintext");
}

static inline int
key_state_read_ciphertext (struct tls_multi *multi, struct key_state *ks, struct buffer *buf,
			   int maxlen)
{
  return bio_read (multi, ks->ct_out, buf, maxlen, "tls_read_ciphertext");
}

/*
 * Initialize a key_state.  Each key_state corresponds to
 * a specific SSL/TLS session.
 */
static void
key_state_init (struct tls_session *session, struct key_state *ks)
{
  update_time ();

  /*
   * Build TLS object that reads/writes ciphertext
   * to/from memory BIOs.
   */
  CLEAR (*ks);

  ks->ssl = SSL_new (session->opt->ssl_ctx);
  if (!ks->ssl)
    msg (M_SSLERR, "SSL_new failed");

  /* put session * in ssl object so we can access it
     from verify callback*/
  SSL_set_ex_data (ks->ssl, mydata_index, session);

  ks->ssl_bio = getbio (BIO_f_ssl (), "ssl_bio");
  ks->ct_in = getbio (BIO_s_mem (), "ct_in");
  ks->ct_out = getbio (BIO_s_mem (), "ct_out");

#ifdef BIO_DEBUG
  bio_debug_oc ("open ssl_bio", ks->ssl_bio);
  bio_debug_oc ("open ct_in", ks->ct_in);
  bio_debug_oc ("open ct_out", ks->ct_out);
#endif

  if (session->opt->server)
    SSL_set_accept_state (ks->ssl);
  else
    SSL_set_connect_state (ks->ssl);

  SSL_set_bio (ks->ssl, ks->ct_in, ks->ct_out);
  BIO_set_ssl (ks->ssl_bio, ks->ssl, BIO_NOCLOSE);

  /* Set control-channel initiation mode */
  ks->initial_opcode = session->initial_opcode;
  session->initial_opcode = P_CONTROL_SOFT_RESET_V1;
  ks->state = S_INITIAL;
  ks->key_id = session->key_id;

  /*
   * key_id increments to KEY_ID_MASK then recycles back to 1.
   * This way you know that if key_id is 0, it is the first key.
   */
  ++session->key_id;
  session->key_id &= P_KEY_ID_MASK;
  if (!session->key_id)
    session->key_id = 1;

  /* allocate key source material object */
  ALLOC_OBJ_CLEAR (ks->key_src, struct key_source2);

  /* allocate reliability objects */
  ALLOC_OBJ_CLEAR (ks->send_reliable, struct reliable);
  ALLOC_OBJ_CLEAR (ks->rec_reliable, struct reliable);
  ALLOC_OBJ_CLEAR (ks->rec_ack, struct reliable_ack);

  /* allocate buffers */
  ks->plaintext_read_buf = alloc_buf (PLAINTEXT_BUFFER_SIZE);
  ks->plaintext_write_buf = alloc_buf (PLAINTEXT_BUFFER_SIZE);
  ks->ack_write_buf = alloc_buf (BUF_SIZE (&session->opt->frame));
  reliable_init (ks->send_reliable, BUF_SIZE (&session->opt->frame),
		 FRAME_HEADROOM (&session->opt->frame), TLS_RELIABLE_N_SEND_BUFFERS);
  reliable_init (ks->rec_reliable, BUF_SIZE (&session->opt->frame),
		 FRAME_HEADROOM (&session->opt->frame), TLS_RELIABLE_N_REC_BUFFERS);
  reliable_set_timeout (ks->send_reliable, session->opt->packet_timeout);

  /* init packet ID tracker */
  packet_id_init (&ks->packet_id,
		  session->opt->replay_window,
		  session->opt->replay_time);
}

static void
key_state_free (struct key_state *ks, bool clear)
{
  ks->state = S_UNDEF;

  if (ks->ssl) {
#ifdef BIO_DEBUG
    bio_debug_oc ("close ssl_bio", ks->ssl_bio);
    bio_debug_oc ("close ct_in", ks->ct_in);
    bio_debug_oc ("close ct_out", ks->ct_out);
#endif
    BIO_free_all(ks->ssl_bio);
    SSL_free (ks->ssl);
  }

  free_key_ctx_bi (&ks->key);
  free_buf (&ks->plaintext_read_buf);
  free_buf (&ks->plaintext_write_buf);
  free_buf (&ks->ack_write_buf);

  if (ks->send_reliable)
    {
      reliable_free (ks->send_reliable);
      free (ks->send_reliable);
    }

  if (ks->rec_reliable)
    {
      reliable_free (ks->rec_reliable);
      free (ks->rec_reliable);
    }

  if (ks->rec_ack)
    free (ks->rec_ack);

  if (ks->key_src)
    free (ks->key_src);

  packet_id_free (&ks->packet_id);

  if (clear)
    CLEAR (*ks);
}

/*
 * Must be called if we move a tls_session in memory.
 */
static inline void tls_session_set_self_referential_pointers (struct tls_session* session) {
  session->tls_auth.packet_id = &session->tls_auth_pid;
}

/*
 * Initialize a TLS session.  A TLS session normally has 2 key_state objects,
 * one for the current key, and one for the lame duck (i.e. retiring) key.
 */
static void
tls_session_init (struct tls_multi *multi, struct tls_session *session)
{
  struct gc_arena gc = gc_new ();

  msg (D_TLS_DEBUG, "TLS: tls_session_init: entry");

  CLEAR (*session);

  /* Set options data to point to parent's option structure */
  session->opt = &multi->opt;
  
  /* Randomize session # if it is 0 */
  while (!session_id_defined(&session->session_id))
    session_id_random (&session->session_id);

  /* Are we a TLS server or client? */
  ASSERT (session->opt->key_method >= 1);
  if (session->opt->key_method == 1)
    {
      session->initial_opcode = session->opt->server ?
	P_CONTROL_HARD_RESET_SERVER_V1 : P_CONTROL_HARD_RESET_CLIENT_V1;
    }
  else /* session->opt->key_method >= 2 */
    {
      session->initial_opcode = session->opt->server ?
	P_CONTROL_HARD_RESET_SERVER_V2 : P_CONTROL_HARD_RESET_CLIENT_V2;
    }

  /* Initialize control channel authentication parameters */
  session->tls_auth = session->opt->tls_auth;

  /* Set session internal pointers (also called if session object is moved in memory) */
  tls_session_set_self_referential_pointers (session);

  /* initialize packet ID replay window for --tls-auth */
  packet_id_init (session->tls_auth.packet_id,
		  session->opt->replay_window,
		  session->opt->replay_time);

  /* load most recent packet-id to replay protect on --tls-auth */
  packet_id_persist_load_obj (session->tls_auth.pid_persist, session->tls_auth.packet_id);

  key_state_init (session, &session->key[KS_PRIMARY]);

  msg (D_TLS_DEBUG, "TLS: tls_session_init: new session object, sid=%s",
       session_id_print (&session->session_id, &gc));

  gc_free (&gc);
}

static void
tls_session_free (struct tls_session *session, bool clear)
{
  int i;

  if (session->tls_auth.packet_id)
    packet_id_free (session->tls_auth.packet_id);

  for (i = 0; i < KS_SIZE; ++i)
    key_state_free (&session->key[i], false);

  if (session->common_name)
    free (session->common_name);

  if (clear)
    CLEAR (*session);
}

static void
move_session (struct tls_multi* multi, int dest, int src, bool reinit_src)
{
  msg (D_TLS_DEBUG_LOW, "TLS: move_session: dest=%s src=%s reinit_src=%d",
       session_index_name(dest),
       session_index_name(src),
       reinit_src);
  ASSERT (src != dest);
  ASSERT (src >= 0 && src < TM_SIZE);
  ASSERT (dest >= 0 && dest < TM_SIZE);
  tls_session_free (&multi->session[dest], false);
  multi->session[dest] = multi->session[src];
  tls_session_set_self_referential_pointers (&multi->session[dest]);

  if (reinit_src)
    tls_session_init (multi, &multi->session[src]);
  else
    CLEAR (multi->session[src]);

  msg (D_TLS_DEBUG, "TLS: move_session: exit");
}

static void
reset_session (struct tls_multi *multi, struct tls_session *session)
{
  tls_session_free (session, false);
  tls_session_init (multi, session);
}

#if 0
/*
 * Transmit a TLS reset on our untrusted channel.
 */
static void
initiate_untrusted_session (struct tls_multi *multi, struct sockaddr_in *to)
{
  struct tls_session *session = &multi->session[TM_UNTRUSTED];
  struct key_state *ks = &session->key[KS_PRIMARY];

  reset_session (multi, session);
  ks->remote_addr = *to;
  msg (D_TLS_DEBUG_LOW, "TLS: initiate_untrusted_session: addr=%s", print_sockaddr (to));
}
#endif

/*
 * Used to determine in how many seconds we should be
 * called again.
 */
static inline void
compute_earliest_wakeup (interval_t *earliest, interval_t seconds_from_now) {
  if (seconds_from_now < *earliest)
    *earliest = seconds_from_now;
  if (*earliest < 0)
    *earliest = 0;
}

/*
 * Return true if "lame duck" or retiring key has expired and can
 * no longer be used.
 */
static inline bool
lame_duck_must_die (const struct tls_session* session, interval_t *wakeup)
{
  const struct key_state* lame = &session->key[KS_LAME_DUCK];
  if (lame->state >= S_INITIAL)
    {
      const time_t local_now = now;
      ASSERT (lame->must_die); /* a lame duck key must always have an expiration */
      if (local_now < lame->must_die)
	{
	  compute_earliest_wakeup (wakeup, lame->must_die - local_now);
	  return false;
	}
      else
	return true;
    }
  else if (lame->state == S_ERROR)
    return true;
  else
    return false;
}

/*
 * A tls_multi object fully encapsulates OpenVPN's TLS state.
 * See ssl.h for more comments.
 */
struct tls_multi *
tls_multi_init (struct tls_options *tls_options)
{
  struct tls_multi *ret;

  ALLOC_OBJ_CLEAR (ret, struct tls_multi);

  /* get command line derived options */
  ret->opt = *tls_options;

  /* set up pointer to HMAC object for TLS packet authentication */
  ret->opt.tls_auth.key_ctx_bi = &ret->opt.tls_auth_key;

  /* set up list of keys to be scanned by data channel encrypt and decrypt routines */
  ASSERT (SIZE (ret->key_scan) == 3);
  ret->key_scan[0] = &ret->session[TM_ACTIVE].key[KS_PRIMARY];
  ret->key_scan[1] = &ret->session[TM_ACTIVE].key[KS_LAME_DUCK];
  ret->key_scan[2] = &ret->session[TM_LAME_DUCK].key[KS_LAME_DUCK];

  return ret;
}

/*
 * Finalize our computation of frame sizes.
 */
void
tls_multi_init_finalize (struct tls_multi* multi, const struct frame* frame)
{
  tls_init_control_channel_frame_parameters(frame, &multi->opt.frame);
  
  /* initialize the active and untrusted sessions */

  tls_session_init (multi, &multi->session[TM_ACTIVE]);

  if (!multi->opt.single_session)
    tls_session_init (multi, &multi->session[TM_UNTRUSTED]);
}

/*
 * Set local and remote option compatibility strings.
 * Used to verify compatibility of local and remote option
 * sets.
 */
void
tls_multi_init_set_options (struct tls_multi* multi,
			   const char *local,
			   const char *remote)
{
  /* initialize options string */
  multi->opt.local_options = local;
  multi->opt.remote_options = remote;
}

void
tls_multi_free (struct tls_multi *multi, bool clear)
{
  int i;

  ASSERT (multi);

  for (i = 0; i < TM_SIZE; ++i)
    tls_session_free (&multi->session[i], false);

  if (clear)
    CLEAR (*multi);

  free(multi);
}

/*
 * Move a packet authentication HMAC + related fields to or from the front
 * of the buffer so it can be processed by encrypt/decrypt.
 */

/*
 * Dependent on hmac size, opcode size, and session_id size.
 * Will assert if too small.
 */
#define SWAP_BUF_SIZE 256

static bool
swap_hmac (struct buffer *buf, const struct crypto_options *co, bool incoming)
{
  struct key_ctx *ctx;

  ASSERT (co);

  ctx = (incoming ? &co->key_ctx_bi->decrypt : &co->key_ctx_bi->encrypt);
  ASSERT (ctx->hmac);

  {
    /* hmac + packet_id (8 bytes) */
    const int hmac_size = HMAC_size (ctx->hmac) + packet_id_size (true);

    /* opcode + session_id */
    const int osid_size = 1 + SID_SIZE;

    int e1, e2;
    uint8_t *b = BPTR (buf);
    uint8_t buf1[SWAP_BUF_SIZE];
    uint8_t buf2[SWAP_BUF_SIZE];

    if (incoming)
      {
	e1 = osid_size;
	e2 = hmac_size;
      }
    else
      {
	e1 = hmac_size;
	e2 = osid_size;
      }

    ASSERT (e1 <= SWAP_BUF_SIZE && e2 <= SWAP_BUF_SIZE);

    if (buf->len >= e1 + e2)
      {
	memcpy (buf1, b, e1);
	memcpy (buf2, b + e1, e2);
	memcpy (b, buf2, e2);
	memcpy (b + e2, buf1, e1);
	return true;
      }
    else
      return false;
  }
}

#undef SWAP_BUF_SIZE

/*
 * Write a control channel authentication record.
 */
static void
write_control_auth (struct tls_session *session,
		    struct key_state *ks,
		    struct buffer *buf,
		    struct sockaddr_in *to_link_addr,
		    int opcode,
		    int max_ack,
		    bool prepend_ack)
{
  uint8_t *header;
  struct buffer null = clear_buf ();

  ASSERT (addr_defined (&ks->remote_addr));
  ASSERT (reliable_ack_write
	  (ks->rec_ack, buf, &ks->session_id_remote, max_ack, prepend_ack));
  ASSERT (session_id_write_prepend (&session->session_id, buf));
  ASSERT (header = buf_prepend (buf, 1));
  *header = ks->key_id | (opcode << P_OPCODE_SHIFT);
  if (session->tls_auth.key_ctx_bi->encrypt.hmac)
    {
      /* no encryption, only write hmac */
      openvpn_encrypt (buf, null, &session->tls_auth, NULL);
      ASSERT (swap_hmac (buf, &session->tls_auth, false));
    }
  *to_link_addr = ks->remote_addr;
}

/*
 * Read a control channel authentication record.
 */
static bool
read_control_auth (struct buffer *buf,
		   const struct crypto_options *co,
		   const struct sockaddr_in *from)
{
  struct gc_arena gc = gc_new ();

  if (co->key_ctx_bi->decrypt.hmac)
    {
      struct buffer null = clear_buf ();

      /* move the hmac record to the front of the packet */
      if (!swap_hmac (buf, co, true))
	{
	  msg (D_TLS_ERRORS,
	       "TLS Error: cannot locate HMAC in incoming packet from %s",
	       print_sockaddr (from, &gc));
	  gc_free (&gc);
	  return false;
	}

      /* authenticate only (no decrypt) and remove the hmac record
         from the head of the buffer */
      openvpn_decrypt (buf, null, co, NULL);
      if (!buf->len)
	{
	  msg (D_TLS_ERRORS,
	       "TLS Error: incoming packet authentication failed from %s",
	       print_sockaddr (from, &gc));
	  gc_free (&gc);
	  return false;
	}

    }

  /* advance buffer pointer past opcode & session_id since our caller
     already read it */
  buf_advance (buf, SID_SIZE + 1);

  gc_free (&gc);
  return true;
}

/*
 * For debugging, print contents of key_source2 structure.
 */

static void
key_source_print (const struct key_source *k,
		  const char *prefix)
{
  struct gc_arena gc = gc_new ();

  msg (D_SHOW_KEY_SOURCE,
       "%s pre_master: %s",
       prefix,
       format_hex (k->pre_master, sizeof (k->pre_master), 0, &gc));
  msg (D_SHOW_KEY_SOURCE,
       "%s random1: %s",
       prefix,
       format_hex (k->random1, sizeof (k->random1), 0, &gc));
  msg (D_SHOW_KEY_SOURCE,
       "%s random2: %s",
       prefix,
       format_hex (k->random2, sizeof (k->random2), 0, &gc));

  gc_free (&gc);
}

static void
key_source2_print (const struct key_source2 *k)
{
  key_source_print (&k->client, "Client");
  key_source_print (&k->server, "Server");
}

/*
 * Use the TLS PRF function for generating data channel keys.
 * This code is taken from the OpenSSL library.
 *
 * TLS generates keys as such:
 *
 * master_secret[48] = PRF(pre_master_secret[48], "master secret",
 *                         ClientHello.random[32] + ServerHello.random[32])
 *
 * key_block[] = PRF(SecurityParameters.master_secret[48],
 *                 "key expansion",
 *                 SecurityParameters.server_random[32] +
 *                 SecurityParameters.client_random[32]);
 *
 * Notes:
 *
 * (1) key_block contains a full set of 4 keys.
 * (2) The pre-master secret is generated by the client.
 */

static void
tls1_P_hash(const EVP_MD *md,
	    const uint8_t *sec,
	    int sec_len,
	    const uint8_t *seed,
	    int seed_len,
	    uint8_t *out,
	    int olen)
{
  struct gc_arena gc = gc_new ();
  int chunk,n;
  unsigned int j;
  HMAC_CTX ctx;
  HMAC_CTX ctx_tmp;
  uint8_t A1[EVP_MAX_MD_SIZE];
  unsigned int A1_len;
  const int olen_orig = olen;
  const uint8_t *out_orig = out;
	
  msg (D_SHOW_KEY_SOURCE, "tls1_P_hash sec: %s", format_hex (sec, sec_len, 0, &gc));
  msg (D_SHOW_KEY_SOURCE, "tls1_P_hash seed: %s", format_hex (seed, seed_len, 0, &gc));

  chunk=EVP_MD_size(md);

  HMAC_CTX_init(&ctx);
  HMAC_CTX_init(&ctx_tmp);
  HMAC_Init_ex(&ctx,sec,sec_len,md, NULL);
  HMAC_Init_ex(&ctx_tmp,sec,sec_len,md, NULL);
  HMAC_Update(&ctx,seed,seed_len);
  HMAC_Final(&ctx,A1,&A1_len);

  n=0;
  for (;;)
    {
      HMAC_Init_ex(&ctx,NULL,0,NULL,NULL); /* re-init */
      HMAC_Init_ex(&ctx_tmp,NULL,0,NULL,NULL); /* re-init */
      HMAC_Update(&ctx,A1,A1_len);
      HMAC_Update(&ctx_tmp,A1,A1_len);
      HMAC_Update(&ctx,seed,seed_len);

      if (olen > chunk)
	{
	  HMAC_Final(&ctx,out,&j);
	  out+=j;
	  olen-=j;
	  HMAC_Final(&ctx_tmp,A1,&A1_len); /* calc the next A1 value */
	}
      else	/* last one */
	{
	  HMAC_Final(&ctx,A1,&A1_len);
	  memcpy(out,A1,olen);
	  break;
	}
    }
  HMAC_CTX_cleanup(&ctx);
  HMAC_CTX_cleanup(&ctx_tmp);
  CLEAR (A1);

  msg (D_SHOW_KEY_SOURCE, "tls1_P_hash out: %s", format_hex (out_orig, olen_orig, 0, &gc));
  gc_free (&gc);
}

static void
tls1_PRF(uint8_t *label,
	 int label_len,
	 const uint8_t *sec,
	 int slen,
	 uint8_t *out1,
	 int olen)
{
  struct gc_arena gc = gc_new ();
  const EVP_MD *md5 = EVP_md5();
  const EVP_MD *sha1 = EVP_sha1();
  int len,i;
  const uint8_t *S1,*S2;
  uint8_t *out2;

  out2 = (uint8_t *) gc_malloc (olen, false, &gc);

  len=slen/2;
  S1=sec;
  S2= &(sec[len]);
  len+=(slen&1); /* add for odd, make longer */

	
  tls1_P_hash(md5 ,S1,len,label,label_len,out1,olen);
  tls1_P_hash(sha1,S2,len,label,label_len,out2,olen);

  for (i=0; i<olen; i++)
    out1[i]^=out2[i];

  memset (out2, 0, olen);

  msg (D_SHOW_KEY_SOURCE, "tls1_PRF out[%d]: %s", olen, format_hex (out1, olen, 0, &gc));

  gc_free (&gc);
}

static void
openvpn_PRF (const uint8_t *secret,
	     int secret_len,
	     const char *label,
	     const uint8_t *client_seed,
	     int client_seed_len,
	     const uint8_t *server_seed,
	     int server_seed_len,
	     const struct session_id *client_sid,
	     const struct session_id *server_sid,
	     uint8_t *output,
	     int output_len)
{
  /* concatenate seed components */

  struct buffer seed = alloc_buf (strlen (label)
				  + client_seed_len
				  + server_seed_len
				  + SID_SIZE * 2);

  ASSERT (buf_write (&seed, label, strlen (label)));
  ASSERT (buf_write (&seed, client_seed, client_seed_len));
  ASSERT (buf_write (&seed, server_seed, server_seed_len));

  if (client_sid)
      ASSERT (buf_write (&seed, client_sid->id, SID_SIZE));
  if (server_sid)
      ASSERT (buf_write (&seed, server_sid->id, SID_SIZE));

  /* compute PRF */
  tls1_PRF (BPTR(&seed), BLEN(&seed), secret, secret_len, output, output_len);

  buf_clear (&seed);
  free_buf (&seed);
}

/* 
 * Using source entropy from local and remote hosts, mix into
 * master key.
 */
static bool
generate_key_expansion (struct key_ctx_bi *key,
			const struct key_type *key_type,
			const struct key_source2 *key_src,
			const struct session_id *client_sid,
			const struct session_id *server_sid,
			bool server)
{
  uint8_t master[48];
  struct key2 key2;
  bool ret = false;
  int i;

  CLEAR (master);
  CLEAR (key2);

  /* debugging print of source key material */
  key_source2_print (key_src);

  /* compute master secret */
  openvpn_PRF (key_src->client.pre_master,
	       sizeof(key_src->client.pre_master),
	       PACKAGE_NAME " master secret",
	       key_src->client.random1,
	       sizeof(key_src->client.random1),
	       key_src->server.random1,
	       sizeof(key_src->server.random1),
	       NULL,
	       NULL,
	       master,
	       sizeof(master));
  
  /* compute key expansion */
  openvpn_PRF (master,
	       sizeof(master),
	       PACKAGE_NAME " key expansion",
	       key_src->client.random2,
	       sizeof(key_src->client.random2),
	       key_src->server.random2,
	       sizeof(key_src->server.random2),
	       client_sid,
	       server_sid,
	       (uint8_t*)key2.keys,
	       sizeof(key2.keys));

  key2.n = 2;

  key2_print (&key2, key_type, "Master Encrypt", "Master Decrypt");

  /* check for weak keys */
  for (i = 0; i < 2; ++i)
    {
      fixup_key (&key2.keys[i], key_type);
      if (!check_key (&key2.keys[i], key_type))
	{
	  msg (D_TLS_ERRORS, "TLS Error: Bad dynamic key generated");
	  goto exit;
	}
    }

  /* Initialize OpenSSL key contexts */

  ASSERT (server == true || server == false);

  init_key_ctx (&key->encrypt,
		&key2.keys[(int)server],
		key_type,
		DO_ENCRYPT,
		"Data Channel Encrypt");

  init_key_ctx (&key->decrypt,
		&key2.keys[1-(int)server],
		key_type,
		DO_DECRYPT,
		"Data Channel Decrypt");

  ret = true;

 exit:
  CLEAR (master);
  CLEAR (key2);

  return ret;
}

static void
random_bytes_to_buf (struct buffer *buf,
		     uint8_t *out,
		     int outlen)
{
  if (!RAND_bytes (out, outlen))
    msg (M_FATAL, "ERROR: Random number generator cannot obtain entropy for key generation [SSL]");
  ASSERT (buf_write (buf, out, outlen));
}

static void
key_source2_randomize_write (struct key_source2 *k2,
			     struct buffer *buf,
			     bool server)
{
  struct key_source *k = &k2->client;
  if (server)
    k = &k2->server;

  CLEAR (*k);

  if (!server)
    random_bytes_to_buf (buf, k->pre_master, sizeof (k->pre_master));

  random_bytes_to_buf (buf, k->random1, sizeof (k->random1));
  random_bytes_to_buf (buf, k->random2, sizeof (k->random2));
}

static int
key_source2_read (struct key_source2 *k2,
		  struct buffer *buf,
		  bool server)
{
  struct key_source *k = &k2->client;

  if (!server)
    k = &k2->server;

  CLEAR (*k);

  if (server)
    {
      if (!buf_read (buf, k->pre_master, sizeof (k->pre_master)))
	return 0;
    }

  if (!buf_read (buf, k->random1, sizeof (k->random1)))
    return 0;
  if (!buf_read (buf, k->random2, sizeof (k->random2)))
    return 0;

  return 1;
}

/*
 * Macros for key_state_soft_reset & tls_process
 */
#define ks      (&session->key[KS_PRIMARY])	/* primary key */
#define ks_lame (&session->key[KS_LAME_DUCK])	/* retiring key */

/* true if no in/out acknowledgements pending */
#define FULL_SYNC \
  (reliable_empty(ks->send_reliable) && reliable_ack_empty (ks->rec_ack))

/*
 * Move the active key to the lame duck key and reinitialize the
 * active key.
 */
static void
key_state_soft_reset (struct tls_session *session)
{
  ks->must_die = now + session->opt->transition_window; /* remaining lifetime of old key */
  key_state_free (ks_lame, false);
  *ks_lame = *ks;

  key_state_init (session, ks);
  ks->session_id_remote = ks_lame->session_id_remote;
  ks->remote_addr = ks_lame->remote_addr;
}

/*
 * This is the primary routine for processing TLS stuff inside the
 * the main event loop (see openvpn.c).  When this routine exits
 * with non-error status, it will set *wakeup to the number of seconds
 * when it wants to be called again.
 *
 * Return value is true if we have placed a packet in *to_link which we
 * want to send to our peer.
 */
static bool
tls_process (struct tls_multi *multi,
	     struct tls_session *session,
	     struct buffer *to_link,
	     struct sockaddr_in *to_link_addr,
	     struct link_socket_info *to_link_socket_info,
	     interval_t *wakeup)
{
  struct gc_arena gc = gc_new ();
  struct buffer *buf;
  bool state_change = false;
  bool active = false;

  /* Make sure we were initialized and that we're not in an error state */
  ASSERT (ks->state != S_UNDEF);
  ASSERT (ks->state != S_ERROR);
  ASSERT (session_id_defined (&session->session_id));

  /* Should we trigger a soft reset? -- new key, keeps old key for a while */
  if (ks->state >= S_ACTIVE &&
      ((session->opt->renegotiate_seconds
	&& now >= ks->established + session->opt->renegotiate_seconds)
       || (session->opt->renegotiate_bytes
	   && ks->n_bytes >= session->opt->renegotiate_bytes)
       || (session->opt->renegotiate_packets
	   && ks->n_packets >= session->opt->renegotiate_packets)
       || (packet_id_close_to_wrapping (&ks->packet_id.send))))
    {
      msg (D_TLS_DEBUG_LOW, "TLS: soft reset sec=%d bytes=%d/%d pkts=%d/%d",
	   (int)(ks->established + session->opt->renegotiate_seconds - now),
	   ks->n_bytes, session->opt->renegotiate_bytes,
	   ks->n_packets, session->opt->renegotiate_packets);
      key_state_soft_reset (session);
    }

  /* Kill lame duck key transition_window seconds after primary key negotiation */
  if (lame_duck_must_die (session, wakeup)) {
	key_state_free (ks_lame, true);
	msg (D_TLS_DEBUG_LOW, "TLS: tls_process: killed expiring key");
  }

  //mutex_cycle (multi->mutex);

  do
    {
      update_time ();

      msg (D_TLS_DEBUG, "TLS: tls_process: chg=%d ks=%s lame=%s to_link->len=%d wakeup=%d",
	   state_change,
	   state_name (ks->state),
	   state_name (ks_lame->state),
	   to_link->len,
	   *wakeup);

      state_change = false;

      /*
       * TLS activity is finished once we get to S_ACTIVE,
       * though we will still process acknowledgements.
       *
       * CHANGED with 2.0 -> now we may send tunnel configuration
       * info over the control channel.
       */
      if (true)
	{
	  /* Initial handshake */
	  if (ks->state == S_INITIAL)
	    {
	      buf = reliable_get_buf_output_sequenced (ks->send_reliable);
	      if (buf)
		{
		  ks->must_negotiate = now + session->opt->handshake_window;

		  /* null buffer */
		  reliable_mark_active_outgoing (ks->send_reliable, buf, ks->initial_opcode);
		  INCR_GENERATED;
	      
		  ks->state = S_PRE_START;
		  state_change = true;
		  msg (D_TLS_DEBUG, "TLS: Initial Handshake, sid=%s",
		       session_id_print (&session->session_id, &gc));
		}
	    }

	  /* Are we timed out on receive? */
	  if (now >= ks->must_negotiate)
	    {
	      if (ks->state < S_ACTIVE)
		{
		  msg (D_TLS_ERRORS,
		       "TLS Error: TLS key negotiation failed to occur within %d seconds",
		       session->opt->handshake_window);
		  goto error;
		}
	      else /* assume that ks->state == S_ACTIVE */
		{
		  msg (D_TLS_DEBUG_MED, "STATE S_NORMAL");
		  ks->state = S_NORMAL;
		  ks->must_negotiate = 0;
		}
	    }

	  /* Wait for Initial Handshake ACK */
	  if (ks->state == S_PRE_START && FULL_SYNC)
	    {
	      ks->state = S_START;
	      state_change = true;
	      msg (D_TLS_DEBUG_MED, "STATE S_START");
	    }

	  /* Wait for ACK */
	  if (((ks->state == S_GOT_KEY && !session->opt->server) ||
	       (ks->state == S_SENT_KEY && session->opt->server)))
	    {
	      if (FULL_SYNC)
		{
		  ks->established = now;
		  msg (D_TLS_DEBUG_MED, "STATE S_ACTIVE");
		  if (check_debug_level (D_HANDSHAKE))
		    print_details (ks->ssl, "Control Channel:");
		  state_change = true;
		  ks->state = S_ACTIVE;
		  INCR_SUCCESS;

		  /* Set outgoing address for data channel packets */
		  link_socket_set_outgoing_addr (NULL, to_link_socket_info, &ks->remote_addr, session->common_name);

#ifdef MEASURE_TLS_HANDSHAKE_STATS
		  show_tls_performance_stats();
#endif
		}
	    }

	  /* Reliable buffer to outgoing TCP/UDP (send up to CONTROL_SEND_ACK_MAX ACKs
	     for previously received packets) */
	  if (!to_link->len && reliable_can_send (ks->send_reliable))
	    {
	      int opcode;
	      struct buffer b;

	      buf = reliable_send (ks->send_reliable, &opcode);
	      ASSERT (buf);
	      b = *buf;
	      INCR_SENT;

	      write_control_auth (session, ks, &b, to_link_addr, opcode,
				  CONTROL_SEND_ACK_MAX, true);
	      *to_link = b;
	      active = true;
	      state_change = true;
	      msg (D_TLS_DEBUG, "Reliable -> TCP/UDP");
	      break;
	    }

#ifndef TLS_AGGREGATE_ACK
	  /* Send 1 or more ACKs (each received control packet gets one ACK) */
	  if (!to_link->len && !reliable_ack_empty (ks->rec_ack))
	    {
	      buf = &ks->ack_write_buf;
	      ASSERT (buf_init (buf, FRAME_HEADROOM (&multi->opt.frame)));
	      write_control_auth (session, ks, buf, to_link_addr, P_ACK_V1,
				  RELIABLE_ACK_SIZE, false);
	      *to_link = *buf;
	      active = true;
	      state_change = true;
	      msg (D_TLS_DEBUG, "Dedicated ACK -> TCP/UDP");
	      break;
	    }
#endif

	  /* Write incoming ciphertext to TLS object */
	  buf = reliable_get_buf_sequenced (ks->rec_reliable);
	  if (buf)
	    {
	      int status = 0;
	      if (buf->len)
		{
		  status = key_state_write_ciphertext (multi, ks, buf);
		  if (status == -1)
		    {
		      msg (D_TLS_ERRORS,
			   "TLS Error: Incoming Ciphertext -> TLS object write error");
		      goto error;
		    }
		}
	      else
		{
		  status = 1;
		}
	      if (status == 1)
		{
		  reliable_mark_deleted (ks->rec_reliable, buf, true);
		  state_change = true;
		  msg (D_TLS_DEBUG, "Incoming Ciphertext -> TLS");
		}
	    }

	  /* Read incoming plaintext from TLS object */
	  buf = &ks->plaintext_read_buf;
	  if (!buf->len)
	    {
	      int status;

	      ASSERT (buf_init (buf, 0));
	      status = key_state_read_plaintext (multi, ks, buf, PLAINTEXT_BUFFER_SIZE);
	      update_time ();
	      if (status == -1)
		{
		  msg (D_TLS_ERRORS, "TLS Error: TLS object -> incoming plaintext read error");
		  goto error;
		}
	      if (status == 1)
		{
		  state_change = true;
		  msg (D_TLS_DEBUG, "TLS -> Incoming Plaintext");
		}
	    }

	  /* Send Key */
	  buf = &ks->plaintext_write_buf;
	  if (!buf->len && ((ks->state == S_START && !session->opt->server) ||
			    (ks->state == S_GOT_KEY && session->opt->server)))
	    {
	      const int optlen = strlen (session->opt->local_options) + 1;
	      if (session->opt->key_method == 1)
		{
		  struct key key;
		  ASSERT (buf_init (buf, 0));
		  generate_key_random (&key, &session->opt->key_type);
		  if (!check_key (&key, &session->opt->key_type))
		    {
		      msg (D_TLS_ERRORS, "TLS Error: Bad encrypting key generated");
		      goto error;
		    }
		  write_key (&key, &session->opt->key_type, buf);
		  init_key_ctx (&ks->key.encrypt, &key, &session->opt->key_type,
				DO_ENCRYPT, "Data Channel Encrypt");
		  CLEAR (key);

		  /* send local options string */
		  ASSERT (buf_write (buf, session->opt->local_options, optlen));
		}
	      else
		{
		  ASSERT (session->opt->key_method == 2);
		  ASSERT (buf_init (buf, 0));

		  /* write a uin32 0 */
		  ASSERT (buf_write_u32 (buf, 0));

		  /* write key_method + flags */
		  ASSERT (buf_write_u8 (buf, (session->opt->key_method & KEY_METHOD_MASK)
					| (session->opt->pass_config_info ? TLS_PASS_CONFIG_INFO : 0)));

		  /* write key source material */
		  key_source2_randomize_write (ks->key_src, buf, session->opt->server);

		  /* write options string */
		  ASSERT (optlen >= 0 && optlen < 65536);
		  ASSERT (buf_write_u16 (buf, optlen));
		  ASSERT (buf_write (buf, session->opt->local_options, optlen));

		  if (session->opt->server)
		    {
		      if (!generate_key_expansion (&ks->key,
						   &session->opt->key_type,
						   ks->key_src,
						   &ks->session_id_remote,
						   &session->session_id,
						   true))
			goto error;
		      
		      CLEAR (*ks->key_src);
		    }
		}

	      state_change = true;
	      msg (D_TLS_DEBUG_MED, "STATE S_SENT_KEY");
	      ks->state = S_SENT_KEY;
	    }

	  /* Receive Key */
	  buf = &ks->plaintext_read_buf;
	  if (buf->len
	      && ((ks->state == S_SENT_KEY && !session->opt->server)
		  || (ks->state == S_START && session->opt->server)))
	    {
	      if (session->opt->key_method == 1)
		{
		  int status;
		  struct key key;

		  status = read_key (&key, &session->opt->key_type, buf);
		  if (status == -1)
		    {
		      msg (D_TLS_ERRORS,
			   "TLS Error: Error reading data channel key from plaintext buffer");
		      goto error;
		    }

		  if (!check_key (&key, &session->opt->key_type))
		    {
		      msg (D_TLS_ERRORS, "TLS Error: Bad decrypting key received from peer");
		      goto error;
		    }

		  ASSERT (buf->len > 0);

		  /* compare received remote options string
		     with our locally computed options string */
		  if (!session->opt->disable_occ &&
		      !options_cmp_equal (BPTR (buf),
					  session->opt->remote_options, buf->len))
		    {
		      options_warning (BPTR (buf),
				       session->opt->remote_options, buf->len);
		    }
		  buf_clear (buf);

		  if (status == 1)
		    {
		      init_key_ctx (&ks->key.decrypt, &key, &session->opt->key_type,
				    DO_DECRYPT, "Data Channel Decrypt");
		    }
		  CLEAR (key);

		  if (status == 0)
		    goto error;
		}
	      else
		{
		  int key_method_flags;
		  int status;
		  int optlen;

		  ASSERT (session->opt->key_method >= 2);
		  status = 0;
		  
		  /* discard leading uint32 */
		  ASSERT (buf_advance (buf, 4));

		  /* get key method */
		  key_method_flags = buf_read_u8 (buf);
		  if ((key_method_flags & KEY_METHOD_MASK) != 2)
		    {
		      msg (D_TLS_ERRORS,
			   "TLS ERROR: Unknown key_method/flags=%d received from remote host",
			   key_method_flags);
		      goto error;
		    }

		  /* verify TLS_PASS_CONFIG_INFO consistency between peers */
		  if (((key_method_flags & TLS_PASS_CONFIG_INFO) != 0) 
		      ^ (session->opt->pass_config_info == true))
		    {
		      msg (D_TLS_ERRORS, "TLS ERROR: Inconsistent options between peers regarding configuration info exchange over the control channel");
		      goto error;
		    }
		  
		  /* get key source material (not actual keys yet) */
		  if (!key_source2_read (ks->key_src, buf, session->opt->server))
		    {
		      msg (D_TLS_ERRORS, "TLS Error: Error reading remote data channel key source entropy from plaintext buffer");
		      goto error;
		    }

		  /* get options */
		  optlen = buf_read_u16 (buf);
		  if (optlen < 0 || optlen >= 65536)
		    {
		      msg (D_TLS_ERRORS, "TLS Error: Bad options string length: %d", optlen);
		      goto error;
		    }

		  if (BLEN(buf) < optlen)
		    {
		      msg (D_TLS_ERRORS, "TLS Error: Options string truncation");
		      goto error;
		    }

		  /* check options consistency */

		  if (!session->opt->disable_occ &&
		      !options_cmp_equal (BPTR (buf),
					  session->opt->remote_options, buf->len))
		    {
		      options_warning (BPTR (buf),
				       session->opt->remote_options, buf->len);
		    }

		  buf_clear (buf);

		  if (!session->opt->server)
		    {
		      if (!generate_key_expansion (&ks->key,
						   &session->opt->key_type,
						   ks->key_src,
						   &session->session_id,
						   &ks->session_id_remote,
						   false))
			goto error;
		      
		      CLEAR (*ks->key_src);
		    }
		}

	      state_change = true;
	      msg (D_TLS_DEBUG_MED, "STATE S_GOT_KEY");
	      ks->state = S_GOT_KEY;
	    }

	  /* Write outgoing plaintext to TLS object */
	  buf = &ks->plaintext_write_buf;
	  if (buf->len)
	    {
	      int status = key_state_write_plaintext (multi, ks, buf);
	      if (status == -1)
		{
		  msg (D_TLS_ERRORS,
		       "TLS ERROR: Outgoing Plaintext -> TLS object write error");
		  goto error;
		}
	      if (status == 1)
		{
		  state_change = true;
		  msg (D_TLS_DEBUG, "Outgoing Plaintext -> TLS");
		}
	    }

	  /* Outgoing Ciphertext to reliable buffer */
	  if (ks->state >= S_START)
	    {
	      buf = reliable_get_buf_output_sequenced (ks->send_reliable);
	      if (buf)
		{
		  int status = key_state_read_ciphertext (multi, ks, buf, PAYLOAD_SIZE_DYNAMIC (&multi->opt.frame));
		  if (status == -1)
		    {
		      msg (D_TLS_ERRORS,
			   "TLS Error: Ciphertext -> reliable TCP/UDP transport read error");
		      goto error;
		    }
		  if (status == 1)
		    {
		      reliable_mark_active_outgoing (ks->send_reliable, buf, P_CONTROL_V1);
		      INCR_GENERATED;
		      state_change = true;
		      msg (D_TLS_DEBUG, "Outgoing Ciphertext -> Reliable");
		    }
		}
	    }
	}
      //mutex_cycle (multi->mutex);
    }
  while (state_change);

  update_time ();

#ifdef TLS_AGGREGATE_ACK
  /* Send 1 or more ACKs (each received control packet gets one ACK) */
  if (!to_link->len && !reliable_ack_empty (ks->rec_ack))
    {
      buf = &ks->ack_write_buf;
      ASSERT (buf_init (buf, FRAME_HEADROOM (&multi->opt.frame)));
      write_control_auth (session, ks, buf, to_link_addr, P_ACK_V1,
			  RELIABLE_ACK_SIZE, false);
      *to_link = *buf;
      active = true;
      state_change = true;
      msg (D_TLS_DEBUG, "Dedicated ACK -> TCP/UDP");
    }
#endif

  /* When should we wake up again? */
  {
    if (ks->state >= S_INITIAL)
      {
	compute_earliest_wakeup (wakeup,
	  reliable_send_timeout (ks->send_reliable));
	
	if (ks->must_negotiate)
	  compute_earliest_wakeup (wakeup, ks->must_negotiate - now);
      }

    if (ks->established && session->opt->renegotiate_seconds)
      compute_earliest_wakeup (wakeup,
        ks->established + session->opt->renegotiate_seconds - now);

    /* prevent event-loop spinning by setting minimum wakeup of 1 second */
    if (*wakeup <= 0)
      {
	*wakeup = 1;

	/* if we had something to send to remote, but to_link was busy,
	   let caller know we need to be called again soon */
	active = true;
      }

    msg (D_TLS_DEBUG, "TLS: tls_process: timeout set to %d", *wakeup);

    gc_free (&gc);
    return active;
  }

error:
  ks->state = S_ERROR;
  msg (D_TLS_ERRORS, "TLS Error: TLS handshake failed");
  INCR_ERROR;
  gc_free (&gc);
  return false;
}

#undef ks
#undef ks_lame

/*
 * Called at the top of the event loop in openvpn.c.
 *
 * Basically decides if we should call tls_process for
 * the active or untrusted sessions.
 */

bool
tls_multi_process (struct tls_multi *multi,
		   struct buffer *to_link,
		   struct sockaddr_in *to_link_addr,
		   struct link_socket_info *to_link_socket_info,
		   interval_t *wakeup)
{
  struct gc_arena gc = gc_new ();
  int i;
  bool active = false;

  /*
   * Process each session object having state of S_INITIAL or greater,
   * and which has a defined remote IP addr.
   */

  for (i = 0; i < TM_SIZE; ++i)
    {
      struct tls_session *session = &multi->session[i];
      struct key_state *ks = &session->key[KS_PRIMARY];
      struct key_state *ks_lame = &session->key[KS_LAME_DUCK];

      /* set initial remote address */
      if (i == TM_ACTIVE && ks->state == S_INITIAL &&
	  addr_defined (&to_link_socket_info->lsa->actual))
	ks->remote_addr = to_link_socket_info->lsa->actual;

      msg (D_TLS_DEBUG,
	   "TLS: tls_multi_process: i=%d state=%s, mysid=%s, stored-sid=%s, stored-ip=%s",
	   i,
	   state_name (ks->state),
	   session_id_print (&session->session_id, &gc),
	   session_id_print (&ks->session_id_remote, &gc),
	   print_sockaddr (&ks->remote_addr, &gc));

      if (ks->state >= S_INITIAL && addr_defined (&ks->remote_addr))
	{
	  update_time ();

	  if (tls_process (multi, session, to_link, to_link_addr,
			   to_link_socket_info, wakeup))
	    active = true;

	  /*
	   * If tls_process hits an error:
	   * (1) If the session has an unexpired lame duck key, preserve it.
	   * (2) Reinitialize the session.
	   */
	  if (ks->state == S_ERROR)
	    {
	      ++multi->n_errors;
	      if (i == TM_ACTIVE
		  && ks_lame->state >= S_ACTIVE
		  && !multi->opt.single_session)
		move_session (multi, TM_LAME_DUCK, TM_ACTIVE, true);
	      else
		reset_session (multi, session);
	    }
	}
      //mutex_cycle (multi->mutex);
    }

  update_time ();

  /*
   * If lame duck session expires, kill it.
   */
  if (lame_duck_must_die (&multi->session[TM_LAME_DUCK], wakeup)) {
    tls_session_free (&multi->session[TM_LAME_DUCK], true);
    msg (D_TLS_DEBUG_LOW, "TLS: tls_multi_process: killed expiring key");
  }

  /*
   * If untrusted session achieves TLS authentication,
   * move it to active session, usurping any prior session.
   */
  if (DECRYPT_KEY_ENABLED (multi, &multi->session[TM_UNTRUSTED].key[KS_PRIMARY])) {
    move_session (multi, TM_ACTIVE, TM_UNTRUSTED, true);
    msg (D_TLS_DEBUG_LOW, "TLS: tls_multi_process: untrusted session promoted to trusted");
  }

  gc_free (&gc);
  return active;
}

/*
 * Send a payload over the TLS control channel.
 * Called externally.
 */

bool
tls_send_payload (struct tls_multi *multi,
		  const struct buffer *buf)
{
  struct tls_session *session;
  struct key_state *ks;
  bool ret = false;

  ASSERT (multi);

  session = &multi->session[TM_ACTIVE];
  ks = &session->key[KS_PRIMARY];

  if (ks->state >= S_ACTIVE && !BLEN (&ks->plaintext_write_buf))
    {
      if (buf_copy (&ks->plaintext_write_buf, buf))
	ret = true;
    }

  return ret;
}

bool
tls_rec_payload (struct tls_multi *multi,
		 struct buffer *buf)
{
  struct tls_session *session;
  struct key_state *ks;
  bool ret = false;

  ASSERT (multi);

  session = &multi->session[TM_ACTIVE];
  ks = &session->key[KS_PRIMARY];

  if (ks->state >= S_ACTIVE && BLEN (&ks->plaintext_read_buf))
    {
      if (buf_copy (buf, &ks->plaintext_read_buf))
	ret = true;
      ks->plaintext_read_buf.len = 0;
    }

  return ret;
}

/*
 * Pre and post-process the encryption & decryption buffers in order
 * to implement a multiplexed TLS channel over the TCP/UDP port.
 */

/*
 *
 * When we are in TLS mode, this is the first routine which sees
 * an incoming packet.
 *
 * If it's a data packet, we set opt so that our caller can
 * decrypt it.  We also give our caller the appropriate decryption key.
 *
 * If it's a control packet, we authenticate it and process it,
 * possibly creating a new tls_session if it represents the
 * first packet of a new session.  For control packets, we will
 * also zero the size of *buf so that our caller ignores the
 * packet on our return.
 *
 * Note that openvpn only allows one active session at a time,
 * so a new session (once authenticated) will always usurp
 * an old session.
 *
 * Return true if input was an authenticated control channel
 * packet.
 *
 * If we are running in TLS thread mode, all public routines
 * below this point must be called with the L_TLS lock held.
 */

bool
tls_pre_decrypt (struct tls_multi *multi,
		 struct sockaddr_in *from,
		 struct buffer *buf,
		 struct crypto_options *opt)
{
  struct gc_arena gc = gc_new ();
  bool ret = false;

  if (buf->len > 0)
    {
      int i;
      int op;
      int key_id;

      /* get opcode and key ID */
      {
	uint8_t c = *BPTR (buf);
	op = c >> P_OPCODE_SHIFT;
	key_id = c & P_KEY_ID_MASK;
      }

      if (op == P_DATA_V1)
	{			/* data channel packet */
	  for (i = 0; i < KEY_SCAN_SIZE; ++i)
	    {
	      struct key_state *ks = multi->key_scan[i];
	      if (DECRYPT_KEY_ENABLED (multi, ks)
		  && key_id == ks->key_id
		  && addr_port_match(from, &ks->remote_addr))
		{
		  /* return appropriate data channel decrypt key in opt */
		  opt->key_ctx_bi = &ks->key;
		  opt->packet_id = multi->opt.replay ? &ks->packet_id : NULL;
		  opt->pid_persist = NULL;
		  opt->packet_id_long_form = multi->opt.packet_id_long_form;
		  ASSERT (buf_advance (buf, 1));
		  ++ks->n_packets;
		  ks->n_bytes += buf->len;
		  msg (D_TLS_DEBUG,
		       "TLS: data channel, key_id=%d, IP=%s",
		       key_id, print_sockaddr (from, &gc));
		  gc_free (&gc);
		  return ret;
		}
	    }

	  msg (D_TLS_ERRORS,
	       "TLS Error: Unknown data channel key ID or IP address received from %s: %d (see FAQ for more info on this error)",
	       print_sockaddr (from, &gc), key_id);
	  goto error;
	}
      else			  /* control channel packet */
	{
	  bool do_burst = false;
	  bool new_link = false;
	  struct session_id sid;  /* remote session ID */

	  /* verify legal opcode */
	  if (op < P_FIRST_OPCODE || op > P_LAST_OPCODE)
	    {
	      msg (D_TLS_ERRORS,
		   "TLS Error: unknown opcode received from %s op=%d",
		   print_sockaddr (from, &gc), op);
	      goto error;
	    }

	  /* hard reset ? */
	  if (is_hard_reset (op, 0))
	    {
	      /* verify client -> server or server -> client connection */
	      if (((op == P_CONTROL_HARD_RESET_CLIENT_V1
		    || op == P_CONTROL_HARD_RESET_CLIENT_V2) && !multi->opt.server)
		  || ((op == P_CONTROL_HARD_RESET_SERVER_V1
		       || op == P_CONTROL_HARD_RESET_SERVER_V2) && multi->opt.server))
		{
		  msg (D_TLS_ERRORS,
		       "TLS Error: client->client or server->server connection attempted from %s",
		       print_sockaddr (from, &gc));
		  goto error;
		}
	    }

	  /*
	   * Authenticate Packet
	   */
	  msg (D_TLS_DEBUG, "TLS: control channel, op=%s, IP=%s",
	       packet_opcode_name (op), print_sockaddr (from, &gc));

	  /* get remote session-id */
	  {
	    struct buffer tmp = *buf;
	    buf_advance (&tmp, 1);
	    if (!session_id_read (&sid, &tmp) || !session_id_defined (&sid))
	      {
		msg (D_TLS_ERRORS,
		     "TLS Error: session-id not found in packet from %s",
		     print_sockaddr (from, &gc));
		goto error;
	      }
	  }

	  /* use session ID to match up packet with appropriate tls_session object */
	  for (i = 0; i < TM_SIZE; ++i)
	    {
	      struct tls_session *session = &multi->session[i];
	      struct key_state *ks = &session->key[KS_PRIMARY];

	      msg (D_TLS_DEBUG,
		   "TLS: initial packet test, i=%d state=%s, mysid=%s, rec-sid=%s, rec-ip=%s, stored-sid=%s, stored-ip=%s",
		   i,
		   state_name (ks->state),
		   session_id_print (&session->session_id, &gc),
		   session_id_print (&sid, &gc),
		   print_sockaddr (from, &gc),
		   session_id_print (&ks->session_id_remote, &gc),
		   print_sockaddr (&ks->remote_addr, &gc));

	      if (session_id_equal (&ks->session_id_remote, &sid))
		/* found a match */
		{
		  if (i == TM_LAME_DUCK) {
		    msg (D_TLS_ERRORS,
			 "TLS ERROR: received control packet with stale session-id=%s",
			 session_id_print (&sid, &gc));
		    goto error;
		  }
		  msg (D_TLS_DEBUG,
		       "TLS: found match, session[%d], sid=%s",
		       i, session_id_print (&sid, &gc));
		  break;
		}
	    }

	  /*
	   * Initial packet received.
	   */

	  if (i == TM_SIZE && is_hard_reset (op, 0))
	    {
	      struct tls_session *session = &multi->session[TM_ACTIVE];
	      struct key_state *ks = &session->key[KS_PRIMARY];

	      if (!is_hard_reset (op, multi->opt.key_method))
		{
		  msg (D_TLS_ERRORS, "TLS ERROR: initial packet local/remote key_method mismatch, local key_method=%d, op=%s",
		       multi->opt.key_method,
		       packet_opcode_name (op));
		  goto error;
		}

	      /*
	       * If we have no session currently in progress, the initial packet will
	       * open a new session in TM_ACTIVE rather than TM_UNTRUSTED.
	       */
	      if (!session_id_defined (&ks->session_id_remote))
		{
		  if (multi->opt.single_session && multi->n_sessions)
		    {
		      msg (D_TLS_ERRORS,
			   "TLS Error: Cannot accept new session request from %s due to --single-session [1]",
			   print_sockaddr (from, &gc));
		      goto error;
		    }

		  msg (D_TLS_DEBUG_LOW,
		       "TLS: Initial packet from %s, sid=%s",
		       print_sockaddr (from, &gc),
		       session_id_print (&sid, &gc));
		  do_burst = true;
		  new_link = true;
		  i = TM_ACTIVE;
		  session->untrusted_sockaddr = *from;
		}
	    }

	  if (i == TM_SIZE && is_hard_reset (op, 0))
	    {
	      /*
	       * No match with existing sessions,
	       * probably a new session.
	       */
	      struct tls_session *session = &multi->session[TM_UNTRUSTED];

	      /*
	       * If --single-session, don't allow any hard-reset connection request
	       * unless it the the first packet of the session.
	       */
	      if (multi->opt.single_session)
		{
		  msg (D_TLS_ERRORS,
		       "TLS Error: Cannot accept new session request from %s due to --single-session [2]",
		       print_sockaddr (from, &gc));
		  goto error;
		}
	      
	      if (!is_hard_reset (op, multi->opt.key_method))
		{
		  msg (D_TLS_ERRORS, "TLS ERROR: new session local/remote key_method mismatch, local key_method=%d, op=%s",
		       multi->opt.key_method,
		       packet_opcode_name (op));
		  goto error;
		}

	      if (!read_control_auth (buf, &session->tls_auth, from))
		goto error;

	      /*
	       * New session-initiating control packet is authenticated at this point,
	       * assuming that the --tls-auth command line option was used.
	       *
	       * Without --tls-auth, we leave authentication entirely up to TLS.
	       */
	      msg (D_TLS_DEBUG_LOW,
		   "TLS: new session incoming connection from %s",
		   print_sockaddr (from, &gc));

	      new_link = true;
	      i = TM_UNTRUSTED;
	      session->untrusted_sockaddr = *from;
	    }
	  else
	    {
	      struct tls_session *session = &multi->session[i];
	      struct key_state *ks = &session->key[KS_PRIMARY];

	      /*
	       * Packet must belong to an existing session.
	       */
	      if (i != TM_ACTIVE && i != TM_UNTRUSTED)
		{
		  msg (D_TLS_ERRORS,
		       "TLS Error: Unroutable control packet received from %s (si=%d op=%s)",
		       print_sockaddr (from, &gc),
		       i,
		       packet_opcode_name (op));
		  goto error;
		}

	      /*
	       * Verify remote IP address
	       */
	      if (!new_link && !addr_port_match (&ks->remote_addr, from))
		{
		  msg (D_TLS_ERRORS, "TLS Error: Received control packet from unexpected IP addr: %s",
		      print_sockaddr (from, &gc));
		  goto error;
		}

	      /*
	       * Remote is requesting a key renegotiation
	       */
	      if (op == P_CONTROL_SOFT_RESET_V1
		  && DECRYPT_KEY_ENABLED (multi, ks))
		{
		  if (!read_control_auth (buf, &session->tls_auth, from))
		    goto error;

		  key_state_soft_reset (session);

		  msg (D_TLS_DEBUG,
		       "TLS: received P_CONTROL_SOFT_RESET_V1 s=%d sid=%s",
		       i, session_id_print (&sid, &gc));
		}
	      else
		{
		  /*
		   * Remote responding to our key renegotiation request?
		   */
		  if (op == P_CONTROL_SOFT_RESET_V1)
		    do_burst = true;

		  if (!read_control_auth (buf, &session->tls_auth, from))
		    goto error;

		  msg (D_TLS_DEBUG,
		       "TLS: received control channel packet s#=%d sid=%s",
		       i, session_id_print (&sid, &gc));
		}
	    }
	  
	  /*
	   * We have an authenticated packet (if --tls-auth was set).
           * Now pass to our reliability level which deals with
	   * packet acknowledgements, retransmits, sequencing, etc.
	   */
	  {
	    struct tls_session *session = &multi->session[i];
	    struct key_state *ks = &session->key[KS_PRIMARY];

	    /* Make sure we were initialized and that we're not in an error state */
	    ASSERT (ks->state != S_UNDEF);
	    ASSERT (ks->state != S_ERROR);
	    ASSERT (session_id_defined (&session->session_id));

	    /* Let our caller know we processed a control channel packet */
	    ret = true;

	    /*
	     * Set our remote address and remote session_id
	     */
	    if (new_link)
	      {
		ks->session_id_remote = sid;
		ks->remote_addr = *from;
		++multi->n_sessions;
	      }
	    else if (!addr_port_match (&ks->remote_addr, from))
	      {
		msg (D_TLS_ERRORS,
		     "TLS Error: Existing session control channel packet from unknown IP address: %s",
		     print_sockaddr (from, &gc));
		goto error;
	      }

	    /*
	     * Should we do a retransmit of all unacknowledged packets in
	     * the send buffer?  This improves the start-up efficiency of the
	     * initial key negotiation after the 2nd peer comes online.
	     */
	    if (do_burst && !session->burst)
	      {
		reliable_schedule_now (ks->send_reliable);
		session->burst = true;
	      }

	    /* Check key_id */
	    if (ks->key_id != key_id)
	      {
		msg (D_TLS_ERRORS,
		     "TLS ERROR: local/remote key IDs out of sync (%d/%d) ID: %s",
		     ks->key_id, key_id, print_key_id (multi, &gc));
		goto error;
	      }
	      
	    /*
	     * Process incoming ACKs for packets we can now
	     * delete from reliable send buffer
	     */
	    {
	      /* buffers all packet IDs to delete from send_reliable */
	      struct reliable_ack send_ack;

	      send_ack.len = 0;
	      if (!reliable_ack_read (&send_ack, buf, &session->session_id))
		{
		  msg (D_TLS_ERRORS,
		       "TLS Error: reading acknowledgement record from packet");
		  goto error;
		}
	      reliable_send_purge (ks->send_reliable, &send_ack);
	    }

	    if (op != P_ACK_V1 && reliable_can_get (ks->rec_reliable))
	      {
		packet_id_type id;

		/* Extract the packet ID from the packet */
		if (reliable_ack_read_packet_id (buf, &id))
		  {
		    /* Avoid deadlock by rejecting packet that would de-sequentialize receive buffer */
		    if (reliable_wont_break_sequentiality (ks->rec_reliable, id))
		      {
			if (reliable_not_replay (ks->rec_reliable, id))
			  {
			    /* Save incoming ciphertext packet to reliable buffer */
			    struct buffer *in = reliable_get_buf (ks->rec_reliable);
			    ASSERT (in);
			    ASSERT (buf_copy (in, buf));
			    reliable_mark_active_incoming (ks->rec_reliable, in, id, op);
			  }

			/* Process outgoing acknowledgment for packet just received, even if it's a replay */
			reliable_ack_acknowledge_packet_id (ks->rec_ack, id);
		      }
		  }
	      }
	  }
	}
    }

 done:
  buf->len = 0;
  opt->key_ctx_bi = NULL;
  opt->packet_id = NULL;
  opt->pid_persist = NULL;
  opt->packet_id_long_form = false;
  gc_free (&gc);
  return ret;

 error:
  ++multi->n_errors;
  goto done;
}

/*
 * This function is similar to tls_pre_decrypt, except it is called
 * when we are in forking server mode.  Note that we don't modify
 * any state in our parameter objects.  The purpose is solely to
 * determine whether we should fork, in which case true is returned.
 */
bool
tls_pre_decrypt_dynamic (const struct tls_multi *multi,
			 const struct sockaddr_in *from,
			 const struct buffer *buf)
{
  struct gc_arena gc = gc_new ();
  bool ret = false;

  if (buf->len > 0)
    {
      int op;
      int key_id;

      /* get opcode and key ID */
      {
	uint8_t c = *BPTR (buf);
	op = c >> P_OPCODE_SHIFT;
	key_id = c & P_KEY_ID_MASK;
      }

      /* this packet is from an as-yet untrusted source, so
	 scrutinize carefully */

      if (op != P_CONTROL_HARD_RESET_CLIENT_V2)
	{
	  msg (D_TLS_ERRORS,
	       "TLS Error: Unknown opcode (%d) received from %s -- make sure the connecting client is using the --dynamic option",
	       op,
	       print_sockaddr (from, &gc));
	  goto error;
	}

      if (key_id != 0)
	{
	  msg (D_TLS_ERRORS,
	       "TLS Error: Unknown key ID (%d) received from %s -- 0 was expected",
	       key_id,
	       print_sockaddr (from, &gc));
	  goto error;
	}

      if (buf->len > EXPANDED_SIZE_DYNAMIC (&multi->opt.frame))
	{
	  msg (D_TLS_ERRORS,
	       "TLS Error: Large packet (size %d) received from %s -- a packet no larger than %d bytes was expected",
	       buf->len,
	       print_sockaddr (from, &gc),
	       EXPANDED_SIZE_DYNAMIC (&multi->opt.frame));
	  goto error;
	}

      {
	struct buffer newbuf = clone_buf (buf);
	const struct tls_session *session = &multi->session[TM_UNTRUSTED];
	struct crypto_options co = session->tls_auth;
	bool status;

	/*
	 * We are in read-only mode at this point with respect to TLS
	 * control channel state.  After we fork, we will process this
	 * session-initiating packet for real.
	 */
	co.ignore_packet_id = true;

	/* HMAC test, if --tls-auth was specified */
	status = read_control_auth (&newbuf, &co, from);
	free_buf (&newbuf);
	if (!status)
	  goto error;

	/*
	 * At this point, if --tls-auth is being used, we know that
	 * the packet has passed the HMAC test, but we don't know if
	 * it is a replay yet.  We will attempt to defeat replays
	 * by not advancing to the S_START state until we
	 * receive an ACK from our first reply to the client
	 * that includes an HMAC of our randomly generated 64 bit
	 * session ID.
	 *
	 * On the other hand if --tls-auth is not being used, we
	 * will fork and proceed to begin the TLS authentication
	 * handshake with only cursory integrity checks having
	 * been performed, since we will be leaving the task
	 * of authentication solely up to TLS.
	 */

	ret = true;
      }
    }
 error:
  gc_free (&gc);
  return ret;
}

/* Choose the key with which to encrypt a data packet */
void
tls_pre_encrypt (struct tls_multi *multi,
		 struct buffer *buf, struct crypto_options *opt)
{
  multi->save_ks = NULL;
  if (buf->len > 0)
    {
      int i;
      for (i = 0; i < KEY_SCAN_SIZE; ++i)
	{
	  struct key_state *ks = multi->key_scan[i];
	  if (ks->state >= S_ACTIVE)
	    {
	      opt->key_ctx_bi = &ks->key;
	      opt->packet_id = multi->opt.replay ? &ks->packet_id : NULL;
	      opt->pid_persist = NULL;
	      opt->packet_id_long_form = multi->opt.packet_id_long_form;
	      multi->save_ks = ks;
	      msg (D_TLS_DEBUG, "TLS: tls_pre_encrypt: key_id=%d", ks->key_id);
	      return;
	    }
	}

      {
	struct gc_arena gc = gc_new ();
	msg (D_TLS_NO_SEND_KEY, "TLS Warning: no data channel send key available: %s",
	     print_key_id (multi, &gc));
	gc_free (&gc);
      }
    }

  buf->len = 0;
  opt->key_ctx_bi = NULL;
  opt->packet_id = NULL;
  opt->pid_persist = NULL;
  opt->packet_id_long_form = false;
}

/* Prepend the appropriate opcode to encrypted buffer prior to TCP/UDP send */
void
tls_post_encrypt (struct tls_multi *multi, struct buffer *buf)
{
  struct key_state *ks;
  uint8_t *op;

  ks = multi->save_ks;
  multi->save_ks = NULL;
  if (buf->len > 0)
    {
      ASSERT (ks);
      ASSERT (op = buf_prepend (buf, 1));
      *op = (P_DATA_V1 << P_OPCODE_SHIFT) | ks->key_id;
      ++ks->n_packets;
      ks->n_bytes += buf->len;
    }
}

/*
 * Dump a human-readable rendition of an openvpn packet
 * into a garbage collectable string which is returned.
 */
const char *
protocol_dump (struct buffer *buffer, unsigned int flags, struct gc_arena *gc)
{
  struct buffer out = alloc_buf_gc (256, gc);
  struct buffer buf = *buffer;

  uint8_t c;
  int op;
  int key_id;

  int tls_auth_hmac_size = (flags & PD_TLS_AUTH_HMAC_SIZE_MASK);

  if (buf.len <= 0)
    {
      buf_printf (&out, "DATA UNDEF len=%d", buf.len);
      goto done;
    }

  if (!(flags & PD_TLS))
    goto print_data;

  /*
   * Initial byte (opcode)
   */
  if (!buf_read (&buf, &c, sizeof (c)))
    goto done;
  op = (c >> P_OPCODE_SHIFT);
  key_id = c & P_KEY_ID_MASK;
  buf_printf (&out, "%s kid=%d", packet_opcode_name (op), key_id);

  if (op == P_DATA_V1)
    goto print_data;

  /*
   * Session ID
   */
  {
    struct session_id sid;

    if (!session_id_read (&sid, &buf))
      goto done;
    if (flags & PD_VERBOSE)
	buf_printf (&out, " sid=%s", session_id_print (&sid, gc));
  }

  /*
   * tls-auth hmac + packet_id
   */
  if (tls_auth_hmac_size)
    {
      struct packet_id_net pin;
      uint8_t tls_auth_hmac[MAX_HMAC_KEY_LENGTH];

      ASSERT (tls_auth_hmac_size <= MAX_HMAC_KEY_LENGTH);

      if (!buf_read (&buf, tls_auth_hmac, tls_auth_hmac_size))
	goto done;
      if (flags & PD_VERBOSE)
	buf_printf (&out, " tls_hmac=%s", format_hex (tls_auth_hmac, tls_auth_hmac_size, 0, gc));

      if (!packet_id_read (&pin, &buf, true))
	goto done;
      buf_printf(&out, " pid=%s", packet_id_net_print (&pin, (flags & PD_VERBOSE), gc));
    }

  /*
   * ACK list
   */
  buf_printf (&out, " %s", reliable_ack_print(&buf, (flags & PD_VERBOSE), gc));

  if (op == P_ACK_V1)
    goto done;

  /*
   * Packet ID
   */
  {
    packet_id_type l;
    if (!buf_read (&buf, &l, sizeof (l)))
      goto done;
    l = ntohpid (l);
    buf_printf (&out, " pid=" packet_id_format, (packet_id_print_type)l);
  }

print_data:
  if (flags & PD_SHOW_DATA)
    buf_printf (&out, " DATA %s", format_hex (BPTR (&buf), BLEN (&buf), 80, gc));
  else
    buf_printf (&out, " DATA len=%d", buf.len);

done:
  return BSTR (&out);
}

#else
static void dummy(void) {}
#endif /* USE_CRYPTO && USE_SSL*/
