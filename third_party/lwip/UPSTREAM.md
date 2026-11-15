# lwIP source provenance

- Upstream: https://github.com/lwip-tcpip/lwip
- Release: `STABLE-2_2_1_RELEASE` (2.2.1)
- Commit: `77dcd25a72509eb83f72b033d219b1d40cd8eb95`
- Archive: https://codeload.github.com/lwip-tcpip/lwip/tar.gz/refs/tags/STABLE-2_2_1_RELEASE
- Archive SHA-256: `ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539`
- License: [COPYING](COPYING). Individual source/header notices are retained.

This directory contains the IPv4 core and Ethernet implementation used by SOS2,
plus their header dependencies. Unused IPv6 sources and disabled raw/altcp, OS,
statistics, fragmentation, ACD, IGMP and AutoIP implementations are omitted.
Retained files match upstream except for the resolver adaptations below.
Enabling omitted features requires restoring their sources and headers from
the pinned archive. The operating-system
socket/netconn implementation is not included or linked.

Local resolver adaptations: `dns_clear_cache()` cancels pending queries and
clears cached answers/servers on link loss. `dns_call_found()` bounds its shared
UDP-socket search by `DNS_TABLE_SIZE`, because it indexes `dns_table` (SOS2 has
more request slots than cached-query slots).

SOS2-specific configuration and the small freestanding C header boundary live
in `include/lwip/`. The adapter, process handles and polling are in `kernel/net/`.
No Linux library or Linux syscall compatibility layer is needed in the guest.
