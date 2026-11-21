#include "wcn_dxe.h"
#include "wlan_wire.h"
#include "platform.h"
#include "kernel_io.h"
#include "wire.h"
_Static_assert(sizeof(struct wcn_descriptor) == sizeof(struct wcn36xx_dxe_desc),
               "DXE wire descriptor");
_Static_assert(sizeof(struct wcn36xx_tx_bd) + 8 == WCN_TX_BD_SIZE,
               "Pronto TX descriptor prefix and extension");

/* Pronto's reset is CCU bit 2, not the Riva CSR reset. Its CSR enable value
 * is H2H_SYNC | ECTR | ENABLE; see the stock DXE sources in docs/redmi2.md. */
enum {
    DXE = 0x0a202000,
    CCU = 0x0a204000,
    CCU_RESET = CCU + 0x10,
    DXE_RESET = 4,
    DXE_ENABLE = 0xa001
};

static const unsigned channels[] = {0, 4, 1, 3};
static const uint32_t defaults[] = {
    WCN36XX_DXE_CH_DEFAULT_CTL_TX_L, WCN36XX_DXE_CH_DEFAULT_CTL_TX_H,
    WCN36XX_DXE_CH_DEFAULT_CTL_RX_L, WCN36XX_DXE_CH_DEFAULT_CTL_RX_H};
static const uint32_t controls[] = {WCN36XX_DXE_CTRL_TX_L, WCN36XX_DXE_CTRL_TX_H,
                                    WCN36XX_DXE_CTRL_RX_L, WCN36XX_DXE_CTRL_RX_H};

static uintptr_t reg(unsigned c, unsigned offset) {
    return DXE + 0x400 + channels[c] * 64 + offset;
}

static void write_reg(struct wcn_dxe *d, uintptr_t a, uint32_t v) {
    d->io.write(d->io.context, a, v);
}

static uint32_t address(struct wcn_dxe *d, const void *p) {
    return (uint32_t)(d->physical + (uintptr_t)p - (uintptr_t)d->dma);
}

static uint32_t read_control(struct wcn_descriptor *p) {
    uint32_t v = *(volatile uint32_t *)&p->control;
    barrier();
    return v;
}

void wcn_dxe_stop(struct wcn_dxe *d) {
    if (!d) return;
    if (d->active) {
        write_reg(d, DXE + WCN36XX_DXE_INT_MASK_REG, 0);
        write_reg(d, CCU_RESET, d->io.read(d->io.context, CCU_RESET) | DXE_RESET);
        barrier();
    }
    d->active = false;
    d->failed = true;
    /* The statically reserved DMA arena is never freed or repurposed after a
     * failure. A reset write alone does not prove the bus master has stopped. */
}

bool wcn_dxe_start(struct wcn_dxe *d, struct wcn_dma *memory, uintptr_t physical,
                   const struct wcnss_io *io) {
    if (!d || d->active || d->failed || !memory || ((uintptr_t)memory & 63) || !physical ||
        (physical & 63) || physical > UINT32_MAX - sizeof(*memory) || !io || !io->read ||
        !io->write)
        return false;
    *d = (struct wcn_dxe){.dma = memory, .physical = physical, .io = *io, .active = true};
    write_reg(d, DXE + WCN36XX_DXE_INT_MASK_REG, 0);
    uint32_t reset = d->io.read(d->io.context, CCU_RESET);
    write_reg(d, CCU_RESET, reset | DXE_RESET);
    barrier();
    memset(memory, 0, sizeof(*memory));
    write_reg(d, CCU_RESET, reset & ~DXE_RESET);
    barrier();
    /* The stock driver retries the master enable for up to 30 x 1 ms. Yield
     * to the desktop between attempts; never install rings without an ACK. */
    bool enabled = false;
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        write_reg(d, DXE, DXE_ENABLE);
        barrier();
        if (d->io.read(d->io.context, DXE) & 1) {
            enabled = true;
            break;
        }
        uint64_t start = time_us();
        unsigned polls = 0;
        while (time_us() - start < 1000 && polls++ < 10000) kernel_io_wait();
        if (time_us() - start < 1000) break;
    }
    if (!enabled) {
        wcn_dxe_stop(d);
        return false;
    }
    write_reg(d, CCU + WCN36XX_CCU_DXE_INT_SELECT_PRONTO,
              ((1u << 3 | 1u << 1) << 16) | (1u << 0 | 1u << 4));
    for (unsigned c = 0; c < 4; ++c) {
        unsigned n = c < 2 ? WCN_RING * 2 : WCN_RING;
        for (unsigned i = 0; i < n; ++i) {
            struct wcn_descriptor *p = &memory->descriptors[c][i];
            p->next = address(d, &memory->descriptors[c][(i + 1) % n]);
            if (c < 2) {
                p->destination = 0x17;
                p->control = controls[c];
            } else {
                p->source = c == 2 ? 0xb : 4;
                p->destination = address(d, memory->buffers[c][i]);
                p->control = controls[c];
            }
        }
        barrier();
        write_reg(d, reg(c, 0x1c), address(d, &memory->descriptors[c][0]));
        if (c < 2) write_reg(d, reg(c, 0x14), 0x17);
        else {
            write_reg(d, reg(c, 0x0c), c == 2 ? 0xb : 4);
            write_reg(d, reg(c, 0x14), memory->descriptors[c][0].next);
            write_reg(d, reg(c, 0), defaults[c]);
        }
    }
    d->active = true;
    return true;
}

static void big_endian(void *p, unsigned size) {
    unsigned char *s = p;
    for (unsigned i = 0; i < size; i += 4) {
        unsigned char a = s[i], b = s[i + 1];
        s[i] = s[i + 3];
        s[i + 1] = s[i + 2];
        s[i + 2] = b;
        s[i + 3] = a;
    }
}

bool wcn_dxe_send(struct wcn_dxe *d, const void *frame, unsigned length,
                  const struct wcn_tx_info *info, uint64_t now) {
    if (!d || !d->active || d->failed || !frame || length < 24 || length > WCN_PACKET - 128 ||
        !info || info->station == 255 || info->signature > 7)
        return false;
    const unsigned char *bytes = frame;
    bool eapol = info->data && length >= 32 && (le16(bytes) & 0x03fc) == 0x0108 &&
                 !memcmp(bytes + 24, "\xaa\xaa\3\0\0\0\x88\x8e", 8);
    /* WDTS_TxPacket reserves the high-priority channel for EAPOL as well as
     * management. A full data ring must not prevent authentication/rekey. */
    unsigned c = info->data && !eapol ? 0 : 1, slot = d->producer[c];
    if (d->pending[c] == WCN_RING) return false;
    struct wcn_descriptor *bd = &d->dma->descriptors[c][slot * 2], *packet = bd + 1;
    if ((read_control(bd) | read_control(packet)) & 1) {
        wcn_dxe_stop(d);
        return false;
    }
    struct wcn36xx_tx_bd header = {0};
    /* Stock's VSTA build routes EAPOL and virtual station IDs through
     * firmware (WQ5), before DPU/BTQM. Ordinary hardware stations use WQ25. */
    header.dpu_rf = eapol || (info->station >= 16 && info->station < 41) ? 5 : 25;
    header.dpu_ne = !info->encrypt;
    header.sta_index = info->station;
    header.dpu_desc_idx = info->dpu;
    header.dpu_sign = info->signature;
    header.rmf = !info->data && info->encrypt;
    /* This station does not negotiate WMM and sends non-QoS data. Stock's
     * WDI_FillTxBd selects BTQM_QUEUE_TX_nQOS (8) for that peer, not TID 0's
     * QoS queue. The low-priority DMA channel is separate from this queue. */
    header.bd_rate = info->data ? 0 : 2;
    header.queue_id = info->data ? 8 : 9;
    /* Share the radio's non-QoS sequence counter with firmware-generated
     * frames, as WDI_FillTxBd does for both data and management traffic. */
    header.pdu.bd_ssn = WCN36XX_TXBD_SSN_FILL_DPU_NON_QOS;
    header.pdu.mpdu_header_off = WCN_TX_BD_SIZE;
    header.pdu.mpdu_header_len = 24;
    header.pdu.mpdu_data_off = WCN_TX_BD_SIZE + 24;
    header.pdu.mpdu_len = length;
    header.pdu.tid = info->data ? 0 : 7;
    header.tx_bd_sign = 0xbdbdbdbd;
    if (bytes[4] & 1) {
        header.ub = 1;
        header.ack_policy = 1;
        if (!info->data) header.queue_id = 10;
    }
    /* CSU offload and enhanced token completion are not negotiated. Include
     * their zero words anyway: stock uses sizeof(WDI_TxBdType), not the
     * obsolete 40-byte WLANHAL_TX_BD_HEADER_SIZE constant. */
    memset(d->dma->tx_bd[c][slot], 0, WCN_TX_BD_SIZE);
    big_endian(&header, sizeof(header));
    memcpy(d->dma->tx_bd[c][slot], &header, sizeof(header));
    memcpy(d->dma->buffers[c][slot], frame, length);
    bd->source = address(d, d->dma->tx_bd[c][slot]);
    bd->length = WCN_TX_BD_SIZE;
    packet->source = address(d, d->dma->buffers[c][slot]);
    packet->length = length;
    d->sent_at[c][slot] = now;
    d->producer[c] = (slot + 1) % WCN_RING;
    ++d->pending[c];
    barrier();
    packet->control = controls[c] | 1 | 8;
    barrier();
    bd->control = (controls[c] & ~(1u << 17)) | 1;
    barrier();
    write_reg(d, reg(c, 0), defaults[c]);
    return true;
}

static bool intact(struct wcn_dxe *d, unsigned c, unsigned i) {
    struct wcn_descriptor *p = &d->dma->descriptors[c][i];
    unsigned n = c < 2 ? 2 * WCN_RING : WCN_RING;
    return !p->source_high && !p->destination_high && !p->next_high &&
           p->next == address(d, &d->dma->descriptors[c][(i + 1) % n]) &&
           (c < 2 ? p->destination == 0x17
                  : p->source == (c == 2 ? 0xb : 4) &&
                        p->destination == address(d, d->dma->buffers[c][i]));
}

bool wcn_dxe_drained(const struct wcn_dxe *d) {
    return d && d->active && !d->failed && !d->pending[0] && !d->pending[1];
}

bool wcn_dxe_poll(struct wcn_dxe *d,
                  void (*receive)(void *, const unsigned char *, unsigned,
                                  const struct wcn_rx_info *),
                  void *context, uint64_t now) {
    if (!d || !d->active || d->failed || !receive) return false;
    for (unsigned c = 0; c < 4; ++c) {
        unsigned status = d->io.read(d->io.context, reg(c, 4));
        if (status & WCN36XX_CH_STAT_INT_ERR_MASK) {
            wcn_dxe_stop(d);
            return false;
        }
        for (unsigned budget = 0; budget < WCN_RING; ++budget) {
            unsigned slot = d->consumer[c], index = c < 2 ? slot * 2 : slot;
            struct wcn_descriptor *p = &d->dma->descriptors[c][index];
            if (!intact(d, c, index) || (c < 2 && !intact(d, c, index + 1))) {
                wcn_dxe_stop(d);
                return false;
            }
            if (c < 2) {
                if (!d->pending[c]) break;
                if ((read_control(p) | read_control(p + 1)) & 1) {
                    if (now - d->sent_at[c][slot] >= 2000000) {
                        wcn_dxe_stop(d);
                        return false;
                    }
                    break;
                }
                --d->pending[c];
            } else {
                if (read_control(p) & 1) break;
                const unsigned char *bytes = d->dma->buffers[c][slot];
                struct wcn36xx_rx_bd bd;
                memcpy(&bd, bytes, sizeof(bd));
                big_endian(&bd, sizeof(bd));
                unsigned offset = bd.pdu.mpdu_header_off, length = bd.pdu.mpdu_len,
                         header = bd.pdu.mpdu_header_len, data = bd.pdu.mpdu_data_off;
                /* WDTS_RxPacket preserves the entire MPDU, including any gap
                 * before data_off. Stock's TL translation then skips that gap
                 * using data_off - header_off. Preserve that offset for our
                 * station parser too; header_len alone cannot locate LLC. */
                if (!bd.ft && !bd.asf && !bd.aef && offset >= sizeof(bd) && offset <= WCN_PACKET &&
                    length >= 24 && length <= WCN_PACKET - offset && header >= 24 &&
                    header <= length && data >= offset + header && data <= offset + length) {
                    struct wcn_rx_info info = {.decrypted = !bd.dpu_ne,
                                               .management = bd.rmf,
                                               .key_index = bd.rx_key_id,
                                               .data_offset = data - offset};
                    receive(context, bytes + offset, length, &info);
                }
                if (d->failed) return false;
                /* Clear stale bytes before returning ownership to the device. */
                memset(d->dma->buffers[c][slot], 0, WCN_PACKET);
                p->length = 0;
                barrier();
                p->control = controls[c];
                barrier();
            }
            d->consumer[c] = (slot + 1) % WCN_RING;
        }
        uint32_t bit = 1u << channels[c];
        if (c >= 2) write_reg(d, DXE + WCN36XX_DXE_ENCH_ADDR, bit);
        write_reg(d, DXE + WCN36XX_DXE_0_INT_CLR, bit);
        write_reg(d, DXE + WCN36XX_DXE_0_INT_ED_CLR, bit);
        write_reg(d, DXE + WCN36XX_DXE_0_INT_DONE_CLR, bit);
    }
    return true;
}
