// include/nbtv/nbtv.h 的實作。
#include "nbtv/nbtv.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "analog_tables.hpp"
#include "av1_payload.hpp"
#include "callsign.hpp"
#include "digital_frame.hpp"
#include "dsp.hpp"
#include "image_codec.hpp"
#include "learned_codec.hpp"
#include "modes.hpp"
#include "receiver.hpp"
#include "spectrum.hpp"
#include "transmitter.hpp"

#define NBTV_STR_(x) #x
#define NBTV_STR(x) NBTV_STR_(x)

static_assert(NBTV_IMAGE_RGB_BYTES == nbtv::kImageRgbBytes);
static_assert(NBTV_CALLSIGN_MAX == nbtv::kCallsignMaxChars);
static_assert(NBTV_SPECTRUM_BINS == nbtv::kSpectrumBins);
static_assert(NBTV_AVIF_OVERHEAD == nbtv::kAvifShellBytes + nbtv::kAvifTemporalDelimiterBytes);

namespace {

using nbtv::Mode;

void fill(const Mode& m, nbtv_mode_info* out) {
    *out = nbtv_mode_info{};
    out->id = m.id;
    out->kind = m.is_digital() ? NBTV_MODE_DIGITAL : m.is_learned() ? NBTV_MODE_LEARNED : NBTV_MODE_ANALOG;
    out->payload_symbols = m.payload_symbols;
    out->frame_symbols = m.frame_symbols();
    out->frame_samples = m.frame_samples();
    out->frame_seconds = m.frame_seconds();
    out->data_cells = m.data_cells();
    out->bits_per_cell = m.bits_per_cell;
    out->code_rate_num = m.rate_num;
    out->code_rate_den = m.rate_den;
    out->ldpc_info_bits = m.ldpc_info_bits;
    out->payload_bytes = m.payload_bytes();
    out->image_bytes = m.image_bytes();
    out->padding_cells = m.padding_cells();
    out->header_cells = m.header_cells();
    out->analog_values = m.analog_values();
    out->analog_coefficients = m.is_analog() ? nbtv::analog_plan(m).coefficients : 0;
    out->learned_values = m.learned_values;
}

constexpr bool valid_rate(int sample_rate) { return sample_rate == 12000 || sample_rate == 48000; }

constexpr int max_image_bytes() {
    int n = 0;
    for (const Mode& m : nbtv::kModes) n = std::max(n, m.image_bytes());
    return n;
}
static_assert(NBTV_MAX_IMAGE_BYTES == max_image_bytes());

std::span<const std::uint8_t> rgb_span(const std::uint8_t* rgb) { return {rgb, static_cast<std::size_t>(nbtv::kImageRgbBytes)}; }

}  // namespace

struct nbtv_model {
    std::shared_ptr<const nbtv::LearnedCodec> codec;
};

struct nbtv_tx {
    int rate = nbtv::kSampleRate;
    const Mode* mode = nbtv::find_mode(1);
    int speed = nbtv::kDefaultEncoderSpeed;
    nbtv::Transmitter tx;
    nbtv::Interpolator4 up;
    bool need_flush = false;

    // 編最近那一幀之前的狀態，供 nbtv_tx_discard_last 還原
    struct Undo {
        bool valid = false;
        int sequence = 0;
        nbtv::Interpolator4 up;
        bool need_flush = false;
    } undo;

    // 每次編碼之前呼叫
    void remember() {
        undo.valid = true;
        undo.sequence = tx.sequence();
        undo.up = up;
        undo.need_flush = need_flush;
    }

    int out_samples(std::size_t n) const { return static_cast<int>(n) * (rate / nbtv::kSampleRate); }

    // 12 kHz 的一幀 → 輸出取樣率，寫進 samples
    int deliver(const std::vector<float>& frame, float* samples, int capacity) {
        if (rate == nbtv::kSampleRate) {
            std::copy(frame.begin(), frame.end(), samples);
            return static_cast<int>(frame.size());
        }
        std::vector<float> out;
        out.reserve(frame.size() * 4);
        up.process(frame, out);
        need_flush = true;
        (void)capacity;
        std::copy(out.begin(), out.end(), samples);
        return static_cast<int>(out.size());
    }
};

struct nbtv_rx {
    int rate = nbtv::kSampleRate;
    nbtv::Receiver rx;
    nbtv::Decimator4 down;
    std::vector<float> scratch;
};

struct nbtv_spectrum {
    int rate = nbtv::kSampleRate;
    nbtv::Spectrum spectrum;
    nbtv::Decimator4 down;
    std::vector<float> scratch;
};

extern "C" {

const char* nbtv_version_string(void) {
    return NBTV_STR(NBTV_VERSION_MAJOR) "." NBTV_STR(NBTV_VERSION_MINOR) "." NBTV_STR(NBTV_VERSION_PATCH);
}

/* ---- 模式 ---- */

int nbtv_mode_count(void) { return static_cast<int>(nbtv::kModes.size()); }

int nbtv_mode_at(int index, nbtv_mode_info* out) {
    if (out == nullptr || index < 0 || index >= nbtv_mode_count()) return -1;
    fill(nbtv::kModes[static_cast<std::size_t>(index)], out);
    return 0;
}

int nbtv_mode_by_id(int id, nbtv_mode_info* out) {
    const Mode* m = nbtv::find_mode(id);
    if (out == nullptr || m == nullptr) return -1;
    fill(*m, out);
    return 0;
}

/* ---- 學習式模式的模型 ---- */

nbtv_model* nbtv_model_load(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return nullptr;
    std::unique_ptr<nbtv::LearnedCodec> codec = nbtv::LearnedCodec::load({data, size});
    if (codec == nullptr) return nullptr;
    nbtv_model* model = new nbtv_model;
    model->codec = std::move(codec);
    return model;
}

void nbtv_model_destroy(nbtv_model* model) { delete model; }

uint32_t nbtv_model_id(const nbtv_model* model) { return model != nullptr ? model->codec->model_id() : 0; }

int nbtv_model_values(const nbtv_model* model) { return model != nullptr ? model->codec->value_count() : 0; }

/* ---- 呼號 ---- */

int nbtv_callsign_normalize(const char* text, char* out) {
    if (out == nullptr) return 0;
    const std::string call = nbtv::normalize_callsign(text != nullptr ? text : "");
    std::memcpy(out, call.c_str(), call.size() + 1);
    return static_cast<int>(call.size());
}

/* ---- 影像 ---- */

int nbtv_has_image_codec(void) { return nbtv::image_codec_available() ? 1 : 0; }

int nbtv_image_encode(const uint8_t* rgb, int max_bytes, int speed, uint8_t* out, int out_capacity) {
    if (rgb == nullptr || out == nullptr) return -1;
    nbtv::ImageEncodeResult r;
    if (!nbtv::image_encode(rgb_span(rgb), max_bytes, speed < 0 ? nbtv::kDefaultEncoderSpeed : speed, r)) return -1;
    if (static_cast<int>(r.payload.size()) > out_capacity) return -1;
    std::copy(r.payload.begin(), r.payload.end(), out);
    return static_cast<int>(r.payload.size());
}

int nbtv_image_decode(const uint8_t* payload, int length, uint8_t* rgb) {
    if (payload == nullptr || rgb == nullptr || length <= 0) return -1;
    const bool ok = nbtv::image_decode({payload, static_cast<std::size_t>(length)}, {rgb, static_cast<std::size_t>(nbtv::kImageRgbBytes)});
    return ok ? 0 : -1;
}

int nbtv_avif_from_payload(const uint8_t* payload, int length, uint8_t* out, int out_capacity) {
    if (payload == nullptr || out == nullptr || length <= 0) return -1;
    const std::vector<std::uint8_t> file = nbtv::avif_from_payload({payload, static_cast<std::size_t>(length)});
    if (file.empty() || static_cast<int>(file.size()) > out_capacity) return -1;
    std::copy(file.begin(), file.end(), out);
    return static_cast<int>(file.size());
}

/* ---- 發射 ---- */

nbtv_tx* nbtv_tx_create(int sample_rate) {
    if (!valid_rate(sample_rate)) return nullptr;
    nbtv_tx* tx = new nbtv_tx;
    tx->rate = sample_rate;
    return tx;
}

void nbtv_tx_destroy(nbtv_tx* tx) { delete tx; }

int nbtv_tx_set_mode(nbtv_tx* tx, int mode_id) {
    const Mode* m = nbtv::find_mode(mode_id);
    if (tx == nullptr || m == nullptr) return -1;
    tx->mode = m;
    return 0;
}

int nbtv_tx_mode(const nbtv_tx* tx) { return tx != nullptr ? tx->mode->id : -1; }

void nbtv_tx_set_callsign(nbtv_tx* tx, const char* callsign) {
    if (tx != nullptr) tx->tx.set_callsign(nbtv::normalize_callsign(callsign != nullptr ? callsign : ""));
}

void nbtv_tx_set_encoder_speed(nbtv_tx* tx, int speed) {
    if (tx != nullptr) tx->speed = speed < 0 ? nbtv::kDefaultEncoderSpeed : std::min(speed, 10);
}

void nbtv_tx_set_model(nbtv_tx* tx, const nbtv_model* model) {
    if (tx != nullptr) tx->tx.set_model(model != nullptr ? model->codec : nullptr);
}

void nbtv_tx_set_papr_db(nbtv_tx* tx, float papr_db) {
    if (tx != nullptr) tx->tx.set_papr_db(papr_db);
}

int nbtv_tx_frame_samples(const nbtv_tx* tx) {
    return tx != nullptr ? tx->out_samples(static_cast<std::size_t>(tx->mode->frame_samples())) : 0;
}

int nbtv_tx_encode_image(nbtv_tx* tx, const uint8_t* rgb, float* samples, int capacity, uint8_t* preview_rgb,
                         nbtv_tx_frame_info* info) {
    if (tx == nullptr || rgb == nullptr || samples == nullptr || capacity < nbtv_tx_frame_samples(tx)) return -1;
    const Mode& mode = *tx->mode;
    nbtv_tx_frame_info local{};
    local.mode_id = mode.id;
    local.sequence = tx->tx.sequence();
    std::vector<float> frame;
    if (mode.is_digital()) {
        nbtv::ImageEncodeResult enc;
        if (!nbtv::image_encode(rgb_span(rgb), mode.image_bytes(), tx->speed, enc)) return -1;
        tx->remember();
        local.image_bytes = static_cast<int>(enc.payload.size());
        local.quantizer = enc.quantizer;
        if (preview_rgb != nullptr &&
            !nbtv::image_decode(enc.payload, {preview_rgb, static_cast<std::size_t>(nbtv::kImageRgbBytes)})) {
            std::memcpy(preview_rgb, rgb, static_cast<std::size_t>(nbtv::kImageRgbBytes));
        }
        frame = tx->tx.digital_frame(mode, enc.payload, static_cast<int>(nbtv::ImageFormat::Av1));
    } else if (mode.is_learned()) {
        if (tx->tx.model_for(mode) == nullptr) return -1;
        std::vector<std::uint8_t> preview;
        tx->remember();
        frame = tx->tx.learned_frame(mode, rgb_span(rgb), preview_rgb != nullptr ? &preview : nullptr);
        if (frame.empty()) {
            tx->undo.valid = false;
            return -1;
        }
        if (preview_rgb != nullptr) std::copy(preview.begin(), preview.end(), preview_rgb);
    } else {
        std::vector<std::uint8_t> preview;
        tx->remember();
        frame = tx->tx.analog_frame(mode, rgb_span(rgb), preview_rgb != nullptr ? &preview : nullptr);
        if (preview_rgb != nullptr) std::copy(preview.begin(), preview.end(), preview_rgb);
        local.coefficients = nbtv::analog_plan(mode).coefficients;
    }
    if (info != nullptr) *info = local;
    return tx->deliver(frame, samples, capacity);
}

int nbtv_tx_encode_payload(nbtv_tx* tx, const uint8_t* payload, int length, float* samples, int capacity,
                           nbtv_tx_frame_info* info) {
    if (tx == nullptr || payload == nullptr || samples == nullptr || capacity < nbtv_tx_frame_samples(tx)) return -1;
    const Mode& mode = *tx->mode;
    if (!mode.is_digital() || length < 0 || length > mode.image_bytes()) return -1;
    if (info != nullptr) {
        *info = nbtv_tx_frame_info{};
        info->mode_id = mode.id;
        info->sequence = tx->tx.sequence();
        info->image_bytes = length;
    }
    tx->remember();
    const std::vector<float> frame =
        tx->tx.digital_frame(mode, {payload, static_cast<std::size_t>(length)}, static_cast<int>(nbtv::ImageFormat::Av1));
    return tx->deliver(frame, samples, capacity);
}

int nbtv_tx_discard_last(nbtv_tx* tx) {
    if (tx == nullptr || !tx->undo.valid) return -1;
    tx->tx.set_sequence(tx->undo.sequence);
    tx->up = tx->undo.up;
    tx->need_flush = tx->undo.need_flush;
    tx->undo.valid = false;
    return 0;
}

int nbtv_tx_finish(nbtv_tx* tx, float* samples, int capacity) {
    if (tx != nullptr) tx->undo.valid = false;
    if (tx == nullptr || samples == nullptr || !tx->need_flush) return 0;
    // 推入足夠的 0，把濾波器裡的尾巴擠出來
    const std::size_t zeros = static_cast<std::size_t>(tx->up.delay_samples() + 3) / 4 + 1;
    std::vector<float> out;
    tx->up.process(std::vector<float>(zeros, 0.0f), out);
    tx->up.reset();
    tx->need_flush = false;
    const int n = std::min(static_cast<int>(out.size()), capacity);
    std::copy_n(out.begin(), n, samples);
    return n;
}

/* ---- 接收 ---- */

nbtv_rx* nbtv_rx_create(int sample_rate) {
    if (!valid_rate(sample_rate)) return nullptr;
    nbtv_rx* rx = new nbtv_rx;
    rx->rate = sample_rate;
    return rx;
}

void nbtv_rx_destroy(nbtv_rx* rx) { delete rx; }

void nbtv_rx_reset(nbtv_rx* rx) {
    if (rx == nullptr) return;
    rx->rx.reset();
    rx->down.reset();
}

void nbtv_rx_set_model(nbtv_rx* rx, const nbtv_model* model) {
    if (rx != nullptr) rx->rx.set_model(model != nullptr ? model->codec : nullptr);
}

void nbtv_rx_push(nbtv_rx* rx, const float* samples, int count) {
    if (rx == nullptr || samples == nullptr || count <= 0) return;
    const std::span<const float> in(samples, static_cast<std::size_t>(count));
    if (rx->rate == nbtv::kSampleRate) {
        rx->rx.push(in);
    } else {
        rx->scratch.clear();
        rx->down.process(in, rx->scratch);
        rx->rx.push(rx->scratch);
    }
}

void nbtv_rx_get_status(const nbtv_rx* rx, nbtv_rx_status* out) {
    if (rx == nullptr || out == nullptr) return;
    const nbtv::RxStatus s = rx->rx.status();
    *out = nbtv_rx_status{};
    out->synced = s.synced ? 1 : 0;
    out->mode_id = s.mode_id;
    out->snr_valid = s.snr_valid ? 1 : 0;
    out->snr_db = s.snr_db;
    out->freq_offset_hz = s.freq_offset_hz;
    out->frame_symbols_done = s.symbols_done;
    out->frame_symbols_total = s.symbols_total;
    out->input_level_db = s.input_level_db;
    out->frames_decoded = static_cast<int>(s.frames_decoded);
    out->frames_failed = static_cast<int>(s.frames_failed);
}

int nbtv_rx_poll_frame(nbtv_rx* rx, nbtv_rx_frame* out) {
    if (rx == nullptr || out == nullptr) return 0;
    nbtv::RxFrame f;
    if (!rx->rx.poll(f)) return 0;
    std::memset(out, 0, sizeof *out);
    out->mode_id = f.mode_id;
    out->kind = f.is_digital ? NBTV_MODE_DIGITAL : f.is_learned ? NBTV_MODE_LEARNED : NBTV_MODE_ANALOG;
    out->model_missing = f.model_missing ? 1 : 0;
    out->decoded = f.decoded ? 1 : 0;
    out->header_ok = f.header_ok ? 1 : 0;
    std::strncpy(out->callsign, f.callsign.c_str(), NBTV_CALLSIGN_MAX);
    out->sequence = f.header_ok ? f.sequence : -1;
    out->snr_db = f.snr_db;
    out->freq_offset_hz = f.freq_offset_hz;
    if (f.is_digital) {
        if (f.decoded && f.image_format == static_cast<int>(nbtv::ImageFormat::Av1)) {
            const std::size_t length = std::min<std::size_t>(nbtv::av1_payload_length(f.image_data), NBTV_MAX_IMAGE_BYTES);
            out->image_bytes = static_cast<int>(length);
            std::copy_n(f.image_data.begin(), length, out->image_data);
            out->has_image = length > 0 && nbtv::image_decode({out->image_data, length}, {out->rgb, sizeof out->rgb}) ? 1 : 0;
        }
    } else if (f.decoded && f.rgb.size() == sizeof out->rgb) {
        std::copy(f.rgb.begin(), f.rgb.end(), out->rgb);
        out->has_image = 1;
    }
    return 1;
}

/* ---- 頻譜 ---- */

nbtv_spectrum* nbtv_spectrum_create(int sample_rate) {
    if (!valid_rate(sample_rate)) return nullptr;
    nbtv_spectrum* sp = new nbtv_spectrum;
    sp->rate = sample_rate;
    return sp;
}

void nbtv_spectrum_destroy(nbtv_spectrum* sp) { delete sp; }

void nbtv_spectrum_reset(nbtv_spectrum* sp) {
    if (sp == nullptr) return;
    sp->spectrum.reset();
    sp->down.reset();
}

void nbtv_spectrum_push(nbtv_spectrum* sp, const float* samples, int count) {
    if (sp == nullptr || samples == nullptr || count <= 0) return;
    const std::span<const float> in(samples, static_cast<std::size_t>(count));
    if (sp->rate == nbtv::kSampleRate) {
        sp->spectrum.push(in);
    } else {
        sp->scratch.clear();
        sp->down.process(in, sp->scratch);
        sp->spectrum.push(sp->scratch);
    }
}

int nbtv_spectrum_poll(nbtv_spectrum* sp, float* db) {
    if (sp == nullptr || db == nullptr) return 0;
    return sp->spectrum.poll({db, static_cast<std::size_t>(nbtv::kSpectrumBins)}) ? 1 : 0;
}

}  // extern "C"
