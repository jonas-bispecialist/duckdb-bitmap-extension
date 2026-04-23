#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace duckdb::bitmap
{
/// Bitmap blob envelope for CRoaring-compatible payloads.
///
/// Layout (little-endian for multibyte fields):
/// - bytes 0..3   : magic = "FPBM"
/// - byte  4      : format version = 1
/// - byte  5      : payload encoding = 1 (CRoaring portable serialization)
/// - bytes 6..7   : reserved, currently 0
/// - bytes 8..11  : payload length in bytes
/// - bytes 12..N  : CRoaring portable bitmap payload
///
/// The payload is treated as opaque by the extension layer. The extension only
/// validates the envelope and passes the payload to CRoaring-compatible code.
struct BitmapBlobHeader
{
    std::array<std::uint8_t, 4> magic{};
    std::uint8_t version = 1;
    std::uint8_t encoding = 1;
    std::uint16_t reserved = 0;
    std::uint32_t payload_length = 0;
};

struct BitmapBlobView
{
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
};

struct BitmapBlobEnvelope
{
    BitmapBlobHeader header{};
    BitmapBlobView payload{};
};

enum class BitmapPayloadEncoding : std::uint8_t
{
    CroaringPortable = 1
};

constexpr std::array<std::uint8_t, 4> kBitmapMagic = {'F', 'P', 'B', 'M'};
constexpr std::uint8_t kBitmapFormatVersion = 1;
constexpr std::uint8_t kBitmapPayloadEncodingCroaringPortable = 1;
constexpr std::size_t kBitmapBlobHeaderSize = 12;

[[nodiscard]] bool IsBitmapBlobView(BitmapBlobView blob) noexcept;
[[nodiscard]] bool HasBitmapBlobEnvelope(BitmapBlobView blob) noexcept;
[[nodiscard]] std::optional<BitmapBlobEnvelope> TryParseBitmapBlob(BitmapBlobView blob) noexcept;
[[nodiscard]] std::vector<std::uint8_t> MakeBitmapBlob(BitmapBlobView croaring_payload);
[[nodiscard]] std::vector<std::uint8_t> MakeEmptyBitmapBlob();
} // namespace duckdb::bitmap
