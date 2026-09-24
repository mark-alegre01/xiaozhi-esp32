#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "radio_player.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (RadioPlayer::GetInstance().IsPlaying()) {
                RadioPlayer::GetInstance().Stop();
            }
            app.ToggleChatState();
        });

        // Touch button: tap/click to toggle chat mode; long-press to push-to-talk
        touch_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() <= kDeviceStateStarting) {
                return;
            }
            if (RadioPlayer::GetInstance().IsPlaying()) {
                RadioPlayer::GetInstance().Stop();
            }
            app.ToggleChatState();
        });

        touch_button_.OnLongPress([this]() {
            if (RadioPlayer::GetInstance().IsPlaying()) {
                RadioPlayer::GetInstance().Stop();
            }
            Application::GetInstance().StartListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);

        auto& mcp = McpServer::GetInstance();

        // Dedicated YouTube / Online Song Player
        mcp.AddTool(
            "self.music.play_song",
            "Search and play any specific song, music track, or artist (such as Philippine OPM music, pop, rock, Eraserheads, Ben&Ben, etc.) on YouTube or online. "
            "Call this tool whenever the user asks to play a song, play music, or listen to an artist or track.",
            PropertyList({
                Property("query", kPropertyTypeString)
            }),
            [](const PropertyList& props) -> ToolResult {
                auto query = props["query"].value<std::string>();
                if (query.empty()) {
                    return std::unexpected("Song query cannot be empty");
                }
                // PlaySong() is non-blocking: starts a background FreeRTOS task
                // so the WebSocket thread is NOT blocked during TTS playback.
                bool ok = RadioPlayer::GetInstance().PlaySong(query);
                if (!ok) {
                    return std::unexpected("Failed to start song search");
                }
                return std::string("Searching and playing '") + query + "' in the background";
            });

        mcp.AddTool(
            "self.music.stop",
            "Stop the currently playing music, song, or audio stream.",
            PropertyList(),
            [](const PropertyList& props) -> ToolResult {
                RadioPlayer::GetInstance().Stop();
                return true;
            });

        mcp.AddTool(
            "self.radio.play",
            "Play a Philippine internet radio station, OPM music stream, or any audio stream URL through the speaker. "
            "Call this tool when the user asks to play a radio station, listen to Filipino music, or stream audio.",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("name", kPropertyTypeString, std::string("Radio"))
            }),
            [](const PropertyList& props) -> ToolResult {
                auto url = props["url"].value<std::string>();
                auto name = props["name"].value<std::string>();
                if (url.empty() && name.empty()) {
                    return std::unexpected("Stream URL or name cannot be empty");
                }

                // If URL does not look like a direct HTTP/HTTPS link, treat as song query
                // Use non-blocking PlaySong() so the WebSocket thread isn't blocked
                if (!url.empty() && url.find("http://") != 0 && url.find("https://") != 0) {
                    bool ok = RadioPlayer::GetInstance().PlaySong(url);
                    if (!ok) return std::unexpected("Failed to start song search");
                    return std::string("Searching and playing '") + url + "' in the background";
                }
                if (url.empty() && !name.empty() && name != "Radio") {
                    bool ok = RadioPlayer::GetInstance().PlaySong(name);
                    if (!ok) return std::unexpected("Failed to start song search");
                    return std::string("Searching and playing '") + name + "' in the background";
                }

                bool ok = RadioPlayer::GetInstance().Play(url, name);
                if (!ok) {
                    return std::unexpected("Failed to start radio playback");
                }
                return "Now playing " + name;
            });

        mcp.AddTool(
            "self.radio.stop",
            "Stop the currently playing internet radio or audio stream.",
            PropertyList(),
            [](const PropertyList& props) -> ToolResult {
                RadioPlayer::GetInstance().Stop();
                return true;
            });

        mcp.AddTool(
            "self.radio.search_stations",
            "Search online for Philippine radio stations and OPM stream URLs. "
            "Returns a JSON list of available stations with names, URLs, and audio formats. "
            "Call this tool when the user asks what radio stations are available or wants to find a station.",
            PropertyList({
                Property("query", kPropertyTypeString, std::string("Philippines"))
            }),
            [](const PropertyList& props) -> ToolResult {
                auto query = props["query"].value<std::string>();
                return RadioPlayer::SearchStationsOnline(query);
            });

        mcp.AddTool(
            "self.radio.get_status",
            "Get the current status of radio playback (is_playing, current station, current URL).",
            PropertyList(),
            [](const PropertyList& props) -> ToolResult {
                auto& player = RadioPlayer::GetInstance();
                cJSON* root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "is_playing", player.IsPlaying());
                cJSON_AddStringToObject(root, "station", player.GetCurrentStation().c_str());
                cJSON_AddStringToObject(root, "url", player.GetCurrentUrl().c_str());
                char* str = cJSON_PrintUnformatted(root);
                std::string res(str ? str : "{}");
                if (str) free(str);
                cJSON_Delete(root);
                return res;
            });

        // ── News Tools: Spoken AI Summaries & Live News Radio ───────────────
        mcp.AddTool(
            "self.news.get_headlines",
            "Get the latest top news headlines (e.g. Philippines news, world news, technology, business, or sports). "
            "Use this tool whenever the user asks 'What is the news today?', 'Give me the latest headlines', or asks about current events. "
            "Returns a list of current news stories so you can summarize and read them aloud to the user.",
            PropertyList({
                Property("category", kPropertyTypeString, std::string("philippines"))
            }),
            [](const PropertyList& props) -> ToolResult {
                std::string category = "philippines";
                try {
                    auto prop = props["category"];
                    category = prop.value<std::string>();
                } catch (...) {
                    category = "philippines";
                }
                if (category.empty()) category = "philippines";
                return RadioPlayer::FetchNewsHeadlines(category);
            });

        mcp.AddTool(
            "self.news.play_broadcast",
            "Play a live 24/7 news radio broadcast or audio stream through the speaker. "
            "Choose 'DZRH' (or 'philippines') for live Philippine news in Tagalog, or 'BBC' (or 'world') for BBC World Service in English. "
            "Call this tool when the user asks to play news radio, listen to live news, or tune into a news station.",
            PropertyList({
                Property("station", kPropertyTypeString, std::string("DZRH"))
            }),
            [](const PropertyList& props) -> ToolResult {
                std::string station = "DZRH";
                try {
                    auto prop = props["station"];
                    station = prop.value<std::string>();
                } catch (...) {
                    station = "DZRH";
                }
                if (station.empty()) station = "DZRH";
                std::string lower_station = station;
                std::transform(lower_station.begin(), lower_station.end(), lower_station.begin(), ::tolower);

                std::string url;
                std::string name;
                if (lower_station.find("bbc") != std::string::npos || lower_station.find("world") != std::string::npos || lower_station.find("english") != std::string::npos) {
                    url = "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service";
                    name = "BBC World Service";
                } else {
                    url = "https://azura.dzrh.com.ph/listen/dzrh_manila/radio.mp3";
                    name = "DZRH News Manila";
                }

                bool ok = RadioPlayer::GetInstance().Play(url, name);
                if (!ok) {
                    return std::unexpected("Failed to start news broadcast");
                }
                return "Now playing live news broadcast: " + name;
            });
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO, TOUCH_BUTTON_ACTIVE_HIGH),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
