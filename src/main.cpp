#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>
#include <lvgl.h>
#include <LovyanGFX.hpp>
#include <CSE_CST328.h>
#include <driver/pcnt.h>

#include "secrets.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

namespace {
constexpr uint32_t WIFI_RETRY_DELAY_MS = 5000;
constexpr char WS_SERVER_IP[] = "192.168.4.1";
constexpr uint16_t WS_SERVER_PORT = 80;
constexpr char WS_SERVER_PATH[] = "/ws";

constexpr char PROTOCOL_VERSION[] = "reefnet.v1";
constexpr char MODULE_ID[] = "PickleFlow.FlowMeter";
constexpr char SUBMODULE_ID[] = "PickleFlow.FlowMeter";
constexpr char SUBSYSTEM_KEY[] = "flow";
constexpr char SUBSYSTEM_LABEL[] = "Pickle Flow";
constexpr char FIRMWARE_VERSION[] = "0.1.0";
constexpr char FIRMWARE_BUILD[] = __DATE__ " " __TIME__;

constexpr uint8_t STATUS_LED_PIN = LED_BUILTIN;
constexpr uint8_t FLOW_SENSOR_PIN = 15;
constexpr uint32_t DEFAULT_REPORT_INTERVAL_MS = 1000;
constexpr uint32_t REPORT_INTERVAL_MIN_MS = 200;
constexpr uint32_t REPORT_INTERVAL_MAX_MS = 10000;
constexpr float FLOW_CALIBRATION_PULSES_PER_LITER = 70.01f;
constexpr uint32_t PULSE_DEBOUNCE_US = 120;
constexpr uint32_t SETTINGS_SAVE_INTERVAL_MS = 60000;

constexpr char PREF_NAMESPACE[] = "pickle_flow";
constexpr char PREF_KEY_REPORT_MS[] = "report_ms";
constexpr char PREF_KEY_INVERTED[] = "inverted";
constexpr char PREF_KEY_TOTAL_L[] = "total_l";
constexpr char PREF_KEY_TOTAL_P[] = "total_p";

WebSocketsClient wsClient;
Preferences prefs;

volatile uint32_t isrPulseCounter = 0;
volatile uint32_t isrIgnoredBounceCounter = 0;
volatile uint32_t isrLastPulseUs = 0;
bool pulseCounterUsingPcnt = false;

float pulsesPerLiter = FLOW_CALIBRATION_PULSES_PER_LITER;
uint32_t reportIntervalMs = DEFAULT_REPORT_INTERVAL_MS;
bool flowInputInverted = false;

uint64_t totalPulses = 0;
float totalLiters = 0.0f;
float flowRateLpm = 0.0f;
float pulsesPerSecond = 0.0f;
float uiNeedleLphFiltered = 0.0f;
uint32_t lastSamplePulseCount = 0;
unsigned long lastSampleMs = 0;
unsigned long lastSettingsSaveMs = 0;
unsigned long lastStatusBroadcastMs = 0;
unsigned long lastBlinkMs = 0;
unsigned long lastPulseDebugMs = 0;
bool ledOn = false;
bool settingsDirty = false;

constexpr int LCD_BL_PIN = 5;
constexpr uint32_t LVGL_BUF_SIZE = 320 * 20;
constexpr int TOUCH_SDA_PIN = 1;
constexpr int TOUCH_SCL_PIN = 3;
constexpr int TOUCH_RST_PIN = 2;
constexpr int TOUCH_INT_PIN = 4;

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_ST7789 _panel;

public:
    LGFX() {
        auto busCfg = _bus.config();
        busCfg.spi_host = SPI2_HOST;
        busCfg.spi_mode = 0;
        busCfg.freq_write = 40000000;
        busCfg.freq_read = 16000000;
        busCfg.spi_3wire = true;
        busCfg.use_lock = true;
        busCfg.dma_channel = SPI_DMA_CH_AUTO;
        busCfg.pin_sclk = 40;
        busCfg.pin_mosi = 45;
        busCfg.pin_miso = -1;
        busCfg.pin_dc = 41;
        _bus.config(busCfg);

        auto panelCfg = _panel.config();
        panelCfg.pin_cs = 42;
        panelCfg.pin_rst = 39;
        panelCfg.panel_width = 240;
        panelCfg.panel_height = 320;
        panelCfg.memory_width = 240;
        panelCfg.memory_height = 320;
        panelCfg.invert = true;
        panelCfg.offset_rotation = 0;
        _panel.config(panelCfg);

        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

LGFX lcd;
lv_disp_draw_buf_t lvDrawBuf;
lv_disp_drv_t lvDispDrv;
lv_indev_drv_t lvIndevDrv;
lv_color_t lvBuf1[LVGL_BUF_SIZE];
lv_color_t lvBuf2[LVGL_BUF_SIZE];
TwoWire touchWire(1);
CSE_CST328 touch(320, 240, &touchWire, TOUCH_RST_PIN, TOUCH_INT_PIN);
bool touchReady = false;

bool displayReady = false;
lv_obj_t* uiScreen = nullptr;
lv_obj_t* uiTabView = nullptr;
lv_obj_t* uiMainLph = nullptr;
lv_obj_t* uiMainAvgLph = nullptr;
lv_obj_t* uiMainLphGauge = nullptr;
lv_meter_scale_t* uiMainLphGaugeScale = nullptr;
lv_meter_indicator_t* uiMainLphGaugeNeedle = nullptr;
lv_obj_t* uiResetButton = nullptr;
lv_obj_t* uiLph = nullptr;
lv_obj_t* uiFlowRate = nullptr;
lv_obj_t* uiPulseRate = nullptr;
lv_obj_t* uiTotal = nullptr;
lv_obj_t* uiHistoryChart = nullptr;
lv_chart_series_t* uiHistorySeries = nullptr;
lv_obj_t* uiSpanBtnMatrix = nullptr;

unsigned long lastUiUpdateMs = 0;
unsigned long lastLvTickMs = 0;

constexpr uint32_t COLOR_BG_HEX = 0x07121C;
constexpr uint32_t COLOR_BG_GRAD_HEX = 0x0E2436;
constexpr uint32_t COLOR_SURFACE_HEX = 0x102A3C;
constexpr uint32_t COLOR_SURFACE_ALT_HEX = 0x1A3A52;
constexpr uint32_t COLOR_ACCENT_HEX = 0xFD6A02;
constexpr uint32_t COLOR_GOOD_HEX = 0x2ECC71;
constexpr uint32_t COLOR_ALERT_HEX = 0xF94444;
constexpr uint32_t COLOR_INFO_HEX = 0x34B3C6;
constexpr uint32_t COLOR_NEEDLE_PURPLE_HEX = 0xA855F7;
constexpr uint32_t COLOR_TEXT_PRIMARY_HEX = 0xFFFFFF;
constexpr uint32_t COLOR_TEXT_MUTED_HEX = 0x9FB8CA;
constexpr float LPH_GAUGE_MIN = 0.0f;
constexpr float LPH_GAUGE_MAX = 2000.0f;
constexpr float LPH_OPTIMAL_MIN = 750.0f;
constexpr float LPH_OPTIMAL_MAX = 1250.0f;
constexpr float LPH_NEEDLE_DAMPING_ALPHA = 0.18f;

constexpr uint16_t HISTORY_TOTAL_MINUTES = 10080;
constexpr uint16_t HISTORY_CHART_POINTS = 120;
constexpr uint32_t HISTORY_SAMPLE_INTERVAL_MS = 60000;

enum HistorySpan : uint8_t {
    SPAN_1H = 0,
    SPAN_6H,
    SPAN_12H,
    SPAN_1D,
    SPAN_1W,
    SPAN_COUNT
};

const uint16_t HISTORY_SPAN_MINUTES[SPAN_COUNT] = {60, 360, 720, 1440, 10080};

uint16_t lphHistory[HISTORY_TOTAL_MINUTES] = {0};
uint16_t lphHistoryWriteIndex = 0;
uint16_t lphHistoryCount = 0;
HistorySpan selectedHistorySpan = SPAN_1H;
unsigned long lastHistorySampleMs = 0;
}

IRAM_ATTR void flowPulseIsrFallback();

template <typename T>
T clampValue(T value, T minValue, T maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

String isoTimestampNow() {
    time_t now = time(nullptr);
    struct tm tmInfo;
    char buffer[25];
    if (now > 0 && gmtime_r(&now, &tmInfo) != nullptr) {
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmInfo);
        return String(buffer);
    }

    time_t fallback = static_cast<time_t>(millis() / 1000);
    if (gmtime_r(&fallback, &tmInfo) != nullptr) {
        strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tmInfo);
        return String(buffer);
    }

    snprintf(buffer, sizeof(buffer), "1970-01-01T00:00:%02luZ", static_cast<unsigned long>((millis() / 1000) % 60));
    return String(buffer);
}

JsonObject beginEnvelope(JsonDocument& doc, const char* messageType) {
    doc.clear();
    doc["protocol"] = PROTOCOL_VERSION;
    doc["module_id"] = MODULE_ID;
    doc["submodule_id"] = SUBMODULE_ID;
    doc["type"] = messageType;
    doc["sent_at"] = isoTimestampNow();
    return doc.createNestedObject("payload");
}

void appendSensor(JsonArray& sensors, const char* label, float value, const char* unit = nullptr) {
    JsonObject sensor = sensors.createNestedObject();
    sensor["label"] = label;
    sensor["value"] = value;
    if (unit && unit[0]) {
        sensor["unit"] = unit;
    }
}

bool configurePcntPolarity() {
    pcnt_count_mode_t posMode = flowInputInverted ? PCNT_COUNT_INC : PCNT_COUNT_DIS;
    pcnt_count_mode_t negMode = flowInputInverted ? PCNT_COUNT_DIS : PCNT_COUNT_INC;

    esp_err_t modeErr = pcnt_set_mode(
        PCNT_UNIT_0,
        PCNT_CHANNEL_0,
        posMode,
        negMode,
        PCNT_MODE_KEEP,
        PCNT_MODE_KEEP);
    return modeErr == ESP_OK;
}

bool initializePulseCounterPcnt() {
    pcnt_config_t pcntConfig = {};
    pcntConfig.pulse_gpio_num = FLOW_SENSOR_PIN;
    pcntConfig.ctrl_gpio_num = PCNT_PIN_NOT_USED;
    pcntConfig.channel = PCNT_CHANNEL_0;
    pcntConfig.unit = PCNT_UNIT_0;
    pcntConfig.pos_mode = flowInputInverted ? PCNT_COUNT_INC : PCNT_COUNT_DIS;
    pcntConfig.neg_mode = flowInputInverted ? PCNT_COUNT_DIS : PCNT_COUNT_INC;
    pcntConfig.lctrl_mode = PCNT_MODE_KEEP;
    pcntConfig.hctrl_mode = PCNT_MODE_KEEP;
    pcntConfig.counter_h_lim = 32767;
    pcntConfig.counter_l_lim = 0;

    if (pcnt_unit_config(&pcntConfig) != ESP_OK) {
        return false;
    }

    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_counter_clear(PCNT_UNIT_0);

    pcnt_filter_disable(PCNT_UNIT_0);

    pcnt_counter_resume(PCNT_UNIT_0);
    return true;
}

void applyFlowInputPolarity() {
    if (pulseCounterUsingPcnt) {
        pcnt_counter_pause(PCNT_UNIT_0);
        if (!configurePcntPolarity()) {
            Serial.println("[Flow] Failed to update PCNT polarity");
        }
        pcnt_counter_clear(PCNT_UNIT_0);
        pcnt_counter_resume(PCNT_UNIT_0);
        return;
    }

    detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
    const int mode = flowInputInverted ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulseIsrFallback, mode);
}

void lvglFlushCb(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* colorBuffer) {
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;

    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, width, height);
    lcd.pushPixels(reinterpret_cast<uint16_t*>(colorBuffer), width * height, true);
    lcd.endWrite();

    lv_disp_flush_ready(disp);
}

void lvglTouchRead(lv_indev_drv_t* indevDriver, lv_indev_data_t* data) {
    (void)indevDriver;

    if (!touchReady) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    touch.readData();
    auto point = touch.getPoint();
    bool touched = touch.isTouched();

    if (touched) {
        data->point.x = clampValue<int16_t>(static_cast<int16_t>(point.x), 0, 319);
        data->point.y = clampValue<int16_t>(static_cast<int16_t>(point.y), 0, 239);
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void runDisplaySelfTest() {
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(12, 16);
    lcd.print("Pickle Flow");
    lcd.setCursor(12, 46);
    lcd.print("LCD self-test");

    lcd.fillRect(12, 90, 90, 28, TFT_RED);
    lcd.fillRect(114, 90, 90, 28, TFT_GREEN);
    lcd.fillRect(216, 90, 90, 28, TFT_BLUE);

    delay(500);
}

void saveSettings();
void refreshHistoryChart();

void onHistorySpanChanged(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t* obj = lv_event_get_target(event);
    int selected = lv_btnmatrix_get_selected_btn(obj);
    if (selected >= 0 && selected < static_cast<int>(SPAN_COUNT)) {
        selectedHistorySpan = static_cast<HistorySpan>(selected);
        refreshHistoryChart();
    }
}

void refreshHistoryChart() {
    if (!uiHistoryChart || !uiHistorySeries) {
        return;
    }

    const uint16_t spanMinutes = HISTORY_SPAN_MINUTES[selectedHistorySpan];
    const uint16_t available = (lphHistoryCount < spanMinutes) ? lphHistoryCount : spanMinutes;
    const uint16_t points = HISTORY_CHART_POINTS;

    if (available == 0) {
        lv_chart_set_all_value(uiHistoryChart, uiHistorySeries, 0);
        lv_chart_refresh(uiHistoryChart);
        return;
    }

    for (uint16_t pointIndex = 0; pointIndex < points; ++pointIndex) {
        uint32_t minuteOffset = (static_cast<uint32_t>(pointIndex) * available) / points;
        if (minuteOffset >= available) {
            minuteOffset = available - 1;
        }

        uint16_t historyOffsetFromLatest = (available - 1) - static_cast<uint16_t>(minuteOffset);
        int32_t idx = static_cast<int32_t>(lphHistoryWriteIndex) - 1 - static_cast<int32_t>(historyOffsetFromLatest);
        while (idx < 0) {
            idx += HISTORY_TOTAL_MINUTES;
        }
        idx %= HISTORY_TOTAL_MINUTES;

        uint16_t stored = lphHistory[idx];
        uiHistorySeries->y_points[pointIndex] = static_cast<lv_coord_t>(stored / 10);
    }

    lv_chart_refresh(uiHistoryChart);
}

float getAverageLph(uint16_t minutes) {
    if (lphHistoryCount == 0 || minutes == 0) {
        return 0.0f;
    }

    uint16_t sampleCount = (lphHistoryCount < minutes) ? lphHistoryCount : minutes;
    uint32_t sumTenths = 0;
    for (uint16_t index = 0; index < sampleCount; ++index) {
        int32_t historyIndex = static_cast<int32_t>(lphHistoryWriteIndex) - 1 - static_cast<int32_t>(index);
        while (historyIndex < 0) {
            historyIndex += HISTORY_TOTAL_MINUTES;
        }
        historyIndex %= HISTORY_TOTAL_MINUTES;
        sumTenths += lphHistory[historyIndex];
    }

    return static_cast<float>(sumTenths) / static_cast<float>(sampleCount) / 10.0f;
}

void sampleLphHistory(unsigned long now) {
    if (lastHistorySampleMs != 0 && (now - lastHistorySampleMs) < HISTORY_SAMPLE_INTERVAL_MS) {
        return;
    }

    lastHistorySampleMs = now;
    float lph = flowRateLpm * 60.0f;
    if (lph < 0.0f) {
        lph = 0.0f;
    }
    if (lph > 6553.5f) {
        lph = 6553.5f;
    }

    lphHistory[lphHistoryWriteIndex] = static_cast<uint16_t>(lph * 10.0f);
    lphHistoryWriteIndex = (lphHistoryWriteIndex + 1) % HISTORY_TOTAL_MINUTES;
    if (lphHistoryCount < HISTORY_TOTAL_MINUTES) {
        lphHistoryCount++;
    }

    refreshHistoryChart();
}

void resetTotalCounters() {
    totalLiters = 0.0f;
    totalPulses = 0;
    flowRateLpm = 0.0f;
    pulsesPerSecond = 0.0f;
    uiNeedleLphFiltered = 0.0f;
    lastSamplePulseCount = 0;
    lastSampleMs = 0;

    noInterrupts();
    isrPulseCounter = 0;
    isrIgnoredBounceCounter = 0;
    isrLastPulseUs = 0;
    interrupts();

    if (pulseCounterUsingPcnt) {
        pcnt_counter_pause(PCNT_UNIT_0);
        pcnt_counter_clear(PCNT_UNIT_0);
        pcnt_counter_resume(PCNT_UNIT_0);
    }

    settingsDirty = true;
    saveSettings();
    lastSettingsSaveMs = millis();

    memset(lphHistory, 0, sizeof(lphHistory));
    lphHistoryWriteIndex = 0;
    lphHistoryCount = 0;
    lastHistorySampleMs = 0;
    refreshHistoryChart();

    Serial.println("[Flow] Totals reset from UI");
}

void onResetButtonClick(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    resetTotalCounters();
}

lv_obj_t* createHeatStyleCard(lv_obj_t* parent, lv_coord_t width, lv_coord_t height, lv_coord_t x, lv_coord_t y, uint32_t colorHex) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, width, height);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(colorHex), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_ofs_y(card, 4, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    return card;
}

void initializeDisplayUi() {
    Serial.println("[LCD] Enabling backlight");
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, HIGH);
    delay(500);

    Serial.println("[LCD] Initializing panel");
    lcd.init();
    lcd.setBrightness(128);
    lcd.setRotation(1);
    runDisplaySelfTest();

    Serial.println("[LVGL] Init start");

    lv_init();
    lv_disp_draw_buf_init(&lvDrawBuf, lvBuf1, lvBuf2, LVGL_BUF_SIZE);

    lv_disp_drv_init(&lvDispDrv);
    lvDispDrv.hor_res = 320;
    lvDispDrv.ver_res = 240;
    lvDispDrv.draw_buf = &lvDrawBuf;
    lvDispDrv.flush_cb = lvglFlushCb;
    lv_disp_drv_register(&lvDispDrv);

    Serial.println("[Touch] Initializing CST328");
    touchWire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, 400000);
    touchReady = touch.begin();
    if (touchReady) {
        touch.setRotation(1);
        Serial.println("[Touch] Ready");
    } else {
        Serial.println("[Touch] Init failed");
    }

    lv_indev_drv_init(&lvIndevDrv);
    lvIndevDrv.type = LV_INDEV_TYPE_POINTER;
    lvIndevDrv.read_cb = lvglTouchRead;
    lv_indev_drv_register(&lvIndevDrv);

    uiScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(uiScreen, lv_color_hex(COLOR_BG_HEX), 0);
    lv_obj_set_style_bg_grad_color(uiScreen, lv_color_hex(COLOR_BG_GRAD_HEX), 0);
    lv_obj_set_style_bg_grad_dir(uiScreen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(uiScreen, LV_OPA_COVER, 0);

    uiTabView = lv_tabview_create(uiScreen, LV_DIR_TOP, 0);
    lv_obj_set_size(uiTabView, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(uiTabView, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(uiTabView, 0, 0);
    lv_obj_set_style_pad_all(uiTabView, 0, 0);

    lv_obj_t* tabButtons = lv_tabview_get_tab_btns(uiTabView);
    if (tabButtons) {
        lv_obj_add_flag(tabButtons, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* tabMain = lv_tabview_add_tab(uiTabView, "Main");
    lv_obj_t* tabChart = lv_tabview_add_tab(uiTabView, "Chart");
    lv_obj_t* tabStats = lv_tabview_add_tab(uiTabView, "Stats");

    lv_obj_set_style_bg_opa(tabMain, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(tabChart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(tabStats, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(tabMain, 0, 0);

    lv_obj_t* mainCard = createHeatStyleCard(tabMain, LV_PCT(100), LV_PCT(100), 0, 0, COLOR_SURFACE_HEX);
    lv_obj_set_style_radius(mainCard, 0, 0);
    lv_obj_set_style_shadow_width(mainCard, 0, 0);
    lv_obj_set_style_pad_all(mainCard, 0, 0);

    uiMainLphGauge = lv_meter_create(mainCard);
    lv_obj_remove_style_all(uiMainLphGauge);
    lv_obj_set_size(uiMainLphGauge, 188, 188);
    lv_obj_align(uiMainLphGauge, LV_ALIGN_CENTER, 0, -18);
    lv_obj_set_style_bg_opa(uiMainLphGauge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(uiMainLphGauge, lv_color_hex(COLOR_TEXT_MUTED_HEX), 0);

    uiMainLphGaugeScale = lv_meter_add_scale(uiMainLphGauge);
    lv_meter_set_scale_ticks(uiMainLphGauge, uiMainLphGaugeScale, 41, 2, 8, lv_color_hex(COLOR_TEXT_MUTED_HEX));
    lv_meter_set_scale_major_ticks(uiMainLphGauge, uiMainLphGaugeScale, 5, 4, 14, lv_color_hex(COLOR_TEXT_MUTED_HEX), 10);
    lv_meter_set_scale_range(uiMainLphGauge, uiMainLphGaugeScale, static_cast<int32_t>(LPH_GAUGE_MIN), static_cast<int32_t>(LPH_GAUGE_MAX), 270, 135);

    lv_meter_indicator_t* gaugeTrack = lv_meter_add_arc(uiMainLphGauge, uiMainLphGaugeScale, 7, lv_color_hex(COLOR_INFO_HEX), 0);
    lv_meter_set_indicator_start_value(uiMainLphGauge, gaugeTrack, static_cast<int32_t>(LPH_GAUGE_MIN));
    lv_meter_set_indicator_end_value(uiMainLphGauge, gaugeTrack, static_cast<int32_t>(LPH_GAUGE_MAX));

    lv_meter_indicator_t* optimalBand = lv_meter_add_arc(uiMainLphGauge, uiMainLphGaugeScale, 9, lv_color_hex(COLOR_GOOD_HEX), 0);
    lv_meter_set_indicator_start_value(uiMainLphGauge, optimalBand, static_cast<int32_t>(LPH_OPTIMAL_MIN));
    lv_meter_set_indicator_end_value(uiMainLphGauge, optimalBand, static_cast<int32_t>(LPH_OPTIMAL_MAX));

    uiMainLphGaugeNeedle = lv_meter_add_needle_line(uiMainLphGauge, uiMainLphGaugeScale, 4, lv_color_hex(COLOR_NEEDLE_PURPLE_HEX), -12);
    lv_meter_set_indicator_value(uiMainLphGauge, uiMainLphGaugeNeedle, 0);

    uiMainLph = lv_label_create(mainCard);
    lv_label_set_text(uiMainLph, "0");
    lv_obj_set_style_text_font(uiMainLph, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(uiMainLph, lv_color_hex(COLOR_TEXT_PRIMARY_HEX), 0);
    lv_obj_align(uiMainLph, LV_ALIGN_CENTER, 0, 0);

    uiMainAvgLph = lv_label_create(mainCard);
    lv_label_set_text(uiMainAvgLph, "Avg 1h: 0.0 LPH");
    lv_obj_set_style_text_color(uiMainAvgLph, lv_color_hex(COLOR_TEXT_MUTED_HEX), 0);
    lv_obj_set_style_text_font(uiMainAvgLph, &lv_font_montserrat_14, 0);
    lv_obj_align(uiMainAvgLph, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_obj_t* chartCard = createHeatStyleCard(tabChart, 292, 188, 8, 18, COLOR_SURFACE_ALT_HEX);
    static const char* spanMap[] = {"1h", "6h", "12h", "1d", "1w", ""};
    uiSpanBtnMatrix = lv_btnmatrix_create(chartCard);
    lv_btnmatrix_set_map(uiSpanBtnMatrix, spanMap);
    lv_obj_set_size(uiSpanBtnMatrix, 268, 26);
    lv_obj_align(uiSpanBtnMatrix, LV_ALIGN_TOP_MID, 0, 0);
    lv_btnmatrix_set_btn_ctrl_all(uiSpanBtnMatrix, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(uiSpanBtnMatrix, true);
    lv_btnmatrix_set_btn_ctrl(uiSpanBtnMatrix, SPAN_1H, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_add_event_cb(uiSpanBtnMatrix, onHistorySpanChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    uiHistoryChart = lv_chart_create(chartCard);
    lv_obj_set_size(uiHistoryChart, 268, 140);
    lv_obj_align(uiHistoryChart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(uiHistoryChart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(uiHistoryChart, HISTORY_CHART_POINTS);
    lv_chart_set_range(uiHistoryChart, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
    lv_chart_set_update_mode(uiHistoryChart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_obj_set_style_bg_opa(uiHistoryChart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(uiHistoryChart, 0, 0);
    lv_obj_set_style_pad_all(uiHistoryChart, 0, 0);
    uiHistorySeries = lv_chart_add_series(uiHistoryChart, lv_color_hex(COLOR_INFO_HEX), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(uiHistoryChart, uiHistorySeries, 0);

    lv_obj_t* statsCard = createHeatStyleCard(tabStats, 292, 188, 8, 18, COLOR_SURFACE_HEX);
    lv_obj_t* lpmTitle = lv_label_create(statsCard);
    lv_label_set_text(lpmTitle, "LPM");
    lv_obj_set_style_text_color(lpmTitle, lv_color_hex(COLOR_TEXT_MUTED_HEX), 0);
    lv_obj_align(lpmTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    uiFlowRate = lv_label_create(statsCard);
    lv_label_set_text(uiFlowRate, "0.00");
    lv_obj_set_style_text_font(uiFlowRate, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(uiFlowRate, lv_color_hex(COLOR_ACCENT_HEX), 0);
    lv_obj_align(uiFlowRate, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t* pulseTitle = lv_label_create(statsCard);
    lv_label_set_text(pulseTitle, "Pulse Rate");
    lv_obj_set_style_text_color(pulseTitle, lv_color_hex(COLOR_TEXT_MUTED_HEX), 0);
    lv_obj_align(pulseTitle, LV_ALIGN_TOP_LEFT, 0, 42);

    uiPulseRate = lv_label_create(statsCard);
    lv_label_set_text(uiPulseRate, "0.00");
    lv_obj_set_style_text_font(uiPulseRate, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(uiPulseRate, lv_color_hex(COLOR_TEXT_PRIMARY_HEX), 0);
    lv_obj_align(uiPulseRate, LV_ALIGN_TOP_RIGHT, 0, 42);

    lv_obj_t* totalTitle = lv_label_create(statsCard);
    lv_label_set_text(totalTitle, "Total Volume");
    lv_obj_set_style_text_color(totalTitle, lv_color_hex(COLOR_TEXT_MUTED_HEX), 0);
    lv_obj_align(totalTitle, LV_ALIGN_TOP_LEFT, 0, 84);

    uiTotal = lv_label_create(statsCard);
    lv_label_set_text(uiTotal, "0.000 L");
    lv_obj_set_style_text_font(uiTotal, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(uiTotal, lv_color_hex(COLOR_TEXT_PRIMARY_HEX), 0);
    lv_obj_align(uiTotal, LV_ALIGN_TOP_RIGHT, 0, 84);

    uiResetButton = lv_btn_create(statsCard);
    lv_obj_set_size(uiResetButton, 120, 34);
    lv_obj_align(uiResetButton, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(uiResetButton, lv_color_hex(COLOR_SURFACE_ALT_HEX), 0);
    lv_obj_set_style_radius(uiResetButton, 12, 0);
    lv_obj_add_event_cb(uiResetButton, onResetButtonClick, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* resetLabel = lv_label_create(uiResetButton);
    lv_label_set_text(resetLabel, "Reset Total");
    lv_obj_set_style_text_color(resetLabel, lv_color_hex(COLOR_TEXT_PRIMARY_HEX), 0);
    lv_obj_center(resetLabel);

    lv_scr_load(uiScreen);
    refreshHistoryChart();
    lv_timer_handler();

    Serial.println("[LCD] LVGL screen loaded");
    displayReady = true;
}

void updateDisplayUi(unsigned long now) {
    if (!displayReady || (now - lastUiUpdateMs) < 250) {
        return;
    }

    lastUiUpdateMs = now;

    char text[96];
    snprintf(text, sizeof(text), "%.2f", flowRateLpm);
    lv_label_set_text(uiFlowRate, text);

    snprintf(text, sizeof(text), "%.2f Hz", pulsesPerSecond);
    lv_label_set_text(uiPulseRate, text);

    const float lph = flowRateLpm * 60.0f;
    snprintf(text, sizeof(text), "%.0f", lph);
    lv_label_set_text(uiMainLph, text);
    if (uiMainLphGauge && uiMainLphGaugeNeedle) {
        const float clampedLph = clampValue(lph, LPH_GAUGE_MIN, LPH_GAUGE_MAX);
        const float filtered = uiNeedleLphFiltered + ((clampedLph - uiNeedleLphFiltered) * LPH_NEEDLE_DAMPING_ALPHA);
        uiNeedleLphFiltered = clampValue(filtered, LPH_GAUGE_MIN, LPH_GAUGE_MAX);
        lv_meter_set_indicator_value(uiMainLphGauge, uiMainLphGaugeNeedle, static_cast<int32_t>(uiNeedleLphFiltered));
    }

    if (lph >= LPH_OPTIMAL_MIN && lph <= LPH_OPTIMAL_MAX) {
        lv_obj_set_style_text_color(uiMainLph, lv_color_hex(COLOR_GOOD_HEX), 0);
    } else {
        lv_obj_set_style_text_color(uiMainLph, lv_color_hex(COLOR_TEXT_PRIMARY_HEX), 0);
    }

    const float averageLphHour = getAverageLph(60);
    snprintf(text, sizeof(text), "Avg 1h: %.1f LPH", averageLphHour);
    lv_label_set_text(uiMainAvgLph, text);

    snprintf(text, sizeof(text), "%.3f L", totalLiters);
    lv_label_set_text(uiTotal, text);

    sampleLphHistory(now);
}

void saveSettings() {
    if (!prefs.begin(PREF_NAMESPACE, false)) {
        Serial.println("[Prefs] Failed to open namespace for write");
        return;
    }

    prefs.putUInt(PREF_KEY_REPORT_MS, reportIntervalMs);
    prefs.putBool(PREF_KEY_INVERTED, flowInputInverted);
    prefs.putFloat(PREF_KEY_TOTAL_L, totalLiters);
    prefs.putULong64(PREF_KEY_TOTAL_P, totalPulses);
    prefs.end();
    settingsDirty = false;
}

void loadSettings() {
    if (!prefs.begin(PREF_NAMESPACE, false)) {
        Serial.println("[Prefs] Failed to open namespace for read");
        return;
    }

    pulsesPerLiter = FLOW_CALIBRATION_PULSES_PER_LITER;
    reportIntervalMs = clampValue(prefs.getUInt(PREF_KEY_REPORT_MS, DEFAULT_REPORT_INTERVAL_MS), REPORT_INTERVAL_MIN_MS, REPORT_INTERVAL_MAX_MS);
    flowInputInverted = prefs.getBool(PREF_KEY_INVERTED, false);
    totalLiters = prefs.getFloat(PREF_KEY_TOTAL_L, 0.0f);
    totalPulses = prefs.getULong64(PREF_KEY_TOTAL_P, 0);
    prefs.end();
}

IRAM_ATTR void flowPulseIsrFallback() {
    uint32_t nowUs = static_cast<uint32_t>(micros());
    uint32_t dt = nowUs - isrLastPulseUs;
    if (isrLastPulseUs != 0 && dt < PULSE_DEBOUNCE_US) {
        isrIgnoredBounceCounter++;
        return;
    }

    isrLastPulseUs = nowUs;
    isrPulseCounter++;
}

void connectToWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[WiFi] Connecting to ");
    Serial.println(WIFI_SSID);

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_RETRY_DELAY_MS) {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] Connected, IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WiFi] Connection timed out, retrying later");
    }
}

void transmitDocument(const JsonDocument& doc) {
    if (!wsClient.isConnected()) {
        return;
    }

    String payload;
    serializeJson(doc, payload);
    wsClient.sendTXT(payload);
}

void sendStatus() {
    StaticJsonDocument<1024> doc;
    JsonObject payload = beginEnvelope(doc, "status");
    payload["uptime_s"] = millis() / 1000;

    JsonObject firmware = payload.createNestedObject("firmware");
    firmware["version"] = FIRMWARE_VERSION;
    firmware["build"] = FIRMWARE_BUILD;

    if (WiFi.status() == WL_CONNECTED) {
        JsonObject network = payload.createNestedObject("network");
        network["ip"] = WiFi.localIP().toString();
        network["mac"] = WiFi.macAddress();
        network["ssid"] = WiFi.SSID();
        network["rssi_dbm"] = WiFi.RSSI();
    }

    JsonObject environment = payload.createNestedObject("environment");
    environment["temperature_c"] = NAN;

    JsonArray subsystems = payload.createNestedArray("subsystems");
    JsonObject flowSub = subsystems.createNestedObject();
    flowSub["key"] = SUBSYSTEM_KEY;
    flowSub["label"] = SUBSYSTEM_LABEL;
    flowSub["kind"] = "sensor";
    flowSub["submodule_id"] = SUBMODULE_ID;
    flowSub["state"] = flowRateLpm > 0.0001f ? "flowing" : "idle";
    flowSub["badge"] = flowRateLpm > 0.0001f ? "active" : "ok";

    JsonObject setpoints = flowSub.createNestedObject("setpoints");
    setpoints["pulses_per_liter"] = pulsesPerLiter;
    setpoints["report_interval_ms"] = reportIntervalMs;
    setpoints["flow_input_inverted"] = flowInputInverted;

    JsonArray sensors = flowSub.createNestedArray("sensors");
    appendSensor(sensors, "Flow Rate", flowRateLpm, "L/min");
    appendSensor(sensors, "Pulse Rate", pulsesPerSecond, "Hz");
    appendSensor(sensors, "Total Volume", totalLiters, "L");
    appendSensor(sensors, "Total Pulses", static_cast<float>(totalPulses), "count");
    appendSensor(sensors, "Ignored Bounce", static_cast<float>(isrIgnoredBounceCounter), "count");

    transmitDocument(doc);
}

void updateFlowMetrics(unsigned long now) {
    if (lastSampleMs == 0) {
        lastSampleMs = now;
        return;
    }

    unsigned long elapsedMs = now - lastSampleMs;
    if (elapsedMs < 200) {
        return;
    }

    uint32_t deltaPulses = 0;
    if (pulseCounterUsingPcnt) {
        int16_t pcntValue = 0;
        pcnt_counter_pause(PCNT_UNIT_0);
        if (pcnt_get_counter_value(PCNT_UNIT_0, &pcntValue) == ESP_OK) {
            if (pcntValue > 0) {
                deltaPulses = static_cast<uint32_t>(pcntValue);
            }
            pcnt_counter_clear(PCNT_UNIT_0);
        }
        pcnt_counter_resume(PCNT_UNIT_0);
    } else {
        noInterrupts();
        const uint32_t pulseSnapshot = isrPulseCounter;
        interrupts();
        deltaPulses = pulseSnapshot - lastSamplePulseCount;
        lastSamplePulseCount = pulseSnapshot;
    }
    lastSampleMs = now;

    const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
    pulsesPerSecond = elapsedSeconds > 0.0f ? (static_cast<float>(deltaPulses) / elapsedSeconds) : 0.0f;
    flowRateLpm = pulsesPerLiter > 0.0f ? ((pulsesPerSecond / pulsesPerLiter) * 60.0f) : 0.0f;

    if (deltaPulses > 0 && pulsesPerLiter > 0.0f) {
        totalPulses += deltaPulses;
        totalLiters += static_cast<float>(deltaPulses) / pulsesPerLiter;
        settingsDirty = true;
    }

    if (now - lastPulseDebugMs >= 1000) {
        lastPulseDebugMs = now;
        Serial.printf(
            "[FlowDbg] src=%s dP=%lu pps=%.2f lpm=%.2f total=%llu\n",
            pulseCounterUsingPcnt ? "PCNT" : "ISR",
            static_cast<unsigned long>(deltaPulses),
            static_cast<double>(pulsesPerSecond),
            static_cast<double>(flowRateLpm),
            static_cast<unsigned long long>(totalPulses));
    }
}

bool applySetParam(const char* name, JsonVariantConst value) {
    if (!name || !name[0] || value.isNull()) {
        return false;
    }

    if (strcmp(name, "report_interval_ms") == 0) {
        reportIntervalMs = clampValue(value.as<uint32_t>(), REPORT_INTERVAL_MIN_MS, REPORT_INTERVAL_MAX_MS);
        settingsDirty = true;
        return true;
    }

    if (strcmp(name, "flow_input_inverted") == 0) {
        flowInputInverted = value.as<bool>();
        applyFlowInputPolarity();
        settingsDirty = true;
        return true;
    }

    if (strcmp(name, "reset_total_liters") == 0) {
        if (value.as<bool>() || value.as<int>() == 1) {
            totalLiters = 0.0f;
            totalPulses = 0;
            noInterrupts();
            isrPulseCounter = 0;
            interrupts();
            lastSamplePulseCount = 0;
            settingsDirty = true;
        }
        return true;
    }

    return false;
}

bool handleSetParamMap(JsonObjectConst params) {
    bool updated = false;
    for (JsonPairConst kv : params) {
        updated |= applySetParam(kv.key().c_str(), kv.value());
    }

    return updated;
}

void handleControlMessage(const JsonObject& payload) {
    const char* command = payload["command"] | "";
    if (!command[0]) {
        Serial.println("[WS] Control frame missing command");
        return;
    }

    if (strcmp(command, "set_param") == 0 || strcmp(command, "set_parameter") == 0) {
        JsonObjectConst params = payload["parameters"].as<JsonObjectConst>();
        if (params.isNull()) {
            Serial.println("[WS] set_param requires payload.parameters object");
            return;
        }

        bool updated = false;
        const char* singleName = params["name"] | params["param"] | "";
        JsonVariantConst singleValue = params["value"];
        if (singleName[0] && !singleValue.isNull()) {
            updated = applySetParam(singleName, singleValue);
        } else {
            updated = handleSetParamMap(params);
        }

        if (updated) {
            saveSettings();
            lastSettingsSaveMs = millis();
            sendStatus();
        }
        return;
    }

    if (strcmp(command, "config_request") == 0 ||
        strcmp(command, "module_manifest_request") == 0 ||
        strcmp(command, "status_request") == 0 ||
        strcmp(command, "ping") == 0) {
        sendStatus();
        return;
    }
}

void handleInboundPayload(const uint8_t* payload, size_t length) {
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.print("[WS] JSON parse error: ");
        Serial.println(err.f_str());
        return;
    }

    const char* type = doc["type"] | "";
    if (strcmp(type, "control") != 0) {
        return;
    }

    JsonObject controlPayload = doc["payload"].as<JsonObject>();
    if (controlPayload.isNull()) {
        return;
    }

    handleControlMessage(controlPayload);
}

void configureWebSocket() {
    wsClient.begin(WS_SERVER_IP, WS_SERVER_PORT, WS_SERVER_PATH);
    wsClient.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
        switch (type) {
            case WStype_CONNECTED:
                Serial.println("[WS] Connected");
                sendStatus();
                break;
            case WStype_DISCONNECTED:
                Serial.println("[WS] Disconnected");
                break;
            case WStype_TEXT:
                handleInboundPayload(payload, length);
                break;
            default:
                break;
        }
    });

    wsClient.setReconnectInterval(WIFI_RETRY_DELAY_MS);
    wsClient.enableHeartbeat(15000, 2000, 1);
}

void setupFlowInput() {
    pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);

    pulseCounterUsingPcnt = initializePulseCounterPcnt();
    if (pulseCounterUsingPcnt) {
        Serial.println("[Flow] PCNT enabled on GPIO15");
        return;
    }

    Serial.println("[Flow] PCNT unavailable, using ISR fallback");
    const int mode = flowInputInverted ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulseIsrFallback, mode);
}

void setup() {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    Serial.begin(115200);
    while (!Serial && millis() < 2000) {
        delay(10);
    }

    Serial.println();
    Serial.println("=========================");
    Serial.println(" Pickle_Flow Controller ");
    Serial.println("=========================");

    initializeDisplayUi();
    loadSettings();
    lastSettingsSaveMs = millis();
    setupFlowInput();
    connectToWifi();
    configureWebSocket();
}

void loop() {
    unsigned long now = millis();

    if (displayReady) {
        if (lastLvTickMs == 0) {
            lastLvTickMs = now;
        }
        lv_tick_inc(now - lastLvTickMs);
        lastLvTickMs = now;
        lv_timer_handler();
    }

    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastRetry = 0;
        if (now - lastRetry >= WIFI_RETRY_DELAY_MS) {
            lastRetry = now;
            connectToWifi();
        }
    }

    wsClient.loop();
    updateFlowMetrics(now);

    if (wsClient.isConnected() && WiFi.status() == WL_CONNECTED && (now - lastStatusBroadcastMs) >= reportIntervalMs) {
        lastStatusBroadcastMs = now;
        sendStatus();
    }

    if (settingsDirty && (now - lastSettingsSaveMs) >= SETTINGS_SAVE_INTERVAL_MS) {
        saveSettings();
        lastSettingsSaveMs = now;
    }

    if (now - lastBlinkMs >= 1000) {
        lastBlinkMs = now;
        ledOn = !ledOn;
        digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
    }

    updateDisplayUi(now);

    delay(5);
}
