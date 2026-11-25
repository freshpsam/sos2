#include "nexus5.h"
#include "platform.h"
#include "wire.h"

/* Playback through the audio DSP, as the stock kernel sets it up for the
 * loudspeaker (sound/soc/msm/qdsp6v2/q6afe.c, q6adm.c, q6asm.c and
 * msm-pcm-routing-v2.c, with include/sound/apr_audio-v2.h's layouts): the
 * AFE's SLIMbus port 0 (SLIMBUS_0_RX) configured for one 16-bit channel at
 * 48 kHz on the codec's SLIMbus channel and started; an ADM device session
 * (COPP) on it with the default topology; an ASM stream (session 1, stream
 * 1) taking 16-bit PCM from shared memory, routed by the ADM's matrix to
 * the COPP; the shared memory mapped once; then buffers written, each
 * answered by a write-done event when the DSP has read it. */
enum {
    SVC_AFE = 4,
    SVC_ASM = 7,
    SVC_ADM = 8,
    SLIMBUS_0_RX = 0x4000,
    AFE_PORT_CMD_SET_PARAM_V2 = 0x000100ef,
    AFE_PORT_CMD_DEVICE_START = 0x000100e5,
    AFE_PORT_CMD_DEVICE_STOP = 0x000100e6,
    AFE_MODULE_AUDIO_DEV_INTERFACE = 0x0001020c,
    AFE_PARAM_ID_SLIMBUS_CONFIG = 0x00010212,
    ADM_CMD_DEVICE_OPEN_V5 = 0x00010326,
    ADM_CMD_DEVICE_CLOSE_V5 = 0x00010327,
    ADM_CMDRSP_DEVICE_OPEN_V5 = 0x00010329,
    ADM_CMD_MATRIX_MAP_ROUTINGS_V5 = 0x00010325,
    DEFAULT_COPP_TOPOLOGY = 0x00010be3,
    ASM_STREAM_CMD_OPEN_WRITE_V3 = 0x00010db3,
    ASM_STREAM_CMD_CLOSE = 0x00010bcd,
    ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2 = 0x00010d98,
    ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 = 0x00010da5,
    ASM_STREAM_POSTPROC_TOPO_ID_DEFAULT = 0x00010be4,
    ASM_CMD_SHARED_MEM_MAP_REGIONS = 0x00010d92,
    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS = 0x00010d93,
    ASM_SESSION_CMD_RUN_V2 = 0x00010daa,
    ASM_DATA_CMD_WRITE_V2 = 0x00010dab,
    ASM_DATA_EVENT_WRITE_DONE_V2 = 0x00010d99,
    SHMEM8_4K_POOL = 3,
    PCM_CHANNEL_FC = 3,
    SESSION = 1,
    STREAM_PORT = SESSION << 8 | 1,
    RATE = 48000
};

static uint16_t copp;
static uint32_t memory_handle;
static uint32_t mapped_address, mapped_size;
static void (*written)(uint32_t token);

/* A command answered by success (a basic result of 0). */
static bool command(uint8_t *packet, unsigned size, uint8_t svc, uint16_t port, uint32_t token,
                    uint32_t opcode) {
    return !nexus5_apr_command(packet, size, svc, port, token, opcode, 0, 0, 0);
}

static void events(const uint8_t *packet, unsigned size) {
    unsigned header = (packet[0] >> 4 & 15) * 4;
    if (le32(packet + 16) == ASM_DATA_EVENT_WRITE_DONE_V2 && size >= header + 16 && written)
        written(le32(packet + 12));
}

/* The AFE's port: its SLIMbus configuration (the payload the union of
 * port configurations fills, 32 bytes) and start. */
bool nexus5_q6_afe_start(uint8_t channel) {
    uint8_t p[80] = {0};
    put16(p + 20, SLIMBUS_0_RX);
    put16(p + 22, 12 + 32);
    put32(p + 36, AFE_MODULE_AUDIO_DEV_INTERFACE);
    put32(p + 40, AFE_PARAM_ID_SLIMBUS_CONFIG);
    put16(p + 44, 32);
    put32(p + 48, 1);  /* the configuration's minor version */
    put16(p + 52, 0);  /* AFE_SLIMBUS_DEVICE_1 */
    put16(p + 54, 16); /* bits */
    put16(p + 56, 0);  /* PCM */
    put16(p + 58, 1);  /* one channel */
    p[60] = channel;   /* the shared channel's number */
    put32(p + 68, RATE);
    if (!command(p, sizeof(p), SVC_AFE, 0, 0, AFE_PORT_CMD_SET_PARAM_V2)) return false;
    uint8_t start[24] = {0};
    put16(start + 20, SLIMBUS_0_RX);
    return command(start, sizeof(start), SVC_AFE, 0, 0, AFE_PORT_CMD_DEVICE_START);
}

bool nexus5_q6_afe_stop(void) {
    uint8_t stop[24] = {0};
    put16(stop + 20, SLIMBUS_0_RX);
    return command(stop, sizeof(stop), SVC_AFE, 0, 0, AFE_PORT_CMD_DEVICE_STOP);
}

/* The ASM stream: opened for PCM into the matrix, then the ADM's COPP on the
 * port with the stream routed to it, then the stream's format. */
bool nexus5_q6_open(void) {
    nexus5_apr_events(events);
    uint8_t open[36] = {0};
    put16(open + 26, 16);
    put32(open + 28, ASM_STREAM_POSTPROC_TOPO_ID_DEFAULT);
    put32(open + 32, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2);
    if (!command(open, sizeof(open), SVC_ASM, STREAM_PORT, SESSION, ASM_STREAM_CMD_OPEN_WRITE_V3))
        return false;
    uint8_t device[48] = {0}, answer[8] = {0};
    put16(device + 22, 1); /* playback */
    put16(device + 24, SLIMBUS_0_RX);
    put16(device + 26, 0xffff);
    put32(device + 28, DEFAULT_COPP_TOPOLOGY);
    put16(device + 32, 1);
    put16(device + 34, 16);
    put32(device + 36, RATE);
    device[40] = PCM_CHANNEL_FC;
    /* Answered by the COPP's status and number. */
    if (nexus5_apr_command(device, sizeof(device), SVC_ADM, SLIMBUS_0_RX, SLIMBUS_0_RX,
                           ADM_CMD_DEVICE_OPEN_V5, ADM_CMDRSP_DEVICE_OPEN_V5, answer,
                           sizeof(answer)) ||
        le32(answer))
        return false;
    copp = le16(answer + 4);
    uint8_t route[36] = {0};
    put32(route + 20, 0); /* the audio RX matrix */
    put32(route + 24, 1);
    put16(route + 28, SESSION);
    put16(route + 30, 1);
    put16(route + 32, copp);
    if (!command(route, sizeof(route), SVC_ADM, copp, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5))
        return false;
    uint8_t format[44] = {0};
    put32(format + 20, 44 - 20 - 4);
    put16(format + 24, 1);
    put16(format + 26, 16);
    put32(format + 28, RATE);
    put16(format + 32, 1);
    format[36] = PCM_CHANNEL_FC;
    return command(format, sizeof(format), SVC_ASM, STREAM_PORT, SESSION,
                   ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2);
}

/* The shared memory the stream reads, mapped into the DSP once. */
bool nexus5_q6_map(uint32_t address, uint32_t size) {
    if (memory_handle && mapped_address == address && mapped_size == size) return true;
    uint8_t map[40] = {0}, answer[4] = {0};
    put16(map + 20, SHMEM8_4K_POOL);
    put16(map + 22, 1);
    put32(map + 28, address);
    put32(map + 36, size);
    if (nexus5_apr_command(map, sizeof(map), SVC_ASM, 0, SESSION << 8,
                           ASM_CMD_SHARED_MEM_MAP_REGIONS, ASM_CMDRSP_SHARED_MEM_MAP_REGIONS,
                           answer, sizeof(answer)))
        return false;
    memory_handle = le32(answer);
    mapped_address = address;
    mapped_size = size;
    return memory_handle != 0;
}

bool nexus5_q6_run(void (*done)(uint32_t token)) {
    written = done;
    uint8_t run[32] = {0};
    return command(run, sizeof(run), SVC_ASM, STREAM_PORT, SESSION, ASM_SESSION_CMD_RUN_V2);
}

/* A buffer of the mapped memory for the stream, without a timestamp; its
 * token comes back in the write-done event. */
bool nexus5_q6_write(uint32_t address, uint32_t size, uint32_t token) {
    uint8_t write[52] = {0};
    put32(write + 20, address);
    put32(write + 28, memory_handle);
    put32(write + 32, size);
    put32(write + 36, token);
    return nexus5_apr_send(write, sizeof(write), SVC_ASM, STREAM_PORT, token,
                           ASM_DATA_CMD_WRITE_V2);
}

/* The stream, then the COPP closed. */
bool nexus5_q6_close(void) {
    uint8_t close[20] = {0};
    bool ok = command(close, sizeof(close), SVC_ASM, STREAM_PORT, SESSION, ASM_STREAM_CMD_CLOSE);
    uint8_t device[20] = {0};
    return command(device, sizeof(device), SVC_ADM, copp, SLIMBUS_0_RX, ADM_CMD_DEVICE_CLOSE_V5) &&
           ok;
}
