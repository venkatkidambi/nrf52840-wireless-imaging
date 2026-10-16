#ifndef IMAGING_USB_H
#define IMAGING_USB_H
#include <stdbool.h>
#include <stdint.h>
bool usb_init(void);
void usb_task(void);
bool usb_connected(void);
bool usb_write_line(const char *line);
/* Returns accepted count; caller retains unsent bytes and retries after task(). */
uint32_t usb_write_bytes(const uint8_t *bytes, uint32_t count);
void usb_clear_output(void);
/* Called by the locally adapted TinyUSB port if a hardware wait expires. */
void usb_hardware_fault(void);
extern uint32_t usb_errors;
extern volatile uint32_t usb_irq_max_us, usb_end_in_irq;
#endif
