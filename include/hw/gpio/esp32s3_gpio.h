#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"
#include "esp32_gpio.h"
#include "qemu/timer.h"

#define TYPE_ESP32S3_GPIO "esp32s3.gpio"
#define ESP32S3_GPIO(obj)           OBJECT_CHECK(ESP32S3GPIOState, (obj), TYPE_ESP32S3_GPIO)
#define ESP32S3_GPIO_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32S3GPIOClass, obj, TYPE_ESP32S3_GPIO)
#define ESP32S3_GPIO_CLASS(klass)   OBJECT_CLASS_CHECK(ESP32S3GPIOClass, klass, TYPE_ESP32S3_GPIO)

/* Bootstrap options for ESP32-S3 (4-bit) */
#define ESP32S3_STRAP_MODE_FLASH_BOOT 0x4   /* SPI Boot */

/* ESP32-S3 has GPIOs 0-48 (49 pins) */
#define ESP32S3_GPIO_COUNT  49

/* Register offsets */
#define GPIO_BT_SELECT      0x000
#define GPIO_OUT_REG         0x004
#define GPIO_OUT_W1TS_REG    0x008
#define GPIO_OUT_W1TC_REG    0x00C
#define GPIO_OUT1_REG        0x010
#define GPIO_OUT1_W1TS_REG   0x014
#define GPIO_OUT1_W1TC_REG   0x018
#define GPIO_ENABLE_REG      0x020
#define GPIO_ENABLE_W1TS_REG 0x024
#define GPIO_ENABLE_W1TC_REG 0x028
#define GPIO_ENABLE1_REG     0x02C
#define GPIO_ENABLE1_W1TS_REG 0x030
#define GPIO_ENABLE1_W1TC_REG 0x034
#define GPIO_STRAP_REG       0x038
#define GPIO_IN_REG          0x03C
#define GPIO_IN1_REG         0x040

#define GPIO_STATUS_REG      0x044
#define GPIO_STATUS_W1TS_REG 0x048
#define GPIO_STATUS_W1TC_REG 0x04C
#define GPIO_STATUS1_REG     0x050
#define GPIO_STATUS1_W1TS_REG 0x054
#define GPIO_STATUS1_W1TC_REG 0x058

/* Per-CPU interrupt status (read-only, mirrors STATUS for the current CPU) */
#define GPIO_PCPU_INT_REG    0x05C
#define GPIO_PCPU_NMI_INT_REG 0x060
#define GPIO_CPUSDIO_INT_REG 0x064
#define GPIO_PCPU_INT1_REG   0x068
#define GPIO_PCPU_NMI_INT1_REG 0x06C
#define GPIO_CPUSDIO_INT1_REG 0x070

/* Per-pin config: GPIO_PINn at 0x74 + n*4, for n=0..48 */
#define GPIO_PIN0_REG        0x074
#define GPIO_PIN_REG(n)      (GPIO_PIN0_REG + (n) * 4)

/* Input function select: GPIO_FUNCn_IN_SEL_CFG at 0x154 + n*4, for n=0..255 */
#define GPIO_FUNC_IN_SEL_CFG_REG(n) (0x154 + (n) * 4)

/* Output function select: GPIO_FUNCn_OUT_SEL_CFG at 0x554 + n*4, for n=0..48 */
#define GPIO_FUNC_OUT_SEL_CFG_REG(n) (0x554 + (n) * 4)

/* GPIO_PINn register fields */
#define GPIO_PIN_INT_TYPE_SHIFT  7
#define GPIO_PIN_INT_TYPE_MASK   (0x7 << GPIO_PIN_INT_TYPE_SHIFT)
#define GPIO_PIN_PAD_DRIVER_BIT  (1 << 2)

/* Interrupt types */
#define GPIO_INT_DISABLE    0
#define GPIO_INT_RISING     1
#define GPIO_INT_FALLING    2
#define GPIO_INT_BOTH       3
#define GPIO_INT_LOW        4
#define GPIO_INT_HIGH       5

/* GPIO region covers 0x000 to 0x700 */
#define ESP32S3_GPIO_IO_SIZE  0x700

/* Callback for output pin level changes */
typedef void (*gpio_output_cb_fn)(void *opaque, int pin, int level);

#define ESP32S3_GPIO_MAX_OUTPUT_CBS 8

typedef struct {
    gpio_output_cb_fn fn;
    void *opaque;
    int pin;  /* -1 = all pins */
} ESP32S3GPIOOutputCb;

typedef struct ESP32S3GPIOState {
    Esp32GpioState parent;

    /* Output registers */
    uint32_t out[2];        /* GPIO_OUT, GPIO_OUT1 */
    uint32_t enable[2];     /* GPIO_ENABLE, GPIO_ENABLE1 */

    /* Input levels (directly settable by external device models) */
    uint32_t in_levels[2];  /* GPIO_IN, GPIO_IN1 */

    /* Interrupt status */
    uint32_t status[2];     /* GPIO_STATUS, GPIO_STATUS1 */

    /* Per-pin config */
    uint32_t pin_reg[ESP32S3_GPIO_COUNT];

    /* Function select registers */
    uint32_t func_in_sel[256];
    uint32_t func_out_sel[ESP32S3_GPIO_COUNT];

    /* IRQ outputs to interrupt matrix */
    qemu_irq irq_cpu0;
    qemu_irq irq_cpu1;

    /* Deferred IRQ timer to prevent re-entrant interrupt processing */
    QEMUTimer irq_timer;

    /* Output change callbacks */
    ESP32S3GPIOOutputCb output_cbs[ESP32S3_GPIO_MAX_OUTPUT_CBS];
    int output_cb_count;

    /* RTC_CNTL reference for wakeup notification */
    struct Esp32s3RtcCntlState *rtc_cntl;
} ESP32S3GPIOState;

typedef struct ESP32S3GPIOClass {
    Esp32GpioClass parent;
} ESP32S3GPIOClass;

/* Public API for device models to set input pin levels */
void esp32s3_gpio_set_input(ESP32S3GPIOState *s, int gpio_num, bool level);

/* Register a callback for when a GPIO output pin changes level */
void esp32s3_gpio_register_output_cb(ESP32S3GPIOState *s,
                                      int pin,
                                      gpio_output_cb_fn fn,
                                      void *opaque);
