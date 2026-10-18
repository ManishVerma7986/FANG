#include "canonical_encoder.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fang::studio::canonical {

namespace {

// Portable FIPS-180-2 SHA-256 implementation
namespace sha256 {
    inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    inline uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
    inline uint32_t majority(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }
    inline uint32_t sig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    inline uint32_t sig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    inline uint32_t theta0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    inline uint32_t theta1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    struct Context {
        uint32_t state[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint64_t count{0};
        uint8_t buffer[64]{0};

        void transform(const uint8_t data[64]) {
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
            uint32_t m[64];
            for (int i = 0; i < 16; ++i) {
                m[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
                       (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
                       (static_cast<uint32_t>(data[i * 4 + 3]));
            }
            for (int i = 16; i < 64; ++i) {
                m[i] = theta1(m[i - 2]) + m[i - 7] + theta0(m[i - 15]) + m[i - 16];
            }
            for (int i = 0; i < 64; ++i) {
                uint32_t t1 = h + sig1(e) + choose(e, f, g) + K[i] + m[i];
                uint32_t t2 = sig0(a) + majority(a, b, c);
                h = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
            state[4] += e; state[5] += f; state[6] += g; state[7] += h;
        }

        void update(const uint8_t* data, size_t len) {
            for (size_t i = 0; i < len; ++i) {
                buffer[count % 64] = data[i];
                count++;
                if (count % 64 == 0) {
                    transform(buffer);
                }
            }
        }

        std::string finalize() {
            uint64_t bit_count = count * 8;
            uint8_t pad = 0x80;
            update(&pad, 1);
            while (count % 64 != 56) {
                uint8_t zero = 0;
                update(&zero, 1);
            }
            uint8_t len_bytes[8];
            for (int i = 0; i < 8; ++i) {
                len_bytes[i] = static_cast<uint8_t>((bit_count >> ((7 - i) * 8)) & 0xFF);
            }
            update(len_bytes, 8);
            std::ostringstream oss;
            for (int i = 0; i < 8; ++i) {
                oss << std::hex << std::setw(8) << std::setfill('0') << state[i];
            }
            return oss.str();
        }
    };
} // namespace sha256

// Big-endian encoding helpers
inline void write_u32be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void write_u64be(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void write_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

inline void write_bytes(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
    out.insert(out.end(), data, data + len);
}

inline void write_string_utf8(std::vector<uint8_t>& out, const std::string& str) {
    write_u32be(out, static_cast<uint32_t>(str.size()));
    write_bytes(out, reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

inline void write_hex_digest(std::vector<uint8_t>& out, const std::string& hex) {
    // hex is 64-char lowercase hex string, convert to 32 raw bytes
    for (size_t i = 0; i < 32 && i * 2 < hex.size(); ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        out.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
}

} // anonymous namespace

std::string sha256_hex(const std::vector<uint8_t>& data) {
    sha256::Context ctx;
    ctx.update(data.data(), data.size());
    return ctx.finalize();
}

std::string sha256_file_hex(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) return {};
    sha256::Context ctx;
    uint8_t buffer[8192];
    while (file.read(reinterpret_cast<char*>(buffer), sizeof(buffer))) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    if (file.gcount() > 0) {
        ctx.update(buffer, static_cast<size_t>(file.gcount()));
    }
    return ctx.finalize();
}

std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        out.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
    }
    return out;
}

CanonicalResult build_canonical_stream(const CanonicalInput& input) {
    CanonicalResult result;

    // Validate required fields
    if (input.repo_root.empty()) {
        result.errors.push_back("repo_root is empty");
        return result;
    }
    if (input.source_revision.empty()) {
        result.errors.push_back("source_revision is empty");
        return result;
    }
    if (input.abi.empty()) {
        result.errors.push_back("abi is empty");
        return result;
    }
    if (input.configuration.empty()) {
        result.errors.push_back("configuration is empty");
        return result;
    }
    if (input.target_triple.empty()) {
        result.errors.push_back("target_triple is empty");
        return result;
    }
    if (input.ndk_version.empty()) {
        result.errors.push_back("ndk_version is empty");
        return result;
    }
    if (input.compile_args.empty()) {
        result.errors.push_back("compile_args is empty");
        return result;
    }

    // Sort entries by UTF-8 path bytes
    std::vector<CanonicalEntry> sorted_entries = input.entries;
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const CanonicalEntry& a, const CanonicalEntry& b) {
                  return a.path < b.path;
              });

    // Build canonical binary stream
    std::vector<uint8_t> canonical;

    // MAGIC: "FANGCIM" + NUL + format-version(1)
    canonical.insert(canonical.end(), {'F','A','N','G','C','I','M', 0x00, 0x01});

    // SCOPE: u32be length + UTF-8 bytes
    const std::string scope = "fang-guest-daemon-inputs-v1";
    write_u32be(canonical, static_cast<uint32_t>(scope.size()));
    write_bytes(canonical, reinterpret_cast<const uint8_t*>(scope.data()), scope.size());

    // TARGET component
    const std::string component = "fang-input-daemon";
    write_string_utf8(canonical, component);

    // TARGET abi
    write_string_utf8(canonical, input.abi);

    // TARGET api_level (u32be)
    write_u32be(canonical, input.api_level);

    // TARGET configuration
    write_string_utf8(canonical, input.configuration);

    // TOOLCHAIN: compiler identity
    write_string_utf8(canonical, "clang");

    // TOOLCHAIN: compiler version (NDK version)
    write_string_utf8(canonical, input.ndk_version);

    // TOOLCHAIN: target triple
    write_string_utf8(canonical, input.target_triple);

    // TOOLCHAIN: argument count (u32be)
    write_u32be(canonical, static_cast<uint32_t>(input.compile_args.size()));

    // TOOLCHAIN: each argument in exact order
    for (const auto& arg : input.compile_args) {
        write_string_utf8(canonical, arg);
    }

    // ENTRIES: entry count (u32be)
    write_u32be(canonical, static_cast<uint32_t>(sorted_entries.size()));

    // ENTRIES: each entry sorted by UTF-8 path bytes (already sorted)
    for (const auto& entry : sorted_entries) {
        // path: u32be length + UTF-8 bytes
        write_string_utf8(canonical, entry.path);

        // kind: u8
        write_u8(canonical, static_cast<uint8_t>(entry.kind));

        // role: u8
        write_u8(canonical, static_cast<uint8_t>(entry.role));

        // size_bytes: u64be
        write_u64be(canonical, entry.size);

        // sha256: 32 raw bytes
        write_hex_digest(canonical, entry.hash);
    }

    // Final fingerprint = SHA-256 of all preceding bytes
    result.fingerprint_sha256 = sha256_hex(canonical);
    result.canonical_bytes = std::move(canonical);
    return result;
}

} // namespace fang::studio::canonical