
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csp/csp.h>
#include <csp/csp_error.h>
#include <csp/interfaces/csp_if_kiss.h>

#define LAND_NODE_ADDR     10
#define SAT_NODE_ADDR      20
#define DEMO_PORT          10
#define KISS_TCP_HOST      "127.0.0.1"
#define KISS_TCP_PORT      26001

typedef struct {
    int listen_fd;
    int client_fd;
    pthread_t accept_thread;
    pthread_t rx_thread;
    volatile bool run;
} kiss_tcp_server_t;

typedef struct {
    csp_iface_t iface;
    csp_kiss_interface_data_t kiss_state;
    kiss_tcp_server_t server;
    pthread_mutex_t tx_lock;
} kiss_tcp_iface_t;

static kiss_tcp_iface_t g_kiss_iface = {
    .tx_lock = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_t router_thread;
static volatile bool router_running = true;

static int kiss_tcp_driver_tx(void *driver_data, const uint8_t *data, size_t len) {
    kiss_tcp_iface_t *ctx = driver_data;
    if (ctx->server.client_fd < 0) {
        return CSP_ERR_NOCONN;
    }
    pthread_mutex_lock(&ctx->tx_lock);
    ssize_t sent = send(ctx->server.client_fd, data, len, 0);
    pthread_mutex_unlock(&ctx->tx_lock);
    return (sent == (ssize_t) len) ? CSP_ERR_NONE : CSP_ERR_TX;
}

static void *kiss_tcp_rx_loop(void *arg) {
    kiss_tcp_iface_t *ctx = arg;
    uint8_t buf[512];
    while (ctx->server.run) {
        ssize_t rd = recv(ctx->server.client_fd, buf, sizeof(buf), 0);
        if (rd > 0) {
            csp_kiss_rx(&ctx->iface, buf, (size_t) rd, NULL);
        } else if (rd == 0) {
            fprintf(stderr, "kiss-tcp server: client disconnected\n");
            break;
        } else if (errno != EINTR) {
            perror("kiss-tcp server: recv");
            break;
        }
    }
    ctx->server.run = false;
    return NULL;
}

static void *kiss_tcp_accept_loop(void *arg) {
    kiss_tcp_iface_t *ctx = arg;
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    while (ctx->server.run) {
        int fd = accept(ctx->server.listen_fd, (struct sockaddr *) &client_addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("kiss-tcp server: accept");
            break;
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        printf("kiss-tcp server: connection from %s:%d\n", ipstr, ntohs(client_addr.sin_port));

        ctx->server.client_fd = fd;
        ctx->server.run = true;
        if (pthread_create(&ctx->server.rx_thread, NULL, kiss_tcp_rx_loop, ctx) != 0) {
            perror("pthread_create");
            close(fd);
            ctx->server.client_fd = -1;
            ctx->server.run = false;
            break;
        }

        pthread_join(ctx->server.rx_thread, NULL);
        close(fd);
        ctx->server.client_fd = -1;
        ctx->server.run = true;
    }
    return NULL;
}

static int kiss_tcp_iface_init(kiss_tcp_iface_t *ctx, const char *name, const char *host, uint16_t port) {
    memset(&ctx->iface, 0, sizeof(ctx->iface));
    ctx->iface.name = name;
    ctx->iface.addr = SAT_NODE_ADDR;
    ctx->iface.netmask = 8;
    ctx->iface.nexthop = csp_kiss_tx;
    ctx->iface.interface_data = &ctx->kiss_state;
    ctx->iface.driver_data = ctx;
    ctx->iface.is_default = 1;

    ctx->kiss_state.tx_func = kiss_tcp_driver_tx;
    ctx->kiss_state.rx_mode = KISS_MODE_NOT_STARTED;
    ctx->kiss_state.rx_first = true;

    ctx->server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->server.listen_fd < 0) {
        perror("socket");
        return CSP_ERR_NOMEM;
    }

    int reuse = 1;
    setsockopt(ctx->server.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address %s\n", host);
        return CSP_ERR_INVAL;
    }
    if (bind(ctx->server.listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        return CSP_ERR_NOCONN;
    }
    if (listen(ctx->server.listen_fd, 1) < 0) {
        perror("listen");
        return CSP_ERR_NOCONN;
    }

    if (csp_kiss_add_interface(&ctx->iface) != CSP_ERR_NONE) {
        fprintf(stderr, "Failed to add KISS interface\n");
        return CSP_ERR_NOMEM;
    }
    csp_iflist_add(&ctx->iface);
    csp_rtable_set(0, 0, &ctx->iface, CSP_NO_VIA_ADDRESS);

    ctx->server.client_fd = -1;
    ctx->server.run = true;
    if (pthread_create(&ctx->server.accept_thread, NULL, kiss_tcp_accept_loop, ctx) != 0) {
        perror("pthread_create");
        return CSP_ERR_NOMEM;
    }

    return CSP_ERR_NONE;
}

static void *router_loop(void *arg) {
    (void) arg;
    while (router_running) {
        csp_route_work();
        usleep(1000);
    }
    return NULL;
}

static int start_router_task(void) {
    if (pthread_create(&router_thread, NULL, router_loop, NULL) != 0) {
        perror("pthread_create");
        return CSP_ERR_NOMEM;
    }
    return CSP_ERR_NONE;
}

static void run_receiver(void) {
    csp_socket_t *sock = csp_socket(CSP_SO_NONE);
    if (!sock) {
        fprintf(stderr, "Failed to create CSP socket\n");
        return;
    }
    if (csp_bind(sock, DEMO_PORT) != CSP_ERR_NONE) {
        fprintf(stderr, "Bind failed on port %d\n", DEMO_PORT);
        return;
    }
    if (csp_listen(sock, 5) != CSP_ERR_NONE) {
        fprintf(stderr, "Listen failed\n");
        return;
    }
    printf("Satellite receiver listening on port %d\n", DEMO_PORT);

    while (true) {
        csp_conn_t *conn = csp_accept(sock, CSP_MAX_TIMEOUT);
        if (!conn) {
            continue;
        }
        printf("Accepted connection from node %d\n", csp_conn_src(conn));

        while (true) {
            csp_packet_t *packet = csp_read(conn, 1000);
            if (!packet) {
                break;
            }
            printf("Received %u bytes: %.*s\n", packet->length, (int) packet->length, (char *) packet->data);
            csp_buffer_free(packet);
        }

        csp_close(conn);
    }
}

int main(void) {
    csp_conf.hostname = "satellite";
    csp_init();

    if (kiss_tcp_iface_init(&g_kiss_iface, "KISS-TCP", KISS_TCP_HOST, KISS_TCP_PORT) != CSP_ERR_NONE) {
        fprintf(stderr, "Failed to init KISS TCP interface\n");
        return 1;
    }
    if (start_router_task() != CSP_ERR_NONE) {
        fprintf(stderr, "Router task start failed\n");
        return 1;
    }

    run_receiver();
    router_running = false;
    pthread_join(router_thread, NULL);
    return 0;
}
