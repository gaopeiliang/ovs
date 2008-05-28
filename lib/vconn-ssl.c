/* Copyright (c) 2008 The Board of Trustees of The Leland Stanford
 * Junior University
 * 
 * We are making the OpenFlow specification and associated documentation
 * (Software) available for public use and benefit with the expectation
 * that others will use, modify and enhance the Software and contribute
 * those enhancements back to the community. However, since we would
 * like to make the Software available for broadest use, with as few
 * restrictions as possible permission is hereby granted, free of
 * charge, to any person obtaining a copy of this Software to deal in
 * the Software under the copyrights without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * The name and trademarks of copyright holder(s) may NOT be used in
 * advertising or publicity pertaining to the Software or any
 * derivatives without specific, written prior permission.
 */

#include "vconn-ssl.h"
#include "dhparams.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <unistd.h>
#include "buffer.h"
#include "socket-util.h"
#include "util.h"
#include "openflow.h"
#include "poll-loop.h"
#include "ofp-print.h"
#include "socket-util.h"
#include "vconn.h"

#include "vlog.h"
#define THIS_MODULE VLM_vconn_ssl

/* Active SSL. */

enum ssl_state {
    STATE_TCP_CONNECTING,
    STATE_SSL_CONNECTING
};

enum session_type {
    CLIENT,
    SERVER
};

struct ssl_vconn
{
    struct vconn vconn;
    enum ssl_state state;
    int connect_error;
    enum session_type type;
    int fd;
    SSL *ssl;
    struct buffer *rxbuf;
    struct buffer *txbuf;
    struct poll_waiter *tx_waiter;
};

/* SSL context created by ssl_init(). */
static SSL_CTX *ctx;

/* Required configuration. */
static bool has_private_key, has_certificate, has_ca_cert;

static int ssl_init(void);
static int do_ssl_init(void);
static bool ssl_wants_io(int ssl_error);
static void ssl_close(struct vconn *);
static int interpret_ssl_error(const char *function, int ret, int error);
static void ssl_do_tx(int fd, short int revents, void *vconn_);
static DH *tmp_dh_callback(SSL *ssl, int is_export UNUSED, int keylength);

static int
new_ssl_vconn(const char *name, int fd, enum session_type type,
              enum ssl_state state, struct vconn **vconnp)
{
    struct ssl_vconn *sslv;
    SSL *ssl = NULL;
    int on = 1;
    int retval;

    /* Check for all the needful configuration. */
    if (!has_private_key) {
        VLOG_ERR("Private key must be configured to use SSL");
        goto error;
    }
    if (!has_certificate) {
        VLOG_ERR("Certificate must be configured to use SSL");
        goto error;
    }
    if (!has_ca_cert) {
        VLOG_ERR("CA certificate must be configured to use SSL");
        goto error;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        VLOG_ERR("Private key does not match certificate public key");
        goto error;
    }

    /* Disable Nagle. */
    retval = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    if (retval) {
        VLOG_ERR("%s: setsockopt(TCP_NODELAY): %s", name, strerror(errno));
        close(fd);
        return errno;
    }

    /* Create and configure OpenSSL stream. */
    ssl = SSL_new(ctx);
    if (ssl == NULL) {
        VLOG_ERR("SSL_new: %s", ERR_error_string(ERR_get_error(), NULL));
        close(fd);
        return ENOPROTOOPT;
    }
    if (SSL_set_fd(ssl, fd) == 0) {
        VLOG_ERR("SSL_set_fd: %s", ERR_error_string(ERR_get_error(), NULL));
        goto error;
    }

    /* Create and return the ssl_vconn. */
    sslv = xmalloc(sizeof *sslv);
    sslv->vconn.class = &ssl_vconn_class;
    sslv->vconn.connect_status = EAGAIN;
    sslv->state = state;
    sslv->type = type;
    sslv->fd = fd;
    sslv->ssl = ssl;
    sslv->rxbuf = NULL;
    sslv->txbuf = NULL;
    sslv->tx_waiter = NULL;
    *vconnp = &sslv->vconn;
    return 0;

error:
    if (ssl) {
        SSL_free(ssl);
    }
    close(fd);
    return ENOPROTOOPT;
}

static struct ssl_vconn *
ssl_vconn_cast(struct vconn *vconn)
{
    assert(vconn->class == &ssl_vconn_class);
    return CONTAINER_OF(vconn, struct ssl_vconn, vconn);
}

static int
ssl_open(const char *name, char *suffix, struct vconn **vconnp)
{
    char *save_ptr, *host_name, *port_string;
    struct sockaddr_in sin;
    int retval;
    int fd;

    retval = ssl_init();
    if (retval) {
        return retval;
    }

    /* Glibc 2.7 has a bug in strtok_r when compiling with optimization that
     * can cause segfaults here:
     * http://sources.redhat.com/bugzilla/show_bug.cgi?id=5614.
     * Using "::" instead of the obvious ":" works around it. */
    host_name = strtok_r(suffix, "::", &save_ptr);
    port_string = strtok_r(NULL, "::", &save_ptr);
    if (!host_name) {
        fatal(0, "%s: bad peer name format", name);
    }

    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    if (lookup_ip(host_name, &sin.sin_addr)) {
        return ENOENT;
    }
    sin.sin_port = htons(port_string && *port_string ? atoi(port_string)
                         : OFP_SSL_PORT);

    /* Create socket. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        VLOG_ERR("%s: socket: %s", name, strerror(errno));
        return errno;
    }
    retval = set_nonblocking(fd);
    if (retval) {
        close(fd);
        return retval;
    }

    /* Connect socket. */
    retval = connect(fd, (struct sockaddr *) &sin, sizeof sin);
    if (retval < 0) {
        if (errno == EINPROGRESS) {
            return new_ssl_vconn(name, fd, CLIENT, STATE_TCP_CONNECTING,
                                 vconnp);
        } else {
            int error = errno;
            VLOG_ERR("%s: connect: %s", name, strerror(error));
            close(fd);
            return error;
        }
    } else {
        return new_ssl_vconn(name, fd, CLIENT, STATE_SSL_CONNECTING,
                             vconnp);
    }
}

static int
ssl_connect(struct vconn *vconn)
{
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);
    int retval;

    switch (sslv->state) {
    case STATE_TCP_CONNECTING:
        retval = check_connection_completion(sslv->fd);
        if (retval) {
            return retval;
        }
        sslv->state = STATE_SSL_CONNECTING;
        /* Fall through. */

    case STATE_SSL_CONNECTING:
        retval = (sslv->type == CLIENT
                   ? SSL_connect(sslv->ssl) : SSL_accept(sslv->ssl));
        if (retval != 1) {
            int error = SSL_get_error(sslv->ssl, retval);
            if (retval < 0 && ssl_wants_io(error)) {
                return EAGAIN;
            } else {
                interpret_ssl_error((sslv->type == CLIENT ? "SSL_connect"
                                     : "SSL_accept"), retval, error);
                shutdown(sslv->fd, SHUT_RDWR);
                return EPROTO;
            }
        } else {
            return 0;
        }
    }

    NOT_REACHED();
}

static void
ssl_close(struct vconn *vconn)
{
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);
    poll_cancel(sslv->tx_waiter);
    SSL_free(sslv->ssl);
    close(sslv->fd);
    free(sslv);
}

static int
interpret_ssl_error(const char *function, int ret, int error)
{
    switch (error) {
    case SSL_ERROR_NONE:
        VLOG_ERR("%s: unexpected SSL_ERROR_NONE", function);
        break;

    case SSL_ERROR_ZERO_RETURN:
        VLOG_ERR("%s: unexpected SSL_ERROR_ZERO_RETURN", function);
        break;

    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        return EAGAIN;

    case SSL_ERROR_WANT_CONNECT:
        VLOG_ERR("%s: unexpected SSL_ERROR_WANT_CONNECT", function);
        break;

    case SSL_ERROR_WANT_ACCEPT:
        VLOG_ERR("%s: unexpected SSL_ERROR_WANT_ACCEPT", function);
        break;

    case SSL_ERROR_WANT_X509_LOOKUP:
        VLOG_ERR("%s: unexpected SSL_ERROR_WANT_X509_LOOKUP", function);
        break;

    case SSL_ERROR_SYSCALL: {
        int queued_error = ERR_get_error();
        if (queued_error == 0) {
            if (ret < 0) {
                int status = errno;
                VLOG_WARN("%s: system error (%s)", function, strerror(status));
                return status;
            } else {
                VLOG_WARN("%s: unexpected SSL connection close", function);
                return EPROTO;
            }
        } else {
            VLOG_DBG("%s: %s", function, ERR_error_string(queued_error, NULL));
            break;
        }
    }

    case SSL_ERROR_SSL: {
        int queued_error = ERR_get_error();
        if (queued_error != 0) {
            VLOG_DBG("%s: %s", function, ERR_error_string(queued_error, NULL));
        } else {
            VLOG_ERR("%s: SSL_ERROR_SSL without queued error", function);
        }
        break;
    }

    default:
        VLOG_ERR("%s: bad SSL error code %d", function, error);
        break;
    }
    return EIO;
}

static int
ssl_recv(struct vconn *vconn, struct buffer **bufferp)
{
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);
    struct buffer *rx;
    size_t want_bytes;
    ssize_t ret;

    if (sslv->rxbuf == NULL) {
        sslv->rxbuf = buffer_new(1564);
    }
    rx = sslv->rxbuf;

again:
    if (sizeof(struct ofp_header) > rx->size) {
        want_bytes = sizeof(struct ofp_header) - rx->size;
    } else {
        struct ofp_header *oh = rx->data;
        size_t length = ntohs(oh->length);
        if (length < sizeof(struct ofp_header)) {
            VLOG_ERR("received too-short ofp_header (%zu bytes)", length);
            return EPROTO;
        }
        want_bytes = length - rx->size;
        if (!want_bytes) {
            *bufferp = rx;
            sslv->rxbuf = NULL;
            return 0;
        }
    }
    buffer_reserve_tailroom(rx, want_bytes);

    /* Behavior of zero-byte SSL_read is poorly defined. */
    assert(want_bytes > 0);

    ret = SSL_read(sslv->ssl, buffer_tail(rx), want_bytes);
    if (ret > 0) {
        rx->size += ret;
        if (ret == want_bytes) {
            if (rx->size > sizeof(struct ofp_header)) {
                *bufferp = rx;
                sslv->rxbuf = NULL;
                return 0;
            } else {
                goto again;
            }
        }
        return EAGAIN;
    } else {
        int error = SSL_get_error(sslv->ssl, ret);
        if (error == SSL_ERROR_ZERO_RETURN) {
            /* Connection closed (EOF). */
            if (rx->size) {
                VLOG_WARN("SSL_read: unexpected connection close");
                return EPROTO;
            } else {
                return EOF;
            }
        } else {
            return interpret_ssl_error("SSL_read", ret, error);
        }
    }
}

static void
ssl_clear_txbuf(struct ssl_vconn *sslv)
{
    buffer_delete(sslv->txbuf);
    sslv->txbuf = NULL;
    sslv->tx_waiter = NULL;
}

static void
ssl_register_tx_waiter(struct vconn *vconn) 
{
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);
    short int events = SSL_want_read(sslv->ssl) ? POLLIN : POLLOUT;
    sslv->tx_waiter = poll_fd_callback(sslv->fd, events, ssl_do_tx, vconn);
}

static void
ssl_do_tx(int fd UNUSED, short int revents UNUSED, void *vconn_)
{
    struct vconn *vconn = vconn_;
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);
    int ret = SSL_write(sslv->ssl, sslv->txbuf->data, sslv->txbuf->size);
    if (ret > 0) {
        buffer_pull(sslv->txbuf, ret);
        if (sslv->txbuf->size == 0) {
            ssl_clear_txbuf(sslv);
            return;
        }
    } else {
        int error = SSL_get_error(sslv->ssl, ret);
        if (error == SSL_ERROR_ZERO_RETURN) {
            /* Connection closed (EOF). */
            VLOG_WARN("SSL_write: connection close");
        } else if (interpret_ssl_error("SSL_write", ret, error) != EAGAIN) {
            ssl_clear_txbuf(sslv);
            return;
        }
    }
    ssl_register_tx_waiter(vconn);
}

static int
ssl_send(struct vconn *vconn, struct buffer *buffer)
{
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);
    ssize_t ret;

    if (sslv->txbuf) {
        return EAGAIN;
    }

    ret = SSL_write(sslv->ssl, buffer->data, buffer->size);
    if (ret > 0) {
        if (ret == buffer->size) {
            buffer_delete(buffer);
        } else {
            sslv->txbuf = buffer;
            buffer_pull(buffer, ret);
            ssl_register_tx_waiter(vconn);
        }
        return 0;
    } else {
        int error = SSL_get_error(sslv->ssl, ret);
        if (error == SSL_ERROR_ZERO_RETURN) {
            /* Connection closed (EOF). */
            VLOG_WARN("SSL_write: connection close");
            return EPIPE;
        } else {
            return interpret_ssl_error("SSL_write", ret, error);
        }
    }
}

static bool
ssl_needs_wait(struct ssl_vconn *sslv) 
{
    if (SSL_want_read(sslv->ssl)) {
        poll_fd_wait(sslv->fd, POLLIN);
        return true;
    } else if (SSL_want_write(sslv->ssl)) {
        poll_fd_wait(sslv->fd, POLLOUT);
        return true;
    } else {
        return false;
    }
}

static void
ssl_wait(struct vconn *vconn, enum vconn_wait_type wait)
{
    struct ssl_vconn *sslv = ssl_vconn_cast(vconn);

    switch (wait) {
    case WAIT_CONNECT:
        if (vconn_connect(vconn) != EAGAIN) {
            poll_immediate_wake();
        } else if (sslv->state == STATE_TCP_CONNECTING) {
            poll_fd_wait(sslv->fd, POLLOUT);
        } else if (!ssl_needs_wait(sslv)) {
            NOT_REACHED();
        }
        break;

    case WAIT_RECV:
        if (!ssl_needs_wait(sslv)) {
            if (SSL_pending(sslv->ssl)) {
                poll_immediate_wake();
            } else {
                poll_fd_wait(sslv->fd, POLLIN);
            }
        }
        break;

    case WAIT_SEND:
        if (!sslv->txbuf && !ssl_needs_wait(sslv)) {
            poll_fd_wait(sslv->fd, POLLOUT);
        }
        break;

    default:
        NOT_REACHED();
    }
}

struct vconn_class ssl_vconn_class = {
    .name = "ssl",
    .open = ssl_open,
    .close = ssl_close,
    .connect = ssl_connect,
    .recv = ssl_recv,
    .send = ssl_send,
    .wait = ssl_wait,
};

/* Passive SSL. */

struct pssl_vconn
{
    struct vconn vconn;
    int fd;
};

static struct pssl_vconn *
pssl_vconn_cast(struct vconn *vconn)
{
    assert(vconn->class == &pssl_vconn_class);
    return CONTAINER_OF(vconn, struct pssl_vconn, vconn);
}

static int
pssl_open(const char *name, char *suffix, struct vconn **vconnp)
{
    struct sockaddr_in sin;
    struct pssl_vconn *pssl;
    int retval;
    int fd;
    unsigned int yes = 1;

    retval = ssl_init();
    if (retval) {
        return retval;
    }

    /* Create socket. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        int error = errno;
        VLOG_ERR("%s: socket: %s", name, strerror(error));
        return error;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        int error = errno;
        VLOG_ERR("%s: setsockopt(SO_REUSEADDR): %s", name, strerror(errno));
        return error;
    }

    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(atoi(suffix) ? atoi(suffix) : OFP_SSL_PORT);
    retval = bind(fd, (struct sockaddr *) &sin, sizeof sin);
    if (retval < 0) {
        int error = errno;
        VLOG_ERR("%s: bind: %s", name, strerror(error));
        close(fd);
        return error;
    }

    retval = listen(fd, 10);
    if (retval < 0) {
        int error = errno;
        VLOG_ERR("%s: listen: %s", name, strerror(error));
        close(fd);
        return error;
    }

    retval = set_nonblocking(fd);
    if (retval) {
        close(fd);
        return retval;
    }

    pssl = xmalloc(sizeof *pssl);
    pssl->vconn.class = &pssl_vconn_class;
    pssl->vconn.connect_status = 0;
    pssl->fd = fd;
    *vconnp = &pssl->vconn;
    return 0;
}

static void
pssl_close(struct vconn *vconn)
{
    struct pssl_vconn *pssl = pssl_vconn_cast(vconn);
    close(pssl->fd);
    free(pssl);
}

static int
pssl_accept(struct vconn *vconn, struct vconn **new_vconnp)
{
    struct pssl_vconn *pssl = pssl_vconn_cast(vconn);
    int new_fd;
    int error;

    new_fd = accept(pssl->fd, NULL, NULL);
    if (new_fd < 0) {
        int error = errno;
        if (error != EAGAIN) {
            VLOG_DBG("accept: %s", strerror(error));
        }
        return error;
    }

    error = set_nonblocking(new_fd);
    if (error) {
        close(new_fd);
        return error;
    }

    return new_ssl_vconn("ssl" /* FIXME */, new_fd,
                         SERVER, STATE_SSL_CONNECTING, new_vconnp);
}

static void
pssl_wait(struct vconn *vconn, enum vconn_wait_type wait)
{
    struct pssl_vconn *pssl = pssl_vconn_cast(vconn);
    assert(wait == WAIT_ACCEPT);
    poll_fd_wait(pssl->fd, POLLIN);
}

struct vconn_class pssl_vconn_class = {
    .name = "pssl",
    .open = pssl_open,
    .close = pssl_close,
    .accept = pssl_accept,
    .wait = pssl_wait,
};

/*
 * Returns true if OpenSSL error is WANT_READ or WANT_WRITE, indicating that
 * OpenSSL is requesting that we call it back when the socket is ready for read
 * or writing, respectively.
 */
static bool
ssl_wants_io(int ssl_error)
{
    return (ssl_error == SSL_ERROR_WANT_WRITE
            || ssl_error == SSL_ERROR_WANT_READ);
}

static int
ssl_init(void)
{
    static int init_status = -1;
    if (init_status < 0) {
        init_status = do_ssl_init();
        assert(init_status >= 0);
    }
    return init_status;
}

static int
do_ssl_init(void)
{
    SSL_METHOD *method;

    SSL_library_init();
    SSL_load_error_strings();

    method = TLSv1_method();
    if (method == NULL) {
        VLOG_ERR("TLSv1_method: %s", ERR_error_string(ERR_get_error(), NULL));
        return ENOPROTOOPT;
    }

    ctx = SSL_CTX_new(method);
    if (ctx == NULL) {
        VLOG_ERR("SSL_CTX_new: %s", ERR_error_string(ERR_get_error(), NULL));
        return ENOPROTOOPT;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    SSL_CTX_set_tmp_dh_callback(ctx, tmp_dh_callback);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_mode(ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);

    return 0;
}

static DH *
tmp_dh_callback(SSL *ssl, int is_export UNUSED, int keylength)
{
    struct dh {
        int keylength;
        DH *dh;
        DH *(*constructor)(void);
    };

    static struct dh dh_table[] = {
        {1024, NULL, get_dh1024},
        {2048, NULL, get_dh2048},
        {4096, NULL, get_dh4096},
    };

    struct dh *dh;

    for (dh = dh_table; dh < &dh_table[ARRAY_SIZE(dh_table)]; dh++) {
        if (dh->keylength == keylength) {
            if (!dh->dh) {
                dh->dh = dh->constructor();
                if (!dh->dh) {
                    fatal(ENOMEM, "out of memory constructing "
                          "Diffie-Hellman parameters");
                }
            }
            return dh->dh;
        }
    }
    VLOG_ERR("no Diffie-Hellman parameters for key length %d", keylength);
    return NULL;
}

void
vconn_ssl_set_private_key_file(const char *file_name)
{
    if (ssl_init()) {
        return;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, file_name, SSL_FILETYPE_PEM) != 1) {
        VLOG_ERR("SSL_use_PrivateKey_file: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    has_private_key = true;
}

void
vconn_ssl_set_certificate_file(const char *file_name)
{
    if (ssl_init()) {
        return;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, file_name) != 1) {
        VLOG_ERR("SSL_use_certificate_file: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    has_certificate = true;
}

void
vconn_ssl_set_ca_cert_file(const char *file_name)
{
    STACK_OF(X509_NAME) *ca_list;

    if (ssl_init()) {
        return;
    }

    /* Set up list of CAs that the server will accept from the client. */
    ca_list = SSL_load_client_CA_file(file_name);
    if (ca_list == NULL) {
        VLOG_ERR("SSL_load_client_CA_file: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return;
    }
    SSL_CTX_set_client_CA_list(ctx, ca_list);

    /* Set up CAs for OpenSSL to trust in verifying the peer's certificate. */
    if (SSL_CTX_load_verify_locations(ctx, file_name, NULL) != 1) {
        VLOG_ERR("SSL_CTX_load_verify_locations: %s",
                 ERR_error_string(ERR_get_error(), NULL));
        return;
    }

    has_ca_cert = true;
}
