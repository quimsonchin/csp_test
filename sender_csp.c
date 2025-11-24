
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
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
    int sockfd;
    pthread_t rx_thread;
    volatile bool run;
} kiss_tcp_driver_t;

typedef struct {
    csp_iface_t iface;
    csp_kiss_interface_data_t kiss_state;
    kiss_tcp_driver_t driver;
} kiss_tcp_iface_t;

static kiss_tcp_iface_t g_kiss_iface;
static pthread_t router_thread;
static volatile bool router_running = true;

static int kiss_tcp_driver_tx(void *driver_data, const uint8_t *data, size_t len) {
    kiss_tcp_driver_t *ctx = driver_data;
    ssize_t sent = send(ctx->sockfd, data, len, 0);
    return (sent == (ssize_t) len) ? CSP_ERR_NONE : CSP_ERR_TX;
}

static void *kiss_tcp_rx_loop(void *arg) {
    kiss_tcp_iface_t *ctx = arg;
    uint8_t buf[512];
    while (ctx->driver.run) {
        ssize_t rd = recv(ctx->driver.sockfd, buf, sizeof(buf), 0);
        if (rd > 0) {
            csp_kiss_rx(&ctx->iface, buf, (size_t) rd, NULL);
        } else if (rd == 0) {
            fprintf(stderr, "kiss-tcp: peer closed connection\n");
            break;
        } else if (errno != EINTR) {
            perror("kiss-tcp: recv");
            break;
        }
    }
    ctx->driver.run = false;
    return NULL;
}

static int kiss_tcp_iface_init(kiss_tcp_iface_t *ctx, const char *name,
                               const char *host, uint16_t port) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->iface.name = name;
    ctx->iface.addr = LAND_NODE_ADDR;
    ctx->iface.netmask = 8;
    ctx->iface.nexthop = csp_kiss_tx;
    ctx->iface.interface_data = &ctx->kiss_state;
    ctx->iface.driver_data = &ctx->driver;
    ctx->iface.is_default = 1;

    ctx->kiss_state.tx_func = kiss_tcp_driver_tx;
    ctx->kiss_state.rx_mode = KISS_MODE_NOT_STARTED;
    ctx->kiss_state.rx_first = true;

    ctx->driver.sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->driver.sockfd < 0) {
        perror("socket");
        return CSP_ERR_NOMEM;
    }

    struct sockaddr_in remote = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, host, &remote.sin_addr) != 1) {
        fprintf(stderr, "Invalid KISS host %s\n", host);
        return CSP_ERR_INVAL;
    }

    if (connect(ctx->driver.sockfd, (struct sockaddr *) &remote, sizeof(remote)) < 0) {
        perror("connect");
        return CSP_ERR_NOCONN;
    }

    if (csp_kiss_add_interface(&ctx->iface) != CSP_ERR_NONE) {
        fprintf(stderr, "Failed to add KISS interface\n");
        return CSP_ERR_NOMEM;
    }
    csp_iflist_add(&ctx->iface);
    csp_rtable_set(0, 0, &ctx->iface, CSP_NO_VIA_ADDRESS);

    ctx->driver.run = true;
    if (pthread_create(&ctx->driver.rx_thread, NULL, kiss_tcp_rx_loop, ctx) != 0) {
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

static void send_demo_payload(const char *message) {
    csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, SAT_NODE_ADDR, DEMO_PORT, CSP_MAX_TIMEOUT, 0);
    if (!conn) {
        fprintf(stderr, "Failed to connect to node %u\n", SAT_NODE_ADDR);
        return;
    }

    size_t len = strlen(message) + 1;
    csp_packet_t *packet = csp_buffer_get(len);
    if (!packet) {
        fprintf(stderr, "No free CSP buffers\n");
        csp_close(conn);
        return;
    }

    memcpy(packet->data, message, len);
    packet->length = len;
    csp_send(conn, packet);
    csp_close(conn);
}

int main(void) {
    csp_conf.hostname = "ground-station";
    csp_init();

    if (kiss_tcp_iface_init(&g_kiss_iface, "KISS-TCP", KISS_TCP_HOST, KISS_TCP_PORT) != CSP_ERR_NONE) {
        fprintf(stderr, "KISS interface init failed\n");
        return 1;
    }
    if (start_router_task() != CSP_ERR_NONE) {
        fprintf(stderr, "Router task start failed\n");
        return 1;
    }

    puts("Ground sender ready; waiting for instructions...");
    send_demo_payload("Ground says hello over CSP/KISS-TCP");

    router_running = false;
    pthread_join(router_thread, NULL);
    return 0;
}
