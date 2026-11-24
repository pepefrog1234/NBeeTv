// 學習式影像編碼的推論：編碼器、解碼器兩個卷積網路的前向計算。權重檔的解析與卷積這些零件在 learned_nn.hpp。
// 結構和 tools/learned/model.py 的 JSCC 一一對應；改了那邊，這裡和權重檔的結構編號都要跟著改。
#include "learned_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

#include "bits.hpp"
#include "learned_nn.hpp"
#include "types.hpp"

namespace nbtv {

using namespace nn;

namespace {

constexpr char kMagic[8] = {'N', 'B', 'T', 'V', 'L', 'R', 'N', '1'};
constexpr std::uint32_t kFormatVersion = 1;
// 結構編號：1 的解碼器只吃訊雜比；2 的另外吃各個值的可靠程度（輸入的通道數加倍），訓練時涵蓋的訊雜比也比較低
constexpr std::uint32_t kMaxArchitecture = 2;

// 以下和 model.py 相同。各層的寬度（解析度每減半一次的通道數，共 kStages + 1 個）不是固定的，由權重檔看出來
constexpr int kStages = 3;
constexpr int kBlocksPerStage = 2;
constexpr int kSnrFeatures = 64;

}  // namespace

// ---------------------------------------------------------------- 模型

struct LearnedCodec::Impl {
    std::uint32_t id = 0;
    int latent = 0;
    int architecture = 1;
    int widths[kStages + 1] = {};  // 解析度每減半一次的通道數

    // 編碼器：96×72×3 →（每一級解析度減半）→ 12×9×latent
    Conv enc_stem;
    Conv enc_down[kStages];
    Block enc_block[kStages][kBlocksPerStage];
    Conv enc_head;

    // 解碼器：12×9×latent 與訊雜比 →（每一級解析度加倍）→ 96×72×3
    Linear snr1, snr2;
    Conv dec_inp;
    Block dec_block[kStages][kBlocksPerStage];
    Linear dec_film[kStages][kBlocksPerStage];
    Conv dec_up[kStages];
    Block dec_tail[kBlocksPerStage];
    Conv dec_out;
};

namespace {

// 從解析好的張量組出模型：逐一核對名稱與形狀（Builder 在 learned_nn.hpp）
bool build(std::span<const std::uint8_t> file, LearnedCodec::Impl& m, std::string& error) {
    std::map<std::string, RawTensor> tensors;
    FileHeader header;
    const auto accept = [](const FileHeader& h, std::string& why) {
        if (h.version != kFormatVersion || h.structure == 0 || h.structure > kMaxArchitecture) {
            why = "權重檔的格式版本或模型結構不是這個版本認得的";
            return false;
        }
        return true;
    };
    if (!parse_file(file, kMagic, "不是學習式編碼的權重檔", accept, tensors, header, m.id, error)) return false;
    m.latent = static_cast<int>(header.latent);
    m.architecture = static_cast<int>(header.structure);

    // 各層的寬度不另外記在檔案裡：就是編碼器開頭的卷積，以及三個把解析度減半的卷積的輸出通道數。
    // 其餘的張量下面逐一核對，都要和這四個數對得上。
    for (int i = 0; i <= kStages; ++i) {
        const std::string name = i == 0 ? "enc.stem.weight" : "enc.stages." + std::to_string((i - 1) * (1 + kBlocksPerStage)) + ".weight";
        const auto it = tensors.find(name);
        if (it == tensors.end() || it->second.dims[0] > kMaxWidth) {
            error = "權重檔缺少張量或形狀不符：" + name;
            return false;
        }
        m.widths[i] = static_cast<int>(it->second.dims[0]);
    }

    Builder b{tensors, error};
    bool ok = b.conv("enc.stem", m.widths[0], 3, 5, 1, 2, m.enc_stem);
    for (int s = 0; s < kStages; ++s) {
        // model.py 的 stages 是 [縮小的卷積、殘差塊、殘差塊] 重複三次
        const int first = s * (1 + kBlocksPerStage);
        ok = ok && b.conv("enc.stages." + std::to_string(first), m.widths[s + 1], m.widths[s], 3, 2, 1, m.enc_down[s]);
        for (int j = 0; j < kBlocksPerStage; ++j)
            ok = ok && b.block("enc.stages." + std::to_string(first + 1 + j), m.widths[s + 1], m.enc_block[s][j]);
    }
    ok = ok && b.conv("enc.head", m.latent, m.widths[3], 1, 1, 0, m.enc_head);

    ok = ok && b.linear("dec.snr_embed.0", kSnrFeatures, 1, m.snr1) && b.linear("dec.snr_embed.2", kSnrFeatures, kSnrFeatures, m.snr2);
    // 結構 2：輸入是「收到的值」接上「可靠程度」，通道數加倍
    ok = ok && b.conv("dec.inp", m.widths[3], m.latent * (m.architecture >= 2 ? 2 : 1), 1, 1, 0, m.dec_inp);
    for (int s = 0; s < kStages; ++s) {
        const int channels = m.widths[3 - s], next = m.widths[2 - s];
        const std::string stage = std::to_string(s);
        for (int j = 0; j < kBlocksPerStage; ++j) {
            const std::string which = stage + "." + std::to_string(j);
            ok = ok && b.block("dec.blocks." + which, channels, m.dec_block[s][j]);
            ok = ok && b.linear("dec.films." + which, 2 * channels, kSnrFeatures, m.dec_film[s][j]);
        }
        ok = ok && b.conv("dec.ups." + stage + ".0", next * 4, channels, 3, 1, 1, m.dec_up[s]);
    }
    for (int j = 0; j < kBlocksPerStage; ++j) ok = ok && b.block("dec.tail." + std::to_string(j), m.widths[0], m.dec_tail[j]);
    ok = ok && b.conv("dec.out", 3, m.widths[0], 5, 1, 2, m.dec_out);
    if (!ok) return false;
    if (b.used != tensors.size()) {
        error = "權重檔裡有這個模型用不到的張量";
        return false;
    }
    return true;
}

}  // namespace

LearnedCodec::LearnedCodec(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LearnedCodec::~LearnedCodec() = default;

std::unique_ptr<LearnedCodec> LearnedCodec::load(std::span<const std::uint8_t> file, std::string* error) {
    auto impl = std::make_unique<Impl>();
    std::string why;
    if (!build(file, *impl, why)) {
        if (error != nullptr) *error = why;
        return nullptr;
    }
    return std::unique_ptr<LearnedCodec>(new LearnedCodec(std::move(impl)));
}

std::uint32_t LearnedCodec::model_id() const { return impl_->id; }

int LearnedCodec::value_count() const { return impl_->latent * kGridH * kGridW; }

std::array<int, 4> LearnedCodec::widths() const {
    static_assert(kStages + 1 == 4);
    return {impl_->widths[0], impl_->widths[1], impl_->widths[2], impl_->widths[3]};
}

int LearnedCodec::architecture() const { return impl_->architecture; }

float LearnedCodec::min_snr_db() const { return impl_->architecture >= 2 ? -6.0f : -2.0f; }

bool LearnedCodec::encode(std::span<const std::uint8_t> rgb, std::span<float> values) const {
    const Impl& m = *impl_;
    if (rgb.size() != static_cast<std::size_t>(kImageRgbBytes) || values.size() != static_cast<std::size_t>(value_count())) return false;

    Map x, a, t1, t2;
    x.shape(kImageHeight, kImageWidth, 3);
    for (std::size_t i = 0; i < rgb.size(); ++i) x.v[i] = static_cast<float>(rgb[i]) / 255.0f;

    conv2d(m.enc_stem, x, a);
    for (int s = 0; s < kStages; ++s) {
        conv2d(m.enc_down[s], a, t1);
        std::swap(a, t1);
        for (int j = 0; j < kBlocksPerStage; ++j) res_block(m.enc_block[s][j], a, nullptr, nullptr, t1, t2);
    }
    gelu_inplace(a.v);
    Map z;
    conv2d(m.enc_head, a, z);

    // 功率正規化：每個實數的均方值 1/2
    double power = 0.0;
    for (float v : z.v) power += static_cast<double>(v) * static_cast<double>(v);
    power /= static_cast<double>(z.v.size());
    const float scale = static_cast<float>(std::sqrt(0.5 / (power + 1e-9)));

    // 輸出的順序是 [通道][列][行]，和 PyTorch 把張量攤平的順序相同
    std::size_t n = 0;
    for (int c = 0; c < m.latent; ++c)
        for (int y = 0; y < kGridH; ++y)
            for (int col = 0; col < kGridW; ++col) values[n++] = z.at(y, col)[c] * scale;
    return true;
}

bool LearnedCodec::decode(std::span<const float> values, std::span<const float> reliability, float snr_db,
                          std::span<std::uint8_t> rgb) const {
    const Impl& m = *impl_;
    if (values.size() != static_cast<std::size_t>(value_count()) || rgb.size() != static_cast<std::size_t>(kImageRgbBytes)) return false;
    if (!reliability.empty() && reliability.size() != values.size()) return false;

    // 訊雜比 → 一組特徵，用來調整每個殘差塊
    const float snr = std::isnan(snr_db) ? 10.0f : std::clamp(snr_db, min_snr_db(), kLearnedMaxSnrDb);
    const float scaled = (snr - 12.0f) / 12.0f;
    std::vector<float> e = apply(m.snr1, &scaled);
    gelu_inplace(e);
    e = apply(m.snr2, e.data());
    gelu_inplace(e);

    // 輸入：收到的值；結構 2 後面再接上各個值的可靠程度（順序和 PyTorch 的 cat([值, 可靠程度]) 相同）
    const bool with_reliability = m.architecture >= 2;
    Map z, a, t1, t2;
    z.shape(kGridH, kGridW, m.latent * (with_reliability ? 2 : 1));
    std::size_t n = 0;
    for (int c = 0; c < m.latent; ++c)
        for (int y = 0; y < kGridH; ++y)
            for (int col = 0; col < kGridW; ++col, ++n) {
                const float v = values[n];
                z.at(y, col)[c] = std::isfinite(v) ? v : 0.0f;
                if (with_reliability) {
                    const float w = reliability.empty() ? 1.0f : reliability[n];
                    z.at(y, col)[m.latent + c] = w >= 0.0f && w <= 1.0f ? w : (w > 1.0f ? 1.0f : 0.0f);  // 非數值當成 0
                }
            }

    conv2d(m.dec_inp, z, a);
    for (int s = 0; s < kStages; ++s) {
        for (int j = 0; j < kBlocksPerStage; ++j) {
            const std::vector<float> film = apply(m.dec_film[s][j], e.data());  // 前半是縮放、後半是位移
            res_block(m.dec_block[s][j], a, film.data(), film.data() + a.c, t1, t2);
        }
        gelu_inplace(a.v);
        conv2d(m.dec_up[s], a, t1);
        pixel_shuffle2(t1, a);
    }
    for (int j = 0; j < kBlocksPerStage; ++j) res_block(m.dec_tail[j], a, nullptr, nullptr, t1, t2);
    gelu_inplace(a.v);
    conv2d(m.dec_out, a, t1);

    for (std::size_t i = 0; i < rgb.size(); ++i) {
        float v = t1.v[i];
        v = !(v > 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);  // 非數值也歸零
        rgb[i] = static_cast<std::uint8_t>(std::nearbyint(v * 255.0f));
    }
    return true;
}

}  // namespace nbtv
