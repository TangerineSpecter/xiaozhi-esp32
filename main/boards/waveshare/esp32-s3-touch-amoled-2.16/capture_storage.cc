#include "capture_storage.h"

#include "config.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#define TAG "CaptureStorage"

namespace {

void PutLe16(uint8_t* destination, uint16_t value) {
    destination[0] = value & 0xFF;
    destination[1] = (value >> 8) & 0xFF;
}

void PutLe32(uint8_t* destination, uint32_t value) {
    destination[0] = value & 0xFF;
    destination[1] = (value >> 8) & 0xFF;
    destination[2] = (value >> 16) & 0xFF;
    destination[3] = (value >> 24) & 0xFF;
}

bool IsClockValid(time_t now) {
    // 2024-01-01. Earlier values mean SNTP/server time has not arrived yet.
    return now >= 1704067200;
}

}  // namespace

CaptureStorage::CaptureStorage() {
    command_queue_ = xQueueCreate(16, sizeof(Command));
    audio_stream_storage_ = static_cast<uint8_t*>(
        heap_caps_malloc(kAudioRingBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (audio_stream_storage_ != nullptr) {
        audio_stream_ = xStreamBufferCreateStatic(kAudioRingBytes, 1, audio_stream_storage_,
                                                  &audio_stream_state_);
    }
    if (command_queue_ == nullptr || audio_stream_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate capture queues");
        return;
    }
    BaseType_t created = xTaskCreate(WorkerEntry, "capture_writer", 6144, this, 2, &worker_task_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture writer task");
        worker_task_ = nullptr;
    }
}

CaptureStorage::~CaptureStorage() {
    if (worker_task_ != nullptr) {
        (void)Submit(CommandType::Shutdown);
    }
    if (command_queue_ != nullptr) {
        vQueueDelete(command_queue_);
    }
    if (audio_stream_storage_ != nullptr) {
        heap_caps_free(audio_stream_storage_);
    }
}

CaptureResult CaptureStorage::Initialize() { return Submit(CommandType::Mount); }

CaptureResult CaptureStorage::StartAudioRecording() { return Submit(CommandType::StartAudio); }

CaptureResult CaptureStorage::StopAudioRecording() { return Submit(CommandType::StopAudio); }

CaptureResult CaptureStorage::StartTextNotes() { return Submit(CommandType::StartNotes); }

CaptureResult CaptureStorage::StopTextNotes(bool drop_last_transcript) {
    return Submit(CommandType::StopNotes, drop_last_transcript);
}

void CaptureStorage::AppendTranscript(const std::string& text) {
    if (!notes_active_.load() || text.empty()) {
        return;
    }
    EnqueueTranscript(new std::string(text));
}

void CaptureStorage::PushPcm(const int16_t* samples, size_t sample_count, int channels) {
    if (!audio_active_.load() || audio_stream_ == nullptr || samples == nullptr || channels <= 0) {
        return;
    }

    std::array<int16_t, 160> mono{};
    size_t frame_count = sample_count / static_cast<size_t>(channels);
    size_t frame_offset = 0;
    while (frame_offset < frame_count) {
        size_t count = std::min(mono.size(), frame_count - frame_offset);
        for (size_t i = 0; i < count; ++i) {
            mono[i] = samples[(frame_offset + i) * static_cast<size_t>(channels)];
        }
        size_t bytes = count * sizeof(int16_t);
        if (xStreamBufferSpacesAvailable(audio_stream_) < bytes ||
            xStreamBufferSend(audio_stream_, mono.data(), bytes, 0) != bytes) {
            audio_overflow_.store(true);
            if (worker_task_ != nullptr) {
                xTaskNotifyGive(worker_task_);
            }
            return;
        }
        frame_offset += count;
    }
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
}

std::string CaptureStorage::GetStatusJson() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return std::string("{\"sd_mounted\":") + (mounted_.load() ? "true" : "false") +
           ",\"audio_recording\":" + (audio_active_.load() ? "true" : "false") +
           ",\"text_notes\":" + (notes_active_.load() ? "true" : "false") + ",\"audio_path\":\"" +
           audio_path_ + "\",\"notes_path\":\"" + notes_path_ + "\"}";
}

void CaptureStorage::SetErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(callback);
}

void CaptureStorage::WorkerEntry(void* argument) {
    static_cast<CaptureStorage*>(argument)->WorkerLoop();
    vTaskDelete(nullptr);
}

void CaptureStorage::WorkerLoop() {
    bool running = true;
    while (running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (audio_active_.load()) {
            DrainAudio();
            if (audio_overflow_.exchange(false)) {
                FailAudio("录音缓冲区已满，录音已停止");
            } else if (audio_active_.load() &&
                       esp_timer_get_time() - last_wav_checkpoint_us_ >= 10 * 1000 * 1000) {
                if (!CheckpointWav(true)) {
                    FailStorage("TF 卡写入失败，录音和记录已停止");
                }
            }
        }

        Command command{};
        while (xQueueReceive(command_queue_, &command, 0) == pdTRUE) {
            CaptureResult result{true, "ok", {}};
            switch (command.type) {
                case CommandType::Mount:
                    result = MountIfNeeded();
                    break;
                case CommandType::StartAudio:
                    result = StartAudioOnWorker();
                    break;
                case CommandType::StopAudio:
                    DrainAudio();
                    result = StopAudioOnWorker();
                    break;
                case CommandType::StartNotes:
                    result = StartNotesOnWorker();
                    break;
                case CommandType::StopNotes:
                    result = StopNotesOnWorker(command.drop_last);
                    break;
                case CommandType::AppendTranscript:
                    if (command.text != nullptr) {
                        AppendTranscriptOnWorker(*command.text);
                        delete command.text;
                    }
                    break;
                case CommandType::Shutdown:
                    DrainAudio();
                    StopAudioOnWorker();
                    StopNotesOnWorker(false);
                    if (mounted_.load()) {
                        esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
                        mounted_.store(false);
                        card_ = nullptr;
                    }
                    if (spi_bus_owned_) {
                        spi_bus_free(SD_SPI_HOST);
                        spi_bus_owned_ = false;
                    }
                    running = false;
                    break;
            }
            CompleteCommand(command, std::move(result));
        }
    }
}

CaptureResult CaptureStorage::Submit(CommandType type, bool drop_last) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (worker_task_ == nullptr || command_queue_ == nullptr) {
        return {false, "存储任务不可用", {}};
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        return {false, "内存不足", {}};
    }
    CaptureResult result;
    Command command{.type = type, .drop_last = drop_last, .result = &result, .done = done};
    if (xQueueSend(command_queue_, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
        vSemaphoreDelete(done);
        return {false, "存储命令队列已满", {}};
    }
    xTaskNotifyGive(worker_task_);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        // The worker may still own pointers in command; do not return a dangling result.
        xSemaphoreTake(done, portMAX_DELAY);
    }
    vSemaphoreDelete(done);
    return result;
}

void CaptureStorage::EnqueueTranscript(std::string* text) {
    if (worker_task_ == nullptr || command_queue_ == nullptr || text == nullptr) {
        delete text;
        return;
    }
    Command command{.type = CommandType::AppendTranscript, .text = text};
    if (xQueueSend(command_queue_, &command, 0) != pdTRUE) {
        delete text;
        NotifyError("文字记录队列已满");
        return;
    }
    xTaskNotifyGive(worker_task_);
}

void CaptureStorage::CompleteCommand(const Command& command, CaptureResult result) {
    if (command.result != nullptr) {
        *command.result = std::move(result);
    }
    if (command.done != nullptr) {
        xSemaphoreGive(command.done);
    }
}

CaptureResult CaptureStorage::MountIfNeeded() {
    if (mounted_.load()) {
        return {true, "TF 卡已挂载", kMountPoint};
    }

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = SD_MOSI_PIN;
    bus_config.miso_io_num = SD_MISO_PIN;
    bus_config.sclk_io_num = SD_CLK_PIN;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = 16 * 1024;
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return {false, std::string("TF SPI 初始化失败: ") + esp_err_to_name(ret), {}};
    }
    spi_bus_owned_ = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = SD_CS_PIN;
    slot.gpio_cd = SDSPI_SLOT_NO_CD;
    slot.gpio_wp = GPIO_NUM_NC;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 32 * 1024,
    };
    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot, &mount_config, &card_);
    if (ret != ESP_OK) {
        spi_bus_free(SD_SPI_HOST);
        spi_bus_owned_ = false;
        card_ = nullptr;
        return {false, std::string("未检测到可用 TF 卡: ") + esp_err_to_name(ret), {}};
    }
    mounted_.store(true);
    ESP_LOGI(TAG, "TF card mounted at %s", kMountPoint);
    return {true, "TF 卡挂载成功", kMountPoint};
}

CaptureResult CaptureStorage::EnsureStorageReady() {
    auto mount = MountIfNeeded();
    if (!mount.ok) {
        return mount;
    }

    auto space_status = GetFreeSpaceStatus();
    if (space_status == FreeSpaceStatus::Enough) {
        return mount;
    }
    if (space_status == FreeSpaceStatus::Low) {
        return {false, "TF 卡剩余空间不足 32 MB", {}};
    }

    // A card removed while idle leaves the VFS mount marked as active. Tear
    // down that stale mount and retry once so a newly inserted card works
    // without rebooting. An I/O failure during another capture is fatal to all
    // open files and must use the normal safe-stop path instead.
    if (audio_active_.load() || notes_active_.load()) {
        FailStorage("TF 卡访问失败，录音和记录已停止");
        return {false, "TF 卡访问失败，请重新插卡", {}};
    }

    InvalidateMount();
    mount = MountIfNeeded();
    if (!mount.ok) {
        return mount;
    }
    space_status = GetFreeSpaceStatus();
    if (space_status == FreeSpaceStatus::Enough) {
        return mount;
    }
    if (space_status == FreeSpaceStatus::Low) {
        return {false, "TF 卡剩余空间不足 32 MB", {}};
    }

    InvalidateMount();
    return {false, "TF 卡访问失败，请重新插卡", {}};
}

CaptureResult CaptureStorage::StartAudioOnWorker() {
    if (audio_active_.load()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return {true, "已经在录音", audio_path_};
    }
    auto ready = EnsureStorageReady();
    if (!ready.ok) {
        return ready;
    }

    std::string path = MakeUniquePath("audio", "wav");
    if (path.empty()) {
        if (notes_active_.load()) {
            FailStorage("TF 卡访问失败，录音和记录已停止");
        } else {
            InvalidateMount();
        }
        return {false, "无法创建录音目录", {}};
    }
    audio_file_ = fopen(path.c_str(), "wb+");
    if (audio_file_ == nullptr) {
        int error = errno;
        if (notes_active_.load()) {
            FailStorage("TF 卡访问失败，录音和记录已停止");
        } else {
            InvalidateMount();
        }
        return {false, std::string("无法创建录音文件: ") + strerror(error), {}};
    }
    audio_data_bytes_ = 0;
    next_space_check_bytes_ = 1024 * 1024;
    last_wav_checkpoint_us_ = esp_timer_get_time();
    audio_overflow_.store(false);
    xStreamBufferReset(audio_stream_);
    if (!WriteWavHeader() || fseek(audio_file_, 0, SEEK_END) != 0) {
        fclose(audio_file_);
        audio_file_ = nullptr;
        if (notes_active_.load()) {
            FailStorage("TF 卡访问失败，录音和记录已停止");
        } else {
            InvalidateMount();
        }
        return {false, "无法写入 WAV 文件头", {}};
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        audio_path_ = path;
    }
    audio_active_.store(true);
    ESP_LOGI(TAG, "Audio recording started: %s", path.c_str());
    return {true, "录音已开始", path};
}

CaptureResult CaptureStorage::StopAudioOnWorker() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        path = audio_path_;
    }
    if (!audio_active_.exchange(false) && audio_file_ == nullptr) {
        return {true, "当前没有录音", path};
    }
    bool ok = audio_file_ != nullptr && CheckpointWav(true);
    if (audio_file_ != nullptr) {
        fclose(audio_file_);
        audio_file_ = nullptr;
    }
    ESP_LOGI(TAG, "Audio recording stopped: %s", path.c_str());
    return {ok, ok ? "录音已保存" : "录音停止，但文件收尾失败", path};
}

CaptureResult CaptureStorage::StartNotesOnWorker() {
    if (notes_active_.load()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return {true, "已经在记录", notes_path_};
    }
    auto ready = EnsureStorageReady();
    if (!ready.ok) {
        return ready;
    }

    std::string path = MakeUniquePath("notes", "txt");
    if (path.empty()) {
        if (audio_active_.load()) {
            FailStorage("TF 卡访问失败，录音和记录已停止");
        } else {
            InvalidateMount();
        }
        return {false, "无法创建记录目录", {}};
    }
    notes_file_ = fopen(path.c_str(), "wb+");
    if (notes_file_ == nullptr) {
        int error = errno;
        if (audio_active_.load()) {
            FailStorage("TF 卡访问失败，录音和记录已停止");
        } else {
            InvalidateMount();
        }
        return {false, std::string("无法创建记录文件: ") + strerror(error), {}};
    }
    last_note_entry_offset_ = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        notes_path_ = path;
    }
    notes_active_.store(true);
    ESP_LOGI(TAG, "Text notes started: %s", path.c_str());
    return {true, "文字记录已开始", path};
}

CaptureResult CaptureStorage::StopNotesOnWorker(bool drop_last) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        path = notes_path_;
    }
    if (!notes_active_.exchange(false) && notes_file_ == nullptr) {
        return {true, "当前没有文字记录", path};
    }
    bool ok = true;
    if (notes_file_ != nullptr) {
        if (drop_last && last_note_entry_offset_ >= 0) {
            fflush(notes_file_);
            if (ftruncate(fileno(notes_file_), last_note_entry_offset_) != 0) {
                ok = false;
            }
        }
        if (fflush(notes_file_) != 0 || fsync(fileno(notes_file_)) != 0) {
            ok = false;
        }
        fclose(notes_file_);
        notes_file_ = nullptr;
    }
    ESP_LOGI(TAG, "Text notes stopped: %s", path.c_str());
    return {ok, ok ? "文字记录已保存" : "文字记录停止，但文件收尾失败", path};
}

void CaptureStorage::AppendTranscriptOnWorker(const std::string& text) {
    if (!notes_active_.load() || notes_file_ == nullptr) {
        return;
    }
    if (fseek(notes_file_, 0, SEEK_END) != 0) {
        FailStorage("TF 卡定位失败，录音和记录已停止");
        return;
    }
    last_note_entry_offset_ = ftello(notes_file_);
    std::string line = "[" + FormatEntryTime() + "] " + text + "\n";
    if (fwrite(line.data(), 1, line.size(), notes_file_) != line.size() ||
        fflush(notes_file_) != 0) {
        FailStorage("TF 卡写入失败，录音和记录已停止");
    }
}

void CaptureStorage::DrainAudio() {
    if (!audio_active_.load() || audio_file_ == nullptr || audio_stream_ == nullptr) {
        return;
    }
    std::array<uint8_t, 8192> buffer{};
    while (audio_active_.load()) {
        size_t bytes = xStreamBufferReceive(audio_stream_, buffer.data(), buffer.size(), 0);
        if (bytes == 0) {
            break;
        }
        if (audio_data_bytes_ + bytes > kMaxWavDataBytes) {
            FailAudio("WAV 文件已达到大小上限");
            return;
        }
        if (fwrite(buffer.data(), 1, bytes, audio_file_) != bytes) {
            FailStorage("TF 卡写入失败，录音和记录已停止");
            return;
        }
        audio_data_bytes_ += bytes;
        if (audio_data_bytes_ >= next_space_check_bytes_) {
            next_space_check_bytes_ = audio_data_bytes_ + 1024 * 1024;
            auto space_status = GetFreeSpaceStatus();
            if (space_status == FreeSpaceStatus::Low) {
                FailAudio("TF 卡剩余空间不足 32 MB，录音已停止");
                return;
            }
            if (space_status == FreeSpaceStatus::Error) {
                FailStorage("TF 卡访问失败，录音和记录已停止");
                return;
            }
        }
    }
}

void CaptureStorage::FailAudio(const std::string& message) {
    if (!audio_active_.exchange(false) && audio_file_ == nullptr) {
        return;
    }
    CheckpointWav(true);
    if (audio_file_ != nullptr) {
        fclose(audio_file_);
        audio_file_ = nullptr;
    }
    NotifyError(message);
}

void CaptureStorage::FailStorage(const std::string& message) {
    audio_active_.store(false);
    notes_active_.store(false);
    if (audio_file_ != nullptr) {
        (void)CheckpointWav(true);
        fclose(audio_file_);
        audio_file_ = nullptr;
    }
    if (notes_file_ != nullptr) {
        (void)fflush(notes_file_);
        (void)fsync(fileno(notes_file_));
        fclose(notes_file_);
        notes_file_ = nullptr;
    }
    InvalidateMount();
    NotifyError(message);
}

void CaptureStorage::InvalidateMount() {
    if (mounted_.exchange(false)) {
        esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
    }
    card_ = nullptr;
    if (spi_bus_owned_) {
        spi_bus_free(SD_SPI_HOST);
        spi_bus_owned_ = false;
    }
}

void CaptureStorage::NotifyError(const std::string& message) {
    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = error_callback_;
    }
    if (callback) {
        callback(message);
    }
}

bool CaptureStorage::EnsureDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "mkdir(%s) failed: %s", path.c_str(), strerror(errno));
    return false;
}

CaptureStorage::FreeSpaceStatus CaptureStorage::GetFreeSpaceStatus() const {
    struct statvfs stats = {};
    if (statvfs(kMountPoint, &stats) != 0) {
        ESP_LOGE(TAG, "statvfs(%s) failed: %s", kMountPoint, strerror(errno));
        return FreeSpaceStatus::Error;
    }
    uint64_t free_bytes = static_cast<uint64_t>(stats.f_bavail) * stats.f_frsize;
    return free_bytes >= kMinimumFreeBytes ? FreeSpaceStatus::Enough : FreeSpaceStatus::Low;
}

std::string CaptureStorage::MakeUniquePath(const char* category, const char* extension) {
    std::string root = std::string(kMountPoint) + "/xiaozhi";
    std::string category_root = root + "/" + category;
    if (!EnsureDirectory(root) || !EnsureDirectory(category_root)) {
        return {};
    }

    time_t now = time(nullptr);
    std::string directory;
    std::string basename;
    if (IsClockValid(now)) {
        struct tm local_time = {};
        localtime_r(&now, &local_time);
        char date[16];
        char clock[16];
        strftime(date, sizeof(date), "%Y%m%d", &local_time);
        strftime(clock, sizeof(clock), "%H%M%S", &local_time);
        directory = category_root + "/" + date;
        basename = clock;
    } else {
        directory = category_root + "/unsynced";
        basename = "boot-" + std::to_string(esp_timer_get_time() / 1000) + "-" +
                   std::to_string(++fallback_sequence_);
    }
    if (!EnsureDirectory(directory)) {
        return {};
    }

    struct stat info = {};
    for (int suffix = 0; suffix < 100; ++suffix) {
        std::string candidate = directory + "/" + basename;
        if (suffix > 0) {
            char number[8];
            snprintf(number, sizeof(number), "-%02d", suffix);
            candidate += number;
        }
        candidate += ".";
        candidate += extension;
        if (stat(candidate.c_str(), &info) != 0 && errno == ENOENT) {
            return candidate;
        }
    }
    return {};
}

std::string CaptureStorage::FormatEntryTime() const {
    time_t now = time(nullptr);
    if (IsClockValid(now)) {
        struct tm local_time = {};
        localtime_r(&now, &local_time);
        char clock[16];
        strftime(clock, sizeof(clock), "%H:%M:%S", &local_time);
        return clock;
    }
    uint64_t total_seconds = esp_timer_get_time() / 1000000;
    char elapsed[24];
    snprintf(elapsed, sizeof(elapsed), "+%02llu:%02llu:%02llu",
             static_cast<unsigned long long>(total_seconds / 3600),
             static_cast<unsigned long long>((total_seconds / 60) % 60),
             static_cast<unsigned long long>(total_seconds % 60));
    return elapsed;
}

bool CaptureStorage::WriteWavHeader() {
    if (audio_file_ == nullptr || audio_data_bytes_ > UINT32_MAX - 36) {
        return false;
    }
    std::array<uint8_t, 44> header{};
    memcpy(header.data(), "RIFF", 4);
    PutLe32(header.data() + 4, static_cast<uint32_t>(36 + audio_data_bytes_));
    memcpy(header.data() + 8, "WAVEfmt ", 8);
    PutLe32(header.data() + 16, 16);
    PutLe16(header.data() + 20, 1);
    PutLe16(header.data() + 22, 1);
    PutLe32(header.data() + 24, 16000);
    PutLe32(header.data() + 28, 16000 * sizeof(int16_t));
    PutLe16(header.data() + 32, sizeof(int16_t));
    PutLe16(header.data() + 34, 16);
    memcpy(header.data() + 36, "data", 4);
    PutLe32(header.data() + 40, static_cast<uint32_t>(audio_data_bytes_));
    if (fseek(audio_file_, 0, SEEK_SET) != 0 ||
        fwrite(header.data(), 1, header.size(), audio_file_) != header.size()) {
        return false;
    }
    return true;
}

bool CaptureStorage::CheckpointWav(bool force_sync) {
    if (audio_file_ == nullptr || !WriteWavHeader() || fseek(audio_file_, 0, SEEK_END) != 0 ||
        fflush(audio_file_) != 0) {
        return false;
    }
    if (force_sync && fsync(fileno(audio_file_)) != 0) {
        return false;
    }
    last_wav_checkpoint_us_ = esp_timer_get_time();
    return true;
}
