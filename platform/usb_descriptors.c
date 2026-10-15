#include "tusb.h"
#include "nrf.h"

/* TinyUSB example VID/PID for local development only, not a shipping identity. */
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, .idVendor = 0xcafe, .idProduct = 0x4001,
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 3, .bNumConfigurations = 1
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64)
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration;
}
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t descriptor[40];
    static const char *const strings[] = { "", "Wireless Imaging", "nRF52840 Imaging RX", "", "Radio diagnostics" };
    unsigned len = 0;
    if (index == 0) descriptor[1 + len++] = 0x0409;
    else if (index == 3) {
        static const char hex[] = "0123456789ABCDEF";
        /* FICR DEVICEID is immutable; serial path is still selected explicitly. */
        for (int word = 1; word >= 0; --word)
            for (int shift = 28; shift >= 0; shift -= 4)
                descriptor[1 + len++] = hex[(NRF_FICR->DEVICEID[word] >> shift) & 15u];
    } else {
        if (index >= sizeof strings / sizeof strings[0]) return NULL;
        const char *s = strings[index];
        while (*s && len < 39) descriptor[1 + len++] = (uint8_t)*s++;
    }
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * len + 2u));
    return descriptor;
}
