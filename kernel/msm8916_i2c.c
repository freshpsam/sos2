#include "msm8916_i2c.h"

/* Original polled implementation of the QUP v2 register/tag protocol.
 * Hardware register definitions and sequencing were checked against:
 * https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-qup.c
 * https://github.com/u-boot/u-boot/blob/master/drivers/i2c/qup_i2c.c
 * Base/pins: Linux arch/arm64/boot/dts/qcom/msm8916.dtsi (blsp_i2c4/5).
 * Source clock: Linux drivers/clk/qcom/gcc-msm8916.c (QUP4/5 XO /1).
 *
 * Register reads serve the independent SMB1360 battery and FT5x06 touch buses.
 * The register-pointer write keeps the bus active; the read emits the final
 * NACK/STOP. Input block mode handles a 63-byte report plus its two QUP tags
 * even when that exceeds the hardware FIFO. No controller report is split.
 * A register write sends the pointer and one value, then STOP.
 */
#include "platform.h"

static uint32_t read32(uintptr_t address) {
    uint32_t value = mmio_read32(address);
    barrier();
    return value;
}

static void write32(uintptr_t address, uint32_t value) {
    barrier();
    mmio_write32(address, value);
    barrier();
}

#define now_us time_us

enum {
    CONFIG = 0x000,
    STATE = 0x004,
    IO_MODE = 0x008,
    SW_RESET = 0x00c,
    OPERATIONAL = 0x018,
    ERROR_FLAGS = 0x01c,
    ERROR_ENABLE = 0x020,
    HW_VERSION = 0x030,
    MX_OUTPUT = 0x100,
    OUT_FIFO = 0x110,
    MX_WRITE = 0x150,
    MX_INPUT = 0x200,
    MX_READ = 0x208,
    IN_FIFO = 0x218,
    CLK_CTL = 0x400,
    I2C_STATUS = 0x404,
    MASTER_GEN = 0x408,
    RESET = 0,
    RUN = 1,
    PAUSE = 3,
    VALID = 1 << 2,
    I2C_MASTER = 1 << 4,
    OUT_SERVICE = 1 << 8,
    IN_SERVICE = 1 << 9,
    IN_BLOCK_REQUEST = 1 << 13,
    INPUT_BLOCK = 1 << 12,
    REPACK = (1 << 14) | (1 << 15),
    CORE_CONFIG = (2 << 8) | 7,
    NO_INPUT = 1 << 7,
    BUS_ACTIVE = 1 << 8,
    BUS_ERRORS = 0x038000fc,
    CORE_ERRORS = 0x7c,
    TAG_START = 0x81,
    TAG_WRITE = 0x82,
    TAG_WRITE_STOP = 0x83,
    TAG_READ_STOP = 0x87,
    CLOCK_100KHZ = (3 << 8) | 93,
    TIMEOUT_US = 25000,
    POLL_LIMIT = 100000,
};

#define RECONFIGURE (UINT32_C(1) << 31)

static struct msm8916_i2c touch_bus = {.base = 0x078b9000};

struct deadline {
    uint64_t start;
    unsigned polls;
};

static bool within_deadline(struct deadline *d) {
    return d->polls++ < POLL_LIMIT && now_us() - d->start < TIMEOUT_US;
}

static bool state_is(struct msm8916_i2c *bus, unsigned mask, unsigned wanted, struct deadline *d) {
    do {
        uint32_t value = read32(bus->base + STATE);
        if ((value & VALID) && (value & mask) == wanted) return true;
    } while (within_deadline(d));
    return false;
}

static bool change_state(struct msm8916_i2c *bus, unsigned state, struct deadline *d) {
    if (!state_is(bus, 0, 0, d)) return false; /* QUP state-valid timeout */
    write32(bus->base + STATE, state);
    return state_is(bus, 3, state, d);
}

static bool no_error(struct msm8916_i2c *bus, uint32_t *status) {
    uint32_t value = read32(bus->base + I2C_STATUS);
    if (status) *status = value;
    uint32_t bus_errors = value & BUS_ERRORS;
    uint32_t core = read32(bus->base + ERROR_FLAGS) & CORE_ERRORS;
    if (bus_errors) write32(bus->base + I2C_STATUS, bus_errors);
    if (core) write32(bus->base + ERROR_FLAGS, core);
    if (bus_errors & (1u << 3)) return false; /* I2C device did not acknowledge */
    if (bus_errors) return false;             /* I2C bus error */
    if (core) return false;                   /* QUP core/FIFO error */
    return true;
}

static bool wait_service(struct msm8916_i2c *bus, unsigned flag, struct deadline *d,
                         uint32_t *flags) {
    do {
        if (!no_error(bus, 0)) return false;
        *flags = read32(bus->base + OPERATIONAL);
        if (*flags & flag) return true;
    } while (within_deadline(d));
    return false; /* service timeout */
}

static bool reset_engine(struct msm8916_i2c *bus, struct deadline *d) {
    write32(bus->base + SW_RESET, 1);
    return state_is(bus, 3, RESET, d);
}

bool msm8916_i2c_bus_init(struct msm8916_i2c *bus) {
    bus->ready = false;
    struct deadline d = {now_us(), 0};
    if (!reset_engine(bus, &d)) return false;
    uint32_t version = read32(bus->base + HW_VERSION);
    if (!version || version == UINT32_MAX) return false; /* QUP version unavailable */
    uint32_t geometry = read32(bus->base + IO_MODE);
    unsigned output_block = geometry & 3, input_block = (geometry >> 5) & 3;
    if (output_block == 3 || input_block == 3) return false; /* QUP FIFO geometry invalid */
    const unsigned block_bytes[] = {4, 16, 32};
    unsigned output_capacity = block_bytes[output_block] * (2u << ((geometry >> 2) & 7));
    bus->rx_block_bytes = block_bytes[input_block];
    bus->rx_fifo_bytes = bus->rx_block_bytes * (2u << ((geometry >> 7) & 7));
    if (output_capacity < 8 || bus->rx_fifo_bytes < 8) return false; /* QUP FIFO too small */
    write32(bus->base + CONFIG, CORE_CONFIG | NO_INPUT);
    write32(bus->base + MASTER_GEN, 1);
    if (!state_is(bus, I2C_MASTER, I2C_MASTER, &d)) return false; /* QUP I2C master-mode timeout */
    bus->ready = true;
    return true;
}

bool msm8916_i2c_bus_read(struct msm8916_i2c *bus, uint8_t address, uint8_t reg, uint8_t *data,
                          size_t length) {
    if (!bus->ready) return false; /* QUP not initialized */
    if (address < 0x08 || address > 0x77 || !data || !length || length > MSM8916_I2C_MAX_READ)
        return false; /* I2C read arguments invalid */
    struct deadline d = {now_us(), 0};
    uint8_t result[MSM8916_I2C_MAX_READ];
    size_t input_size = length + 2;
    bool block = input_size > bus->rx_fifo_bytes;
    uint32_t flags;
    if (!reset_engine(bus, &d)) goto failed;
    write32(bus->base + CONFIG, CORE_CONFIG | NO_INPUT);
    write32(bus->base + MASTER_GEN, 1);
    if (!state_is(bus, I2C_MASTER, I2C_MASTER, &d)) goto failed;
    write32(bus->base + ERROR_FLAGS, CORE_ERRORS);
    write32(bus->base + I2C_STATUS, BUS_ERRORS);
    write32(bus->base + ERROR_ENABLE, CORE_ERRORS);
    write32(bus->base + MX_OUTPUT, 0);
    write32(bus->base + MX_INPUT, 0);
    write32(bus->base + MX_READ, 0);
    write32(bus->base + IO_MODE, REPACK | (block ? INPUT_BLOCK : 0));
    write32(bus->base + MX_WRITE, 5); /* Four command bytes and one register byte. */
    if (!change_state(bus, RUN, &d)) goto failed;
    write32(bus->base + CLK_CTL, CLOCK_100KHZ);
    if (!change_state(bus, PAUSE, &d)) goto failed;
    write32(bus->base + OUT_FIFO,
            TAG_START | ((uint32_t)address << 9) | (TAG_WRITE << 16) | (1u << 24));
    write32(bus->base + OUT_FIFO, reg);
    if (!change_state(bus, RUN, &d) || !wait_service(bus, OUT_SERVICE, &d, &flags)) goto failed;
    write32(bus->base + OPERATIONAL, OUT_SERVICE);
    if (!change_state(bus, PAUSE, &d)) goto failed;

    /* Reconfigure counts without resetting the bus between write and read. */
    write32(bus->base + MX_WRITE, RECONFIGURE | 4);
    write32(bus->base + (block ? MX_INPUT : MX_READ), RECONFIGURE | (uint32_t)input_size);
    write32(bus->base + CONFIG, CORE_CONFIG);
    write32(bus->base + OUT_FIFO, TAG_START | (((uint32_t)address * 2 + 1) << 8) |
                                      (TAG_READ_STOP << 16) | ((uint32_t)length << 24));
    if (!change_state(bus, RUN, &d)) goto failed;

    size_t received = 0;
    while (received < input_size) {
        if (!wait_service(bus, IN_SERVICE, &d, &flags)) goto failed;
        if (flags & OUT_SERVICE) write32(bus->base + OPERATIONAL, OUT_SERVICE);
        write32(bus->base + OPERATIONAL, IN_SERVICE);
        if (block && !(flags & IN_BLOCK_REQUEST)) {
            if (!within_deadline(&d)) goto failed;
            continue;
        }
        size_t available = block ? bus->rx_block_bytes : input_size;
        if (available > input_size - received) available = input_size - received;
        for (size_t offset = 0; offset < available; offset += 4) {
            uint32_t word = read32(bus->base + IN_FIFO);
            size_t bytes = available - offset;
            if (bytes > 4) bytes = 4;
            for (size_t byte = 0; byte < bytes; ++byte, ++received) {
                /* The first two bytes are QUP protocol tags, not register data. */
                if (received >= 2) result[received - 2] = (uint8_t)(word >> (byte * 8));
            }
        }
    }
    do {
        uint32_t status;
        if (!no_error(bus, &status)) goto failed;
        if (!(status & BUS_ACTIVE)) {
            if (!change_state(bus, RESET, &d)) goto failed;
            for (size_t i = 0; i < length; ++i) data[i] = result[i];
            return true;
        }
    } while (within_deadline(&d));
    /* The bus stayed busy after STOP. */
failed:
    /* Bound recovery separately; a wedged clock or state machine disables use. */
    d = (struct deadline){now_us(), 0};
    if (!reset_engine(bus, &d)) bus->ready = false;
    return false;
}

bool msm8916_i2c_bus_write(struct msm8916_i2c *bus, uint8_t address, uint8_t reg, uint8_t value) {
    if (!bus->ready) return false;                      /* QUP not initialized */
    if (address < 0x08 || address > 0x77) return false; /* I2C write arguments invalid */
    struct deadline d = {now_us(), 0};
    uint32_t flags;
    if (!reset_engine(bus, &d)) goto failed;
    write32(bus->base + CONFIG, CORE_CONFIG | NO_INPUT);
    write32(bus->base + MASTER_GEN, 1);
    if (!state_is(bus, I2C_MASTER, I2C_MASTER, &d)) goto failed;
    write32(bus->base + ERROR_FLAGS, CORE_ERRORS);
    write32(bus->base + I2C_STATUS, BUS_ERRORS);
    write32(bus->base + ERROR_ENABLE, CORE_ERRORS);
    write32(bus->base + MX_OUTPUT, 0);
    write32(bus->base + MX_INPUT, 0);
    write32(bus->base + MX_READ, 0);
    write32(bus->base + IO_MODE, REPACK);
    write32(bus->base + MX_WRITE, 6); /* Four command bytes, the register and its value. */
    if (!change_state(bus, RUN, &d)) goto failed;
    write32(bus->base + CLK_CTL, CLOCK_100KHZ);
    if (!change_state(bus, PAUSE, &d)) goto failed;
    write32(bus->base + OUT_FIFO,
            TAG_START | ((uint32_t)address << 9) | (TAG_WRITE_STOP << 16) | (2u << 24));
    write32(bus->base + OUT_FIFO, reg | ((uint32_t)value << 8));
    if (!change_state(bus, RUN, &d) || !wait_service(bus, OUT_SERVICE, &d, &flags)) goto failed;
    write32(bus->base + OPERATIONAL, OUT_SERVICE);
    do {
        uint32_t status;
        if (!no_error(bus, &status)) goto failed;
        if (!(status & BUS_ACTIVE)) {
            if (!change_state(bus, RESET, &d)) goto failed;
            return true;
        }
    } while (within_deadline(&d));
    /* The bus stayed busy after STOP. */
failed:
    d = (struct deadline){now_us(), 0};
    if (!reset_engine(bus, &d)) bus->ready = false;
    return false;
}

bool msm8916_i2c_init(void) { return msm8916_i2c_bus_init(&touch_bus); }

bool msm8916_i2c_write(uint8_t address, uint8_t reg, uint8_t value) {
    return msm8916_i2c_bus_write(&touch_bus, address, reg, value);
}

bool msm8916_i2c_read(uint8_t address, uint8_t reg, uint8_t *data, size_t length) {
    return msm8916_i2c_bus_read(&touch_bus, address, reg, data, length);
}
