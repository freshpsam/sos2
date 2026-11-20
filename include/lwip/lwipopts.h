#ifndef SOS2_LWIP_OPTIONS_H
#define SOS2_LWIP_OPTIONS_H
/* All raw API calls run on the one kernel CPU outside interrupt handlers.
 * Memory, pending packets and work per poll are bounded independently of pages. */
#define NO_SYS               1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_SOCKET          0
#define LWIP_NETCONN         0
#define LWIP_IPV4            1
#define LWIP_IPV6            0
#define LWIP_ARP             1
#define LWIP_ETHERNET        1
#define LWIP_ICMP            1
#define LWIP_RAW             0
#define LWIP_UDP             1
#define LWIP_TCP             1
#define LWIP_DHCP            1
#define LWIP_HOOK_FILENAME   "sos2_hooks.h"
#define LWIP_DNS             1
#define DNS_MAX_NAME_LENGTH  254
/* Eight concurrent connection lookups (kernel/net/network.c) plus SNTP. */
#define DNS_TABLE_SIZE             9
#define DNS_MAX_REQUESTS           16
#define DNS_LOCAL_HOSTLIST         0
#define LWIP_DNS_SECURE            7
#define LWIP_DHCP_DOES_ACD_CHECK   0
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   1
#define LWIP_NETIF_HOSTNAME        0
#define LWIP_STATS                 0
#define LWIP_PROVIDE_ERRNO         0
/* Four bytes on ARM32; host protocol tests also allocate pointer-bearing
 * lwIP objects and must use their host's pointer alignment. */
#define MEM_ALIGNMENT __SIZEOF_POINTER__
#define MEM_SIZE      (512 * 1024)
/* The heap and pools in the kernel's cached CPU-private memory: only CPU 0
 * runs lwIP, and no device reads its buffers (frames are copied). */
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
    u8_t variable_name[LWIP_MEM_ALIGN_BUFFER(size)] __attribute__((section(".cpu_private")))
#define MEMP_NUM_TCP_PCB        24
#define MEMP_NUM_TCP_PCB_LISTEN 0
#define MEMP_NUM_TCP_SEG        512
#define MEMP_NUM_UDP_PCB        20
#define PBUF_POOL_SIZE          512
#define PBUF_POOL_BUFSIZE       1536
#define TCP_MSS                 1460
#define LWIP_WND_SCALE          1
#define TCP_RCV_SCALE           2
#define TCP_WND                 (128 * 1024)
#define TCP_SND_BUF             (32 * 1024)
#define TCP_SND_QUEUELEN        96
/* Wi-Fi loses and reorders frames: segments past a gap wait for it, and
 * SACK tells the server which ones to resend, instead of whole windows.
 * Each connection may hold 64 of the pool's buffers that way. */
#define TCP_QUEUE_OOSEQ     1
#define LWIP_TCP_SACK_OUT   1
#define TCP_OOSEQ_MAX_PBUFS 64
#define TCP_LISTEN_BACKLOG  0
#define IP_REASSEMBLY       0
#define IP_FRAG             0
#define LWIP_TCP_TIMESTAMPS 0
/* lwIP only consults LWIP_HOOK_TCP_ISN (not LWIP_TCP_ISN); without it, ISNs
 * come from a predictable counter. Each new connection draws a random word. */
#define LWIP_HOOK_TCP_ISN(local_ip, local_port, remote_ip, remote_port) (network_random())
#define LWIP_DHCP_MAX_DNS_SERVERS                                       2
#define LWIP_NETIF_TX_SINGLE_PBUF                                       1
#endif
