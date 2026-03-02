#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define DEFAULT_REG_IP "193.136.138.142"
#define DEFAULT_REG_UDP 59000

#define MAX_NEIGHBORS 64
#define MAX_DESTS 100
#define MAX_LINE 1024
#define MAX_READ_BUFFER 4096
#define MAX_CHAT_LEN 128
#define INF_DISTANCE 1000000000

typedef struct {
    int active;
    int fd;
    int id;
    char ip[INET_ADDRSTRLEN];
    int port;
    char inbuf[MAX_READ_BUFFER];
    size_t inlen;
} Neighbor;

typedef struct {
    int dist;
    int succ;
    int state; /* 0 = forwarding, 1 = coordination */
    int succ_coord;
    unsigned char coord_pending[MAX_NEIGHBORS];
} RouteEntry;

typedef struct {
    int running;
    int joined;
    int direct_mode;
    int monitor;

    char my_ip[INET_ADDRSTRLEN];
    int my_tcp_port;

    char reg_ip[INET_ADDRSTRLEN];
    int reg_udp_port;
    int udp_fd;
    struct sockaddr_in reg_addr;

    int listen_fd;
    char net[4];
    int my_id;

    Neighbor neighbors[MAX_NEIGHBORS];
    RouteEntry routes[MAX_DESTS];
} OWRContext;

static void trim_line(char *s);
static int parse_int_in_range(const char *s, int min_v, int max_v, int *out);
static int parse_net(const char *s, char out[4]);
static int parse_id(const char *s, int *id_out);
static int parse_port(const char *s, int *port_out);
static void print_usage(const char *prog);
static int send_all(int fd, const char *buf, size_t len);
static int send_line_fd(int fd, const char *fmt, ...);
static void monitor_tx(OWRContext *ctx, int to_id, const char *fmt, ...);
static void monitor_rx(OWRContext *ctx, int from_id, const char *fmt, ...);
static int find_neighbor_by_id(const OWRContext *ctx, int id);
static int alloc_neighbor_slot(OWRContext *ctx);
static void reset_routing(OWRContext *ctx);
static void broadcast_route_for_dest(OWRContext *ctx, int dest);
static void maybe_finish_coordination(OWRContext *ctx, int dest);
static void enter_coordination(OWRContext *ctx, int dest, int succ_coord_value);
static void on_route_received(OWRContext *ctx, int from_id, int dest, int n);
static void on_coord_received(OWRContext *ctx, int from_id, int dest);
static void on_uncoord_received(OWRContext *ctx, int from_id, int dest);
static void on_edge_added(OWRContext *ctx, int idx);
static void on_edge_removed(OWRContext *ctx, int removed_id, int removed_idx);
static void disconnect_neighbor(OWRContext *ctx, int idx, int trigger_routing);
static int init_listen_socket(const char *ip, int port);
static int init_udp_socket(OWRContext *ctx);
static int registry_request(OWRContext *ctx, const char *request, char *response, size_t response_size);
static int registry_register(OWRContext *ctx, const char *net, int id);
static int registry_unregister(OWRContext *ctx, const char *net, int id);
static int registry_contact_query(OWRContext *ctx, const char *net, int id, char *ip_out, size_t ip_out_size, int *port_out);
static void cmd_join(OWRContext *ctx, const char *net_s, const char *id_s, int direct);
static void cmd_leave(OWRContext *ctx);
static void cmd_show_nodes(OWRContext *ctx, const char *net_s);
static int connect_to_neighbor(OWRContext *ctx, int id, const char *ip, int port);
static void cmd_add_edge(OWRContext *ctx, const char *id_s);
static void cmd_direct_add_edge(OWRContext *ctx, const char *id_s, const char *ip_s, const char *port_s);
static void cmd_remove_edge(OWRContext *ctx, const char *id_s);
static void cmd_show_neighbors(OWRContext *ctx);
static void cmd_announce(OWRContext *ctx);
static void cmd_show_routing(OWRContext *ctx, const char *dest_s);
static void cmd_start_monitor(OWRContext *ctx);
static void cmd_end_monitor(OWRContext *ctx);
static void cmd_send_message(OWRContext *ctx, int dest, const char *message);
static void process_neighbor_line(OWRContext *ctx, int idx, char *line);
static void handle_neighbor_io(OWRContext *ctx, int idx);
static void handle_accept(OWRContext *ctx);
static int parse_message_command(const char *line, int *dest_out, char *message_out, size_t message_out_size);
static void process_command(OWRContext *ctx, char *line);

static void trim_line(char *s) {
    size_t n;
    if (s == NULL) {
        return;
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int parse_int_in_range(const char *s, int min_v, int max_v, int *out) {
    long v;
    char *endptr = NULL;
    if (s == NULL || *s == '\0') {
        return 0;
    }
    errno = 0;
    v = strtol(s, &endptr, 10);
    if (errno != 0 || endptr == s || *endptr != '\0') {
        return 0;
    }
    if (v < min_v || v > max_v) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int parse_net(const char *s, char out[4]) {
    int v;
    if (!parse_int_in_range(s, 0, 999, &v)) {
        return 0;
    }
    (void)snprintf(out, 4, "%03d", v);
    return 1;
}

static int parse_id(const char *s, int *id_out) {
    return parse_int_in_range(s, 0, 99, id_out);
}

static int parse_port(const char *s, int *port_out) {
    return parse_int_in_range(s, 1, 65535, port_out);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s IP TCP [regIP regUDP]\n", prog);
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, buf + sent, len - sent);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

static int send_line_fd(int fd, const char *fmt, ...) {
    char line[MAX_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(line) - 1) {
        return -1;
    }

    line[n++] = '\n';
    line[n] = '\0';
    return send_all(fd, line, (size_t)n);
}

static void monitor_tx(OWRContext *ctx, int to_id, const char *fmt, ...) {
    char msg[MAX_LINE];
    va_list ap;
    if (!ctx->monitor) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("[MON][TX][to %02d] %s\n", to_id, msg);
    fflush(stdout);
}

static void monitor_rx(OWRContext *ctx, int from_id, const char *fmt, ...) {
    char msg[MAX_LINE];
    va_list ap;
    if (!ctx->monitor) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("[MON][RX][from %02d] %s\n", from_id, msg);
    fflush(stdout);
}

static int find_neighbor_by_id(const OWRContext *ctx, int id) {
    int i;
    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (ctx->neighbors[i].active && ctx->neighbors[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int alloc_neighbor_slot(OWRContext *ctx) {
    int i;
    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (!ctx->neighbors[i].active) {
            ctx->neighbors[i].active = 1;
            ctx->neighbors[i].fd = -1;
            ctx->neighbors[i].id = -1;
            ctx->neighbors[i].ip[0] = '\0';
            ctx->neighbors[i].port = 0;
            ctx->neighbors[i].inlen = 0;
            memset(ctx->neighbors[i].inbuf, 0, sizeof(ctx->neighbors[i].inbuf));
            return i;
        }
    }
    return -1;
}

static void send_route_msg(OWRContext *ctx, int idx, int dest, int dist) {
    int d = dist;
    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    if (d >= INF_DISTANCE) {
        d = INF_DISTANCE;
    }
    monitor_tx(ctx, ctx->neighbors[idx].id, "ROUTE %02d %d", dest, d);
    if (send_line_fd(ctx->neighbors[idx].fd, "ROUTE %02d %d", dest, d) < 0) {
        disconnect_neighbor(ctx, idx, 1);
    }
}

static void send_coord_msg(OWRContext *ctx, int idx, int dest) {
    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    monitor_tx(ctx, ctx->neighbors[idx].id, "COORD %02d", dest);
    if (send_line_fd(ctx->neighbors[idx].fd, "COORD %02d", dest) < 0) {
        disconnect_neighbor(ctx, idx, 1);
    }
}

static void send_uncoord_msg(OWRContext *ctx, int idx, int dest) {
    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    monitor_tx(ctx, ctx->neighbors[idx].id, "UNCOORD %02d", dest);
    if (send_line_fd(ctx->neighbors[idx].fd, "UNCOORD %02d", dest) < 0) {
        disconnect_neighbor(ctx, idx, 1);
    }
}

static void send_chat_msg(OWRContext *ctx, int idx, int origin, int dest, const char *chat) {
    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    if (send_line_fd(ctx->neighbors[idx].fd, "CHAT %02d %02d %s", origin, dest, chat) < 0) {
        disconnect_neighbor(ctx, idx, 1);
    }
}

static void send_neighbor_msg(OWRContext *ctx, int idx) {
    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    if (send_line_fd(ctx->neighbors[idx].fd, "NEIGHBOR %02d", ctx->my_id) < 0) {
        disconnect_neighbor(ctx, idx, 1);
    }
}

static void reset_routing(OWRContext *ctx) {
    int d;
    for (d = 0; d < MAX_DESTS; d++) {
        ctx->routes[d].dist = INF_DISTANCE;
        ctx->routes[d].succ = -1;
        ctx->routes[d].state = 0;
        ctx->routes[d].succ_coord = -1;
        memset(ctx->routes[d].coord_pending, 0, sizeof(ctx->routes[d].coord_pending));
    }
    if (ctx->joined && ctx->my_id >= 0 && ctx->my_id < MAX_DESTS) {
        ctx->routes[ctx->my_id].dist = 0;
        ctx->routes[ctx->my_id].succ = ctx->my_id;
    }
}

static void broadcast_route_for_dest(OWRContext *ctx, int dest) {
    int i;
    if (dest < 0 || dest >= MAX_DESTS) {
        return;
    }
    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (!ctx->neighbors[i].active || ctx->neighbors[i].id < 0) {
            continue;
        }
        send_route_msg(ctx, i, dest, ctx->routes[dest].dist);
    }
}

static void maybe_finish_coordination(OWRContext *ctx, int dest) {
    int i;
    int succ_coord_id;
    int succ_coord_idx;
    RouteEntry *r;

    if (dest < 0 || dest >= MAX_DESTS) {
        return;
    }
    r = &ctx->routes[dest];
    if (r->state != 1) {
        return;
    }

    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (r->coord_pending[i]) {
            return;
        }
    }

    succ_coord_id = r->succ_coord;
    r->state = 0;
    r->succ_coord = -1;

    if (r->dist < INF_DISTANCE) {
        broadcast_route_for_dest(ctx, dest);
    }

    if (succ_coord_id != -1) {
        succ_coord_idx = find_neighbor_by_id(ctx, succ_coord_id);
        if (succ_coord_idx >= 0) {
            send_uncoord_msg(ctx, succ_coord_idx, dest);
        }
    }
}

static void enter_coordination(OWRContext *ctx, int dest, int succ_coord_value) {
    int i;
    int sent_any = 0;
    RouteEntry *r;

    if (dest < 0 || dest >= MAX_DESTS) {
        return;
    }
    r = &ctx->routes[dest];
    if (r->state == 1) {
        return;
    }

    r->state = 1;
    r->succ_coord = succ_coord_value;
    r->dist = INF_DISTANCE;
    r->succ = -1;
    memset(r->coord_pending, 0, sizeof(r->coord_pending));

    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (!ctx->neighbors[i].active || ctx->neighbors[i].id < 0) {
            continue;
        }
        r->coord_pending[i] = 1;
        sent_any = 1;
        send_coord_msg(ctx, i, dest);
    }

    if (!sent_any) {
        maybe_finish_coordination(ctx, dest);
    }
}

static void on_route_received(OWRContext *ctx, int from_id, int dest, int n) {
    int candidate;
    RouteEntry *r;

    if (dest < 0 || dest >= MAX_DESTS || from_id < 0) {
        return;
    }
    if (n < 0) {
        n = INF_DISTANCE;
    }
    if (n >= INF_DISTANCE - 1) {
        candidate = INF_DISTANCE;
    } else {
        candidate = n + 1;
    }

    r = &ctx->routes[dest];
    if (candidate < r->dist) {
        r->dist = candidate;
        r->succ = from_id;
        if (r->state == 0) {
            broadcast_route_for_dest(ctx, dest);
        }
    }
}

static void on_coord_received(OWRContext *ctx, int from_id, int dest) {
    RouteEntry *r;
    int idx;

    if (dest < 0 || dest >= MAX_DESTS || from_id < 0) {
        return;
    }
    idx = find_neighbor_by_id(ctx, from_id);
    if (idx < 0) {
        return;
    }
    r = &ctx->routes[dest];

    if (r->state == 1) {
        send_uncoord_msg(ctx, idx, dest);
        return;
    }

    if (from_id != r->succ) {
        send_route_msg(ctx, idx, dest, r->dist);
        send_uncoord_msg(ctx, idx, dest);
        return;
    }

    enter_coordination(ctx, dest, r->succ);
}

static void on_uncoord_received(OWRContext *ctx, int from_id, int dest) {
    RouteEntry *r;
    int idx;

    if (dest < 0 || dest >= MAX_DESTS || from_id < 0) {
        return;
    }
    r = &ctx->routes[dest];
    if (r->state != 1) {
        return;
    }
    idx = find_neighbor_by_id(ctx, from_id);
    if (idx >= 0) {
        r->coord_pending[idx] = 0;
    }
    maybe_finish_coordination(ctx, dest);
}

static void on_edge_added(OWRContext *ctx, int idx) {
    int d;
    if (!ctx->joined || idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active || ctx->neighbors[idx].id < 0) {
        return;
    }
    for (d = 0; d < MAX_DESTS; d++) {
        if (ctx->routes[d].state == 0) {
            if (ctx->routes[d].dist < INF_DISTANCE) {
                send_route_msg(ctx, idx, d, ctx->routes[d].dist);
            }
        } else {
            ctx->routes[d].coord_pending[idx] = 0;
        }
    }
}

static void on_edge_removed(OWRContext *ctx, int removed_id, int removed_idx) {
    int d;
    for (d = 0; d < MAX_DESTS; d++) {
        RouteEntry *r = &ctx->routes[d];
        if (r->state == 0) {
            if (r->succ == removed_id) {
                enter_coordination(ctx, d, -1);
            }
        } else {
            if (r->succ == removed_id) {
                r->succ = -1;
                r->dist = INF_DISTANCE;
            }
            if (removed_idx >= 0 && removed_idx < MAX_NEIGHBORS) {
                r->coord_pending[removed_idx] = 0;
            }
            maybe_finish_coordination(ctx, d);
        }
    }
}

static void disconnect_neighbor(OWRContext *ctx, int idx, int trigger_routing) {
    int old_id;
    int d;
    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }

    old_id = ctx->neighbors[idx].id;
    if (ctx->neighbors[idx].fd >= 0) {
        close(ctx->neighbors[idx].fd);
    }
    ctx->neighbors[idx].active = 0;
    ctx->neighbors[idx].fd = -1;
    ctx->neighbors[idx].id = -1;
    ctx->neighbors[idx].ip[0] = '\0';
    ctx->neighbors[idx].port = 0;
    ctx->neighbors[idx].inlen = 0;
    memset(ctx->neighbors[idx].inbuf, 0, sizeof(ctx->neighbors[idx].inbuf));

    for (d = 0; d < MAX_DESTS; d++) {
        ctx->routes[d].coord_pending[idx] = 0;
    }

    if (trigger_routing && ctx->joined && old_id >= 0) {
        on_edge_removed(ctx, old_id, idx);
    }
}

static int init_listen_socket(const char *ip, int port) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(TCP)");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid local IP: %s\n", ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(TCP)");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static int init_udp_socket(OWRContext *ctx) {
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(UDP)");
        return -1;
    }

    memset(&ctx->reg_addr, 0, sizeof(ctx->reg_addr));
    ctx->reg_addr.sin_family = AF_INET;
    ctx->reg_addr.sin_port = htons((unsigned short)ctx->reg_udp_port);
    if (inet_pton(AF_INET, ctx->reg_ip, &ctx->reg_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid registry IP: %s\n", ctx->reg_ip);
        close(fd);
        return -1;
    }

    ctx->udp_fd = fd;
    return 0;
}

static int registry_request(OWRContext *ctx, const char *request, char *response, size_t response_size) {
    fd_set rfds;
    struct timeval tv;
    ssize_t n;
    int ready;

    if (ctx->udp_fd < 0) {
        return -1;
    }
    if (sendto(ctx->udp_fd, request, strlen(request), 0, (struct sockaddr *)&ctx->reg_addr, sizeof(ctx->reg_addr)) < 0) {
        perror("sendto(registry)");
        return -1;
    }

    FD_ZERO(&rfds);
    FD_SET(ctx->udp_fd, &rfds);
    tv.tv_sec = 3;
    tv.tv_usec = 0;

    ready = select(ctx->udp_fd + 1, &rfds, NULL, NULL, &tv);
    if (ready < 0) {
        perror("select(registry)");
        return -1;
    }
    if (ready == 0) {
        fprintf(stderr, "Registry timeout\n");
        return -1;
    }

    n = recvfrom(ctx->udp_fd, response, response_size - 1, 0, NULL, NULL);
    if (n < 0) {
        perror("recvfrom(registry)");
        return -1;
    }
    response[n] = '\0';
    return 0;
}

static int registry_register(OWRContext *ctx, const char *net, int id) {
    char req[MAX_LINE];
    char resp[MAX_LINE];
    char ctrl[32];

    (void)snprintf(req, sizeof(req), "REG %s %02d %s %d\n", net, id, ctx->my_ip, ctx->my_tcp_port);
    if (registry_request(ctx, req, resp, sizeof(resp)) < 0) {
        return -1;
    }
    trim_line(resp);
    if (sscanf(resp, "OKREG %31s", ctrl) != 1) {
        fprintf(stderr, "Unexpected REG response: %s\n", resp);
        return -1;
    }
    if (strcmp(ctrl, "DONE") == 0) {
        return 0;
    }
    if (strcmp(ctrl, "DUP") == 0) {
        return 1;
    }
    fprintf(stderr, "Unknown REG control message: %s\n", ctrl);
    return -1;
}

static int registry_unregister(OWRContext *ctx, const char *net, int id) {
    char req[MAX_LINE];
    char resp[MAX_LINE];
    char ctrl[32];

    (void)snprintf(req, sizeof(req), "UNREG %s %02d\n", net, id);
    if (registry_request(ctx, req, resp, sizeof(resp)) < 0) {
        return -1;
    }
    trim_line(resp);
    if (sscanf(resp, "OKUNREG %31s", ctrl) != 1) {
        fprintf(stderr, "Unexpected UNREG response: %s\n", resp);
        return -1;
    }
    if (strcmp(ctrl, "DONE") == 0 || strcmp(ctrl, "NULL") == 0) {
        return 0;
    }
    fprintf(stderr, "Unknown UNREG control message: %s\n", ctrl);
    return -1;
}

static int registry_contact_query(OWRContext *ctx, const char *net, int id, char *ip_out, size_t ip_out_size, int *port_out) {
    char req[MAX_LINE];
    char resp[MAX_LINE];
    char net_resp[4];
    int id_resp;
    char ip[INET_ADDRSTRLEN];
    int port;

    (void)snprintf(req, sizeof(req), "CONTACTQUERY %s %02d\n", net, id);
    if (registry_request(ctx, req, resp, sizeof(resp)) < 0) {
        return -1;
    }
    trim_line(resp);

    if (sscanf(resp, "CONTACTRESP %3s %d %15s %d", net_resp, &id_resp, ip, &port) != 4) {
        fprintf(stderr, "Unexpected CONTACT response: %s\n", resp);
        return -1;
    }

    if (strcmp(net_resp, net) != 0 || id_resp != id) {
        fprintf(stderr, "CONTACT response mismatch: %s\n", resp);
        return -1;
    }

    (void)snprintf(ip_out, ip_out_size, "%s", ip);
    *port_out = port;
    return 0;
}

static void cmd_join(OWRContext *ctx, const char *net_s, const char *id_s, int direct) {
    char net[4];
    int id;
    int reg_status;

    if (ctx->joined) {
        printf("Already joined: net=%s id=%02d\n", ctx->net, ctx->my_id);
        return;
    }
    if (!parse_net(net_s, net)) {
        printf("Invalid net. Use value 000..999.\n");
        return;
    }
    if (!parse_id(id_s, &id)) {
        printf("Invalid id. Use value 00..99.\n");
        return;
    }

    if (!direct) {
        reg_status = registry_register(ctx, net, id);
        if (reg_status < 0) {
            printf("Join failed: registry error.\n");
            return;
        }
        if (reg_status == 1) {
            printf("Join failed: id %02d already exists in net %s.\n", id, net);
            return;
        }
    }

    ctx->joined = 1;
    ctx->direct_mode = direct;
    (void)snprintf(ctx->net, sizeof(ctx->net), "%s", net);
    ctx->my_id = id;
    reset_routing(ctx);

    printf("Joined net %s as node %02d (%s mode).\n", ctx->net, ctx->my_id, direct ? "direct" : "registry");
}

static void cmd_leave(OWRContext *ctx) {
    int i;

    if (!ctx->joined) {
        printf("Not joined.\n");
        return;
    }

    if (!ctx->direct_mode) {
        if (registry_unregister(ctx, ctx->net, ctx->my_id) < 0) {
            printf("Warning: registry unregistration failed.\n");
        }
    }

    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (ctx->neighbors[i].active) {
            disconnect_neighbor(ctx, i, 0);
        }
    }

    ctx->joined = 0;
    ctx->direct_mode = 0;
    ctx->net[0] = '\0';
    ctx->my_id = -1;
    reset_routing(ctx);

    printf("Left overlay network.\n");
}

static void cmd_show_nodes(OWRContext *ctx, const char *net_s) {
    char net[4];
    char req[MAX_LINE];
    char resp[MAX_LINE];
    char *saveptr = NULL;
    char *line;
    char net_resp[4];

    if (!parse_net(net_s, net)) {
        printf("Invalid net. Use value 000..999.\n");
        return;
    }

    (void)snprintf(req, sizeof(req), "NODESQUERY %s\n", net);
    if (registry_request(ctx, req, resp, sizeof(resp)) < 0) {
        printf("show nodes failed: registry error.\n");
        return;
    }

    line = strtok_r(resp, "\n", &saveptr);
    if (line == NULL || sscanf(line, "NODESRESP %3s", net_resp) != 1) {
        printf("Unexpected NODES response.\n");
        return;
    }

    printf("Nodes in net %s:\n", net_resp);
    while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL) {
        trim_line(line);
        if (*line != '\0') {
            printf("%s\n", line);
        }
    }
}

static int connect_to_neighbor(OWRContext *ctx, int id, const char *ip, int port) {
    int fd;
    int idx;
    struct sockaddr_in addr;

    if (!ctx->joined) {
        printf("Join the network first.\n");
        return -1;
    }
    if (id == ctx->my_id) {
        printf("Cannot add edge to self.\n");
        return -1;
    }
    if (find_neighbor_by_id(ctx, id) >= 0) {
        printf("Edge with node %02d already exists.\n", id);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(connect)");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        printf("Invalid neighbor IP: %s\n", ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    idx = alloc_neighbor_slot(ctx);
    if (idx < 0) {
        printf("Too many neighbors.\n");
        close(fd);
        return -1;
    }

    ctx->neighbors[idx].fd = fd;
    ctx->neighbors[idx].id = id;
    (void)snprintf(ctx->neighbors[idx].ip, sizeof(ctx->neighbors[idx].ip), "%s", ip);
    ctx->neighbors[idx].port = port;
    ctx->neighbors[idx].inlen = 0;

    send_neighbor_msg(ctx, idx);
    on_edge_added(ctx, idx);
    printf("Edge added with %02d (%s:%d).\n", id, ip, port);
    return 0;
}

static void cmd_add_edge(OWRContext *ctx, const char *id_s) {
    int id;
    char ip[INET_ADDRSTRLEN];
    int port;

    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    if (!parse_id(id_s, &id)) {
        printf("Invalid id.\n");
        return;
    }
    if (registry_contact_query(ctx, ctx->net, id, ip, sizeof(ip), &port) < 0) {
        printf("add edge failed: contact query failed.\n");
        return;
    }
    (void)connect_to_neighbor(ctx, id, ip, port);
}

static void cmd_direct_add_edge(OWRContext *ctx, const char *id_s, const char *ip_s, const char *port_s) {
    int id;
    int port;
    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    if (!parse_id(id_s, &id) || !parse_port(port_s, &port)) {
        printf("Invalid id or TCP port.\n");
        return;
    }
    (void)connect_to_neighbor(ctx, id, ip_s, port);
}

static void cmd_remove_edge(OWRContext *ctx, const char *id_s) {
    int id;
    int idx;

    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    if (!parse_id(id_s, &id)) {
        printf("Invalid id.\n");
        return;
    }
    idx = find_neighbor_by_id(ctx, id);
    if (idx < 0) {
        printf("No edge with node %02d.\n", id);
        return;
    }
    disconnect_neighbor(ctx, idx, 1);
    printf("Edge removed with %02d.\n", id);
}

static void cmd_show_neighbors(OWRContext *ctx) {
    int i;
    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    printf("Neighbors:\n");
    for (i = 0; i < MAX_NEIGHBORS; i++) {
        if (!ctx->neighbors[i].active) {
            continue;
        }
        if (ctx->neighbors[i].id >= 0) {
            printf("- id=%02d fd=%d\n", ctx->neighbors[i].id, ctx->neighbors[i].fd);
        } else {
            printf("- id=?? fd=%d (waiting NEIGHBOR)\n", ctx->neighbors[i].fd);
        }
    }
}

static void cmd_announce(OWRContext *ctx) {
    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    ctx->routes[ctx->my_id].dist = 0;
    ctx->routes[ctx->my_id].succ = ctx->my_id;
    ctx->routes[ctx->my_id].state = 0;
    ctx->routes[ctx->my_id].succ_coord = -1;
    memset(ctx->routes[ctx->my_id].coord_pending, 0, sizeof(ctx->routes[ctx->my_id].coord_pending));
    broadcast_route_for_dest(ctx, ctx->my_id);
    printf("Announced destination %02d to neighbors.\n", ctx->my_id);
}

static void cmd_show_routing(OWRContext *ctx, const char *dest_s) {
    int dest;
    RouteEntry *r;

    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    if (!parse_id(dest_s, &dest)) {
        printf("Invalid destination id.\n");
        return;
    }
    r = &ctx->routes[dest];
    if (r->state == 0) {
        if (r->dist >= INF_DISTANCE || r->succ < 0) {
            printf("dest=%02d state=forwarding dist=INF succ=-1\n", dest);
        } else {
            printf("dest=%02d state=forwarding dist=%d succ=%02d\n", dest, r->dist, r->succ);
        }
    } else {
        if (r->dist >= INF_DISTANCE || r->succ < 0) {
            printf("dest=%02d state=coordination dist=INF succ=-1 succ_coord=%02d\n", dest, r->succ_coord);
        } else {
            printf("dest=%02d state=coordination dist=%d succ=%02d succ_coord=%02d\n", dest, r->dist, r->succ, r->succ_coord);
        }
    }
}

static void cmd_start_monitor(OWRContext *ctx) {
    ctx->monitor = 1;
    printf("Routing monitor enabled.\n");
}

static void cmd_end_monitor(OWRContext *ctx) {
    ctx->monitor = 0;
    printf("Routing monitor disabled.\n");
}

static void cmd_send_message(OWRContext *ctx, int dest, const char *message) {
    int idx;
    RouteEntry *r;
    char chat[MAX_CHAT_LEN + 1];

    if (!ctx->joined) {
        printf("Join the network first.\n");
        return;
    }
    if (dest == ctx->my_id) {
        printf("[CHAT][local %02d] %s\n", ctx->my_id, message);
        return;
    }

    memset(chat, 0, sizeof(chat));
    (void)snprintf(chat, sizeof(chat), "%.*s", MAX_CHAT_LEN, message);

    r = &ctx->routes[dest];
    if (r->state != 0 || r->succ < 0 || r->dist >= INF_DISTANCE) {
        printf("No forwarding route to %02d.\n", dest);
        return;
    }

    idx = find_neighbor_by_id(ctx, r->succ);
    if (idx < 0) {
        printf("Successor %02d for destination %02d is not connected.\n", r->succ, dest);
        return;
    }

    send_chat_msg(ctx, idx, ctx->my_id, dest, chat);
    printf("CHAT sent to %02d via %02d.\n", dest, r->succ);
}

static void process_neighbor_line(OWRContext *ctx, int idx, char *line) {
    int id;
    int dest;
    int n;
    int origin;
    int existing_idx;
    char msg[MAX_LINE];
    Neighbor *nb;

    trim_line(line);
    if (*line == '\0') {
        return;
    }

    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    nb = &ctx->neighbors[idx];

    if (sscanf(line, "NEIGHBOR %d", &id) == 1) {
        if (id < 0 || id >= MAX_DESTS) {
            printf("Ignoring invalid NEIGHBOR id: %s\n", line);
            return;
        }
        if (nb->id == -1) {
            existing_idx = find_neighbor_by_id(ctx, id);
            if (existing_idx >= 0 && existing_idx != idx) {
                printf("Duplicate edge with %02d detected; closing one connection.\n", id);
                disconnect_neighbor(ctx, idx, 1);
                return;
            }
            nb->id = id;
            on_edge_added(ctx, idx);
            printf("Edge established with %02d.\n", id);
        } else if (nb->id != id) {
            printf("Neighbor id mismatch on fd %d (%02d != %02d), closing.\n", nb->fd, nb->id, id);
            disconnect_neighbor(ctx, idx, 1);
        }
        return;
    }

    if (nb->id < 0) {
        return;
    }

    if (sscanf(line, "ROUTE %d %d", &dest, &n) == 2) {
        monitor_rx(ctx, nb->id, "ROUTE %02d %d", dest, n);
        on_route_received(ctx, nb->id, dest, n);
        return;
    }
    if (sscanf(line, "COORD %d", &dest) == 1) {
        monitor_rx(ctx, nb->id, "COORD %02d", dest);
        on_coord_received(ctx, nb->id, dest);
        return;
    }
    if (sscanf(line, "UNCOORD %d", &dest) == 1) {
        monitor_rx(ctx, nb->id, "UNCOORD %02d", dest);
        on_uncoord_received(ctx, nb->id, dest);
        return;
    }
    if (sscanf(line, "CHAT %d %d %1023[^\n]", &origin, &dest, msg) == 3) {
        if (dest == ctx->my_id) {
            printf("[CHAT][from %02d] %s\n", origin, msg);
            return;
        }
        if (dest >= 0 && dest < MAX_DESTS) {
            RouteEntry *r = &ctx->routes[dest];
            int succ_idx;
            if (r->state == 0 && r->succ >= 0 && r->dist < INF_DISTANCE) {
                succ_idx = find_neighbor_by_id(ctx, r->succ);
                if (succ_idx >= 0) {
                    send_chat_msg(ctx, succ_idx, origin, dest, msg);
                } else {
                    printf("Dropping CHAT to %02d: successor disconnected.\n", dest);
                }
            } else {
                printf("Dropping CHAT to %02d: no forwarding route.\n", dest);
            }
        }
        return;
    }
}

static void handle_neighbor_io(OWRContext *ctx, int idx) {
    Neighbor *nb;
    char buf[1024];
    ssize_t n;
    size_t i;
    size_t start;

    if (idx < 0 || idx >= MAX_NEIGHBORS || !ctx->neighbors[idx].active) {
        return;
    }
    nb = &ctx->neighbors[idx];

    n = read(nb->fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EINTR) {
            return;
        }
        printf("Read error on neighbor fd %d.\n", nb->fd);
        disconnect_neighbor(ctx, idx, 1);
        return;
    }
    if (n == 0) {
        if (nb->id >= 0) {
            printf("Neighbor %02d disconnected.\n", nb->id);
        } else {
            printf("Unknown neighbor disconnected.\n");
        }
        disconnect_neighbor(ctx, idx, 1);
        return;
    }

    if (nb->inlen + (size_t)n >= sizeof(nb->inbuf)) {
        printf("Input buffer overflow from neighbor fd %d, disconnecting.\n", nb->fd);
        disconnect_neighbor(ctx, idx, 1);
        return;
    }

    memcpy(nb->inbuf + nb->inlen, buf, (size_t)n);
    nb->inlen += (size_t)n;

    start = 0;
    for (i = 0; i < nb->inlen; i++) {
        if (nb->inbuf[i] == '\n') {
            char line[MAX_LINE];
            size_t len = i - start;
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1;
            }
            memcpy(line, nb->inbuf + start, len);
            line[len] = '\0';
            process_neighbor_line(ctx, idx, line);
            start = i + 1;
        }
    }

    if (start > 0) {
        memmove(nb->inbuf, nb->inbuf + start, nb->inlen - start);
        nb->inlen -= start;
    }
}

static void handle_accept(OWRContext *ctx) {
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int cfd;
    int idx;

    cfd = accept(ctx->listen_fd, (struct sockaddr *)&caddr, &clen);
    if (cfd < 0) {
        if (errno != EINTR) {
            perror("accept");
        }
        return;
    }

    if (!ctx->joined) {
        close(cfd);
        return;
    }

    idx = alloc_neighbor_slot(ctx);
    if (idx < 0) {
        printf("Too many neighbors, rejecting new TCP session.\n");
        close(cfd);
        return;
    }

    ctx->neighbors[idx].fd = cfd;
    ctx->neighbors[idx].id = -1;
    ctx->neighbors[idx].inlen = 0;
    if (inet_ntop(AF_INET, &caddr.sin_addr, ctx->neighbors[idx].ip, sizeof(ctx->neighbors[idx].ip)) == NULL) {
        ctx->neighbors[idx].ip[0] = '\0';
    }
    ctx->neighbors[idx].port = ntohs(caddr.sin_port);

    send_neighbor_msg(ctx, idx);
}

static int parse_message_command(const char *line, int *dest_out, char *message_out, size_t message_out_size) {
    const char *p = NULL;
    char idbuf[16];
    int consumed = 0;

    if (strncmp(line, "message ", 8) == 0) {
        p = line + 8;
    } else if (strncmp(line, "m ", 2) == 0) {
        p = line + 2;
    } else {
        return 0;
    }

    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }

    if (sscanf(p, "%15s%n", idbuf, &consumed) != 1) {
        return 0;
    }
    if (!parse_id(idbuf, dest_out)) {
        return 0;
    }
    p += consumed;
    while (*p == ' ') {
        p++;
    }
    (void)snprintf(message_out, message_out_size, "%.*s", (int)(message_out_size - 1), p);
    return 1;
}

static void process_command(OWRContext *ctx, char *line) {
    char a[64], b[64], c[64];
    int dest;
    char message[MAX_CHAT_LEN + 1];

    trim_line(line);
    if (*line == '\0') {
        return;
    }

    if (strcmp(line, "help") == 0 || strcmp(line, "h") == 0) {
        printf("Commands:\n");
        printf("  join|j net id\n");
        printf("  direct join|dj net id\n");
        printf("  show nodes|n net\n");
        printf("  leave|l\n");
        printf("  exit|x\n");
        printf("  add edge|ae id\n");
        printf("  direct add edge|dae id ip tcp\n");
        printf("  remove edge|re id\n");
        printf("  show neighbors|sg\n");
        printf("  announce|a\n");
        printf("  show routing|sr dest\n");
        printf("  start monitor|sm\n");
        printf("  end monitor|em\n");
        printf("  message|m dest text\n");
        return;
    }

    if (sscanf(line, "direct add edge %63s %63s %63s", a, b, c) == 3 ||
        sscanf(line, "dae %63s %63s %63s", a, b, c) == 3) {
        cmd_direct_add_edge(ctx, a, b, c);
        return;
    }
    if (sscanf(line, "direct join %63s %63s", a, b) == 2 || sscanf(line, "dj %63s %63s", a, b) == 2) {
        cmd_join(ctx, a, b, 1);
        return;
    }
    if (sscanf(line, "join %63s %63s", a, b) == 2 || sscanf(line, "j %63s %63s", a, b) == 2) {
        cmd_join(ctx, a, b, 0);
        return;
    }
    if (sscanf(line, "show nodes %63s", a) == 1 || sscanf(line, "n %63s", a) == 1) {
        cmd_show_nodes(ctx, a);
        return;
    }
    if (sscanf(line, "add edge %63s", a) == 1 || sscanf(line, "ae %63s", a) == 1) {
        cmd_add_edge(ctx, a);
        return;
    }
    if (sscanf(line, "remove edge %63s", a) == 1 || sscanf(line, "re %63s", a) == 1) {
        cmd_remove_edge(ctx, a);
        return;
    }
    if (strcmp(line, "show neighbors") == 0 || strcmp(line, "sg") == 0) {
        cmd_show_neighbors(ctx);
        return;
    }
    if (strcmp(line, "announce") == 0 || strcmp(line, "a") == 0) {
        cmd_announce(ctx);
        return;
    }
    if (sscanf(line, "show routing %63s", a) == 1 || sscanf(line, "sr %63s", a) == 1) {
        cmd_show_routing(ctx, a);
        return;
    }
    if (strcmp(line, "start monitor") == 0 || strcmp(line, "sm") == 0) {
        cmd_start_monitor(ctx);
        return;
    }
    if (strcmp(line, "end monitor") == 0 || strcmp(line, "em") == 0) {
        cmd_end_monitor(ctx);
        return;
    }
    if (strcmp(line, "leave") == 0 || strcmp(line, "l") == 0) {
        cmd_leave(ctx);
        return;
    }
    if (strcmp(line, "exit") == 0 || strcmp(line, "x") == 0) {
        if (ctx->joined) {
            cmd_leave(ctx);
        }
        ctx->running = 0;
        return;
    }
    if (parse_message_command(line, &dest, message, sizeof(message))) {
        cmd_send_message(ctx, dest, message);
        return;
    }

    printf("Unknown command. Type 'help'.\n");
}

int main(int argc, char **argv) {
    OWRContext ctx;
    char line[MAX_LINE];
    int local_port;
    fd_set rfds;

    memset(&ctx, 0, sizeof(ctx));
    ctx.running = 1;
    ctx.joined = 0;
    ctx.direct_mode = 0;
    ctx.monitor = 0;
    ctx.my_id = -1;
    ctx.listen_fd = -1;
    ctx.udp_fd = -1;
    memset(ctx.net, 0, sizeof(ctx.net));

    if (argc != 3 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }
    if (strlen(argv[1]) >= sizeof(ctx.my_ip)) {
        fprintf(stderr, "IP is too long.\n");
        return 1;
    }
    if (!parse_port(argv[2], &local_port)) {
        fprintf(stderr, "Invalid local TCP port.\n");
        return 1;
    }

    (void)snprintf(ctx.my_ip, sizeof(ctx.my_ip), "%s", argv[1]);
    ctx.my_tcp_port = local_port;

    if (argc == 5) {
        if (strlen(argv[3]) >= sizeof(ctx.reg_ip)) {
            fprintf(stderr, "regIP is too long.\n");
            return 1;
        }
        if (!parse_port(argv[4], &ctx.reg_udp_port)) {
            fprintf(stderr, "Invalid regUDP port.\n");
            return 1;
        }
        (void)snprintf(ctx.reg_ip, sizeof(ctx.reg_ip), "%s", argv[3]);
    } else {
        (void)snprintf(ctx.reg_ip, sizeof(ctx.reg_ip), "%s", DEFAULT_REG_IP);
        ctx.reg_udp_port = DEFAULT_REG_UDP;
    }

    ctx.listen_fd = init_listen_socket(ctx.my_ip, ctx.my_tcp_port);
    if (ctx.listen_fd < 0) {
        return 1;
    }
    if (init_udp_socket(&ctx) < 0) {
        close(ctx.listen_fd);
        return 1;
    }
    reset_routing(&ctx);

    printf("OWR started on %s:%d, registry %s:%d\n", ctx.my_ip, ctx.my_tcp_port, ctx.reg_ip, ctx.reg_udp_port);
    printf("Type 'help' for available commands.\n");

    while (ctx.running) {
        int i;
        int maxfd = STDIN_FILENO;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        FD_SET(ctx.listen_fd, &rfds);
        if (ctx.listen_fd > maxfd) {
            maxfd = ctx.listen_fd;
        }

        for (i = 0; i < MAX_NEIGHBORS; i++) {
            if (ctx.neighbors[i].active && ctx.neighbors[i].fd >= 0) {
                FD_SET(ctx.neighbors[i].fd, &rfds);
                if (ctx.neighbors[i].fd > maxfd) {
                    maxfd = ctx.neighbors[i].fd;
                }
            }
        }

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                ctx.running = 0;
            } else {
                process_command(&ctx, line);
            }
        }

        if (ctx.listen_fd >= 0 && FD_ISSET(ctx.listen_fd, &rfds)) {
            handle_accept(&ctx);
        }

        for (i = 0; i < MAX_NEIGHBORS; i++) {
            if (ctx.neighbors[i].active && ctx.neighbors[i].fd >= 0 && FD_ISSET(ctx.neighbors[i].fd, &rfds)) {
                handle_neighbor_io(&ctx, i);
            }
        }
    }

    if (ctx.joined) {
        cmd_leave(&ctx);
    }
    if (ctx.listen_fd >= 0) {
        close(ctx.listen_fd);
    }
    if (ctx.udp_fd >= 0) {
        close(ctx.udp_fd);
    }
    return 0;
}
