#ifndef RADIO_PLAYER_H_
#define RADIO_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <string>
#include <atomic>
#include <vector>
#include <esp_timer.h>

class AudioCodec;
class Http;

// A single timed line of lyrics
struct LyricLine {
    int ms;            // milliseconds from start of song
    std::string text;
};

class RadioPlayer {
public:
    static RadioPlayer& GetInstance();

    bool Play(const std::string& url, const std::string& station_name = "Radio");
    bool PlaySong(const std::string& query);
    void Stop();
    bool IsPlaying() const { return is_playing_.load(); }
    std::string GetCurrentStation() const;
    std::string GetCurrentUrl() const;

    // Lyrics API
    void ShowLyrics();  // fetch & enable scrolling lyrics on OLED
    void HideLyrics();  // hide lyrics, restore robot face
    bool IsLyricsEnabled() const { return lyrics_enabled_.load(); }
    int GetCurrentLyricIndex() const;  // index of active lyric line based on elapsed time
    const std::vector<LyricLine>& GetLyrics() const { return lyrics_lines_; }
    uint32_t GetElapsedMs() const { return GetPlaybackElapsedMs(); }

    static std::string SearchStationsOnline(const std::string& query);
    static std::string FetchNewsHeadlines(const std::string& category = "philippines");
    static std::string GetBridgeHost();
    static void DiscoverBridge();
    std::string PlaySongOrFallback(const std::string& query);

private:
    RadioPlayer();
    ~RadioPlayer();

    RadioPlayer(const RadioPlayer&) = delete;
    RadioPlayer& operator=(const RadioPlayer&) = delete;

    static void TaskTrampoline(void* arg);
    void WorkerTask();

    std::mutex mutex_;
    std::string url_;
    std::string current_playing_url_;
    std::string station_name_;
    std::string pending_search_query_;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
    AudioCodec* codec_ = nullptr;
    Http* http_active_ = nullptr;  // set while streaming; Stop() calls Close() to unblock Read()

    // Lyrics state
    std::atomic<bool> lyrics_enabled_{false};
    std::atomic<int64_t> playback_start_us_{0}; // esp_timer_get_time() when first audio was output
    std::atomic<bool>    playback_started_{false}; // true once first PCM frame was written
    int32_t lyric_offset_ms_{-800};  // negative = show lyric earlier (compensates for buffer fill delay)
    std::vector<LyricLine> lyrics_lines_;   // parsed from LRCLIB (guarded by mutex_)
    std::string lyrics_video_id_;           // video id used to fetch current lyrics

    // Returns wall-clock elapsed playback time in ms (used for lyric sync)
    uint32_t GetPlaybackElapsedMs() const {
        if (!playback_started_.load()) return 0;
        int64_t start = playback_start_us_.load();
        if (start == 0) return 0;
        int64_t elapsed_us = esp_timer_get_time() - start;
        if (elapsed_us < 0) return 0;
        int64_t adjusted = elapsed_us / 1000 + lyric_offset_ms_;
        return (adjusted > 0) ? (uint32_t)adjusted : 0;
    }

    void FetchAndStoreLyrics(const std::string& video_id, const std::string& title, const std::string& query);
    void UpdateLyricsDisplay();
};

#endif // RADIO_PLAYER_H_
