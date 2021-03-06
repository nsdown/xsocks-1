#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "util.h"
#include "logger.h"
#include "common.h"
#include "crypto.h"
#include "packet.h"
#include "cache.h"


#define KEY_BYTES 32U

struct client_context {
    struct sockaddr addr;
    uv_udp_t *local_handle;
    uv_udp_t server_handle;
    uv_timer_t *timer;
    char key[KEY_BYTES + 1];
};

extern uint32_t idle_timeout;
static uv_mutex_t mutex;
static struct cache *cache;


static void
timer_expire(uv_timer_t *handle) {
    struct client_context *client = handle->data;
    uv_mutex_lock(&mutex);
    cache_remove(cache, client->key);
    uv_mutex_unlock(&mutex);
}

static void
timer_close_cb(uv_handle_t *handle) {
    free(handle);
}

static void
reset_timer(struct client_context *client) {
    client->timer->data = client;
    uv_timer_start(client->timer, timer_expire, idle_timeout, 0);
}

static struct client_context *
new_client() {
    struct client_context *client = malloc(sizeof(*client));
    memset(client, 0, sizeof(*client));
    client->timer = malloc(sizeof(uv_timer_t));
    return client;
}

static void
client_close_cb(uv_handle_t *handle) {
    struct client_context *client =
      container_of(handle, struct client_context, server_handle);
    free(client);
}

static void
close_client(struct client_context *client) {
    uv_close((uv_handle_t *)client->timer, timer_close_cb);
    if (!uv_is_closing((uv_handle_t *)&client->server_handle)) {
        uv_close((uv_handle_t *)&client->server_handle, client_close_cb);
    } else {
        free(client);
    }
}

static void
client_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    char *buffer = malloc(suggested_size);
    buf->base = buffer + PRIMITIVE_BYTES;
    buf->len = suggested_size - PRIMITIVE_BYTES;
}

static void
server_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
#if defined(_MSC_VER)
	buf->base = (char*)malloc(suggested_size) + 3;
#else
	buf->base = malloc(suggested_size) + 3;
#endif
    buf->len = suggested_size - 3;
}

static void
client_send_cb(uv_udp_send_t *req, int status) {
    if (status) {
        logger_log(LOG_ERR, "forward to client failed: %s", uv_strerror(status));
    }
    uv_buf_t *buf = (uv_buf_t *)(req + 1);
    free(buf->base);
    free(req);
}


/*
 *
 * SOCKS5 UDP Response
 * +----+------+------+----------+----------+----------+
 * |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
 * +----+------+------+----------+----------+----------+
 * | 2  |  1   |  1   | Variable |    2     | Variable |
 * +----+------+------+----------+----------+----------+
 *
 */
static void
forward_to_client(struct client_context *client, uint8_t *data, ssize_t len) {
    uv_udp_send_t *write_req = malloc(sizeof(*write_req) + sizeof(uv_buf_t));
    uv_buf_t *buf = (uv_buf_t *)(write_req + 1);
    buf->base = (char *)data;
    buf->len = len;
    write_req->data = client;
    uv_udp_send(write_req, client->local_handle, buf, 1, &client->addr, client_send_cb);
}

/*
 *
 * xsocks UDP Response
 * +------+----------+----------+----------+
 * | ATYP | DST.ADDR | DST.PORT |   DATA   |
 * +------+----------+----------+----------+
 * |  1   | Variable |    2     | Variable |
 * +------+----------+----------+----------+
 *
 */
static void
server_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
    if (nread > 0) {
        struct client_context *client = handle->data;
        reset_timer(client);

        int mlen = nread - PRIMITIVE_BYTES;
        uint8_t *m = (uint8_t *)buf->base;
        int rc = crypto_decrypt(m, (uint8_t *)buf->base, nread);
        if (rc) {
            logger_log(LOG_ERR, "invalid packet");
            dump_hex(buf->base, nread, "server recv");
            goto err;
        }

        m -= 3;
        mlen += 3;
        memcpy(m, "\x0\x0\x0", 3); // RSV + FRAG

        forward_to_client(client, m , mlen);

        return;

    } else {
        goto err;
    }

err:
    free(buf->base - 3);
}

static void
server_send_cb(uv_udp_send_t *req, int status) {
    if (status) {
        logger_log(LOG_ERR, "forward to server failed: %s", uv_strerror(status));
    }
    uv_buf_t *buf = (uv_buf_t *)(req + 1);
    free(buf->base);
    free(req);
}

/*
 *
 * xsocks UDP Request
 * +------+----------+----------+----------+
 * | ATYP | DST.ADDR | DST.PORT |   DATA   |
 * +------+----------+----------+----------+
 * |  1   | Variable |    2     | Variable |
 * +------+----------+----------+----------+
 *
 */
static void
forward_to_server(struct sockaddr *server_addr, struct client_context *client, uint8_t *data, ssize_t datalen) {
    uv_udp_send_t *write_req = malloc(sizeof(*write_req) + sizeof(uv_buf_t));
    uv_buf_t *buf = (uv_buf_t *)(write_req + 1);
    buf->base = (char *)data;
    buf->len = datalen;
    write_req->data = client;;
    uv_udp_send(write_req, &client->server_handle, buf, 1, server_addr, server_send_cb);
}

/*
 *
 * SOCKS5 UDP Request
 * +----+------+------+----------+----------+----------+
 * |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
 * +----+------+------+----------+----------+----------+
 * | 2  |  1   |  1   | Variable |    2     | Variable |
 * +----+------+------+----------+----------+----------+
 *
 */
static void
client_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
    struct server_context *server = container_of(handle, struct server_context, udp);
    if (nread > 0) {
        uint8_t frag = buf->base[2];
        if (frag) {
            logger_log(LOG_ERR, "don't support udp dgram frag");
            goto err;
        }

        char key[KEY_BYTES + 1] = {0};
        crypto_generickey((uint8_t *)key, sizeof(key) -1, (uint8_t*)addr, sizeof(*addr), NULL, 0);

        struct client_context *client = NULL;
        uv_mutex_lock(&mutex);
        cache_lookup(cache, key, (void *)&client);
        uv_mutex_unlock(&mutex);

        if (client == NULL) {
            client = new_client();
            client->addr = *addr;
            client->local_handle = handle;
            memcpy(client->key, key, sizeof(key));
            uv_timer_init(handle->loop, client->timer);
            uv_udp_init(handle->loop, &client->server_handle);
            client->server_handle.data = client;
            uv_udp_recv_start(&client->server_handle, server_alloc_cb, server_recv_cb);
            uv_mutex_lock(&mutex);
            cache_insert(cache, client->key, (void *)client);
            uv_mutex_unlock(&mutex);
        }

        int clen = nread - 3 + PRIMITIVE_BYTES;
        uint8_t *c = (uint8_t *)buf->base - PRIMITIVE_BYTES;
        int rc = crypto_encrypt(c, (uint8_t*)buf->base + 3, nread - 3);
        if (!rc) {
            reset_timer(client);
            forward_to_server(server->server_addr, client, c, clen);
        }

    } else {
        goto err;
    }

    return;

err:
    free(buf->base - PRIMITIVE_BYTES);
}

static void
free_cb(void *element) {
    struct client_context *client = (struct client_context *)element;
    close_client(client);
}

static int
select_cb(void *element, void *opaque) {
    struct client_context *client = (struct client_context *)element;
    if (client->local_handle->loop == opaque) {
        return 1;
    }
    return 0;
}

int
udprelay_init() {
    uv_mutex_init(&mutex);
    cache_create(&cache, 1024, free_cb);
    return 0;
}

int
udprelay_start(uv_loop_t *loop, struct server_context *server) {
    int rc;

    uv_udp_init(loop, &server->udp);

    if ((rc = uv_udp_open(&server->udp, server->udp_fd))) {
        logger_stderr("udp open error: %s", uv_strerror(rc));
        return 1;
    }

    rc = uv_udp_bind(&server->udp, server->local_addr, UV_UDP_REUSEADDR);
    if (rc) {
        logger_stderr("bind error: %s", uv_strerror(rc));
        return 1;
    }

    uv_udp_recv_start(&server->udp, client_alloc_cb, client_recv_cb);

    return 0;
}

void
udprelay_close(struct server_context *server) {
    uv_close((uv_handle_t *)&server->udp, NULL);
    uv_mutex_lock(&mutex);
    cache_removeall(cache, server->udp.loop, select_cb);
    uv_mutex_unlock(&mutex);
}

void
udprelay_destroy() {
    uv_mutex_destroy(&mutex);
    free(cache);
}
