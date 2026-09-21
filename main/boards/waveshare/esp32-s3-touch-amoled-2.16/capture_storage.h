#pragma once

#include <sys/types.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <sdmmc_cmd.h>

struct CaptureResult {
    bool ok = false;
    std::string message;
    std::string path;
};

// Board-local SD storage worker. The audio callback only copies PCM into a
// preallocated stream buffer; all filesystem operations run on worker_task_.
class CaptureStorage {
public:
    using ErrorCallback = std::function<void(const std::string&)>;

    CaptureStorage();
    ~CaptureStorage();

    CaptureStorage(const CaptureStorage&) = delete;
    CaptureStorage& operator=(const CaptureStorage&) = delete;

    CaptureResult Initialize();
    CaptureResult StartAudioRecording();
    CaptureResult StopAudioRecording();
    CaptureResult StartTextNotes();
    CaptureResult StopTextNotes(bool drop_last_transcript);
    void AppendTranscript(const std::string& text);

    // Called from AudioInputTask. It must never wait for the SD card.
    void PushPcm(const int16_t* samples, size_t sample_count, int channels);

    bool IsAudioRecording() const { return audio_active_.load(); }
    bool IsTextNotesActive() const { return notes_active_.load(); }
    bool IsMounted() const { return mounted_.load(); }
    std::string GetStatusJson() const;
    void SetErrorCallback(ErrorCallback callback);

private:
    enum class FreeSpaceStatus {
        Enough,
        Low,
        Error,
    };

    enum class CommandType {
        Mount,
        StartAudio,
        StopAudio,
        StartNotes,
        StopNotes,
        AppendTranscript,
        Shutdown,
    };

    struct Command {
        CommandType type;
        bool drop_last = false;
        std::string* text = nullptr;
        CaptureResult* result = nullptr;
        SemaphoreHandle_t done = nullptr;
    };

    static constexpr const char* kMountPoint = "/sdcard";
    static constexpr size_t kAudioRingBytes = 64 * 1024;
    static constexpr uint64_t kMinimumFreeBytes = 32ULL * 1024 * 1024;
    static constexpr uint64_t kMaxWavDataBytes = 0xFFFFFF00ULL;

    QueueHandle_t command_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    StreamBufferHandle_t audio_stream_ = nullptr;
    StaticStreamBuffer_t audio_stream_state_{};
    uint8_t* audio_stream_storage_ = nullptr;
    std::mutex control_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::mutex callback_mutex_;
    ErrorCallback error_callback_;

    std::atomic_bool mounted_{false};
    std::atomic_bool audio_active_{false};
    std::atomic_bool notes_active_{false};
    std::atomic_bool audio_overflow_{false};
    bool spi_bus_owned_ = false;
    sdmmc_card_t* card_ = nullptr;

    FILE* audio_file_ = nullptr;
    FILE* notes_file_ = nullptr;
    uint64_t audio_data_bytes_ = 0;
    uint64_t next_space_check_bytes_ = 0;
    int64_t last_wav_checkpoint_us_ = 0;
    off_t last_note_entry_offset_ = -1;
    std::string audio_path_;
    std::string notes_path_;
    unsigned fallback_sequence_ = 0;

    static void WorkerEntry(void* argument);
    void WorkerLoop();
    CaptureResult Submit(CommandType type, bool drop_last = false);
    void EnqueueTranscript(std::string* text);
    void CompleteCommand(const Command& command, CaptureResult result);

    CaptureResult MountIfNeeded();
    CaptureResult EnsureStorageReady();
    CaptureResult StartAudioOnWorker();
    CaptureResult StopAudioOnWorker();
    CaptureResult StartNotesOnWorker();
    CaptureResult StopNotesOnWorker(bool drop_last);
    void AppendTranscriptOnWorker(const std::string& text);
    void DrainAudio();
    void FailAudio(const std::string& message);
    void FailStorage(const std::string& message);
    void InvalidateMount();
    void NotifyError(const std::string& message);

    bool EnsureDirectory(const std::string& path);
    FreeSpaceStatus GetFreeSpaceStatus() const;
    std::string MakeUniquePath(const char* category, const char* extension);
    std::string FormatEntryTime() const;
    bool WriteWavHeader();
    bool CheckpointWav(bool force_sync);
};
