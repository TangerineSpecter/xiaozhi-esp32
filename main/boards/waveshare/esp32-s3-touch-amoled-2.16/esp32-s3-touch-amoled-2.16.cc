#include "wifi_board.h"
#include "display/lcd_display.h"
#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#define TAG "WaveshareEsp32s3TouchAMOLED2inch16"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x0A); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, // Sleep out

    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 600},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
private:
    static constexpr lv_coord_t kEmotionSourceSize = 108;
    static constexpr lv_coord_t kEmotionImageSize = 300;
    static constexpr lv_coord_t kEmotionBottomMargin = 0;
    static constexpr lv_coord_t kStatusBubbleWidth = 152;
    static constexpr lv_coord_t kStatusBubbleHeight = 32;
    static constexpr lv_coord_t kStatusBubbleTop = 36;
    static constexpr lv_coord_t kSpeechWidth = 212;
    static constexpr lv_coord_t kSpeechHeight = 82;
    static constexpr lv_coord_t kSpeechTop = 88;
    static constexpr lv_coord_t kSpeechTextWidth = kSpeechWidth - 34;
    static constexpr lv_coord_t kSpeechLineSpace = 4;
    static constexpr uint32_t kSpeechPageDurationMs = 4000;
    static constexpr lv_coord_t kStaminaPanelWidth = 164;
    static constexpr lv_coord_t kStaminaPanelHeight = 44;
    static constexpr lv_coord_t kStaminaBarWidth = 67;
    static constexpr lv_coord_t kStaminaBarHeight = 4;
    static constexpr lv_coord_t kStaminaContentLeft = 42;
    static constexpr lv_coord_t kEmotionScaleOverscan =
        (kEmotionImageSize - kEmotionSourceSize) / 2;
    static constexpr uint16_t kEmotionImageScale =
        static_cast<uint16_t>((kEmotionImageSize * 256) / kEmotionSourceSize);

    lv_obj_t* stamina_panel_ = nullptr;
    lv_obj_t* stamina_icon_ = nullptr;
    lv_obj_t* stamina_label_ = nullptr;
    lv_obj_t* stamina_bar_background_ = nullptr;
    lv_obj_t* stamina_bar_fill_ = nullptr;
    lv_obj_t* charging_label_ = nullptr;
    lv_timer_t* speech_timer_ = nullptr;
    std::string speech_text_;
    size_t speech_offset_ = 0;

    // Runs under the LVGL lock (including when invoked by its timer). Measure
    // each UTF-8 prefix using the same font/wrapping rules as the label.
    void ShowSpeechPage() {
        if (chat_message_label_ == nullptr || speech_text_.empty()) {
            return;
        }
        if (speech_offset_ >= speech_text_.size()) {
            speech_offset_ = 0;
        }
        const auto* font = lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
        const auto letter_space =
            lv_obj_get_style_text_letter_space(chat_message_label_, LV_PART_MAIN);
        const int32_t height = 2 * font->line_height + kSpeechLineSpace;
        char page[256] = {};
        size_t length = 0;
        while (speech_offset_ + length < speech_text_.size()) {
            size_t next = length + 1;
            while (next - length < 4 && speech_offset_ + next < speech_text_.size() &&
                   (static_cast<unsigned char>(speech_text_[speech_offset_ + next]) & 0xC0) ==
                       0x80) {
                ++next;
            }
            if (next >= sizeof(page)) {
                break;
            }
            std::memcpy(page + length, speech_text_.data() + speech_offset_ + length,
                        next - length);
            page[next] = '\0';
            lv_point_t size;
            lv_text_get_size(&size, page, font, letter_space, kSpeechLineSpace, kSpeechTextWidth,
                             LV_TEXT_FLAG_NONE);
            if (size.y > height && length > 0) {
                page[length] = '\0';
                break;
            }
            length = next;
        }
        lv_label_set_text(chat_message_label_, page);
        speech_offset_ += length;
        if (speech_timer_ != nullptr) {
            if (hide_subtitle_ || speech_offset_ >= speech_text_.size()) {
                // Keep the final page visible until replaced or cleared.
                lv_timer_pause(speech_timer_);
            } else {
                lv_timer_reset(speech_timer_);
                lv_timer_resume(speech_timer_);
            }
        }
    }

    static lv_color_t StaminaColor(int level) {
        if (level >= 60) {
            return lv_color_hex(0x9C76C6);
        }
        if (level >= 30) {
            return lv_color_hex(0xF0B83F);
        }
        return lv_color_hex(0xE85B5B);
    }

    void UpdateStamina(int level, bool charging) {
        if (stamina_panel_ == nullptr || stamina_icon_ == nullptr || stamina_label_ == nullptr ||
            stamina_bar_fill_ == nullptr || charging_label_ == nullptr) {
            return;
        }

        if (level < 0) {
            level = 0;
        } else if (level > 100) {
            level = 100;
        }

        lv_color_t color = StaminaColor(level);
        char stamina_text[24];
        std::snprintf(stamina_text, sizeof(stamina_text), "%d", level);
        lv_label_set_text(stamina_label_, stamina_text);
        lv_obj_set_style_bg_color(stamina_bar_fill_, color, 0);
        lv_obj_set_width(stamina_bar_fill_, (kStaminaBarWidth * level) / 100);
        lv_obj_set_flag(stamina_bar_fill_, LV_OBJ_FLAG_HIDDEN, level == 0);

        if (charging) {
            lv_label_set_text(charging_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT);
            lv_obj_remove_flag(charging_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(charging_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void ApplyPetUiStyles() {
        if (current_theme_ == nullptr) {
            return;
        }

        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        lv_color_t bubble_color = lv_color_hex(0xFFF8FF);
        lv_color_t bubble_border = lv_color_hex(0xC9ACD9);
        lv_color_t text_color = lv_color_hex(0x2B2140);

        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        }
        if (battery_label_ != nullptr) {
            // The board-specific stamina HUD replaces the small generic icon.
            lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        }

        if (status_bar_ != nullptr) {
            lv_obj_set_size(status_bar_, kStatusBubbleWidth, kStatusBubbleHeight);
            lv_obj_set_style_bg_color(status_bar_, bubble_color, 0);
            lv_obj_set_style_bg_opa(status_bar_, LV_OPA_90, 0);
            lv_obj_set_style_radius(status_bar_, 16, 0);
            lv_obj_set_style_border_width(status_bar_, 1, 0);
            lv_obj_set_style_border_color(status_bar_, bubble_border, 0);
            lv_obj_set_style_pad_all(status_bar_, 0, 0);
            lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
            lv_obj_align(status_bar_, LV_ALIGN_TOP_RIGHT, -28, kStatusBubbleTop);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_width(status_label_, kStatusBubbleWidth - 24);
            lv_obj_set_style_text_color(status_label_, text_color, 0);
            lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_width(notification_label_, kStatusBubbleWidth - 24);
            lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_text_color(notification_label_, text_color, 0);
            lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
        }

        if (bottom_bar_ != nullptr) {
            lv_obj_set_size(bottom_bar_, kSpeechWidth, kSpeechHeight);
            lv_obj_set_style_bg_color(bottom_bar_, bubble_color, 0);
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_90, 0);
            lv_obj_set_style_radius(bottom_bar_, 18, 0);
            lv_obj_set_style_border_width(bottom_bar_, 1, 0);
            lv_obj_set_style_border_color(bottom_bar_, bubble_border, 0);
            lv_obj_set_style_pad_all(bottom_bar_, 16, 0);
            lv_obj_set_style_pad_top(bottom_bar_, 10, 0);
            lv_obj_set_style_pad_bottom(bottom_bar_, 10, 0);
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(bottom_bar_, LV_ALIGN_TOP_RIGHT, -28, kSpeechTop);
        }
        if (chat_message_label_ != nullptr) {
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(chat_message_label_, kSpeechTextWidth);
            lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
            lv_obj_set_style_text_line_space(chat_message_label_, kSpeechLineSpace, 0);
            lv_obj_set_style_text_color(chat_message_label_, text_color, 0);
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(chat_message_label_, LV_ALIGN_TOP_LEFT, 0, 0);
        }

        if (stamina_panel_ != nullptr) {
            lv_obj_set_style_bg_color(stamina_panel_, bubble_color, 0);
            lv_obj_set_style_bg_opa(stamina_panel_, LV_OPA_90, 0);
            lv_obj_set_style_radius(stamina_panel_, 16, 0);
            lv_obj_set_style_border_width(stamina_panel_, 1, 0);
            lv_obj_set_style_border_color(stamina_panel_, bubble_border, 0);
        }
        if (stamina_icon_ != nullptr) {
            lv_obj_set_style_text_color(stamina_icon_, lv_color_hex(0x8F3FF0), 0);
            lv_obj_set_style_text_font(stamina_icon_, lvgl_theme->icon_font()->font(), 0);
        }
        if (stamina_label_ != nullptr) {
            lv_obj_set_style_text_color(stamina_label_, text_color, 0);
            lv_obj_set_style_text_font(stamina_label_, lvgl_theme->text_font()->font(), 0);
        }
        if (charging_label_ != nullptr) {
            lv_obj_set_style_text_color(charging_label_, lv_color_hex(0x8960AA), 0);
            lv_obj_set_style_text_font(charging_label_, lvgl_theme->icon_font()->font(), 0);
        }
    }

    void CreateStaminaHud() {
        stamina_panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(stamina_panel_, kStaminaPanelWidth, kStaminaPanelHeight);
        lv_obj_set_style_pad_all(stamina_panel_, 0, 0);
        lv_obj_remove_flag(stamina_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(stamina_panel_, LV_ALIGN_TOP_LEFT, 28, 30);

        stamina_icon_ = lv_label_create(stamina_panel_);
        lv_label_set_text(stamina_icon_, MATERIAL_SYMBOLS_FAVORITE);
        lv_obj_align(stamina_icon_, LV_ALIGN_LEFT_MID, 11, 0);

        stamina_label_ = lv_label_create(stamina_panel_);
        lv_obj_set_width(stamina_label_, kStaminaBarWidth);
        lv_obj_align(stamina_label_, LV_ALIGN_TOP_LEFT, kStaminaContentLeft, 3);

        charging_label_ = lv_label_create(stamina_panel_);
        lv_label_set_text(charging_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT);
        lv_obj_align(charging_label_, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_flag(charging_label_, LV_OBJ_FLAG_HIDDEN);

        stamina_bar_background_ = lv_obj_create(stamina_panel_);
        lv_obj_set_size(stamina_bar_background_, kStaminaBarWidth, kStaminaBarHeight);
        lv_obj_set_style_bg_color(stamina_bar_background_, lv_color_hex(0xE7DEED), 0);
        lv_obj_set_style_bg_opa(stamina_bar_background_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stamina_bar_background_, kStaminaBarHeight / 2, 0);
        lv_obj_set_style_border_width(stamina_bar_background_, 0, 0);
        lv_obj_set_style_pad_all(stamina_bar_background_, 0, 0);
        lv_obj_remove_flag(stamina_bar_background_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(stamina_bar_background_, LV_ALIGN_TOP_LEFT, kStaminaContentLeft, 32);

        stamina_bar_fill_ = lv_obj_create(stamina_bar_background_);
        lv_obj_set_size(stamina_bar_fill_, 1, kStaminaBarHeight);
        lv_obj_set_style_radius(stamina_bar_fill_, kStaminaBarHeight / 2, 0);
        lv_obj_set_style_border_width(stamina_bar_fill_, 0, 0);
        lv_obj_set_style_bg_opa(stamina_bar_fill_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(stamina_bar_fill_, 0, 0);
        lv_obj_remove_flag(stamina_bar_fill_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(stamina_bar_fill_, LV_ALIGN_LEFT_MID, 0, 0);

        ApplyPetUiStyles();
        UpdateStamina(0, false);
    }

public:
    ~CustomLcdDisplay() override {
        DisplayLockGuard lock(this);
        if (speech_timer_ != nullptr) {
            lv_timer_delete(speech_timer_);
        }
    }

    void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        if (chat_message_label_ == nullptr || bottom_bar_ == nullptr) {
            return;
        }
        (void)role;
        if (speech_timer_ != nullptr) {
            lv_timer_pause(speech_timer_);
        }
        speech_text_ = content != nullptr ? content : "";
        speech_offset_ = 0;
        if (speech_text_.empty()) {
            lv_label_set_text(chat_message_label_, "");
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        ShowSpeechPage();
        if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void ClearChatMessages() override { SetChatMessage("system", ""); }

    void SetStatus(const char* status) override {
        SpiLcdDisplay::SetStatus(status);
        if (status != nullptr && std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            ClearChatMessages();
        }
    }

    void SetHideSubtitle(bool hide) override {
        SpiLcdDisplay::SetHideSubtitle(hide);
        DisplayLockGuard lock(this);
        if (speech_timer_ != nullptr) {
            if (hide) {
                lv_timer_pause(speech_timer_);
            } else if (!speech_text_.empty()) {
                speech_offset_ = 0;
                ShowSpeechPage();
            }
        }
    }

    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                     int width, int height, int offset_x, int offset_y, bool mirror_x,
                     bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        // Use the whole screen as the transparent emotion canvas. The status
        // bar remains on top while the character is anchored to the bottom.
        lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES);
        lv_obj_align(emoji_box_, LV_ALIGN_TOP_MID, 0, 0);
        lv_image_set_antialias(emoji_image_, false);
        lv_obj_align(emoji_image_, LV_ALIGN_BOTTOM_MID, 0,
                     -(kEmotionBottomMargin + kEmotionScaleOverscan));
        CreateStaminaHud();
        speech_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* display = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
                if (!display->hide_subtitle_) {
                    display->ShowSpeechPage();
                }
            },
            kSpeechPageDurationMs, this);
        lv_timer_pause(speech_timer_);
        ApplyPetUiStyles();
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    virtual void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        if (stamina_panel_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        ApplyPetUiStyles();
        if (!speech_text_.empty()) {
            speech_offset_ = 0;
            ShowSpeechPage();
        }
    }

    virtual void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        if (stamina_panel_ == nullptr) {
            return;
        }

        int battery_level = 0;
        bool charging = false;
        bool discharging = false;
        if (!Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
            return;
        }
        (void)discharging;

        DisplayLockGuard lock(this);
        UpdateStamina(battery_level, charging);
    }

    virtual void SetEmotion(const char* emotion) override {
        SpiLcdDisplay::SetEmotion(emotion);

        DisplayLockGuard lock(this);
        if (emoji_image_ == nullptr || lv_obj_has_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }

        lv_image_set_scale(emoji_image_, kEmotionImageScale);
        lv_image_set_antialias(emoji_image_, false);
        lv_obj_align(emoji_image_, LV_ALIGN_BOTTOM_MID, 0,
                     -(kEmotionBottomMargin + kEmotionScaleOverscan));
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch16 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button key3_button_;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    esp_timer_handle_t emergency_restart_timer_ = nullptr;
    std::atomic_bool key3_pressed_{false};
    std::atomic_bool boot_pressed_{false};
    std::atomic_bool emergency_restart_timer_running_{false};
    std::atomic_bool emergency_restart_ready_{false};

    void CancelEmergencyRestart() {
        emergency_restart_ready_.store(false);
        if (emergency_restart_timer_running_.exchange(false)) {
            esp_err_t ret = esp_timer_stop(emergency_restart_timer_);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Failed to stop emergency restart timer: %s", esp_err_to_name(ret));
            }
        }
    }

    void OnEmergencyRestartTimer() {
        emergency_restart_timer_running_.store(false);
        if (key3_pressed_.load() && boot_pressed_.load()) {
            emergency_restart_ready_.store(true);
            ESP_LOGW(TAG, "Emergency restart armed; release GPIO18 and BOOT to restart");
        }
    }

    void OnEmergencyButtonPress(bool is_key3) {
        if (is_key3) {
            key3_pressed_.store(true);
        } else {
            boot_pressed_.store(true);
        }

        if (!key3_pressed_.load() || !boot_pressed_.load() ||
            emergency_restart_ready_.load() || emergency_restart_timer_running_.load()) {
            return;
        }

        emergency_restart_timer_running_.store(true);
        esp_err_t ret = esp_timer_start_once(emergency_restart_timer_, 10 * 1000 * 1000);
        if (ret != ESP_OK) {
            emergency_restart_timer_running_.store(false);
            ESP_LOGW(TAG, "Failed to start emergency restart timer: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Emergency restart started; hold GPIO18 and BOOT for 10 seconds");
        }
    }

    void OnEmergencyButtonRelease(bool is_key3) {
        if (is_key3) {
            key3_pressed_.store(false);
        } else {
            boot_pressed_.store(false);
        }

        if (!key3_pressed_.load() && !boot_pressed_.load() &&
            emergency_restart_ready_.exchange(false)) {
            // GPIO0 is the BOOT strap pin. Wait until both buttons are released
            // before restarting, otherwise the chip may enter download mode.
            ESP_LOGW(TAG, "Emergency restart requested");
            esp_restart();
            return;
        }

        // Once the 10-second hold has completed, keep the restart armed while
        // either button is being released. Cancel only before the timer fires.
        if (!emergency_restart_ready_.load() &&
            (!key3_pressed_.load() || !boot_pressed_.load())) {
            CancelEmergencyRestart();
        }
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        esp_timer_create_args_t emergency_restart_timer_args = {
            .callback = [](void* arg) {
                static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(arg)->OnEmergencyRestartTimer();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "emergency_restart",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&emergency_restart_timer_args, &emergency_restart_timer_));

        key3_button_.OnPressDown([this]() { OnEmergencyButtonPress(true); });
        key3_button_.OnPressUp([this]() { OnEmergencyButtonRelease(true); });
        boot_button_.OnPressDown([this]() { OnEmergencyButtonPress(false); });
        boot_button_.OnPressUp([this]() { OnEmergencyButtonRelease(false); });

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    static void OnTouchShortClick(lv_event_t* event) {
        auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(
            lv_event_get_user_data(event));
        auto* indev = static_cast<lv_indev_t*>(lv_event_get_target(event));
        if (board == nullptr || indev == nullptr ||
            lv_indev_get_short_click_streak(indev) != 2) {
            return;
        }

        // LVGL callbacks run on the LVGL task. Run the wake/display work in
        // the application task, just like other cross-task input callbacks.
        Application::GetInstance().Schedule([board]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle) {
                return;
            }

            board->power_save_timer_->WakeUp();
            app.ToggleChatState();
        });
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        auto* touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (touch_indev == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize touch input");
            return;
        }
        // LVGL sends SHORT_CLICKED to the input device before dispatching
        // widget events. The second short click therefore represents a
        // double tap regardless of which UI object is underneath the finger.
        lv_indev_add_event_cb(touch_indev, OnTouchShortClick,
                              LV_EVENT_SHORT_CLICKED, this);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED2inch16()
        : key3_button_(KEY3_BUTTON_GPIO), boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch16);
