#pragma once

#include <lvgl.h>
#include <material_symbols.h>

// Board-local overlay. All methods must run under the display lock.
class SettingsMenu {
private:
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* cards_[4] = {};
    lv_obj_t* card_names_[4] = {};
    lv_obj_t* value_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* hint_ = nullptr;

    static lv_obj_t* Label(lv_obj_t* parent, const char* text, int y) {
        auto* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
        return label;
    }

public:
    void Destroy() {
        if (overlay_ != nullptr) {
            lv_obj_delete(overlay_);
            overlay_ = nullptr;
        }
    }

    void Create(lv_display_t* display, const lv_font_t* text_font, const lv_font_t* icon_font) {
        if (overlay_ != nullptr) {
            return;
        }
        overlay_ = lv_obj_create(lv_display_get_layer_top(display));
        lv_obj_remove_style_all(overlay_);
        lv_obj_set_size(overlay_, 480, 480);
        lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x21182E), 0);
        lv_obj_set_style_bg_opa(overlay_, LV_OPA_60, 0);
        lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

        panel_ = lv_obj_create(overlay_);
        lv_obj_set_size(panel_, 340, 410);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(panel_, 0, 0);
        lv_obj_set_style_radius(panel_, 28, 0);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0xFFF8FF), 0);
        lv_obj_set_style_border_color(panel_, lv_color_hex(0xC9ACD9), 0);
        lv_obj_set_style_border_width(panel_, 2, 0);
        lv_obj_set_style_text_font(panel_, text_font, 0);
        lv_obj_set_style_text_color(panel_, lv_color_hex(0x2B2140), 0);
        title_ = Label(panel_, "设置", 22);

        const char* icons[] = {MATERIAL_SYMBOLS_VOLUME_UP, MATERIAL_SYMBOLS_MIC,
                               MATERIAL_SYMBOLS_EDIT_SQUARE, MATERIAL_SYMBOLS_ARROW_BACK};
        const char* names[] = {"音量", "录音", "记录", "返回"};
        for (int i = 0; i < 4; ++i) {
            cards_[i] = lv_obj_create(panel_);
            lv_obj_set_size(cards_[i], 124, 110);
            lv_obj_set_pos(cards_[i], 36 + (i % 2) * 140, 66 + (i / 2) * 122);
            lv_obj_set_style_pad_all(cards_[i], 0, 0);
            lv_obj_set_style_radius(cards_[i], 22, 0);
            lv_obj_remove_flag(cards_[i], LV_OBJ_FLAG_SCROLLABLE);
            auto* icon = Label(cards_[i], icons[i], 18);
            lv_obj_set_style_text_font(icon, icon_font, 0);
            card_names_[i] = Label(cards_[i], names[i], 65);
        }
        value_ = Label(panel_, "", 110);
        bar_ = lv_bar_create(panel_);
        lv_obj_set_size(bar_, 244, 16);
        lv_obj_align(bar_, LV_ALIGN_TOP_MID, 0, 180);
        lv_bar_set_range(bar_, 0, 100);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(0xE7DEED), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(0x9C76C6), LV_PART_INDICATOR);
        hint_ = Label(panel_, "", 330);
        lv_obj_set_width(hint_, 300);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(hint_, 8, 0);
    }

    void Show(bool editing, int selected, int volume, bool audio_recording, bool notes_active,
              bool animate) {
        if (overlay_ == nullptr) {
            return;
        }
        lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(title_, editing ? "音量设置" : "设置");
        lv_label_set_text(card_names_[1], audio_recording ? "停止录音" : "录音");
        lv_label_set_text(card_names_[2], notes_active ? "停止记录" : "记录");
        for (int i = 0; i < 4; ++i) {
            lv_obj_set_flag(cards_[i], LV_OBJ_FLAG_HIDDEN, editing);
            lv_obj_set_style_bg_color(cards_[i], lv_color_hex(i == selected ? 0xEBDDF6 : 0xF7F0FA),
                                      0);
            lv_obj_set_style_border_color(cards_[i],
                                          lv_color_hex(i == selected ? 0x9C76C6 : 0xE7DEED), 0);
            lv_obj_set_style_border_width(cards_[i], i == selected ? 3 : 1, 0);
        }
        lv_obj_set_flag(value_, LV_OBJ_FLAG_HIDDEN, !editing);
        lv_obj_set_flag(bar_, LV_OBJ_FLAG_HIDDEN, !editing);
        lv_label_set_text_fmt(value_, "%d%%", volume);
        lv_bar_set_value(bar_, volume, LV_ANIM_OFF);
        lv_obj_align(hint_, LV_ALIGN_TOP_MID, 0, editing ? 245 : 330);
        lv_label_set_text(hint_, editing ? "上 / 下 调整 · 中键保存\n长按中键 2 秒取消"
                                         : "上 / 下 选择 · 中键确认\n长按中键 2 秒返回");
        if (animate) {
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, panel_);
            lv_anim_set_values(&animation, 24, 0);
            lv_anim_set_duration(&animation, 180);
            lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&animation, [](void* object, int32_t value) {
                lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(object), value, 0);
            });
            lv_anim_start(&animation);
            lv_obj_fade_in(overlay_, 180, 0);
        }
    }

    void Hide() {
        if (overlay_ != nullptr) {
            lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        }
    }
};
