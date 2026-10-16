#ifndef IMAGE_USB_H
#define IMAGE_USB_H
#include "image_protocol.h"
#define IMAGE_USB_HEADER 24u
enum { IMAGE_USB_READY = 1, IMAGE_USB_START, IMAGE_USB_PIXELS, IMAGE_USB_RESULT, IMAGE_USB_ERROR };
void image_usb_header(uint8_t out[IMAGE_USB_HEADER], uint8_t type, uint32_t frame,
                      const uint8_t *body, uint32_t length);
void image_usb_task(image_receiver_t *s, bool radio_ready);
#endif
