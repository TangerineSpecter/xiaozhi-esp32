#include "assets/lang_config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "esp_lcd_co5300.h"
#include "wifi_board.h"

#include "application.h"
#include "axp2101.h"
#include "button.h"
#include "capture_storage.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "i2c_device.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include "qmi8658.h"

#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <dirent.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"
#include "settings_menu.h"

#include <algorithm>
#include <cmath>

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <sys/stat.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#define TAG "WaveshareEsp32s3TouchAMOLED2inch16"

#if WAVESHARE_DIZZY_SOUND_EMBEDDED
extern const char dizzy_ogg_start[] asm("_binary_dizzy_ogg_start");
extern const char dizzy_ogg_end[] asm("_binary_dizzy_ogg_end");
static const std::string_view kDizzySound{dizzy_ogg_start,
                                          static_cast<size_t>(dizzy_ogg_end - dizzy_ogg_start)};
#endif

class Pmic : public Axp2101 {
public:
    // AXP2101 IRQ status 0x49: short press bit 3, long press bit 2.
    static constexpr uint8_t kShortPress = 1 << 3;
    static constexpr uint8_t kLongPress = 1 << 2;

    void InitializeMenuKey() {
        // IRQLEVEL bits [5:4] = 2 means 2 seconds. Preserve power on/off timings.
        WriteReg(0x27, (ReadReg(0x27) & 0xCF) | 0x20);
        WriteReg(0x49, kShortPress | kLongPress);
        WriteReg(0x41, ReadReg(0x41) | kShortPress | kLongPress);
    }

    uint8_t ReadMenuKey() {
        // Poll on a dedicated task: I2C must not block the audio/main/timer tasks.
        uint8_t reg = 0x49;
        uint8_t status = 0;
        if (i2c_master_transmit_receive(i2c_device_, &reg, 1, &status, 1, 20) != ESP_OK) {
            return 0;
        }
        status &= kShortPress | kLongPress;
        if (status != 0) {
            uint8_t clear[] = {reg, status};
            if (i2c_master_transmit(i2c_device_, clear, sizeof(clear), 20) != ESP_OK) {
                return 0;
            }
        }
        return status;
    }

    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);   // hold 4s to power off

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

        WriteReg(0x64, 0x02);  // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02);  // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x0A);  // set Main battery charger current to 400mA ( 0x08-200mA,
                               // 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01);  // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},  // Sleep out

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
    static constexpr lv_coord_t kDizzyBottomOverscan = 8;
    static constexpr lv_coord_t kStatusBubbleWidth = 152;
    static constexpr lv_coord_t kStatusBubbleHeight = 32;
    static constexpr lv_coord_t kStatusBubbleTop = 36;
    static constexpr lv_coord_t kSpeechWidth = 212;
    static constexpr lv_coord_t kSpeechHeight = 82;
    static constexpr lv_coord_t kSpeechTop = 88;
    static constexpr lv_coord_t kSpeechTailHeight = 10;
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

    SettingsMenu menu_;

    lv_obj_t* stamina_panel_ = nullptr;
    lv_obj_t* stamina_icon_ = nullptr;
    lv_obj_t* stamina_label_ = nullptr;
    lv_obj_t* stamina_bar_background_ = nullptr;
    lv_obj_t* stamina_bar_fill_ = nullptr;
    lv_obj_t* charging_label_ = nullptr;
    lv_timer_t* speech_timer_ = nullptr;
    lv_timer_t* capture_timer_ = nullptr;
    lv_obj_t* recording_badge_ = nullptr;
    lv_obj_t* recording_label_ = nullptr;
    lv_obj_t* recording_wave_bars_[3] = {};
    lv_obj_t* notes_badge_ = nullptr;
    lv_obj_t* notes_icon_ = nullptr;
    lv_obj_t* notes_label_ = nullptr;
    std::string speech_text_;
    size_t speech_offset_ = 0;
    bool dizzy_active_ = false;
    bool petting_active_ = false;
    bool recording_active_ = false;
    bool notes_active_ = false;
    int64_t recording_started_us_ = 0;
    std::string pending_emotion_ = "neutral";

    void UpdateCaptureBadges() {
        if (recording_badge_ == nullptr || notes_badge_ == nullptr) {
            return;
        }
        lv_obj_set_flag(recording_badge_, LV_OBJ_FLAG_HIDDEN, !recording_active_);
        lv_obj_set_flag(notes_badge_, LV_OBJ_FLAG_HIDDEN, !notes_active_);
        lv_obj_align(notes_badge_, recording_active_ ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_MID,
                     recording_active_ ? -24 : 0, recording_active_ ? -64 : -18);
        if (recording_active_) {
            int64_t elapsed =
                std::max<int64_t>(0, esp_timer_get_time() - recording_started_us_) / 1000000;
            lv_label_set_text_fmt(recording_label_, "● REC %02lld:%02lld",
                                  static_cast<long long>(elapsed / 60),
                                  static_cast<long long>(elapsed % 60));
            const lv_coord_t wave_heights[3][3] = {{6, 14, 9}, {12, 7, 15}, {9, 15, 6}};
            for (int i = 0; i < 3; ++i) {
                lv_obj_set_height(recording_wave_bars_[i], wave_heights[elapsed % 3][i]);
            }
        }
    }

    void ApplyEmotion(const char* emotion) {
        SpiLcdDisplay::SetEmotion(emotion);

        DisplayLockGuard lock(this);
        if (emoji_image_ == nullptr || lv_obj_has_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }

        lv_image_set_scale(emoji_image_, kEmotionImageScale);
        lv_image_set_antialias(emoji_image_, false);
        // Taller animations reserve room above the head for effects. Keep the
        // same character scale and bottom edge instead of shrinking them to fit.
        const lv_coord_t source_height = lv_image_get_src_height(emoji_image_);
        const lv_coord_t scale_overscan =
            (source_height * kEmotionScaleOverscan + kEmotionSourceSize / 2) / kEmotionSourceSize;
        const lv_coord_t bottom_overscan =
            std::strcmp(emotion, "dizzy") == 0
                ? (kDizzyBottomOverscan * kEmotionImageSize + kEmotionSourceSize / 2) /
                      kEmotionSourceSize
                : 0;
        lv_obj_align(emoji_image_, LV_ALIGN_BOTTOM_MID, 0,
                     -(kEmotionBottomMargin + scale_overscan - bottom_overscan));
    }

    // Draw on the status label itself so notifications hide the dot too.
    static void DrawVoiceStatusDot(lv_event_t* event) {
        auto* label = lv_event_get_target_obj(event);
        const char* status = lv_label_get_text(label);
        if (std::strcmp(status, Lang::Strings::LISTENING) != 0 &&
            std::strcmp(status, Lang::Strings::SPEAKING) != 0) {
            return;
        }
        lv_area_t area;
        lv_obj_get_coords(label, &area);
        const int32_t center_y = (area.y1 + area.y2) / 2;
        lv_area_t dot_area = {area.x1 + 4, center_y - 3, area.x1 + 10, center_y + 3};
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.radius = LV_RADIUS_CIRCLE;
        dot.bg_color = lv_color_hex(0x45C56B);
        dot.bg_opa = LV_OPA_COVER;
        lv_draw_rect(lv_event_get_layer(event), &dot, &dot_area);
    }

    // Draw as part of the bubble so clearing/hiding it also removes the tail.
    static void DrawSpeechTail(lv_event_t* event) {
        if (lv_event_get_code(event) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
            lv_event_set_ext_draw_size(event, kSpeechTailHeight + 2);
            return;
        }
        if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN_END) {
            return;
        }
        auto* bubble = lv_event_get_target_obj(event);
        auto* layer = lv_event_get_layer(event);
        lv_area_t area;
        lv_obj_get_coords(bubble, &area);

        lv_draw_triangle_dsc_t tail;
        lv_draw_triangle_dsc_init(&tail);
        tail.color = lv_obj_get_style_bg_color(bubble, LV_PART_MAIN);
        tail.opa = LV_OPA_COVER;
        // Overlap the bottom border by one pixel to leave an open, seamless base.
        tail.p[0] = {area.x1 + 24, area.y2 - 1};
        tail.p[1] = {area.x1 + 44, area.y2 - 1};
        tail.p[2] = {area.x1 + 20, area.y2 + kSpeechTailHeight};
        lv_draw_triangle(layer, &tail);

        lv_draw_line_dsc_t edge;
        lv_draw_line_dsc_init(&edge);
        edge.color = lv_obj_get_style_border_color(bubble, LV_PART_MAIN);
        edge.width = 1;
        edge.p1 = tail.p[0];
        edge.p2 = tail.p[2];
        lv_draw_line(layer, &edge);
        edge.p1 = tail.p[2];
        edge.p2 = tail.p[1];
        lv_draw_line(layer, &edge);
    }

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
            return lv_color_hex(0x45C56B);
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
            // Keep both voice statuses at the listening text position. The dot
            // is drawn in the label's left-side empty area.
            lv_obj_set_style_pad_left(status_label_, 0, 0);
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
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
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
            lv_obj_set_style_text_color(charging_label_, lv_color_hex(0x45C56B), 0);
            lv_obj_set_style_text_font(charging_label_, lvgl_theme->icon_font()->font(), 0);
        }
        if (notes_icon_ != nullptr) {
            lv_obj_set_style_text_font(notes_icon_, lvgl_theme->icon_font()->font(), 0);
        }
        if (notes_label_ != nullptr) {
            lv_obj_set_style_text_font(notes_label_, lvgl_theme->text_font()->font(), 0);
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
    void ShowMenu(SettingsMenu::Page page, int selected, int volume, bool audio_recording,
                  bool notes_active, const std::vector<SettingsMenu::FileItem>& files,
                  const std::string& preview, bool playing, bool animate = false) {
        DisplayLockGuard lock(this);
        if (current_theme_ == nullptr) {
            return;
        }
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        menu_.Create(display_, theme->text_font()->font(), theme->icon_font()->font());
        menu_.Show(page, selected, volume, audio_recording, notes_active, files, preview, playing,
                   animate);
    }

    SettingsMenu::TouchTarget HitTestMenuTouch(lv_coord_t x, lv_coord_t y) const {
        return menu_.HitTestTouch(x, y);
    }

    void HideMenu() {
        DisplayLockGuard lock(this);
        menu_.Hide();
    }

    ~CustomLcdDisplay() override {
        DisplayLockGuard lock(this);
        menu_.Destroy();
        if (speech_timer_ != nullptr) {
            lv_timer_delete(speech_timer_);
        }
        if (capture_timer_ != nullptr) {
            lv_timer_delete(capture_timer_);
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
        {
            DisplayLockGuard lock(this);
            if (status_label_ != nullptr) {
                // Do not shift the text when switching between listening and
                // speaking; both states share the same fixed layout.
                lv_obj_set_style_pad_left(status_label_, 0, 0);
            }
        }
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
        if (bottom_bar_ != nullptr) {
            lv_obj_add_event_cb(bottom_bar_, DrawSpeechTail, LV_EVENT_DRAW_MAIN_END, nullptr);
            lv_obj_add_event_cb(bottom_bar_, DrawSpeechTail, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
            lv_obj_refresh_ext_draw_size(bottom_bar_);
        }
        if (status_label_ != nullptr) {
            lv_obj_add_event_cb(status_label_, DrawVoiceStatusDot, LV_EVENT_DRAW_MAIN_END, nullptr);
        }
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

        recording_badge_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(recording_badge_, 200, 40);
        lv_obj_align(recording_badge_, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_radius(recording_badge_, 20, 0);
        lv_obj_set_style_bg_color(recording_badge_, lv_color_hex(0x321C3E), 0);
        lv_obj_set_style_bg_opa(recording_badge_, LV_OPA_80, 0);
        lv_obj_set_style_border_color(recording_badge_, lv_color_hex(0xFF5B6E), 0);
        lv_obj_set_style_border_width(recording_badge_, 2, 0);
        lv_obj_remove_flag(recording_badge_, LV_OBJ_FLAG_SCROLLABLE);
        recording_label_ = lv_label_create(recording_badge_);
        lv_obj_align(recording_label_, LV_ALIGN_CENTER, -12, 0);
        lv_obj_set_style_text_color(recording_label_, lv_color_hex(0xFF6B7C), 0);
        for (int i = 0; i < 3; ++i) {
            recording_wave_bars_[i] = lv_obj_create(recording_badge_);
            lv_obj_set_size(recording_wave_bars_[i], 3, 8);
            lv_obj_align(recording_wave_bars_[i], LV_ALIGN_RIGHT_MID, -21 + i * 7, 0);
            lv_obj_set_style_radius(recording_wave_bars_[i], 2, 0);
            lv_obj_set_style_bg_color(recording_wave_bars_[i], lv_color_hex(0xFF6B7C), 0);
            lv_obj_set_style_border_width(recording_wave_bars_[i], 0, 0);
            lv_obj_set_style_pad_all(recording_wave_bars_[i], 0, 0);
            lv_obj_remove_flag(recording_wave_bars_[i], LV_OBJ_FLAG_SCROLLABLE);
        }

        notes_badge_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(notes_badge_, 116, 34);
        lv_obj_set_style_radius(notes_badge_, 17, 0);
        lv_obj_set_style_bg_color(notes_badge_, lv_color_hex(0xF7F0FA), 0);
        lv_obj_set_style_bg_opa(notes_badge_, LV_OPA_90, 0);
        lv_obj_set_style_border_color(notes_badge_, lv_color_hex(0x9C76C6), 0);
        lv_obj_set_style_border_width(notes_badge_, 1, 0);
        lv_obj_remove_flag(notes_badge_, LV_OBJ_FLAG_SCROLLABLE);
        notes_icon_ = lv_label_create(notes_badge_);
        lv_label_set_text(notes_icon_, MATERIAL_SYMBOLS_EDIT_SQUARE);
        lv_obj_align(notes_icon_, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_text_color(notes_icon_, lv_color_hex(0x5D3D78), 0);
        notes_label_ = lv_label_create(notes_badge_);
        lv_label_set_text(notes_label_, "记录中");
        lv_obj_align(notes_label_, LV_ALIGN_CENTER, 13, 0);
        lv_obj_set_style_text_color(notes_label_, lv_color_hex(0x5D3D78), 0);

        capture_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* display = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
                display->UpdateCaptureBadges();
            },
            1000, this);
        UpdateCaptureBadges();
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
        const char* requested = emotion != nullptr && emotion[0] != '\0' ? emotion : "neutral";
        pending_emotion_ = requested;
        if (dizzy_active_ || petting_active_ || recording_active_ || notes_active_) {
            return;
        }
        ApplyEmotion(requested);
    }

    void StartDizzy() {
        if (dizzy_active_ || petting_active_ || recording_active_ || notes_active_) {
            return;
        }
        dizzy_active_ = true;
        ApplyEmotion("dizzy");
    }

    void StopDizzy() {
        if (!dizzy_active_) {
            return;
        }
        dizzy_active_ = false;
        if (recording_active_) {
            ApplyEmotion("recording");
        } else if (notes_active_) {
            ApplyEmotion("taking_notes");
        } else {
            ApplyEmotion(pending_emotion_.empty() ? "neutral" : pending_emotion_.c_str());
        }
    }

    void StartPetting() {
        if (dizzy_active_ || recording_active_ || notes_active_) {
            return;
        }
        petting_active_ = true;
        ApplyEmotion("petting");
    }

    void StopPetting() {
        if (!petting_active_) {
            return;
        }
        petting_active_ = false;
        if (recording_active_) {
            ApplyEmotion("recording");
        } else if (notes_active_) {
            ApplyEmotion("taking_notes");
        } else if (dizzy_active_) {
            ApplyEmotion("dizzy");
        } else {
            ApplyEmotion(pending_emotion_.empty() ? "neutral" : pending_emotion_.c_str());
        }
    }

    bool HitTestCharacterHead(lv_coord_t x, lv_coord_t y) const {
        constexpr lv_coord_t kHeadLeft = DISPLAY_WIDTH * 28 / 100;
        constexpr lv_coord_t kHeadRight = DISPLAY_WIDTH * 72 / 100;
        constexpr lv_coord_t kCharacterTop = DISPLAY_HEIGHT - kEmotionImageSize;
        constexpr lv_coord_t kHeadTop = kCharacterTop + kEmotionImageSize * 4 / 100;
        constexpr lv_coord_t kHeadBottom = kHeadTop + kEmotionImageSize * 43 / 100;
        return x >= kHeadLeft && x <= kHeadRight && y >= kHeadTop && y <= kHeadBottom;
    }

    void SetCaptureState(bool audio_recording, bool text_notes) {
        bool capture_changed = recording_active_ != audio_recording || notes_active_ != text_notes;
        bool recording_started = !recording_active_ && audio_recording;
        recording_active_ = audio_recording;
        notes_active_ = text_notes;
        if (recording_active_ || notes_active_) {
            petting_active_ = false;
        }
        if (recording_started) {
            recording_started_us_ = esp_timer_get_time();
        }
        if (capture_changed) {
            if (recording_active_) {
                ApplyEmotion("recording");
            } else if (notes_active_) {
                ApplyEmotion("taking_notes");
            } else {
                ApplyEmotion(pending_emotion_.empty() ? "neutral" : pending_emotion_.c_str());
            }
        }
        DisplayLockGuard lock(this);
        UpdateCaptureBadges();
    }
};

// Keep an explicitly saved mute across restarts on this board. The common
// codec currently replaces zero with 10 during Start().
class MenuAudioCodec : public BoxAudioCodec {
public:
    using BoxAudioCodec::BoxAudioCodec;

    void Start() override {
        BoxAudioCodec::Start();
        Settings settings("audio", false);
        if (settings.GetInt("output_volume", -1) == 0) {
            output_volume_ = 0;
        }
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
        uint8_t data[1] = {((uint8_t)((255 * brightness) / 100))};
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
    CaptureStorage capture_storage_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    esp_timer_handle_t deep_dim_timer_ = nullptr;
    esp_timer_handle_t petting_timer_ = nullptr;
    esp_timer_handle_t emergency_restart_timer_ = nullptr;
    std::atomic_bool key3_pressed_{false};
    std::atomic_bool boot_pressed_{false};
    std::atomic_bool emergency_restart_timer_running_{false};
    std::atomic_bool emergency_restart_ready_{false};
    enum class ScreenPowerStage { Awake, Dimmed, DeepDimmed };
    std::atomic<ScreenPowerStage> screen_power_stage_{ScreenPowerStage::Awake};
    std::atomic<int64_t> last_touch_tap_us_{0};
    qmi8658_dev_t qmi8658_{};
    std::atomic_bool shake_reaction_active_{false};
    std::atomic_bool petting_reaction_active_{false};
    std::atomic_bool menu_open_{false};
    std::atomic<int64_t> last_dizzy_voice_us_{0};

    enum class MenuPage { Closed, Settings, Volume, Files, AudioFiles, NotesFiles };
    enum class CaptureAction {
        StartAudio,
        StopAudio,
        StartNotes,
        StopNotesVoice,
        StopNotesMenu,
    };
    struct CaptureActionContext {
        WaveshareEsp32s3TouchAMOLED2inch16* board;
        CaptureAction action;
    };
    struct FileTaskContext {
        WaveshareEsp32s3TouchAMOLED2inch16* board;
        MenuPage page;
        std::string path;
        uint32_t generation = 0;
    };
    struct WavInfo {
        long data_offset = 0;
        uint32_t data_size = 0;
        uint32_t sample_rate = 0;
        uint16_t channels = 0;
        uint16_t bits_per_sample = 0;
    };
    MenuPage menu_page_ = MenuPage::Closed;  // Application task only.
    int menu_selection_ = 0;
    int menu_volume_ = 0;
    std::vector<SettingsMenu::FileItem> menu_files_;
    std::vector<std::string> menu_file_paths_;
    std::atomic_bool menu_key_pending_{false};
    std::atomic_bool capture_action_pending_{false};
    std::atomic_bool file_action_pending_{false};
    std::atomic_bool playback_active_{false};
    std::atomic_uint32_t playback_generation_{0};

    static uint16_t ReadLe16(const uint8_t* data) {
        return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    }

    static uint32_t ReadLe32(const uint8_t* data) {
        return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    }

    static bool ReadWavInfo(FILE* file, WavInfo& info) {
        uint8_t header[12];
        if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
            std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
            return false;
        }

        bool found_format = false;
        while (!std::feof(file)) {
            uint8_t chunk[8];
            if (std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
                break;
            }
            const uint32_t size = ReadLe32(chunk + 4);
            if (std::memcmp(chunk, "fmt ", 4) == 0) {
                if (size < 16 || size > 128) {
                    return false;
                }
                std::array<uint8_t, 128> format = {};
                if (std::fread(format.data(), 1, size, file) != size ||
                    ReadLe16(format.data()) != 1) {
                    return false;
                }
                info.channels = ReadLe16(format.data() + 2);
                info.sample_rate = ReadLe32(format.data() + 4);
                info.bits_per_sample = ReadLe16(format.data() + 14);
                found_format = true;
            } else if (std::memcmp(chunk, "data", 4) == 0) {
                info.data_offset = std::ftell(file);
                info.data_size = size;
                return found_format;
            } else if (std::fseek(file, size + (size & 1U), SEEK_CUR) != 0) {
                return false;
            }
        }
        return false;
    }

    static std::string FileDisplayName(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            return path;
        }
        const size_t parent_slash = path.find_last_of('/', slash - 1);
        return parent_slash == std::string::npos ? path.substr(slash + 1)
                                                 : path.substr(parent_slash + 1);
    }

    static std::string TruncateUtf8(std::string text, size_t maximum) {
        if (text.size() <= maximum) {
            return text;
        }
        size_t end = maximum;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
            --end;
        }
        text.resize(end);
        text.append("…");
        return text;
    }

    void StopDeepDimTimer() {
        esp_err_t ret = esp_timer_stop(deep_dim_timer_);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop deep-dim timer: %s", esp_err_to_name(ret));
        }
    }

    bool WakeDisplayIfSleeping() {
        if (screen_power_stage_.load() == ScreenPowerStage::Awake) {
            return false;
        }
        last_touch_tap_us_.store(0);
        power_save_timer_->WakeUp();
        return true;
    }

    bool HasActiveCapture() const {
        return capture_storage_.IsAudioRecording() || capture_storage_.IsTextNotesActive();
    }

    bool IsPettingReactionAllowed() const {
        return screen_power_stage_.load() == ScreenPowerStage::Awake && !menu_open_.load() &&
               !capture_action_pending_.load() && !HasActiveCapture() &&
               !shake_reaction_active_.load();
    }

    void StopPettingTimer() {
        if (petting_timer_ == nullptr) {
            return;
        }
        esp_err_t ret = esp_timer_stop(petting_timer_);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to stop petting timer: %s", esp_err_to_name(ret));
        }
    }

    void StopPettingReaction() {
        StopPettingTimer();
        if (petting_reaction_active_.exchange(false)) {
            display_->StopPetting();
        }
    }

    void StartPettingReaction() {
        if (!IsPettingReactionAllowed()) {
            StopPettingReaction();
            return;
        }
        power_save_timer_->WakeUp();
        petting_reaction_active_.store(true);
        display_->StartPetting();
        StopPettingTimer();
        esp_err_t ret = esp_timer_start_once(petting_timer_, 980 * 1000);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start petting timer: %s", esp_err_to_name(ret));
            StopPettingReaction();
        }
    }

    void InitializePettingReaction() {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(arg);
                    Application::GetInstance().Schedule(
                        [board]() { board->StopPettingReaction(); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "petting_reaction",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &petting_timer_));
    }

    bool IsCaptureBusy() const { return capture_action_pending_.load() || HasActiveCapture(); }

    void UpdatePowerSavePolicy() {
        power_save_timer_->SetEnabled(pmic_->IsDischarging() && !HasActiveCapture() &&
                                      !playback_active_.load());
    }

    static void CollectFiles(const std::string& directory, const char* extension,
                             std::vector<std::string>& paths) {
        DIR* dir = opendir(directory.c_str());
        if (dir == nullptr) {
            return;
        }
        while (auto* entry = readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            const std::string path = directory + "/" + entry->d_name;
            struct stat status = {};
            if (stat(path.c_str(), &status) != 0) {
                continue;
            }
            if (S_ISDIR(status.st_mode)) {
                CollectFiles(path, extension, paths);
            } else if (S_ISREG(status.st_mode)) {
                const size_t path_length = path.size();
                const size_t extension_length = std::strlen(extension);
                if (path_length >= extension_length &&
                    path.compare(path_length - extension_length, extension_length, extension) ==
                        0) {
                    paths.push_back(path);
                }
            }
        }
        closedir(dir);
    }

    static std::string ReadNotePreview(const std::string& path) {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            return "无法读取记录";
        }
        std::array<char, 513> buffer = {};
        const size_t length = std::fread(buffer.data(), 1, buffer.size() - 1, file);
        std::fclose(file);
        std::string text(buffer.data(), length);
        return text.empty() ? "（空记录）" : TruncateUtf8(std::move(text), 480);
    }

    static std::string GetFileDetail(const std::string& path, MenuPage page) {
        struct stat status = {};
        if (stat(path.c_str(), &status) != 0) {
            return "无法读取";
        }
        if (page == MenuPage::AudioFiles) {
            FILE* file = std::fopen(path.c_str(), "rb");
            WavInfo info;
            const bool valid = file != nullptr && ReadWavInfo(file, info) && info.channels == 1 &&
                               info.bits_per_sample == 16 && info.sample_rate == 16000;
            if (file != nullptr) {
                std::fclose(file);
            }
            if (!valid) {
                return "不支持的 WAV";
            }
            const uint32_t seconds = info.data_size / (info.sample_rate * 2U);
            char detail[32];
            std::snprintf(detail, sizeof(detail), "%02lu:%02lu · 16 kHz",
                          static_cast<unsigned long>(seconds / 60),
                          static_cast<unsigned long>(seconds % 60));
            return detail;
        }
        char detail[32];
        std::snprintf(detail, sizeof(detail), "%ld 字节", static_cast<long>(status.st_size));
        return detail;
    }

    void CompleteFileLoad(MenuPage page, std::vector<SettingsMenu::FileItem> files,
                          std::vector<std::string> paths, std::string error) {
        file_action_pending_.store(false);
        if (!error.empty()) {
            display_->ShowNotification(error.c_str());
            return;
        }
        if (menu_page_ != page) {
            return;
        }
        menu_files_ = std::move(files);
        menu_file_paths_ = std::move(paths);
        menu_selection_ = 0;
        RefreshMenu();
    }

    void LoadFiles(MenuPage page) {
        if (file_action_pending_.exchange(true)) {
            display_->ShowNotification("正在读取 TF 卡");
            return;
        }
        menu_page_ = page;
        menu_selection_ = 0;
        menu_files_.clear();
        menu_file_paths_.clear();
        RefreshMenu();

        auto* context = new (std::nothrow) FileTaskContext{this, page, {}, 0};
        if (context == nullptr) {
            file_action_pending_.store(false);
            display_->ShowNotification("内存不足，无法读取文件");
            return;
        }
        BaseType_t created = xTaskCreate(
            [](void* argument) {
                auto* context = static_cast<FileTaskContext*>(argument);
                auto* board = context->board;
                const MenuPage page = context->page;
                delete context;

                std::vector<SettingsMenu::FileItem> files;
                std::vector<std::string> paths;
                std::string error;
                CaptureResult mounted = board->capture_storage_.Initialize();
                if (!mounted.ok) {
                    error = mounted.message;
                } else {
                    DIR* mount_directory = opendir("/sdcard");
                    if (mount_directory == nullptr) {
                        error = "TF 卡访问失败，请重新插卡";
                    } else {
                        closedir(mount_directory);
                        const char* category = page == MenuPage::AudioFiles ? "audio" : "notes";
                        const char* extension = page == MenuPage::AudioFiles ? ".wav" : ".txt";
                        std::vector<std::string> all_paths;
                        CollectFiles(std::string("/sdcard/xiaozhi/") + category, extension,
                                     all_paths);
                        std::sort(all_paths.begin(), all_paths.end(), std::greater<std::string>());
                        if (all_paths.size() > 3) {
                            all_paths.resize(3);
                        }
                        for (const auto& path : all_paths) {
                            SettingsMenu::FileItem item;
                            item.name = FileDisplayName(path);
                            item.detail = GetFileDetail(path, page);
                            if (page == MenuPage::NotesFiles) {
                                item.preview = ReadNotePreview(path);
                            }
                            files.push_back(std::move(item));
                            paths.push_back(path);
                        }
                    }
                }
                Application::GetInstance().Schedule([board, page, files = std::move(files),
                                                     paths = std::move(paths),
                                                     error = std::move(error)]() mutable {
                    board->CompleteFileLoad(page, std::move(files), std::move(paths),
                                            std::move(error));
                });
                vTaskDelete(nullptr);
            },
            "sd_file_list", 6144, context, 2, nullptr);
        if (created != pdPASS) {
            delete context;
            file_action_pending_.store(false);
            display_->ShowNotification("无法创建文件读取任务");
        }
    }

    void CompletePlayback(uint32_t generation, std::string message) {
        if (generation != playback_generation_.load()) {
            return;
        }
        playback_active_.store(false);
        UpdatePowerSavePolicy();
        if (!message.empty()) {
            display_->ShowNotification(message.c_str());
        }
        if (menu_page_ == MenuPage::AudioFiles) {
            RefreshMenu();
        }
    }

    void StopAudioPlayback(bool refresh = true) {
        if (!playback_active_.exchange(false)) {
            return;
        }
        playback_generation_.fetch_add(1);
        Application::GetInstance().GetAudioService().ResetDecoder();
        UpdatePowerSavePolicy();
        if (refresh && menu_page_ == MenuPage::AudioFiles) {
            RefreshMenu();
        }
    }

    void StartAudioPlayback() {
        if (menu_selection_ < 0 || menu_selection_ >= static_cast<int>(menu_file_paths_.size())) {
            display_->ShowNotification("没有可播放的录音");
            return;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            display_->ShowNotification("请先结束当前对话");
            return;
        }
        if (playback_active_.load()) {
            StopAudioPlayback();
            return;
        }

        const uint32_t generation = playback_generation_.fetch_add(1) + 1;
        auto* context = new (std::nothrow) FileTaskContext{
            this, MenuPage::AudioFiles, menu_file_paths_[menu_selection_], generation};
        if (context == nullptr) {
            display_->ShowNotification("内存不足，无法播放录音");
            return;
        }
        playback_active_.store(true);
        power_save_timer_->SetEnabled(false);
        RefreshMenu();
        BaseType_t created = xTaskCreate(
            [](void* argument) {
                auto* context = static_cast<FileTaskContext*>(argument);
                auto* board = context->board;
                const std::string path = std::move(context->path);
                const uint32_t generation = context->generation;
                delete context;

                std::string error;
                FILE* file = std::fopen(path.c_str(), "rb");
                WavInfo info;
                if (file == nullptr || !ReadWavInfo(file, info) || info.channels != 1 ||
                    info.bits_per_sample != 16 || info.sample_rate != 16000) {
                    error = "录音格式不受支持";
                } else if (std::fseek(file, info.data_offset, SEEK_SET) != 0) {
                    error = "无法读取录音";
                } else {
                    auto& audio_service = Application::GetInstance().GetAudioService();
                    std::array<int16_t, 640> input = {};
                    uint32_t remaining = info.data_size;
                    while (remaining > 0 && generation == board->playback_generation_.load()) {
                        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                            audio_service.ResetDecoder();
                            error = "播放已中断";
                            break;
                        }
                        const size_t requested =
                            std::min<size_t>(input.size(), remaining / sizeof(int16_t));
                        const size_t read =
                            std::fread(input.data(), sizeof(int16_t), requested, file);
                        if (read == 0) {
                            if (std::ferror(file)) {
                                error = "读取录音失败";
                            }
                            break;
                        }
                        remaining -= read * sizeof(int16_t);
                        std::vector<int16_t> output;
                        output.reserve((read * 3 + 1) / 2);
                        size_t i = 0;
                        for (; i + 1 < read; i += 2) {
                            output.push_back(input[i]);
                            output.push_back(static_cast<int16_t>(
                                (static_cast<int32_t>(input[i]) + input[i + 1]) / 2));
                            output.push_back(input[i + 1]);
                        }
                        if (i < read) {
                            output.push_back(input[i]);
                        }
                        if (!audio_service.PushPcmToPlaybackQueue(std::move(output), true)) {
                            if (generation == board->playback_generation_.load()) {
                                error = "播放已中断";
                            }
                            break;
                        }
                    }
                    while (error.empty() && generation == board->playback_generation_.load() &&
                           !audio_service.IsPlaybackIdle()) {
                        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                            audio_service.ResetDecoder();
                            error = "播放已中断";
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                }
                if (file != nullptr) {
                    std::fclose(file);
                }
                Application::GetInstance().Schedule(
                    [board, generation, error = std::move(error)]() mutable {
                        board->CompletePlayback(generation, std::move(error));
                    });
                vTaskDelete(nullptr);
            },
            "wav_playback", 6144, context, 2, nullptr);
        if (created != pdPASS) {
            delete context;
            playback_active_.store(false);
            UpdatePowerSavePolicy();
            display_->ShowNotification("无法创建播放任务");
            RefreshMenu();
        }
    }

    void CloseMenu() {
        StopAudioPlayback(false);
        menu_page_ = MenuPage::Closed;
        menu_open_.store(false);
        display_->HideMenu();
    }

    void RefreshMenu(bool animate = false) {
        StopPettingReaction();
        menu_open_.store(true);
        SettingsMenu::Page page = SettingsMenu::Page::Settings;
        if (menu_page_ == MenuPage::Volume) {
            page = SettingsMenu::Page::Volume;
        } else if (menu_page_ == MenuPage::Files) {
            page = SettingsMenu::Page::Files;
        } else if (menu_page_ == MenuPage::AudioFiles) {
            page = SettingsMenu::Page::AudioFiles;
        } else if (menu_page_ == MenuPage::NotesFiles) {
            page = SettingsMenu::Page::NotesFiles;
        }
        const std::string preview = menu_page_ == MenuPage::NotesFiles && menu_selection_ >= 0 &&
                                            menu_selection_ < static_cast<int>(menu_files_.size())
                                        ? menu_files_[menu_selection_].preview
                                        : std::string();
        display_->ShowMenu(page, menu_selection_, menu_volume_, capture_storage_.IsAudioRecording(),
                           capture_storage_.IsTextNotesActive(), menu_files_, preview,
                           playback_active_.load(), animate);
    }

    void UpdateCaptureUi() {
        const bool audio = capture_storage_.IsAudioRecording();
        const bool notes = capture_storage_.IsTextNotesActive();
        display_->SetCaptureState(audio, notes);
        if (audio || notes) {
            CloseMenu();
        } else if (menu_page_ != MenuPage::Closed) {
            RefreshMenu();
        }
        if (audio || notes) {
            power_save_timer_->SetEnabled(false);
        } else {
            UpdatePowerSavePolicy();
        }
    }

    void CompleteCaptureAction(CaptureAction action, CaptureResult result) {
        (void)action;
        capture_action_pending_.store(false);
        UpdateCaptureUi();
        display_->ShowNotification(result.message.c_str());
    }

    std::string RequestCaptureAction(CaptureAction action) {
        if (action == CaptureAction::StartAudio && capture_storage_.IsTextNotesActive()) {
            return "正在文字记录，请先结束记录";
        }
        if (action == CaptureAction::StartNotes && capture_storage_.IsAudioRecording()) {
            return "正在录音，请先结束录音";
        }
        if (capture_action_pending_.exchange(true)) {
            return "存储操作正在处理中";
        }

        auto* context = new (std::nothrow) CaptureActionContext{this, action};
        if (context == nullptr) {
            capture_action_pending_.store(false);
            return "内存不足，无法启动操作";
        }
        BaseType_t created = xTaskCreate(
            [](void* argument) {
                auto* context = static_cast<CaptureActionContext*>(argument);
                auto* board = context->board;
                const CaptureAction action = context->action;
                auto& audio_service = Application::GetInstance().GetAudioService();
                CaptureResult result;
                switch (action) {
                    case CaptureAction::StartAudio:
                        result = board->capture_storage_.StartAudioRecording();
                        if (result.ok) {
                            audio_service.EnableLocalCapture(true);
                        }
                        break;
                    case CaptureAction::StopAudio:
                        audio_service.EnableLocalCapture(false);
                        result = board->capture_storage_.StopAudioRecording();
                        break;
                    case CaptureAction::StartNotes:
                        result = board->capture_storage_.StartTextNotes();
                        break;
                    case CaptureAction::StopNotesVoice:
                        result = board->capture_storage_.StopTextNotes(true);
                        break;
                    case CaptureAction::StopNotesMenu:
                        result = board->capture_storage_.StopTextNotes(false);
                        break;
                }
                delete context;
                Application::GetInstance().Schedule(
                    [board, action, result = std::move(result)]() mutable {
                        board->CompleteCaptureAction(action, std::move(result));
                    });
                vTaskDelete(nullptr);
            },
            "capture_control", 4096, context, 2, nullptr);
        if (created != pdPASS) {
            delete context;
            capture_action_pending_.store(false);
            return "无法创建存储操作任务";
        }
        return "操作已受理，请查看屏幕状态";
    }

    void StopActiveCapture() {
        if (capture_storage_.IsAudioRecording()) {
            RequestCaptureAction(CaptureAction::StopAudio);
        } else if (capture_storage_.IsTextNotesActive()) {
            RequestCaptureAction(CaptureAction::StopNotesMenu);
        }
    }

    void HandleAudioRecordingShortcut() {
        // GPIO18 is also part of the emergency-restart chord. Do not start or stop a
        // recording while BOOT is held with it.
        if (boot_pressed_.load()) {
            return;
        }
        if (WakeDisplayIfSleeping()) {
            return;
        }
        if (menu_page_ != MenuPage::Closed) {
            return;
        }

        power_save_timer_->WakeUp();
        if (capture_action_pending_.load()) {
            display_->ShowNotification("存储操作正在处理中");
            return;
        }
        if (capture_storage_.IsTextNotesActive()) {
            display_->ShowNotification("正在文字记录，请先结束记录");
            return;
        }

        StopPettingReaction();
        const std::string result =
            RequestCaptureAction(capture_storage_.IsAudioRecording() ? CaptureAction::StopAudio
                                                                     : CaptureAction::StartAudio);
        if (!capture_action_pending_.load()) {
            display_->ShowNotification(result.c_str());
        }
    }

    void HandleMenuKey(bool back) {
        if (IsCaptureBusy()) {
            power_save_timer_->WakeUp();
            if (HasActiveCapture()) {
                StopActiveCapture();
            }
            return;
        }
        if (WakeDisplayIfSleeping()) {
            return;
        }
        power_save_timer_->WakeUp();
        if (back) {
            if (menu_page_ == MenuPage::Volume) {
                menu_page_ = MenuPage::Settings;
                RefreshMenu();
            } else if (menu_page_ == MenuPage::AudioFiles || menu_page_ == MenuPage::NotesFiles) {
                StopAudioPlayback(false);
                menu_page_ = MenuPage::Files;
                menu_selection_ = 0;
                menu_files_.clear();
                menu_file_paths_.clear();
                RefreshMenu();
            } else if (menu_page_ == MenuPage::Files) {
                menu_page_ = MenuPage::Settings;
                menu_selection_ = 3;
                RefreshMenu();
            } else if (menu_page_ == MenuPage::Settings) {
                CloseMenu();
            }
            return;
        }
        if (menu_page_ == MenuPage::Closed) {
            menu_page_ = MenuPage::Settings;
            menu_selection_ = 0;
            RefreshMenu(true);
        } else if (menu_page_ == MenuPage::Volume) {
            GetAudioCodec()->SetOutputVolume(menu_volume_);
            menu_page_ = MenuPage::Settings;
            RefreshMenu();
        } else if (menu_page_ == MenuPage::Settings) {
            switch (menu_selection_) {
                case 0:
                    menu_volume_ = std::clamp(GetAudioCodec()->output_volume(), 0, 100);
                    menu_page_ = MenuPage::Volume;
                    RefreshMenu();
                    break;
                case 1:
                    CloseMenu();
                    RequestCaptureAction(capture_storage_.IsAudioRecording()
                                             ? CaptureAction::StopAudio
                                             : CaptureAction::StartAudio);
                    break;
                case 2:
                    CloseMenu();
                    RequestCaptureAction(capture_storage_.IsTextNotesActive()
                                             ? CaptureAction::StopNotesMenu
                                             : CaptureAction::StartNotes);
                    break;
                default:
                    menu_page_ = MenuPage::Files;
                    menu_selection_ = 0;
                    menu_files_.clear();
                    menu_file_paths_.clear();
                    RefreshMenu();
                    break;
            }
        } else if (menu_page_ == MenuPage::Files) {
            LoadFiles(menu_selection_ == 0 ? MenuPage::AudioFiles : MenuPage::NotesFiles);
        } else if (menu_page_ == MenuPage::AudioFiles) {
            StartAudioPlayback();
        } else if (menu_page_ == MenuPage::NotesFiles) {
            if (menu_files_.empty()) {
                display_->ShowNotification("没有可查看的记录");
            }
        }
    }

    void HandleMenuTouch(const SettingsMenu::TouchTarget& target) {
        if (!menu_open_.load()) {
            return;
        }
        if (target.close) {
            CloseMenu();
            return;
        }
        if (target.back) {
            HandleMenuKey(true);
            return;
        }
        if (target.outside) {
            HandleMenuKey(true);
            return;
        }
        if (menu_page_ == MenuPage::Volume && target.volume_bar) {
            menu_volume_ = target.volume;
            RefreshMenu();
            return;
        }
        if (menu_page_ == MenuPage::Settings && target.menu_index >= 0) {
            menu_selection_ = target.menu_index;
            HandleMenuKey(false);
        } else if (menu_page_ == MenuPage::Files && target.menu_index >= 0) {
            menu_selection_ = target.menu_index;
            HandleMenuKey(false);
        } else if ((menu_page_ == MenuPage::AudioFiles || menu_page_ == MenuPage::NotesFiles) &&
                   target.file_index >= 0 &&
                   target.file_index < static_cast<int>(menu_files_.size())) {
            if (menu_page_ == MenuPage::AudioFiles && menu_selection_ != target.file_index) {
                StopAudioPlayback(false);
            }
            menu_selection_ = target.file_index;
            RefreshMenu();
        }
    }

    bool IsShakeReactionAllowed() const {
        if (screen_power_stage_.load() != ScreenPowerStage::Awake || menu_open_.load() ||
            capture_storage_.IsAudioRecording() || capture_storage_.IsTextNotesActive() ||
            petting_reaction_active_.load()) {
            return false;
        }
        auto state = Application::GetInstance().GetDeviceState();
        return state == kDeviceStateIdle || state == kDeviceStateListening ||
               state == kDeviceStateSpeaking;
    }

    void StartShakeReaction() {
        if (!IsShakeReactionAllowed()) {
            shake_reaction_active_.store(false);
            return;
        }

        power_save_timer_->WakeUp();
        display_->StartDizzy();

#if WAVESHARE_DIZZY_SOUND_EMBEDDED
        constexpr int64_t kVoiceCooldownUs = 5 * 1000 * 1000;
        int64_t now = esp_timer_get_time();
        if (now - last_dizzy_voice_us_.load() >= kVoiceCooldownUs &&
            Application::GetInstance().TryPlayLocalReactionSound(kDizzySound)) {
            last_dizzy_voice_us_.store(now);
        }
#endif
    }

    void StopShakeReaction() { display_->StopDizzy(); }

    void InitializeMotionSensor() {
        esp_err_t ret = qmi8658_init(&qmi8658_, i2c_bus_, QMI8658_ADDRESS_HIGH);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "QMI8658 unavailable; shake reaction disabled: %s", esp_err_to_name(ret));
            return;
        }
        if ((ret = qmi8658_set_accel_range(&qmi8658_, QMI8658_ACCEL_RANGE_4G)) != ESP_OK ||
            (ret = qmi8658_set_accel_odr(&qmi8658_, QMI8658_ACCEL_ODR_125HZ)) != ESP_OK ||
            (ret = qmi8658_enable_sensors(&qmi8658_, QMI8658_ENABLE_ACCEL)) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure QMI8658; shake reaction disabled: %s",
                     esp_err_to_name(ret));
            return;
        }
        qmi8658_set_accel_unit_mg(&qmi8658_, true);

        BaseType_t result = xTaskCreate(
            [](void* argument) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(argument);
                constexpr float kGravityAlpha = 0.90f;
                constexpr float kShakeThresholdMg = 900.0f;
                constexpr int64_t kPeakSpacingUs = 80 * 1000;
                constexpr int64_t kPeakWindowUs = 600 * 1000;
                constexpr int64_t kRecoveryUs = 2000 * 1000;

                float gravity[3] = {};
                float previous_peak[3] = {};
                float previous_peak_magnitude = 0.0f;
                bool gravity_initialized = false;
                bool have_previous_peak = false;
                int peak_count = 0;
                int64_t peak_window_start_us = 0;
                int64_t last_peak_us = 0;
                int64_t last_motion_us = 0;

                for (;;) {
                    if (!board->IsShakeReactionAllowed()) {
                        gravity_initialized = false;
                        have_previous_peak = false;
                        peak_count = 0;
                        if (board->shake_reaction_active_.exchange(false)) {
                            Application::GetInstance().Schedule(
                                [board]() { board->StopShakeReaction(); });
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }

                    float acceleration[3] = {};
                    esp_err_t read_ret = qmi8658_read_accel(&board->qmi8658_, &acceleration[0],
                                                            &acceleration[1], &acceleration[2]);
                    if (read_ret != ESP_OK) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }

                    if (!gravity_initialized) {
                        for (int i = 0; i < 3; ++i) {
                            gravity[i] = acceleration[i];
                        }
                        gravity_initialized = true;
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }

                    float linear[3];
                    float magnitude_squared = 0.0f;
                    for (int i = 0; i < 3; ++i) {
                        gravity[i] =
                            kGravityAlpha * gravity[i] + (1.0f - kGravityAlpha) * acceleration[i];
                        linear[i] = acceleration[i] - gravity[i];
                        magnitude_squared += linear[i] * linear[i];
                    }
                    float magnitude = std::sqrt(magnitude_squared);
                    int64_t now = esp_timer_get_time();

                    if (board->shake_reaction_active_.load()) {
                        if (magnitude >= kShakeThresholdMg &&
                            now - last_peak_us >= kPeakSpacingUs) {
                            last_peak_us = now;
                            last_motion_us = now;
                        }
                        if (now - last_motion_us >= kRecoveryUs &&
                            board->shake_reaction_active_.exchange(false)) {
                            Application::GetInstance().Schedule(
                                [board]() { board->StopShakeReaction(); });
                        }
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }

                    if (magnitude >= kShakeThresholdMg && now - last_peak_us >= kPeakSpacingUs) {
                        if (peak_count == 0 || now - peak_window_start_us > kPeakWindowUs) {
                            peak_count = 0;
                            have_previous_peak = false;
                            peak_window_start_us = now;
                        }

                        float dot = linear[0] * previous_peak[0] + linear[1] * previous_peak[1] +
                                    linear[2] * previous_peak[2];
                        bool direction_alternates =
                            !have_previous_peak ||
                            dot < -0.15f * magnitude * previous_peak_magnitude;
                        if (direction_alternates) {
                            ++peak_count;
                            for (int i = 0; i < 3; ++i) {
                                previous_peak[i] = linear[i];
                            }
                            previous_peak_magnitude = magnitude;
                            have_previous_peak = true;
                            last_peak_us = now;
                        }

                        if (peak_count >= 3) {
                            peak_count = 0;
                            have_previous_peak = false;
                            last_motion_us = now;
                            board->shake_reaction_active_.store(true);
                            Application::GetInstance().Schedule(
                                [board]() { board->StartShakeReaction(); });
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            },
            "shake_sensor", 4096, this, 2, nullptr);
        if (result != pdPASS) {
            ESP_LOGW(TAG, "Failed to create shake sensor task");
            qmi8658_enable_sensors(&qmi8658_, QMI8658_DISABLE_ALL);
        } else {
            ESP_LOGI(TAG, "QMI8658 shake reaction initialized");
        }
    }

    bool HandleMenuDirection(int direction) {
        power_save_timer_->WakeUp();
        if (IsCaptureBusy() || file_action_pending_.load()) {
            return true;
        }
        if (menu_page_ == MenuPage::Closed) {
            return false;
        }
        if (menu_page_ == MenuPage::Volume) {
            menu_volume_ = std::clamp(menu_volume_ + direction * 5, 0, 100);
        } else if (menu_page_ == MenuPage::Files) {
            menu_selection_ = (menu_selection_ + 1) % 2;
        } else if (menu_page_ == MenuPage::AudioFiles || menu_page_ == MenuPage::NotesFiles) {
            if (menu_files_.empty()) {
                return true;
            }
            if (menu_page_ == MenuPage::AudioFiles) {
                StopAudioPlayback(false);
            }
            const int count = static_cast<int>(menu_files_.size());
            menu_selection_ = (menu_selection_ + (direction > 0 ? 1 : count - 1)) % count;
        } else {
            menu_selection_ = (menu_selection_ + (direction > 0 ? 1 : 3)) % 4;
        }
        RefreshMenu();
        return true;
    }

    void InitializeMenuKey() {
        pmic_->InitializeMenuKey();
        BaseType_t result = xTaskCreate(
            [](void* argument) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(argument);
                for (;;) {
                    // Leave latched events in the PMIC while the application is busy.
                    // At most one polling callback may be queued at a time.
                    if (!board->menu_key_pending_.load()) {
                        uint8_t status = board->pmic_->ReadMenuKey();
                        if (status != 0) {
                            board->menu_key_pending_.store(true);
                            Application::GetInstance().Schedule([board, status]() {
                                board->HandleMenuKey((status & Pmic::kLongPress) != 0);
                                board->menu_key_pending_.store(false);
                            });
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            },
            "menu_key", 3072, this, 2, nullptr);
        ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }

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

        if (!key3_pressed_.load() || !boot_pressed_.load() || emergency_restart_ready_.load() ||
            emergency_restart_timer_running_.load()) {
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
        if (!emergency_restart_ready_.load() && (!key3_pressed_.load() || !boot_pressed_.load())) {
            CancelEmergencyRestart();
        }
    }

    void InitializePowerSaveTimer() {
        esp_timer_create_args_t deep_dim_timer_args = {
            .callback =
                [](void* arg) {
                    auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(arg);
                    if (board->screen_power_stage_.load() != ScreenPowerStage::Dimmed) {
                        return;
                    }
                    board->screen_power_stage_.store(ScreenPowerStage::DeepDimmed);
                    board->last_touch_tap_us_.store(0);
                    board->GetBacklight()->SetBrightness(5);
                    ESP_LOGI(TAG, "Display deeply dimmed; double tap to wake");
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "display_deep_dim",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&deep_dim_timer_args, &deep_dim_timer_));

        power_save_timer_ = new PowerSaveTimer(-1, 60, 600);
        power_save_timer_->OnEnterSleepMode([this]() {
            screen_power_stage_.store(ScreenPowerStage::Dimmed);
            last_touch_tap_us_.store(0);
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
            esp_err_t ret = esp_timer_start_once(deep_dim_timer_, 240 * 1000 * 1000);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start deep-dim timer: %s", esp_err_to_name(ret));
            }
        });
        power_save_timer_->OnExitSleepMode([this]() {
            StopDeepDimTimer();
            screen_power_stage_.store(ScreenPowerStage::Awake);
            last_touch_tap_us_.store(0);
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags =
                {
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
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        esp_timer_create_args_t emergency_restart_timer_args = {
            .callback =
                [](void* arg) {
                    static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(arg)
                        ->OnEmergencyRestartTimer();
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

        key3_button_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (!WakeDisplayIfSleeping()) {
                    HandleMenuDirection(1);
                }
            });
        });
        key3_button_.OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (WakeDisplayIfSleeping()) {
                    return;
                }
                HandleMenuDirection(1);
                HandleMenuDirection(1);
            });
        });
        key3_button_.OnLongPress([this]() {
            Application::GetInstance().Schedule([this]() { HandleAudioRecordingShortcut(); });
        });
        boot_button_.OnClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsCaptureBusy()) {
                    power_save_timer_->WakeUp();
                    if (HasActiveCapture()) {
                        StopActiveCapture();
                    }
                    return;
                }
                if (WakeDisplayIfSleeping()) {
                    return;
                }
                if (HandleMenuDirection(-1)) {
                    return;
                }
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            });
        });

        boot_button_.OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (IsCaptureBusy()) {
                    power_save_timer_->WakeUp();
                    if (HasActiveCapture()) {
                        StopActiveCapture();
                    }
                    return;
                }
                if (WakeDisplayIfSleeping()) {
                    return;
                }
                // A double click in a menu is two downward selections.
                if (menu_page_ != MenuPage::Closed) {
                    HandleMenuDirection(-1);
                    HandleMenuDirection(-1);
                    return;
                }
#if CONFIG_USE_DEVICE_AEC
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
                }
#endif
            });
        });
        InitializeMenuKey();
    }

    static void OnTouchShortClick(lv_event_t* event) {
        auto* board =
            static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(lv_event_get_user_data(event));
        if (board == nullptr) {
            return;
        }

        auto stage = board->screen_power_stage_.load();
        if (stage == ScreenPowerStage::Awake) {
            board->last_touch_tap_us_.store(0);
            lv_indev_t* indev = lv_indev_get_act();
            if (indev == nullptr) {
                return;
            }
            lv_point_t point = {};
            lv_indev_get_point(indev, &point);
            if (board->menu_open_.load()) {
                const auto target = board->display_->HitTestMenuTouch(point.x, point.y);
                Application::GetInstance().Schedule(
                    [board, target]() { board->HandleMenuTouch(target); });
            } else if (board->display_->HitTestCharacterHead(point.x, point.y)) {
                Application::GetInstance().Schedule([board]() { board->StartPettingReaction(); });
            }
            return;
        }

        bool should_wake = stage == ScreenPowerStage::Dimmed;
        if (stage == ScreenPowerStage::DeepDimmed) {
            constexpr int64_t kDoubleTapWindowUs = 700 * 1000;
            int64_t now = esp_timer_get_time();
            int64_t previous = board->last_touch_tap_us_.exchange(now);
            should_wake = previous > 0 && now - previous <= kDoubleTapWindowUs;
        }
        if (!should_wake) {
            return;
        }

        // LVGL callbacks run on the LVGL task. Wake on the application task
        // and consume the gesture instead of also starting a conversation.
        Application::GetInstance().Schedule([board]() { board->WakeDisplayIfSleeping(); });
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
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
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
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = 0,
                    .mirror_x = 1,
                    .mirror_y = 1,
                },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400 * 1000;
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
        // While lightly dimmed, one short tap wakes the display. In the
        // deep-dim stage, the callback applies a relaxed time-only double-tap
        // detector so the two taps do not need to land within LVGL's 10 px
        // default streak radius.
        lv_indev_add_event_cb(touch_indev, OnTouchShortClick, LV_EVENT_SHORT_CLICKED, this);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    void InitializeCaptureStorage() {
        auto& audio_service = Application::GetInstance().GetAudioService();
        audio_service.SetLocalCaptureCallback(
            [this](const int16_t* samples, size_t sample_count, int channels) {
                capture_storage_.PushPcm(samples, sample_count, channels);
            });
        capture_storage_.SetErrorCallback([this](const std::string& message) {
            Application::GetInstance().Schedule([this, message]() {
                if (!capture_storage_.IsAudioRecording()) {
                    Application::GetInstance().GetAudioService().EnableLocalCapture(false);
                }
                UpdateCaptureUi();
                display_->ShowNotification(message.c_str());
            });
        });

        BaseType_t created = xTaskCreate(
            [](void* argument) {
                auto* board = static_cast<WaveshareEsp32s3TouchAMOLED2inch16*>(argument);
                CaptureResult result = board->capture_storage_.Initialize();
                if (!result.ok) {
                    ESP_LOGW(TAG, "%s", result.message.c_str());
                }
                vTaskDelete(nullptr);
            },
            "sd_mount", 4096, this, 2, nullptr);
        if (created != pdPASS) {
            ESP_LOGW(TAG, "Failed to create initial TF mount task");
        }
    }

    // 初始化工具
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.audio_recording.start",
            "Start local microphone recording to the TF card. Use for Chinese requests such as "
            "'帮我录音' or '开始录音'. The device UI confirms recording; after calling this tool, "
            "do not speak a confirmation because speaker audio may enter the recording.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (capture_storage_.IsAudioRecording()) {
                    return std::string("已经在录音");
                }
                return RequestCaptureAction(CaptureAction::StartAudio);
            });
        mcp_server.AddTool(
            "self.audio_recording.stop",
            "Stop and save the current TF-card audio recording. Use for Chinese requests such as "
            "'停止录音' or '保存录音'.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (!capture_storage_.IsAudioRecording()) {
                    return std::string("当前没有录音");
                }
                return RequestCaptureAction(CaptureAction::StopAudio);
            });
        mcp_server.AddTool(
            "self.text_notes.start",
            "Start saving final user speech-to-text results to a UTF-8 text file on the TF card. "
            "Use for Chinese requests such as '帮我记录一下' or '开始记录'.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (capture_storage_.IsTextNotesActive()) {
                    return std::string("已经在记录文字");
                }
                return RequestCaptureAction(CaptureAction::StartNotes);
            });
        mcp_server.AddTool(
            "self.text_notes.stop",
            "Stop and save TF-card text notes. Use for Chinese requests such as '停止记录'. The "
            "last STT line that triggered this tool is removed from the notes file.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (!capture_storage_.IsTextNotesActive()) {
                    return std::string("当前没有文字记录");
                }
                return RequestCaptureAction(CaptureAction::StopNotesVoice);
            });
        mcp_server.AddTool(
            "self.capture.get_status",
            "Return TF-card mount, audio recording, text note, and file path status.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                return capture_storage_.GetStatusJson();
            });
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
        : key3_button_(KEY3_BUTTON_GPIO, false, 1000), boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializePettingReaction();
        InitializeTouch();
        InitializeButtons();
        InitializeMotionSensor();
        InitializeCaptureStorage();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static MenuAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override { return backlight_; }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            UpdatePowerSavePolicy();
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

    void OnUserTranscription(const std::string& text) override {
        capture_storage_.AppendTranscript(text);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch16);
