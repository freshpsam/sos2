#ifndef SOS2_MSM8916_I2C_H
#define SOS2_MSM8916_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Independent QUP controller state: the battery's QUP4 cannot reset or alter
 * the touch controller's QUP5. Board code enables clocks/pins before init. */
struct msm8916_i2c {
    uintptr_t base;
    bool ready;
    unsigned rx_block_bytes, rx_fifo_bytes;
};

bool msm8916_i2c_bus_init(struct msm8916_i2c *);
bool msm8916_i2c_bus_read(struct msm8916_i2c *, uint8_t address, uint8_t reg, uint8_t *data,
                          size_t length);
/* Seven-bit address, register pointer and one value, then STOP. */
bool msm8916_i2c_bus_write(struct msm8916_i2c *, uint8_t address, uint8_t reg, uint8_t value);

/* WT88047 BLSP1 QUP5 touch wrapper. The board setup must first enable its 19.2 MHz
 * core clock, BLSP AHB clock, GPIO18/19 mux and the touchscreen's supplies.
 * Transactions run at 100 kHz without interrupts or DMA. */
#define MSM8916_I2C_MAX_READ 64u
bool msm8916_i2c_init(void);

/* Seven-bit address, one-byte register pointer, repeated START, then STOP.
 * Accepts 1..64 bytes. A failure leaves data unchanged and resets the engine;
 * it does not toggle the board's pins or cycle peripheral power. */
bool msm8916_i2c_read(uint8_t address, uint8_t reg, uint8_t *data, size_t length);
bool msm8916_i2c_write(uint8_t address, uint8_t reg, uint8_t value);

#endif
