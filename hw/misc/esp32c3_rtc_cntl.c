/*
 * ESP32-C3 RTC CNTL
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32c3_rtc_cntl.h"


#define RTCCNTL_DEBUG     0
#define RTCCNTL_WARNING   0


static uint64_t esp32c3_rtc_cntl_read(void* opaque, hwaddr addr, unsigned int size)
{
    ESP32C3RtcCntlState *s = ESP32C3_RTC_CNTL(opaque);
    uint64_t r = 0;

    switch(addr) {
        case A_RTC_CNTL_RTC_OPTIONS0:
            r = s->options0;
            break;
        case A_RTC_CNTL_RTC_RESET_STATE:
            r = ESP32C3_POWERON_RESET;
            break;
        case A_RTC_CNTL_RTC_STORE4:
            /* XTAL frequency: 40MHz, must be in both upper and lower half-word */
            r = 0x00280028;
            break;
        default:
#if RTCCNTL_WARNING
            /* Other registers are not supported yet */
            warn_report("[RTCCNTL] Unsupported read to %08lx\n", addr);
#endif
            break;
    }

    return r;
}


static void esp32c3_rtc_cntl_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    ESP32C3RtcCntlState *s = ESP32C3_RTC_CNTL(opaque);
    const uint32_t c_value = value;

    switch(addr) {
        case A_RTC_CNTL_RTC_OPTIONS0:
            CLEAR_BIT(value, R_RTC_CNTL_RTC_OPTIONS0_SW_SYS_RST_SHIFT);
            CLEAR_BIT(value, R_RTC_CNTL_RTC_OPTIONS0_SW_PROCPU_RST_SHIFT);
            s->options0 = value;
            /* Check if we have to reset the CPU/machine */
            if (FIELD_EX32(c_value, RTC_CNTL_RTC_OPTIONS0, SW_SYS_RST) ||
                FIELD_EX32(c_value, RTC_CNTL_RTC_OPTIONS0, SW_PROCPU_RST)) {
                qemu_irq_raise(s->cpu_reset);
            }
            break;
        default:
#if RTCCNTL_WARNING
            /* Other registers are not supported yet */
            warn_report("[RTCCNTL] Unsupported write to %08lx (%08lx)\n", addr, value);
#endif
            break;
    }
}


static const MemoryRegionOps esp_rtc_cntl_ops = {
    .read =  esp32c3_rtc_cntl_read,
    .write = esp32c3_rtc_cntl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static void esp32c3_rtc_cntl_reset(DeviceState *dev)
{
    ESP32C3RtcCntlState *s = ESP32C3_RTC_CNTL(dev);
    s->options0 = 0;
    qemu_irq_lower(s->cpu_reset);
}


static void esp32c3_rtc_cntl_realize(DeviceState *dev, Error **errp)
{
    ESP32C3RtcCntlState *s = ESP32C3_RTC_CNTL(dev);
    esp32c3_rtc_cntl_reset(dev);
    (void) s;
}


static void esp32c3_rtc_cntl_init(Object *obj)
{
    ESP32C3RtcCntlState *s = ESP32C3_RTC_CNTL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp_rtc_cntl_ops, s,
                          TYPE_ESP32C3_RTC_CNTL, ESP32C3_RTC_CNTL_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* Initialize the GPIO that will notify the CPU to reset itself */
    qdev_init_gpio_out_named(DEVICE(s), &s->cpu_reset, ESP32C3_RTC_CPU_RESET_GPIO, 1);
}


static void esp32c3_rtc_cntl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = esp32c3_rtc_cntl_reset;
    dc->realize = esp32c3_rtc_cntl_realize;
}


static const TypeInfo esp32c3_rtc_cntl_info = {
    .name = TYPE_ESP32C3_RTC_CNTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3RtcCntlState),
    .instance_init = esp32c3_rtc_cntl_init,
    .class_init = esp32c3_rtc_cntl_class_init
};


static void esp32c3_rtc_cntl_register_types(void)
{
    type_register_static(&esp32c3_rtc_cntl_info);
}


type_init(esp32c3_rtc_cntl_register_types)
