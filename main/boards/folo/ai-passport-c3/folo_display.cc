#include "folo_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "beauty_emotions.h"
#include "board.h"
#include "jpeg_to_image.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <material_symbols.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>

#define TAG "FoloDisplay"

namespace {
lv_obj_t* Label(lv_obj_t* parent, int x, int y, int w, int h) {
    auto label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}
bool FullTextState(DeviceState state) {
    return state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating ||
           state == kDeviceStateUpgrading || state == kDeviceStateFatalError;
}
}

FoloDisplay::~FoloDisplay() {
    DisplayLockGuard lock(this);
    esp_timer_stop(notification_timer_);
    esp_timer_stop(preview_timer_);
    if (animation_timer_) lv_timer_delete(animation_timer_);
    if (display_) lv_obj_clean(lv_display_get_screen_active(display_));
    // Base destructors must not delete children after deleting their display.
    network_label_ = status_label_ = notification_label_ = mute_label_ = battery_label_ = nullptr;
    chat_message_label_ = nullptr;
    preview_image_ = nullptr;
    preview_image_cached_.reset();
    portrait_decoded_.reset();
}

void FoloDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (setup_ui_called_ || !display_) return;
    auto screen = lv_display_get_screen_active(display_);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    network_label_ = Label(screen, 8, 4, 24, 24);
    mute_label_ = Label(screen, 36, 4, 24, 24);
    clock_label_ = Label(screen, 65, 4, 84, 24);
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    percentage_label_ = Label(screen, 153, 4, 55, 24);
    lv_obj_set_style_text_align(percentage_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(percentage_label_, "--%");
    battery_label_ = Label(screen, 212, 4, 24, 24);
    status_label_ = Label(screen, 8, 30, 224, 24);
    notification_label_ = Label(screen, 8, 30, 224, 24);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    portrait_obj_ = lv_image_create(screen);
    lv_obj_set_pos(portrait_obj_, 56, 82);  // 128×128 居中于原 192×192 人物区
    lv_obj_set_size(portrait_obj_, 128, 128);
    lv_obj_set_style_img_opa(portrait_obj_, LV_OPA_COVER, 0);

    preview_image_ = lv_image_create(screen);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = Label(screen, 12, 246, 216, 68);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    menu_label_ = Label(screen, 12, 64, 216, 248);
    lv_label_set_long_mode(menu_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(menu_label_, 8, 0);
    lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
    volume_bar_ = lv_bar_create(screen);
    lv_obj_set_pos(volume_bar_, 24, 240);
    lv_obj_set_size(volume_bar_, 192, 4);
    lv_bar_set_range(volume_bar_, 0, 100);
    lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(0xDE6E91), LV_PART_INDICATOR);
    lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);

    Display::SetupUI();
    SetTheme(current_theme_);
    idle_.Activity(esp_timer_get_time());
    RefreshContent();
}

void FoloDisplay::SetTheme(Theme* theme) {
    auto typed = dynamic_cast<LvglTheme*>(theme);
    if (!typed) return;
    DisplayLockGuard lock(this);
    Display::SetTheme(theme);
    if (!setup_ui_called_) return;
    const bool dark = theme->name() == "dark";
    auto screen = lv_display_get_screen_active(display_);
    lv_obj_set_style_bg_color(screen, lv_color_hex(dark ? 0x202027 : 0xF3F5F5), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(dark ? 0xF6F2F5 : 0x30323A), 0);
    lv_obj_set_style_text_font(screen, typed->text_font()->font(), 0);
    for (auto label : {network_label_, mute_label_, battery_label_}) {
        lv_obj_set_style_text_font(label, typed->icon_font()->font(), 0);
    }
    lv_obj_set_style_text_color(status_label_, lv_color_hex(dark ? 0x8AD4C8 : 0x28776D), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(dark ? 0xF3A5BC : 0xA33F63), 0);
}

void FoloDisplay::Activity() {
    idle_.Activity(esp_timer_get_time());
    if (panel_asleep_) {
        ApplySleep(false);
    }
}

void FoloDisplay::ApplySleep(bool asleep) {
    if (asleep == panel_asleep_) return;
    panel_asleep_ = asleep;
    auto backlight = Board::GetInstance().GetBacklight();
    if (asleep) {
        menu_open_ = false;
        lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
        volume_until_ = 0;
        if (animation_timer_) lv_timer_pause(animation_timer_);
        backlight->SetBrightness(0);  // Do not overwrite saved brightness.
    } else {
        wake_until_ = esp_timer_get_time() + 2000000;
        backlight->RestoreBrightness();
        if (animation_timer_) lv_timer_resume(animation_timer_);
    }
    RefreshPortrait();
    RefreshContent();
}

bool FoloDisplay::WakeFromKey() {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return true;
    bool consumed = panel_asleep_;
    Activity();
    RefreshPortrait();
    return consumed;
}

void FoloDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) return;
    DisplayLockGuard lock(this);
    const auto state = Application::GetInstance().GetDeviceState();
    if (state != state_) {
        state_ = state;
        thinking_ = false;
        menu_open_ = false;
        lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
        Activity();
    }
    LvglDisplay::SetStatus(status);
    RefreshPortrait();
    RefreshContent();
}

void FoloDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    emotion_name_ = emotion ? emotion : "";
    emotion_ = PortraitState::Idle;
    RefreshPortrait();
}

void FoloDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    message_ = content ? content : "";
    if (!message_.empty()) {
        Activity();
        if (role && !strcmp(role, "user")) {
            thinking_ = true;
            LvglDisplay::SetStatus("思考中");
        }
        else if (role && !strcmp(role, "assistant")) thinking_ = false;
    }
    RefreshPortrait();
    RefreshContent();
}

void FoloDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    message_.clear();
    thinking_ = false;
    if (setup_ui_called_) RefreshContent();
}

void FoloDisplay::RefreshPortrait() {
    portrait_state_ = emotion_;
    if (panel_asleep_) portrait_state_ = PortraitState::Sleepy;
    else if (state_ == kDeviceStateSpeaking || state_ == kDeviceStateNotifying)
        portrait_state_ = PortraitState::Speaking;
    else if (thinking_ || state_ == kDeviceStateConnecting)
        portrait_state_ = PortraitState::Thinking;
    else if (state_ == kDeviceStateListening) portrait_state_ = PortraitState::Listening;
    else if (esp_timer_get_time() < wake_until_) portrait_state_ = PortraitState::Wake;
    else if (low_battery_) portrait_state_ = PortraitState::Sad;

    const BeautyImage* img = BeautyForState(portrait_state_);
    // 服务器下发的具体情绪（angry/loving/surprised…）在待机或说话时覆盖默认表情。
    if (!panel_asleep_ && !emotion_name_.empty() &&
        (portrait_state_ == PortraitState::Idle || portrait_state_ == PortraitState::Speaking)) {
        if (const BeautyImage* e = BeautyByName(emotion_name_.c_str())) img = e;
    }
    if (img != portrait_image_) {
        portrait_image_ = img;
        UpdatePortraitImage(img);
    }
}

void FoloDisplay::UpdatePortraitImage(const BeautyImage* img) {
    portrait_decoded_.reset();
    if (img == nullptr || img->data == nullptr || img->size == 0) return;

    uint8_t* decoded = nullptr;
    size_t out_len = 0, w = 0, h = 0, stride = 0;
    if (jpeg_to_image(img->data, img->size, &decoded, &out_len, &w, &h, &stride) != ESP_OK ||
        decoded == nullptr) {
        ESP_LOGE(TAG, "beauty JPEG decode failed: %s", img->name ? img->name : "?");
        return;
    }

    portrait_decoded_ = std::make_unique<LvglAllocatedImage>(
        decoded, out_len, static_cast<int>(w), static_cast<int>(h),
        static_cast<int>(stride), LV_COLOR_FORMAT_RGB565);

    lv_image_set_src(portrait_obj_, portrait_decoded_->image_dsc());
}

void FoloDisplay::RefreshContent() {
    const bool full_text = FullTextState(state_);
    const bool hide_portrait = full_text || menu_open_ || preview_image_cached_ != nullptr;
    if (hide_portrait) lv_obj_add_flag(portrait_obj_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(portrait_obj_, LV_OBJ_FLAG_HIDDEN);
    if (full_text || menu_open_ || !preview_image_cached_)
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    if (menu_open_ || (hide_subtitle_ && !full_text))
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(chat_message_label_, 12, full_text ? 64 : 246);
    lv_obj_set_size(chat_message_label_, 216, full_text ? 248 : 68);
    const char* content = message_.c_str();
    if (message_.empty()) {
        if (state_ == kDeviceStateListening) content = "我在听，你慢慢说";
        else if (state_ == kDeviceStateConnecting) content = "正在连接，请稍等";
        else if (state_ == kDeviceStateSpeaking) content = "我正在回答";
        else if (low_battery_) content = "电量较低，请及时充电";
        else content = "你好，我是小智\n今天想聊点什么？";
    }
    if (strcmp(lv_label_get_text(chat_message_label_), content))
        lv_label_set_text(chat_message_label_, content);
}

void FoloDisplay::UpdateStatusBar(bool update_all) {
    if (!setup_ui_called_) return;
    auto& board = Board::GetInstance();
    auto& app = Application::GetInstance();
    int level = 0;
    bool charging = false, discharging = false;
    const bool valid = board.GetBatteryLevel(level, charging, discharging);
    const int volume = board.GetAudioCodec()->output_volume();
    const auto state = app.GetDeviceState();
    const bool voice = state == kDeviceStateListening && app.IsVoiceDetected();
    const bool update_network = update_all || status_ticks_++ % 10 == 0;
    const char* network = update_network ? board.GetNetworkStateIcon() : nullptr;
    DisplayLockGuard lock(this);
    if (state != state_) {
        state_ = state;
        thinking_ = false;
        menu_open_ = false;
        lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
        Activity();
    }
    const auto now = esp_timer_get_time();
    ApplySleep(idle_.Tick(now, state, voice));
    if (panel_asleep_) return;
    lv_label_set_text(mute_label_, volume == 0 ? MATERIAL_SYMBOLS_VOLUME_OFF : "");
    if (network) lv_label_set_text(network_label_, network);
    time_t seconds = time(nullptr);
    struct tm local = {};
    localtime_r(&seconds, &local);
    char clock[8] = "--:--";
    if (local.tm_year >= 125) strftime(clock, sizeof(clock), "%H:%M", &local);
    lv_label_set_text(clock_label_, clock);
    if (valid) {
        lv_label_set_text_fmt(percentage_label_, "%d%%", level);
        lv_label_set_text(battery_label_, level <= 15 ? MATERIAL_SYMBOLS_BATTERY_ANDROID_0 :
                          level < 50 ? MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3 :
                          MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);
    } else {
        lv_label_set_text(percentage_label_, "--%");
        lv_label_set_text(battery_label_, "");
    }
    // Hysteresis avoids repeated alerts when the gauge oscillates near 15%.
    if (valid && level <= 15 && !low_battery_) {
        low_battery_ = true;
        if (!panel_asleep_ && !menu_open_) ShowNotification("电量较低，请及时充电", 5000);
    } else if (!valid || level >= 20) low_battery_ = false;
    lv_obj_set_style_text_color(percentage_label_,
        lv_color_hex(!valid ? 0x888888 : low_battery_ ? 0xD95270 : 0x379B87), 0);
    if (volume_until_ && now >= volume_until_) {
        volume_until_ = 0;
        lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
        RefreshContent();
    }
    RefreshPortrait();
    if (!volume_until_) RefreshContent();
}

void FoloDisplay::ShowVolume(int volume) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    Activity();
    ShowNotification((Lang::Strings::VOLUME + std::to_string(volume) + "%").c_str(), 2500);
    if (FullTextState(state_)) return;
    volume_until_ = esp_timer_get_time() + 2500000;
    lv_bar_set_value(volume_bar_, std::clamp(volume, 0, 100), LV_ANIM_OFF);
    lv_obj_remove_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
}

void FoloDisplay::ShowMenu(const char* text) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    Activity();
    menu_open_ = true;
    volume_until_ = 0;
    lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(menu_label_, text);
    lv_obj_remove_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
    RefreshContent();
}

void FoloDisplay::CloseMenu() {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    menu_open_ = false;
    lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
    RefreshContent();
}

void FoloDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (!on) Activity();
    // Only the inactivity policy turns off the screen; network power saving must not.
}

void FoloDisplay::NotifyUserActivity() {
    DisplayLockGuard lock(this);
    if (setup_ui_called_) Activity();
}

void FoloDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (image) {
        auto dsc = image->image_dsc();
        if (!dsc || !dsc->header.w || !dsc->header.h) return;
        esp_timer_stop(preview_timer_);
        const int scale = std::max(1, std::min(192 * 256 / int(dsc->header.w),
                                              180 * 256 / int(dsc->header.h)));
        lv_image_set_src(preview_image_, dsc);
        lv_image_set_pivot(preview_image_, 0, 0);
        lv_image_set_scale(preview_image_, scale);
        lv_obj_set_pos(preview_image_, (240 - int(dsc->header.w) * scale / 256) / 2,
                       54 + (188 - int(dsc->header.h) * scale / 256) / 2);
        preview_image_cached_ = std::move(image);
        Activity();
        ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
    } else {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(preview_image_, nullptr);
        preview_image_cached_.reset();
    }
    RefreshContent();
}
