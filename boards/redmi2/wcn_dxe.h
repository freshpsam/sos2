#ifndef SOS2_WCN_DXE_H
#define SOS2_WCN_DXE_H
#include "wcnss_boot.h"
#define WCN_RING   8
#define WCN_PACKET 3872
/* Stock Pronto WDI_TxBdType includes the CSU word and completion token. */
#define WCN_TX_BD_SIZE 48

struct wcn_descriptor {
    uint32_t control, length, source, destination, next, source_high, destination_high, next_high;
};

struct wcn_dma {
    struct wcn_descriptor descriptors[4][WCN_RING * 2];
    unsigned char tx_bd[2][WCN_RING][WCN_TX_BD_SIZE];
    unsigned char buffers[4][WCN_RING][WCN_PACKET];
} __attribute__((aligned(64)));

struct wcn_tx_info {
    unsigned char station, dpu, signature;
    bool data, encrypt;
};

/* data_offset is relative to the delivered MPDU, not the DMA buffer. The
 * firmware can leave a gap between the MAC header and the LLC payload. */
struct wcn_rx_info {
    bool decrypted, management;
    unsigned key_index, data_offset;
};

struct wcn_dxe {
    struct wcnss_io io;
    struct wcn_dma *dma;
    uintptr_t physical;
    unsigned producer[2], consumer[4], pending[2];
    uint64_t sent_at[2][WCN_RING];
    bool active, failed;
};

/* Zero-initialize once at boot. Stop/failure permanently quarantines this arena. */
bool wcn_dxe_start(struct wcn_dxe *, struct wcn_dma *, uintptr_t, const struct wcnss_io *);
bool wcn_dxe_send(struct wcn_dxe *, const void *, unsigned, const struct wcn_tx_info *, uint64_t);
bool wcn_dxe_poll(struct wcn_dxe *,
                  void (*)(void *, const unsigned char *, unsigned, const struct wcn_rx_info *),
                  void *, uint64_t);
bool wcn_dxe_drained(const struct wcn_dxe *);
void wcn_dxe_stop(struct wcn_dxe *);
#endif
