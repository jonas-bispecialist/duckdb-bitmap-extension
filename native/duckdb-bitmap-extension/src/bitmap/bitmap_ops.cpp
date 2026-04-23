#include "bitmap_ops.hpp"

#include <stdexcept>

namespace duckdb::bitmap
{
std::vector<std::uint8_t> BitmapOr(BitmapBlobView, BitmapBlobView)
{
    throw std::logic_error("BitmapOr is a contract stub and is not implemented yet.");
}

std::vector<std::uint8_t> BitmapAnd(BitmapBlobView, BitmapBlobView)
{
    throw std::logic_error("BitmapAnd is a contract stub and is not implemented yet.");
}

std::vector<std::uint8_t> BitmapAndNot(BitmapBlobView, BitmapBlobView)
{
    throw std::logic_error("BitmapAndNot is a contract stub and is not implemented yet.");
}

std::optional<std::uint64_t> BitmapCount(BitmapBlobView)
{
    throw std::logic_error("BitmapCount is a contract stub and is not implemented yet.");
}

std::optional<bool> BitmapContains(BitmapBlobView, std::uint64_t)
{
    throw std::logic_error("BitmapContains is a contract stub and is not implemented yet.");
}

std::vector<std::uint64_t> BitmapToRows(BitmapBlobView)
{
    throw std::logic_error("BitmapToRows is a contract stub and is not implemented yet.");
}
} // namespace duckdb::bitmap
