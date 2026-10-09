#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "ui.h"
#include "bidi_switch_knob.h"
#include <Preferences.h>
#include "stm_link.h"

/* Settings persisted on-device (NVS), independent of the STM32 -- purely a
 * display-side behavior, so there's no protocol line and no risk to the
 * STM32's own flash-saved calibration. */
static Preferences displayPrefs;

/* Link to the STM32: USB, this display being the host (stm_link.h). The
 * old UART0 (GPIO43/44) link is gone; on UGN those pins still reach the
 * STM32 and now carry Serial's '#'-prefixed log lines instead. */

static lv_obj_t * meter;
lv_meter_indicator_t * needle;
static lv_meter_scale_t * left_gauge_scale = NULL;  // stashed so set_left_gauge_range() can re-range it per mode

static lv_obj_t * meter2;
lv_meter_indicator_t * needle2;

static lv_obj_t * ui_laserBtn = NULL;
static lv_obj_t * ui_tecBtn   = NULL;  // SPUD-only crystal TEC toggle (was the 5V button)
static bool       crystal_tec_on = false;

/* One shared display binary shows a UGN-appropriate UI or a SPUD-appropriate
 * UI depending on what the STM32 tells it (see process_stm_line()'s
 * "set device.mode ..." handling). Default SPUD: a display that hasn't
 * heard a mode line yet (e.g. right after this firmware update itself)
 * behaves exactly like it always has, not like an unconfigured UGN. */
enum { MODE_UGN = 0, MODE_SPUD = 1 };
static volatile uint8_t g_ui_mode = MODE_SPUD;

/* Boot gate: everything except the branding panel stays hidden (see
 * hide_all_for_boot()) until both of these go true, so the screen never
 * flashes the wrong mode's layout before the STM32's sync catches up. */
static volatile uint8_t g_mode_known   = 0;
static volatile uint8_t g_values_known = 0;

#define FW_VERSION "1.0.0"

#define KNOB_ENCODER_A_PIN    2
#define KNOB_ENCODER_B_PIN    1

#define SET_BIT(reg,bit)   (reg |= ((uint32_t)0x01<<bit))
#define CLEAR_BIT(reg,bit) (reg &= (~((uint32_t)0x01<<bit)))
#define READ_BIT(reg,bit)  (((uint32_t)reg>>bit) & 0x01)
#define BIT_EVEN_ALL (0x00ffffff)

EventGroupHandle_t knob_even_ = NULL;
static knob_handle_t s_knob = 0;
SemaphoreHandle_t mutex;

// value[0] = power (%, 0–100), SPUD mode
// value[1] = laser wavelength (nm, 1540.00–1560.00), SPUD mode
// value[2] = crystal temperature setpoint (C, 15.00-45.00), UGN mode --
//            knob-editable exactly like [0]/[1], just gated by g_ui_mode
//            instead of `chosen` (see user_encoder_loop_task())
float value[3] = {0.0f, 1550.0f, 25.0f};
bool  laser_on  = false;

float       step[3]   = {1.0f,   1.0f,   0.1f};
const float minVal[3] = {0.0f,  1540.0f, 15.0f};
const float maxVal[3] = {100.0f, 1560.0f, 45.0f};

// Read-only telemetry from the STM32 (UGN mode) -- never display-editable,
// unlike value[2] above (you can't "set" a measured temperature).
static float crystal_temp   = 0.0f;  // measured, drives the needle
static float crystal_output = 0.0f;  // PID output, -1.0..1.0 (not currently rendered, kept for parity/future use)
static float crystal_tec_v  = 0.0f;  // commanded (not measured) buck voltage

static int         lw_step_idx = 0; // 0=1, 1=0.1, 2=0.01
static const float lw_steps[]  = {1.0f, 0.1f, 0.01f};
static lv_obj_t  * ui_lwStepLabel = NULL;

// Crystal setpoint (value[2], UGN mode) step cycling -- same idiom as
// lw_step_idx/lw_steps above, just triggered by every click on meter (there's
// only one gauge in UGN mode, so no select-then-cycle dance is needed).
// Starts at index 1 (0.1) to match step[2]'s own initial default below.
static int         ct_step_idx = 1; // 0=1, 1=0.1, 2=0.01
static const float ct_steps[]  = {1.0f, 0.1f, 0.01f};
static lv_obj_t  * ui_ctStepLabel = NULL;

int chosen = 0;

#define DIM_TIMEOUT_MS_DEFAULT  5000u
#define DIM_TIMEOUT_MIN_MS      2000u
#define DIM_TIMEOUT_MAX_MS      30000u
#define DIM_TIMEOUT_STEP_MS     1000u
#define BRIGHT_DUTY     40
#define DIM_DUTY        0

// Adjustable from the settings overlay (long-press branding -> Settings),
// loaded from NVS in setup(), saved back on settings-overlay close.
static uint32_t dim_timeout_ms = DIM_TIMEOUT_MS_DEFAULT;

static volatile uint32_t last_activity_ms = 0;
static bool              screen_dimmed    = false;
static lv_obj_t         *dim_overlay      = NULL;
static lv_obj_t         *status_overlay   = NULL;
static lv_obj_t         *settings_overlay = NULL;
static lv_obj_t         *ui_dimTimeoutLabel = NULL;
static lv_indev_t        *s_touch_indev   = NULL;

// ── Meter init ────────────────────────────────────────────────────────────────

void create_left_gauge(void)
{
    extern lv_obj_t *ui_Screen1;
    meter = lv_meter_create(ui_Screen1);
    lv_obj_add_event_cb(meter, meter_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_center(meter);
    lv_obj_set_size(meter, 200, 200);
    lv_obj_set_pos(meter, -71, 0);
    lv_obj_set_style_bg_opa(meter, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);

    lv_meter_scale_t * scale = lv_meter_add_scale(meter);
    left_gauge_scale = scale;  // stashed for set_left_gauge_range() -- this meter is reused (not recreated) for UGN mode's temperature gauge
    lv_meter_set_scale_range(meter, scale, (int32_t)minVal[0], (int32_t)maxVal[0], 270, 135);
    lv_meter_set_scale_ticks(meter, scale, 21, 3, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter, scale, 5, 5, 15, lv_color_white(), 10);
    lv_obj_set_style_text_font(meter, &lv_font_montserrat_10, LV_PART_TICKS | LV_STATE_DEFAULT);

    lv_meter_indicator_t * indic;
    indic = lv_meter_add_arc(meter, scale, 3, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_meter_set_indicator_start_value(meter, indic, 0);
    lv_meter_set_indicator_end_value(meter, indic, 10);
    indic = lv_meter_add_scale_lines(meter, scale, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_BLUE), false, 0);
    lv_meter_set_indicator_start_value(meter, indic, 0);
    lv_meter_set_indicator_end_value(meter, indic, 10);
    indic = lv_meter_add_arc(meter, scale, 3, lv_palette_main(LV_PALETTE_RED), 0);
    lv_meter_set_indicator_start_value(meter, indic, 90);
    lv_meter_set_indicator_end_value(meter, indic, 100);
    indic = lv_meter_add_scale_lines(meter, scale, lv_palette_main(LV_PALETTE_RED), lv_palette_main(LV_PALETTE_RED), false, 0);
    lv_meter_set_indicator_start_value(meter, indic, 90);
    lv_meter_set_indicator_end_value(meter, indic, 100);

    needle = lv_meter_add_needle_line(meter, scale, 4, lv_color_hex(0xFFFFFF), -10);
    lv_obj_add_event_cb(meter, left_gauge_label_cb, LV_EVENT_ALL, NULL);

    lv_arc_set_range(ui_Arc1, (int32_t)minVal[0], (int32_t)maxVal[0]);
}

/* UGN mode re-ranges meter's scale to min*10..max*10 (see set_left_gauge_range()
 * below) so the widget's integer-only value API can represent one decimal
 * place, the same trick meter2 already uses for wavelength. Left unscaled,
 * the tick labels show the raw x10 values (e.g. "450" instead of "45").
 * SPUD mode's range (0-100) is never scaled, so its labels need no fixup. */
static void left_gauge_label_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_PART_BEGIN) return;
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_TICKS) return;
    if (dsc->type != LV_METER_DRAW_PART_TICK) return;
    if (dsc->text == NULL) return;
    if (g_ui_mode != MODE_UGN) return;
    static char buf[16];
    lv_snprintf(buf, sizeof(buf), "%" LV_PRId32, dsc->value / 10);
    dsc->text = buf;
}

/* Re-ranges the left gauge (meter/needle/ui_Arc1) for the current mode --
 * SPUD mode uses it for laser power (0-100), UGN mode repurposes the same
 * widget for temperature (15-45C) rather than creating a second gauge.
 * Called from apply_ui_mode(). */
void set_left_gauge_range(int32_t min, int32_t max)
{
    lv_meter_set_scale_range(meter, left_gauge_scale, min, max, 270, 135);
    lv_arc_set_range(ui_Arc1, min, max);
}

void create_wavelength_gauge(void)
{
    extern lv_obj_t *ui_Screen1;
    meter2 = lv_meter_create(ui_Screen1);
    lv_obj_add_event_cb(meter2, meter2_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_center(meter2);
    lv_obj_set_size(meter2, 146, 146);
    lv_obj_set_pos(meter2, 94, 15);
    lv_obj_set_style_bg_opa(meter2, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter2, 0, LV_PART_MAIN);

    lv_meter_scale_t * scale = lv_meter_add_scale(meter2);
    lv_meter_set_scale_range(meter2, scale, (int32_t)(minVal[1] * 10.0f), (int32_t)(maxVal[1] * 10.0f), 270, 135);
    lv_meter_set_scale_ticks(meter2, scale, 41, 2, 8, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter2, scale, 10, 3, 11, lv_color_white(), 10);
    lv_obj_set_style_text_font(meter2, &lv_font_montserrat_10, LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(meter2, meter2_label_cb, LV_EVENT_ALL, NULL);

    needle2 = lv_meter_add_needle_line(meter2, scale, 3, lv_color_hex(0xFFFFFF), -10);

    lv_arc_set_range(ui_Arc2, (int32_t)(minVal[1] * 10.0f), (int32_t)(maxVal[1] * 10.0f));
}

static void meter2_label_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_PART_BEGIN) return;
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_TICKS) return;
    if (dsc->type != LV_METER_DRAW_PART_TICK) return;
    if (dsc->text == NULL) return;
    static char buf[16];
    lv_snprintf(buf, sizeof(buf), "%" LV_PRId32, dsc->value / 10);
    dsc->text = buf;
}

void lv_create_laser_wl_step_ui(void)
{
    extern lv_obj_t *ui_Screen1;
    ui_lwStepLabel = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_lwStepLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_lwStepLabel, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_lwStepLabel, 94);
    lv_obj_set_y(ui_lwStepLabel, 92);
    lv_obj_set_align(ui_lwStepLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_lwStepLabel, "step:1.00");
    lv_obj_set_style_text_color(ui_lwStepLabel, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_lwStepLabel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_lwStepLabel, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// ── Laser on/off button ───────────────────────────────────────────────────────

/* Send only the one line that actually changed, not the whole state.
 * Sending all three on every knob tick meant the STM32 had to receive
 * ~90 bytes back-to-back per event with zero flow control; splitting it
 * cuts that to one short line per event, well within what a single-byte
 * receive buffer can keep up with. */
static void send_power(void)
{
    stm_link_printf("set laser.power %d\r\n", (int)value[0]);
}

static void send_wavelength(void)
{
    stm_link_printf("set crystal.wavelength %.2f\r\n", value[1]);
}

static void send_crystal_setpoint(void)
{
    stm_link_printf("set crystal.setpoint %.4f\r\n", value[2]);
}

static void send_laser_state(void)
{
    /* UGN mode: this button is repurposed as the crystal TEC toggle (see
     * update_laser_button_ui()) -- send the command that actually does
     * something on a UGN board instead of "laser on/off", which UGN
     * happens to also currently respond to as a side effect of a separate,
     * unrelated firmware quirk on the STM32 side, not something to rely on. */
    if (g_ui_mode == MODE_UGN) {
        stm_link_write(laser_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
    } else {
        stm_link_write(laser_on ? "laser on\r\n" : "laser off\r\n");
    }
}

static void send_crystal_tec_state(void)
{
    stm_link_write(crystal_tec_on ? "crystal.tec on\r\n" : "crystal.tec off\r\n");
}

// ── Receiving state pushed from the STM32 ───────────────────────────────────

/* The STM32 pushes lines back at us whenever the relevant parameter changes
 * on its side -- at boot (restoring saved params), from a USB CLI command,
 * or from something autonomous like the laser temp guard auto-disabling the
 * laser. This is what keeps the screen in sync with reality instead of just
 * reflecting the last thing this screen itself sent. Must be called with
 * `mutex` already held (both call sites already hold it), and only from the
 * LVGL task -- it touches shared state (fine, protected by the mutex like
 * everywhere else) but the *caller* is responsible for refreshing the
 * on-screen widgets afterward, since this function itself makes no LVGL
 * calls. */
static void process_stm_line(const char *line)
{
    int   ival;
    float fval;

    char hello_mode[8];
    if (sscanf(line, "hello %7s fw=", hello_mode) == 1) {
        /* STM32's answer to our hello (stm_link.cpp): board type decides
         * the UI, same as the older "set device.mode" line still does. */
        if (strcmp(hello_mode, "UGN") == 0)       { g_ui_mode = MODE_UGN;  g_mode_known = 1; }
        else if (strcmp(hello_mode, "SPUD") == 0) { g_ui_mode = MODE_SPUD; g_mode_known = 1; }
    } else if (sscanf(line, "set laser.power %d", &ival) == 1) {
        value[0] = (float)ival;
        g_values_known = 1;
    } else if (sscanf(line, "set crystal.wavelength %f", &fval) == 1) {
        value[1] = fval;
        g_values_known = 1;
    } else if (sscanf(line, "crystal.setpoint %f", &fval) == 1) {
        value[2] = fval;
        g_values_known = 1;
    } else if (sscanf(line, "crystal.temp %f", &fval) == 1) {
        crystal_temp = fval;
        g_values_known = 1;
    } else if (sscanf(line, "crystal.output %f", &fval) == 1) {
        crystal_output = fval;
        g_values_known = 1;
    } else if (sscanf(line, "crystal.tec_voltage %f", &fval) == 1) {
        crystal_tec_v = fval;
        g_values_known = 1;
    } else if (strcmp(line, "set device.mode UGN") == 0) {
        g_ui_mode = MODE_UGN;
        g_mode_known = 1;
    } else if (strcmp(line, "set device.mode SPUD") == 0) {
        g_ui_mode = MODE_SPUD;
        g_mode_known = 1;
    } else if (strcmp(line, "laser on") == 0) {
        laser_on = true;
    } else if (strcmp(line, "laser off") == 0) {
        laser_on = false;
    } else if (strcmp(line, "crystal.tec on") == 0) {
        laser_on = true;       // UGN-mode button state -- see send_laser_state()/update_laser_button_ui()
        crystal_tec_on = true; // SPUD-mode button state -- see send_crystal_tec_state()/update_tec_button_ui()
    } else if (strcmp(line, "crystal.tec off") == 0) {
        laser_on = false;
        crystal_tec_on = false;
    }
    /* anything else (e.g. the STM32's own "OK ..."/"ERR ..." replies to
     * whatever this screen just sent) is silently ignored -- not one of
     * the lines this function understands. */
}

/* Drains whatever the STM32 link has buffered and feeds complete lines to
 * process_stm_line(). Never writes back -- state received
 * here must not be echoed, or every sync would bounce back and forth. */
static void drain_stm_uart(void)
{
    static char    buf[48];
    static uint8_t pos = 0;

    while (stm_link_available()) {
        char c = (char)stm_link_read();
        if (c == '\r' || c == '\n') {
            if (pos > 0) {
                buf[pos] = '\0';
                process_stm_line(buf);
                pos = 0;
            }
        } else if (pos < sizeof(buf) - 1) {
            buf[pos++] = c;
        }
    }
}

/* Shared with the display-sync path (see drain_stm_uart): both a tap on
 * the button and an incoming sync line from the STM32 need to leave the
 * button's on-screen label/color matching `laser_on`/`crystal_tec_on`. Must
 * only be called from the LVGL task (this callback and
 * lvgl_render_task), never from user_encoder_loop_task -- see
 * wake_screen()'s comment on why LVGL calls don't belong on that task. */
static void update_laser_button_ui(void)
{
    lv_obj_t * lbl = lv_obj_get_child(ui_laserBtn, 0);
    const char *on_text  = (g_ui_mode == MODE_UGN) ? "TEC on"  : "laser on";
    const char *off_text = (g_ui_mode == MODE_UGN) ? "TEC off" : "laser off";
    if (laser_on) {
        lv_label_set_text(lbl, on_text);
        lv_obj_set_style_bg_color(ui_laserBtn, lv_color_hex(0x006600), LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(lbl, off_text);
        lv_obj_set_style_bg_color(ui_laserBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void update_tec_button_ui(void)
{
    lv_obj_t * lbl = lv_obj_get_child(ui_tecBtn, 0);
    if (crystal_tec_on) {
        lv_label_set_text(lbl, "TEC on");
        lv_obj_set_style_bg_color(ui_tecBtn, lv_color_hex(0x006600), LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(lbl, "TEC off");
        lv_obj_set_style_bg_color(ui_tecBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void laser_btn_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (xSemaphoreTake(mutex, portMAX_DELAY)) {
            laser_on = !laser_on;
            if (laser_on) last_activity_ms = millis();
            update_laser_button_ui();
            send_laser_state();
            xSemaphoreGive(mutex);
        }
    }
}

void lv_create_laser_button(void)
{
    extern lv_obj_t *ui_Screen1;
    ui_laserBtn = lv_btn_create(ui_Screen1);
    lv_obj_center(ui_laserBtn);
    lv_obj_set_size(ui_laserBtn, 85, 38);
    lv_obj_set_pos(ui_laserBtn, 43, 122);
    lv_obj_set_style_bg_color(ui_laserBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_laserBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_laserBtn, laser_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl = lv_label_create(ui_laserBtn);
    lv_label_set_text(lbl, "laser off");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
}

void tec_btn_event_cb(lv_event_t * e)
{
    if (g_ui_mode == MODE_UGN) return;  // hidden in UGN mode; belt-and-suspenders
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (xSemaphoreTake(mutex, portMAX_DELAY)) {
            crystal_tec_on = !crystal_tec_on;
            last_activity_ms = millis();
            update_tec_button_ui();
            send_crystal_tec_state();
            xSemaphoreGive(mutex);
        }
    }
}

void lv_create_tec_button(void)
{
    /* Position mirrors ui_laserBtn's offset (43, 122) with y negated: that
     * places it NE of meter (laser power, centered at -71,0) and NW of
     * meter2 (crystal wavelength, centered at 94,15) simultaneously, in the
     * gap above both dials. SPUD-only -- hidden in UGN mode by
     * apply_ui_mode() (UGN already has its own TEC toggle, see ui_laserBtn). */
    extern lv_obj_t *ui_Screen1;
    ui_tecBtn = lv_btn_create(ui_Screen1);
    lv_obj_center(ui_tecBtn);
    lv_obj_set_size(ui_tecBtn, 85, 38);
    lv_obj_set_pos(ui_tecBtn, 43, -122);
    lv_obj_set_style_bg_color(ui_tecBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_tecBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_tecBtn, tec_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl = lv_label_create(ui_tecBtn);
    lv_label_set_text(lbl, "TEC off");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
}

// ── UGN-mode text readout (replaces the right gauge) ────────────────────────

static lv_obj_t * ui_ugnTempLabel     = NULL;
static lv_obj_t * ui_ugnSetpointLabel = NULL;
static lv_obj_t * ui_ugnTecVLabel     = NULL;

/* Plain text, not gauges -- occupies the area meter2/ui_Arc2 vacate in UGN
 * mode. Created once, both modes, hidden by default; apply_ui_mode() toggles
 * visibility, lvgl_render_task() updates the text. */
void lv_create_ugn_text_labels(void)
{
    extern lv_obj_t *ui_Screen1;

    ui_ugnTempLabel = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_ugnTempLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ugnTempLabel, LV_SIZE_CONTENT);
    lv_obj_set_pos(ui_ugnTempLabel, 94, -15);
    lv_obj_set_align(ui_ugnTempLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_ugnTempLabel, "Temp: -- C");
    lv_obj_set_style_text_color(ui_ugnTempLabel, lv_color_hex(0xD8D5D5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_ugnTempLabel, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_ugnTempLabel, LV_OBJ_FLAG_HIDDEN);

    /* Positioned at meter's own bottom (same offset ui_power uses for SPUD's
     * power readout, -71,57 -- meter is reused, not recreated, for UGN's
     * temperature gauge) so the setpoint reads directly on the gauge you're
     * turning, the same way SPUD's power value does. */
    ui_ugnSetpointLabel = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_ugnSetpointLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ugnSetpointLabel, LV_SIZE_CONTENT);
    lv_obj_set_pos(ui_ugnSetpointLabel, -71, 57);
    lv_obj_set_align(ui_ugnSetpointLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_ugnSetpointLabel, "--" "\xC2\xB0" "C");
    lv_obj_set_style_text_color(ui_ugnSetpointLabel, lv_color_hex(0xD8D5D5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_ugnSetpointLabel, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_ugnSetpointLabel, LV_OBJ_FLAG_HIDDEN);

    ui_ctStepLabel = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_ctStepLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ctStepLabel, LV_SIZE_CONTENT);
    lv_obj_set_pos(ui_ctStepLabel, -71, 92);
    lv_obj_set_align(ui_ctStepLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_ctStepLabel, "step:0.10");
    lv_obj_set_style_text_color(ui_ctStepLabel, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_ctStepLabel, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_ctStepLabel, LV_OBJ_FLAG_HIDDEN);

    ui_ugnTecVLabel = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_ugnTecVLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_ugnTecVLabel, LV_SIZE_CONTENT);
    lv_obj_set_pos(ui_ugnTecVLabel, 94, 45);
    lv_obj_set_align(ui_ugnTecVLabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_ugnTecVLabel, "TEC V: -- (commanded)");
    lv_obj_set_style_text_color(ui_ugnTecVLabel, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_ugnTecVLabel, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_ugnTecVLabel, LV_OBJ_FLAG_HIDDEN);
}

/* Switches the on-screen widgets between SPUD's laser-oriented layout and
 * UGN's temperature-oriented one -- runtime visibility only (add/clear
 * LV_OBJ_FLAG_HIDDEN), never creates/destroys widgets, so there's no
 * null-pointer risk regardless of which mode is active when this first
 * runs. Called once at the end of setup() and again whenever g_ui_mode
 * actually changes (see lvgl_render_task()). */
void apply_ui_mode(uint8_t mode)
{
    bool ugn = (mode == MODE_UGN);

    /* meter/ui_laserBtn are shared by both modes and were always visible
     * before hide_all_for_boot() started hiding them at boot -- clear that
     * here unconditionally so the first apply_ui_mode() call (boot reveal)
     * actually shows them again. */
    lv_obj_clear_flag(meter,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_laserBtn, LV_OBJ_FLAG_HIDDEN);

    /* SPUD-only chrome around the left gauge -- the meter/needle/ui_Arc1
     * objects themselves are reused for UGN's temperature gauge, not hidden. */
    if (ugn) lv_obj_add_flag(ui_power,  LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_power,  LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_Label2, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_Label2, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_Label13, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_Label13, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_lwStepLabel, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_lwStepLabel, LV_OBJ_FLAG_HIDDEN);

    /* TEC button: SPUD-only (UGN already has its own TEC toggle, ui_laserBtn). */
    if (ugn) lv_obj_add_flag(ui_tecBtn, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_tecBtn, LV_OBJ_FLAG_HIDDEN);

    /* Entire right gauge cluster (laser wavelength, SPUD-only). */
    if (ugn) lv_obj_add_flag(meter2, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(meter2, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_Arc2, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_Arc2, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_power1, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_power1, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_Label1, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_Label1, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_add_flag(ui_Label12, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(ui_Label12, LV_OBJ_FLAG_HIDDEN);

    /* UGN-only text readout, in the area the right gauge just vacated
     * (ui_ugnSetpointLabel itself now sits on meter's own face, see
     * lv_create_ugn_text_labels(), but is still UGN-only chrome). */
    if (ugn) lv_obj_clear_flag(ui_ugnTempLabel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_ugnTempLabel, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_clear_flag(ui_ugnSetpointLabel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_ugnSetpointLabel, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_clear_flag(ui_ugnTecVLabel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_ugnTecVLabel, LV_OBJ_FLAG_HIDDEN);
    if (ugn) lv_obj_clear_flag(ui_ctStepLabel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_ctStepLabel, LV_OBJ_FLAG_HIDDEN);

    /* Left gauge range: temperature (UGN) vs laser power (SPUD). The x10
     * scaling matches meter2's existing convention for representing one
     * decimal place on LVGL's integer-only meter/arc value API. */
    if (ugn) {
        set_left_gauge_range((int32_t)(minVal[2] * 10.0f), (int32_t)(maxVal[2] * 10.0f));
    } else {
        set_left_gauge_range((int32_t)minVal[0], (int32_t)maxVal[0]);
    }

    update_laser_button_ui();
}

/* Called once at boot, before the STM32 has told us the mode -- hides
 * everything except the branding panel (ui_Panel8 and its labels, untouched
 * here) so the screen doesn't flash SPUD's layout and then switch to UGN's
 * a moment later. apply_ui_mode() only covers each mode's own chrome, not
 * the widgets shared by both (meter/needle/ui_Arc1, ui_laserBtn), so those
 * two need hiding here explicitly. Left via the g_mode_known/g_values_known
 * gate in lvgl_render_task(), which calls apply_ui_mode() for real once
 * both are set. */
static void hide_all_for_boot(void)
{
    lv_obj_add_flag(meter,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_laserBtn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(ui_power,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Label2,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Label13, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_lwStepLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_tecBtn,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(meter2,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Arc2,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_power1,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Label1,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Label12, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(ui_ugnTempLabel,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ugnSetpointLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ugnTecVLabel,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ctStepLabel,      LV_OBJ_FLAG_HIDDEN);
}

// ── Screen dim control ────────────────────────────────────────────────────────

static void wake_screen(void)
{
    screen_dimmed = false;
    setUpdutySubdivide(BRIGHT_DUTY);
    if (dim_overlay) { lv_obj_del(dim_overlay); dim_overlay = NULL; }
}

static void overlay_click_cb(lv_event_t *e)
{
    last_activity_ms = millis();
    wake_screen();
}

// ── Status overlay ───────────────────────────────────────────────────────────
// Same idiom as dim_overlay above (opaque child of ui_Screen1, tap to
// dismiss) -- shows firmware version, current mode, and whatever's already
// synced for that mode, triggered by a long-press on the branding panel.

static void hide_status_overlay(void)
{
    if (status_overlay) { lv_obj_del(status_overlay); status_overlay = NULL; }
}

static void status_overlay_click_cb(lv_event_t *e)
{
    last_activity_ms = millis();
    hide_status_overlay();
}

static void settings_btn_event_cb(lv_event_t *e);

static void show_status_overlay(void)
{
    if (status_overlay || settings_overlay) return;  // already open

    extern lv_obj_t *ui_Screen1;
    status_overlay = lv_obj_create(ui_Screen1);
    lv_obj_set_size(status_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(status_overlay);
    lv_obj_clear_flag(status_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(status_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(status_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_overlay, 0, 0);
    lv_obj_add_event_cb(status_overlay, status_overlay_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(status_overlay);
    char buf[160];
    if (xSemaphoreTake(mutex, portMAX_DELAY)) {
        if (g_ui_mode == MODE_UGN) {
            snprintf(buf, sizeof(buf),
                     "FW %s\nMode: UGN\n\nTemp: %.2f C\nSet: %.2f C\nTEC V: ~%.2f V",
                     FW_VERSION, crystal_temp, value[2], crystal_tec_v);
        } else {
            snprintf(buf, sizeof(buf),
                     "FW %s\nMode: SPUD\n\nPower: %d%%\nWavelength: %.2f nm",
                     FW_VERSION, (int)value[0], value[1]);
        }
        xSemaphoreGive(mutex);
    }
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -30);

    /* Separate clickable button, not covered by status_overlay_click_cb's
     * dismiss-on-any-tap: lv_btn_create() widgets are clickable and don't
     * bubble LV_EVENT_CLICKED up to their parent by default, so a tap here
     * fires only settings_btn_event_cb, not the overlay's own dismiss. */
    lv_obj_t *settingsBtn = lv_btn_create(status_overlay);
    lv_obj_set_size(settingsBtn, 110, 38);
    lv_obj_align(settingsBtn, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_event_cb(settingsBtn, settings_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *settingsLbl = lv_label_create(settingsBtn);
    lv_label_set_text(settingsLbl, "Settings");
    lv_obj_center(settingsLbl);

    last_activity_ms = millis();
}

static void branding_long_press_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        show_status_overlay();
    }
}

// ── Settings overlay ─────────────────────────────────────────────────────────
// Reached via the "Settings" button inside the status overlay. Only one
// setting for now (dim timeout) -- same "one editable thing, knob always
// targets it" idiom as UGN mode's temperature setpoint, no select step
// needed. Turning the knob adjusts the in-RAM value live; the label refresh
// happens on lvgl_render_task (see its settings_overlay != NULL check),
// never directly from the encoder task. Saved to NVS on close, not on every
// tick, to avoid a flash write per detent.

static void update_dim_timeout_label(void)
{
    if (!ui_dimTimeoutLabel) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Dim timeout: %lu s", (unsigned long)(dim_timeout_ms / 1000u));
    lv_label_set_text(ui_dimTimeoutLabel, buf);
}

static void hide_settings_overlay(void)
{
    if (settings_overlay) {
        displayPrefs.putUInt("dim_ms", dim_timeout_ms);
        lv_obj_del(settings_overlay);
        settings_overlay   = NULL;
        ui_dimTimeoutLabel = NULL;
    }
}

static void settings_overlay_click_cb(lv_event_t *e)
{
    last_activity_ms = millis();
    hide_settings_overlay();
}

static void show_settings_overlay(void)
{
    if (settings_overlay) return;  // already open

    extern lv_obj_t *ui_Screen1;
    settings_overlay = lv_obj_create(ui_Screen1);
    lv_obj_set_size(settings_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(settings_overlay);
    lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(settings_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(settings_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_overlay, 0, 0);
    lv_obj_add_event_cb(settings_overlay, settings_overlay_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(settings_overlay);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    ui_dimTimeoutLabel = lv_label_create(settings_overlay);
    lv_obj_set_style_text_color(ui_dimTimeoutLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ui_dimTimeoutLabel, LV_ALIGN_CENTER, 0, 0);
    update_dim_timeout_label();

    lv_obj_t *hint = lv_label_create(settings_overlay);
    lv_label_set_text(hint, "turn knob to adjust\ntap to save and exit");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 50);

    last_activity_ms = millis();
}

static void settings_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        hide_status_overlay();
        show_settings_overlay();
    }
}

// ── Knob callbacks ────────────────────────────────────────────────────────────

static void _knob_left_cb(void *arg, void *data)
{
    uint8_t eventBits_ = 0;
    SET_BIT(eventBits_, 0);
    xEventGroupSetBits(knob_even_, eventBits_);
}
static void _knob_right_cb(void *arg, void *data)
{
    uint8_t eventBits_ = 0;
    SET_BIT(eventBits_, 1);
    xEventGroupSetBits(knob_even_, eventBits_);
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup()
{
    mutex = xSemaphoreCreateMutex();
    displayPrefs.begin("display", false);
    dim_timeout_ms = displayPrefs.getUInt("dim_ms", DIM_TIMEOUT_MS_DEFAULT);
    if (dim_timeout_ms < DIM_TIMEOUT_MIN_MS || dim_timeout_ms > DIM_TIMEOUT_MAX_MS) {
        dim_timeout_ms = DIM_TIMEOUT_MS_DEFAULT;  // guard against corrupt/first-ever NVS read
    }
    Serial.begin(115200);   // UART0 (USB CDC On Boot must be Disabled) -- log only, see stm_link.h
    stm_link_begin(FW_VERSION);
    Touch_Init();
    lcd_lvgl_Init();
    /* Stable handle to the touch device for polling its press state directly
     * (see lvgl_render_task) -- lv_indev_drv_register()'s return value
     * is discarded in lcd_bsp.cpp, and lv_indev_get_act() only returns
     * non-NULL while LVGL is actively processing that device, not when
     * polled afterward from the render loop. Only one indev is registered
     * on this board (the touch panel), so the first one is it. */
    s_touch_indev = lv_indev_get_next(NULL);
    create_left_gauge();
    create_wavelength_gauge();
    lv_create_laser_wl_step_ui();
    lv_create_laser_button();
    lv_create_tec_button();
    lv_create_ugn_text_labels();

    set_active_meter(chosen);
    lcd_bl_pwm_bsp_init(BRIGHT_DUTY);

    /* Nothing but branding shows until the STM32's mode+values actually
     * arrive -- see hide_all_for_boot() and the g_mode_known/g_values_known
     * gate in lvgl_render_task(). */
    hide_all_for_boot();

    /* Long-press the branding panel to open the status overlay -- new
     * infrastructure, no long-press handling exists anywhere else on this
     * screen. ui_Panel8 is lv_obj_create()-based (clickable by default);
     * the two label objects layered on top of it are lv_label_create()-based
     * and never given LV_OBJ_FLAG_CLICKABLE, so touches on the text should
     * fall through to the panel underneath. */
    extern lv_obj_t *ui_Panel8;
    lv_obj_add_event_cb(ui_Panel8, branding_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    last_activity_ms = millis();

    knob_even_ = xEventGroupCreate();
    knob_config_t cfg = {
        .gpio_encoder_a = KNOB_ENCODER_A_PIN,
        .gpio_encoder_b = KNOB_ENCODER_B_PIN,
    };
    s_knob = iot_knob_create(&cfg);
    iot_knob_register_cb(s_knob, KNOB_LEFT,  _knob_right_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, _knob_left_cb,  NULL);

    xTaskCreate(user_encoder_loop_task, "user_encoder_loop_task", 3000, NULL, 2, NULL);
    xTaskCreate(lvgl_render_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
}

// ── Encoder task ──────────────────────────────────────────────────────────────

static void user_encoder_loop_task(void *arg)
{
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(knob_even_, BIT_EVEN_ALL, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));

        if (settings_overlay != NULL) {
            /* Settings overlay open -- knob adjusts dim_timeout_ms instead of
             * power/wavelength/setpoint. Only the value changes here; the
             * label refresh happens on lvgl_render_task (LVGL calls don't
             * belong on this task, see wake_screen()'s comment). */
            if (xSemaphoreTake(mutex, portMAX_DELAY)) {
                if (READ_BIT(even, 0) && dim_timeout_ms > DIM_TIMEOUT_MIN_MS) {
                    dim_timeout_ms -= DIM_TIMEOUT_STEP_MS;
                }
                if (READ_BIT(even, 1) && dim_timeout_ms < DIM_TIMEOUT_MAX_MS) {
                    dim_timeout_ms += DIM_TIMEOUT_STEP_MS;
                }
                xSemaphoreGive(mutex);
            }
            last_activity_ms = millis();
            continue;
        }

        if (status_overlay != NULL) {
            /* Status overlay open -- don't silently change power/wavelength/
             * setpoint behind a screen the user can't see. */
            continue;
        }

        /* UGN mode has exactly one editable value (the temperature setpoint,
         * value[2]) and no gauge-selection step -- the knob always targets
         * it, regardless of `chosen` (which stays meaningful only in SPUD
         * mode, where there are two selectable gauges). */
        int idx = (g_ui_mode == MODE_UGN) ? 2 : chosen;

        if (READ_BIT(even, 0)) {
            if (xSemaphoreTake(mutex, portMAX_DELAY)) {
                value[idx] -= step[idx];
                if (value[idx] < minVal[idx]) value[idx] = minVal[idx];
                if (g_ui_mode == MODE_UGN) send_crystal_setpoint();
                else if (idx == 0) send_power(); else send_wavelength();
                xSemaphoreGive(mutex);
            }
            last_activity_ms = millis();
        }
        if (READ_BIT(even, 1)) {
            if (xSemaphoreTake(mutex, portMAX_DELAY)) {
                value[idx] += step[idx];
                if (value[idx] > maxVal[idx]) value[idx] = maxVal[idx];
                if (g_ui_mode == MODE_UGN) send_crystal_setpoint();
                else if (idx == 0) send_power(); else send_wavelength();
                xSemaphoreGive(mutex);
            }
            last_activity_ms = millis();
        }
    }
}

// ── LVGL render task ──────────────────────────────────────────────────────────

static void lvgl_render_task(void *arg)
{
    float    prev[3]              = {-1.0f, -1.0f, -1.0f};
    float    prev_crystal_temp    = -1000.0f;
    float    prev_crystal_tec_v   = -1000.0f;
    bool     lo                   = false;
    bool     prev_laser_on        = false;
    bool     prev_crystal_tec_on  = false;
    uint8_t  prev_ui_mode         = 0xFFu;  // impossible sentinel -- forces apply_ui_mode() on the first pass too
    bool     boot_revealed        = false;
    uint32_t prev_dim_timeout_ms  = 0xFFFFFFFFu;

    for (;;) {
        lv_timer_handler();

        /* Reset the dim timer on a touch anywhere on the screen, not just
         * knob turns. A per-widget LV_EVENT_PRESSED callback on ui_Screen1
         * doesn't work for this: LVGL doesn't bubble press events from a
         * child widget up to its parent by default, and the screen is
         * almost entirely covered by the meters/labels/button, so taps
         * essentially never reached ui_Screen1's own handler. Polling the
         * touch device's own state directly (via the stable handle grabbed
         * in setup()) catches a touch anywhere, regardless of which widget
         * (if any) it landed on -- lv_indev_get_act() doesn't work here,
         * it's only valid while LVGL is actively processing that device,
         * not when polled afterward from this loop. */
        if (s_touch_indev != NULL && s_touch_indev->proc.state == LV_INDEV_STATE_PRESSED) {
            last_activity_ms = millis();
        }

        if (xSemaphoreTake(mutex, portMAX_DELAY)) {
            char buf[32];

            /* Pull in anything the STM32 pushed since the last iteration
             * (boot sync, a USB CLI command, an autonomous change like the
             * laser temp guard tripping) before reacting to value/laser_on/
             * crystal_tec_on below, so a sync update renders in the same pass
             * as a local knob/button change would. */
            drain_stm_uart();

            lo = laser_on;

            if (!boot_revealed) {
                if (g_mode_known && g_values_known) {
                    boot_revealed = true;
                    prev_ui_mode  = g_ui_mode;
                    apply_ui_mode(g_ui_mode);
                }
            } else if (g_ui_mode != prev_ui_mode) {
                prev_ui_mode = g_ui_mode;
                apply_ui_mode(g_ui_mode);
            }

            if (g_ui_mode == MODE_SPUD) {
                if (value[0] != prev[0]) {
                    prev[0] = value[0];
                    snprintf(buf, sizeof(buf), "%d%%", (int)value[0]);
                    lv_label_set_text(ui_power, buf);
                    lv_meter_set_indicator_value(meter, needle, (int)value[0]);
                    lv_arc_set_value(ui_Arc1, (int)value[0]);
                }

                if (value[1] != prev[1]) {
                    prev[1] = value[1];
                    snprintf(buf, sizeof(buf), "%.2f", value[1]);
                    lv_label_set_text(ui_power1, buf);
                    lv_meter_set_indicator_value(meter2, needle2, (int)(value[1] * 10.0f));
                    lv_arc_set_value(ui_Arc2, (int)(value[1] * 10.0f));
                }
            } else {
                /* UGN mode: needle+arc both track the setpoint (value[2]),
                 * same as SPUD's meter1/meter2 both tracking their one knob-
                 * editable value -- so the gauge visibly responds the instant
                 * the knob turns, instead of waiting on the physically-slow
                 * measured temperature to catch up. The measured temp still
                 * has its own readout (ui_ugnTempLabel), just not tied to the
                 * needle. Same x10 integer-scale convention as meter2's
                 * wavelength handling above. */
                if (crystal_temp != prev_crystal_temp) {
                    prev_crystal_temp = crystal_temp;
                    snprintf(buf, sizeof(buf), "Temp: %.2f C", crystal_temp);
                    lv_label_set_text(ui_ugnTempLabel, buf);
                }

                if (value[2] != prev[2]) {
                    prev[2] = value[2];
                    lv_meter_set_indicator_value(meter, needle, (int)(value[2] * 10.0f));
                    lv_arc_set_value(ui_Arc1, (int)(value[2] * 10.0f));
                    snprintf(buf, sizeof(buf), "%.2f" "\xC2\xB0" "C", value[2]);
                    lv_label_set_text(ui_ugnSetpointLabel, buf);
                }

                if (crystal_tec_v != prev_crystal_tec_v) {
                    prev_crystal_tec_v = crystal_tec_v;
                    snprintf(buf, sizeof(buf), "TEC V: ~%.2f V", crystal_tec_v);
                    lv_label_set_text(ui_ugnTecVLabel, buf);
                }
            }

            if (laser_on != prev_laser_on) {
                prev_laser_on = laser_on;
                update_laser_button_ui();
                last_activity_ms = millis();
            }

            if (crystal_tec_on != prev_crystal_tec_on) {
                prev_crystal_tec_on = crystal_tec_on;
                update_tec_button_ui();
                last_activity_ms = millis();
            }

            if (settings_overlay != NULL && dim_timeout_ms != prev_dim_timeout_ms) {
                prev_dim_timeout_ms = dim_timeout_ms;
                update_dim_timeout_label();
            }

            xSemaphoreGive(mutex);
        }

        uint32_t now = millis();
        if (lo && !screen_dimmed && (now - last_activity_ms >= dim_timeout_ms)) {
            screen_dimmed = true;
            setUpdutySubdivide(DIM_DUTY);
            extern lv_obj_t *ui_Screen1;
            dim_overlay = lv_obj_create(ui_Screen1);
            lv_obj_set_size(dim_overlay, LV_HOR_RES, LV_VER_RES);
            lv_obj_center(dim_overlay);
            lv_obj_clear_flag(dim_overlay, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(dim_overlay, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(dim_overlay, 0, 0);
            lv_obj_add_event_cb(dim_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);
        } else if (!lo && screen_dimmed) {
            /* Laser turned off while dimmed. */
            wake_screen();
        } else if (screen_dimmed && (now - last_activity_ms < dim_timeout_ms)) {
            /* Knob turns update last_activity_ms directly from
             * user_encoder_loop_task (a different FreeRTOS task) without
             * touching LVGL objects there -- calling lv_obj_del()/etc. from
             * a non-LVGL task isn't safe. So a knob turn while dimmed can't
             * wake the screen itself the way a tap on dim_overlay does
             * (that's a real LVGL click event, handled by overlay_click_cb,
             * always on this task). Detecting the resulting fresh
             * last_activity_ms here, from the LVGL task's own loop, is the
             * safe equivalent for the knob case. */
            wake_screen();
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ── Meter touch callbacks ─────────────────────────────────────────────────────

void meter_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (g_ui_mode == MODE_UGN) {
        /* UGN has one gauge and no gauge-selection step (see
         * user_encoder_loop_task()) -- every click cycles the setpoint step
         * size directly instead, mirroring meter2_event_cb's step-cycle
         * branch below. */
        if (xSemaphoreTake(mutex, portMAX_DELAY)) {
            ct_step_idx = (ct_step_idx + 1) % 3;
            step[2] = ct_steps[ct_step_idx];
            xSemaphoreGive(mutex);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "step:%.2f", ct_steps[ct_step_idx]);
        lv_label_set_text(ui_ctStepLabel, buf);
        return;
    }

    chosen = 0;
    set_active_meter(chosen);
}
void meter2_event_cb(lv_event_t * e)
{
    if (g_ui_mode == MODE_UGN) return;  // meter2 is hidden in UGN mode; belt-and-suspenders
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (chosen == 1) {
            if (xSemaphoreTake(mutex, portMAX_DELAY)) {
                lw_step_idx = (lw_step_idx + 1) % 3;
                step[1] = lw_steps[lw_step_idx];
                xSemaphoreGive(mutex);
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "step:%.2f", lw_steps[lw_step_idx]);
            lv_label_set_text(ui_lwStepLabel, buf);
        } else {
            chosen = 1;
            set_active_meter(chosen);
        }
    }
}
void set_active_meter(int index)
{
    lv_obj_set_style_border_width(meter,  0, LV_PART_MAIN);
    lv_obj_set_style_border_width(meter2, 0, LV_PART_MAIN);
    switch (index) {
        case 0:
            lv_obj_set_style_border_width(meter, 2, LV_PART_MAIN);
            lv_obj_set_style_border_color(meter, lv_color_hex(0x574509), LV_PART_MAIN);
            break;
        case 1:
            lv_obj_set_style_border_width(meter2, 2, LV_PART_MAIN);
            lv_obj_set_style_border_color(meter2, lv_color_hex(0x574509), LV_PART_MAIN);
            break;
    }
}

void loop()
{
    stm_link_task();   /* USB host housekeeping + mount/hello state machine */
    delay(1);
}
