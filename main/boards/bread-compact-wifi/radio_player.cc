#include "radio_player.h"

#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <vector>

#include "board.h"
#include "audio_codec.h"
#include "display/display.h"
#include "application.h"

#if __has_include("simple_dec/esp_audio_simple_dec.h")
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#else
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#endif

#if __has_include("esp_audio_dec_default.h")
#include "esp_audio_dec_default.h"
#endif

#if __has_include("esp_aac_dec.h")
#include "esp_aac_dec.h"
#endif

#include "esp_ae_rate_cvt.h"

#define TAG "RadioPlayer"

#ifndef RATE_CVT_CFG
#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }
#endif

static void EnsureDecodersRegistered() {
    static bool registered = false;
    if (!registered) {
#if __has_include("esp_audio_dec_default.h")
        esp_audio_dec_register_default();
#endif
        esp_audio_simple_dec_register_default();
        registered = true;
        ESP_LOGI(TAG, "Audio decoders registered");
    }
}

RadioPlayer& RadioPlayer::GetInstance() {
    static RadioPlayer instance;
    return instance;
}

RadioPlayer::RadioPlayer() {
    EnsureDecodersRegistered();
    codec_ = Board::GetInstance().GetAudioCodec();
}

RadioPlayer::~RadioPlayer() {
    Stop();
}

std::string RadioPlayer::GetCurrentStation() const {
    return station_name_;
}

std::string RadioPlayer::GetCurrentUrl() const {
    return url_;
}

void RadioPlayer::TaskTrampoline(void* arg) {
    auto* self = static_cast<RadioPlayer*>(arg);
    self->WorkerTask();
}

bool RadioPlayer::Play(const std::string& input_url, const std::string& station_name) {
    if (input_url.empty() && station_name.empty()) {
        ESP_LOGE(TAG, "Cannot play empty stream URL and station name");
        return false;
    }

    std::string resolved_url = input_url;
    std::string resolved_name = station_name.empty() ? "Radio" : station_name;

    std::string lower_input = input_url;
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);
    std::string lower_name = station_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    // Smart remap for dead URLs or station name inputs to verified live broadcasts
    if (lower_input.find("loveradio") != std::string::npos || lower_name.find("love radio") != std::string::npos || lower_name.find("dzmb") != std::string::npos) {
        resolved_url = "https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3";
        resolved_name = "Love Radio 90.7";
    } else if (lower_input.find("yesfm") != std::string::npos || lower_name.find("yes") != std::string::npos) {
        resolved_url = "https://azura.yesfm.com.ph/listen/yes_fm_manila/radio.mp3";
        resolved_name = "Yes The Best 101.1";
    } else if (lower_input.find("easyrock") != std::string::npos || lower_input.find("dwrk") != std::string::npos || lower_name.find("easy rock") != std::string::npos) {
        resolved_url = "https://azura.easyrock.com.ph/listen/easy_rock_manila/radio.mp3";
        resolved_name = "Easy Rock 96.3";
    } else if (lower_input.find("barangay") != std::string::npos || lower_name.find("barangay") != std::string::npos) {
        resolved_url = "http://28093.live.streamtheworld.com:3690/MORFM_S01AAC_SC";
        resolved_name = "Barangay LS 97.1";
    } else if (lower_input.find("mor1019") != std::string::npos || lower_name.find("mor") != std::string::npos) {
        resolved_url = "https://playerservices.streamtheworld.com/api/livestream-redirect/MORFM_S01.mp3";
        resolved_name = "MOR 101.9";
    } else if (lower_input.find("dzrh") != std::string::npos || lower_name.find("dzrh") != std::string::npos) {
        resolved_url = "https://azura.dzrh.com.ph/listen/dzrh_manila/radio.mp3";
        resolved_name = "DZRH News";
    } else if (lower_input.find("star") != std::string::npos || lower_name.find("star") != std::string::npos) {
        resolved_url = "https://stream-13.zeno.fm/g1pmt17nz9duv";
        resolved_name = "Star FM Manila";
    } else if (lower_input.find("wish1075") != std::string::npos || lower_name.find("wish") != std::string::npos) {
        resolved_url = "https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3";
        resolved_name = "Love Radio Manila";
    } else if (resolved_url.empty() || (resolved_url.find("http://") != 0 && resolved_url.find("https://") != 0)) {
        resolved_url = "https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3";
        resolved_name = "Love Radio 90.7";
    }

    // Stop current stream if running
    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = resolved_url;
        station_name_ = resolved_name;
        stop_requested_.store(false);
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification(station_name_, 4000);
        display->SetStatus("Playing Radio");
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        TaskTrampoline,
        "radio_worker",
        10240,
        this,
        5,
        &task_handle_,
        0
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio worker task");
        if (display) {
            display->ShowNotification("Play Failed", 3000);
        }
        return false;
    }

    is_playing_.store(true);
    ESP_LOGI(TAG, "Started radio stream: %s (%s)", station_name_.c_str(), url_.c_str());
    return true;
}

void RadioPlayer::Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!is_playing_.load() && task_handle_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stopping radio stream...");
    stop_requested_.store(true);

    int wait_ms = 0;
    while (task_handle_ != nullptr && wait_ms < 3000) {
        lock.unlock();
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms += 50;
        lock.lock();
    }

    is_playing_.store(false);
    stop_requested_.store(false);

    if (codec_ != nullptr && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("Radio Stopped", 2000);
        display->SetStatus("Ready");
    }
    ESP_LOGI(TAG, "Radio playback stopped cleanly");
}

void RadioPlayer::WorkerTask() {
    std::string stream_url;
    std::string display_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_url = url_;
        display_name = station_name_;
    }

    ESP_LOGI(TAG, "Connecting to stream: %s", stream_url.c_str());

    // Switch WiFi to high performance mode for continuous stream throughput
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("Connecting...", 3000);
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        if (display) display->ShowNotification("Network Error", 3000);
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_playing_.store(false);
            task_handle_ = nullptr;
        }
        vTaskDelete(nullptr);
        return;
    }

    http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaozhiRadio/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Icy-MetaData", "0");
    http->SetTimeout(12000);

    if (auto opened = http->Open("GET", stream_url); !opened) {
        ESP_LOGE(TAG, "HTTP open failed for %s: %s", stream_url.c_str(), opened.error().ToString().c_str());
        if (display) display->ShowNotification("Connect Failed", 3000);
        http->Close();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_playing_.store(false);
            task_handle_ = nullptr;
        }
        vTaskDelete(nullptr);
        return;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code || *status_code < 200 || *status_code >= 400) {
        ESP_LOGE(TAG, "HTTP stream error, status: %d", status_code ? *status_code : -1);
        if (display) display->ShowNotification("Stream Offline", 3000);
        http->Close();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_playing_.store(false);
            task_handle_ = nullptr;
        }
        vTaskDelete(nullptr);
        return;
    }

    // Determine codec type (MP3 or AAC)
    bool is_mp3 = false;
    std::string lower_url = stream_url;
    std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), ::tolower);
    if (lower_url.find(".mp3") != std::string::npos || lower_url.find("format=mp3") != std::string::npos ||
        lower_url.find("/mp3") != std::string::npos || lower_url.find("type=mp3") != std::string::npos) {
        is_mp3 = true;
    }

    ESP_LOGI(TAG, "Initializing decoder (type: %s)", is_mp3 ? "MP3" : "AAC");

    esp_audio_simple_dec_cfg_t dec_cfg = {};
#if __has_include("esp_aac_dec.h")
    esp_aac_dec_cfg_t aac_cfg = {};
    aac_cfg.aac_plus_enable = true;
#endif

    if (is_mp3) {
        dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        dec_cfg.dec_cfg = nullptr;
        dec_cfg.cfg_size = 0;
        dec_cfg.use_frame_dec = false;
    } else {
        dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
#if __has_include("esp_aac_dec.h")
        dec_cfg.dec_cfg = &aac_cfg;
        dec_cfg.cfg_size = sizeof(esp_aac_dec_cfg_t);
#else
        dec_cfg.dec_cfg = nullptr;
        dec_cfg.cfg_size = 0;
#endif
        dec_cfg.use_frame_dec = false;
    }

    esp_audio_simple_dec_handle_t dec_handle = nullptr;
    auto dec_err = esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
    if (dec_err != ESP_AUDIO_ERR_OK || dec_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to open simple decoder: %d", dec_err);
        if (display) display->ShowNotification("Decode Error", 3000);
        http->Close();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            is_playing_.store(false);
            task_handle_ = nullptr;
        }
        vTaskDelete(nullptr);
        return;
    }

    if (display) {
        display->ShowNotification(display_name, 3000);
        display->SetStatus("Playing");
    }

    // Audio buffers
    constexpr size_t kHttpReadBufSize = 2048;
    std::vector<uint8_t> http_buf(kHttpReadBufSize);
    constexpr size_t kPcmBufSize = 8192;
    std::vector<uint8_t> pcm_buf(kPcmBufSize);

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int current_sample_rate = 0;
    int target_sample_rate = codec_ ? codec_->output_sample_rate() : 24000;

    if (codec_ != nullptr) {
        // Lock prevents AudioService power-save timer from disabling
        // the I2S TX channel while we are actively streaming audio.
        codec_->LockOutput();
        codec_->EnableOutput(true);
    }

    ESP_LOGI(TAG, "Streaming audio (target rate %d Hz)...", target_sample_rate);

    while (!stop_requested_.load()) {
        auto dev_state = Application::GetInstance().GetDeviceState();
        // If Xiaozhi is speaking its initial confirmation to the user, wait until it finishes
        if (dev_state == kDeviceStateSpeaking) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        // If user triggers listening mode to speak a new command, stop radio stream
        if (dev_state == kDeviceStateListening) {
            ESP_LOGI(TAG, "User started speaking, stopping radio");
            break;
        }

        auto read_res = http->Read(reinterpret_cast<char*>(http_buf.data()), http_buf.size());
        if (!read_res) {
            ESP_LOGW(TAG, "Stream read failed: %s", read_res.error().ToString().c_str());
            break;
        }
        if (*read_res == 0) {
            ESP_LOGI(TAG, "Stream reached EOF");
            break;
        }

        // Auto-detect MP3 from magic bytes if not detected by URL
        if (!is_mp3 && *read_res >= 3) {
            if ((http_buf[0] == 'I' && http_buf[1] == 'D' && http_buf[2] == '3') ||
                (http_buf[0] == 0xFF && (http_buf[1] == 0xFB || http_buf[1] == 0xF3 || http_buf[1] == 0xF2))) {
                ESP_LOGI(TAG, "Detected MP3 stream header, switching to MP3 decoder");
                esp_audio_simple_dec_close(dec_handle);
                dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
                dec_cfg.dec_cfg = nullptr;
                dec_cfg.cfg_size = 0;
                esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
                is_mp3 = true;
            }
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = http_buf.data(),
            .len = static_cast<uint32_t>(*read_res),
            .eos = false,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        while (raw.len > 0 && !stop_requested_.load()) {
            auto current_dev_state = Application::GetInstance().GetDeviceState();
            if (current_dev_state == kDeviceStateSpeaking) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (current_dev_state == kDeviceStateListening) {
                break;
            }

            esp_audio_simple_dec_out_t out_frame = {
                .buffer = pcm_buf.data(),
                .len = static_cast<uint32_t>(pcm_buf.size()),
                .decoded_size = 0,
            };

            esp_audio_err_t ret = esp_audio_simple_dec_process(dec_handle, &raw, &out_frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                pcm_buf.resize(pcm_buf.size() * 2);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_CONTINUE) {
                break;
            }

            if (out_frame.decoded_size > 0 && codec_ != nullptr) {
                esp_audio_simple_dec_info_t info = {};
                esp_audio_simple_dec_get_info(dec_handle, &info);

                // Stereo to mono downmixing
                std::vector<int16_t> mono_pcm;
                if (info.channel == 2) {
                    size_t samples = out_frame.decoded_size / (sizeof(int16_t) * 2);
                    int16_t* src = reinterpret_cast<int16_t*>(out_frame.buffer);
                    mono_pcm.resize(samples);
                    for (size_t i = 0; i < samples; i++) {
                        mono_pcm[i] = static_cast<int16_t>(
                            (static_cast<int32_t>(src[i * 2]) + src[i * 2 + 1]) / 2);
                    }
                } else {
                    size_t samples = out_frame.decoded_size / sizeof(int16_t);
                    int16_t* src = reinterpret_cast<int16_t*>(out_frame.buffer);
                    mono_pcm.assign(src, src + samples);
                }

                // Sample rate conversion
                if (info.sample_rate > 0 && info.sample_rate != current_sample_rate) {
                    if (resampler != nullptr) {
                        esp_ae_rate_cvt_close(resampler);
                        resampler = nullptr;
                    }
                    current_sample_rate = info.sample_rate;
                    if (current_sample_rate != target_sample_rate) {
                        esp_ae_rate_cvt_cfg_t cvt_cfg =
                            RATE_CVT_CFG(current_sample_rate, target_sample_rate, ESP_AUDIO_MONO);
                        esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                    }
                }

                if (resampler != nullptr) {
                    uint32_t max_out = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono_pcm.size(), &max_out);
                    std::vector<int16_t> resampled(max_out);
                    uint32_t actual_out = max_out;
                    esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono_pcm.data(), mono_pcm.size(),
                                            (esp_ae_sample_t)resampled.data(), &actual_out);
                    resampled.resize(actual_out);
                    codec_->OutputData(resampled);
                } else {
                    codec_->OutputData(mono_pcm);
                }
            }

            if (raw.consumed > 0 && raw.consumed <= raw.len) {
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                raw.consumed = 0;
            } else {
                break;
            }
        }
    }

    // Cleanup resources
    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    if (dec_handle != nullptr) {
        esp_audio_simple_dec_close(dec_handle);
    }
    http->Close();

    if (codec_ != nullptr) {
        // Release the output lock so AudioService can manage idle power.
        codec_->UnlockOutput();
        if (codec_->output_enabled()) {
            codec_->EnableOutput(false);
        }
    }

    // Restore WiFi power save level
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (display) {
        display->SetStatus("Ready");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        is_playing_.store(false);
        task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "Radio worker task finished");
    vTaskDelete(nullptr);
}

std::string RadioPlayer::SearchStationsOnline(const std::string& query) {
    struct StationPreset {
        const char* name;
        const char* url;
        const char* codec;
        int bitrate;
    };
    static const StationPreset kPresets[] = {
        {"90.7 Love Radio Manila", "https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3", "MP3", 128},
        {"101.1 Yes The Best Manila", "https://azura.yesfm.com.ph/listen/yes_fm_manila/radio.mp3", "MP3", 128},
        {"96.3 Easy Rock Manila", "https://azura.easyrock.com.ph/listen/easy_rock_manila/radio.mp3", "MP3", 128},
        {"Star FM 102.7 Manila", "https://stream-13.zeno.fm/g1pmt17nz9duv", "AAC", 64},
        {"Barangay LS 97.1 Manila", "http://28093.live.streamtheworld.com:3690/MORFM_S01AAC_SC", "AAC", 64},
        {"MOR 101.9 FM", "https://playerservices.streamtheworld.com/api/livestream-redirect/MORFM_S01.mp3", "MP3", 128},
        {"DZRH News Manila", "https://azura.dzrh.com.ph/listen/dzrh_manila/radio.mp3", "MP3", 128},
        {"91.5 Win Radio Manila", "https://stream-31.zeno.fm/2ss1hgnu6hhvv", "MP3", 128},
        {"97.9 Home Radio Manila", "http://142.44.212.114:9071/stream", "AAC", 64},
    };

    std::string encoded_query;
    for (char c : query) {
        if (c == ' ') encoded_query += "%20";
        else if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') encoded_query += c;
    }

    std::string search_url;
    if (encoded_query.empty() || query == "Philippines" || query == "OPM" || query == "PH") {
        search_url = "https://de1.api.radio-browser.info/json/stations/search?countrycode=PH&limit=6&order=clickcount&reverse=true";
    } else {
        search_url = "https://de1.api.radio-browser.info/json/stations/search?countrycode=PH&name=" + encoded_query + "&limit=6";
    }

    cJSON* out_array = cJSON_CreateArray();
    bool online_success = false;

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (http) {
        http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaozhiRadio/1.0");
        http->SetHeader("Accept", "application/json");
        http->SetTimeout(6000);
        if (auto opened = http->Open("GET", search_url); opened) {
            auto status = http->GetStatusCode();
            if (status && *status >= 200 && *status < 300) {
                std::string body;
                char buf[1024];
                while (body.size() < 16384) {
                    auto read_res = http->Read(buf, sizeof(buf) - 1);
                    if (!read_res || *read_res == 0) break;
                    buf[*read_res] = '\0';
                    body.append(buf, *read_res);
                }
                cJSON* root = cJSON_Parse(body.c_str());
                if (root && cJSON_IsArray(root)) {
                    int count = cJSON_GetArraySize(root);
                    for (int i = 0; i < count; i++) {
                        cJSON* item = cJSON_GetArrayItem(root, i);
                        cJSON* name_item = cJSON_GetObjectItem(item, "name");
                        cJSON* url_item = cJSON_GetObjectItem(item, "url_resolved");
                        if (!url_item || !cJSON_IsString(url_item) || strlen(url_item->valuestring) == 0) {
                            url_item = cJSON_GetObjectItem(item, "url");
                        }
                        cJSON* codec_item = cJSON_GetObjectItem(item, "codec");
                        cJSON* bitrate_item = cJSON_GetObjectItem(item, "bitrate");

                        if (name_item && url_item && cJSON_IsString(name_item) && cJSON_IsString(url_item)) {
                            cJSON* station_obj = cJSON_CreateObject();
                            cJSON_AddStringToObject(station_obj, "name", name_item->valuestring);
                            cJSON_AddStringToObject(station_obj, "url", url_item->valuestring);
                            cJSON_AddStringToObject(station_obj, "codec", codec_item && cJSON_IsString(codec_item) ? codec_item->valuestring : "AAC");
                            cJSON_AddNumberToObject(station_obj, "bitrate", bitrate_item && cJSON_IsNumber(bitrate_item) ? bitrate_item->valueint : 64);
                            cJSON_AddItemToArray(out_array, station_obj);
                            online_success = true;
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        http->Close();
    }

    // Add curated stations if online search was empty or failed
    if (!online_success || cJSON_GetArraySize(out_array) == 0) {
        for (const auto& preset : kPresets) {
            bool matches = (query.empty() || query == "Philippines" || query == "OPM" || query == "PH");
            if (!matches) {
                std::string p_name = preset.name;
                std::string q = query;
                std::transform(p_name.begin(), p_name.end(), p_name.begin(), ::tolower);
                std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                if (p_name.find(q) != std::string::npos) {
                    matches = true;
                }
            }
            if (matches) {
                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", preset.name);
                cJSON_AddStringToObject(obj, "url", preset.url);
                cJSON_AddStringToObject(obj, "codec", preset.codec);
                cJSON_AddNumberToObject(obj, "bitrate", preset.bitrate);
                cJSON_AddItemToArray(out_array, obj);
            }
        }
    }

    char* json_str = cJSON_PrintUnformatted(out_array);
    std::string result(json_str ? json_str : "[]");
    if (json_str) free(json_str);
    cJSON_Delete(out_array);
    return result;
}
