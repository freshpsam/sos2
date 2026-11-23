#include "network.h"
#include "network_time.h"
#include "page_host.h"
#include "wifi_setup.h"
#include "net_device.h"
#include "user_abi.h"
#include "user_net.h"
#include "platform.h"
#include "entropy.h"
#include "page_alloc.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_wifi.h"
#include "wifi_device.h"
#define network_entropy_read wifi_device_random
#elif defined(SOS2_BOARD_GTA7LITE)
#include "gta7lite.h"
#include "wifi_device.h"
#define network_entropy_read wifi_device_random
#elif defined(SOS2_BOARD_NEXUS5)
#include "nexus5.h"
#include "wifi_device.h"
#define network_entropy_read wifi_device_random
#else
#define network_entropy_read entropy_read
#endif
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/sos2_hooks.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

_Static_assert(sizeof(struct user_net_info) == 40, "native network info ABI");
_Static_assert(sizeof(struct user_net_status) == 20, "native network status ABI");

/* Match advertised credit to the bounded per-socket receive ring. */
enum { SOCKET_COUNT = 2 * USER_NET_SOCKETS, RECEIVE_BYTES = TCP_WND, RANDOM_WORDS = 128 };

enum { RECEIVE_PAGES = (RECEIVE_BYTES + PAGE_SIZE - 1) / PAGE_SIZE };

enum { DNS_PER_PROCESS = 4, DNS_JOBS = 2 * DNS_PER_PROCESS };

/* Every connection lookup plus the SNTP lookup needs its own lwIP table entry. */
_Static_assert(DNS_JOBS < DNS_TABLE_SIZE,
               "lwIP DNS table smaller than connection lookups plus SNTP");

struct connection {
    uint32_t handle, pid, state;
    int error;
    struct tcp_pcb *pcb;
    char host[USER_NET_HOST_MAX + 1];
    uint16_t port;
    bool resolving, eof;
    uint64_t deadline;
    unsigned head, count;
    uint8_t *received;
};
static struct connection connections[SOCKET_COUNT];

/* lwIP's raw DNS API has no cancellation primitive. A closed connection keeps
 * its bounded resolver reservation until the callback/timeout completes. */
static struct {
    uint32_t handle, pid;
} dns_jobs[DNS_JOBS];
static struct net_device device;
static struct netif interface;
static bool started, previous_link, random_failed;
/* The address the desktop shows (0: none), told only when it changes. */
static uint32_t shown_address;
static uint32_t generation, random_words[RANDOM_WORDS];
static unsigned random_head, random_count;
static uint8_t random_partial[sizeof(uint32_t)];
static unsigned random_partial_bytes;
static uint8_t io_buffer[USER_NET_IO_MAX];
static bool broadcast_replies;

void network_dhcp_prepare(struct dhcp *d, struct dhcp_msg *msg, unsigned type) {
    /* Try ordinary unicast delivery first. After an unanswered DISCOVER,
     * request broadcast OFFER/ACK delivery (RFC 2131 section 4.1) rather than
     * retrying the same pre-address unicast path indefinitely. Keep that
     * preference through REQUEST; renewals with ciaddr set remain unicast. */
    if (type == DHCP_DISCOVER && d->tries) broadcast_replies = true;
    if (broadcast_replies && (type == DHCP_DISCOVER || type == DHCP_REQUEST) && !msg->ciaddr.addr)
        msg->flags = PP_HTONS(0x8000u);
}

void network_assert(void) { panic("network stack invariant failed"); }

/* The current poll's time, the main loop's for this turn (loop_time_us), and
 * lwIP's clock, that time in milliseconds, at most a turn old when a system
 * call has lwIP read it. */
static uint64_t poll_us;
static u32_t poll_ms;

u32_t sys_now(void) { return poll_ms; }

uint32_t network_random(void) {
    if (!random_count) {
        /* Fail closed: output() transmits nothing once random_failed is set
         * and the device stops after this poll. lwIP still redraws DNS ports
         * and IDs until they are >=1024 or unique, so never return a constant. */
        static uint32_t spin;
        random_failed = true;
        spin += 0x9e3779b9u;
        return spin | 0x400u;
    }
    uint32_t value = random_words[random_head];
    random_head = (random_head + 1) % RANDOM_WORDS;
    --random_count;
    return value;
}

static void refill_random(void) {
    /* Virtio entropy reads are nonblocking and may end partway through a word.
     * Accumulate those bytes; never replace missing entropy with a clock seed. */
    for (unsigned budget = 0; random_count < RANDOM_WORDS && budget < 64; ++budget) {
        size_t count = network_entropy_read(random_partial + random_partial_bytes,
                                            sizeof(random_partial) - random_partial_bytes);
        if (!count) break;
        random_partial_bytes += (unsigned)count;
        if (random_partial_bytes == sizeof(random_partial)) {
            memcpy(&random_words[(random_head + random_count) % RANDOM_WORDS], random_partial,
                   sizeof(uint32_t));
            ++random_count;
            random_partial_bytes = 0;
        }
    }
}

static int error_code(err_t error) {
    switch (error) {
        case ERR_OK: return 0;
        case ERR_MEM:
        case ERR_BUF: return USER_EAGAIN;
        case ERR_TIMEOUT: return USER_ETIMEDOUT;
        case ERR_RTE: return USER_ENETUNREACH;
        case ERR_RST: return USER_ECONNRESET;
        case ERR_CLSD:
        case ERR_ABRT: return USER_ECONNABORTED;
        default: return USER_EIO;
    }
}

static void detach(struct connection *c) {
    if (c->pcb) {
        tcp_arg(c->pcb, 0);
        tcp_recv(c->pcb, 0);
        tcp_err(c->pcb, 0);
    }
}

static void fail_connection(struct connection *c, int error) {
    detach(c);
    if (c->pcb) tcp_abort(c->pcb);
    c->pcb = 0;
    c->state = NET_FAILED;
    c->error = error;
}

static void connection_error(void *argument, err_t error) {
    struct connection *c = argument;
    c->error =
        error == ERR_RST && c->state == NET_CONNECTING ? USER_ECONNREFUSED : error_code(error);
    c->pcb = 0;
    c->state = NET_FAILED;
}

static err_t receive_data(void *argument, struct tcp_pcb *pcb, struct pbuf *p, err_t error) {
    (void)pcb;
    struct connection *c = argument;
    if (error != ERR_OK) {
        if (p) pbuf_free(p);
        fail_connection(c, error_code(error));
        return ERR_ABRT;
    }
    if (!p) {
        c->eof = true;
        return ERR_OK;
    }
    if (p->tot_len > RECEIVE_BYTES - c->count) return ERR_MEM;
    unsigned tail = (c->head + c->count) % RECEIVE_BYTES, first = RECEIVE_BYTES - tail;
    if (first > p->tot_len) first = p->tot_len;
    pbuf_copy_partial(p, c->received + tail, (u16_t)first, 0);
    pbuf_copy_partial(p, c->received, (u16_t)(p->tot_len - first), (u16_t)first);
    c->count += p->tot_len;
    pbuf_free(p);
    /* Advertise receive credit only after the owning process consumes bytes. */
    return ERR_OK;
}

static err_t connected(void *argument, struct tcp_pcb *pcb, err_t error) {
    struct connection *c = argument;
    (void)pcb;
    if (error != ERR_OK) {
        fail_connection(c, error_code(error));
        return ERR_ABRT;
    }
    c->state = NET_CONNECTED;
    return ERR_OK;
}

static void resolved(const char *name, const ip_addr_t *address, void *argument) {
    (void)name;
    uint32_t handle = (uint32_t)(uintptr_t)argument;
    for (unsigned i = 0; i < DNS_JOBS; ++i)
        if (dns_jobs[i].handle == handle) dns_jobs[i].handle = 0;
    struct connection *c = &connections[handle % SOCKET_COUNT];
    /* A late DNS callback cannot attach to a closed/reused process handle. */
    if (c->handle != handle || c->state != NET_CONNECTING) return;
    if (!address || ip_addr_isany(address) || ip_addr_ismulticast(address) ||
        ip4_addr_get_u32(ip_2_ip4(address)) == 0xffffffffu) {
        fail_connection(c, USER_EHOSTUNREACH);
        return;
    }
    c->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!c->pcb) {
        fail_connection(c, USER_ENOMEM);
        return;
    }
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, receive_data);
    tcp_err(c->pcb, connection_error);
    tcp_nagle_disable(c->pcb);
    err_t error = tcp_connect(c->pcb, address, c->port, connected);
    if (error != ERR_OK) fail_connection(c, error_code(error));
}

static err_t output(struct netif *netif, struct pbuf *packet) {
    (void)netif;
    uint8_t frame[NET_FRAME_MAX];
    if (random_failed || !device.link()) return ERR_IF;
    if (packet->tot_len > sizeof(frame)) return ERR_BUF;
    pbuf_copy_partial(packet, frame, packet->tot_len, 0);
    return device.send(frame, packet->tot_len) ? ERR_OK : ERR_MEM;
}

static err_t configure(struct netif *netif) {
    netif->name[0] = 's';
    netif->name[1] = 'n';
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, device.mac, 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->output = etharp_output;
    netif->linkoutput = output;
    return ERR_OK;
}

static void receive_frame(const void *bytes, unsigned length) {
    if (!started || !previous_link) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (!p) return;
    if (pbuf_take(p, bytes, (u16_t)length) != ERR_OK || interface.input(p, &interface) != ERR_OK)
        pbuf_free(p);
}

static bool addressed(void) {
    return started && device.online() && device.link() && previous_link &&
           !ip4_addr_isany_val(*netif_ip4_addr(&interface));
}

void network_init(const struct platform_devices *devices) {
    network_time_init();
    started = previous_link = random_failed = false;
    shown_address = UINT32_MAX; /* never assigned: the first poll tells it */
    generation = random_head = random_count = random_partial_bytes = 0;
    broadcast_replies = false;
    memset(connections, 0, sizeof(connections));
    memset(dns_jobs, 0, sizeof(dns_jobs));
    memset(&interface, 0, sizeof(interface));
#ifdef SOS2_BOARD_REDMI2
    (void)devices;
    redmi2_wifi_init(&device);
#elif defined(SOS2_BOARD_GTA7LITE)
    (void)devices;
    gta7lite_wifi_init(&device);
#elif defined(SOS2_BOARD_NEXUS5)
    (void)devices;
    nexus5_wifi_init(&device);
#else
    (void)virtio_net_init(devices, &device);
    wifi_setup_init(0, 0);
#endif
}

bool network_startup_pending(void) {
    return device.online && device.online() && (!started || random_count < RANDOM_WORDS);
}

void network_poll(void) {
    poll_us = loop_time_us();
    poll_ms = (u32_t)(poll_us / 1000);
    device.poll(receive_frame);
    uint32_t address = addressed() ? ip4_addr_get_u32(netif_ip4_addr(&interface)) : 0;
    if (address != shown_address) {
        shown_address = address;
        page_host_network_status(address ? ip4addr_ntoa(netif_ip4_addr(&interface)) : 0);
    }
    if (!device.online() && !started) {
        network_time_poll(false, poll_us);
        for (unsigned i = 0; i < SOCKET_COUNT; ++i)
            if (connections[i].handle && connections[i].state != NET_FAILED)
                fail_connection(&connections[i], USER_ENETDOWN);
        return;
    }
    refill_random();
    if (!started) {
        if (random_count < 16) {
            /* lwIP waits for entropy, but accepted sockets keep their deadline. */
            for (unsigned i = 0; i < SOCKET_COUNT; ++i)
                if (connections[i].handle && connections[i].state == NET_CONNECTING &&
                    poll_us >= connections[i].deadline)
                    fail_connection(&connections[i], USER_ETIMEDOUT);
            return;
        }
        lwip_init();
        if (!netif_add(&interface, 0, 0, 0, 0, configure, ethernet_input)) {
            device.stop();
            return;
        }
        netif_set_default(&interface);
        started = true;
    }
    bool link = device.online() && device.link();
    if (link != previous_link) {
        previous_link = link;
        if (link) {
            device.rx_packets = device.tx_packets = 0;
            broadcast_replies = false;
            netif_set_up(&interface);
            netif_set_link_up(&interface);
            if (dhcp_start(&interface) != ERR_OK) {
                device.stop();
                link = previous_link = false;
            }
        } else {
            dhcp_stop(&interface);
            netif_set_link_down(&interface);
            netif_set_down(&interface);
            netif_set_addr(&interface, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
            for (unsigned i = 0; i < SOCKET_COUNT; ++i)
                if (connections[i].handle) fail_connection(&connections[i], USER_ENETDOWN);
            dns_clear_cache();
        }
    }
    sys_check_timeouts();
    network_time_poll(addressed() && !random_failed, poll_us);
    for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
        struct connection *c = &connections[i];
        if (!c->handle) continue;
        if (random_failed || !device.online()) {
            fail_connection(c, USER_ENETDOWN);
            continue;
        }
        if (c->state != NET_CONNECTING) continue;
        if (poll_us >= c->deadline) {
            fail_connection(c, USER_ETIMEDOUT);
            continue;
        }
        if (!addressed() || c->resolving) continue;
        unsigned own = 0, job = DNS_JOBS;
        for (unsigned j = 0; j < DNS_JOBS; ++j) {
            if (dns_jobs[j].handle && dns_jobs[j].pid == c->pid) ++own;
            if (!dns_jobs[j].handle) job = j;
        }
        if (own >= DNS_PER_PROCESS || job == DNS_JOBS) continue;
        dns_jobs[job].handle = c->handle;
        dns_jobs[job].pid = c->pid;
        c->resolving = true;
        ip_addr_t address;
        err_t error = dns_gethostbyname(c->host, &address, resolved, (void *)(uintptr_t)c->handle);
        if (error == ERR_OK) resolved(c->host, &address, (void *)(uintptr_t)c->handle);
        /* A full resolver table or pcb pool is transient: retry on a later
         * poll until the connection deadline. No callback stays registered. */
        else if (error == ERR_MEM) {
            dns_jobs[job].handle = 0;
            c->resolving = false;
        } else if (error != ERR_INPROGRESS) {
            dns_jobs[job].handle = 0;
            fail_connection(c, error_code(error));
        }
    }
    if (random_failed) device.stop();
}

static struct connection *lookup(unsigned pid, uint32_t handle) {
    struct connection *c = &connections[handle % SOCKET_COUNT];
    return handle && c->handle == handle && c->pid == pid ? c : 0;
}

static void close_connection(struct connection *c) {
    detach(c);
    if (c->pcb && tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
    if (c->received) vm_cpu_free(c->received, RECEIVE_PAGES);
    memset(c, 0, sizeof(*c));
}

void network_close_process(unsigned pid) {
    for (unsigned i = 0; i < SOCKET_COUNT; ++i)
        if (connections[i].handle && connections[i].pid == pid) {
            /* Termination must release all buffers immediately, even if the peer
             * refuses to ACK. Normal close below can finish a graceful TCP close. */
            fail_connection(&connections[i], USER_ECONNABORTED);
            close_connection(&connections[i]);
        }
}

static int connect_to(unsigned pid, struct vm_space *space, user_ptr address, unsigned port) {
    if (!port || port > 65535) return USER_EINVAL;
    char host[USER_NET_HOST_MAX + 1];
    unsigned length = 0, label = 0;
    for (; length < sizeof(host); ++length) {
        if (address > (user_ptr)-1 - length ||
            !vm_copy_from_user(space, host + length, address + length, 1))
            return USER_EFAULT;
        char ch = host[length];
        if (!ch) break;
        if (ch == '.') {
            if (!label || host[length - 1] == '-') return USER_EINVAL;
            label = 0;
            continue;
        }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              (ch == '-' && label)))
            return USER_EINVAL;
        if (++label > 63) return USER_EINVAL;
    }
    if (!length || length == sizeof(host) || host[length - 1] == '-') return USER_EINVAL;
    if (!device.online() || random_failed) return USER_ENETDOWN;
    /* The table is shared by every page. Divide it between the pages that use
     * the network so a few busy pages cannot take every socket; an over-share
     * request is transient (EAGAIN) and the renderer retries. */
    unsigned own = 0, users = 1;
    for (unsigned i = 0; i < SOCKET_COUNT; ++i) {
        const struct connection *c = &connections[i];
        if (!c->handle) continue;
        if (c->pid == pid) {
            ++own;
            continue;
        }
        bool counted = false;
        for (unsigned j = 0; j < i; ++j)
            if (connections[j].handle && connections[j].pid == c->pid) {
                counted = true;
                break;
            }
        if (!counted) ++users;
    }
    unsigned share = SOCKET_COUNT / users;
    if (share < 2) share = 2;
    if (share > USER_NET_SOCKETS) share = USER_NET_SOCKETS;
    if (own == USER_NET_SOCKETS || generation == 0x07ffffffu) return USER_EMFILE;
    if (own >= share) return USER_EAGAIN;
    for (unsigned i = 0; i < SOCKET_COUNT; ++i)
        if (!connections[i].handle) {
            /* Allocate only live sockets. Large static rings would overlap lk2nd's
             * DTB reservation; these cached kernel pages never alias a user mapping
             * or a device DMA queue. Close returns them immediately. */
            uint8_t *received = vm_cpu_alloc(RECEIVE_PAGES);
            if (!received) return USER_ENOMEM;
            struct connection *c = &connections[i];
            *c = (struct connection){.handle = (++generation) * SOCKET_COUNT + i,
                                     .pid = pid,
                                     .port = (uint16_t)port,
                                     .state = NET_CONNECTING,
                                     .deadline = time_us() + 15000000,
                                     .received = received};
            memcpy(c->host, host, length + 1);
            return (int)c->handle;
        }
    return USER_ENOMEM;
}

int32_t network_call(unsigned pid, struct vm_space *space, unsigned call, user_ptr a, user_ptr b,
                     user_ptr bytes) {
    if (call == SYS_NET_INFO) {
        if (b != sizeof(struct user_net_info)) return USER_EINVAL;
        struct user_net_info info = {
            .mtu = 1500, .rx_packets = device.rx_packets, .tx_packets = device.tx_packets};
        if (device.online()) info.flags |= NET_DEVICE;
        if (device.link()) info.flags |= NET_LINK;
        if (addressed()) info.flags |= NET_ADDRESS;
        memcpy(info.mac, device.mac, 6);
        if (started) {
            memcpy(info.address, netif_ip4_addr(&interface), 4);
            memcpy(info.mask, netif_ip4_netmask(&interface), 4);
            memcpy(info.gateway, netif_ip4_gw(&interface), 4);
            memcpy(info.dns, ip_2_ip4(dns_getserver(0)), 4);
        }
        return vm_copy_to_user(space, a, &info, sizeof(info)) ? 0 : USER_EFAULT;
    }
    if (call == SYS_NET_CONNECT) return connect_to(pid, space, a, (unsigned)b);
    struct connection *c = lookup(pid, (uint32_t)a);
    if (!c) return USER_EBADF;
    if (call == SYS_NET_CLOSE) {
        close_connection(c);
        return 0;
    }
    if (call == SYS_NET_STATUS) {
        if (bytes != sizeof(struct user_net_status)) return USER_EINVAL;
        struct user_net_status status = {.state = c->state,
                                         .flags = c->eof ? NET_READ_EOF : 0,
                                         .readable = c->count,
                                         .error = c->error};
        if (c->state == NET_CONNECTED && c->pcb) status.writable = tcp_sndbuf(c->pcb);
        return vm_copy_to_user(space, b, &status, sizeof(status)) ? 0 : USER_EFAULT;
    }
    if (call != SYS_NET_SEND && call != SYS_NET_RECV) return USER_ENOSYS;
    if (bytes > USER_NET_IO_MAX) return USER_EINVAL;
    if (!bytes) return 0;
    if (!vm_accessible(space, b, bytes, call == SYS_NET_SEND ? VM_READ : VM_WRITE))
        return USER_EFAULT;
    if (call == SYS_NET_RECV && c->count) {
        if (bytes > c->count) bytes = c->count;
        unsigned first = RECEIVE_BYTES - c->head;
        if (first > bytes) first = (unsigned)bytes;
        memcpy(io_buffer, c->received + c->head, first);
        memcpy(io_buffer + first, c->received, bytes - first);
        if (!vm_copy_to_user(space, b, io_buffer, bytes)) return USER_EFAULT;
        c->head = (c->head + bytes) % RECEIVE_BYTES;
        c->count -= bytes;
        if (c->pcb) tcp_recved(c->pcb, (u16_t)bytes);
        return (int32_t)bytes;
    }
    if (c->state == NET_FAILED) return c->error;
    if (c->state != NET_CONNECTED) return USER_EAGAIN;
    if (call == SYS_NET_RECV) return c->eof ? 0 : USER_EAGAIN;
    if (bytes > tcp_sndbuf(c->pcb)) bytes = tcp_sndbuf(c->pcb);
    if (!bytes) return USER_EAGAIN;
    if (!vm_copy_from_user(space, io_buffer, b, bytes)) return USER_EFAULT;
    err_t error = tcp_write(c->pcb, io_buffer, (u16_t)bytes, TCP_WRITE_FLAG_COPY);
    if (error != ERR_OK) return error_code(error);
    (void)tcp_output(c->pcb);
    return (int32_t)bytes;
}

bool network_service_pending(void) { return wifi_setup_pending(); }

void network_service_work(void *argument) { wifi_setup_service(argument); }
