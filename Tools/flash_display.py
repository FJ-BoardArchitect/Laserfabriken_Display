#!/usr/bin/env python3
"""Build and flash the Laserfabriken display (ESP32-S3, Guition JC3636K718C).

Asks which firmware to build, builds it with arduino-cli (the copy bundled
with Arduino IDE is found automatically), waits for the display to show up
in its ROM bootloader, flashes, verifies, and tells you when to move the
cable back.

    flash_display.py                  # menu: which variant, then build + flash
    flash_display.py --variant usbc   # no menu (usbc | uart | demo)
    flash_display.py --build-only     # compile, don't flash
    flash_display.py --flash-only     # flash the last build of the chosen variant
    flash_display.py --knob-debug     # add encoder diagnostics ('# knob ...' lines)

Variants:
    usbc   for the SPUD board: the display is a USB host on SPUD's USB-C
           (branch usb-c-Test = this working tree). Board settings: USB Mode
           "USB-OTG (TinyUSB)", USB CDC On Boot "Disabled".
    uart   for the UGN board: UART0 over the FPC to UGN's M8 connector
           (branch main, exported with git archive -- nothing in the working
           tree is touched). Board settings: USB Mode "Hardware CDC and JTAG",
           USB CDC On Boot "Enabled". UGN only; SPUD has no FPC.
    demo   (not in the menu, --variant demo) usbc source with a simulated
           STM32 inside the display (-DSTM_LINK_FAKE); no board needed.

Why BOOT must be held: in USB-host mode the display never shows up on a PC,
so the ROM bootloader has to be entered by hand -- hold BOOT while the display
gets power, then release. The script waits for that.

Requirements: python3, arduino-cli (or Arduino IDE), ESP32 core 3.3.12+ with
the libraries listed in README 2.1. No pip packages.
"""
import argparse, glob, os, shutil, subprocess, sys, time

HERE   = os.path.dirname(os.path.abspath(__file__))
ROOT   = os.path.abspath(os.path.join(HERE, ".."))
SKETCH = os.path.join(ROOT, "KnobRGBControl")
BUILD  = os.path.join(HERE, "build")          # Tools/build/<variant>/ (gitignored)

FQBN_COMMON = ("UploadSpeed=921600,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,"
               "CPUFreq=240,FlashMode=qio120,FlashSize=16M,PartitionScheme=huge_app,"
               "DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,"
               "JTAGAdapter=default,ZigbeeMode=default")
# USB-host firmware: USB Mode "USB-OTG (TinyUSB)", USB CDC On Boot "Disabled"
FQBN_OTG   = "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default," + FQBN_COMMON
# UART firmware:     USB Mode "Hardware CDC and JTAG", USB CDC On Boot "Enabled"
FQBN_HWCDC = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc," + FQBN_COMMON

VARIANTS = {
    "usbc": {"title": "USB-C mot SPUD-kortet  (gren usb-c-Test, arbetskatalogen)",
             "fqbn": FQBN_OTG,   "flags": [],                  "ref": None,   "menu": True},
    "uart": {"title": "UART mot UGN-kortet    (gren main)",
             "fqbn": FQBN_HWCDC, "flags": [],                  "ref": "main", "menu": True},
    "demo": {"title": "Demo:  simulerad STM32 i displayen, inget kort",
             "fqbn": FQBN_OTG,   "flags": ["-DSTM_LINK_FAKE"], "ref": None,   "menu": False},
}

ARDUINO_CLI_CANDIDATES = [
    "arduino-cli",
    "/opt/arduino-ide/resources/app/lib/backend/resources/arduino-cli",
    "/usr/lib/arduino-ide/resources/app/lib/backend/resources/arduino-cli",
    os.path.expanduser("~/.local/bin/arduino-cli"),
]

def find_arduino_cli(explicit):
    for c in ([explicit] if explicit else []) + ARDUINO_CLI_CANDIDATES:
        path = shutil.which(c) or (c if os.path.isfile(c) and os.access(c, os.X_OK) else None)
        if path:
            return path
    sys.exit("arduino-cli hittades inte. Installera Arduino IDE eller arduino-cli, eller ange --arduino-cli <sökväg>.")

def check_core(cli):
    out = subprocess.run([cli, "core", "list"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("esp32:esp32"):
            ver = line.split()[1]
            major, minor, patch = (int(x) for x in ver.split(".")[:3])
            if (major, minor, patch) < (3, 3, 12):
                sys.exit(f"ESP32-kärna {ver} är för gammal: host-länken kräver 3.3.12 eller nyare (USBHost-biblioteket).")
            return ver
    sys.exit("ESP32-kärnan (esp32:esp32) är inte installerad i arduino-cli / Arduino IDE.")

def choose_variant():
    print("Vilken firmware ska byggas?")
    keys = [k for k, v in VARIANTS.items() if v["menu"]]
    for i, k in enumerate(keys, 1):
        print(f"  {i}) {k:<5} {VARIANTS[k]['title']}")
    while True:
        ans = input(f"Val [1-{len(keys)}]: ").strip().lower()
        if ans in keys:
            return ans
        if ans.isdigit() and 1 <= int(ans) <= len(keys):
            return keys[int(ans) - 1]
        print("  ange en siffra eller namnet")

def export_sketch(ref, dest):
    """Export KnobRGBControl/ from a git ref into dest/KnobRGBControl (read-only copy)."""
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    tar = subprocess.run(["git", "-C", ROOT, "archive", "--format=tar", ref, "KnobRGBControl"],
                         capture_output=True)
    if tar.returncode != 0:
        sys.exit(f"git archive {ref} misslyckades: {tar.stderr.decode(errors='replace').strip()}")
    subprocess.run(["tar", "-x", "-C", dest], input=tar.stdout, check=True)
    sha = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", ref], capture_output=True, text=True).stdout.strip()
    print(f"  källkod: gren/ref '{ref}' ({sha}) exporterad till {os.path.relpath(dest, ROOT)}/")
    return os.path.join(dest, "KnobRGBControl")

def build(cli, variant, knob_debug, ref_override=None):
    v = VARIANTS[variant]
    out_dir = os.path.join(BUILD, variant)
    os.makedirs(out_dir, exist_ok=True)
    ref = ref_override or v["ref"]
    sketch = export_sketch(ref, os.path.join(out_dir, "src")) if ref else SKETCH
    if not ref:
        print(f"  källkod: arbetskatalogen {os.path.relpath(SKETCH, ROOT)}/")
    flags = list(v["flags"]) + (["-DKNOB_DEBUG"] if knob_debug else [])
    if knob_debug and ref:
        print("  obs: --knob-debug har ingen effekt i UART-varianten (finns bara i usb-c-Test)")
    cmd = [cli, "compile", "--fqbn", v["fqbn"], "--build-path", out_dir, "--warnings", "none"]
    if flags:
        cmd += ["--build-property", "compiler.cpp.extra_flags=" + " ".join(flags)]
    cmd.append(sketch)
    mode = "USB-OTG (TinyUSB), CDC On Boot Disabled" if v["fqbn"] is FQBN_OTG else "Hardware CDC and JTAG, CDC On Boot Enabled"
    print(f"\nBygger '{variant}'{' + KNOB_DEBUG' if knob_debug else ''} ...  USB-läge: {mode}; flaggor: {' '.join(flags) or 'inga'}")
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    log = p.stdout + p.stderr
    with open(os.path.join(out_dir, "compile.log"), "w") as f:
        f.write(log)
    if p.returncode != 0:
        errs = [l for l in log.splitlines() if "error" in l.lower()]
        print("\n".join(errs[-15:]) or log[-3000:], file=sys.stderr)
        sys.exit(f"kompileringen misslyckades (hela loggen: {out_dir}/compile.log)")
    for l in log.splitlines():
        if l.startswith("Sketch uses") or l.startswith("Global variables"):
            print("  " + l)
    print(f"  klart på {time.time() - t0:.0f} s -> {os.path.relpath(out_dir, ROOT)}/")
    return out_dir

def find_display_port():
    for p in glob.glob("/dev/serial/by-id/*Espressif*"):
        return os.path.realpath(p)
    return None

def wait_for_bootloader(timeout_s):
    port = find_display_port()
    if port:
        print(f"\nDisplayen syns redan på {port} (bootloader eller UART-firmware med USB-CDC).")
        return port
    print("\nDisplayen syns inte på datorn. Så här sätter du den i bootloadern:")
    print("  1. Dra ur displayens USB-C-kabel.")
    print("  2. Håll BOOT nedtryckt.")
    print("  3. Sätt i kabeln i datorn, vänta en sekund, släpp BOOT.")
    print(f"Väntar upp till {timeout_s} s ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        port = find_display_port()
        if port:
            print(f"\n  hittad: {port}")
            time.sleep(1.0)       # let the tty settle
            return port
        time.sleep(0.5)
        print(".", end="", flush=True)
    print()
    sys.exit("displayen dök aldrig upp. Var BOOT nedtryckt i det ögonblick kabeln sattes i?")

def flash(cli, variant, out_dir, port):
    cmd = [cli, "upload", "--fqbn", VARIANTS[variant]["fqbn"], "--input-dir", out_dir, "-p", port, SKETCH]
    print(f"Flashar från {os.path.relpath(out_dir, ROOT)}/ via {port} ...")
    p = subprocess.run(cmd, capture_output=True, text=True)
    log = p.stdout + p.stderr
    with open(os.path.join(out_dir, "upload.log"), "w") as f:
        f.write(log)
    verified = log.count("Hash of data verified")
    if p.returncode != 0 or verified == 0:
        tail = [l for l in log.splitlines() if l.strip()][-12:]
        print("\n".join(tail), file=sys.stderr)
        sys.exit(f"flashningen misslyckades (loggen: {out_dir}/upload.log)")
    print(f"  skrivet och verifierat ({verified} segment), displayen startar om")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--variant", choices=list(VARIANTS), help="hoppa över menyn")
    ap.add_argument("--knob-debug", action="store_true", help="bygg in encoder-diagnostik (-DKNOB_DEBUG), bara usbc/demo")
    ap.add_argument("--ref", help="git-ref att bygga UART-varianten från (standard: main)")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--flash-only", action="store_true", help="flasha senaste bygget av varianten utan att bygga om")
    ap.add_argument("--port", help="seriell port, annars letas /dev/serial/by-id/*Espressif*")
    ap.add_argument("--timeout", type=int, default=120, help="sekunder att vänta på bootloadern")
    ap.add_argument("--arduino-cli", help="sökväg till arduino-cli")
    a = ap.parse_args()

    cli = find_arduino_cli(a.arduino_cli)
    core = check_core(cli)
    print(f"arduino-cli: {cli}\nESP32-kärna: {core}")

    variant = a.variant or choose_variant()
    out_dir = os.path.join(BUILD, variant)

    if a.flash_only:
        if not os.path.isfile(os.path.join(out_dir, "KnobRGBControl.ino.bin")):
            sys.exit(f"inget bygge för '{variant}' i {out_dir}; kör utan --flash-only")
    else:
        out_dir = build(cli, variant, a.knob_debug, a.ref)

    if a.build_only:
        return

    port = a.port or wait_for_bootloader(a.timeout)
    flash(cli, variant, out_dir, port)

    print("\nKlart.")
    if variant == "usbc":
        print("Flytta displayens USB-C-kabel till SPUD-kortet. Den ska säga hello inom ett par sekunder.")
    elif variant == "uart":
        print("Koppla displayen till UGN via FPC:n (5V + UART0). I det här läget syns displayen på datorn")
        print("när den kör, så nästa omflashning behöver inte BOOT.")
    else:
        print("Demobygget kör utan kort. Displayen syns inte på datorn förrän du flashar om med BOOT nedtryckt.")

if __name__ == "__main__":
    main()
