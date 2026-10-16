#include "image_usb.h"
#include "image_radio.h"
#include "usb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t pixels[IMAGE_BYTES], output[IMAGE_BYTES + 2048];
static unsigned used, calls, tick;
static bool connected = true, blocked;
uint32_t usb_errors;
uint32_t image_radio_turn_max;
volatile uint32_t image_radio_crc_errors, image_radio_drops;
void usb_task(void) { ++tick; }
bool usb_connected(void) { return connected; }
uint32_t time_us(void) { return tick; }
void usb_clear_output(void) {}
uint32_t usb_write_bytes(const uint8_t *p, uint32_t n)
{
    if (blocked || ++calls % 7 == 0) return 0;
    if (n > 17) n = 17; /* Headers and bodies both split; repeated backpressure. */
    assert(used + n <= sizeof output);
    memcpy(output + used, p, n); used += n;
    return n;
}
int main(int argc, char **argv)
{
    image_receiver_t s;
    image_generate(pixels); image_receiver_init(&s, pixels, 1);
    for (unsigned i = 0; i < 100; ++i) image_usb_task(&s, true);
    assert(used == 24 + 52);
    s.active = true; s.crc = image_crc32(pixels, sizeof pixels);
    s.verified = s.committed = true;
    s.stats[IM_STATUS] = IMAGE_OK; s.stats[IM_RECEIVED] = IMAGE_PACKETS;
    s.stats[IM_CRC32] = s.crc; s.stats[IM_INITIAL_PACKETS] = IMAGE_PACKETS;
    s.stats[IM_TX_TOTAL_US] = 700000; s.stats[IM_INITIAL_US] = 650000;
    blocked = true;
    for (unsigned i = 0; i < 1000; ++i) image_usb_task(&s, true);
    assert(used == 76); /* No drop when the host is not accepting bytes. */
    blocked = false;
    for (unsigned i = 0; i < 20000; ++i) image_usb_task(&s, true);
    unsigned expected = 76 + 44 + 24 + IMAGE_BYTES + 24 + 96;
    assert(used == expected);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "wb"); assert(file);
        assert(fwrite(output, 1, used, file) == used); assert(!fclose(file));
    }
    connected = false; image_usb_task(&s, true);
    used = 0; connected = true;
    for (unsigned i = 0; i < 20000; ++i) image_usb_task(&s, true);
    assert(used == expected - 76); /* Immutable frame replay from metadata on reconnect. */
    assert(!memcmp(output, "NRFIMG3\n", 8) && output[9] == IMAGE_USB_START);
    connected = false; image_usb_task(&s, true);
    used = 0; connected = true;
    for (unsigned i = 0; i < 100; ++i) image_usb_task(&s, true);
    assert(used > 44 && used < expected - 76); /* Disconnect inside image body. */
    connected = false; image_usb_task(&s, true);
    used = 0; connected = true;
    for (unsigned i = 0; i < 20000; ++i) image_usb_task(&s, true);
    assert(used == expected - 76 && output[9] == IMAGE_USB_START);
    puts("Image USB: partial writes, stalls, exact bytes, reconnect replay passed");
}
