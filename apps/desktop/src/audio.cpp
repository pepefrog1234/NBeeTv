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
    flowing_ = voice_flowing_ = false;
    waited_ = voice_waited_ = 0;
    voice_begun_ = false;
    voice_gap_ = 0;
    voice_gaps_ = 0;
    voice_.clear();
    rendered_tap_.clear();
}

void AudioOutput::enqueue_voice(const float* samples, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t limit = static_cast<std::size_t>(2 * kSampleRate);   // 最多積兩秒
    const std::size_t room = voice_.size() < limit ? limit - voice_.size() : 0;
    voice_.insert(voice_.end(), samples, samples + std::min(count, room));
}

std::int64_t AudioOutput::voice_queued_samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::int64_t>(voice_.size());
}

int AudioOutput::voice_gaps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return voice_gaps_;
}

std::vector<float> AudioOutput::take_rendered() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<float> taken;
    taken.swap(rendered_tap_);
    return taken;
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
    // 墊不夠也不會一直等：等了這麼久（半秒）還是只有一點點，就是後面沒有了，把剩下的播掉
    const std::size_t wait_limit = static_cast<std::size_t>(kSampleRate / 2);
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t have = static_cast<std::size_t>(enqueued_ - played_);
    std::size_t n = 0;
    if (start_level > 0 && !flowing_ && have < start_level && (have == 0 || waited_ < wait_limit)) {
        if (have > 0) waited_ += count;   // 還沒墊夠：先不取
    } else {
        flowing_ = true;
        waited_ = 0;
        n = std::min(have, count);
        for (std::size_t i = 0; i < n; ++i) out[i] = ring_[static_cast<std::size_t>((played_ + static_cast<std::int64_t>(i)) % static_cast<std::int64_t>(capacity_))];
        if (n < count) flowing_ = false;
    }
    std::fill(out + n, out + count, 0.0f);
    // 語音那一半有的話加上去
    std::size_t voice_from = 0;   // 這一批從第幾個取樣開始加語音
    if (!voice_flowing_) {
        if (voice_.size() >= std::max<std::size_t>(voice_start_level, 1) || (!voice_.empty() && voice_waited_ >= wait_limit)) {
            // 中途停過：停的長度要是 voice_gap_step 的整數倍才接回去（這一批裡輪不到的話，下一批再看）
            const std::size_t pad = voice_begun_ && voice_gap_step > 0 ? (voice_gap_step - voice_gap_ % voice_gap_step) % voice_gap_step : 0;
            if (pad < count) {
                voice_from = pad;
                if (voice_begun_) ++voice_gaps_;
                voice_flowing_ = true;
                voice_begun_ = true;
                voice_waited_ = 0;
                voice_gap_ = 0;
            }
        } else if (!voice_.empty()) {
            voice_waited_ += count;   // 還沒墊夠：先不加
        }
        if (!voice_flowing_ && voice_begun_) voice_gap_ += count;
    }
    if (voice_flowing_) {
        const std::size_t voice = std::min(voice_.size(), count - voice_from);
        for (std::size_t i = 0; i < voice; ++i) out[voice_from + i] = std::clamp(out[voice_from + i] + voice_[i], -1.0f, 1.0f);
        voice_.erase(voice_.begin(), voice_.begin() + static_cast<std::ptrdiff_t>(voice));
        if (voice < count - voice_from) {
            voice_flowing_ = false;
            voice_gap_ = count - voice_from - voice;
        }
    }
    if (keep_rendered && rendered_tap_.size() < static_cast<std::size_t>(2 * kSampleRate)) rendered_tap_.insert(rendered_tap_.end(), out, out + count);
    if (volume != 1.0f) {
        for (std::size_t i = 0; i < count; ++i) out[i] *= volume;
    }
    if (n > 0 && n < count) ++underruns_;   // 這一次把佇列取空了
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
