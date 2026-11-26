#include "transmitter.hpp"

#include "frame_header.hpp"
#include "learned_frame.hpp"
#include "ofdm_tx.hpp"

namespace nbtv {

std::vector<float> Transmitter::learned_frame(const Mode& mode, std::span<const std::uint8_t> rgb, std::vector<std::uint8_t>* preview,
                                              std::vector<float>* values) {
    const LearnedCodec* model = models_.find(mode);
    if (model == nullptr) return {};
    FrameHeader header;
    header.sequence = sequence_;
    header.callsign = callsign_;
    std::vector<float> local;
    std::vector<float>* kept = values != nullptr ? values : preview != nullptr ? &local : nullptr;
    const std::vector<cf> cells = learned_encode(mode, *model, header, rgb, kept);
    if (cells.empty()) {
        if (values != nullptr) values->clear();
        return {};
    }
    sequence_ = (sequence_ + 1) & 0xFF;
    if (preview != nullptr) {
        preview->resize(static_cast<std::size_t>(model->rgb_bytes()));
        model->decode(*kept, kLearnedMaxSnrDb, *preview);
    }
    const float papr_db = papr_db_ == 0.0f ? kDefaultPaprDb : papr_db_ < 0.0f ? 0.0f : papr_db_;
    return ofdm_modulate(mode, cells, papr_db);
}

}  // namespace nbtv
