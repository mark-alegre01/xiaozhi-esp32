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
    if (anim_timer_ != nullptr) {
        lv_timer_del(anim_timer_);
        anim_timer_ = nullptr;
    }
    if (speak_timer_ != nullptr) {
        lv_timer_del(speak_timer_);
        speak_timer_ = nullptr;
    }
    if (music_anim_timer_ != nullptr) {
        lv_timer_del(music_anim_timer_);
        music_anim_timer_ = nullptr;
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
    // Spoken conversational words must NOT trigger erratic, inappropriate emotional facial shifts.
    // Talking mouth movement is handled cleanly and smoothly by SetStatus("Speaking").
}

void OledDisplay::SetStatus(const char* status) {
    // Let the base class update the status label
    LvglDisplay::SetStatus(status);

    if (height_ != 64 || face_image_ == nullptr) return;

    bool speaking = (status != nullptr && strstr(status, "peaking") != nullptr);

    DisplayLockGuard lock(this);
    if (speaking && !is_speaking_) {
        is_speaking_ = true;
        // Stop any one-shot emotion animation and start the talking loop
        if (anim_timer_ != nullptr) {
            lv_timer_del(anim_timer_);
            anim_timer_ = nullptr;
            is_animating_ = false;
            active_anim_ = nullptr;
        }
        StartSpeakingAnimation();
    } else if (!speaking && is_speaking_) {
        is_speaking_ = false;
        StopSpeakingAnimation();
        // Resume music animation if music is playing, else show neutral
        if (is_music_playing_) {
            StartMusicAnimation();
        } else if (!is_animating_) {
            DrawFaceBitmap(oled_face_neutral);
        }
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
    lv_obj_set_style_layout(container_, LV_LAYOUT_NONE, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);

    /* Content — full 128x64 area for robot face */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_size(content_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_layout(content_, LV_LAYOUT_NONE, 0);
    lv_obj_align(content_, LV_ALIGN_CENTER, 0, 0);

    CreateRobotEyes(content_);

    /* Layer 1: Top bar - floats transparently over the black margin at top */
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
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

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
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

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

    // Chat message label removed - no captions on OLED
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

    // Periodic natural eye blinking animation every 3.5 seconds
    blink_timer_ = lv_timer_create(BlinkTimerCallback, 3500, this);
}

void OledDisplay::DrawFaceBitmap(const uint8_t* bitmap_1bit) {
    if (!face_buffer_rgb565_ || !bitmap_1bit || !face_image_) return;

    for (int y = 0; y < OLED_FACE_HEIGHT; ++y) {
        for (int byte_idx = 0; byte_idx < (OLED_FACE_WIDTH / 8); ++byte_idx) {
            uint8_t byte_val = bitmap_1bit[y * (OLED_FACE_WIDTH / 8) + byte_idx];
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

void OledDisplay::PlayAnimationOnce(const RobotAnimation* anim) {
    if (!anim || anim->frame_count <= 0 || !face_image_) return;

    // Do NOT interrupt talking or music with emotion animations
    if (is_speaking_ || is_music_playing_) return;

    if (anim_timer_ != nullptr) {
        lv_timer_del(anim_timer_);
        anim_timer_ = nullptr;
    }

    active_anim_ = anim;
    anim_frame_idx_ = 0;
    is_animating_ = true;

    // Show initial frame immediately
    DrawFaceBitmap(active_anim_->frames[0]);

    // Step through remaining frames
    int interval = anim->frame_duration_ms > 0 ? anim->frame_duration_ms : 65;
    anim_timer_ = lv_timer_create(AnimTimerCallback, interval, this);
}

void OledDisplay::AnimTimerCallback(lv_timer_t* timer) {
    auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (!self || !self->is_animating_ || !self->active_anim_) return;

    // If speaking or music started, abort animation immediately
    if (self->is_speaking_ || self->is_music_playing_) {
        self->is_animating_ = false;
        self->active_anim_ = nullptr;
        if (self->anim_timer_ != nullptr) {
            lv_timer_del(self->anim_timer_);
            self->anim_timer_ = nullptr;
        }
        return;
    }

    self->anim_frame_idx_++;
    if (self->anim_frame_idx_ < self->active_anim_->frame_count) {
        self->DrawFaceBitmap(self->active_anim_->frames[self->anim_frame_idx_]);
    } else {
        // Finished playing single pass! Restore resting neutral GIF face
        self->is_animating_ = false;
        self->active_anim_ = nullptr;
        if (self->anim_timer_ != nullptr) {
            lv_timer_del(self->anim_timer_);
            self->anim_timer_ = nullptr;
        }
        if (!self->is_speaking_ && !self->is_music_playing_) {
            self->DrawFaceBitmap(oled_face_neutral);
        }
    }
}

void OledDisplay::BlinkTimerCallback(lv_timer_t* timer) {
    auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (!self || !self->face_image_) return;
    // Do not blink if an animation is currently playing, speaking, or music is looping
    if (self->is_animating_ || self->is_speaking_ || self->is_music_playing_) return;

    // Smooth natural eye blink from the GIF (run6, 7 frames)
    self->PlayAnimationOnce(&oled_anim_blink);
}

// ── Speaking (talking mouth) animation ──────────────────────────────────────
// Smooth natural talking mouth movement while preserving the robot's resting eyes.
// Uses dedicated talking mouth frames: talk_0 (closed) -> talk_1 -> talk_2 -> talk_3 (open).
void OledDisplay::StartSpeakingAnimation() {
    if (!face_image_) return;
    if (speak_timer_ != nullptr) return;  // already running
    speak_frame_idx_ = 1;
    // Show first speaking frame immediately (small open mouth)
    DrawFaceBitmap(oled_talk_frames[1]);
    // Cycle every 130ms – natural speaking cadence
    speak_timer_ = lv_timer_create(SpeakTimerCallback, 130, this);
}

void OledDisplay::StopSpeakingAnimation() {
    if (speak_timer_ != nullptr) {
        lv_timer_del(speak_timer_);
        speak_timer_ = nullptr;
    }
    speak_frame_idx_ = 0;
    if (!is_animating_) {
        DrawFaceBitmap(oled_face_neutral);
    }
}

void OledDisplay::SpeakTimerCallback(lv_timer_t* timer) {
    auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (!self || !self->face_image_ || !self->is_speaking_) return;

    // Smooth mouth cycle: resting -> small -> medium -> wide -> medium -> small
    constexpr int kTalkFrameCount = 6;
    self->DrawFaceBitmap(oled_talk_frames[self->speak_frame_idx_ % kTalkFrameCount]);
    self->speak_frame_idx_++;
    if (self->speak_frame_idx_ >= kTalkFrameCount * 8) {
        self->speak_frame_idx_ = 0;  // wrap without overflow
    }
}

// ── Music playing looped animation ──────────────────────────────────────────
void OledDisplay::StartMusicAnimation() {
    if (!face_image_) return;
    if (music_anim_timer_ != nullptr) return;  // already running
    music_frame_idx_ = 0;
    DrawFaceBitmap(oled_anim_happy_frames[0]);
    // Slow 120ms per frame – plays happy face gently bobbing while music runs
    music_anim_timer_ = lv_timer_create(MusicAnimTimerCallback, 120, this);
}

void OledDisplay::StopMusicAnimation() {
    if (music_anim_timer_ != nullptr) {
        lv_timer_del(music_anim_timer_);
        music_anim_timer_ = nullptr;
    }
    music_frame_idx_ = 0;
}

void OledDisplay::MusicAnimTimerCallback(lv_timer_t* timer) {
    auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(timer));
    if (!self || !self->face_image_ || !self->is_music_playing_) return;
    // Don't override speaking animation
    if (self->is_speaking_) return;

    // Loop through all 18 happy frames continuously
    constexpr int kFrameCount = 18;
    self->DrawFaceBitmap(oled_anim_happy_frames[self->music_frame_idx_ % kFrameCount]);
    self->music_frame_idx_++;
    if (self->music_frame_idx_ >= kFrameCount) {
        self->music_frame_idx_ = 0;
    }
}

void OledDisplay::SetMusicPlaying(bool playing) {
    if (height_ != 64 || face_image_ == nullptr) return;
    DisplayLockGuard lock(this);
    if (playing && !is_music_playing_) {
        is_music_playing_ = true;
        // Don't interrupt speaking animation; music anim will start after TTS ends
        if (!is_speaking_) {
            // Stop any one-shot emotion animation
            if (anim_timer_ != nullptr) {
                lv_timer_del(anim_timer_);
                anim_timer_ = nullptr;
                is_animating_ = false;
                active_anim_ = nullptr;
            }
            StartMusicAnimation();
        }
    } else if (!playing && is_music_playing_) {
        is_music_playing_ = false;
        StopMusicAnimation();
        if (!is_speaking_ && !is_animating_) {
            DrawFaceBitmap(oled_face_neutral);
        }
    }
}

void OledDisplay::SetLyricsMode(bool on) {
    if (height_ != 64) return;
    DisplayLockGuard lock(this);

    if (on == is_lyrics_mode_) return;
    is_lyrics_mode_ = on;

    if (on) {
        // Pause music animation while lyrics are showing
        StopMusicAnimation();

        // Build full-screen lyrics overlay on top of content_
        if (lyrics_overlay_ == nullptr) {
            auto screen = lv_screen_active();
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            const lv_font_t* text_font = lvgl_theme->text_font()->font();

            lyrics_overlay_ = lv_obj_create(screen);
            lv_obj_set_size(lyrics_overlay_, LV_HOR_RES, LV_VER_RES);
            lv_obj_set_style_bg_color(lyrics_overlay_, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(lyrics_overlay_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(lyrics_overlay_, 0, 0);
            lv_obj_set_style_pad_all(lyrics_overlay_, 2, 0);
            lv_obj_set_style_radius(lyrics_overlay_, 0, 0);
            lv_obj_set_scrollbar_mode(lyrics_overlay_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_style_layout(lyrics_overlay_, LV_LAYOUT_FLEX, 0);
            lv_obj_set_flex_flow(lyrics_overlay_, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(lyrics_overlay_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            // Previous line (white text on black)
            lyrics_label_prev_ = lv_label_create(lyrics_overlay_);
            lv_obj_set_width(lyrics_label_prev_, LV_HOR_RES - 4);
            lv_obj_set_style_text_font(lyrics_label_prev_, text_font, 0);
            lv_obj_set_style_text_align(lyrics_label_prev_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(lyrics_label_prev_, lv_color_white(), 0);
            lv_label_set_long_mode(lyrics_label_prev_, LV_LABEL_LONG_DOT);
            lv_label_set_text(lyrics_label_prev_, "");

            // Current line (inverted bar: black text on solid white background for high contrast)
            lyrics_label_curr_ = lv_label_create(lyrics_overlay_);
            lv_obj_set_width(lyrics_label_curr_, LV_HOR_RES - 4);
            lv_obj_set_style_text_font(lyrics_label_curr_, text_font, 0);
            lv_obj_set_style_text_align(lyrics_label_curr_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(lyrics_label_curr_, lv_color_black(), 0);
            lv_obj_set_style_bg_color(lyrics_label_curr_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(lyrics_label_curr_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(lyrics_label_curr_, 2, 0);
            lv_obj_set_style_pad_hor(lyrics_label_curr_, 2, 0);
            lv_label_set_long_mode(lyrics_label_curr_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_label_set_text(lyrics_label_curr_, "Loading...");

            // Next line (white text on black)
            lyrics_label_next_ = lv_label_create(lyrics_overlay_);
            lv_obj_set_width(lyrics_label_next_, LV_HOR_RES - 4);
            lv_obj_set_style_text_font(lyrics_label_next_, text_font, 0);
            lv_obj_set_style_text_align(lyrics_label_next_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(lyrics_label_next_, lv_color_white(), 0);
            lv_label_set_long_mode(lyrics_label_next_, LV_LABEL_LONG_DOT);
            lv_label_set_text(lyrics_label_next_, "");
        }
        lv_obj_clear_flag(lyrics_overlay_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Hide overlay, restore music animation
        if (lyrics_overlay_ != nullptr) {
            lv_obj_add_flag(lyrics_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (is_music_playing_ && !is_speaking_) {
            StartMusicAnimation();
        } else if (!is_speaking_ && !is_animating_) {
            DrawFaceBitmap(oled_face_neutral);
        }
    }
}

void OledDisplay::SetLyricsLine(const char* prev, const char* current, const char* next) {
    if (!is_lyrics_mode_ || lyrics_overlay_ == nullptr) return;
    DisplayLockGuard lock(this);

    if (lyrics_label_prev_ && prev)    lv_label_set_text(lyrics_label_prev_, prev[0] ? prev : " ");
    if (lyrics_label_curr_ && current) lv_label_set_text(lyrics_label_curr_, current[0] ? current : " ");
    if (lyrics_label_next_ && next)    lv_label_set_text(lyrics_label_next_, next[0] ? next : " ");
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

    if (!emotion || emotion[0] == '\0') return;

    // Keep face consistent: never disrupt speaking mouth animation or music playback
    if (is_speaking_ || is_music_playing_) return;

    const RobotAnimation* anim = GetAnimationForEmotion(emotion);
    if (anim != nullptr) {
        PlayAnimationOnce(anim);
    }
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
