# ESP32-C3 + PlatformIO: команды, прошивка, монитор порта и конфиги

Этот файл — краткая шпаргалка по работе с проектом на [`ESP32-C3`](platformio.ini) через [`PlatformIO`](platformio.ini) и [`ESP-IDF`](platformio.ini:14).

Проект использует:
- плату [`esp32-c3-devkitm-1`](platformio.ini:13)
- платформу [`espressif32`](platformio.ini:12)
- фреймворк [`espidf`](platformio.ini:14)

---

## 1. Основные команды

### Список подключённых устройств
```bash
pio device list
```
Показывает, какие serial-устройства видны системе.

На macOS порт часто выглядит так:
- `/dev/cu.usbmodem2101`
- `/dev/cu.usbmodem...`
- `/dev/cu.SLAB_USBtoUART`

Эта команда нужна, чтобы понять, **на каком порту висит твоя ESP32**.

---

### Сборка проекта
```bash
pio run
```
Что делает:
- читает настройки из [`platformio.ini`](platformio.ini)
- компилирует код из [`src/main.c`](src/main.c)
- собирает весь проект
- показывает ошибки компиляции, если они есть

Это аналог шага “build”.

---

### Прошивка платы
```bash
pio run -t upload
```
или
```bash
pio run --target upload
```
Что делает:
- сначала собирает проект
- затем загружает прошивку в ESP32 по USB

Если порт не определяется автоматически, иногда нужно явно указать upload-порт в [`platformio.ini`](platformio.ini).

---

### Открыть Serial Monitor
```bash
pio device monitor
```
Что делает:
- открывает последовательный порт
- показывает логи из [`ESP_LOGI()`](src/main.c:370), [`ESP_LOGW()`](src/main.c:127), [`ESP_LOGE()`](src/main.c:100)
- позволяет видеть, что делает микроконтроллер после запуска

Выход из монитора:
```bash
Ctrl + C
```

---

### Monitor с явным портом и скоростью
```bash
pio device monitor --port /dev/cu.usbmodem2101 -b 115200
```
Что значат флаги:
- `--port` — какой serial-порт открыть
- `-b 115200` — скорость порта, baud rate

Скорость должна совпадать с [`monitor_speed = 115200`](platformio.ini:20).

---

### Monitor без автосброса платы
```bash
pio device monitor --rts 0 --dtr 0
```
или полный вариант:
```bash
pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```
Что значат флаги:
- `--rts 0` — не дёргать линию RTS
- `--dtr 0` — не дёргать линию DTR

Зачем это нужно:
- некоторые ESP32-C3 могут перезагружаться при открытии serial monitor
- отключение RTS/DTR помогает избежать лишних reset

У тебя это уже вынесено в конфиг:
- [`monitor_rts = 0`](platformio.ini:21)
- [`monitor_dtr = 0`](platformio.ini:22)

Поэтому обычно достаточно просто:
```bash
pio device monitor --port /dev/cu.usbmodem2101
```

---

### Прошить и сразу смотреть логи
```bash
pio run -t upload && pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```
Что делает:
1. собирает проект
2. прошивает ESP32
3. сразу открывает монитор

Это один из самых удобных сценариев работы.

---

### Очистить сборку
```bash
pio run -t clean
```
Что делает:
- удаляет временные файлы сборки
- полезно, если что-то странно сломалось

После этого обычно снова запускают `pio run`.

---

### Открыть menuconfig
```bash
pio run -t menuconfig
```
Что делает:
- открывает меню конфигурации [`ESP-IDF`](platformio.ini:14)
- позволяет менять системные настройки проекта

Там можно настраивать:
- уровни логов
- параметры FreeRTOS
- Wi‑Fi / Bluetooth
- параметры flash
- разные внутренние настройки ESP-IDF

Результат сохраняется в [`sdkconfig.esp32-c3-devkitm-1`](sdkconfig.esp32-c3-devkitm-1).

---

## 2. Что за что отвечает в конфиге

Основной конфиг проекта — [`platformio.ini`](platformio.ini).

Ключевые строки:

### Среда сборки
- [`[env:esp32-c3-devkitm-1]`](platformio.ini:11) — имя окружения
- [`platform = espressif32`](platformio.ini:12) — платформа Espressif
- [`board = esp32-c3-devkitm-1`](platformio.ini:13) — конкретная плата
- [`framework = espidf`](platformio.ini:14) — используется ESP-IDF, не Arduino

### Монитор порта
- [`monitor_speed = 115200`](platformio.ini:20) — скорость serial monitor
- [`monitor_rts = 0`](platformio.ini:21) — не переключать RTS
- [`monitor_dtr = 0`](platformio.ini:22) — не переключать DTR

### Порт монитора
- [`;monitor_port = /dev/cu.usbmodem2101`](platformio.ini:19) — сейчас закомментировано

В [`platformio.ini`](platformio.ini:19) символ `;` означает комментарий.

Если хочешь зафиксировать конкретный порт, можно раскомментировать:
```ini
monitor_port = /dev/cu.usbmodem2101
```

Тогда не придётся каждый раз писать `--port` в командной строке.

---

## 3. Как устроен обычный цикл работы

### Вариант 1: по шагам
```bash
pio device list
pio run
pio run -t upload
pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```

Логика такая:
1. найти порт
2. собрать проект
3. прошить плату
4. открыть логи

---

### Вариант 2: короткий рабочий цикл
Если порт уже известен:
```bash
pio run -t upload && pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
```

---

## 4. Как читать логи от ESP32

В коде используются логи из [`esp_log.h`](src/main.c:18).

Примеры из проекта:
- [`ESP_LOGI(TAG, "app_main start...")`](src/main.c:370)
- [`ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err))`](src/main.c:100)
- [`ESP_LOGW(TAG, "Erasing NVS partition due to init err: %s", esp_err_to_name(err))`](src/main.c:127)
- [`ESP_LOGD(TAG, "uptime minutes=%u", (unsigned)s_persist_counter)`](src/main.c:104)

Типы логов:
- `ESP_LOGI` — информация
- `ESP_LOGW` — предупреждение
- `ESP_LOGE` — ошибка
- `ESP_LOGD` — отладка

Тег логов в [`main.c`](src/main.c:42):
```c
static const char *TAG = "BLINK";
```

Это имя модуля, которое видно в serial monitor.

Пример строки лога:
```text
I (1234) BLINK: app_main start, configuring GPIO...
```

Где:
- `I` — уровень INFO
- `1234` — время с запуска в миллисекундах
- `BLINK` — тег
- дальше идёт текст сообщения

---

## 5. Как читать ошибки

В проекте ошибки часто выводятся через [`esp_err_to_name()`](src/main.c:100).

Пример:
```c
ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
```

Что происходит:
1. функция возвращает код ошибки в переменной `err`
2. [`esp_err_to_name()`](src/main.c:100) превращает код в читаемое имя
3. [`ESP_LOGE()`](src/main.c:100) печатает это в монитор

То есть вместо непонятного числа ты видишь что-то вроде:
```text
E (5555) BLINK: NVS save failed: ESP_ERR_NVS_NO_FREE_PAGES
```

Это очень удобно для отладки.

---

## 6. Что такое прошивка простыми словами

Когда ты запускаешь:
```bash
pio run -t upload
```
PlatformIO:
1. компилирует C-код
2. собирает бинарный файл прошивки
3. отправляет этот файл в flash-память ESP32
4. после этого плата начинает выполнять новый код

То есть **прошивка** — это просто запись новой программы в память микроконтроллера.

---

## 7. Какие файлы важны в этом проекте

- [`platformio.ini`](platformio.ini) — главный конфиг PlatformIO
- [`sdkconfig.esp32-c3-devkitm-1`](sdkconfig.esp32-c3-devkitm-1) — конфиг ESP-IDF, который меняется через `menuconfig`
- [`src/main.c`](src/main.c) — основная логика программы
- [`src/oled_042.c`](lib/oled42/oled_042.c) — работа с OLED
- [`src/oled_042.h`](lib/oled42/oled_042.h) — заголовок для OLED-модуля

---

## 8. Минимальный набор команд, который стоит запомнить

```bash
pio device list
pio run
pio run -t upload
pio device monitor --port /dev/cu.usbmodem2101 -b 115200 --rts 0 --dtr 0
pio run -t menuconfig
```

Если коротко:
- `pio device list` — найти порт
- `pio run` — собрать
- `pio run -t upload` — прошить
- `pio device monitor ...` — смотреть логи
- `pio run -t menuconfig` — открыть системные настройки ESP-IDF

---

