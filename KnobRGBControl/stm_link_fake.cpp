/*
 * stm_link_fake -- a simulated STM32 behind the stm_link API.
 *
 * Bench demo for the display when no UGN/SPUD board is at hand: the USB host
 * stack is not started at all; instead this file plays the STM32's part of
 * the protocol (hello, full state, authoritative replies, live telemetry)
 * so the UI can be exercised end to end.
 *
 * Build with:  compiler.cpp.extra_flags=-DSTM_LINK_FAKE
 *   (arduino-cli: --build-property "compiler.cpp.extra_flags=-DSTM_LINK_FAKE")
 * Optional:    -DSTM_LINK_FAKE_MODE=\"UGN\"   (default "SPUD")
 *
 * Nothing here is compiled into a normal build; stm_link.cpp is compiled
 * out instead when STM_LINK_FAKE is defined.
 *
 * What the fake does (all formats copied from Laserfabriken cli.c):
 *   - "mounts" 1.5 s after boot, exactly like a real device would;
 *   - answers "hello display ..." with "hello <MODE> fw=9.9.9 proto=1" and
 *     the full state (send_state()), same for "get all";
 *   - answers every set/on/off with the authoritative line (confirm()),
 *     clamping laser.power to 0..100 so the clamp is visible on screen;
 *   - streams crystal.temp / crystal.output / crystal.tec_voltage at 5 Hz,
 *     the temperature settling towards the setpoint with some noise;
 *   - every 20 s changes laser.power on its own, as a USB-CLI user on the
 *     STM32 would, so the display is seen following the board.
 */
#ifdef STM_LINK_FAKE
#include "stm_link.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef STM_LINK_FAKE_MODE
#define STM_LINK_FAKE_MODE "SPUD"
#endif
#define FAKE_FW "9.9.9"

/* ── simulated STM32 state ─────────────────────────────────────────────── */
static unsigned s_power_pct  = 35;
static float    s_setpoint_c = 25.0f;
static float    s_temp_c     = 22.0f;
static float    s_output     = 0.0f;
static bool     s_laser_on   = false;
static bool     s_tec_on     = true;
/* crystal wavelength model, wl = k*T + m (cli.c g_crystal_wl_k/_m) */
static const float K_NM_PER_C = 0.3f;
static const float M_NM       = 1056.5f;

static float wavelength_nm() { return K_NM_PER_C * s_setpoint_c + M_NM; }

/* ── byte queue STM32 -> display (what stm_link_read() returns) ─────────── */
static char     s_rx[2048];
static size_t   s_rx_head = 0, s_rx_tail = 0;

static void rx_put(const char *s)
{
    for (; *s; s++) {
        size_t next = (s_rx_head + 1) % sizeof(s_rx);
        if (next == s_rx_tail) return;          /* full: drop, like a lost packet */
        s_rx[s_rx_head] = *s;
        s_rx_head = next;
    }
}

static void reply(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rx_put(buf);
}

/* ── the STM32's side of the protocol ──────────────────────────────────── */
static void send_state()
{
    reply("set device.mode %s\r\n", STM_LINK_FAKE_MODE);
    reply("set laser.power %u\r\n", s_power_pct);
    reply("set crystal.wavelength %.2f\r\n", wavelength_nm());
    reply(s_laser_on ? "laser on\r\n" : "laser off\r\n");
    reply("5v on\r\n");
    reply(s_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
    reply("crystal.setpoint %.4f\r\n", s_setpoint_c);
    reply("crystal.temp %.4f\r\n", s_temp_c);
    reply("crystal.output %.4f\r\n", s_output);
    reply("crystal.tec_voltage %.2f\r\n", s_output * 4.5f);
}

static void confirm_setpoint()
{
    reply("crystal.setpoint %.4f\r\n", s_setpoint_c);
    reply("set crystal.wavelength %.2f\r\n", wavelength_nm());
}

static void process_display_line(const char *line)
{
    int   ival;
    float fval;

    stm_link_log("fake-stm32 <- \"%s\"", line);

    if (strncmp(line, "hello", 5) == 0) {
        reply("hello %s fw=%s proto=1\r\n", STM_LINK_FAKE_MODE, FAKE_FW);
        send_state();
    } else if (strcmp(line, "get all") == 0) {
        send_state();
    } else if (sscanf(line, "set laser.power %d", &ival) == 1) {
        if (ival < 0) ival = 0;
        if (ival > 100) ival = 100;
        s_power_pct = (unsigned)ival;
        reply("set laser.power %u\r\n", s_power_pct);
    } else if (sscanf(line, "set crystal.wavelength %f", &fval) == 1) {
        s_setpoint_c = (fval - M_NM) / K_NM_PER_C;
        confirm_setpoint();
    } else if (sscanf(line, "set crystal.setpoint %f", &fval) == 1) {
        s_setpoint_c = fval;
        confirm_setpoint();
    } else if (strcmp(line, "laser on") == 0 || strcmp(line, "laser off") == 0) {
        s_laser_on = (line[6] == 'n');
        reply(s_laser_on ? "laser on\r\n" : "laser off\r\n");
        reply(s_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
    } else if (strcmp(line, "crystal.tec on") == 0 || strcmp(line, "crystal.tec off") == 0) {
        s_tec_on = (line[12] == 'o' && line[13] == 'n');
        reply(s_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
    }
    /* anything else: the real STM32 would answer "ERR ...", which the
     * display ignores -- so just stay quiet. */
}

/* ── link state machine + telemetry ────────────────────────────────────── */
static bool     s_up = false;
static uint32_t s_t0 = 0;
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
    s_t0 = millis();
    stm_link_log("FAKE STM32 (%s) -- no USB host started, simulated link", STM_LINK_FAKE_MODE);
}

void stm_link_task()
{
    static uint32_t last_telemetry = 0, last_remote_change = 0;
    const uint32_t now = millis();

    if (!s_up) {
        if (now - s_t0 < 1500) return;                /* "enumeration" */
        s_up = true;
        stm_link_printf("hello display fw=%s proto=1\r\n", s_fw_version);
        stm_link_log("up (fake), hello sent");
        last_telemetry = last_remote_change = now;
        return;
    }

    if (now - last_telemetry >= 200) {               /* 5 Hz, like the real board */
        last_telemetry = now;
        if (s_tec_on) {
            float err = s_setpoint_c - s_temp_c;
            s_output  = constrain(err * 0.8f, -1.0f, 1.0f);
            s_temp_c += err * 0.04f + ((float)random(-100, 101)) * 0.0003f;
        } else {
            s_output  = 0.0f;
            s_temp_c += (22.0f - s_temp_c) * 0.01f;    /* drifts back to ambient */
        }
        reply("crystal.temp %.4f\r\n", s_temp_c);
        reply("crystal.output %.4f\r\n", s_output);
        reply("crystal.tec_voltage %.2f\r\n", s_output * 4.5f);
    }

    if (now - last_remote_change >= 20000) {          /* "someone typed on the STM32 CLI" */
        last_remote_change = now;
        s_power_pct = (s_power_pct + 15) % 101;
        reply("set laser.power %u\r\n", s_power_pct);
        stm_link_log("fake-stm32: remote change laser.power -> %u", s_power_pct);
    }
}

bool        stm_link_connected()  { return s_up; }
const char *stm_link_state_str()  { return s_up ? "up (FAKE)" : "down"; }

/* display -> STM32: line-buffer and hand to the simulated board */
size_t stm_link_write(const char *s)
{
    static char   buf[64];
    static size_t pos = 0;
    if (!s_up) return 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r' || c == '\n') {
            if (pos) { buf[pos] = '\0'; process_display_line(buf); pos = 0; }
        } else if (pos < sizeof(buf) - 1) {
            buf[pos++] = c;
        }
    }
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
    return (int)((s_rx_head + sizeof(s_rx) - s_rx_tail) % sizeof(s_rx));
}

int stm_link_read()
{
    if (s_rx_head == s_rx_tail) return -1;
    char c = s_rx[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % sizeof(s_rx);
    return (unsigned char)c;
}
#endif /* STM_LINK_FAKE */
