#include "oled_display.h"
#include "oled_faces.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

void OledDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    if (talking_timer_ != nullptr) {
        lv_timer_del(talking_timer_);
        talking_timer_ = nullptr;
    }

    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (blink_timer_ != nullptr) {
        lv_timer_del(blink_timer_);
        blink_timer_ = nullptr;
    }

    if (face_buffer_rgb565_ != nullptr) {
        free(face_buffer_rgb565_);
        face_buffer_rgb565_ = nullptr;
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    // Caption/subtitle display disabled for OLED — face is always shown
    // No-op: do not hide face or show any text overlay
    (void)role;
    (void)content;
}

void OledDisplay::SetStatus(const char* status) {
    // Let the base class update the status label
    LvglDisplay::SetStatus(status);

    if (height_ != 64 || face_image_ == nullptr) return;

    // Detect speaking state to start/stop talking mouth animation
    bool speaking = (status != nullptr && strstr(status, "peaking") != nullptr);

    DisplayLockGuard lock(this);
    if (speaking && !is_speaking_) {
        is_speaking_ = true;
        mouth_frame_ = 0;
        if (talking_timer_ == nullptr) {
            talking_timer_ = lv_timer_create(TalkTimerCallback, 140, this);
        }
    } else if (!speaking && is_speaking_) {
        is_speaking_ = false;
        if (talking_timer_ != nullptr) {
            lv_timer_del(talking_timer_);
            talking_timer_ = nullptr;
        }
        // Restore resting face
        DrawFaceBitmap(current_face_bitmap_ ? current_face_bitmap_ : oled_face_neutral);
    }
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content — full 48px area for robot face, no text overlays */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_height(content_, 48);
    lv_obj_set_style_layout(content_, LV_LAYOUT_NONE, 0);

    CreateRobotEyes(content_);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x32() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_column(container_, 0, 0);

    /* Emotion label on the left side */
    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, 32, 32);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_radius(content_, 0, 0);

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, large_icon_font, 0);
    lv_label_set_text(emotion_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_center(emotion_label_);

    /* Right side */
    side_bar_ = lv_obj_create(container_);
    lv_obj_set_size(side_bar_, width_ - 32, 32);
    lv_obj_set_flex_flow(side_bar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(side_bar_, 0, 0);
    lv_obj_set_style_border_width(side_bar_, 0, 0);
    lv_obj_set_style_radius(side_bar_, 0, 0);
    lv_obj_set_style_pad_row(side_bar_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(side_bar_);
    lv_obj_set_size(status_bar_, width_ - 32, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_pad_left(status_label_, 2, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_pad_left(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    chat_message_label_ = lv_label_create(side_bar_);
    lv_obj_set_size(chat_message_label_, width_ - 32, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chat_message_label_, 2, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(chat_message_label_, "");

    // Start scrolling subtitle after a delay
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
}

void OledDisplay::CreateRobotEyes(lv_obj_t* parent) {
    if (face_buffer_rgb565_ == nullptr) {
        face_buffer_rgb565_ = static_cast<uint16_t*>(malloc(OLED_FACE_WIDTH * OLED_FACE_HEIGHT * sizeof(uint16_t)));
        if (!face_buffer_rgb565_) {
            ESP_LOGE(TAG, "Failed to allocate face_buffer_rgb565_");
            return;
        }
    }

    memset(&face_img_dsc_, 0, sizeof(face_img_dsc_));
    face_img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    face_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    face_img_dsc_.header.w = OLED_FACE_WIDTH;
    face_img_dsc_.header.h = OLED_FACE_HEIGHT;
    face_img_dsc_.header.stride = OLED_FACE_WIDTH * 2;
    face_img_dsc_.data = reinterpret_cast<const uint8_t*>(face_buffer_rgb565_);
    face_img_dsc_.data_size = OLED_FACE_WIDTH * OLED_FACE_HEIGHT * 2;

    face_image_ = lv_image_create(parent);
    lv_obj_set_size(face_image_, OLED_FACE_WIDTH, OLED_FACE_HEIGHT);
    lv_obj_set_style_bg_opa(face_image_, LV_OPA_TRANSP, 0);
    lv_obj_center(face_image_);

    current_face_bitmap_ = oled_face_neutral;
    DrawFaceBitmap(current_face_bitmap_);

    // Periodic blinking animation every 3.5 seconds
    blink_timer_ = lv_timer_create(BlinkTimerCallback, 3500, this);
}

void OledDisplay::DrawFaceBitmap(const uint8_t* bitmap_1bit) {
    if (!face_buffer_rgb565_ || !bitmap_1bit || !face_image_) return;

    for (int y = 0; y < OLED_FACE_HEIGHT; ++y) {
        for (int byte_idx = 0; byte_idx < 16; ++byte_idx) {
            uint8_t byte_val = bitmap_1bit[y * 16 + byte_idx];
            for (int bit = 0; bit < 8; ++bit) {
                int x = byte_idx * 8 + bit;
                bool is_lit = (byte_val & (1 << (7 - bit))) != 0;
                face_buffer_rgb565_[y * OLED_FACE_WIDTH + x] = is_lit ? 0xFFFF : 0x0000;
            }
        }
    }

    lv_image_set_src(face_image_, &face_img_dsc_);
    lv_obj_invalidate(face_image_);
}

void OledDisplay::BlinkTimerCallback(lv_timer_t* timer) {
    auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (!self || !self->face_image_) return;
    // Don't blink while speaking — talking animation takes over
    if (self->is_speaking_) return;

    // Fast blink: draw blink frame
    self->DrawFaceBitmap(oled_face_blink);

    // Reopen eyes after 120ms
    lv_timer_create([](lv_timer_t* t) {
        auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(t));
        if (self && self->face_image_ && !self->is_speaking_) {
            self->DrawFaceBitmap(self->current_face_bitmap_ ? self->current_face_bitmap_ : oled_face_neutral);
        }
        lv_timer_del(t);
    }, 120, self);
}

void OledDisplay::DrawTalkFrame() {
    if (!face_image_) return;
    // Use current emotion's eye rows (from current_face_bitmap_) combined with talk mouth
    // For simplicity use talk frames which have neutral eyes + varying mouth
    const uint8_t* frame = oled_talk_frames[mouth_frame_ % 4];
    DrawFaceBitmap(frame);
    mouth_frame_ = (mouth_frame_ + 1) % 4;
}

void OledDisplay::TalkTimerCallback(lv_timer_t* timer) {
    auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (!self || !self->face_image_ || !self->is_speaking_) return;
    self->DrawTalkFrame();
}

void OledDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (height_ == 32) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (emotion_label_ != nullptr) {
            if (utf8 != nullptr) {
                lv_obj_set_style_text_font(emotion_label_, emotion_font, 0);
                lv_label_set_text(emotion_label_, utf8);
            } else {
                lv_obj_set_style_text_font(emotion_label_, lvgl_theme->emoji_font()->font(), 0);
                lv_label_set_text(emotion_label_, NOTO_EMOJI_NEUTRAL);
            }
        }
        return;
    }

    current_face_bitmap_ = GetOledFaceBitmap(emotion);
    DrawFaceBitmap(current_face_bitmap_);
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}
