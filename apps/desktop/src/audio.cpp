#include "audio.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core_bridge.hpp"
#include "i18n.hpp"

namespace nbtvgui {

namespace {

std::vector<AudioDevice> list_devices(bool input) {
    std::vector<AudioDevice> devices;
    int count = 0;
    SDL_AudioDeviceID* ids = input ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
    if (ids == nullptr) return devices;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        if (name != nullptr && *name != '\0') devices.push_back({name, ids[i]});
    }
    SDL_free(ids);
    return devices;
}

}  // namespace

std::vector<AudioDevice> audio_inputs() { return list_devices(true); }
std::vector<AudioDevice> audio_outputs() { return list_devices(false); }

std::optional<AudioDevice> resolve_audio_device(const std::string& name, bool input) {
    if (name.empty()) {
        // 系統預設的裝置。一個裝置都沒有的時候，開預設的也只會失敗，直接說沒有
        if (list_devices(input).empty()) return std::nullopt;
        return AudioDevice{L("系統預設"), input ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK};
    }
    for (const AudioDevice& device : list_devices(input)) {
        if (device.name == name) return device;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- 輸入

AudioInput::~AudioInput() { stop(); }

bool AudioInput::start(SDL_AudioDeviceID device, std::string* error) {
    stop();
    // 照裝置自己的聲道數收進來，只取第一個聲道（兩個聲道接的可能是兩部接收機，混在一起就壞了）
    SDL_AudioSpec native{};
    int frames = 0;
    channels_ = SDL_GetAudioDeviceFormat(device, &native, &frames) && native.channels > 0 ? native.channels : 1;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = channels_;
    spec.freq = kSampleRate;
    stream_ = SDL_OpenAudioDeviceStream(device, &spec, &AudioInput::callback, this);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
        if (error != nullptr) *error = SDL_GetError();
        stop();
        return false;
    }
    return true;
}

void AudioInput::stop() {
    if (stream_ == nullptr) return;
    SDL_DestroyAudioStream(stream_);   // 裝置跟著關掉；回來之後不會再有回呼
    stream_ = nullptr;
}

void SDLCALL AudioInput::callback(void* userdata, SDL_AudioStream* stream, int, int) {
    auto* self = static_cast<AudioInput*>(userdata);
    const int frame_bytes = static_cast<int>(sizeof(float)) * self->channels_;
    const int available = SDL_GetAudioStreamAvailable(stream) / frame_bytes * frame_bytes;
    if (available <= 0) return;
    self->scratch_.resize(static_cast<std::size_t>(available) / sizeof(float));
    const int got = SDL_GetAudioStreamData(stream, self->scratch_.data(), available);
    if (got <= 0) return;
    const std::size_t frames = static_cast<std::size_t>(got) / static_cast<std::size_t>(frame_bytes);
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) mono[i] = self->scratch_[i * static_cast<std::size_t>(self->channels_)];
    if (self->on_samples) self->on_samples(std::move(mono));
}

// ---------------------------------------------------------------- 輸出

AudioOutput::AudioOutput(double queue_seconds) : capacity_(static_cast<std::size_t>(queue_seconds * kSampleRate)), ring_(capacity_) {}

AudioOutput::~AudioOutput() { stop(); }

void AudioOutput::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    enqueued_ = played_ = rendered_ = silent_ = 0;
    underruns_ = 0;
}

bool AudioOutput::start(SDL_AudioDeviceID device, std::string* error) {
    stop();
    reset();
    if (simulated) {
        simulating_ = true;
        simulator_ = std::thread([this] {
            // 每 10 毫秒取走這段時間該播掉的取樣，和真的裝置一樣照時間前進
            const auto began = std::chrono::steady_clock::now();
            std::int64_t taken = 0;
            std::vector<float> block;
            while (simulating_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                if (simulated_stall_after && elapsed > *simulated_stall_after) continue;   // 裝置「卡住」：不再來取
                const std::int64_t due = static_cast<std::int64_t>(elapsed * kSampleRate);
                if (due <= taken) continue;
                block.resize(static_cast<std::size_t>(due - taken));
                render(block.data(), block.size());
                taken = due;
                if (simulated_sink) simulated_sink(block);
            }
        });
        return true;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = kSampleRate;
    stream_ = SDL_OpenAudioDeviceStream(device, &spec, &AudioOutput::callback, this);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
        if (error != nullptr) *error = SDL_GetError();
        stop();
        return false;
    }
    return true;
}

void AudioOutput::stop() {
    if (simulating_.exchange(false) && simulator_.joinable()) simulator_.join();
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

bool AudioOutput::enqueue(const float* samples, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<std::size_t>(enqueued_ - played_) + count > capacity_) return false;
    for (std::size_t i = 0; i < count; ++i) ring_[static_cast<std::size_t>((enqueued_ + static_cast<std::int64_t>(i)) % static_cast<std::int64_t>(capacity_))] = samples[i];
    enqueued_ += static_cast<std::int64_t>(count);
    return true;
}

bool AudioOutput::discard_queued(std::int64_t from) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (from < played_ || from > enqueued_) return false;
    enqueued_ = from;
    return true;
}

std::int64_t AudioOutput::played_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return played_;
}

std::int64_t AudioOutput::queued_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enqueued_ - played_;
}

std::int64_t AudioOutput::rendered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rendered_;
}

std::int64_t AudioOutput::silent_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return silent_;
}

int AudioOutput::underruns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return underruns_;
}

void AudioOutput::render(float* out, std::size_t count) {
    const float volume = gain.load();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t have = static_cast<std::size_t>(enqueued_ - played_);
    const std::size_t n = std::min(have, count);
    for (std::size_t i = 0; i < n; ++i) out[i] = volume * ring_[static_cast<std::size_t>((played_ + static_cast<std::int64_t>(i)) % static_cast<std::int64_t>(capacity_))];
    std::fill(out + n, out + count, 0.0f);
    if (n < count && have > 0) ++underruns_;   // 這一次把佇列取空了
    played_ += static_cast<std::int64_t>(n);
    rendered_ += static_cast<std::int64_t>(count);
    silent_ += static_cast<std::int64_t>(count - n);
}

void SDLCALL AudioOutput::callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
    auto* self = static_cast<AudioOutput*>(userdata);
    if (additional_amount <= 0) return;
    const std::size_t count = static_cast<std::size_t>(additional_amount) / sizeof(float);
    if (count == 0) return;
    self->scratch_.resize(count);
    self->render(self->scratch_.data(), count);
    SDL_PutAudioStreamData(stream, self->scratch_.data(), static_cast<int>(count * sizeof(float)));
}

}  // namespace nbtvgui
