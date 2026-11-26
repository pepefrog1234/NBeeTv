#include "demo.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <thread>

#include "png.hpp"
#include "process.hpp"
#include "sources.hpp"
#include "tasks.hpp"

namespace nbtvgui::demo {

// 建置時由 apps/desktop/CMakeLists.txt 產生（demo_images.cpp）：core/tests/data 裡的三張 PPM 原封不動編進來
struct EmbeddedImage {
    const char* name;
    const unsigned char* data;
    unsigned long size;
};
extern const EmbeddedImage kEmbeddedImages[];
extern const int kEmbeddedImageCount;

namespace {

constexpr double kPi = 3.14159265358979323846;

/// 二進位 PPM（P6、96×72、最大值 255）→ RGB。
std::optional<Picture> parse_ppm(const unsigned char* data, std::size_t size) {
    std::vector<std::string> fields;
    std::size_t index = 0;
    std::string token;
    // 表頭有四個以空白分隔的欄位：P6、寬、高、最大值；之後隔一個空白字元就是像素
    while (index < size && fields.size() < 4) {
        const unsigned char c = data[index++];
        if (c == '#') {
            while (index < size && data[index] != '\n') ++index;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!token.empty()) {
                fields.push_back(token);
                token.clear();
            }
        } else {
            token.push_back(static_cast<char>(c));
        }
    }
    if (fields.size() != 4 || fields[0] != "P6" || fields[1] != std::to_string(kImageWidth) || fields[2] != std::to_string(kImageHeight) || fields[3] != "255") return std::nullopt;
    if (size - index < static_cast<std::size_t>(kRgbBytes)) return std::nullopt;
    Picture picture;
    std::memcpy(picture.rgb.data(), data + index, kRgbBytes);
    return picture;
}

/// 原地做的快速傅立葉轉換（長度是 2 的次方；不做正規化）。
void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2 * kPi / static_cast<double>(length) * (inverse ? 1 : -1);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        const std::size_t half = length / 2;
        // 這一層用到的旋轉因子先算好一份（每一段都一樣）
        std::vector<std::complex<float>> twiddle(half);
        std::complex<double> w(1.0, 0.0);
        for (std::size_t j = 0; j < half; ++j) {
            twiddle[j] = std::complex<float>(static_cast<float>(w.real()), static_cast<float>(w.imag()));
            w *= step;
        }
        for (std::size_t i = 0; i < n; i += length) {
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<float> u = a[i + j], v = a[i + j + half] * twiddle[j];
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
        }
    }
}

}  // namespace

std::optional<Picture> test_image(const std::string& name) {
    for (int i = 0; i < kEmbeddedImageCount; ++i) {
        if (name == kEmbeddedImages[i].name) return parse_ppm(kEmbeddedImages[i].data, kEmbeddedImages[i].size);
    }
    return std::nullopt;
}

std::uint64_t Random::next() {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double Random::uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }

double Random::gauss() {
    const double u1 = std::max(uniform(), 1e-300), u2 = uniform();
    return std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2);
}

double power(const std::vector<float>& samples) {
    if (samples.empty()) return 0;
    double sum = 0;
    for (const float v : samples) sum += static_cast<double>(v) * v;
    return sum / static_cast<double>(samples.size());
}

std::vector<float> silence(double seconds) { return std::vector<float>(static_cast<std::size_t>(seconds * kSampleRate), 0.0f); }

std::vector<float> radio_channel(const std::vector<float>& input, double signal_power, std::optional<double> snr_db, double shift_hz, double noise_only_level_db,
                                 std::optional<double> output_level_db, std::uint64_t seed) {
    const std::size_t count = input.size();
    if (count == 0) return {};
    std::size_t size = 2;
    while (size < count) size <<= 1;
    std::vector<std::complex<float>> data(size);

    // 白雜訊：48 kHz 取樣下功率 σ² 分布在 0–24 kHz，3 kHz 內的雜訊功率是 σ²/8
    Random rng(seed);
    double sigma = 0;
    if (signal_power > 0 && snr_db) {
        sigma = std::sqrt(8 * signal_power / std::pow(10.0, *snr_db / 10));
    } else if (signal_power <= 0) {
        // 只有雜訊：通帶寬約 2.5 kHz，佔 24 kHz 的 2.5/24
        sigma = std::pow(10.0, noise_only_level_db / 20) / std::sqrt(2.5 / 24.0);
    }
    for (std::size_t i = 0; i < count; ++i) data[i] = input[i] + (sigma > 0 ? static_cast<float>(sigma * rng.gauss()) : 0.0f);
    fft(data, false);

    // 正頻率的每一格往上搬 shift 格（等於整個頻譜平移），再乘上通帶的形狀；負頻率取共軛對稱
    const double bin_hz = static_cast<double>(kSampleRate) / static_cast<double>(size);
    const long shift = std::lround(shift_hz / bin_hz);
    const std::size_t half = size / 2;
    std::vector<std::complex<float>> shaped(half + 1);
    for (std::size_t k = 1; k < half; ++k) {
        const long source = static_cast<long>(k) - shift;
        if (source < 1 || source >= static_cast<long>(half)) continue;
        const double hz = static_cast<double>(k) * bin_hz;
        // 通帶 250–2750 Hz，兩邊各 100 Hz 的餘弦過渡帶；通帶外留一點點（−50 dB），像真的濾波器
        double gain = 0.003;
        if (hz >= 300 && hz <= 2700) gain = 1;
        else if (hz > 200 && hz < 300) gain = std::max(gain, 0.5 - 0.5 * std::cos(kPi * (hz - 200) / 100));
        else if (hz > 2700 && hz < 2800) gain = std::max(gain, 0.5 + 0.5 * std::cos(kPi * (hz - 2700) / 100));
        shaped[k] = data[static_cast<std::size_t>(source)] * static_cast<float>(gain);
    }
    for (std::size_t k = 0; k < size; ++k) {
        if (k == 0 || k == half) data[k] = 0;
        else if (k < half) data[k] = shaped[k];
        else data[k] = std::conj(shaped[size - k]);
    }
    fft(data, true);

    double gain = 1.0 / static_cast<double>(size);
    if (output_level_db && signal_power > 0) {
        const double total = signal_power + (snr_db ? signal_power / std::pow(10.0, *snr_db / 10) * 2.5 / 3.0 : 0.0);
        gain *= std::pow(10.0, *output_level_db / 20) / std::sqrt(total);
    }
    std::vector<float> output(count);
    for (std::size_t i = 0; i < count; ++i) output[i] = static_cast<float>(data[i].real() * gain);
    return output;
}

std::vector<RxUpdate> receive(const std::vector<float>& audio) {
    CoreReceiver receiver;
    receiver.set_merging(true);   // 和 app 接收時一樣
    CoreSpectrum spectrum;
    std::vector<RxUpdate> updates;
    const std::size_t chunk = kSampleRate / 50;
    for (std::size_t position = 0; position < audio.size(); position += chunk) {
        const int count = static_cast<int>(std::min(chunk, audio.size() - position));
        receiver.push(audio.data() + position, count);
        spectrum.push(audio.data() + position, count);
        RxUpdate update;
        const nbtv_rx_status status = receiver.status();
        update.synced = status.synced != 0;
        update.mode_id = status.mode_id;
        update.snr_valid = status.snr_valid != 0;
        update.snr_db = status.snr_db;
        update.offset_hz = status.freq_offset_hz;
        update.input_level_db = status.input_level_db;
        while (std::optional<DecodedFrame> frame = receiver.poll_frame()) update.frames.push_back(std::move(*frame));
        std::array<float, kSpectrumBins> row{};
        while (spectrum.poll(row)) update.spectrum_rows.push_back(row);
        updates.push_back(std::move(update));
    }
    return updates;
}

std::vector<std::array<float, kSpectrumBins>> monitor_rows(const std::vector<float>& audio) {
    CoreSpectrum spectrum;
    std::vector<std::array<float, kSpectrumBins>> rows;
    const std::size_t chunk = kSampleRate / 20;
    for (std::size_t position = 0; position < audio.size(); position += chunk) {
        spectrum.push(audio.data() + position, static_cast<int>(std::min(chunk, audio.size() - position)));
        std::array<float, kSpectrumBins> row{};
        while (spectrum.poll(row)) rows.push_back(row);
    }
    return rows;
}

bool write_picture_png(const Picture& picture, const std::string& path, int zoom) {
    const int width = kImageWidth * zoom, height = kImageHeight * zoom;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* source = picture.rgb.data() + (static_cast<std::size_t>(y / zoom) * kImageWidth + static_cast<std::size_t>(x / zoom)) * 3;
            std::memcpy(pixels.data() + (static_cast<std::size_t>(y) * width + x) * 3, source, 3);
        }
    }
    return write_png(path, pixels.data(), width, height, 3);
}

bool write_picture_jpeg(const Picture& picture, const std::string& path, int zoom, int orientation, bool little_endian, bool after_jfif) {
    const int width = kImageWidth * zoom, height = kImageHeight * zoom;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* source = picture.rgb.data() + (static_cast<std::size_t>(y / zoom) * kImageWidth + static_cast<std::size_t>(x / zoom)) * 3;
            std::memcpy(pixels.data() + (static_cast<std::size_t>(y) * width + x) * 3, source, 3);
        }
    }
    std::vector<std::uint8_t> bytes = encode_jpeg(pixels.data(), width, height, 95);
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) return false;
    if (orientation >= 1 && orientation <= 8) {
        // EXIF 那一段（APP1）：「Exif」、TIFF 的檔頭、只有一項的目錄——0x0112（方向），16 位元的整數一個
        const std::uint8_t value = static_cast<std::uint8_t>(orientation);
        std::vector<std::uint8_t> exif = {0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0};
        const std::vector<std::uint8_t> tiff = little_endian
            ? std::vector<std::uint8_t>{'I', 'I', 0x2A, 0x00, 0x08, 0, 0, 0, 0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0, 0, 0, value, 0, 0, 0, 0, 0, 0, 0}
            : std::vector<std::uint8_t>{'M', 'M', 0x00, 0x2A, 0, 0, 0, 0x08, 0x00, 0x01, 0x01, 0x12, 0x00, 0x03, 0, 0, 0, 0x01, 0, value, 0, 0, 0, 0, 0, 0};
        exif.insert(exif.end(), tiff.begin(), tiff.end());
        // 放在最前面（緊接在檔頭的兩個位元組後面）；after_jfif 時放在第一段（stb 寫的 JFIF 那一段）後面
        std::size_t at = 2;
        if (after_jfif && bytes.size() > 6 && bytes[2] == 0xFF && bytes[3] == 0xE0) at = 4 + ((static_cast<std::size_t>(bytes[4]) << 8) | bytes[5]);
        if (at > bytes.size()) return false;
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), exif.begin(), exif.end());
    }
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return ok;
}

bool make_video(const std::string& path, const Picture& picture, int seconds, int fps, int width, int height) {
    const std::string ffmpeg = FileSource::ffmpeg_path();
    if (ffmpeg.empty()) return false;
    const std::string still = path + ".still.png";
    if (!write_picture_png(picture, still, 10)) return false;
    // 4:3 的圖放大到蓋滿寬度，多出來的高度用來上下移動（每一格的位置都不一樣）
    const int scaled_height = width * 3 / 4;
    const std::string filter = "scale=" + std::to_string(width) + ":" + std::to_string(scaled_height) + ":flags=neighbor,crop=" + std::to_string(width) + ":" +
                               std::to_string(height) + ":0:(ih-oh)*t/" + std::to_string(seconds);
    Process::Options options;
    options.args = {ffmpeg, "-y", "-nostdin", "-hide_banner", "-loglevel", "error", "-loop", "1", "-framerate", std::to_string(fps), "-i", still,
                    "-vf", filter, "-t", std::to_string(seconds), "-r", std::to_string(fps), "-pix_fmt", "yuv420p", path};
    options.pipe_stderr = true;
    std::string error;
    std::unique_ptr<Process> process = Process::spawn(options, &error);
    int code = 1;
    bool ok = process != nullptr && process->wait(180.0, &code) && code == 0;
    SDL_RemovePath(still.c_str());
    // 結束碼在 Windows 上拿不到（見 process.hpp）：另外確認檔案真的寫出來了
    SDL_PathInfo info{};
    ok = ok && SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE && info.size > 0;
    return ok;
}

bool wait_until(App* app, double timeout, const std::function<bool()>& condition) {
    const double deadline = uptime_seconds() + timeout;
    for (;;) {
        MainQueue::shared().drain();
        if (app != nullptr) app->tick();
        if (condition()) return true;
        if (uptime_seconds() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void spin(App* app, double seconds) {
    const double deadline = uptime_seconds() + seconds;
    wait_until(app, seconds + 1, [deadline] { return uptime_seconds() >= deadline; });
}

double psnr(const Picture& a, const Picture& b) {
    double sum = 0;
    for (int i = 0; i < kRgbBytes; ++i) {
        const double d = static_cast<double>(a.rgb[static_cast<std::size_t>(i)]) - static_cast<double>(b.rgb[static_cast<std::size_t>(i)]);
        sum += d * d;
    }
    if (sum == 0) return 99;
    return 10 * std::log10(255.0 * 255.0 / (sum / kRgbBytes));
}

}  // namespace nbtvgui::demo
