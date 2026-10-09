#pragma once
/*
 * stm_link -- the display's transport to the STM32 laser controller.
 *
 * The STM32 is a USB CDC-ACM device; this ESP32-S3 is the USB host
 * (Docs/spud_usb_displaylank.md in the Laserfabriken repo). Built on the
 * Arduino core's USBHost + USBHostSerial (core 3.3.12 or newer).
 *
 * Only a device with the Laserfabriken VID/PID is accepted. On every mount
 * the link sends "hello display fw=<ver> proto=1"; the STM32 answers
 * "hello <UGN|SPUD> fw=<ver> proto=1" followed by its state, which the
 * sketch parses in process_stm_line() like any other line.
 *
 * Board settings (Tools menu): USB Mode = "USB-OTG (TinyUSB)",
 * USB CDC On Boot = "Disabled". The second one matters: with it enabled the
 * core starts the TinyUSB *device* stack on the same PHY at boot and host
 * mode cannot start. Serial is then UART0 (GPIO43/44) -- on UGN that is
 * wired to the STM32, which forwards '#'-prefixed lines to its debug
 * console and ignores everything else, so stm_link_log() lines are visible
 * there without any extra cable.
 */
#include <stddef.h>
#include <stdarg.h>

void   stm_link_begin(const char *fw_version);  /* once, in setup() */
void   stm_link_task();                         /* every loop() iteration */
bool   stm_link_connected();                    /* right device mounted and hello sent */
const char *stm_link_state_str();               /* "down" / "wrong device" / "up" */

size_t stm_link_write(const char *s);           /* whole string, blocking until queued */
size_t stm_link_printf(const char *fmt, ...);
int    stm_link_available();
int    stm_link_read();                         /* one byte, or -1 */

void   stm_link_log(const char *fmt, ...);      /* "# link: ..." on Serial */
