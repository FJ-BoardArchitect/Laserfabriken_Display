#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "ui.h"
#include "bidi_switch_knob.h"

/* Hardware UART to STM32 -- GPIO43(U0TX)/GPIO44(U0RX), the board's default
 * UART0 pins. Confirmed via the JC3636K718 schematic that these aren't used
 * by any other peripheral on this board (LCD/touch/knob/audio/mic/battery
 * ADC/SD/RGB all use other pins, USB is native on GPIO19/20) -- they're
 * broken out to the inter-board FPC connector on their own, unclaimed by
 * anything else. Separate from Serial (native USB CDC), so no conflict. */
HardwareSerial SerialSTM(0);

static const char *TAG = "encoder";

static lv_obj_t * meter;
lv_meter_indicator_t * needle;

static lv_obj_t * meter2;
lv_meter_indicator_t * needle2;

static lv_obj_t * ui_laserBtn = NULL;
static lv_obj_t * ui_5vBtn    = NULL;
static bool       v5_on       = false;

#define EXAMPLE_ENCODER_ECA_PIN    2
#define EXAMPLE_ENCODER_ECB_PIN    1

#define SET_BIT(reg,bit)   (reg |= ((uint32_t)0x01<<bit))
#define CLEAR_BIT(reg,bit) (reg &= (~((uint32_t)0x01<<bit)))
#define READ_BIT(reg,bit)  (((uint32_t)reg>>bit) & 0x01)
#define BIT_EVEN_ALL (0x00ffffff)

EventGroupHandle_t knob_even_ = NULL;
static knob_handle_t s_knob = 0;
SemaphoreHandle_t mutex;

// value[0] = power (%, 0–100)
// value[1] = laser wavelength (nm, 1540.00–1560.00)
float value[2] = {0.0f, 1550.0f};
bool  laser_on  = false;

float       step[2]   = {1.0f,   1.0f};
const float minVal[2] = {0.0f,  1540.0f};
const float maxVal[2] = {100.0f, 1560.0f};

static int         lw_step_idx = 0; // 0=1, 1=0.1, 2=0.01
static const float lw_steps[]  = {1.0f, 0.1f, 0.01f};
static lv_obj_t  * ui_lwStepLabel = NULL;

int chosen = 0;

#define DIM_TIMEOUT_MS  5000
#define BRIGHT_DUTY     40
#define DIM_DUTY        0

static volatile uint32_t last_activity_ms = 0;
static bool              screen_dimmed    = false;
static lv_obj_t         *dim_overlay      = NULL;
static lv_indev_t        *s_touch_indev   = NULL;

// ── Meter init ────────────────────────────────────────────────────────────────

void lv_example_meter_1(void)
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
    lv_meter_set_scale_range(meter, scale, 0, 100, 270, 135);
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

    lv_arc_set_range(ui_Arc1, 0, 100);
}

void lv_example_meter_2(void)
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
    lv_meter_set_scale_range(meter2, scale, 15400, 15600, 270, 135);
    lv_meter_set_scale_ticks(meter2, scale, 41, 2, 8, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter2, scale, 10, 3, 11, lv_color_white(), 10);
    lv_obj_set_style_text_font(meter2, &lv_font_montserrat_10, LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(meter2, meter2_label_cb, LV_EVENT_ALL, NULL);

    needle2 = lv_meter_add_needle_line(meter2, scale, 3, lv_color_hex(0xFFFFFF), -10);

    lv_arc_set_range(ui_Arc2, 15400, 15600);
}

void lv_example_meter_3(void) {}  // removed

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
    SerialSTM.printf("set laser.power %d\r\n", (int)value[0]);
}

static void send_wavelength(void)
{
    SerialSTM.printf("set crystal.wavelength %.2f\r\n", value[1]);
}

static void send_laser_state(void)
{
    SerialSTM.print(laser_on ? "laser on\r\n" : "laser off\r\n");
}

static void send_5v_state(void)
{
    SerialSTM.print(v5_on ? "5v on\r\n" : "5v off\r\n");
}

// ── Receiving state pushed from the STM32 ───────────────────────────────────

/* The STM32 pushes these same four lines back at us whenever the relevant
 * parameter changes on its side -- at boot (restoring saved params), from
 * a USB CLI command, or from something autonomous like the laser temp
 * guard auto-disabling the laser. This is what keeps the screen in sync
 * with reality instead of just reflecting the last thing this screen itself
 * sent. Must be called with `mutex` already held (both call sites already
 * hold it), and only from the LVGL task -- it touches `value`/`laser_on`/
 * `v5_on` (fine, protected by the mutex like everywhere else) but the
 * *caller* is responsible for refreshing the on-screen widgets afterward,
 * since this function itself makes no LVGL calls. */
static void process_stm_line(const char *line)
{
    int   ival;
    float fval;

    if (sscanf(line, "set laser.power %d", &ival) == 1) {
        value[0] = (float)ival;
    } else if (sscanf(line, "set crystal.wavelength %f", &fval) == 1) {
        value[1] = fval;
    } else if (strcmp(line, "laser on") == 0) {
        laser_on = true;
    } else if (strcmp(line, "laser off") == 0) {
        laser_on = false;
    } else if (strcmp(line, "5v on") == 0) {
        v5_on = true;
    } else if (strcmp(line, "5v off") == 0) {
        v5_on = false;
    }
    /* anything else (e.g. the STM32's own "OK ..."/"ERR ..." replies to
     * whatever this screen just sent) is silently ignored -- not one of
     * the four synced parameters */
}

/* Drains whatever SerialSTM has buffered and feeds complete lines to
 * process_stm_line(). Never writes back to SerialSTM -- state received
 * here must not be echoed, or every sync would bounce back and forth. */
static void drain_stm_uart(void)
{
    static char    buf[48];
    static uint8_t pos = 0;

    while (SerialSTM.available()) {
        char c = (char)SerialSTM.read();
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
 * button's on-screen label/color matching `laser_on`/`v5_on`. Must only be
 * called from the LVGL task (this callback and example_lvgl_port_task),
 * never from user_encoder_loop_task -- see wake_screen()'s comment on why
 * LVGL calls don't belong on that task. */
static void update_laser_button_ui(void)
{
    lv_obj_t * lbl = lv_obj_get_child(ui_laserBtn, 0);
    if (laser_on) {
        lv_label_set_text(lbl, "laser on");
        lv_obj_set_style_bg_color(ui_laserBtn, lv_color_hex(0x006600), LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(lbl, "laser off");
        lv_obj_set_style_bg_color(ui_laserBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void update_5v_button_ui(void)
{
    lv_obj_t * lbl = lv_obj_get_child(ui_5vBtn, 0);
    if (v5_on) {
        lv_label_set_text(lbl, "5v on");
        lv_obj_set_style_bg_color(ui_5vBtn, lv_color_hex(0x006600), LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_label_set_text(lbl, "5v off");
        lv_obj_set_style_bg_color(ui_5vBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
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

void v5_btn_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (xSemaphoreTake(mutex, portMAX_DELAY)) {
            v5_on = !v5_on;
            last_activity_ms = millis();
            update_5v_button_ui();
            send_5v_state();
            xSemaphoreGive(mutex);
        }
    }
}

void lv_create_5v_button(void)
{
    /* Position mirrors ui_laserBtn's offset (43, 122) with y negated: that
     * places it NE of meter (laser power, centered at -71,0) and NW of
     * meter2 (crystal wavelength, centered at 94,15) simultaneously, in the
     * gap above both dials. */
    extern lv_obj_t *ui_Screen1;
    ui_5vBtn = lv_btn_create(ui_Screen1);
    lv_obj_center(ui_5vBtn);
    lv_obj_set_size(ui_5vBtn, 85, 38);
    lv_obj_set_pos(ui_5vBtn, 43, -122);
    lv_obj_set_style_bg_color(ui_5vBtn, lv_color_hex(0x880000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_5vBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_5vBtn, v5_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl = lv_label_create(ui_5vBtn);
    lv_label_set_text(lbl, "5v off");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(lbl);
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
    Serial.begin(115200);
    SerialSTM.begin(115200, SERIAL_8N1, 44, 43);  // RX=GPIO44(U0RX), TX=GPIO43(U0TX)
    Touch_Init();
    lcd_lvgl_Init();
    /* Stable handle to the touch device for polling its press state directly
     * (see example_lvgl_port_task) -- lv_indev_drv_register()'s return value
     * is discarded in lcd_bsp.cpp, and lv_indev_get_act() only returns
     * non-NULL while LVGL is actively processing that device, not when
     * polled afterward from the render loop. Only one indev is registered
     * on this board (the touch panel), so the first one is it. */
    s_touch_indev = lv_indev_get_next(NULL);
    lv_example_meter_1();
    lv_example_meter_2();
    lv_create_laser_wl_step_ui();
    lv_create_laser_button();
    lv_create_5v_button();

    set_active_meter(chosen);
    lcd_bl_pwm_bsp_init(BRIGHT_DUTY);

    last_activity_ms = millis();

    knob_even_ = xEventGroupCreate();
    knob_config_t cfg = {
        .gpio_encoder_a = EXAMPLE_ENCODER_ECA_PIN,
        .gpio_encoder_b = EXAMPLE_ENCODER_ECB_PIN,
    };
    s_knob = iot_knob_create(&cfg);
    iot_knob_register_cb(s_knob, KNOB_LEFT,  _knob_right_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, _knob_left_cb,  NULL);

    xTaskCreate(user_encoder_loop_task, "user_encoder_loop_task", 3000, NULL, 2, NULL);
    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
}

// ── Encoder task ──────────────────────────────────────────────────────────────

static void user_encoder_loop_task(void *arg)
{
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(knob_even_, BIT_EVEN_ALL, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));

        if (READ_BIT(even, 0)) {
            if (xSemaphoreTake(mutex, portMAX_DELAY)) {
                value[chosen] -= step[chosen];
                if (value[chosen] < minVal[chosen]) value[chosen] = minVal[chosen];
                if (chosen == 0) send_power(); else send_wavelength();
                xSemaphoreGive(mutex);
            }
            last_activity_ms = millis();
        }
        if (READ_BIT(even, 1)) {
            if (xSemaphoreTake(mutex, portMAX_DELAY)) {
                value[chosen] += step[chosen];
                if (value[chosen] > maxVal[chosen]) value[chosen] = maxVal[chosen];
                if (chosen == 0) send_power(); else send_wavelength();
                xSemaphoreGive(mutex);
            }
            last_activity_ms = millis();
        }
    }
}

// ── LVGL render task ──────────────────────────────────────────────────────────

static void example_lvgl_port_task(void *arg)
{
    float prev[2]          = {-1.0f, -1.0f};
    bool  lo               = false;
    bool  prev_laser_on    = false;
    bool  prev_v5_on       = false;

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
            char buf[16];

            /* Pull in anything the STM32 pushed since the last iteration
             * (boot sync, a USB CLI command, an autonomous change like the
             * laser temp guard tripping) before reacting to value/laser_on/
             * v5_on below, so a sync update renders in the same pass as a
             * local knob/button change would. */
            drain_stm_uart();

            lo = laser_on;

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

            if (laser_on != prev_laser_on) {
                prev_laser_on = laser_on;
                update_laser_button_ui();
                last_activity_ms = millis();
            }

            if (v5_on != prev_v5_on) {
                prev_v5_on = v5_on;
                update_5v_button_ui();
                last_activity_ms = millis();
            }

            xSemaphoreGive(mutex);
        }

        uint32_t now = millis();
        if (lo && !screen_dimmed && (now - last_activity_ms >= DIM_TIMEOUT_MS)) {
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
        } else if (screen_dimmed && (now - last_activity_ms < DIM_TIMEOUT_MS)) {
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
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) { chosen = 0; set_active_meter(chosen); }
}
void meter2_event_cb(lv_event_t * e)
{
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

void loop() {}
