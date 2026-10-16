#include "usb.h"
#include "clock.h"
#include "radio.h"
#include "nrf.h"
#include "tusb.h"
#include <string.h>

void tusb_hal_nrf_power_event(uint32_t event);
enum { USB_DETECTED = 0, USB_REMOVED = 1, USB_READY = 2 };
static bool detected, ready, failed;
static uint32_t detected_at;
uint32_t usb_errors;
volatile uint32_t usb_irq_max_us, usb_end_in_irq;
volatile bool usb_irq_active;

void USBD_IRQHandler(void)
{
    uint32_t start = time_us();
    uint32_t events = radio_rx_events;
    usb_irq_active = true;
    tud_int_handler(0);
    usb_irq_active = false;
    usb_end_in_irq += radio_rx_events - events;
    uint32_t duration = time_us() - start;
    if (duration > usb_irq_max_us) usb_irq_max_us = duration;
}

void usb_hardware_fault(void)
{
    failed = true;
    ++usb_errors;
    NRF_USBD->USBPULLUP = 0;
    NVIC_DisableIRQ(USBD_IRQn);
    NRF_USBD->INTENCLR = 0xffffffffu;
    NRF_USBD->ENABLE = 0;
}

bool usb_init(void)
{
    /* Poll VBUS status in main, so no POWER/CLOCK interrupt or nrfx driver. */
    NRF_USBD->USBPULLUP = 0;
    NRF_USBD->INTENCLR = 0xffffffffu;
    NRF_USBD->ENABLE = 0;
    NVIC_SetPriority(USBD_IRQn, 6);
    return tud_init(0);
}

void usb_task(void)
{
    uint32_t status = NRF_POWER->USBREGSTATUS;
    if (!(status & POWER_USBREGSTATUS_VBUSDETECT_Msk)) {
        if (detected) tusb_hal_nrf_power_event(USB_REMOVED);
        detected = ready = failed = false;
    } else if (!failed) {
        if (!detected) {
            detected = true;
            detected_at = time_us();
            tusb_hal_nrf_power_event(USB_DETECTED);
        }
        /* Avoid blocking on the peripheral startup event in the USB port. */
        if (!ready && (status & POWER_USBREGSTATUS_OUTPUTRDY_Msk) &&
            (NRF_USBD->EVENTCAUSE & USBD_EVENTCAUSE_READY_Msk) && clock_hf_running()) {
            tusb_hal_nrf_power_event(USB_READY);
            ready = !failed;
        }
        if (!ready && (uint32_t)(time_us() - detected_at) > 100000u)
            usb_hardware_fault();
    }
    tud_task_ext(0, false); /* Nonblocking OS_NONE service. */
    if (tud_cdc_available()) tud_cdc_read_flush(); /* Diagnostics are output only. */
}

bool usb_connected(void) { return !failed && tud_cdc_connected(); }

uint32_t usb_write_bytes(const uint8_t *bytes, uint32_t count)
{
    if (!usb_connected()) return 0;
    uint32_t written = tud_cdc_write(bytes, count);
    tud_cdc_write_flush();
    return written;
}
void usb_clear_output(void) { tud_cdc_write_clear(); }

bool usb_write_line(const char *line)
{
    size_t n = strlen(line);
    if (!usb_connected() || tud_cdc_write_available() < n) return false;
    uint32_t written = tud_cdc_write(line, (uint32_t)n);
    tud_cdc_write_flush();
    return written == n;
}
