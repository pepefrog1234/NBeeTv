// 位元工具、CRC、導頻 PRBS、呼號編碼與 FFT。
#include <cmath>
#include <string>
#include <vector>

#include "bits.hpp"
#include "callsign.hpp"
#include "check.hpp"
#include "fft.hpp"

using namespace nbtv;

namespace {

void test_bit_packing() {
    const std::vector<std::uint8_t> bytes = {0x80, 0x01, 0xA5, 0x3C};
    std::vector<std::uint8_t> bits(bytes.size() * 8);
    bytes_to_bits(bytes, bits);
    CHECK_EQ(bits[0], 1);  // 高位元在前
    CHECK_EQ(bits[1], 0);
    CHECK_EQ(bits[15], 1);
    std::vector<std::uint8_t> back(bytes.size());
    bits_to_bytes(bits, back);
    CHECK(back == bytes);
}

void test_crc() {
    const std::string s = "123456789";
    const std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    CHECK_EQ(crc32(data), 0xCBF43926u);  // CRC-32 的標準檢查值
    CHECK_EQ(crc16(data), 0x29B1u);      // CRC-16/CCITT-FALSE 的標準檢查值
    CHECK_EQ(crc32({}), 0u);
}

void test_pilot_prbs() {
    const auto w = pilot_prbs();
    for (int k = 0; k < 11; ++k) CHECK_EQ(w[static_cast<std::size_t>(k)], -1);  // 初值全 1 → -1
    for (int k = 11; k < 20; ++k) CHECK_EQ(w[static_cast<std::size_t>(k)], 1);  // 接著九個 0
    CHECK_EQ(w[20], -1);
    CHECK_EQ(w[21], -1);
    int sum = 0;
    for (int k = 8; k <= 56; ++k) sum += w[static_cast<std::size_t>(k)];
    CHECK(std::abs(sum) < 20);  // 使用的 49 格裡正負大致平衡
}

void test_callsign() {
    CHECK(normalize_callsign("bx4acp") == "BX4ACP");
    CHECK(normalize_callsign("  vk2/bx4acp/p!! ") == "VK2/BX4AC");  // 截到 9 個字元
    CHECK(normalize_callsign("n0call-7") == "N0CALL-7");
    CHECK(normalize_callsign("呼號") == "");
    for (const char* call : {"BX4ACP", "N0CALL", "A", "", "VK2/BX4AC", "W1AW/QRP", "9A1A", "JA1ZZZ-15", "........."}) {
        const auto bytes = encode_callsign(call);
        const auto back = decode_callsign(bytes);
        CHECK(back.has_value());
        if (back) CHECK(*back == call);
    }
    // 空呼號編成全 0
    for (std::uint8_t b : encode_callsign("")) CHECK_EQ(b, 0);
    // 超出 40^9 的數值不是合法呼號
    CHECK(!decode_callsign({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}).has_value());
    CHECK(decode_callsign({0xEE, 0x6B, 0x27, 0xFF, 0xFF, 0xFF}).has_value());   // 40^9 − 1
    CHECK(!decode_callsign({0xEE, 0x6B, 0x28, 0x00, 0x00, 0x00}).has_value());  // 40^9
}

void test_fft() {
    for (int n : {8, 256, 1024}) {
        const Fft fft(n);
        // 單一頻率的複指數落在單一格
        std::vector<cf> x(static_cast<std::size_t>(n));
        const int bin = n / 8 + 1;
        for (int i = 0; i < n; ++i) {
            const double a = kTwoPi * bin * i / n;
            x[static_cast<std::size_t>(i)] = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
        }
        std::vector<cf> y = x;
        fft.forward(y.data());
        for (int k = 0; k < n; ++k) {
            const float expect = k == bin ? static_cast<float>(n) : 0.0f;
            CHECK_NEAR(std::abs(y[static_cast<std::size_t>(k)]), expect, 1e-3 * n);
        }
        // 正轉再反轉回到原訊號（乘上 N）
        fft.inverse(y.data());
        double err = 0;
        for (int i = 0; i < n; ++i) {
            err = std::max<double>(err, std::abs(y[static_cast<std::size_t>(i)] / static_cast<float>(n) - x[static_cast<std::size_t>(i)]));
        }
        CHECK_NEAR(err, 0.0, 1e-5);
    }
    // 與直接計算的 DFT 比對
    const int n = 16;
    const Fft fft(n);
    std::vector<cf> x(n);
    for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = cf(static_cast<float>((i * 7) % 5) - 2.0f, static_cast<float>((i * 3) % 4) - 1.5f);
    std::vector<cf> y = x;
    fft.forward(y.data());
    for (int k = 0; k < n; ++k) {
        cd acc = 0;
        for (int i = 0; i < n; ++i) acc += cd(x[static_cast<std::size_t>(i)]) * std::polar(1.0, -kTwoPi * k * i / n);
        CHECK_NEAR(std::abs(cd(y[static_cast<std::size_t>(k)]) - acc), 0.0, 1e-4);
    }
}

}  // namespace

int main() {
    test_bit_packing();
    test_crc();
    test_pilot_prbs();
    test_callsign();
    test_fft();
    return check::finish("bits");
}
