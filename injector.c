#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

#include <sodium.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "log.h"
#include "utp.h"
#include "timer.h"
#include "network.h"
#include "constants.h"
#include "utp_bufferevent.h"


typedef struct bufferevent bufferevent;
typedef struct evbuffer evbuffer;

typedef bool (^http_stream_callback)(uint8_t *data, size_t length, size_t total_length);

void fetch_url(const char *url, http_stream_callback stream)
{
    if (!stream((uint8_t*)"TODO", 4, 8)) {
        return;
    }
    if (!stream((uint8_t*)"TODO", 4, 8)) {
        return;
    }
}

void dht_put_value(const uint8_t *key, const uint8_t *value)
{
    // TODO
    /*
    dht_put(n->dht, g_public_key, g_secret_key, value_str, 0, ^{
        printf("put complete\n");
    });
    */
}

void* memdup(const void *m, size_t length)
{
    void *r = malloc(length);
    memcpy(r, m, length);
    return r;
}

void process_line(bufferevent *bev, const char *line)
{
    const char *url = line;

    // XXX: currently we don't handle a backlog of requests, so multiple responses will interleave

    __block struct {
        uint8_t url_hash[crypto_generichash_BYTES];
        crypto_generichash_state content_state;
    } hash_state;

    crypto_generichash(hash_state.url_hash, sizeof(hash_state.url_hash), (const uint8_t*)url, strlen(url), NULL, 0);
    crypto_generichash_init(&hash_state.content_state, NULL, 0, crypto_generichash_BYTES);

    __block size_t progress = 0;
    fetch_url(url, ^bool (uint8_t *data, size_t length, size_t total_length) {
        if (!progress) {
            uint32_t iprefix = (uint32_t)total_length;
            bufferevent_write(bev, &iprefix, sizeof(iprefix));
        }
        crypto_generichash_update(&hash_state.content_state, data, length);
        bufferevent_write(bev, data, length);
        progress += length;
        if (progress == total_length) {
            uint8_t content_hash[crypto_generichash_BYTES];
            crypto_generichash_final(&hash_state.content_state, content_hash, sizeof(content_hash));
            dht_put_value(hash_state.url_hash, content_hash);
        }
        return true;
    });
}


void http_request_cb(struct evhttp_request *req, void *arg)
{
    const char *uri = evhttp_request_get_uri(req);
    debug("uri: %s\n", uri);
    process_line(NULL/*TODO*/, uri);
}

void bev_read_cb(struct bufferevent *bev, void *ctx)
{
    debug("bev_read_cb %p %x\n", ctx);
    evbuffer *input = bufferevent_get_input(bev);
    unsigned char *buf = evbuffer_pullup(input, evbuffer_get_length(input));
    debug("%s\n", buf);
    process_line(bev, (const char *)buf);
}

uint64 utp_on_accept(utp_callback_arguments *a)
{
    debug("Accepted inbound socket %p\n", a->socket);
    network *n = (network*)utp_context_get_userdata(a->context);
    int fd = utp_socket_create_fd_interface(n->evbase, a->socket);
    evutil_make_socket_closeonexec(fd);
    evutil_make_socket_nonblocking(fd);

    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &salen)) {
        debug("getsockname failed %d %s\n", errno, strerror(errno));
        close(fd);
        return 0;
    }

    bufferevent *bev = bufferevent_socket_new(n->evbase, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        close(fd);
        return 0;
    }
    bufferevent_setcb(bev, bev_read_cb, NULL, NULL, NULL);
    bufferevent_enable(bev, EV_READ);

    return 0;
}

void usage(char *name)
{
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "    %s [options] -p <listening-port>\n", name);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "    -h          Help\n");
    fprintf(stderr, "    -p <port>   Local port\n");
    fprintf(stderr, "    -s <IP>     Source IP\n");
    fprintf(stderr, "\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    char *address = "0.0.0.0";
    char *port = NULL;

    for (;;) {
        int c = getopt(argc, argv, "hp:s:n");
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            usage(argv[0]);
            break;
        case 'p':
            port = optarg;
            break;
        case 's':
            address = optarg;
            break;
        default:
            die("Unhandled argument: %c\n", c);
        }
    }

    if (!port) {
        usage(argv[0]);
    }

    network *n = network_setup(address, port);

    utp_set_callback(n->utp, UTP_ON_ACCEPT, &utp_on_accept);

    timer_repeating(n, 6 * 60 * 60 * 1000, ^{
        dht_announce(n->dht, injector_swarm, ^(const byte *peers, uint num_peers) {
            if (!peers) {
                printf("announce complete\n");
            }
        });
    });

    evhttp_set_gencb(n->http, http_request_cb, n);

    return network_loop(n);
}
