#include "machine_identity.h"

#include "pico/stdio_usb.h"
#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "tusb.h"

#ifndef USBD_VID
#define USBD_VID (0x2E8A)
#endif

#ifndef USBD_PID
#if PICO_RP2040
#define USBD_PID (0x000a)
#else
#define USBD_PID (0x0009)
#endif
#endif

#ifndef USBD_MANUFACTURER
#define USBD_MANUFACTURER "MEGA65"
#endif

#if !PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE
#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#else
#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)
#endif

#if !PICO_STDIO_USB_DEVICE_SELF_POWERED
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE (0)
#define USBD_MAX_POWER_MA (250)
#else
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE TUSB_DESC_CONFIG_ATT_SELF_POWERED
#define USBD_MAX_POWER_MA (1)
#endif

#define USBD_ITF_CDC       (0)
#if !PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE
#define USBD_ITF_MAX       (2)
#else
#define USBD_ITF_RPI_RESET (2)
static_assert(USBD_ITF_RPI_RESET == PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF,
              "USBD_ITF_RPI_RESET must match PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF");
#define USBD_ITF_MAX       (3)
#endif

#define USBD_CDC_EP_CMD (0x81)
#define USBD_CDC_EP_OUT (0x02)
#define USBD_CDC_EP_IN (0x82)
#define USBD_CDC_CMD_MAX_SIZE (8)
#define USBD_CDC_IN_OUT_MAX_SIZE (64)

#define USBD_STR_0 (0x00)
#define USBD_STR_MANUF (0x01)
#define USBD_STR_PRODUCT (0x02)
#define USBD_STR_SERIAL (0x03)
#define USBD_STR_CDC (0x04)
#define USBD_STR_RPI_RESET (0x05)

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
#if PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE && PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR
    .bcdUSB = 0x0210,
#else
    .bcdUSB = 0x0200,
#endif
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_0, USBD_DESC_LEN,
        USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE, USBD_MAX_POWER_MA),

    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC, USBD_STR_CDC, USBD_CDC_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE, USBD_CDC_EP_OUT, USBD_CDC_EP_IN, USBD_CDC_IN_OUT_MAX_SIZE),

#if PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE
    TUD_RPI_RESET_DESCRIPTOR(USBD_ITF_RPI_RESET, USBD_STR_RPI_RESET),
#endif
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return usbd_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
#ifndef USBD_DESC_STR_MAX
#define USBD_DESC_STR_MAX (64)
#elif USBD_DESC_STR_MAX > 127
#error USBD_DESC_STR_MAX too high
#elif USBD_DESC_STR_MAX < 17
#error USBD_DESC_STR_MAX too low
#endif
    static uint16_t desc_str[USBD_DESC_STR_MAX];

    if (!usbd_serial_str[0]) {
        pico_get_unique_board_id_string(usbd_serial_str, sizeof usbd_serial_str);
    }

    const char *str = NULL;
    uint8_t len = 0;
    if (index == 0) {
        desc_str[1] = 0x0409;
        len = 1;
    } else {
        switch (index) {
        case USBD_STR_MANUF:
            str = USBD_MANUFACTURER;
            break;
        case USBD_STR_PRODUCT:
            str = machine_identity_usb_product();
            break;
        case USBD_STR_SERIAL:
            str = usbd_serial_str;
            break;
        case USBD_STR_CDC:
            str = "Board CDC";
            break;
#if PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE
        case USBD_STR_RPI_RESET:
            str = "Reset";
            break;
#endif
        default:
            return NULL;
        }
        for (len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len) {
            desc_str[1 + len] = (uint8_t)str[len];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
