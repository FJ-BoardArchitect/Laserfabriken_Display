# Porting from Waveshare ESP32-S3-Knob to JC3636K718C (ST77916)

The original code was based on the Waveshare Knob18Meters project which targets the
SH8601 AMOLED display. The JC3636K718C uses an ST77916 driver chip with different
wiring, so several things had to be changed.

---

## 1. LCD Driver Library

**Problem:** The Waveshare project used a custom low-level `esp_lcd_sh8601` driver.
The ST77916 is a different chip and needs the `ESP32_Display_Panel` Arduino library
which has proper ST77916 support.

**Files changed:**
- `KnobRGBControl/lcd_bsp.c` — emptied (implementation moved to `.cpp`)
- `KnobRGBControl/lcd_bsp.cpp` — new file, full rewrite using `ESP_PanelBusQSPI` and `ESP_PanelLcd_ST77916`
- `KnobRGBControl/lcd_bsp.h` — stripped down to just the `lcd_lvgl_Init()` declaration, removed all SH8601-specific includes
- `KnobRGBControl/esp_lcd_sh8601.c` — emptied (ESP32_Display_Panel library ships its own copy, keeping the local one caused a duplicate symbol linker error)

---

## 2. GPIO Pins

**Problem:** Every pin was wrong. The Waveshare and JC3636K718C boards have completely
different wiring for the LCD, backlight, touch, and encoder.

| Signal | Waveshare (old) | JC3636K718C (new) |
|---|---|---|
| LCD CS | 14 | 12 |
| LCD CLK | 13 | 11 |
| LCD D0 | 15 | 13 |
| LCD D1 | 16 | 14 |
| LCD D2 | 17 | 15 |
| LCD D3 | 18 | 16 |
| LCD RST | 21 | 17 |
| Backlight | 47 | 21 |
| Touch SCL | 12 | 10 |
| Touch SDA | 11 | 9 |
| Touch INT | (missing) | 7 |
| Touch RST | (missing) | 8 |
| Encoder A | 8 | 2 |
| Encoder B | 7 | 1 |

**Files changed:**
- `KnobRGBControl/lcd_config.h` — all LCD, backlight, and touch pin definitions updated; touch INT and RST pins added
- `KnobRGBControl/KnobRGBControl.ino` — encoder pins updated (`EXAMPLE_ENCODER_ECA_PIN`, `EXAMPLE_ENCODER_ECB_PIN`)

---

## 3. Color Inversion

**Problem:** The ST77916 requires `invertColor(true)` to be called after initialisation
or the panel renders everything with inverted colours, which appears as a black screen.
The SH8601 code did not need this.

**Files changed:**
- `KnobRGBControl/lcd_bsp.cpp` — `lcd_panel->invertColor(true)` added after `lcd_panel->begin()`

---

## 4. Touch Driver

**Problem:** The old code used a hand-written I2C touch driver (`cst816.cpp`) on the
wrong pins with no INT or RST support. The new code uses `ESP_PanelTouch_CST816S` from
the ESP32_Display_Panel library, wired to the correct pins and integrated directly into
`lcd_lvgl_Init()`.

**Files changed:**
- `KnobRGBControl/lcd_bsp.cpp` — touch init and read callback now use `ESP_PanelTouch_CST816S`
- `KnobRGBControl/cst816.cpp` — both functions stubbed out (`Touch_Init` is a no-op, touch is handled in `lcd_bsp.cpp`)
- `KnobRGBControl/cst816.h` — removed `driver/i2c.h` dependency

---

## 5. Encoder Direction

**Problem:** Turning the knob clockwise decreased the meter value instead of increasing it.

**Fix:** Swapped which callback fires on `KNOB_LEFT` vs `KNOB_RIGHT` in software.
The hardware pin definitions are kept correct — the reversal is purely logical.

**Files changed:**
- `KnobRGBControl/KnobRGBControl.ino` — `iot_knob_register_cb` calls for LEFT and RIGHT swapped

---

## Arduino IDE Settings

See `Laserfabriken_Display/README.md` §2.2 ("Board-inställningar") for the board settings — that table is confirmed working on real hardware and is kept up to date there. (This section used to duplicate a shorter version of that table here; removed to avoid the two drifting out of sync.)

The key difference from the Waveshare settings is **Flash Mode: QIO 120MHz** (Waveshare used 80MHz).

---

## 6. ESP-NOW Callback Signature (Arduino Core 3.x)

**Problem:** The original code used the old ESP-NOW send callback signature from Arduino
core 2.x. Core 3.x changed the first argument from `const uint8_t *mac_addr` to
`const wifi_tx_info_t *mac_addr`, causing a compile error.

**Files changed:**
- `KnobRGBControl/KnobRGBControl.ino` — `OnDataSent` first parameter updated to `const wifi_tx_info_t *mac_addr`

---

## 7. lv_conf.h Placement

**Problem:** LVGL requires `lv_conf.h` to sit **beside** the `lvgl` library folder, not
inside it. If it is inside the library folder, LVGL ignores it and falls back to its
default template, causing missing symbols or unexpected compile errors.

**Correct structure:**
```
~/Arduino/libraries/
├── lvgl/          <- the library folder
└── lv_conf.h      <- must be here, one level up
```

**Files changed:** `lv_conf.h` moved from inside the `lvgl/` folder to `~/Arduino/libraries/`

---

## 8. Removed LVGL Demos Header

**Problem:** The original `lcd_bsp.h` included `"demos/lv_demos.h"`. This file only
exists if the LVGL library is installed with demos enabled. With demos disabled in
`lv_conf.h` the include causes a compile error.

**Files changed:**
- `KnobRGBControl/lcd_bsp.h` — `#include "demos/lv_demos.h"` removed
