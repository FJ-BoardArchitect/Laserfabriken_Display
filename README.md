# Laserfabriken Display — ESP32-S3 pekskärm/ratt-firmware

Firmware för den runda pekskärmen med ratt (Guition JC3636K718C, ESP32-S3) som fungerar som display/kontrollpanel för Laserfabrikens STM32-baserade laserkontroller (`Laserfabriken_v1`). Det här dokumentet är en startpunkt för att bygga och förstå projektet utan att ha varit med i utvecklingen.

Huvudsketchen är **`KnobRGBControl/KnobRGBControl.ino`**.

---

## 1. Hårdvara

- **Skärm/MCU-modul:** Guition JC3636K718C — 1.85" rund 360×360 IPS, kapacitiv touch (CST816), rotationsratt, ESP32-S3R8 (8MB PSRAM, 16MB Flash)
- **Anslutning till STM32-kortet (UGN):** en 20-pin FPC-kabel (AFC42-S20FMA-1H) rakt in i UGN-kortets egen M8-kontakt — **inte** via displaymodulens egen bas-kort/USB-C. UGN-kortet fungerar som strömkälla (5V) och kommunikationsvärd för displayen i den färdiga produkten.
- **UART till STM32:** GPIO43 (U0TX) / GPIO44 (U0RX) — kortets ordinarie UART0-pinnar. Bekräftat via schemat att inget annat på kortet (LCD/touch/ratt/ljud/mikrofon/batteri-ADC/SD/RGB-ring) använder dessa; USB är nativt på separata GPIO19/20.
- **Ström:** hela displaymodulen strömförsörjs via 5V från UGN-kortets M6-regulator, genom samma FPC-kabel (pinnar 1 och 20). **Viktigt:** stänger man av 5V-utgången på UGN (CLI-kommandot `5v off`, eller knappen på skärmen) stängs hela displayen av — det är inte en bugg, det är hur strömförsörjningen faktiskt är kopplad. Se `Laserfabriken_v1/Docs/` för mer.
- **GPIO0/GPIO45/GPIO46:** ESP32-S3:s boot-strap-pinnar, kopplade till UGN-kortets `DISPLAY_DO1/DO2/DO3` (PB13/14/15). UGN-firmwaren måste hålla GPIO0 hög vid start (annars startar displayen aldrig — den fastnar i ROM-bootloadern) och GPIO46 låg (aldrig hög — chipets egen dokumentation kräver det). Se `Laserfabriken_v1/README.md` och git-historiken om detta någonsin behöver ändras.

---

## 2. Bygga och flasha (Arduino IDE)

### 2.1 Nödvändiga bibliotek

Installera via Arduino IDE:s bibliotekshanterare (eller `arduino-cli lib install`):

| Bibliotek | Version som använts |
|---|---|
| `lvgl` | 8.4.0 |
| `ESP32_Display_Panel` | 1.0.4 |
| `esp-lib-utils` | 0.2.3 |
| `ESP32_IO_Expander` | 1.1.1 |

`lv_conf.h` (LVGL:s konfigurationsfil) måste ligga i Arduino-bibliotekens rotmapp (`~/Arduino/libraries/lv_conf.h`), inte inuti `lvgl`-mappen — annars hittar inte LVGL den. Kontrollera vilka Montserrat-fontstorlekar som är aktiverade där (`LV_FONT_MONTSERRAT_*`) innan du använder en ny fontstorlek i koden — flera är avstängda som standard, och det ger ett kompileringsfel, inte en tyst fallback.

### 2.2 Board-inställningar (Tools-menyn)

Följande inställningar är bekräftat fungerande (verifierad flashning på riktig hårdvara):

| Inställning | Värde |
|---|---|
| Board | `ESP32S3 Dev Module` |
| USB CDC On Boot | `Enabled` |
| CPU Frequency | `240MHz (WiFi)` |
| Core Debug Level | `None` |
| USB DFU On Boot | `Disabled` |
| Erase All Flash Before Sketch Upload | `Disabled` |
| Events Run On | `Core 1` |
| Flash Mode | `QIO 120MHz` |
| Flash Size | `16MB (128Mb)` |
| JTAG Adapter | `Disabled` |
| Arduino Runs On | `Core 1` |
| USB Firmware MSC On Boot | `Disabled` |
| Partition Scheme | `Huge APP (3MB No OTA/1MB SPIFFS)` |
| PSRAM | `OPI PSRAM` |
| Upload Mode | `UART0 / Hardware CDC` |
| Upload Speed | `921600` |
| USB Mode | `Hardware CDC and JTAG` |
| Zigbee Mode | `Disabled` |

**`USB CDC On Boot: Enabled` är kritiskt** — annars hamnar `Serial` (USB-debug) på samma fysiska UART0-pinnar som `SerialSTM` (kommunikationen till STM32) använder, och de två krockar. Se kommentaren högst upp i `KnobRGBControl.ino`.

### 2.3 Flashning

Med bas-kortet (Guitions egna, med USB-C och strömbrytare): koppla in FPC-kabeln mellan displaymodulen och bas-kortet, slå på strömbrytaren, anslut USB-C, välj rätt port i Arduino IDE, tryck Upload.

Det finns ingen väg att flasha displayen *via* UGN-kortets USB-port i nuläget — STM32-firmwaren har ingen transparent USB↔UART-brygga implementerad för det syftet.

---

## 3. Arkitektur

Sketchen kör tre FreeRTOS-uppgifter:

1. **`example_lvgl_port_task`** — LVGL:s renderingsloop. **All** LVGL-API-anrop måste ske härifrån (LVGL är inte trådsäkert). Läser även av UART-mottagning (`drain_stm_uart()`) och touch-input direkt i samma loop.
2. **`user_encoder_loop_task`** — hanterar rattens vänster/höger-events. Rör **aldrig** LVGL-objekt direkt (skriver bara till delade variabler, skyddade av `mutex`) — se kommentarerna kring `wake_screen()` för varför.
3. LVGL:s egna interna uppgifter (touch-drivrutin m.m.)

**Delat tillstånd** (`value[0]` = laser-effekt, `value[1]` = våglängd, `laser_on`, `v5_on`) skyddas av en gemensam `SemaphoreHandle_t mutex`. Rendering-loopen jämför varje varv mot föregående värden (`prev[]`) och uppdaterar bara de UI-element som faktiskt ändrats.

### 3.1 Synk-protokoll mot STM32

Samma enkla textprotokoll används i båda riktningarna, över `SerialSTM` (`HardwareSerial(0)` på GPIO43/44):

```
set laser.power <0-100>
set crystal.wavelength <nm, t.ex. 1550.00>
laser on | laser off
5v on | 5v off
```

- **Skickas till STM32** (`send_power()`, `send_wavelength()`, `send_laser_state()`, `send_5v_state()`): bara när användaren faktiskt ändrar något via ratten/knapparna, och bara den enskilda raden som ändrats — inte hela tillståndet varje gång (STM32:ns mottagningsbuffert är bara 1 byte djup, så mindre trafik = mindre risk för tappade tecken).
- **Tas emot från STM32** (`drain_stm_uart()` → `process_stm_line()`): STM32 skickar samma format tillbaka vid uppstart (återställda sparade värden) och när något ändras på STM32-sidan — via USB-CLI, eller autonomt (t.ex. att lasertemperaturvakten stänger av lasern). Mottagna rader uppdaterar bara de delade variablerna — **skickar aldrig något tillbaka**, annars skulle synkningen studsa fram och tillbaka i all oändlighet.

---

## 4. Kända begränsningar

- Ingen bekräftelse/felhantering i synk-protokollet — om en rad tappas på vägen (borde inte hända längre efter STM32-sidans avbrottsdrivna mottagning, men teoretiskt möjligt) upptäcks det inte förrän nästa ändring händer att synka.
- `Serial` (USB-debug) har ingen kvarvarande debug-utskrift i normal drift — lades till temporärt vid felsökning och togs bort igen. Lägg till egna `Serial.print()`-anrop vid behov, men kom ihåg att USB och UGN-länken inte alltid går att ha inkopplade samtidigt beroende på fysisk uppställning (se avsnitt 2.3).
- Ingen kod har testkompilerats med `arduino-cli` under utvecklingen av det här projektet (endast manuellt via Arduino IDE) — förvänta dig att behöva felsöka kompileringsfel efter större ändringar, särskilt sådana som rör LVGL:s API (versionen som faktiskt är installerad, 8.4.0, skiljer sig ibland från vad som antas baserat på minnet/dokumentation).
