// Минимальный драйвер OLED-дисплея 0.42" 72x40 и совместимых модулей с постраничной адресацией.
// Основные решения в реализации:
// - используется режим Page Addressing Mode (0x20, 0x02), совместимый со многими 0.42" модулями;
// - применяется смещение столбцов (col_offset), так как видимая область обычно является окном внутри памяти на 128 столбцов;
// - хранится компактный VRAM-буфер и данные отправляются постранично, чтобы экономить RAM;
// - код устойчив к уже установленному драйверу I2C и не завершает работу из-за повторной инициализации.

#include "oled_042.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

// Внутренний VRAM/буфер кадра в постраничной организации: width * (height / 8) байт.
static uint8_t *s_fb = NULL;
static oled042_cfg_t s_cfg;
static const char *TAG_OLED = "OLED042";

static inline int pages(void) { return s_cfg.height / 8; }

// Низкоуровневые I2C-хелперы ------------------------------------------------
static esp_err_t i2c_write_cmd(uint8_t cmd)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (s_cfg.i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x00, true); // Управляющий байт: далее идёт команда.
    i2c_master_write_byte(h, cmd, true);
    i2c_master_stop(h);
    esp_err_t err = i2c_master_cmd_begin(s_cfg.port, h, pdMS_TO_TICKS(300));
    i2c_cmd_link_delete(h);
    return err;
}

static esp_err_t i2c_write_data(const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (s_cfg.i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x40, true); // Управляющий байт: далее идёт поток данных дисплея.
    i2c_master_write(h, (uint8_t*)data, len, true);
    i2c_master_stop(h);
    esp_err_t err = i2c_master_cmd_begin(s_cfg.port, h, pdMS_TO_TICKS(600));
    i2c_cmd_link_delete(h);
    return err;
}

static void set_page_col(uint8_t page, uint8_t col)
{
    // Выбираем страницу памяти и начальный столбец с учётом аппаратного смещения окна.
    i2c_write_cmd(0xB0 | (page & 0x07));
    uint8_t x = (uint8_t)(s_cfg.col_offset + col);
    i2c_write_cmd(0x00 | (x & 0x0F));
    i2c_write_cmd(0x10 | (x >> 4));
}

// Публичный API --------------------------------------------------------------
esp_err_t oled042_init(const oled042_cfg_t *cfg, bool scan_i2c)
{
    s_cfg = *cfg;

    // Настройка мастера I2C. Повторный вызов с теми же параметрами допустим.
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = s_cfg.sda_io,
        .scl_io_num = s_cfg.scl_io,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = s_cfg.clk_hz,
        .clk_flags = 0
    };
    // Конфигурируем порт I2C; при одинаковых параметрах операция по сути идемпотентна.
    ESP_ERROR_CHECK(i2c_param_config(s_cfg.port, &conf));
    // Пытаемся установить драйвер I2C. Если он уже установлен
    // (в старых версиях IDF это может вернуть ESP_FAIL вместо ESP_ERR_INVALID_STATE),
    // просто используем существующий экземпляр.
    esp_err_t e = i2c_driver_install(s_cfg.port, conf.mode, 0, 0, 0);
    if (e == ESP_OK) {
        ESP_LOGI(TAG_OLED, "I2C driver installed (port=%d)", (int)s_cfg.port);
    } else if (e == ESP_ERR_INVALID_STATE || e == ESP_FAIL) {
        ESP_LOGW(TAG_OLED, "I2C driver already installed, reusing (err=%d)", (int)e);
    } else {
        ESP_ERROR_CHECK(e);
    }

    if (scan_i2c) {
        // Необязательное сканирование шины полезно для быстрой проверки адреса дисплея и других устройств.
        ESP_LOGI(TAG_OLED, "I2C scan start");
        for (uint8_t a = 1; a < 127; ++a) {
            i2c_cmd_handle_t h = i2c_cmd_link_create();
            i2c_master_start(h);
            i2c_master_write_byte(h, (a << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(h);
            if (i2c_master_cmd_begin(s_cfg.port, h, pdMS_TO_TICKS(20)) == ESP_OK)
                ESP_LOGI(TAG_OLED, "found 0x%02X", a);
            i2c_cmd_link_delete(h);
        }
        ESP_LOGI(TAG_OLED, "I2C scan done");
    }

    // Выделяем локальный VRAM-буфер под весь кадр.
    size_t fb_size = (size_t)s_cfg.width * (size_t)pages();
    s_fb = (uint8_t*)heap_caps_malloc(fb_size, MALLOC_CAP_8BIT);
    if (!s_fb) return ESP_ERR_NO_MEM;
    memset(s_fb, 0, fb_size);

    // Последовательность инициализации дисплея.
    // Ориентирована на Page Addressing Mode и типичную высоту 40 строк.
    const uint8_t seq[] = {
        0xAE,               // Выключить дисплей на время настройки.
        0xD5, 0x80,         // Настройка тактирования контроллера.
        0xA8, (uint8_t)(s_cfg.height - 1), // Мультиплекс: число строк минус 1.
        0xD3, (uint8_t)s_cfg.row_offset,   // Вертикальное смещение области отображения.
        0x40,               // Начальная линия дисплея = 0.
        0x8D, 0x14,         // Включение charge pump для SSD1306; для SH1106 используется 0xAD, 0x8B.
        0x20, 0x02,         // Режим постраничной адресации (Page Addressing Mode).
        0xA1,               // Переназначение сегментов: зеркалирование по X.
        0xC8,               // Направление COM-сканирования; можно перевернуть все 0xC8 но тогда буквы надо тоже перевернуть
        0xDA, 0x12,         // Конфигурация COM-линий, при необходимости можно подбирать под модуль.
        0x81, 0x7F,         // Контраст.
        0xD9, 0xF1,         // Предзаряд.
        0xDB, 0x40,         // Уровень VCOM detect.
        0xA4,               // Продолжить вывод содержимого RAM.
        0xA6,               // Нормальный режим отображения без инверсии.
        0xAF                // Включить дисплей.
    };
    for (size_t i = 0; i < sizeof(seq); ++i) i2c_write_cmd(seq[i]);

    // Для SH1106 при необходимости дополнительно включаем встроенный DC-DC преобразователь.
    if (s_cfg.use_sh1106_pump) { i2c_write_cmd(0xAD); i2c_write_cmd(0x8B); }

    return ESP_OK;
}

void oled042_clear(void)
{
    if (!s_fb) return;
    // Полностью очищаем локальный буфер кадра; фактический вывод произойдёт после oled042_update().
    memset(s_fb, 0x00, (size_t)s_cfg.width * (size_t)pages());
}

esp_err_t oled042_update(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    // Передаём буфер на дисплей страница за страницей, чтобы соответствовать формату памяти контроллера.
    for (int p = 0; p < pages(); ++p) {
        set_page_col((uint8_t)p, 0);
        esp_err_t e = i2c_write_data(&s_fb[p * s_cfg.width], s_cfg.width);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

// --- Шрифт 5x7 (ограниченное подмножество символов) ------------------------
static const uint8_t GLYPH_SPC[5] = {0,0,0,0,0};
static const uint8_t GLYPH_H[5]   = {0x7F,0x08,0x08,0x7F,0x00};
static const uint8_t GLYPH_E[5]   = {0x7F,0x49,0x49,0x49,0x41};
static const uint8_t GLYPH_L[5]   = {0x7F,0x40,0x40,0x40,0x40};
static const uint8_t GLYPH_O[5]   = {0x3E,0x41,0x41,0x41,0x3E};
static const uint8_t GLYPH_EX[5]  = {0x00,0x00,0x5F,0x00,0x00};
static const uint8_t GLYPH_M[5]   = {0x7F,0x02,0x04,0x02,0x7F};
static const uint8_t GLYPH_I[5]   = {0x00,0x41,0x7F,0x41,0x00};
static const uint8_t GLYPH_N[5]   = {0x7F,0x04,0x08,0x10,0x7F};
static const uint8_t GLYPH_A[5]   = {0x7E,0x11,0x11,0x11,0x7E};
static const uint8_t GLYPH_C[5]   = {0x3E,0x41,0x41,0x41,0x22};
static const uint8_t GLYPH_G[5]   = {0x3E,0x41,0x49,0x49,0x7A};
static const uint8_t GLYPH_P[5]   = {0x7F,0x09,0x09,0x09,0x06};
static const uint8_t GLYPH_R[5]   = {0x7F,0x09,0x19,0x29,0x46};
static const uint8_t GLYPH_S[5]   = {0x46,0x49,0x49,0x49,0x31};
static const uint8_t GLYPH_T[5]   = {0x01,0x01,0x7F,0x01,0x01};
static const uint8_t GLYPH_U[5]   = {0x3F,0x40,0x40,0x40,0x3F};
static const uint8_t GLYPH_W[5]   = {0x7F,0x20,0x18,0x20,0x7F};
static const uint8_t GLYPH_K[5]   = {0x7F,0x08,0x14,0x22,0x41};
static const uint8_t GLYPH_UND[5] = {0x40,0x40,0x40,0x40,0x40};
static const uint8_t GLYPH_COL[5] = {0x00,0x36,0x36,0x00,0x00};
static const uint8_t GLYPH_0[5]   = {0x3E,0x51,0x49,0x45,0x3E};
static const uint8_t GLYPH_1[5]   = {0x00,0x42,0x7F,0x40,0x00};
static const uint8_t GLYPH_2[5]   = {0x42,0x61,0x51,0x49,0x46};
static const uint8_t GLYPH_3[5]   = {0x21,0x41,0x45,0x4B,0x31};
static const uint8_t GLYPH_4[5]   = {0x18,0x14,0x12,0x7F,0x10};
static const uint8_t GLYPH_5[5]   = {0x27,0x45,0x45,0x45,0x39};
static const uint8_t GLYPH_6[5]   = {0x3C,0x4A,0x49,0x49,0x30};
static const uint8_t GLYPH_7[5]   = {0x01,0x71,0x09,0x05,0x03};
static const uint8_t GLYPH_8[5]   = {0x36,0x49,0x49,0x49,0x36};
static const uint8_t GLYPH_9[5]   = {0x06,0x49,0x49,0x29,0x1E};

static const uint8_t* glyph5x7(char c)
{
    // Возвращаем указатель на 5-байтный глиф; неподдерживаемые символы заменяются пробелом.
    switch (c) {
        case ' ': return GLYPH_SPC; case 'A': return GLYPH_A;   case 'C': return GLYPH_C;
        case 'E': return GLYPH_E;   case 'G': return GLYPH_G;   case 'H': return GLYPH_H;
        case 'I': return GLYPH_I;   case 'L': return GLYPH_L;   case 'M': return GLYPH_M;
        case 'N': return GLYPH_N;   case 'O': return GLYPH_O;   case 'P': return GLYPH_P;
        case 'R': return GLYPH_R;   case 'S': return GLYPH_S;   case 'T': return GLYPH_T;
        case 'U': return GLYPH_U;   case 'W': return GLYPH_W;   case 'K': return GLYPH_K;
        case '_': return GLYPH_UND; case '!': return GLYPH_EX;  case ':': return GLYPH_COL;
        case '0': return GLYPH_0;   case '1': return GLYPH_1;   case '2': return GLYPH_2;
        case '3': return GLYPH_3;   case '4': return GLYPH_4;   case '5': return GLYPH_5;
        case '6': return GLYPH_6;   case '7': return GLYPH_7;   case '8': return GLYPH_8;
        case '9': return GLYPH_9;   default: return GLYPH_SPC;
    }
}

static inline void fb_set_px(int x, int y)
{
    if ((unsigned)x >= (unsigned)s_cfg.width || (unsigned)y >= (unsigned)s_cfg.height) return;
    // Каждый байт буфера хранит вертикальную колонку из 8 пикселей внутри одной страницы.
    size_t idx = (size_t)(y >> 3) * (size_t)s_cfg.width + (size_t)x;
    s_fb[idx] |= (uint8_t)(1u << (y & 7));
}

void oled042_draw_text(int x, int y, const char *s)
{
    // Рисуем строку в локальном буфере: 5 столбцов глифа + 1 пустой столбец между символами.
    int cx = x;
    while (*s) {
        const uint8_t *g = glyph5x7(*s++);
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; ++row) if (bits & (1u << row)) fb_set_px(cx + col, y + row);
        }
        cx += 6; // 5 столбцов символа + 1 столбец интервала.
        if (cx > s_cfg.width - 6) break;
    }
}

void oled042_draw_uint(int x, int y, unsigned v)
{
    // Преобразуем беззнаковое число в строку и выводим тем же текстовым рендерером.
    char buf[12]; int i = 11; buf[i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v && i) { buf[--i] = (char)('0' + v%10); v/=10; }
    oled042_draw_text(x, y, &buf[i]);
}

void oled042_test_pattern(void)
{
    if (!s_fb) return;
    // Тестовый паттерн: чередуем горизонтальные полосы по страницам,
    // чтобы быстро увидеть смещение, ориентацию и корректность обновления.
    for (int p = 0; p < pages(); ++p) {
        uint8_t val = (p & 1) ? 0xAA : 0x55;
        memset(&s_fb[p * s_cfg.width], val, s_cfg.width);
    }
    oled042_update();
}

void oled042_set_col_offset(int offset)
{
    // Меняем смещение столбцов во время работы: полезно, если видимая область модуля сдвинута.
    s_cfg.col_offset = offset;
}
