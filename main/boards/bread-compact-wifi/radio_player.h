#ifndef RADIO_PLAYER_H_
#define RADIO_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <string>
#include <atomic>

class AudioCodec;

class RadioPlayer {
public:
    static RadioPlayer& GetInstance();

    bool Play(const std::string& url, const std::string& station_name = "Radio");
    bool PlaySong(const std::string& query);
    void Stop();
    bool IsPlaying() const { return is_playing_.load(); }
    std::string GetCurrentStation() const;
    std::string GetCurrentUrl() const;

    static std::string SearchStationsOnline(const std::string& query);
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
    std::string station_name_;
    std::string pending_search_query_;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
    AudioCodec* codec_ = nullptr;
};

#endif // RADIO_PLAYER_H_
