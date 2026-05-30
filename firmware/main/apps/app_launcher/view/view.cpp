/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"
#include <mooncake_log.h>
#include <assets/assets.h>
#include <functional>
#include <hal/hal.h>
#include <cstdint>
#include <vector>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ArduinoJson.hpp>
#include <stackchan/stackchan.h>
#include "hal/board/hal_bridge.h"
#include <board.h>
#include <audio/audio_codec.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include <application.h>
#include <audio/audio_service.h>

using namespace view;
using namespace uitk;
using namespace uitk::lvgl_cpp;

/* -------------------------------------------------------------------------- */
/*                    Dynamic backgroud color handle class                    */
/* -------------------------------------------------------------------------- */
class DynamicBgColor {
public:
    std::function<void(const uint32_t& bgColor)> onBgColorChanged;

    void init(const std::vector<uint32_t>& stepColors, int stepGap)
    {
        _step_colors = stepColors;
        _step_gap    = stepGap;

        _bg_color.duration = 0.3;
        _bg_color.begin();

        jumpTo(0);
    }

    void jumpTo(int index)
    {
        if (index < 0 || index >= _step_colors.size()) {
            return;
        }
        _bg_color.teleport(_step_colors[index]);
        _current_index = index;

        if (onBgColorChanged) {
            onBgColorChanged(_step_colors[index]);
        }
    }

    void update(int scrollValue)
    {
        _last_index = _current_index;

        // Update current index
        _current_index = (scrollValue + _step_gap / 2) / _step_gap;
        if (_current_index < 0) {
            _current_index = 0;
        }
        if (_current_index >= _step_colors.size()) {
            _current_index = _step_colors.size() - 1;
        }

        // If index changed
        if (_last_index != _current_index) {
            // mclog::tagInfo(_tag, "index changed from {} to {}", _last_index, _current_index);
            _bg_color = _step_colors[_current_index];
        }

        // Update background color
        _bg_color.update();
        if (!_bg_color.done()) {
            if (onBgColorChanged) {
                onBgColorChanged(_bg_color.toHex());
            }
        }
    }

private:
    std::vector<uint32_t> _step_colors;
    int _current_index = 0;
    int _last_index    = 0;
    int _step_gap      = 0;
    color::AnimateRgb_t _bg_color;
};

/* -------------------------------------------------------------------------- */
/*                               Page indicator                               */
/* -------------------------------------------------------------------------- */
class PageIndicator {
public:
    const int dot_size     = 8;
    const int dot_size_big = 14;
    const int dot_gap      = 16;

    void init(int pageNum, int pageGap, lv_obj_t* parent, int posX, int posY)
    {
        _page_num = pageNum;
        _page_gap = pageGap;

        _panel = std::make_unique<Container>(parent);
        _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _panel->addFlag(LV_OBJ_FLAG_FLOATING);
        _panel->setAlign(LV_ALIGN_CENTER);
        _panel->setPadding(0, 0, 24, 24);
        _panel->setPos(posX, posY);
        _panel->setBorderWidth(0);
        _panel->setHeight(24);
        _panel->setWidth((pageNum * dot_size) + (pageNum - 1) * (dot_gap - dot_size) + 24 * 2);
        _panel->setBgOpa(0);

        for (int i = 0; i < pageNum; i++) {
            _dots.push_back(std::make_unique<Container>(_panel->get()));
            _dots.back()->setAlign(LV_ALIGN_CENTER);
            _dots.back()->setPos(i * dot_gap - (pageNum - 1) * dot_gap / 2, 0);
            _dots.back()->setBgColor(lv_color_hex(0xFFFFFF));
            _dots.back()->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
            _dots.back()->setRadius(LV_RADIUS_CIRCLE);
            _dots.back()->setSize(dot_size, dot_size);
            _dots.back()->setBorderWidth(0);
        }

        jumpTo(0);
    }

    void jumpTo(int index)
    {
        if (index < 0 || index >= _page_num) {
            return;
        }
        _current_index = index;
        _last_index    = index;
        update_dots();
    }

    void update(int scrollValue)
    {
        _last_index = _current_index;

        // Calculate absolute index
        int abs_index = (scrollValue + _page_gap / 2) / _page_gap;

        // Map to 0 ~ N-1
        _current_index = abs_index % _page_num;
        if (_current_index < 0) {
            _current_index += _page_num;
        }

        if (_last_index != _current_index) {
            update_dots();
        }
    }

private:
    int _page_num = 0;
    int _page_gap = 0;

    int _current_index = 0;
    int _last_index    = 0;

    std::unique_ptr<Container> _panel;
    std::vector<std::unique_ptr<Container>> _dots;

    void update_dots()
    {
        for (int i = 0; i < _page_num; i++) {
            if (i == _current_index) {
                _dots[i]->setSize(dot_size_big, dot_size_big);
                _dots[i]->setOpa(255);
            } else {
                _dots[i]->setSize(dot_size, dot_size);
                _dots[i]->setOpa(128);
            }
        }
    }
};

/* -------------------------------------------------------------------------- */
/*                             Dynamic icon label                             */
/* -------------------------------------------------------------------------- */
class DynamicIconLabel {
public:
    const int show_range      = 50;
    const int hidden_pos_y    = -150;
    const int visible_pos_y   = -99;
    const int transition_zone = 30;

    void init(const std::vector<std::string>& iconLabelTexts, int iconGap, lv_obj_t* parent)
    {
        _icon_label_texts = iconLabelTexts;
        _icon_gap         = iconGap;

        // Create floating label
        _label = std::make_unique<Label>(parent);
        _label->setTextColor(lv_color_hex(0x000000));
        _label->setTextFont(&MontserratSemiBold26);
        _label->setAlign(LV_ALIGN_CENTER);
        _label->addFlag(LV_OBJ_FLAG_FLOATING);
        _label->setOpa(233);

        // Setup animation
        _pos_y_anim                                 = std::make_unique<AnimateValue>();
        _pos_y_anim->springOptions().visualDuration = 0.3;
        _pos_y_anim->springOptions().bounce         = 0.1;
        _pos_y_anim->begin();

        jumpTo(0);
    }

    void jumpTo(int index)
    {
        if (index < 0 || index >= _icon_label_texts.size()) {
            return;
        }

        _current_index = index;
        _last_index    = index;
        _is_visible    = true;

        // Update label
        _label->setText(_icon_label_texts[index]);
        _label->setPos(0, visible_pos_y);
        _pos_y_anim->teleport(visible_pos_y);
    }

    void update(int scrollValue)
    {
        _last_index = _current_index;

        // Calculate current icon index and distance to icon center
        _current_index        = (scrollValue + _icon_gap / 2) / _icon_gap;
        int icon_center_pos_x = _current_index * _icon_gap;
        int distance_to_icon  = std::abs(scrollValue - icon_center_pos_x);

        // Clamp index
        if (_current_index < 0) {
            _current_index = 0;
        }
        if (_current_index >= _icon_label_texts.size()) {
            _current_index = _icon_label_texts.size() - 1;
        }

        // Check if label should be visible
        bool should_be_visible = (distance_to_icon <= show_range);

        // If index changed, update label text
        if (_last_index != _current_index) {
            _label->setText(_icon_label_texts[_current_index]);
        }

        // Handle visibility state change
        if (should_be_visible && !_is_visible) {
            // Show label
            _pos_y_anim->move(visible_pos_y);
            _is_visible = true;
        } else if (!should_be_visible && _is_visible) {
            // Hide label
            _pos_y_anim->move(hidden_pos_y);
            _is_visible = false;
        }

        // Update animation and apply position
        _pos_y_anim->update();
        _label->setY(_pos_y_anim->directValue());

        // Update opacity based on distance when in transition zone
        if (should_be_visible && distance_to_icon > (show_range - transition_zone)) {
            // Fade out as approaching edge
            float fade_ratio = 1.0f - (float)(distance_to_icon - (show_range - transition_zone)) / transition_zone;
            _label->setOpa(233 * fade_ratio);
        } else if (should_be_visible) {
            _label->setOpa(233);
        }
    }

private:
    std::vector<std::string> _icon_label_texts;
    int _icon_gap      = 0;
    int _current_index = 0;
    int _last_index    = 0;
    bool _is_visible   = false;

    std::unique_ptr<Label> _label;
    std::unique_ptr<AnimateValue> _pos_y_anim;
};

static std::string _tag        = "LauncherView";
static constexpr int _icon_gap = 320;
// Create 5 copies: [0:Backup] [1:Buffer] [2:Main] [3:Buffer] [4:Backup]
static constexpr int _loop_copies       = 5;
static constexpr int _center_copy_index = 2;

static int _last_clicked_icon_pos_x = -1;
static std::unique_ptr<DynamicBgColor> _dynamic_bg_color;
static std::unique_ptr<PageIndicator> _page_indicator;
static std::unique_ptr<DynamicIconLabel> _dynamic_icon_label;

LauncherView::~LauncherView()
{
    _icon_images.clear();
    _icon_panels.clear();
    _lr_indicators_images.clear();
    _lr_indicator_panels.clear();
    _panel.reset();
    _dynamic_bg_color.reset();
    _page_indicator.reset();
    _dynamic_icon_label.reset();
}

/* -------------------------------------------------------------------------- */
/*            Local STT + LLM + TTS 会話ループ (POST /stt-chat-tts)           */
/* -------------------------------------------------------------------------- */
// pi5-3.local FastAPI エンドポイント
// STT-chat-TTS: raw WAV body → Whisper → Ollama → VOICEVOX → WAV response
#define LOCAL_STT_TTS_URL   "http://192.168.1.235:8000/stt-chat-tts"
#define TTS_WAV_BUF_SIZE    (256 * 1024)  // TTS応答用 256KB PSRAM
#define WAV_HEADER_SIZE     44

// STT録音パラメータ（16000Hz mono, 4秒）
#define STT_SAMPLE_RATE     16000
#define STT_RECORD_SEC      4
#define STT_RECORD_SAMPLES  (STT_SAMPLE_RATE * STT_RECORD_SEC)
// 録音WAVバッファ: WAVヘッダー + PCMデータ
#define STT_WAV_BUF_SIZE    (WAV_HEADER_SIZE + STT_RECORD_SAMPLES * 2)

static const char* LOCAL_LLM_TAG = "LocalLLM";

// STT+TTS応答を受け取るコンテキスト
struct SttTtsRespCtx {
    uint8_t* buf;
    size_t   len;
    size_t   max_len;
    char     emotion[32];
};

// WAV ヘッダーを buf[0..43] に書き込む（PCMバイト数・サンプルレート指定）
static void build_wav_header(uint8_t* buf, uint32_t pcm_bytes, uint32_t sample_rate)
{
    auto le32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xff; p[1] = (v>>8) & 0xff;
        p[2] = (v>>16) & 0xff; p[3] = (v>>24) & 0xff;
    };
    auto le16 = [](uint8_t* p, uint16_t v) {
        p[0] = v & 0xff; p[1] = (v>>8) & 0xff;
    };
    memcpy(buf,      "RIFF", 4);
    le32(buf +  4, 36 + pcm_bytes);     // file size - 8
    memcpy(buf + 8,  "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    le32(buf + 16, 16);                 // fmt chunk size
    le16(buf + 20, 1);                  // PCM
    le16(buf + 22, 1);                  // mono
    le32(buf + 24, sample_rate);
    le32(buf + 28, sample_rate * 2);    // byte rate (16bit mono)
    le16(buf + 32, 2);                  // block align
    le16(buf + 34, 16);                 // bits per sample
    memcpy(buf + 36, "data", 4);
    le32(buf + 40, pcm_bytes);
}

// HTTP イベントハンドラ: X-Emotion ヘッダー取得 + WAV ボディ蓄積
static esp_err_t stt_tts_http_handler(esp_http_client_event_t* evt)
{
    auto* ctx = static_cast<SttTtsRespCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && strcasecmp(evt->header_key, "X-Emotion") == 0 && evt->header_value) {
                strncpy(ctx->emotion, evt->header_value, sizeof(ctx->emotion) - 1);
                ctx->emotion[sizeof(ctx->emotion) - 1] = '\0';
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len > 0 && ctx->buf) {
                size_t space = ctx->max_len - ctx->len;
                size_t n = ((size_t)evt->data_len < space) ? (size_t)evt->data_len : space;
                if (n > 0) {
                    memcpy(ctx->buf + ctx->len, evt->data, n);
                    ctx->len += n;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// アバターの準備ができるまで待つ（最大120秒）
static bool wait_for_avatar()
{
    const int kIntervalMs = 500;
    const int kMaxMs      = 120000;
    hal_bridge::disply_lvgl_lock();
    auto& sc = GetStackChan();
    for (int waited = 0; !sc.hasAvatar() && waited < kMaxMs; waited += kIntervalMs) {
        hal_bridge::disply_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
        hal_bridge::disply_lvgl_lock();
    }
    bool ok = sc.hasAvatar();
    hal_bridge::disply_lvgl_unlock();
    return ok;
}

// WAV 再生（PCM を AudioCodec に流す）
static void play_wav(const uint8_t* wav_buf, size_t wav_len)
{
    if (wav_len <= WAV_HEADER_SIZE) {
        ESP_LOGW(LOCAL_LLM_TAG, "WAV too small: %u", (unsigned)wav_len);
        return;
    }
    auto* audio_codec = Board::GetInstance().GetAudioCodec();
    if (!audio_codec) {
        ESP_LOGE(LOCAL_LLM_TAG, "AudioCodec unavailable");
        return;
    }
    const int16_t* pcm          = reinterpret_cast<const int16_t*>(wav_buf + WAV_HEADER_SIZE);
    size_t         total_samples = (wav_len - WAV_HEADER_SIZE) / sizeof(int16_t);
    const size_t   kChunk        = 512;

    audio_codec->EnableOutput(true);
    size_t offset = 0;
    while (offset < total_samples) {
        size_t n = std::min(kChunk, total_samples - offset);
        std::vector<int16_t> chunk(pcm + offset, pcm + offset + n);
        audio_codec->OutputData(chunk);
        offset += n;
    }
    audio_codec->EnableOutput(false);
}

// 1回の会話ループ:
//   1. マイクから STT_RECORD_SEC 秒録音 (AudioService tee)
//   2. WAV を POST /stt-chat-tts
//   3. 返ってきた WAV を再生
static bool do_stt_voice_chat()
{
    // ── 1. STT録音バッファ確保 (WAVヘッダー44B + PCM) ──
    uint8_t* stt_wav_buf = static_cast<uint8_t*>(
        heap_caps_malloc(STT_WAV_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!stt_wav_buf) {
        ESP_LOGE(LOCAL_LLM_TAG, "STT WAV alloc failed");
        return false;
    }
    int16_t* pcm_buf = reinterpret_cast<int16_t*>(stt_wav_buf + WAV_HEADER_SIZE);

    // ── 2. 録音開始 ──
    auto& audio_svc = Application::GetInstance().GetAudioService();
    ESP_LOGI(LOCAL_LLM_TAG, "Recording %ds @ %dHz...", STT_RECORD_SEC, STT_SAMPLE_RATE);

    // アバターに「聞いています」表情を設定
    hal_bridge::disply_lvgl_lock();
    auto& sc = GetStackChan();
    if (sc.hasAvatar()) {
        sc.addModifier(std::make_unique<stackchan::TimedEmotionModifier>(
            stackchan::avatar::Emotion::Doubt, STT_RECORD_SEC * 1000 + 500));
    }
    hal_bridge::disply_lvgl_unlock();

    audio_svc.StartSttRecording(pcm_buf, STT_RECORD_SAMPLES);

    // 録音完了まで待機（タイムアウト = STT_RECORD_SEC + 2秒）
    const int kTimeoutMs = (STT_RECORD_SEC + 2) * 1000;
    for (int elapsed = 0; !audio_svc.IsSttRecordingDone() && elapsed < kTimeoutMs; elapsed += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    audio_svc.StopSttRecording();

    size_t recorded = audio_svc.GetSttRecordedCount();
    ESP_LOGI(LOCAL_LLM_TAG, "Recorded %u samples", (unsigned)recorded);

    if (recorded == 0) {
        ESP_LOGW(LOCAL_LLM_TAG, "No audio recorded, skipping");
        heap_caps_free(stt_wav_buf);
        return false;
    }

    // ── 3. WAVヘッダーを先頭に書き込む ──
    uint32_t pcm_bytes = (uint32_t)(recorded * sizeof(int16_t));
    build_wav_header(stt_wav_buf, pcm_bytes, STT_SAMPLE_RATE);
    size_t wav_send_size = WAV_HEADER_SIZE + (size_t)pcm_bytes;

    // ── 4. TTS応答バッファを確保 ──
    SttTtsRespCtx resp_ctx = {};
    resp_ctx.max_len = TTS_WAV_BUF_SIZE;
    resp_ctx.buf     = static_cast<uint8_t*>(
        heap_caps_malloc(resp_ctx.max_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!resp_ctx.buf) {
        ESP_LOGE(LOCAL_LLM_TAG, "TTS resp alloc failed");
        heap_caps_free(stt_wav_buf);
        return false;
    }

    // ── 5. POST /stt-chat-tts (raw WAV body) ──
    ESP_LOGI(LOCAL_LLM_TAG, "POST /stt-chat-tts: %u bytes...", (unsigned)wav_send_size);

    esp_http_client_config_t cfg = {};
    cfg.url           = LOCAL_STT_TTS_URL;
    cfg.method        = HTTP_METHOD_POST;
    cfg.timeout_ms    = 120000;  // STT + Ollama + VOICEVOX
    cfg.user_data     = &resp_ctx;
    cfg.event_handler = stt_tts_http_handler;
    cfg.buffer_size   = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(LOCAL_LLM_TAG, "HTTP init failed");
        heap_caps_free(stt_wav_buf);
        heap_caps_free(resp_ctx.buf);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, reinterpret_cast<const char*>(stt_wav_buf), (int)wav_send_size);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    heap_caps_free(stt_wav_buf);

    if (err != ESP_OK) {
        ESP_LOGE(LOCAL_LLM_TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        heap_caps_free(resp_ctx.buf);
        return false;
    }
    ESP_LOGI(LOCAL_LLM_TAG, "Response: emotion=%s wav=%u bytes",
             resp_ctx.emotion, (unsigned)resp_ctx.len);

    // ── 6. アバターに表情を設定し WAV 再生 ──
    std::string emotion_str = resp_ctx.emotion[0] ? resp_ctx.emotion : "idle";
    stackchan::avatar::Emotion emo = stackchan::avatar::Emotion::Neutral;
    if (emotion_str == "happy")                        emo = stackchan::avatar::Emotion::Happy;
    else if (emotion_str == "thinking" || emotion_str == "confused") emo = stackchan::avatar::Emotion::Doubt;

    int speech_ms = (int)((resp_ctx.len - WAV_HEADER_SIZE) / sizeof(int16_t) * 1000 / 24000) + 1000;
    if (speech_ms > 30000) speech_ms = 30000;

    hal_bridge::disply_lvgl_lock();
    if (sc.hasAvatar()) {
        sc.addModifier(std::make_unique<stackchan::TimedEmotionModifier>(emo, speech_ms));
    }
    hal_bridge::disply_lvgl_unlock();

    play_wav(resp_ctx.buf, resp_ctx.len);
    heap_caps_free(resp_ctx.buf);

    ESP_LOGI(LOCAL_LLM_TAG, "Chat cycle done");
    return true;
}

static void local_llm_task(void* pvParameters)
{
    // WiFi接続待ち（getWifiStatus ポーリング）
    ESP_LOGI(LOCAL_LLM_TAG, "Waiting for WiFi...");
    const int kWifiMaxMs  = 60000;
    const int kWifiStepMs = 1000;
    bool wifi_ok = false;
    for (int waited = 0; waited < kWifiMaxMs; waited += kWifiStepMs) {
        if (GetHAL().getWifiStatus() != WifiStatus::None) {
            wifi_ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kWifiStepMs));
    }
    if (!wifi_ok) {
        ESP_LOGW(LOCAL_LLM_TAG, "WiFi not connected after %ds, aborting", kWifiMaxMs / 1000);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(LOCAL_LLM_TAG, "WiFi connected");

    // アバター準備待ち
    if (!wait_for_avatar()) {
        ESP_LOGW(LOCAL_LLM_TAG, "Avatar not ready, aborting");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(LOCAL_LLM_TAG, "Avatar ready. Starting voice chat loop...");

    // 会話ループ
    while (true) {
        do_stt_voice_chat();
        // 次の録音まで少し間隔を開ける
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* -------------------------------------------------------------------------- */

void LauncherView::init(std::vector<mooncake::AppProps_t> appPorps)
{
    mclog::tagInfo(_tag, "init");

    // STT+LLM+TTS 会話ループタスクを起動
    xTaskCreate(local_llm_task, "local_llm", 24576, nullptr, 5, nullptr);

    /* ------------------------------ Screen setup ------------------------------ */
    ScreenActive screen;
    screen.removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    /* ---------------------------------- Panel --------------------------------- */
    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setAlign(LV_ALIGN_CENTER);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setBorderWidth(0);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _panel->setBgColor(lv_color_hex(0x33CC99));
    _panel->addFlag(LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scroll_snap_x(_panel->get(), LV_SCROLL_SNAP_CENTER);

    /* ---------------------------------- Icons --------------------------------- */
    int icon_x = 0;
    int icon_y = 0;
    std::vector<std::string> icon_label_texts;
    std::vector<uint32_t> step_colors;

    // Loop multiple times to create fake infinite scroll
    for (int loop = 0; loop < _loop_copies; loop++) {
        for (const auto& props : appPorps) {
            // Icon panel
            _icon_panels.push_back(std::make_unique<Container>(_panel->get()));
            _icon_panels.back()->setAlign(LV_ALIGN_CENTER);
            _icon_panels.back()->setSize(190, 160);
            _icon_panels.back()->setPos(icon_x, icon_y);
            _icon_panels.back()->setBorderWidth(0);
            _icon_panels.back()->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
            _icon_panels.back()->setBgOpa(0);

            // Icon click callback
            auto app_id = props.appID;
            auto pos_x  = icon_x;
            _icon_panels.back()->onClick().connect([&, app_id, pos_x]() {
                _clicked_app_id          = app_id;
                _last_clicked_icon_pos_x = pos_x;
            });

            // Keep track of data for helpers
            icon_label_texts.push_back(props.info.name);

            uint32_t color = 0xDADADA;
            if (props.info.userData != nullptr) {
                color = *(uint32_t*)props.info.userData;
            }
            step_colors.push_back(color);

            // Icon image
            if (props.info.icon != nullptr) {
                _icon_images.push_back(std::make_unique<Image>(_icon_panels.back()->get()));
                _icon_images.back()->setSrc(props.info.icon);
                _icon_images.back()->setAlign(LV_ALIGN_CENTER);
            }

            icon_x += _icon_gap;
        }
    }

    /* ------------------------------ LR indicators ----------------------------- */
    // Scroll to nearby icon handler
    auto scroll_to_nearby_icon = [&](int direction) {
        auto current_scroll_x = _panel->getScrollX();
        int current_index     = (current_scroll_x + _icon_gap / 2) / _icon_gap;
        int target_index      = current_index + direction;

        int target_x        = target_index * _icon_gap;
        int scroll_distance = target_x - current_scroll_x;
        _panel->scrollBy(-scroll_distance, 0, LV_ANIM_ON);
    };

    // Go left indicator
    _lr_indicator_panels.push_back(std::make_unique<Container>(_panel->get()));
    _lr_indicator_panels.back()->setAlign(LV_ALIGN_CENTER);
    _lr_indicator_panels.back()->setSize(52, 160);
    _lr_indicator_panels.back()->setPos(-134, 0);
    _lr_indicator_panels.back()->setBorderWidth(0);
    _lr_indicator_panels.back()->addFlag(LV_OBJ_FLAG_FLOATING);
    _lr_indicator_panels.back()->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _lr_indicator_panels.back()->setBgOpa(0);
    _lr_indicator_panels.back()->onClick().connect([scroll_to_nearby_icon]() { scroll_to_nearby_icon(-1); });

    _lr_indicators_images.push_back(std::make_unique<Image>(_lr_indicator_panels.back()->get()));
    static auto icon_indicator_left = assets::get_image("icon_indicator_left.bin");
    _lr_indicators_images.back()->setSrc(&icon_indicator_left);
    _lr_indicators_images.back()->align(LV_ALIGN_CENTER, 0, 0);

    // Go right indicator
    _lr_indicator_panels.push_back(std::make_unique<Container>(_panel->get()));
    _lr_indicator_panels.back()->setAlign(LV_ALIGN_CENTER);
    _lr_indicator_panels.back()->setSize(52, 160);
    _lr_indicator_panels.back()->setPos(134, 0);
    _lr_indicator_panels.back()->setBorderWidth(0);
    _lr_indicator_panels.back()->addFlag(LV_OBJ_FLAG_FLOATING);
    _lr_indicator_panels.back()->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _lr_indicator_panels.back()->setBgOpa(0);
    _lr_indicator_panels.back()->onClick().connect([scroll_to_nearby_icon]() { scroll_to_nearby_icon(1); });

    _lr_indicators_images.push_back(std::make_unique<Image>(_lr_indicator_panels.back()->get()));
    static auto icon_indicator_right = assets::get_image("icon_indicator_right.bin");
    _lr_indicators_images.back()->setSrc(&icon_indicator_right);
    _lr_indicators_images.back()->align(LV_ALIGN_CENTER, 0, 0);

    /* ---------------------------- Dynamic bg color ---------------------------- */
    _dynamic_bg_color = std::make_unique<DynamicBgColor>();

    _dynamic_bg_color->onBgColorChanged = [&](const uint32_t& bgColor) {
        // mclog::tagInfo(_tag, "bg color changed to {:06X}", bgColor);
        _panel->setBgColor(lv_color_hex(bgColor));
    };

    _dynamic_bg_color->init(step_colors, _icon_gap);

    /* ------------------------------ Page indicator ---------------------------- */
    _page_indicator = std::make_unique<PageIndicator>();
    // Page indicator only needs to know the real app count (N), not N * copies
    _page_indicator->init(appPorps.size(), _icon_gap, _panel->get(), 0, 103);

    /* --------------------------- Dynamic icon label --------------------------- */
    _dynamic_icon_label = std::make_unique<DynamicIconLabel>();
    _dynamic_icon_label->init(icon_label_texts, _icon_gap, _panel->get());

    /* ----------------------------- History restore ---------------------------- */
    bool need_restore      = false;
    int restore_icon_pos_x = -1;

    // Normal start pos (Center of the repeated sets)
    int base_offset_rounds = _center_copy_index * appPorps.size();
    int default_start_x    = base_offset_rounds * _icon_gap;

    // If warm boot was requested
    if (GetHAL().getWarmRebootTarget() >= 0) {
        auto app_index = GetHAL().getWarmRebootTarget();
        mclog::tagInfo(_tag, "warm boot was requested, app index: {}", app_index);
        app_index = uitk::clamp(app_index, 0, static_cast<int>(appPorps.size()) - 1);

        // Restore to center set
        restore_icon_pos_x = (base_offset_rounds + app_index) * _icon_gap;
        need_restore       = true;
        GetHAL().clearWarmRebootRequest();
    }

    if (_last_clicked_icon_pos_x != -1) {
        // Just restore where they left off, it should be in a valid range
        // mclog::tagInfo(_tag, "navigate to last clicked icon, pos x: {}", _last_clicked_icon_pos_x);
        restore_icon_pos_x       = _last_clicked_icon_pos_x;
        need_restore             = true;
        _last_clicked_icon_pos_x = -1;
    }

    if (need_restore) {
        _panel->scrollBy(-restore_icon_pos_x, 0, LV_ANIM_OFF);

        _dynamic_bg_color->jumpTo(restore_icon_pos_x / _icon_gap);
        _page_indicator->jumpTo(restore_icon_pos_x / _icon_gap);
        _dynamic_icon_label->jumpTo(restore_icon_pos_x / _icon_gap);

        _state = STATE_NORMAL;
    }

    // If first create
    else {
        // Init at Center Set
        _panel->scrollBy(-default_start_x, 0, LV_ANIM_OFF);
        _dynamic_bg_color->jumpTo(default_start_x / _icon_gap);
        _page_indicator->jumpTo(default_start_x / _icon_gap);
        _dynamic_icon_label->jumpTo(default_start_x / _icon_gap);

        // Setup startup animation
        // x for pos_y, y for radius
        _startup_anim = std::make_unique<AnimateVector2>();

        _startup_anim->x.springOptions().damping        = 12.0;
        _startup_anim->y.delay                          = 0.15;
        _startup_anim->y.springOptions().visualDuration = 0.4;
        _startup_anim->y.springOptions().bounce         = 0.05;

        _startup_anim->teleport(240, 120);
        _panel->setY(_startup_anim->directValue().x);
        _panel->setRadius(_startup_anim->directValue().y);
        _startup_anim->move(0, 0);

        _state = STATE_STARTUP;
    }

    // Destory boot logo label
    GetHAL().bootLogo.reset();
}

void LauncherView::update()
{
    switch (_state) {
        case STATE_STARTUP:
            handle_state_startup();
            break;
        case STATE_NORMAL:
            handle_state_normal();
            break;
        default:
            break;
    }
}

void LauncherView::handle_state_startup()
{
    _startup_anim->update();

    _panel->setY(_startup_anim->directValue().x);
    _panel->setRadius(_startup_anim->directValue().y);

    if (_startup_anim->done()) {
        _startup_anim.reset();
        _state = STATE_NORMAL;
    }
}

void LauncherView::handle_state_normal()
{
    if (_clicked_app_id != -1) {
        if (onAppClicked) {
            onAppClicked(_clicked_app_id);
        }
        _clicked_app_id = -1;
    }

    // We get total size from underlying icons count / copies
    int total_icons   = _icon_panels.size();
    int icons_per_set = total_icons / _loop_copies;
    int set_width_px  = icons_per_set * _icon_gap;

    // Check boundaries
    // If we are mostly in Copy 1, jump to Copy 2
    // If we are mostly in Copy 3, jump to Copy 2
    // Copy Index: 0 1 [2] 3 4

    int current_scroll_x = _panel->getScrollX();

    // Define safe zone (Copy 2)
    int center_set_start_x = _center_copy_index * set_width_px;

    // Thresholds: midpoint of Wrap sets
    int left_trigger_limit  = 1 * set_width_px + (set_width_px / 2);  // Middle of Set 1
    int right_trigger_limit = 3 * set_width_px + (set_width_px / 2);  // Middle of Set 3

    // Wrap-around Logic
    // Only perform teleport if we are NOT in an automated scroll animation
    // (To avoid interrupting the snap/scroll-to animation which would leave us stuck between icons)
    // However, if the user is manually dragging (PRESSED), we MUST teleport to allow infinite drag.
    bool is_auto_scrolling = lv_obj_is_scrolling(_panel->get()) && !lv_obj_has_state(_panel->get(), LV_STATE_PRESSED);

    if (!is_auto_scrolling) {
        if (current_scroll_x < left_trigger_limit) {
            // Too far left (Set 1), warp right to Set 2
            // scrollBy(-val) increases scroll_x
            _panel->scrollBy(-set_width_px, 0, LV_ANIM_OFF);
        } else if (current_scroll_x > right_trigger_limit) {
            // Too far right (Set 3), warp left to Set 2
            // scrollBy(+val) decreases scroll_x
            _panel->scrollBy(set_width_px, 0, LV_ANIM_OFF);
        }
    }

    int scroll_x = _panel->getScrollX();
    // mclog::tagInfo(_tag, "scroll x: {}", scroll_x);

    _dynamic_bg_color->update(scroll_x);
    _page_indicator->update(scroll_x);
    _dynamic_icon_label->update(scroll_x);
}
