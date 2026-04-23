#include "bitmap_blob_format.hpp"

#include <stdexcept>

namespace duckdb::bitmap
{
bool IsBitmapBlobView(BitmapBlobView blob) noexcept
{
    return blob.data != nullptr || blob.size == 0;
}

bool HasBitmapBlobEnvelope(BitmapBlobView blob) noexcept
{
    // Stub-only contract. Real validation will check the FPBM header and length.
    return blob.size >= kBitmapBlobHeaderSize;
}

std::optional<BitmapBlobEnvelope> TryParseBitmapBlob(BitmapBlobView blob) noexcept
{
    (void)blob;
    // Stub-only contract. Final implementation will parse the envelope,
    // validate the FPBM header, and expose the CRoaring payload without
    // copying where possible.
    throw std::logic_error("TryParseBitmapBlob is a contract stub and is not implemented yet.");
}

std::vector<std::uint8_t> MakeBitmapBlob(BitmapBlobView)
{
    throw std::logic_error("MakeBitmapBlob is a contract stub and is not implemented yet.");
}

std::vector<std::uint8_t> MakeEmptyBitmapBlob()
{
    throw std::logic_error("MakeEmptyBitmapBlob is a contract stub and is not implemented yet.");
}
} // namespace duckdb::bitmap
