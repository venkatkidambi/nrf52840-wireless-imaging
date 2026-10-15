#!/usr/bin/env python3
"""Adapt pinned TinyUSB port without modifying the upstream dependency tree.

RADIO owns HFXO for the entire runtime. USB detach must not stop it.
Bound the upstream port's two hardware startup waits using TIMER0.
"""
from pathlib import Path
import sys

def prepare(source):
    start = source.index("static void hfclk_disable(void) {")
    end = source.index("\n// Power & Clock Peripheral", start)
    source = source[:start] + """static void hfclk_disable(void) {
  /* Application owns HFXO for RADIO, including while USB is detached. */
}
""" + source[end:]
    source = '#include "clock.h"\n#include "usb.h"\n' + source
    replacements = {
        "while (!(USBD_EVENTCAUSE_READY_Msk & NRF_USBD->EVENTCAUSE)) {}":
        """uint32_t ready_start = time_us();
      while (!(USBD_EVENTCAUSE_READY_Msk & NRF_USBD->EVENTCAUSE)) {
        if ((uint32_t)(time_us() - ready_start) >= 100000u) {
          usb_hardware_fault(); return;
        }
      }""",
        "while (!hfclk_running()) {}":
        """uint32_t clock_start = time_us();
      while (!hfclk_running()) {
        if ((uint32_t)(time_us() - clock_start) >= 100000u) {
          usb_hardware_fault(); return;
        }
      }""",
    }
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise ValueError("Pinned TinyUSB port changed; review adaptation")
        source = source.replace(old, new)
    return source

if __name__ == "__main__":
    Path(sys.argv[2]).write_text(prepare(Path(sys.argv[1]).read_text()))
