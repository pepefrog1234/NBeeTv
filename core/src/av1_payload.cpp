#include "av1_payload.hpp"

#include <cstring>
#include <string_view>

#include "types.hpp"

namespace nbtv {

namespace {

constexpr int kObuSequenceHeader = 1;
constexpr int kObuTemporalDelimiter = 2;

// 讀一個 OBU 的標頭；成功時回傳整個 OBU 的長度（含標頭與長度欄位），否則回傳 0。
std::size_t obu_size(std::span<const std::uint8_t> data, int* type, std::size_t* payload_offset) {
    if (data.empty()) return 0;
    const std::uint8_t header = data[0];
    const bool forbidden = (header & 0x80) != 0;
    const int obu_type = (header >> 3) & 0xF;
    const bool extension = (header & 0x04) != 0;
    const bool has_size = (header & 0x02) != 0;
    // 0 是保留的型別，補零的位元組會落在這裡；沒有長度欄位的 OBU 無法定界，也不接受
    if (forbidden || obu_type == 0 || !has_size) return 0;
    std::size_t pos = 1 + (extension ? 1u : 0u);
    std::uint64_t size = 0;
    for (int i = 0; i < 8; ++i) {  // LEB128
        if (pos >= data.size()) return 0;
        const std::uint8_t b = data[pos++];
        size |= static_cast<std::uint64_t>(b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) break;
        if (i == 7) return 0;
    }
    if (size > data.size() - pos) return 0;
    if (type != nullptr) *type = obu_type;
    if (payload_offset != nullptr) *payload_offset = pos;
    return pos + static_cast<std::size_t>(size);
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}
    bool ok() const { return !overrun_; }
    unsigned read(int bits) {
        unsigned v = 0;
        for (int i = 0; i < bits; ++i) {
            if (pos_ >= data_.size() * 8) {
                overrun_ = true;
                return 0;
            }
            v = (v << 1) | ((data_[pos_ / 8] >> (7 - pos_ % 8)) & 1u);
            ++pos_;
        }
        return v;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool overrun_ = false;
};

// ISO BMFF 的箱子：寫的時候先留 4 位元組的長度，結束時回填。
class BoxWriter {
public:
    std::vector<std::uint8_t> bytes;

    std::size_t begin(std::string_view type) {
        const std::size_t start = bytes.size();
        u32(0);
        for (char c : type) bytes.push_back(static_cast<std::uint8_t>(c));
        return start;
    }
    std::size_t begin_full(std::string_view type, std::uint8_t version, std::uint32_t flags) {
        const std::size_t start = begin(type);
        u32((static_cast<std::uint32_t>(version) << 24) | flags);
        return start;
    }
    void end(std::size_t start) { patch_u32(start, static_cast<std::uint32_t>(bytes.size() - start)); }
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) {
        bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        bytes.push_back(static_cast<std::uint8_t>(v));
    }
    void u32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) bytes.push_back(static_cast<std::uint8_t>(v >> s));
    }
    void text(std::string_view s) {
        for (char c : s) bytes.push_back(static_cast<std::uint8_t>(c));
    }
    void patch_u32(std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
    }
};

std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t at) {
    return (static_cast<std::uint32_t>(d[at]) << 24) | (static_cast<std::uint32_t>(d[at + 1]) << 16) |
           (static_cast<std::uint32_t>(d[at + 2]) << 8) | d[at + 3];
}

}  // namespace

std::size_t av1_payload_length(std::span<const std::uint8_t> payload) {
    std::size_t pos = 0;
    bool first = true;
    while (pos < payload.size()) {
        int type = 0;
        const std::size_t n = obu_size(payload.subspan(pos), &type, nullptr);
        if (n == 0) break;
        if (first && type != kObuSequenceHeader) return 0;
        first = false;
        pos += n;
    }
    // 至少要有序列標頭與一個畫面
    return first ? 0 : pos;
}

bool av1_parse_sequence_header(std::span<const std::uint8_t> payload, Av1SequenceInfo& info) {
    int type = 0;
    std::size_t offset = 0;
    const std::size_t n = obu_size(payload, &type, &offset);
    if (n == 0 || type != kObuSequenceHeader) return false;
    BitReader br(payload.subspan(offset, n - offset));
    info.profile = static_cast<int>(br.read(3));
    const bool still_picture = br.read(1) != 0;
    const bool reduced = br.read(1) != 0;
    // 空中格式只收靜態畫面的精簡標頭（libaom 的全幀內編碼就是這種）
    if (!still_picture || !reduced) return false;
    info.level = static_cast<int>(br.read(5));
    const int width_bits = static_cast<int>(br.read(4)) + 1;
    const int height_bits = static_cast<int>(br.read(4)) + 1;
    info.width = static_cast<int>(br.read(width_bits)) + 1;
    info.height = static_cast<int>(br.read(height_bits)) + 1;
    br.read(1);  // use_128x128_superblock
    br.read(1);  // enable_filter_intra
    br.read(1);  // enable_intra_edge_filter
    br.read(1);  // enable_superres
    br.read(1);  // enable_cdef
    br.read(1);  // enable_restoration
    info.high_bitdepth = br.read(1) != 0;
    return br.ok();
}

std::vector<std::uint8_t> avif_from_payload(std::span<const std::uint8_t> payload) {
    const std::size_t length = av1_payload_length(payload);
    Av1SequenceInfo seq;
    if (length == 0 || !av1_parse_sequence_header(payload, seq)) return {};
    // 畫面固定 96×72、4:4:4（High profile）、8 位元
    if (seq.width != kImageWidth || seq.height != kImageHeight || seq.profile != 1 || seq.high_bitdepth) return {};

    const std::uint32_t data_bytes = static_cast<std::uint32_t>(length + kAvifTemporalDelimiterBytes);
    BoxWriter w;
    const std::size_t ftyp = w.begin("ftyp");
    w.text("avif");
    w.u32(0);
    w.text("avifmif1miafMA1A");
    w.end(ftyp);

    const std::size_t meta = w.begin_full("meta", 0, 0);
    {
        const std::size_t hdlr = w.begin_full("hdlr", 0, 0);
        w.u32(0);
        w.text("pict");
        for (int i = 0; i < 12; ++i) w.u8(0);
        w.u8(0);  // 空的名稱
        w.end(hdlr);

        const std::size_t pitm = w.begin_full("pitm", 0, 0);
        w.u16(1);
        w.end(pitm);

        const std::size_t iloc = w.begin_full("iloc", 0, 0);
        w.u8(0x44);  // 位移與長度各 4 位元組
        w.u8(0x00);
        w.u16(1);    // 一個項目
        w.u16(1);    // 項目編號
        w.u16(0);    // data_reference_index
        w.u16(1);    // 一段
        w.u32(static_cast<std::uint32_t>(kAvifShellBytes));  // 資料在檔案裡的位移：緊接在外殼之後
        w.u32(data_bytes);
        w.end(iloc);

        const std::size_t iinf = w.begin_full("iinf", 0, 0);
        w.u16(1);
        const std::size_t infe = w.begin_full("infe", 2, 0);
        w.u16(1);
        w.u16(0);
        w.text("av01");
        w.text("Color");
        w.u8(0);
        w.end(infe);
        w.end(iinf);

        const std::size_t iprp = w.begin("iprp");
        const std::size_t ipco = w.begin("ipco");
        const std::size_t ispe = w.begin_full("ispe", 0, 0);
        w.u32(kImageWidth);
        w.u32(kImageHeight);
        w.end(ispe);
        const std::size_t pixi = w.begin_full("pixi", 0, 0);
        w.u8(3);
        w.u8(8);
        w.u8(8);
        w.u8(8);
        w.end(pixi);
        const std::size_t av1c = w.begin("av1C");
        w.u8(0x81);
        w.u8(static_cast<std::uint8_t>((seq.profile << 5) | (seq.level & 0x1F)));
        w.u8(0x00);  // tier 0、8 位元、彩色、4:4:4
        w.u8(0x00);
        w.end(av1c);
        const std::size_t colr = w.begin("colr");
        w.text("nclx");
        w.u16(1);    // 原色 BT.709
        w.u16(13);   // 轉換特性 sRGB
        w.u16(6);    // 矩陣 BT.601
        w.u8(0x80);  // 全範圍
        w.end(colr);
        w.end(ipco);
        const std::size_t ipma = w.begin_full("ipma", 0, 0);
        w.u32(1);
        w.u16(1);
        w.u8(4);
        w.u8(0x01);
        w.u8(0x02);
        w.u8(0x83);  // av1C 是必要屬性
        w.u8(0x04);
        w.end(ipma);
        w.end(iprp);
    }
    w.end(meta);

    const std::size_t mdat = w.begin("mdat");
    if (w.bytes.size() != kAvifShellBytes) return {};
    w.u8(0x12);  // 時序分隔 OBU
    w.u8(0x00);
    w.bytes.insert(w.bytes.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(length));
    w.end(mdat);
    return w.bytes;
}

std::vector<std::uint8_t> payload_from_avif(std::span<const std::uint8_t> file) {
    // 走過最上層的箱子找 mdat
    std::size_t pos = 0;
    while (pos + 8 <= file.size()) {
        const std::uint32_t size = read_u32(file, pos);
        if (size < 8 || size > file.size() - pos) return {};
        if (std::memcmp(&file[pos + 4], "mdat", 4) == 0) {
            std::span<const std::uint8_t> data = file.subspan(pos + 8, size - 8);
            // 去掉開頭的時序分隔
            int type = 0;
            const std::size_t n = obu_size(data, &type, nullptr);
            if (n != 0 && type == kObuTemporalDelimiter) data = data.subspan(n);
            if (av1_payload_length(data) != data.size()) return {};
            return std::vector<std::uint8_t>(data.begin(), data.end());
        }
        pos += size;
    }
    return {};
}

}  // namespace nbtv
