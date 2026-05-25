# ESP32-C3 + PlatformIO: Commands, Flashing, Serial Monitor, and Configuration

This file is a compact reference for working with this [`ESP32-C3`](../platformio.ini) project using [`PlatformIO`](../platformio.ini) and [`ESP-IDF`](../platformio.ini:14).

This project uses:
- board: [`esp32-c3-devkitm-1`](../platformio.ini:13)
- platform: [`espressif32`](../platformio.ini:12)
- framework: [`espidf`](../platformio.ini:14)

---

## 1. Basic Commands

### List Connected Devices
```bash
pio device list
```

Shows which serial devices are currently visible to the system.

On macOS, the port often looks like one of these:
- `/dev/cu.usbmodem2101`
- `/dev/cu.usbmodem...`
- `/dev/cu.SLAB_USBtoUART`

Use this command to find **which serial port your ESP32 is connected to**.

---

### Build the Project
```bash
pio run
```

What it does:
- reads settings from [`platformio.ini`](../platformio.ini)
- compiles code from [`src/main.c`](../src/main.c)
- builds the full project
- shows compilation errors if any exist

This is the equivalent of a standard build step.

---

### Flash the Board
```bash
pio run -t upload
```

or

```bash
pio run --target upload
```

What it does:
- builds the project first
- then uploads the firmware to the ESP32 over USB

If the port is not detected automatically, you may need to set the upload port explicitly in [`platformio.ini`](../platformio.ini).

---

### Open the Serial Monitor
```bash
pio device monitor
```

What it does:
- opens the serial port
- shows logs from [`ESP_LOGI()`](../src/main.c:370), [`ESP_LOGW()`](../src/main.c:127), and [`ESP_LOGE()`](../src/main.c:100)
- lets you see what the microcontroller is doing after boot

To exit the monitor:

```bash
Ctrl + C
```

---

### Open the Monitor with an Explicit Port and Baud Rate
```bash
pio device monitor --port /dev/cu.usbmodem2101 -b 115200
```

Flag meanings:
- `--port` — selects which serial port to open
- `-b 115200` — sets the baud rate

The baud rate should match [`monitor_speed = 115200`](../platformio.ini:20).

---

### Open the Monitor Without Auto-Reset
```bash
pio device monitor --rts 0 --dtr 0
```

or the full version:

```bash
pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```

Flag meanings:
- `--rts 0` — do not toggle the RTS line
- `--dtr 0` — do not toggle the DTR line

Why this helps:
- some ESP32-C3 boards reset when the serial monitor opens
- disabling RTS/DTR helps avoid unnecessary resets

This is already configured in the project:
- [`monitor_rts = 0`](../platformio.ini:21)
- [`monitor_dtr = 0`](../platformio.ini:22)

So in many cases, this is enough:

```bash
pio device monitor --port /dev/cu.usbmodem2101
```

---

### Flash and Immediately Start Monitoring Logs
```bash
pio run -t upload && pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```

What it does:
1. builds the project
2. flashes the ESP32
3. immediately opens the serial monitor

This is one of the most convenient workflows for development.

---

### Clean the Build
```bash
pio run -t clean
```

What it does:
- removes temporary build files
- is useful if something behaves strangely during compilation

After that, you usually run `pio run` again.

---

### Open `menuconfig`
```bash
pio run -t menuconfig
```

What it does:
- opens the [`ESP-IDF`](../platformio.ini:14) configuration menu
- lets you change system-level project settings

Typical configuration areas include:
- log levels
- FreeRTOS settings
- Wi‑Fi / Bluetooth
- flash parameters
- other internal ESP-IDF settings

The result is stored in [`sdkconfig.esp32-c3-devkitm-1`](../sdkconfig.esp32-c3-devkitm-1).

---

## 2. What the Configuration File Controls

The main project configuration file is [`platformio.ini`](../platformio.ini).

### Build Environment
- [`[env:esp32-c3-devkitm-1]`](../platformio.ini:11) — environment name
- [`platform = espressif32`](../platformio.ini:12) — Espressif platform
- [`board = esp32-c3-devkitm-1`](../platformio.ini:13) — target board
- [`framework = espidf`](../platformio.ini:14) — uses ESP-IDF, not Arduino

### Serial Monitor Settings
- [`monitor_speed = 115200`](../platformio.ini:20) — serial monitor speed
- [`monitor_rts = 0`](../platformio.ini:21) — do not toggle RTS
- [`monitor_dtr = 0`](../platformio.ini:22) — do not toggle DTR

### Monitor Port
- [`;monitor_port = /dev/cu.usbmodem2101`](../platformio.ini:19) — currently commented out

In [`platformio.ini`](../platformio.ini:19), the `;` character means the line is commented.

If you want to lock the monitor to a specific port, uncomment it:

```ini
monitor_port = /dev/cu.usbmodem2101
```

Then you will not need to pass `--port` every time in the terminal.

---

## 3. Typical Development Workflow

### Option 1: Step by Step
```bash
pio device list
pio run
pio run -t upload
pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```

The logic is:
1. find the port
2. build the project
3. flash the board
4. open the logs

---

### Option 2: Short Development Cycle
If the port is already known:

```bash
pio run -t upload && pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```

---

## 4. How to Read ESP32 Logs

The code uses logging from [`esp_log.h`](../src/main.c:18).

Examples from this project:
- [`ESP_LOGI(TAG, "app_main start...")`](../src/main.c:370)
- [`ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err))`](../src/main.c:100)
- [`ESP_LOGW(TAG, "Erasing NVS partition due to init err: %s", esp_err_to_name(err))`](../src/main.c:127)
- [`ESP_LOGD(TAG, "uptime minutes=%u", (unsigned)s_persist_counter)`](../src/main.c:104)

Log types:
- `ESP_LOGI` — information
- `ESP_LOGW` — warning
- `ESP_LOGE` — error
- `ESP_LOGD` — debug

The log tag in [`main.c`](../src/main.c:42):

```c
static const char *TAG = "BLINK";
```

This is the module name shown in the serial monitor.

Example log line:

```text
I (1234) BLINK: app_main start, configuring GPIO...
```

Where:
- `I` — INFO level
- `1234` — milliseconds since boot
- `BLINK` — log tag
- the rest is the message text

---

## 5. How to Read Errors

In this project, errors are often printed using [`esp_err_to_name()`](../src/main.c:100).

Example:

```c
ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
```

What happens:
1. a function returns an error code in the `err` variable
2. [`esp_err_to_name()`](../src/main.c:100) converts the code into a readable name
3. [`ESP_LOGE()`](../src/main.c:100) prints it to the monitor

So instead of an unclear number, you see something like:

```text
E (5555) BLINK: NVS save failed: ESP_ERR_NVS_NO_FREE_PAGES
```

That makes debugging much easier.

---

## 6. What “Flashing” Means in Simple Terms

When you run:

```bash
pio run -t upload
```

PlatformIO:
1. compiles the C code
2. builds the firmware binary
3. writes that binary into ESP32 flash memory
4. after that, the board starts running the new code

So **flashing firmware** simply means writing a new program into the microcontroller’s memory.

---

## 7. Important Files in This Project

- [`platformio.ini`](../platformio.ini) — main PlatformIO configuration
- [`sdkconfig.esp32-c3-devkitm-1`](../sdkconfig.esp32-c3-devkitm-1) — ESP-IDF configuration updated through `menuconfig`
- [`src/main.c`](../src/main.c) — main application logic
- [`lib/oled42/oled_042.c`](../lib/oled42/oled_042.c) — OLED driver implementation
- [`lib/oled42/oled_042.h`](../lib/oled42/oled_042.h) — OLED driver header

---

## 8. Minimal Command Set to Remember

```bash
pio device list
pio run
pio run -t upload
pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
pio run -t menuconfig
```

In short:
- `pio device list` — find the port
- `pio run` — build
- `pio run -t upload` — flash
- `pio device monitor ...` — view logs
- `pio run -t menuconfig` — open ESP-IDF system settings

---

