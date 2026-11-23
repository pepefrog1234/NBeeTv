#include "transmitter.hpp"

#include "frame_header.hpp"
#include "learned_frame.hpp"
#include "ofdm_tx.hpp"

namespace nbtv {

std::vector<float> Transmitter::learned_frame(const Mode& mode, std::span<const std::uint8_t> rgb, std::vector<std::uint8_t>* preview) {
    const LearnedCodec* model = models_.find(mode);
    if (model == nullptr) return {};
    FrameHeader header;
    header.sequence = sequence_;
    header.callsign = callsign_;
    std::vector<float> values;
    const std::vector<cf> cells = learned_encode(mode, *model, header, rgb, preview != nullptr ? &values : nullptr);
    if (cells.empty()) return {};
    sequence_ = (sequence_ + 1) & 0xFF;
    if (preview != nullptr) {
        preview->resize(static_cast<std::size_t>(kImageRgbBytes));
        model->decode(values, kLearnedMaxSnrDb, *preview);
    }
    const float papr_db = papr_db_ == 0.0f ? kDefaultPaprDb : papr_db_ < 0.0f ? 0.0f : papr_db_;
    return ofdm_modulate(mode, cells, papr_db);
}

}  // namespace nbtv
