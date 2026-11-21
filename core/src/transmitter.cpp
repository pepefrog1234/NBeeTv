#include "transmitter.hpp"

#include <cassert>

#include "analog_frame.hpp"
#include "digital_frame.hpp"
#include "ofdm_tx.hpp"

namespace nbtv {

float Transmitter::papr_for(const Mode& mode) const {
    if (papr_db_ == 0.0f) return default_papr_db(mode);
    return papr_db_ < 0.0f ? 0.0f : papr_db_;
}

std::vector<float> Transmitter::digital_frame(const Mode& mode, std::span<const std::uint8_t> image_data, int image_format) {
    assert(mode.is_digital());
    DigitalHeader header;
    header.image_format = image_format;
    header.sequence = sequence_;
    header.callsign = callsign_;
    sequence_ = (sequence_ + 1) & 0xFF;
    return ofdm_modulate(mode, digital_encode(mode, header, image_data), papr_for(mode));
}

std::vector<float> Transmitter::analog_frame(const Mode& mode, std::span<const std::uint8_t> rgb, std::vector<std::uint8_t>* preview) {
    assert(!mode.is_digital());
    AnalogHeader header;
    header.sequence = sequence_;
    header.callsign = callsign_;
    sequence_ = (sequence_ + 1) & 0xFF;
    AnalogEncoded enc = analog_encode(mode, header, rgb);
    if (preview != nullptr) *preview = std::move(enc.preview);
    return ofdm_modulate(mode, enc.cells, papr_for(mode));
}

}  // namespace nbtv
