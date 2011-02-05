/*
 * $Id$
 *
 * Copyright (c) 2006, Christian Biere
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * Common TLS functions.
 */

#include "common.h"

RCSID("$Id$")

#ifdef HAS_GNUTLS
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#endif /* HAS_GNUTLS */

#include "tls_common.h"
#include "features.h"
#include "sockets.h"

#include "if/gnet_property_priv.h"
#include "if/core/settings.h"

#include "lib/array.h"
#include "lib/concat.h"
#include "lib/fd.h"
#include "lib/file.h"
#include "lib/halloc.h"
#include "lib/header.h"
#include "lib/path.h"
#include "lib/stringify.h"
#include "lib/walloc.h"

#include "lib/override.h"		/* Must be the last header included */

#ifdef HAS_GNUTLS

/**
 * Disabled because incoming connections get stuck after gnutls_handshake.
 * This cannot work trivially because we are losing physical events on
 * the socket to GnuTLS. If there is still data buffered to be read,
 * there won't be any further events on the socket. GnuTLS uses some
 * hacks including recv() with MSG_PEEK to keep the socket select()able.
 * So we would either have to do the same or synthesize events using
 * gnutls_record_check_pending().
 */
#define USE_TLS_CUSTOM_IO

struct tls_context {
	gnutls_session session;
	gnutls_anon_server_credentials server_cred;
	gnutls_anon_client_credentials client_cred;
	const struct gnutella_socket *s;
};

static gnutls_certificate_credentials server_cert_cred;

static inline gnutls_session
tls_socket_get_session(struct gnutella_socket *s)
{
	g_return_val_if_fail(s, NULL);
	g_return_val_if_fail(s->tls.ctx, NULL);
	return s->tls.ctx->session;
}

static inline size_t
tls_adjust_send_size(struct gnutella_socket *s, size_t size)
{
	size_t max_size = gnutls_record_get_max_size(tls_socket_get_session(s));
	g_assert(max_size > 0);
	return MIN(size, max_size);
}

static inline void
tls_transport_debug(const char *op, int fd, size_t size, ssize_t ret)
{
	int saved_errno = errno;

	if ((ssize_t) -1 == ret) {
		unsigned level = is_temporary_error(saved_errno) ? 2 : 0;

		if (GNET_PROPERTY(tls_debug) > level) {
			g_debug("%s(): fd=%d size=%lu ret=%ld errno=\"%s\"",
				op, fd, (gulong) size, (glong) ret,
				g_strerror(saved_errno));
		}
	} else {
		if (GNET_PROPERTY(tls_debug) > 2) {
			g_debug("%s(): fd=%d size=%lu ret=%ld",
				op, fd, (gulong) size, (glong) ret);
		}
	}
	errno = saved_errno;
}

/**
 * Change the monitoring condition on the socket.
 */
static void
tls_socket_evt_change(struct gnutella_socket *s, inputevt_cond_t cond)
{
	g_assert(s);
	g_assert(socket_with_tls(s));	/* No USES yet, may not have handshaked */
	g_assert(INPUT_EVENT_EXCEPTION != cond);

	if (0 == s->gdk_tag)
		return;

	if (cond != s->tls.cb_cond) {
		int saved_errno = errno;

		if (GNET_PROPERTY(tls_debug) > 1) {
			int fd = socket_evt_fd(s);
			g_debug("tls_socket_evt_change: fd=%d, cond=%s -> %s",
				fd, inputevt_cond_to_string(s->tls.cb_cond),
				inputevt_cond_to_string(cond));
		}
		if (s->gdk_tag) {
			inputevt_remove(s->gdk_tag);
			s->gdk_tag = 0;
		}
		socket_evt_set(s, cond, s->tls.cb_handler, s->tls.cb_data);
		errno = saved_errno;
	}
}

static inline void
tls_signal_pending(struct gnutella_socket *s)
{
	size_t n = gnutls_record_check_pending(tls_socket_get_session(s));

	if (n > 0) {
		if (GNET_PROPERTY(tls_debug) > 1) {
			g_debug("%s: pending=%lu", G_STRFUNC, (unsigned long) n);
		}
		inputevt_set_readable(s->file_desc);
	}
}

static inline ssize_t
tls_push(gnutls_transport_ptr ptr, const void *buf, size_t size) 
{
	struct gnutella_socket *s = ptr;
	ssize_t ret;

	g_assert(s);
	g_assert(is_valid_fd(s->file_desc));

	ret = s_write(s->file_desc, buf, size);
	tls_signal_pending(s);

	if ((ssize_t) -1 == ret) {
		gnutls_transport_set_global_errno(errno);
	}
	tls_transport_debug("tls_push", s->file_desc, size, ret);
	return ret;
}

static inline ssize_t
tls_pull(gnutls_transport_ptr ptr, void *buf, size_t size) 
{
	struct gnutella_socket *s = ptr;
	ssize_t ret;

	g_assert(s);
	g_assert(is_valid_fd(s->file_desc));

	ret = s_read(s->file_desc, buf, size);
	tls_signal_pending(s);

	if ((ssize_t) -1 == ret) {
		gnutls_transport_set_global_errno(errno);
	} else if (0 == ret) {
		socket_eof(s);
	}
	tls_transport_debug("tls_pull", s->file_desc, size, ret);
	return ret;
}

static gnutls_dh_params
get_dh_params(void)
{
	static gnutls_dh_params dh_params;
	static gboolean initialized = FALSE;

	if (!initialized) {
 		if (gnutls_dh_params_init(&dh_params)) {
			g_warning("get_dh_params(): gnutls_dh_params_init() failed");
			return NULL;
		}
    	if (gnutls_dh_params_generate2(dh_params, TLS_DH_BITS)) {
			g_warning("get_dh_params(): gnutls_dh_params_generate2() failed");
			return NULL;
		}
		initialized = TRUE;
	}
	return dh_params;
}

static void
tls_print_session_info(const host_addr_t addr, guint16 port,
	gnutls_session session, gboolean incoming)
{
	const char *proto, *cert, *kx, *ciph, *mac, *comp;

	g_return_if_fail(session);

	proto = gnutls_protocol_get_name(gnutls_protocol_get_version(session));
	cert = gnutls_certificate_type_get_name(
				gnutls_certificate_type_get(session));
	kx = gnutls_kx_get_name(gnutls_kx_get(session));
	comp = gnutls_compression_get_name(gnutls_compression_get(session));
	ciph = gnutls_cipher_get_name(gnutls_cipher_get(session));
	mac = gnutls_mac_get_name(gnutls_mac_get (session));

	g_debug(
		"TLS session info (%s):\n"
		"    Host:         %s\n"
		"    Protocol:     %s\n"
		"    Certificate:  %s\n"
		"    Key Exchange: %s\n"
		"    Cipher:       %s\n"
		"    MAC:          %s\n"
		"    Compression:  %s",
		incoming ? "incoming" : "outgoing",
		host_addr_port_to_string(addr, port),
		NULL_STRING(proto),
		NULL_STRING(cert),
		NULL_STRING(kx),
		NULL_STRING(ciph),
		NULL_STRING(mac),
		NULL_STRING(comp)
	);
}

/**
 * @return	TLS_HANDSHAKE_ERROR if the TLS handshake failed.
 *			TLS_HANDSHAKE_RETRY if the handshake is incomplete; thus
 *				tls_handshake() should called again on the next I/O event.
 *			TLS_HANDSHAKE_FINISHED if the TLS handshake succeeded. Note
 *				that this is also returned if TLS is disabled. Therefore
 *				this does not imply an encrypted connection.
 */
enum tls_handshake_result
tls_handshake(struct gnutella_socket *s)
{
	gnutls_session session;
	gboolean do_warn;
	int ret;

	g_assert(s);

	/*
	 * For connect-back probes, the handshake will probably fail. We use
	 * TLS anyway to avoid getting blocked which the remote peer would
	 * not notice. Thus suppress warnings for failed handshakes in this
	 * case.
	 */
	do_warn = SOCK_TYPE_CONNBACK != s->type;

	session = tls_socket_get_session(s);
	g_return_val_if_fail(session, TLS_HANDSHAKE_ERROR);
	g_return_val_if_fail(SOCK_TLS_INITIALIZED == s->tls.stage,
		TLS_HANDSHAKE_ERROR);

#ifdef USE_TLS_CUSTOM_IO
	if (NULL == gnutls_transport_get_ptr(session)) {
		gnutls_transport_set_ptr(session, s);
	}
#else
	if (GPOINTER_TO_INT(gnutls_transport_get_ptr(session)) < 0) {
		g_assert(is_valid_fd(s->file_desc));
		gnutls_transport_set_ptr(session, GINT_TO_POINTER(s->file_desc));
	}
#endif

	ret = gnutls_handshake(session);
	switch (ret) {
	case 0:
		if (GNET_PROPERTY(tls_debug) > 3) {
			g_debug("TLS handshake succeeded");
		}
		tls_socket_evt_change(s, SOCK_CONN_INCOMING == s->direction
									? INPUT_EVENT_R : INPUT_EVENT_W);
		if (GNET_PROPERTY(tls_debug > 3)) {
			tls_print_session_info(s->addr, s->port, session,
				SOCK_CONN_INCOMING == s->direction);
		}
		tls_signal_pending(s);
		return TLS_HANDSHAKE_FINISHED;
	case GNUTLS_E_AGAIN:
	case GNUTLS_E_INTERRUPTED:
		tls_socket_evt_change(s, gnutls_record_get_direction(session)
				? INPUT_EVENT_WX : INPUT_EVENT_RX);
		if (GNET_PROPERTY(tls_debug) > 3) {
			g_debug("TLS handshake proceeding...");
		}
		tls_signal_pending(s);
		return TLS_HANDSHAKE_RETRY;
	case GNUTLS_E_PULL_ERROR:
	case GNUTLS_E_PUSH_ERROR:
		if (GNET_PROPERTY(tls_debug)) {
			switch (errno) {
			case EPIPE:
			case ECONNRESET:
				if (GNET_PROPERTY(tls_debug) < 2)
					break;
			default:
				g_debug("gnutls_handshake() failed: host=%s errno=\"%s\"",
					host_addr_port_to_string(s->addr, s->port),
					g_strerror(errno));
			}
		}
		break;
	case GNUTLS_E_UNEXPECTED_PACKET_LENGTH:
		if (SOCK_F_EOF & s->flags) {
		   	/* Remote peer has hung up */
			break;
		}
		/* FALLTHROUGH */
	default:
		if (do_warn && GNET_PROPERTY(tls_debug)) {
			g_carp("gnutls_handshake() failed: host=%s (%s) error=\"%s\"",
				host_addr_port_to_string(s->addr, s->port),
				SOCK_CONN_INCOMING == s->direction ? "incoming" : "outgoing",
				gnutls_strerror(ret));
		}
	}
	return TLS_HANDSHAKE_ERROR;
}

/**
 * Initiates a new TLS session.
 *
 * @return 0 on success, -1 on error.
 */
int
tls_init(struct gnutella_socket *s)
{
	static const int cipher_list[] = {
		GNUTLS_CIPHER_AES_256_CBC,
		GNUTLS_CIPHER_AES_128_CBC,
		0
	};
	static const int kx_list[] = {
		GNUTLS_KX_ANON_DH,
		GNUTLS_KX_RSA,
		GNUTLS_KX_DHE_DSS,
		GNUTLS_KX_DHE_RSA,
		0
	};
	static const int mac_list[] = {
		GNUTLS_MAC_MD5,
		GNUTLS_MAC_SHA,
		GNUTLS_MAC_RMD160,
		0
	};
	static const int comp_list[] = {
#if 0
		/* FIXME: This causes internal errors from gnutls_record_recv()
		 * at least when browsing hosts which send deflated data.
		 */
		GNUTLS_COMP_DEFLATE,
#endif
		GNUTLS_COMP_NULL,
		0
	};
	static const int cert_list[] = {
		GNUTLS_CRT_X509,
		GNUTLS_CRT_OPENPGP,
		0
	};
	struct tls_context *ctx;
	gboolean server = SOCK_CONN_INCOMING == s->direction;
	const char *fn;
	int e;

#define TRY(function) (fn = (#function)), e = function

	socket_check(s);

	ctx = walloc0(sizeof *ctx);
	ctx->s = s;
	s->tls.ctx = ctx;

	if (
		TRY(gnutls_init)(&ctx->session, server ? GNUTLS_SERVER : GNUTLS_CLIENT)
	) {
		ctx->session = NULL;
		goto failure;
	}

#ifdef USE_TLS_CUSTOM_IO
	gnutls_transport_set_ptr(ctx->session, s);
	gnutls_transport_set_push_function(ctx->session, tls_push);
	gnutls_transport_set_pull_function(ctx->session, tls_pull);
	gnutls_transport_set_lowat(ctx->session, 0);
#endif	/* USE_TLS_CUSTOM_IO */

	if (server) {
		if (TRY(gnutls_anon_allocate_server_credentials)(&ctx->server_cred))
			goto failure;

		gnutls_anon_set_server_dh_params(ctx->server_cred, get_dh_params());
		gnutls_dh_set_prime_bits(ctx->session, TLS_DH_BITS);

		if (TRY(gnutls_credentials_set)(ctx->session,
				GNUTLS_CRD_ANON, ctx->server_cred))
			goto failure;

		if (server_cert_cred) {
			if (TRY(gnutls_credentials_set)(ctx->session,
					GNUTLS_CRD_CERTIFICATE, server_cert_cred))
				goto failure;
		}
	} else {
		if (TRY(gnutls_anon_allocate_client_credentials)(&ctx->client_cred))
			goto failure;

		if (TRY(gnutls_credentials_set)(ctx->session,
				GNUTLS_CRD_ANON, ctx->client_cred))
			goto failure;
	}

	gnutls_set_default_priority(ctx->session);
	if (TRY(gnutls_cipher_set_priority)(ctx->session, cipher_list))
		goto failure;

	if (TRY(gnutls_kx_set_priority)(ctx->session, kx_list))
		goto failure;

	if (TRY(gnutls_mac_set_priority)(ctx->session, mac_list))
		goto failure;

	if (TRY(gnutls_certificate_type_set_priority)(ctx->session, cert_list)) {
		if (GNUTLS_E_UNIMPLEMENTED_FEATURE != e)
			goto failure;
	}

	if (TRY(gnutls_compression_set_priority)(ctx->session, comp_list))
		goto failure;

	return 0;

failure:
	g_warning("%s() failed: %s", EMPTY_STRING(fn), gnutls_strerror(e));
	tls_free(s);
	return -1;
#undef TRY
}

void
tls_free(struct gnutella_socket *s)
{
	tls_context_t ctx;

	socket_check(s);
	ctx = s->tls.ctx;
	if (ctx) {
		if (ctx->session) {
			gnutls_deinit(ctx->session);
		}
		if (ctx->server_cred) {
			gnutls_anon_free_server_credentials(ctx->server_cred);
			ctx->server_cred = NULL;
		}
		if (ctx->client_cred) {
			gnutls_anon_free_client_credentials(ctx->client_cred);
			ctx->client_cred = NULL;
		}
		wfree(ctx, sizeof *ctx);
		s->tls.ctx = NULL;
	}
}

static inline void
tls_log_function(int level, const char *text)
{
	if (GNET_PROPERTY(tls_debug) > UNSIGNED(level)) {
		char *str = h_strdup(text);
		strchomp(str, 0);
		g_debug("TLS(%d): %s", level, str);
		hfree(str);
	}
}

void
tls_global_init(void)
{
	static const struct {
		const char * const name;
		const int major;
		const int minor;
	} f = {
		"tls", 1, 0
	};
	char *cert_file, *key_file;

#if !defined(REMAP_ZALLOC) && !defined(TRACK_MALLOC) && !defined(TRACK_ZALLOC)
	gnutls_global_set_mem_functions(halloc, halloc, NULL, hrealloc, hfree);
#endif

	if (gnutls_global_init()) {
		g_error("gnutls_global_init() failed");
	}

#ifdef USE_TLS_CUSTOM_IO
	gnutls_global_set_log_level(9);
	gnutls_global_set_log_function(tls_log_function);
#endif	/* USE_TLS_CUSTOM_IO */

	get_dh_params();

	key_file = make_pathname(settings_config_dir(), "key.pem");
	cert_file = make_pathname(settings_config_dir(), "cert.pem");

	if (file_exists(key_file) && file_exists(cert_file)) {
		int ret;

		gnutls_certificate_allocate_credentials(&server_cert_cred);
		ret = gnutls_certificate_set_x509_key_file(server_cert_cred,
				cert_file, key_file, GNUTLS_X509_FMT_PEM);
		if (ret < 0) {
			g_warning("gnutls_certificate_set_x509_key_file() failed: %s",
					gnutls_strerror(ret));
			gnutls_certificate_free_credentials(server_cert_cred);
			server_cert_cred = NULL;
		} else {
			gnutls_certificate_set_dh_params(server_cert_cred, get_dh_params());
		}
	}
	HFREE_NULL(key_file);
	HFREE_NULL(cert_file);

	header_features_add(FEATURES_CONNECTIONS, f.name, f.major, f.minor);
	header_features_add(FEATURES_DOWNLOADS, f.name, f.major, f.minor);
	header_features_add(FEATURES_UPLOADS, f.name, f.major, f.minor);
}

void
tls_global_close(void)
{
	gnutls_global_deinit();
}

static ssize_t
tls_write_intern(struct wrap_io *wio, gconstpointer buf, size_t size)
{
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret;

	g_assert((0 == s->tls.snarf) ^ (NULL == buf));
	g_assert((0 == s->tls.snarf) ^ (0 == size));

	size = tls_adjust_send_size(s, size);
	ret = gnutls_record_send(tls_socket_get_session(s), buf, size);
	if (ret < 0) {
		switch (ret) {
		case GNUTLS_E_INTERRUPTED:
		case GNUTLS_E_AGAIN:
			if (0 == s->tls.snarf) {
				s->tls.snarf = size;
				ret = size;
			} else {
				errno = VAL_EAGAIN;
				ret = -1;
			}
			break;
		case GNUTLS_E_PULL_ERROR:
		case GNUTLS_E_PUSH_ERROR:
			if (GNET_PROPERTY(tls_debug)) {
				g_debug("tls_write(): socket_tls_write() failed: "
					"host=%s errno=\"%s\"",
					host_addr_port_to_string(s->addr, s->port),
					g_strerror(errno));
			}
			errno = EIO;
			ret = -1;
			goto finish;

		default:
			if (GNET_PROPERTY(tls_debug)) {
				g_carp("tls_write(): gnutls_record_send() failed: "
					"host=%s snarf=%lu error=\"%s\"",
					host_addr_port_to_string(s->addr, s->port),
					(unsigned long) s->tls.snarf,
					gnutls_strerror(ret));
			}
			errno = EIO;
			ret = -1;
			goto finish;
		}
	} else {

		if (s->tls.snarf) {
			g_assert(s->tls.snarf >= (size_t) ret);
			s->tls.snarf -= ret;
			errno = VAL_EAGAIN;
			ret = -1;
			goto finish;
		}
	}

	if (s->tls.snarf) {
		tls_socket_evt_change(s, INPUT_EVENT_WX);
	}

finish:
	g_assert(ret == (ssize_t) -1 || (size_t) ret <= size);
	return ret;
}

static int
tls_flush(struct wrap_io *wio)
{
	struct gnutella_socket *s = wio->ctx;

	socket_check(s);

	if (s->tls.snarf) {
		if (GNET_PROPERTY(tls_debug > 1)) {
			g_debug("tls_flush: snarf=%lu host=%s",
					(gulong) s->tls.snarf,
					host_addr_port_to_string(s->addr, s->port));
		}
		(void ) tls_write_intern(wio, NULL, 0);
		if (s->tls.snarf)
			return -1;
	}
	return 0;
}


static ssize_t
tls_write(struct wrap_io *wio, gconstpointer buf, size_t size)
{
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret;

	g_assert(size > 0);
	g_assert(size <= INT_MAX);
	g_assert(s != NULL);
	g_assert(buf != NULL);

	g_assert(socket_uses_tls(s));

	ret = tls_flush(wio);
	if (0 == ret) {
		ret = tls_write_intern(wio, buf, size);
		if (s->gdk_tag) {
			tls_socket_evt_change(s, INPUT_EVENT_WX);
		}
	}
	g_assert(ret == (ssize_t) -1 || (size_t) ret <= size);
	tls_signal_pending(s);
	return ret;
}

static ssize_t
tls_read(struct wrap_io *wio, gpointer buf, size_t size)
{
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret;

	g_assert(size <= INT_MAX);
	g_assert(s != NULL);
	g_assert(buf != NULL);

	g_assert(socket_uses_tls(s));

	if (tls_flush(wio) && !is_temporary_error(errno)) {
		if (GNET_PROPERTY(tls_debug)) {
			g_warning("tls_read: flush error: %s", g_strerror(errno));
		}
		return -1;
	}

	ret = gnutls_record_recv(tls_socket_get_session(s), buf, size);
	if (ret < 0) {
		switch (ret) {
		case GNUTLS_E_INTERRUPTED:
		case GNUTLS_E_AGAIN:
			errno = VAL_EAGAIN;
			break;
		case GNUTLS_E_PULL_ERROR:
		case GNUTLS_E_PUSH_ERROR:
			if (GNET_PROPERTY(tls_debug)) {
				g_debug("tls_read(): socket_tls_read() failed: "
					"host=%s errno=\"%s\"",
					host_addr_port_to_string(s->addr, s->port),
					g_strerror(errno));
			}
			errno = EIO;
			break;
		case GNUTLS_E_UNEXPECTED_PACKET_LENGTH:
			if (SOCK_F_EOF & s->flags) {
			   	/*
				 * Remote peer has hung up.
				 *
				 * This is not exceptional, so we make it appear to upper
				 * layers (who do not necessarily know they're dealing with
				 * a TLS socket) as a regular EOF condition: the read()
				 * operation return 0.
				 */
				ret = 0;
				goto no_error;
			}
			/* FALLTHROUGH */
		default:
			if (GNET_PROPERTY(tls_debug)) {
				g_carp("tls_read(): gnutls_record_recv() failed: "
					"host=%s error=\"%s\"",
					host_addr_port_to_string(s->addr, s->port),
					gnutls_strerror(ret));
			}
			errno = EIO;
		}
		ret = -1;
	}

no_error:
	if (s->gdk_tag && 0 == s->tls.snarf) {
		tls_socket_evt_change(s, INPUT_EVENT_RX);
	}
	g_assert(ret == (ssize_t) -1 || (size_t) ret <= size);
	tls_signal_pending(s);
	return ret;
}

static ssize_t
tls_writev(struct wrap_io *wio, const iovec_t *iov, int iovcnt)
{
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret, done;
	int i;

	g_assert(socket_uses_tls(s));
	g_assert(iovcnt > 0);

	done = 0;
	ret = 0;
	for (i = 0; i < iovcnt; i++) {
		const size_t size = iovec_len(&iov[i]);

		ret = tls_write(wio, iovec_base(&iov[i]), size);
		if ((ssize_t) -1 == ret)
			break;
		done += (size_t) ret;
		if (size != (size_t) ret)
			break;
	}
	return done > 0 ? done : ret;
}

static ssize_t
tls_readv(struct wrap_io *wio, iovec_t *iov, int iovcnt)
{
	struct gnutella_socket *s = wio->ctx;
	ssize_t ret, done;
	int i;

	g_assert(socket_uses_tls(s));
	g_assert(iovcnt > 0);

	done = 0;
	ret = 0;
	for (i = 0; i < iovcnt; i++) {
		const size_t size = iovec_len(&iov[i]);

		ret = tls_read(wio, iovec_base(&iov[i]), size);
		if ((ssize_t) -1 == ret)
			break;
		done += (size_t) ret;
		if (size != (size_t) ret)
			break;
	}

	return done > 0 ? done : ret;
}

static ssize_t
tls_no_sendto(struct wrap_io *unused_wio, const gnet_host_t *unused_to,
	gconstpointer unused_buf, size_t unused_size)
{
	(void) unused_wio;
	(void) unused_to;
	(void) unused_buf;
	(void) unused_size;
	g_error("no sendto() routine allowed");
	return -1;
}

void
tls_wio_link(struct gnutella_socket *s)
{
	socket_check(s);

	s->wio.write = tls_write;
	s->wio.read = tls_read;
	s->wio.writev = tls_writev;
	s->wio.readv = tls_readv;
	s->wio.sendto = tls_no_sendto;
	s->wio.flush = tls_flush;
}

void
tls_bye(struct gnutella_socket *s)
{
	int ret;
	
	socket_check(s);
	g_return_if_fail(s->tls.ctx);
	g_return_if_fail(s->tls.ctx->session);

	if ((SOCK_F_EOF | SOCK_F_SHUTDOWN) & s->flags)
		return;

	if (tls_flush(&s->wio) && GNET_PROPERTY(tls_debug)) {
		g_warning("tls_bye: tls_flush() failed");
	}

	ret = gnutls_bye(s->tls.ctx->session,
			SOCK_CONN_INCOMING != s->direction
				? GNUTLS_SHUT_WR : GNUTLS_SHUT_RDWR);

	if (ret < 0) {
		switch (ret) {
		case GNUTLS_E_INTERRUPTED:
		case GNUTLS_E_AGAIN:
			break;
		case GNUTLS_E_PULL_ERROR:
		case GNUTLS_E_PUSH_ERROR:
			if (GNET_PROPERTY(tls_debug)) {
				switch (errno) {
				case EPIPE:
				case ECONNRESET:
					if (GNET_PROPERTY(tls_debug) < 2)
						break;
				default:
					if (GNET_PROPERTY(tls_debug)) {
						g_debug("gnutls_bye() failed: host=%s errno=\"%s\"",
							host_addr_port_to_string(s->addr, s->port),
							g_strerror(errno));
					}
				}
			}
			break;
		default:
			if (GNET_PROPERTY(tls_debug)) {
				g_carp("gnutls_bye() failed: host=%s error=\"%s\"",
					host_addr_port_to_string(s->addr, s->port),
					gnutls_strerror(ret));
			}
		}
	}
}

const char *
tls_version_string(void)
{
	static char buf[128];

	if ('\0' == buf[0]) {
		const char *current = gnutls_check_version(NULL);
		int differ = strcmp(current, LIBGNUTLS_VERSION);

		concat_strings(buf, sizeof buf, "GnuTLS ", current,
			differ ? " (compiled against " : "",
			differ ? LIBGNUTLS_VERSION : "",
			differ ? ")" : "",
			(void *) 0);
	}
	return buf;
}

gboolean
tls_enabled(void)
{
	return TRUE;
}

static gnutls_x509_crt
svn_release_notify_certificate(void)
{
	static const char certificate[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBKTCB1qADAgECAgEAMAsGCSqGSIb3DQEBBTAAMB4XDTA3MDgyNTA0MjIxMVoX\n"
"DTA4MDgyNDA0MjIxNVowADBZMAsGCSqGSIb3DQEBAQNKADBHAkCpadMxWZWWzcV7\n"
"Mu66wzBuQ8AkanGspm7ImdRKOlo55V3uBlSob9N/GFlzZ9kG6kS169wgdK2vNQwR\n"
"5jOMeIMbAgMBAAGjQDA+MAwGA1UdEwEB/wQCMAAwDwYDVR0PAQH/BAUDAweAADAd\n"
"BgNVHQ4EFgQU8pP/Zgh/K6N0zVHMEs2VIWZNjUIwCwYJKoZIhvcNAQEFA0EAO6ld\n"
"1NFx0QRBCHE+BUaCX3tuRC0a7HRq8UEqhcKgW7Xk3nkGUNXTcSSo7wu+jpePUsw8\n"
"njFhJCXeDIcR7jzNCA==\n"
"-----END CERTIFICATE-----\n";
	static gboolean initialized;
	static gnutls_x509_crt cert;

	if (!initialized) {
		gnutls_datum cert_data;
		int error;

		initialized = TRUE;
		error = gnutls_x509_crt_init(&cert);
		if (error) {
			g_warning("gnutls_x509_crt_init() failed: %s",
					gnutls_strerror(error));
			cert = NULL;
			return NULL;
		}

		cert_data.data = (void *) certificate;
		cert_data.size = CONST_STRLEN(certificate);
		error = gnutls_x509_crt_import(cert, &cert_data, GNUTLS_X509_FMT_PEM);
		if (error) {
			g_warning("gnutls_x509_crt_import() failed: %s",
					gnutls_strerror(error));
			gnutls_x509_crt_deinit(cert);
			cert = NULL;
			return NULL;
		}
	}
	return cert; 
}

gboolean
svn_release_notification_can_verify(void)
{
	return NULL != svn_release_notify_certificate();
}

static gboolean
verify_signature(gnutls_x509_crt cert,
	const struct array *input, const struct array *signature)
{
	gnutls_datum data, sig;

	g_return_val_if_fail(cert, FALSE);
	g_return_val_if_fail(input, FALSE);
	g_return_val_if_fail(signature, FALSE);

	data.data = (void *) input->data;
	data.size = input->size;

	sig.data = (void *) signature->data;
	sig.size = signature->size;

	return 1 == gnutls_x509_crt_verify_data(cert, 0, &data, &sig);
}

/**
 * Verifies "data" against "signature".
 *
 * @return TRUE if the signature matches.
 */
gboolean
svn_release_notification_verify(guint32 revision, time_t date,
	const struct array *signature)
{
	char rev[12], data[64];
	struct array input;

	uint32_to_string_buf(revision, rev, sizeof rev);
	input.data = (void *) data;
	input.size = concat_strings(data, sizeof data,
					"r", rev,
					"@", uint32_to_string(date),
					(void *) 0);

	return verify_signature(svn_release_notify_certificate(),
				&input, signature);
}

#else	/* !HAS_GNUTLS*/

enum tls_handshake_result
tls_handshake(struct gnutella_socket *s)
{
	(void) s;
	return TLS_HANDSHAKE_FINISHED;
}

int
tls_init(struct gnutella_socket *s)
{
	socket_check(s);
	g_assert_not_reached();
	return -1;
}

void
tls_free(struct gnutella_socket *s)
{
	socket_check(s);
	g_assert_not_reached();
}

void
tls_bye(struct gnutella_socket *s)
{
	socket_check(s);
	g_assert_not_reached();
}

void
tls_wio_link(struct gnutella_socket *s)
{
	socket_check(s);
	g_assert_not_reached();
}

void
tls_global_init(void)
{
	/* Nothing to do */
}

void
tls_global_close(void)
{
	/* Nothing to do */
}

const char *
tls_version_string(void)
{
	return NULL;
}

gboolean
tls_enabled(void)
{
	return FALSE;
}

gboolean
svn_release_notification_can_verify(void)
{
	return FALSE;
}

gboolean
svn_release_notification_verify(guint32 revision, time_t date,
	const struct array *signature)
{
	g_return_val_if_fail(signature, FALSE);
	(void) revision;
	(void) date;
	return FALSE;
}

#endif	/* HAS_GNUTLS */

/* vi: set ts=4 sw=4 cindent: */
