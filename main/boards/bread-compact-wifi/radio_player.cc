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
#include "config.h"
#include <http.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

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

static std::string g_bridge_host = MUSIC_BRIDGE_HOST;

std::string RadioPlayer::GetBridgeHost() {
    if (g_bridge_host.empty()) {
        g_bridge_host = MUSIC_BRIDGE_HOST;
    }
    return g_bridge_host;
}

void RadioPlayer::DiscoverBridge() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP discovery socket");
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 200000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(8088);
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const char* msg = "XIAOZHI_DISCOVER";
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));

    char rx_buffer[128];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr*)&source_addr, &socklen);

    if (len > 0) {
        rx_buffer[len] = 0;
        char ip_str[32] = {0};
        int port = 8080;
        if (sscanf(rx_buffer, "XIAOZHI_BRIDGE %31s %d", ip_str, &port) >= 1) {
            g_bridge_host = ip_str;
            ESP_LOGI(TAG, "Discovered music bridge at %s:%d via UDP", g_bridge_host.c_str(), port);
        }
    }
    close(sock);
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
    } else if (lower_input.find("bbc") != std::string::npos || lower_name.find("bbc") != std::string::npos) {
        resolved_url = "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service";
        resolved_name = "BBC World Service";
    } else if (lower_input.find("dzrh") != std::string::npos || lower_name.find("dzrh") != std::string::npos || lower_input.find("news") != std::string::npos || lower_name.find("news") != std::string::npos) {
        resolved_url = "https://azura.dzrh.com.ph/listen/dzrh_manila/radio.mp3";
        resolved_name = "DZRH News Manila";
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

    // Store new stream and signal any running worker to stop.
    // This runs on the WebSocket event thread, so we must NOT block.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_search_query_.clear();
        url_ = resolved_url;
        station_name_ = resolved_name;
        stop_requested_.store(true);
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification(station_name_, 3000);
        display->SetStatus("Playing");
    }

    if (!is_playing_.load()) {
        stop_requested_.store(false);

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
    }

    ESP_LOGI(TAG, "Queued background radio stream: %s (%s)", station_name_.c_str(), url_.c_str());
    return true;
}

bool RadioPlayer::PlaySong(const std::string& query) {
    if (query.empty()) {
        return false;
    }

    // Store new query and signal any running worker to stop.
    // This function runs on the WebSocket event thread, so we must NOT block.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_search_query_ = query;
        url_ = "";
        station_name_ = query;
        stop_requested_.store(true);  // tells any running stream loop to exit
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification(query, 3000);
        display->SetStatus("Searching");
    }

    // If a task is already running, it will exit via stop_requested_ and then
    // notice that pending_search_query_ is set — it will restart on its own.
    // We only need to create a new task if nothing is currently running.
    if (!is_playing_.load()) {
        stop_requested_.store(false);

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
            if (display) display->ShowNotification("Play Failed", 3000);
            return false;
        }

        is_playing_.store(true);
    }
    // else: running task sees stop_requested_=true, exits, then re-enters with new query

    ESP_LOGI(TAG, "Queued background song play: %s", query.c_str());
    return true;
}

void RadioPlayer::Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!is_playing_.load() && task_handle_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stopping radio stream...");

    // Clear any pending song/stream so the worker won't restart after exiting
    pending_search_query_.clear();
    url_.clear();

    // Signal the streaming loop to stop
    stop_requested_.store(true);
    is_playing_.store(false);

    // Interrupt the blocking http->Read() so the worker exits immediately.
    // We release the lock first so the worker task can proceed to clear http_active_
    // after Close() returns (avoiding a potential deadlock).
    Http* http_to_close = http_active_;
    lock.unlock();
    if (http_to_close != nullptr) {
        http_to_close->Close();  // unblocks the blocking Read() in WorkerTask
    }

    // Immediately kill audio output so there's no more sound
    if (codec_ != nullptr) {
        codec_->UnlockOutput();
        if (codec_->output_enabled()) {
            codec_->EnableOutput(false);
        }
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("Stopped", 2000);
        display->SetStatus("Ready");
    }
    ESP_LOGI(TAG, "Radio stop signalled; worker will clean up asynchronously");
}

void RadioPlayer::WorkerTask() {
    std::string search_query;
    std::string stream_url;
    std::string display_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        search_query = pending_search_query_;
        pending_search_query_.clear();
        stream_url = url_;
        display_name = station_name_;
    }

    auto display = Board::GetInstance().GetDisplay();

    // ── Phase 1: Background Song Search (if query was provided) ─────────────
    if (!search_query.empty()) {
        ESP_LOGI(TAG, "WorkerTask searching YouTube for: '%s'", search_query.c_str());
        if (display) {
            display->ShowNotification(search_query, 3000);
            display->SetStatus("Searching");
        }

        std::string encoded_query;
        for (char c : search_query) {
            if (c == ' ') {
                encoded_query += "%20";
            } else if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
                encoded_query += c;
            } else {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
                encoded_query += hex;
            }
        }

        std::string bridge_host = GetBridgeHost();
        int bridge_port = MUSIC_BRIDGE_PORT;
        std::string search_url = "http://" + bridge_host + ":" + std::to_string(bridge_port) + "/search?q=" + encoded_query;

        bool found = false;
        auto http_search = Board::GetInstance().GetNetwork()->CreateHttp(3);
        if (http_search) {
            http_search->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaozhiRobot/1.0");
            http_search->SetHeader("Accept", "application/json");
            http_search->SetTimeout(4000);

            bool opened = http_search->Open("GET", search_url).has_value();
            if (!opened) {
                DiscoverBridge();
                std::string new_host = GetBridgeHost();
                if (new_host != bridge_host) {
                    bridge_host = new_host;
                    search_url = "http://" + bridge_host + ":" + std::to_string(bridge_port) + "/search?q=" + encoded_query;
                    opened = http_search->Open("GET", search_url).has_value();
                }
            }

            if (opened) {
                auto status = http_search->GetStatusCode();
                if (status && *status >= 200 && *status < 300) {
                    std::string body;
                    char buf[512];
                    while (body.size() < 8192 && !stop_requested_.load()) {
                        auto read_res = http_search->Read(buf, sizeof(buf) - 1);
                        if (!read_res || *read_res == 0) break;
                        buf[*read_res] = '\0';
                        body.append(buf, *read_res);
                    }
                    cJSON* root = cJSON_Parse(body.c_str());
                    if (root) {
                        cJSON* status_item = cJSON_GetObjectItem(root, "status");
                        cJSON* title_item = cJSON_GetObjectItem(root, "title");
                        cJSON* url_item = cJSON_GetObjectItem(root, "stream_url");
                        if (status_item && cJSON_IsString(status_item) && strcmp(status_item->valuestring, "ok") == 0 &&
                            url_item && cJSON_IsString(url_item) && strlen(url_item->valuestring) > 0) {
                            stream_url = url_item->valuestring;
                            if (title_item && cJSON_IsString(title_item)) {
                                display_name = title_item->valuestring;
                            }
                            found = true;
                        }
                        cJSON_Delete(root);
                    }
                }
            }
            http_search->Close();
        }

        if (stop_requested_.load()) {
            goto cleanup;
        }

        if (!found || stream_url.empty()) {
            ESP_LOGW(TAG, "Bridge offline or song not found. Falling back to Love Radio.");
            stream_url = "https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3";
            display_name = "Love Radio (OPM Live)";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            url_ = stream_url;
            station_name_ = display_name;
        }
    }

    if (display) {
        display->ShowNotification(display_name, 4000);
        display->SetStatus("Connecting");
    }

    // ── Phase 2: Connect to HTTP audio stream ───────────────────────────────
    ESP_LOGI(TAG, "Connecting to stream: %s", stream_url.c_str());
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            if (display) display->ShowNotification("Network Error", 3000);
            goto cleanup;
        }

        http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaozhiRadio/1.0");
        http->SetHeader("Accept", "*/*");
        http->SetHeader("Icy-MetaData", "0");
        http->SetTimeout(12000);

        if (auto opened = http->Open("GET", stream_url); !opened) {
            ESP_LOGE(TAG, "HTTP open failed for %s: %s", stream_url.c_str(), opened.error().ToString().c_str());
            if (display) display->ShowNotification("Connect Failed", 3000);
            http->Close();
            goto cleanup;
        }

        auto status_code = http->GetStatusCode();
        if (!status_code || *status_code < 200 || *status_code >= 400) {
            ESP_LOGE(TAG, "HTTP stream error, status: %d", status_code ? *status_code : -1);
            if (display) display->ShowNotification("Stream Offline", 3000);
            http->Close();
            goto cleanup;
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
            goto cleanup;
        }

        // ── Phase 3: Wait for robot TTS announcement to finish ──────────────
        // In manual-stop mode: Speaking -> Idle
        // In auto-listen mode: Speaking -> Listening (both mean TTS is done)
        int tts_wait = 0;
        while (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking &&
               !stop_requested_.load() && tts_wait < 6000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            tts_wait += 50;
        }
        if (stop_requested_.load()) {
            esp_audio_simple_dec_close(dec_handle);
            http->Close();
            goto cleanup;
        }
        // Extra settle time so audio decode queue drains TTS completely
        vTaskDelay(pdMS_TO_TICKS(300));

        // Switch to Idle so microphone does NOT pick up the music as voice input
        Application::GetInstance().SetDeviceState(kDeviceStateIdle);

        if (display) {
            display->ShowNotification(display_name, 3000);
            display->SetStatus("Playing");
        }

        // Lock output on speaker so idle timer doesn't disable I2S TX
        if (codec_ != nullptr) {
            codec_->LockOutput();
            codec_->EnableOutput(true);
        }

        // Audio buffers
        constexpr size_t kHttpReadBufSize = 2048;
        std::vector<uint8_t> http_buf(kHttpReadBufSize);
        constexpr size_t kPcmBufSize = 8192;
        std::vector<uint8_t> pcm_buf(kPcmBufSize);

        esp_ae_rate_cvt_handle_t resampler = nullptr;
        int current_sample_rate = 0;
        int target_sample_rate = codec_ ? codec_->output_sample_rate() : 24000;

        std::vector<uint8_t> acc_buf;
        acc_buf.reserve(kHttpReadBufSize * 4);

        int frames_decoded = 0;
        int consec_errors = 0;
        bool mp3_fallback_tried = false;
        constexpr int kMaxConsecErrors = 64;
        constexpr size_t kMaxAccBuf = 128 * 1024;

        // ── Phase 4: Streaming & Decode Loop ────────────────────────────────
        // Register active http handle so Stop() can close it to interrupt Read()
        {
            std::lock_guard<std::mutex> lk(mutex_);
            http_active_ = http.get();
        }
        while (!stop_requested_.load()) {
            auto read_res = http->Read(reinterpret_cast<char*>(http_buf.data()), http_buf.size());
            if (!read_res) {
                ESP_LOGW(TAG, "Stream read failed: %s", read_res.error().ToString().c_str());
                break;
            }
            if (*read_res == 0) {
                ESP_LOGI(TAG, "Stream reached EOF");
                break;
            }

            // Auto-detect codec from the FIRST real chunk when URL was ambiguous
            if (frames_decoded == 0 && acc_buf.empty() && *read_res >= 3) {
                uint8_t b0 = http_buf[0], b1 = http_buf[1], b2 = http_buf[2];
                bool looks_mp3 = (b0 == 'I' && b1 == 'D' && b2 == '3') ||
                                 (b0 == 0xFF && (b1 & 0xE0) == 0xE0);
                bool looks_aac = (b0 == 0xFF && (b1 & 0xF6) == 0xF0);

                if (!is_mp3 && looks_mp3) {
                    ESP_LOGI(TAG, "Detected MP3 from magic bytes, switching decoder");
                    esp_audio_simple_dec_close(dec_handle);
                    dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
                    dec_cfg.dec_cfg  = nullptr;
                    dec_cfg.cfg_size = 0;
                    esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
                    is_mp3 = true;
                } else if (is_mp3 && looks_aac) {
                    ESP_LOGI(TAG, "Detected AAC from magic bytes, switching decoder");
                    esp_audio_simple_dec_close(dec_handle);
                    dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
#if __has_include("esp_aac_dec.h")
                    esp_aac_dec_cfg_t aac_fb = {};
                    aac_fb.aac_plus_enable = true;
                    dec_cfg.dec_cfg  = &aac_fb;
                    dec_cfg.cfg_size = sizeof(esp_aac_dec_cfg_t);
#else
                    dec_cfg.dec_cfg  = nullptr;
                    dec_cfg.cfg_size = 0;
#endif
                    esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
                    is_mp3 = false;
                }
            }

            acc_buf.insert(acc_buf.end(), http_buf.begin(), http_buf.begin() + *read_res);

            if (acc_buf.size() > kMaxAccBuf) {
                ESP_LOGW(TAG, "Acc buffer (%u B) too large, trimming", (unsigned)acc_buf.size());
                acc_buf.erase(acc_buf.begin(), acc_buf.begin() + (acc_buf.size() - kMaxAccBuf / 2));
            }

            size_t pos = 0;
            while (pos < acc_buf.size() && !stop_requested_.load()) {
                esp_audio_simple_dec_raw_t raw = {};
                raw.buffer       = acc_buf.data() + pos;
                raw.len          = static_cast<uint32_t>(acc_buf.size() - pos);
                raw.eos          = false;
                raw.consumed     = 0;
                raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

                esp_audio_simple_dec_out_t out_frame = {};
                out_frame.buffer       = pcm_buf.data();
                out_frame.len          = static_cast<uint32_t>(pcm_buf.size());
                out_frame.decoded_size = 0;

                esp_audio_err_t ret = esp_audio_simple_dec_process(dec_handle, &raw, &out_frame);

                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    pcm_buf.resize(pcm_buf.size() * 2);
                    continue;
                }

                if (raw.consumed > 0) {
                    pos += raw.consumed;
                    consec_errors = 0;
                } else if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_CONTINUE) {
                    pos++;
                    consec_errors++;
                    if (consec_errors >= kMaxConsecErrors) {
                        if (is_mp3 && !mp3_fallback_tried) {
                            ESP_LOGW(TAG, "MP3 decoder failing (%d errors), trying AAC fallback", consec_errors);
                            esp_audio_simple_dec_close(dec_handle);
                            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
#if __has_include("esp_aac_dec.h")
                            esp_aac_dec_cfg_t aac_fb = {};
                            aac_fb.aac_plus_enable = true;
                            dec_cfg.dec_cfg  = &aac_fb;
                            dec_cfg.cfg_size = sizeof(esp_aac_dec_cfg_t);
#else
                            dec_cfg.dec_cfg  = nullptr;
                            dec_cfg.cfg_size = 0;
#endif
                            esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
                            is_mp3 = false;
                            mp3_fallback_tried = true;
                            consec_errors = 0;
                            pos = 0;
                            acc_buf.clear();
                            break;
                        }
                        ESP_LOGE(TAG, "Too many consecutive decode errors, aborting stream");
                        stop_requested_.store(true);
                        break;
                    }
                } else {
                    break;
                }

                if (out_frame.decoded_size > 0 && codec_ != nullptr) {
                    frames_decoded++;

                    esp_audio_simple_dec_info_t info = {};
                    esp_audio_simple_dec_get_info(dec_handle, &info);

                    if (frames_decoded == 1 || frames_decoded % 100 == 0) {
                        ESP_LOGI(TAG, "Frame #%d: ch=%d sr=%d Hz, decoded=%u B",
                                 frames_decoded, info.channel, info.sample_rate,
                                 (unsigned)out_frame.decoded_size);
                    }

                    std::vector<int16_t> mono_pcm;
                    if (info.channel >= 2) {
                        size_t n = out_frame.decoded_size / (sizeof(int16_t) * 2);
                        int16_t* src = reinterpret_cast<int16_t*>(out_frame.buffer);
                        mono_pcm.resize(n);
                        for (size_t i = 0; i < n; i++) {
                            mono_pcm[i] = static_cast<int16_t>(
                                (static_cast<int32_t>(src[i * 2]) + src[i * 2 + 1]) / 2);
                        }
                    } else {
                        size_t n = out_frame.decoded_size / sizeof(int16_t);
                        int16_t* src = reinterpret_cast<int16_t*>(out_frame.buffer);
                        mono_pcm.assign(src, src + n);
                    }

                    if (info.sample_rate > 0 && info.sample_rate != current_sample_rate) {
                        if (resampler != nullptr) {
                            esp_ae_rate_cvt_close(resampler);
                            resampler = nullptr;
                        }
                        current_sample_rate = info.sample_rate;
                        if (current_sample_rate != target_sample_rate) {
                            esp_ae_rate_cvt_cfg_t cvt_cfg =
                                RATE_CVT_CFG(current_sample_rate, target_sample_rate, ESP_AUDIO_MONO);
                            auto rc = esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                            ESP_LOGI(TAG, "Resampler %d->%d Hz rc=%d", current_sample_rate, target_sample_rate, rc);
                        }
                    }

                    if (resampler != nullptr) {
                        uint32_t max_out = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono_pcm.size(), &max_out);
                        if (max_out > 0) {
                            std::vector<int16_t> resampled(max_out);
                            uint32_t actual_out = max_out;
                            esp_ae_rate_cvt_process(resampler,
                                reinterpret_cast<esp_ae_sample_t>(mono_pcm.data()), mono_pcm.size(),
                                reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual_out);
                            resampled.resize(actual_out);
                            if (!resampled.empty()) codec_->OutputData(resampled);
                        }
                    } else {
                        if (!mono_pcm.empty()) codec_->OutputData(mono_pcm);
                    }
                }
            } // inner decode loop

            if (pos > 0 && pos <= acc_buf.size()) {
                acc_buf.erase(acc_buf.begin(), acc_buf.begin() + pos);
            } else if (pos > acc_buf.size()) {
                acc_buf.clear();
            }
        } // outer HTTP-read loop

        ESP_LOGI(TAG, "Stream loop exited. Total frames decoded: %d", frames_decoded);

        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
        }

        // Unregister before closing so Stop() won't call Close() twice
        {
            std::lock_guard<std::mutex> lk(mutex_);
            http_active_ = nullptr;
        }
        esp_audio_simple_dec_close(dec_handle);
        http->Close();
    }

cleanup:
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    // Only clean up codec if Stop() hasn't already done so
    // (Stop() sets is_playing_=false and calls UnlockOutput before we reach here)
    if (codec_ != nullptr && is_playing_.load()) {
        codec_->UnlockOutput();
        if (codec_->output_enabled()) {
            codec_->EnableOutput(false);
        }
    }
    if (display) {
        display->SetStatus("Ready");
    }

    // Check if PlaySong() or Play() queued a new song/stream while we were streaming.
    // If so, restart the worker immediately instead of terminating.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_requested_.load() && (!pending_search_query_.empty() || !url_.empty())) {
            // A new song or stream was queued — restart
            stop_requested_.store(false);
            is_playing_.store(true);
            ESP_LOGI(TAG, "Restarting worker for new stream/song");
        } else {
            is_playing_.store(false);
            stop_requested_.store(false);
            task_handle_ = nullptr;
        }
    }

    // If there's a new pending song/stream, re-enter the task body
    if (is_playing_.load() && !stop_requested_.load()) {
        ESP_LOGI(TAG, "Re-entering WorkerTask for queued stream");
        WorkerTask();  // tail call
        return;  // vTaskDelete already called by recursive WorkerTask()
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

std::string RadioPlayer::PlaySongOrFallback(const std::string& query) {
    if (query.empty()) {
        Play("https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3", "Love Radio 90.7");
        return "Playing Philippine OPM Live Radio";
    }

    ESP_LOGI(TAG, "Searching YouTube song for: '%s'", query.c_str());

    std::string encoded_query;
    for (char c : query) {
        if (c == ' ') {
            encoded_query += "%20";
        } else if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            encoded_query += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            encoded_query += hex;
        }
    }

    std::string bridge_host = MUSIC_BRIDGE_HOST;
    int bridge_port = MUSIC_BRIDGE_PORT;
    std::string search_url = "http://" + bridge_host + ":" + std::to_string(bridge_port) + "/search?q=" + encoded_query;

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("Searching Song...", 2500);
    }

    bool bridge_success = false;
    std::string stream_url;
    std::string song_title = query;

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (http) {
        http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaozhiRobot/1.0");
        http->SetHeader("Accept", "application/json");
        http->SetTimeout(6000);

        if (auto opened = http->Open("GET", search_url); opened) {
            auto status = http->GetStatusCode();
            if (status && *status >= 200 && *status < 300) {
                std::string body;
                char buf[512];
                while (body.size() < 8192) {
                    auto read_res = http->Read(buf, sizeof(buf) - 1);
                    if (!read_res || *read_res == 0) break;
                    buf[*read_res] = '\0';
                    body.append(buf, *read_res);
                }
                cJSON* root = cJSON_Parse(body.c_str());
                if (root) {
                    cJSON* status_item = cJSON_GetObjectItem(root, "status");
                    cJSON* title_item = cJSON_GetObjectItem(root, "title");
                    cJSON* url_item = cJSON_GetObjectItem(root, "stream_url");
                    if (status_item && cJSON_IsString(status_item) && strcmp(status_item->valuestring, "ok") == 0 &&
                        url_item && cJSON_IsString(url_item) && strlen(url_item->valuestring) > 0) {
                        stream_url = url_item->valuestring;
                        if (title_item && cJSON_IsString(title_item)) {
                            song_title = title_item->valuestring;
                        }
                        bridge_success = true;
                    }
                    cJSON_Delete(root);
                }
            }
        }
        http->Close();
    }

    if (bridge_success && !stream_url.empty()) {
        ESP_LOGI(TAG, "Playing song from bridge: %s (%s)", song_title.c_str(), stream_url.c_str());
        Play(stream_url, song_title);
        return "Now playing " + song_title + " from YouTube";
    }

    ESP_LOGW(TAG, "Music bridge unreachable or song not found. Falling back to live OPM radio.");
    Play("https://azura.loveradio.com.ph/listen/love_radio_manila/radio.mp3", "Love Radio 90.7 (OPM)");
    return "Music bridge offline or not found. Playing live Philippine OPM radio: Love Radio 90.7";
}

std::string RadioPlayer::FetchNewsHeadlines(const std::string& category) {
    std::string safe_cat = category.empty() ? "philippines" : category;
    std::transform(safe_cat.begin(), safe_cat.end(), safe_cat.begin(), ::tolower);

    std::string encoded_cat;
    for (char c : safe_cat) {
        if (isalnum(static_cast<unsigned char>(c))) {
            encoded_cat += c;
        } else if (c == ' ') {
            encoded_cat += "%20";
        }
    }

    std::string bridge_host = GetBridgeHost();
    int bridge_port = MUSIC_BRIDGE_PORT;
    std::string url = "http://" + bridge_host + ":" + std::to_string(bridge_port) + "/news?category=" + encoded_cat + "&limit=5";

    ESP_LOGI(TAG, "Fetching news headlines from: %s", url.c_str());

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("Fetching News...", 2000);
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        return "Unable to initialize network request for news headlines.";
    }

    http->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) XiaozhiRobot/1.0");
    http->SetHeader("Accept", "application/json");
    http->SetTimeout(4000);

    bool opened = http->Open("GET", url).has_value();
    if (!opened) {
        ESP_LOGW(TAG, "Failed to connect to %s, attempting UDP discovery...", bridge_host.c_str());
        DiscoverBridge();
        std::string new_host = GetBridgeHost();
        if (new_host != bridge_host) {
            bridge_host = new_host;
            url = "http://" + bridge_host + ":" + std::to_string(bridge_port) + "/news?category=" + encoded_cat + "&limit=5";
            opened = http->Open("GET", url).has_value();
        }
    }

    if (!opened) {
        http->Close();
        return "Failed to connect to news service. Please ensure the local bridge is running.";
    }

    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        http->Close();
        return "News service responded with an error (HTTP status code).";
    }

    std::string body;
    char buf[512];
    while (body.size() < 12288) {
        auto read_res = http->Read(buf, sizeof(buf) - 1);
        if (!read_res || *read_res == 0) break;
        buf[*read_res] = '\0';
        body.append(buf, *read_res);
    }
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return "Failed to parse news headlines response.";
    }

    cJSON* headlines_arr = cJSON_GetObjectItem(root, "headlines");
    if (!headlines_arr || !cJSON_IsArray(headlines_arr) || cJSON_GetArraySize(headlines_arr) == 0) {
        cJSON_Delete(root);
        return "No recent news headlines found for category: " + safe_cat;
    }

    std::string summary = "Latest " + safe_cat + " news headlines:\n";
    int count = cJSON_GetArraySize(headlines_arr);
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(headlines_arr, i);
        if (!item) continue;
        cJSON* title_item = cJSON_GetObjectItem(item, "title");
        cJSON* source_item = cJSON_GetObjectItem(item, "source");
        const char* title = (title_item && cJSON_IsString(title_item)) ? title_item->valuestring : "Headline";
        const char* src = (source_item && cJSON_IsString(source_item)) ? source_item->valuestring : "";

        summary += std::to_string(i + 1) + ". " + title;
        if (src && strlen(src) > 0) {
            summary += " (Source: " + std::string(src) + ")";
        }
        summary += "\n";
    }

    cJSON_Delete(root);
    return summary;
}


