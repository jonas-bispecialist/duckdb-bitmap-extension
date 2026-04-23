#pragma once

#include "bitmap_blob_format.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace duckdb::bitmap
{
/// Bitmap set-operation and inspection contracts.
///
/// These signatures define the V1 SQL surface in a transport-agnostic form.
/// The eventual DuckDB binding layer should map:
/// - BLOB -> BitmapBlobView / opaque byte buffer
/// - bm_or / bm_and / bm_andnot -> BLOB result
/// - bm_count -> BIGINT / UBIGINT result
/// - bm_contains -> BOOLEAN result
/// - bm_to_rows -> table function yielding BIGINT row ids
///
/// Null policy for the future SQL binding:
/// - binary operators are NULL-preserving
/// - bm_count(NULL) returns NULL
/// - bm_contains(NULL, x) returns NULL
/// - bm_to_rows(NULL) returns zero rows

[[nodiscard]] std::vector<std::uint8_t> BitmapOr(BitmapBlobView lhs, BitmapBlobView rhs);
[[nodiscard]] std::vector<std::uint8_t> BitmapAnd(BitmapBlobView lhs, BitmapBlobView rhs);
[[nodiscard]] std::vector<std::uint8_t> BitmapAndNot(BitmapBlobView lhs, BitmapBlobView rhs);
[[nodiscard]] std::optional<std::uint64_t> BitmapCount(BitmapBlobView bitmap);
[[nodiscard]] std::optional<bool> BitmapContains(BitmapBlobView bitmap, std::uint64_t row_id);
[[nodiscard]] std::vector<std::uint64_t> BitmapToRows(BitmapBlobView bitmap);
} // namespace duckdb::bitmap
