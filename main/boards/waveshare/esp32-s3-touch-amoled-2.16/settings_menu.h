#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <lvgl.h>
#include <material_symbols.h>

// Board-local overlay. UI updates run under the display lock; hit testing runs
// on the LVGL task and only reads the existing objects.
class SettingsMenu {
public:
    enum class Page { Settings, Volume, Files, AudioFiles, NotesFiles };

    struct FileItem {
        std::string name;
        std::string detail;
        std::string preview;
    };

    struct TouchTarget {
        bool outside = true;
        bool back = false;
        bool close = false;
        int menu_index = -1;
        int file_index = -1;
        bool volume_bar = false;
        int volume = 0;
    };

private:
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* back_button_ = nullptr;
    lv_obj_t* close_button_ = nullptr;
    lv_obj_t* cards_[4] = {};
    lv_obj_t* card_icons_[4] = {};
    lv_obj_t* card_names_[4] = {};
    lv_obj_t* file_rows_[3] = {};
    lv_obj_t* file_row_names_[3] = {};
    lv_obj_t* file_row_details_[3] = {};
    lv_obj_t* file_empty_ = nullptr;
    lv_obj_t* preview_ = nullptr;
    lv_obj_t* value_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    bool visible_ = false;
    bool editing_ = false;
    Page page_ = Page::Settings;

    static bool Contains(const lv_area_t& area, lv_coord_t x, lv_coord_t y) {
        return x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2;
    }

    static lv_area_t Coordinates(lv_obj_t* object) {
        lv_area_t area = {};
        lv_obj_get_coords(object, &area);
        return area;
    }

    static lv_obj_t* Label(lv_obj_t* parent, const char* text, int y) {
        auto* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
        return label;
    }

    static lv_obj_t* ActionButton(lv_obj_t* parent, const char* icon, const char* text, int x,
                                  const lv_font_t* text_font, const lv_font_t* icon_font) {
        auto* button = lv_obj_create(parent);
        lv_obj_set_size(button, 58, 34);
        lv_obj_set_pos(button, x, 17);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_radius(button, 17, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xF7F0FA), 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xE3D4EB), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);

        auto* icon_label = lv_label_create(button);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, icon_font, 0);
        lv_obj_set_style_text_color(icon_label, lv_color_hex(0x76528F), 0);
        lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 7, 0);

        auto* text_label = lv_label_create(button);
        lv_label_set_text(text_label, text);
        lv_obj_set_style_text_font(text_label, text_font, 0);
        lv_obj_set_style_text_color(text_label, lv_color_hex(0x76528F), 0);
        lv_obj_align(text_label, LV_ALIGN_RIGHT_MID, -6, 0);
        return button;
    }

public:
    void Destroy() {
        if (overlay_ != nullptr) {
            lv_obj_delete(overlay_);
            overlay_ = nullptr;
        }
        panel_ = nullptr;
        title_ = nullptr;
        back_button_ = nullptr;
        close_button_ = nullptr;
        file_empty_ = nullptr;
        preview_ = nullptr;
        for (int i = 0; i < 3; ++i) {
            file_rows_[i] = nullptr;
            file_row_names_[i] = nullptr;
            file_row_details_[i] = nullptr;
        }
        visible_ = false;
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

        back_button_ =
            ActionButton(panel_, MATERIAL_SYMBOLS_ARROW_BACK, "返回", 20, text_font, icon_font);
        close_button_ =
            ActionButton(panel_, MATERIAL_SYMBOLS_CLOSE, "关闭", 262, text_font, icon_font);

        const char* icons[] = {MATERIAL_SYMBOLS_VOLUME_UP, MATERIAL_SYMBOLS_MIC,
                               MATERIAL_SYMBOLS_EDIT_SQUARE, MATERIAL_SYMBOLS_SD_CARD};
        const char* names[] = {"音量", "录音", "记录", "文件"};
        for (int i = 0; i < 4; ++i) {
            cards_[i] = lv_obj_create(panel_);
            lv_obj_set_size(cards_[i], 124, 110);
            lv_obj_set_pos(cards_[i], 36 + (i % 2) * 140, 66 + (i / 2) * 122);
            lv_obj_set_style_pad_all(cards_[i], 0, 0);
            lv_obj_set_style_radius(cards_[i], 22, 0);
            lv_obj_remove_flag(cards_[i], LV_OBJ_FLAG_SCROLLABLE);
            card_icons_[i] = Label(cards_[i], icons[i], 18);
            lv_obj_set_style_text_font(card_icons_[i], icon_font, 0);
            card_names_[i] = Label(cards_[i], names[i], 65);
        }

        for (int i = 0; i < 3; ++i) {
            file_rows_[i] = lv_obj_create(panel_);
            lv_obj_set_size(file_rows_[i], 276, 58);
            lv_obj_set_pos(file_rows_[i], 32, 66 + i * 66);
            lv_obj_set_style_pad_all(file_rows_[i], 0, 0);
            lv_obj_set_style_radius(file_rows_[i], 14, 0);
            lv_obj_set_style_bg_color(file_rows_[i], lv_color_hex(0xF7F0FA), 0);
            lv_obj_set_style_border_color(file_rows_[i], lv_color_hex(0xE7DEED), 0);
            lv_obj_set_style_border_width(file_rows_[i], 1, 0);
            lv_obj_remove_flag(file_rows_[i], LV_OBJ_FLAG_SCROLLABLE);
            file_row_names_[i] = lv_label_create(file_rows_[i]);
            lv_obj_set_pos(file_row_names_[i], 12, 8);
            lv_obj_set_width(file_row_names_[i], 252);
            lv_label_set_long_mode(file_row_names_[i], LV_LABEL_LONG_DOT);
            file_row_details_[i] = lv_label_create(file_rows_[i]);
            lv_obj_set_pos(file_row_details_[i], 12, 33);
            lv_obj_set_width(file_row_details_[i], 252);
            lv_obj_set_style_text_color(file_row_details_[i], lv_color_hex(0x81748C), 0);
            lv_label_set_long_mode(file_row_details_[i], LV_LABEL_LONG_DOT);
            lv_obj_add_flag(file_rows_[i], LV_OBJ_FLAG_HIDDEN);
        }

        file_empty_ = Label(panel_, "没有找到文件", 150);
        lv_obj_set_style_text_color(file_empty_, lv_color_hex(0x81748C), 0);
        lv_obj_add_flag(file_empty_, LV_OBJ_FLAG_HIDDEN);

        preview_ = lv_label_create(panel_);
        lv_obj_set_pos(preview_, 32, 270);
        lv_obj_set_size(preview_, 276, 72);
        lv_label_set_long_mode(preview_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(preview_, lv_color_hex(0x766981), 0);
        lv_obj_add_flag(preview_, LV_OBJ_FLAG_HIDDEN);
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

    void Show(Page page, int selected, int volume, bool audio_recording, bool notes_active,
              const std::vector<FileItem>& files, const std::string& preview, bool playing,
              bool animate) {
        if (overlay_ == nullptr) {
            return;
        }
        visible_ = true;
        page_ = page;
        editing_ = page == Page::Volume;
        lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        const char* title = page == Page::Volume       ? "音量设置"
                            : page == Page::Files      ? "文件"
                            : page == Page::AudioFiles ? "播放录音"
                            : page == Page::NotesFiles ? "查看记录"
                                                       : "设置";
        lv_label_set_text(title_, title);
        lv_label_set_text(card_names_[1], audio_recording ? "停止录音" : "录音");
        lv_label_set_text(card_names_[2], notes_active ? "停止记录" : "记录");
        for (int i = 0; i < 4; ++i) {
            bool card_visible = page == Page::Settings || (page == Page::Files && i < 2);
            lv_obj_set_flag(cards_[i], LV_OBJ_FLAG_HIDDEN, !card_visible);
            lv_obj_set_style_bg_color(cards_[i], lv_color_hex(i == selected ? 0xEBDDF6 : 0xF7F0FA),
                                      0);
            lv_obj_set_style_border_color(cards_[i],
                                          lv_color_hex(i == selected ? 0x9C76C6 : 0xE7DEED), 0);
            lv_obj_set_style_border_width(cards_[i], i == selected ? 3 : 1, 0);
        }
        if (page == Page::Files) {
            lv_label_set_text(card_icons_[0], MATERIAL_SYMBOLS_PLAY_ARROW);
            lv_label_set_text(card_names_[0], "播放录音");
            lv_label_set_text(card_icons_[1], MATERIAL_SYMBOLS_EDIT_SQUARE);
            lv_label_set_text(card_names_[1], "查看记录");
        } else {
            const char* icons[] = {MATERIAL_SYMBOLS_VOLUME_UP, MATERIAL_SYMBOLS_MIC,
                                   MATERIAL_SYMBOLS_EDIT_SQUARE, MATERIAL_SYMBOLS_SD_CARD};
            const char* names[] = {"音量", audio_recording ? "停止录音" : "录音",
                                   notes_active ? "停止记录" : "记录", "文件"};
            for (int i = 0; i < 4; ++i) {
                lv_label_set_text(card_icons_[i], icons[i]);
                lv_label_set_text(card_names_[i], names[i]);
            }
        }

        const bool listing = page == Page::AudioFiles || page == Page::NotesFiles;
        const int visible_rows = std::min<int>(3, files.size());
        for (int i = 0; i < 3; ++i) {
            const bool visible = listing && i < visible_rows;
            lv_obj_set_flag(file_rows_[i], LV_OBJ_FLAG_HIDDEN, !visible);
            if (visible) {
                lv_label_set_text(file_row_names_[i], files[i].name.c_str());
                lv_label_set_text(file_row_details_[i], files[i].detail.c_str());
                lv_obj_set_style_bg_color(file_rows_[i],
                                          lv_color_hex(i == selected ? 0xEBDDF6 : 0xF7F0FA), 0);
                lv_obj_set_style_border_color(file_rows_[i],
                                              lv_color_hex(i == selected ? 0x9C76C6 : 0xE7DEED), 0);
                lv_obj_set_style_border_width(file_rows_[i], i == selected ? 3 : 1, 0);
            }
        }
        lv_obj_set_flag(file_empty_, LV_OBJ_FLAG_HIDDEN, !(listing && files.empty()));
        const bool show_preview = page == Page::NotesFiles && !files.empty();
        lv_obj_set_flag(preview_, LV_OBJ_FLAG_HIDDEN, !show_preview);
        if (show_preview) {
            lv_label_set_text(preview_, preview.c_str());
        }

        lv_obj_set_flag(value_, LV_OBJ_FLAG_HIDDEN, page != Page::Volume);
        lv_obj_set_flag(bar_, LV_OBJ_FLAG_HIDDEN, page != Page::Volume);
        lv_label_set_text_fmt(value_, "%d%%", volume);
        lv_bar_set_value(bar_, volume, LV_ANIM_OFF);
        lv_obj_align(hint_, LV_ALIGN_TOP_MID, 0, page == Page::Volume ? 245 : 350);
        const char* hint =
            page == Page::Volume       ? "上 / 下 调整 · 中键保存\n点击返回取消 · 点击关闭菜单"
            : page == Page::AudioFiles ? (playing ? "中键停止播放 · 上 / 下选择\n点击返回回到文件"
                                                  : "中键播放 · 上 / 下选择\n点击返回回到文件")
            : page == Page::NotesFiles ? "上 / 下查看记录\n点击返回回到文件"
                                       : "上 / 下 选择 · 中键确认\n点击返回逐级返回 · 点击关闭菜单";
        lv_label_set_text(hint_, hint);
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
        visible_ = false;
    }

    TouchTarget HitTestTouch(lv_coord_t x, lv_coord_t y) const {
        TouchTarget target;
        if (!visible_ || overlay_ == nullptr || lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN)) {
            return target;
        }

        if (!Contains(Coordinates(panel_), x, y)) {
            return target;
        }
        target.outside = false;
        if (Contains(Coordinates(back_button_), x, y)) {
            target.back = true;
            return target;
        }
        if (Contains(Coordinates(close_button_), x, y)) {
            target.close = true;
            return target;
        }
        if (editing_) {
            const lv_area_t bar_area = Coordinates(bar_);
            if (Contains(bar_area, x, y)) {
                target.volume_bar = true;
                const int width = std::max(1, static_cast<int>(bar_area.x2 - bar_area.x1 + 1));
                const int offset = static_cast<int>(x - bar_area.x1);
                target.volume = std::clamp(offset * 100 / width, 0, 100);
            }
            return target;
        }

        if (page_ == Page::AudioFiles || page_ == Page::NotesFiles) {
            for (int i = 0; i < 3; ++i) {
                if (!lv_obj_has_flag(file_rows_[i], LV_OBJ_FLAG_HIDDEN) &&
                    Contains(Coordinates(file_rows_[i]), x, y)) {
                    target.file_index = i;
                    break;
                }
            }
            return target;
        }

        const int card_count = page_ == Page::Files ? 2 : 4;
        for (int i = 0; i < card_count; ++i) {
            if (Contains(Coordinates(cards_[i]), x, y)) {
                target.menu_index = i;
                break;
            }
        }
        return target;
    }
};
