#ifndef SOS2_QUP_I2C_H
#define SOS2_QUP_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A Qualcomm BLSP QUP v2 controller as a polled I2C master (qup_i2c.c), the
 * same on MSM8916 and MSM8974: no interrupts or DMA. Each controller's state
 * is its own, so one bus cannot reset or alter another (the Redmi 2's
 * battery, touch and motion-sensor buses). The board enables the
 * controller's 19.2 MHz core clock, its BLSP's AHB clock, the pins' mux and
 * the devices' supplies before init. khz is the bus rate, 0 for 100 kHz. */
struct qup_i2c {
    uintptr_t base;
    unsigned khz;
    bool ready;
    uint32_t clock;
    unsigned rx_block_bytes, rx_fifo_bytes;
};

#define QUP_I2C_MAX_READ 64u

bool qup_i2c_init(struct qup_i2c *);
/* Seven-bit address, one-byte register pointer, repeated START, then STOP.
 * Accepts 1..64 bytes. A failure leaves data unchanged and resets the engine;
 * it does not toggle the board's pins or cycle peripheral power. */
bool qup_i2c_read(struct qup_i2c *, uint8_t address, uint8_t reg, uint8_t *data, size_t length);
/* Seven-bit address, register pointer and one value, then STOP. */
bool qup_i2c_write(struct qup_i2c *, uint8_t address, uint8_t reg, uint8_t value);

#endif
