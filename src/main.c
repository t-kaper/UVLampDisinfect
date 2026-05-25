// GPIO + FreeRTOS + логирование + таймеры + контроль памяти + NVS ("EEPROM")
//
// Этот пример для ESP32‑C3 Super Mini переносит LED на GPIO10 и
// освобождает GPIO8/9 под I²C. По I²C подключён монохромный OLED на SSD1306.
// Показано:
//  - инициализация I²C master драйвера ESP‑IDF
//  - отправка команд/данных SSD1306 и базовая инициализация дисплея
//  - кадровый буфер 1 bpp с примитивным рендерингом текста 5×7
//  - NVS («EEPROM») для хранения счётчика минут работы
//  - FreeRTOS‑задача, которая раз в минуту мигает светодиодом, увеличивает
//    счётчик и перерисовывает OLED
#include "driver/gpio.h"
#include "driver/i2c.h"           // I2C master (ESP-IDF)
#include "../lib/oled42/oled_042.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"     // FreeRTOS software timers
#include "freertos/semphr.h"     // mutex для общего счётчика и экрана
#include "esp_log.h"
#include "esp_heap_caps.h"       // heap_caps_get_free_size()
#include "nvs_flash.h"            // NVS init/erase
#include "nvs.h"                  // NVS CRUD API
#include "esp_err.h"
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#define LED_PIN 10                 // перенесли LED на GPIO10
#define BUTTON_PIN 2               // кнопка сброса счётчика на GPIO2

// === I2C и SSD1306 параметры ===
#define I2C_PORT   I2C_NUM_0
#define I2C_SDA    8              // SDA — GPIO8
#define I2C_SCL    9              // SCL — GPIO9
#define I2C_FREQ   400000         // 400 кГц

#define SSD1306_ADDR 0x3C         // типовой адрес OLED
// 0.42" OLED обычно 72x40 (механика ~70x40). Эти панели используют часть
// 128‑колоночной памяти SSD1306 со сдвигом по колонкам.
#define OLED_WIDTH  72
#define OLED_HEIGHT 40
#define OLED_PAGES  (OLED_HEIGHT/8)
#define OLED_COL_OFFSET 28        // активная область начинается с ~28 колонки
#define LAMP_LIFETIME_HOURS 8000  // после этого порога на экране выводим предупреждение о замене лампы

static const char *TAG = "BLINK";

// Этот флаг говорит, можно ли реально отправлять картинку на OLED.
// Если инициализация дисплея не удалась, остальная логика всё равно продолжит работать.
static bool s_oled_ready = false;

// Мьютекс нужен, чтобы две FreeRTOS-задачи не меняли счётчик одновременно.
// Заодно через этот же мьютекс защищаем момент, когда читаем значение для экрана и логов.
static SemaphoreHandle_t s_state_mutex = NULL;

// === NVS globals and prototypes need to be visible before use in callbacks ===
static nvs_handle_t s_nvs = 0;         // handle к пространству имён NVS
static uint32_t s_persist_counter = 0; // значение счётчика в ОЗУ (отражение NVS)
static esp_err_t nvs_save_counter(uint32_t value);
static void oled_draw_runtime_screen(void);
static void button_task(void *pv);
static esp_err_t nvs_init_and_load(void);


static esp_err_t i2c_master_init(void);
static void      i2c_scan_log(void);

// Простой сканер I2C-шины для отладки: печатает все ACK-адреса (0..127)
static void i2c_scan_log(void)
{
    ESP_LOGI(TAG, "I2C scan start");
    for (uint8_t addr = 1; addr < 127; ++addr) {
        i2c_cmd_handle_t h = i2c_cmd_link_create();
        i2c_master_start(h);
        i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(h);
        esp_err_t err = i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C found device at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan done");
}

// Минуточная задача: раз в 60 секунд переключает LED, увеличивает и
// сохраняет счётчик минут в NVS, а также перерисовывает OLED (HELLO!/MIN:N)
static void minute_task(void *pv)
{
    ESP_LOGI(TAG, "minute_task started (period=60s)");
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(60000)); // период 60 секунд


        // Под мьютексом одновременно меняем общий счётчик и берём значение,
        // которое потом будем сохранять в NVS. Так `button_task` не сможет
        // вклиниться ровно посередине операции.
        // portMAX_DELAY означает "ждать бесконечно, пока не освободится"
        uint32_t counter_to_save = 0;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_persist_counter++;
        counter_to_save = s_persist_counter;
        xSemaphoreGive(s_state_mutex);

        // Инкремент и сохранение в NVS (эмуляция EEPROM через SPI Flash с wear-leveling)
        esp_err_t err = nvs_save_counter(counter_to_save);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        }

        // DEBUG: сколько минут проработал микроконтроллер
        ESP_LOGD(TAG, "uptime minutes=%" PRIu32, counter_to_save);

        // Обновим OLED: часы и остаток минут, либо предупреждение о замене лампы.
        oled_draw_runtime_screen();
    }
}



static esp_err_t nvs_init_and_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open("storage", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) return err;

    err = nvs_get_u32(s_nvs, "ctr", &s_persist_counter);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_persist_counter = 0;
        return ESP_OK;
    }
    return err;
}

// Сохранение счётчика в NVS с коммитом.
// Обновляет ключ "ctr" и вызывает nvs_commit(), чтобы записать
// изменения во flash (wear‑leveling внутри драйвера NVS).
static esp_err_t nvs_save_counter(uint32_t value)
{
    if (s_nvs == 0) return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_set_u32(s_nvs, "ctr", value);
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs);
}

static void oled_draw_runtime_screen(void)
{
    uint32_t counter_snapshot = 0;
    uint32_t hours = 0;
    uint32_t minutes = 0;

    // Берём снимок счётчика под мьютексом, чтобы лог и OLED показывали одно и то же значение.
    if (s_state_mutex != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        counter_snapshot = s_persist_counter;
        xSemaphoreGive(s_state_mutex);
    } else {
        counter_snapshot = s_persist_counter;
    }

    hours = counter_snapshot / 60;
    minutes = counter_snapshot % 60;

    // Даже если экран недоступен, в логах всё равно видно тот же текст,
    // который мы бы показали пользователю на OLED.
    if (hours > LAMP_LIFETIME_HOURS) {
        if (s_oled_ready) {
            oled042_clear();
            oled042_draw_text(0, 0, "WORK_TIME");
            oled042_draw_text(12, 16, "CHANGE");
            oled042_draw_text(18, 32, "LAMP");
            oled042_update();
            ESP_LOGI(TAG, "screen: WORK_TIME / CHANGE / LAMP");
        } else {
            ESP_LOGW(TAG, "screen not updated: WORK_TIME / CHANGE / LAMP");
        }
    } else {
        if (s_oled_ready) {
            oled042_clear();
            oled042_draw_text(0, 0, "WORK_TIME");
            oled042_draw_text(0, 8, "HOUR:");
            oled042_draw_uint(0, 16, hours);
            oled042_draw_text(0, 24, "MINUTES:");
            oled042_draw_uint(0, 32, minutes);
            oled042_update();
            ESP_LOGI(TAG, "screen: WORK_TIME / HOUR=%" PRIu32 " / MINUTES=%" PRIu32, hours, minutes);
        } else {
            ESP_LOGW(TAG, "screen not updated: WORK_TIME / HOUR=%" PRIu32 " / MINUTES=%" PRIu32, hours, minutes);
        }
    }
}

static void button_task(void *pv)
{
    int last_level = 1;

    ESP_LOGI(TAG, "button_task started (GPIO=%d)", BUTTON_PIN);

    while (1) {  // Бесконечный цикл опроса кнопки

        // 1. Считываем текущее состояние кнопки (0 - нажата, 1 - отпущена)
        int level = gpio_get_level(BUTTON_PIN);

        // 2. Детектируем нажатие:
        //    last_level == 1 (кнопка была отпущена)
        //    level == 0     (кнопка нажата сейчас)
        if (last_level == 1 && level == 0) {

            // 3. Обнуляем счетчик в оперативной памяти.
            // Делаем это под мьютексом, чтобы minute_task не увеличил значение в тот же момент.
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_persist_counter = 0;
            xSemaphoreGive(s_state_mutex);

            // 4. Сохраняем ноль в энергонезависимую память (NVS)
            esp_err_t err = nvs_save_counter(0);

            // 5. Проверка ошибки сохранения
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "counter reset save failed: %s", esp_err_to_name(err));
            }

            // 6. Обновляем дисплей (показываем обнуленный счетчик)
            oled_draw_runtime_screen();

            // 7. Логируем событие сброса
            ESP_LOGI(TAG, "counter reset by button");

            // 8. Антидребезг: задержка 300 мс, чтобы игнорировать ложные срабатывания
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        // 9. Сохраняем текущее состояние кнопки для следующей итерации
        last_level = level;

        // 10. Задержка 20 мс перед следующим опросом (периодичность ~50 Гц)
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ========================= I2C + SSD1306 =========================

// Инициализация I²C master на выбранных GPIO (SDA=GPIO8, SCL=GPIO9).
// Включены внутренние подтяжки; при наличии внешних 4.7–10 кОм можно
// оставить как есть — они просто усилят линию. Частота по умолчанию 400 кГц.
static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ,
        .clk_flags = 0
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    return i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
}


// Точка входа приложения:
//  1) уменьшаем глобальный уровень логов, для нашего тега включаем DEBUG
//  2) настраиваем GPIO LED (GPIO10)
//  3) инициализируем/читаем NVS
//  4) запускаем I²C и инициализируем SSD1306; рисуем стартовый экран
//  5) создаём минутную задачу
//  6) выводим статистику по куче
void app_main(void)
{
    // Глобально уменьшим болтливость логов ради экономии памяти на форматировании
    esp_log_level_set("*", ESP_LOG_WARN);
    // Для нашего тега можно временно поднять уровень для отладки
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "app_main start, configuring GPIO (LED_PIN=%d, BUTTON_PIN=%d, I2C SDA=%d SCL=%d)...", LED_PIN, BUTTON_PIN, I2C_SDA, I2C_SCL);

    // Создаём мьютекс в самом начале, чтобы обе задачи потом работали с одним и тем же объектом.
    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "state mutex create failed");
        return;
    }

    // Сбрасываем предыдущую конфигурацию GPIO2, чтобы начать настройку кнопки с чистого состояния.
    gpio_reset_pin(BUTTON_PIN);
    // Переводим пин в режим входа: будем читать его уровень, а не управлять им.
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    // Включаем внутреннюю подтяжку к питанию. В покое на входе будет логическая 1,
    // а при нажатии кнопка обычно замыкает пин на GND и мы увидим логический 0.
    gpio_pullup_en(BUTTON_PIN);
    // Отключаем подтяжку вниз, чтобы она не конфликтовала с pull-up.
    gpio_pulldown_dis(BUTTON_PIN);

    // Инициализируем NVS и загружаем стартовое значение счётчика
    esp_err_t nvs_err = nvs_init_and_load();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init/load failed: %s", esp_err_to_name(nvs_err));
    } else {
        ESP_LOGI(TAG, "NVS loaded: ctr=%u", (unsigned)s_persist_counter);
    }


    // Инициализация GPIO (LED на GPIO10) --------------------------------**
    // gpio_reset_pin(LED_PIN);
    // gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    // // Стартуем с выключенного состояния, чтобы после перезагрузки поведение было предсказуемым.
    // gpio_set_level(LED_PIN, 0);
    //

    // --------------------------------**


    // I2C + OLED стартовый экран
    ESP_ERROR_CHECK(i2c_master_init());
    i2c_scan_log(); // выведем адреса устройств на шине в лог (диагностика)

    // ====== ВАРИАНТ ДЛЯ 0.91" OLED (128x32) — АКТИВЕН ======
    // Настройка под 0.91" (обычно SSD1306, адрес 0x3C):
    // const oled042_cfg_t ocfg_091 = {
    //     .port = I2C_PORT,
    //     .sda_io = I2C_SDA,
    //     .scl_io = I2C_SCL,
    //     .clk_hz = I2C_FREQ,
    //     .i2c_addr = SSD1306_ADDR,
    //     .width = 128,
    //     .height = 32,
    //     .col_offset = 0,    // для 128x32 смещения нет
    //     .row_offset = 0,
    //     .use_sh1106_pump = false
    // };
    // ESP_ERROR_CHECK(oled042_init(&ocfg_091, false));
    // oled042_clear();
    // oled042_draw_text(0, 0, "HELLO 0.91\"");
    // oled042_draw_text(0, 16, "MIN:");
    // oled042_draw_uint(28, 16, s_persist_counter);
    // oled042_update();

    // ====== ВАРИАНТ ДЛЯ 0.42" OLED (72x40) — ОТКЛЮЧЁН ======
    // Если понадобится вернуться к маленькому экрану — раскомментируйте ниже,
    // а блок для 0.91" выше закомментируйте.
    const oled042_cfg_t ocfg_042 = {
        .port = I2C_PORT,
        .sda_io = I2C_SDA,
        .scl_io = I2C_SCL,
        .clk_hz = I2C_FREQ,
        .i2c_addr = SSD1306_ADDR,
        .width = 72,
        .height = 40,
        .col_offset = 28,
        .row_offset = 0,
        .use_sh1106_pump = false
    };
    // Если OLED не ответил или не инициализировался, прошивка всё равно продолжает работать.
    // Просто в логах будет видно, что данные подготовлены, но на экран они не ушли.
    esp_err_t oled_err = oled042_init(&ocfg_042, false);
    if (oled_err != ESP_OK) {
        s_oled_ready = false;
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(oled_err));
    } else {
        s_oled_ready = true;
        ESP_LOGI(TAG, "OLED init OK");
    }
    oled_draw_runtime_screen();


    // Покажем доступную кучу до/после создания задачи (экономия и диагностика)
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap free before task: %u bytes", (unsigned)free_before);

    BaseType_t ok = xTaskCreate(
            minute_task,
            "minute_task",
            2048,                 // стек (слова); можно уменьшить после замера HighWaterMark
            NULL,
            tskIDLE_PRIORITY+1,
            NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "minute_task create failed (%ld)", (long)ok);
        return;
    }

    ok = xTaskCreate(
            button_task,
            "button_task",
            2048,
            NULL,
            tskIDLE_PRIORITY+1,
            NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "button_task create failed (%ld)", (long)ok);
        return;
    }

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap free after task: %u bytes (delta %d)", (unsigned)free_after, (int)free_after - (int)free_before);
}
