#include "voice.hpp"

#include <algorithm>
#include <cmath>

#include "i18n.hpp"
#include "nbtv/voice.h"

namespace nbtvgui {

namespace voice {

bool available() { return nbtv_voice_available() != 0; }

std::string codec_name() { return nbtv_voice_description(); }

bool supports(const ModeInfo* mode) { return mode != nullptr && (mode->pilot_free() || mode->large_picture()); }

double slow_factor(VoiceLayout layout) { return nbtv_duplex_slow_factor(duplex_code(layout)); }

double slowest_factor() { return std::max(slow_factor(VoiceLayout::standard), slow_factor(VoiceLayout::narrow)); }

}  // namespace voice

// ---------------------------------------------------------------- 發射端

std::unique_ptr<VoiceSender> VoiceSender::create(VoiceLayout layout) {
    nbtv_voice_tx* encoder = nbtv_voice_tx_create();
    if (encoder == nullptr) return nullptr;
    nbtv_duplex_tx* mixer = nbtv_duplex_tx_create(duplex_code(layout));
    if (mixer == nullptr) {
        nbtv_voice_tx_destroy(encoder);
        return nullptr;
    }
    return std::unique_ptr<VoiceSender>(new VoiceSender(encoder, mixer));
}

VoiceSender::VoiceSender(nbtv_voice_tx* encoder, nbtv_duplex_tx* mixer) : encoder_(encoder), mixer_(mixer), iq_(2 * 2048) {}

VoiceSender::~VoiceSender() {
    nbtv_voice_tx_destroy(encoder_);
    nbtv_duplex_tx_destroy(mixer_);
}

std::vector<float> VoiceSender::process(const float* speech, int count) {
    nbtv_voice_tx_push(encoder_, speech, count);
    return drain();
}

std::vector<float> VoiceSender::finish() {
    nbtv_voice_tx_finish(encoder_);
    std::vector<float> out = drain();
    const std::vector<float> quiet(2 * 400, 0.0f);   // 50 毫秒的靜音
    nbtv_duplex_tx_push_voice(mixer_, quiet.data(), 400);
    const std::vector<float> rest = take();
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

std::vector<float> VoiceSender::drain() {
    for (int n; (n = nbtv_voice_tx_read(encoder_, iq_.data(), static_cast<int>(iq_.size() / 2))) > 0;) nbtv_duplex_tx_push_voice(mixer_, iq_.data(), n);
    return take();
}

std::vector<float> VoiceSender::take() {
    const int pending = nbtv_duplex_tx_pending_voice(mixer_);
    std::vector<float> out(static_cast<std::size_t>(std::max(pending, 0)));
    if (pending > 0) nbtv_duplex_tx_read(mixer_, out.data(), pending);
    return out;
}

// ---------------------------------------------------------------- 接收端

std::unique_ptr<VoiceReceiver> VoiceReceiver::create(VoiceLayout layout) {
    nbtv_voice_rx* decoder = nbtv_voice_rx_create();
    if (decoder == nullptr) return nullptr;
    nbtv_duplex_rx* splitter = nbtv_duplex_rx_create(duplex_code(layout));
    if (splitter == nullptr) {
        nbtv_voice_rx_destroy(decoder);
        return nullptr;
    }
    nbtv_duplex_rx_enable(splitter, 0, 1);   // 畫面那一路由另一個接收機自己分
    return std::unique_ptr<VoiceReceiver>(new VoiceReceiver(layout, splitter, decoder));
}

VoiceReceiver::VoiceReceiver(VoiceLayout layout, nbtv_duplex_rx* splitter, nbtv_voice_rx* decoder)
    : layout_(layout), splitter_(splitter), decoder_(decoder), iq_(2 * 2048), speech_(9600) {}

VoiceReceiver::~VoiceReceiver() {
    nbtv_duplex_rx_destroy(splitter_);
    nbtv_voice_rx_destroy(decoder_);
}

void VoiceReceiver::reset() {
    nbtv_duplex_rx_reset(splitter_);
    nbtv_voice_rx_reset(decoder_);
}

std::vector<float> VoiceReceiver::process(const float* samples, int count) {
    nbtv_duplex_rx_push(splitter_, samples, count);
    for (int n; (n = nbtv_duplex_rx_read_voice(splitter_, iq_.data(), static_cast<int>(iq_.size() / 2))) > 0;) nbtv_voice_rx_push(decoder_, iq_.data(), n);
    std::vector<float> out;
    for (int n; (n = nbtv_voice_rx_read(decoder_, speech_.data(), static_cast<int>(speech_.size()))) > 0;) out.insert(out.end(), speech_.begin(), speech_.begin() + n);
    return out;
}

VoiceReceiver::Status VoiceReceiver::status() const {
    nbtv_voice_rx_status raw{};
    nbtv_voice_rx_get_status(decoder_, &raw);
    Status status;
    status.synced = raw.synced != 0;
    status.snr_db = raw.snr_db;
    status.overs_ended = raw.overs_ended;
    return status;
}

// ---------------------------------------------------------------- 發射時語音走的那一條路

VoiceTransmitPath::VoiceTransmitPath() {
    microphone_.on_samples = [this](std::vector<float> samples) {
        worker_.post([this, samples = std::move(samples)] { feed(samples); });
    };
}

VoiceTransmitPath::~VoiceTransmitPath() { stop(); }

bool VoiceTransmitPath::start(VoiceLayout layout, std::optional<SDL_AudioDeviceID> microphone, AudioOutput* output, std::string* error) {
    bool ready = false;
    worker_.sync([&] {
        sender_ = VoiceSender::create(layout);
        output_ = output;
        active_ = sender_ != nullptr;
        ready = active_;
    });
    level_.store(-120.0);
    if (!ready) {
        if (error != nullptr) *error = L("語音的編解碼器開不起來");
        return false;
    }
    if (!microphone) return true;
    if (microphone_.start(*microphone, error)) return true;
    worker_.sync([this] { active_ = false; });
    return false;
}

void VoiceTransmitPath::speak(std::vector<float> samples) {
    worker_.post([this, samples = std::move(samples)] { feed(samples); });
}

void VoiceTransmitPath::finish() {
    microphone_.stop();
    worker_.sync([this] {
        if (!active_ || !sender_) return;
        active_ = false;
        const std::vector<float> tail = sender_->finish();
        if (!tail.empty() && output_ != nullptr) output_->enqueue_voice(tail.data(), tail.size());
    });
}

void VoiceTransmitPath::stop() {
    microphone_.stop();
    worker_.sync([this] {
        active_ = false;
        sender_.reset();
        output_ = nullptr;
    });
    level_.store(-120.0);
}

void VoiceTransmitPath::feed(const std::vector<float>& samples) {
    if (!active_ || !sender_ || samples.empty()) return;
    double energy = 0;
    for (float v : samples) energy += static_cast<double>(v) * v;
    level_.store(10.0 * std::log10(std::max(energy / static_cast<double>(samples.size()), 1e-12)));
    const std::vector<float> voice = sender_->process(samples.data(), static_cast<int>(samples.size()));
    if (!voice.empty() && output_ != nullptr) output_->enqueue_voice(voice.data(), voice.size());
}

}  // namespace nbtvgui
