#include "stm_link.h"
#include <Arduino.h>
#include <USBHost.h>
#include <USBHostSerial.h>
#include <stdio.h>

/* Must match USB_Device/App/usbd_desc.c in the Laserfabriken repo. */
#define STM_USB_VID 0x1209
#define STM_USB_PID 0x0001

/* The host handler objects live in the sketch, not the core, so a build
 * only pays for the classes it uses (the class registers itself with the
 * TinyUSB CDC callbacks in its constructor). */
static USBHostSerialClass s_serial;

enum LinkState { LINK_DOWN = 0, LINK_WRONG_DEVICE, LINK_UP };
static volatile LinkState s_state = LINK_DOWN;
static const char *s_fw_version = "0.0.0";

void stm_link_log(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.printf("# link: %s\r\n", buf);
}

void stm_link_begin(const char *fw_version)
{
    s_fw_version = fw_version;
    if (!USBHost.begin()) {
        stm_link_log("USBHost.begin() failed -- check USB Mode / CDC On Boot settings");
        return;
    }
    /* baud 0: skip SET_LINE_CODING on mount. The STM32's CDC ignores line
     * coding anyway and the fewer control transfers before hello the better. */
    s_serial.begin(0);
    stm_link_log("host started, waiting for vid=%04x pid=%04x", STM_USB_VID, STM_USB_PID);
}

/* VID/PID of the device the CDC class just bound to. USBHostSerial keeps
 * its device address private and the core owns tuh_mount_cb, so look it
 * up: TinyUSB tracks a handful of addresses, the first mounted one with a
 * CDC interface is ours (this host only ever has one device plugged in). */
static bool identify(uint16_t *vid, uint16_t *pid)
{
    for (uint8_t daddr = 1; daddr <= CFG_TUH_DEVICE_MAX; daddr++) {
        if (tuh_mounted(daddr) && tuh_vid_pid_get(daddr, vid, pid))
            return true;
    }
    return false;
}

void stm_link_task()
{
    USBHost.task();

    const bool mounted = s_serial.mounted();
    if (!mounted) {
        if (s_state != LINK_DOWN) {
            s_state = LINK_DOWN;
            stm_link_log("down");
        }
        return;
    }
    if (s_state != LINK_DOWN)
        return;                         /* already classified this mount */

    uint16_t vid = 0, pid = 0;
    if (identify(&vid, &pid) && vid == STM_USB_VID && pid == STM_USB_PID) {
        s_serial.readClear();           /* banner etc. from before we said hello */
        s_state = LINK_UP;
        stm_link_printf("hello display fw=%s proto=1\r\n", s_fw_version);
        stm_link_log("up, vid=%04x pid=%04x, hello sent", vid, pid);
    } else {
        s_state = LINK_WRONG_DEVICE;
        stm_link_log("ignoring device vid=%04x pid=%04x", vid, pid);
    }
}

bool stm_link_connected()
{
    return s_state == LINK_UP && s_serial.mounted();
}

const char *stm_link_state_str()
{
    switch (s_state) {
        case LINK_UP:           return "up";
        case LINK_WRONG_DEVICE: return "wrong device";
        default:                return "down";
    }
}

size_t stm_link_write(const char *s)
{
    if (!stm_link_connected())
        return 0;
    size_t n = s_serial.write((const uint8_t *)s, strlen(s));
    s_serial.flush();
    return n;
}

size_t stm_link_printf(const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return stm_link_write(buf);
}

int stm_link_available()
{
    return stm_link_connected() ? s_serial.available() : 0;
}

int stm_link_read()
{
    return stm_link_connected() ? s_serial.read() : -1;
}
