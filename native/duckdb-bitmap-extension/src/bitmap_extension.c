#include "duckdb_extension.h"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "roaring.h"

DUCKDB_EXTENSION_EXTERN

// Bitmap scalars use an FPBM envelope with sorted unique uint32 payloads.

#define BITMAP_MAGIC_0 'F'
#define BITMAP_MAGIC_1 'P'
#define BITMAP_MAGIC_2 'B'
#define BITMAP_MAGIC_3 'M'
#define BITMAP_FORMAT_VERSION 1
#define BITMAP_ENCODING_SORTED_U32 1
#define BITMAP_ENCODING_ROARING32  2
#define BITMAP_HEADER_SIZE 12

typedef struct BitmapView {
    const uint8_t *payload;
    idx_t count;
    idx_t payload_size;
    idx_t total_size;
    uint8_t version;
    uint8_t encoding;
} BitmapView;

typedef enum BitmapBinaryOp {
    BITMAP_OP_OR = 0,
    BITMAP_OP_AND = 1,
    BITMAP_OP_ANDNOT = 2
} BitmapBinaryOp;

static void SetFunctionError(duckdb_function_info info, const char *message) {
    duckdb_function_set_error(info, message);
}

static uint32_t ReadU32LE(const uint8_t *ptr) {
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8U) | ((uint32_t)ptr[2] << 16U) | ((uint32_t)ptr[3] << 24U);
}

static void WriteU32LE(uint8_t *ptr, uint32_t value) {
    ptr[0] = (uint8_t)(value & 0xFFU);
    ptr[1] = (uint8_t)((value >> 8U) & 0xFFU);
    ptr[2] = (uint8_t)((value >> 16U) & 0xFFU);
    ptr[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static bool RowIsValid(uint64_t *validity, idx_t row) {
    return validity == NULL || duckdb_validity_row_is_valid(validity, row);
}

static const uint8_t *BlobDataAt(duckdb_vector vector, idx_t row, idx_t *size_out) {
    duckdb_string_t *entries = (duckdb_string_t *)duckdb_vector_get_data(vector);
    *size_out = (idx_t)duckdb_string_t_length(entries[row]);
    return (const uint8_t *)duckdb_string_t_data(&entries[row]);
}

static bool ParseBitmapBlobBytes(duckdb_function_info info, const uint8_t *data, idx_t size, BitmapView *out) {
    if (size < BITMAP_HEADER_SIZE) {
        SetFunctionError(info, "invalid bitmap blob: envelope is too small");
        return false;
    }
    if (data[0] != BITMAP_MAGIC_0 || data[1] != BITMAP_MAGIC_1 || data[2] != BITMAP_MAGIC_2 ||
        data[3] != BITMAP_MAGIC_3) {
        SetFunctionError(info, "invalid bitmap blob: bad magic");
        return false;
    }
    if (data[4] != BITMAP_FORMAT_VERSION) {
        SetFunctionError(info, "invalid bitmap blob: unsupported version");
        return false;
    }
    uint8_t encoding = data[5];
    if (encoding != BITMAP_ENCODING_SORTED_U32 && encoding != BITMAP_ENCODING_ROARING32) {
        SetFunctionError(info, "invalid bitmap blob: unsupported payload encoding");
        return false;
    }
    if (data[6] != 0 || data[7] != 0) {
        SetFunctionError(info, "invalid bitmap blob: reserved bytes must be zero");
        return false;
    }

    uint32_t payload_len = ReadU32LE(data + 8);
    if ((idx_t)payload_len != size - BITMAP_HEADER_SIZE) {
        SetFunctionError(info, "invalid bitmap blob: payload length mismatch");
        return false;
    }
    /* The 4-byte alignment check is sorted_u32 specific; roaring32 has variable-length serialization */
    if (encoding == BITMAP_ENCODING_SORTED_U32) {
        if ((payload_len % 4U) != 0U) {
            SetFunctionError(info, "invalid bitmap blob: payload length must be a multiple of 4");
            return false;
        }
    }

    out->payload = data + BITMAP_HEADER_SIZE;
    /* count field: for sorted_u32 it is the element count; for roaring32 it is a sentinel (0)
       and must be computed on demand via BitmapViewCardinality() */
    if (encoding == BITMAP_ENCODING_SORTED_U32) {
        out->count = (idx_t)(payload_len / 4U);
    } else {
        out->count = 0;
    }
    out->payload_size = (idx_t)payload_len;
    out->total_size = size;
    out->version = data[4];
    out->encoding = encoding;
    return true;
}

static bool ParseBitmapBlob(duckdb_function_info info, duckdb_vector vector, idx_t row, BitmapView *out) {
    idx_t size = 0;
    const uint8_t *data = BlobDataAt(vector, row, &size);
    return ParseBitmapBlobBytes(info, data, size, out);
}

static uint32_t BitmapViewValueAt(const BitmapView *view, idx_t index) {
    return ReadU32LE(view->payload + (index * 4));
}

/* CRoaring integration helpers */

/* Deserialize a roaring32 payload to a heap-allocated bitmap.
   Caller MUST call roaring_bitmap_free() on the result.
   Returns NULL on failure (malformed data). */
static roaring_bitmap_t *BitmapViewDeserializeRoaring(const BitmapView *view) {
    return roaring_bitmap_portable_deserialize_safe(
        (const char *)view->payload, (size_t)view->payload_size);
}

/* Returns element count for any encoding.
   For sorted_u32 this is O(1). For roaring32 it deserializes, counts, frees. */
static uint64_t BitmapViewCardinality(const BitmapView *view) {
    if (view->encoding == BITMAP_ENCODING_SORTED_U32) {
        return (uint64_t)view->count;
    }
    roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(view);
    if (rb == NULL) return 0;
    uint64_t c = roaring_bitmap_get_cardinality(rb);
    roaring_bitmap_free(rb);
    return c;
}

/* Converts any BitmapView to heap-owned roaring_bitmap_t.
   Caller MUST roaring_bitmap_free() the result.
   For sorted_u32: builds a new roaring bitmap from the uint32 array (all DuckDB targets are LE).
   For roaring32: deserializes the portable format.
   Returns NULL on OOM or bad data. */
static roaring_bitmap_t *BitmapViewToRoaring(const BitmapView *view) {
    if (view->encoding == BITMAP_ENCODING_SORTED_U32) {
        roaring_bitmap_t *rb = roaring_bitmap_create();
        if (rb == NULL) return NULL;
        if (view->count > 0) {
            /* view->payload is a little-endian uint32 array; roaring_bitmap_add_many expects
               host-endian uint32 values. This cast is safe on LE platforms (all DuckDB targets). */
            roaring_bitmap_add_many(rb, (size_t)view->count,
                                    (const uint32_t *)view->payload);
        }
        return rb;
    }
    /* BITMAP_ENCODING_ROARING32 */
    return BitmapViewDeserializeRoaring(view);
}

static bool MakeBitmapBlobBytes(const uint32_t *values, idx_t count, uint8_t **out_data, idx_t *out_size) {
    if (count > (idx_t)(UINT32_MAX / 4U)) {
        return false;
    }

    idx_t payload_len = count * 4;
    idx_t total_len = BITMAP_HEADER_SIZE + payload_len;
    uint8_t *data = (uint8_t *)malloc((size_t)total_len);
    if (data == NULL) {
        return false;
    }

    data[0] = BITMAP_MAGIC_0;
    data[1] = BITMAP_MAGIC_1;
    data[2] = BITMAP_MAGIC_2;
    data[3] = BITMAP_MAGIC_3;
    data[4] = BITMAP_FORMAT_VERSION;
    data[5] = BITMAP_ENCODING_SORTED_U32;
    data[6] = 0;
    data[7] = 0;
    WriteU32LE(data + 8, (uint32_t)payload_len);

    for (idx_t i = 0; i < count; i++) {
        WriteU32LE(data + BITMAP_HEADER_SIZE + (i * 4), values[i]);
    }

    *out_data = data;
    *out_size = total_len;
    return true;
}

/* Serialize a roaring_bitmap_t to an FPBM blob with encoding=2 (ROARING32).
   Applies run-length optimization before serializing.
   Caller owns the returned buffer; free() it when done.
   Returns false on OOM or if roaring bitmap is invalid. */
static bool MakeRoaringBlobBytes(roaring_bitmap_t *rb, uint8_t **out_data, idx_t *out_size) {
    roaring_bitmap_run_optimize(rb);

    size_t payload_len = roaring_bitmap_portable_size_in_bytes(rb);
    if (payload_len > (size_t)UINT32_MAX) {
        return false;
    }

    idx_t total_len = BITMAP_HEADER_SIZE + (idx_t)payload_len;
    uint8_t *data = (uint8_t *)malloc((size_t)total_len);
    if (data == NULL) {
        return false;
    }

    data[0] = BITMAP_MAGIC_0;
    data[1] = BITMAP_MAGIC_1;
    data[2] = BITMAP_MAGIC_2;
    data[3] = BITMAP_MAGIC_3;
    data[4] = BITMAP_FORMAT_VERSION;
    data[5] = BITMAP_ENCODING_ROARING32;
    data[6] = 0;
    data[7] = 0;
    WriteU32LE(data + 8, (uint32_t)payload_len);

    roaring_bitmap_portable_serialize(rb, (char *)(data + BITMAP_HEADER_SIZE));

    *out_data = data;
    *out_size = total_len;
    return true;
}



static bool BitmapContainsValue(const BitmapView *view, uint64_t needle) {
    if (needle > UINT32_MAX) {
        return false;
    }

    uint32_t target = (uint32_t)needle;

    if (view->encoding == BITMAP_ENCODING_SORTED_U32) {
        idx_t low = 0;
        idx_t high = view->count;
        while (low < high) {
            idx_t mid = low + ((high - low) / 2);
            if (BitmapViewValueAt(view, mid) < target) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low < view->count && BitmapViewValueAt(view, low) == target;
    }

    roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(view);
    if (rb == NULL) return false;
    bool result = roaring_bitmap_contains(rb, target);
    roaring_bitmap_free(rb);
    return result;
}

static idx_t BitmapLowerBoundGreaterThan(const BitmapView *view, int64_t start_after) {
    if (start_after < 0) {
        return 0;
    }
    if ((uint64_t)start_after >= UINT32_MAX) {
        return view->count;
    }

    uint32_t target = (uint32_t)((uint64_t)start_after + 1U);
    idx_t low = 0;
    idx_t high = view->count;
    while (low < high) {
        idx_t mid = low + ((high - low) / 2);
        if (BitmapViewValueAt(view, mid) < target) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}


typedef struct BitmapAggState {
    roaring_bitmap_t *rb;
    bool failed;
} BitmapAggState;

static void BitmapHello(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    (void)info;
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector input_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(input_vector);
    if (input_validity) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (input_validity && !duckdb_validity_row_is_valid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        duckdb_vector_assign_string_element(output, row, "bitmap hello from extension");
    }
}

static void BitmapOrFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector lhs_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector rhs_vector = duckdb_data_chunk_get_vector(input, 1);
    uint64_t *lhs_validity = duckdb_vector_get_validity(lhs_vector);
    uint64_t *rhs_validity = duckdb_vector_get_validity(rhs_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView lhs = {0};
        BitmapView rhs = {0};
        if (!ParseBitmapBlob(info, lhs_vector, row, &lhs)) {
            return;
        }
        if (!ParseBitmapBlob(info, rhs_vector, row, &rhs)) {
            return;
        }

        roaring_bitmap_t *rb_lhs = BitmapViewToRoaring(&lhs);
        if (rb_lhs == NULL) {
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }
        roaring_bitmap_t *rb_rhs = BitmapViewToRoaring(&rhs);
        if (rb_rhs == NULL) {
            roaring_bitmap_free(rb_lhs);
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }

        roaring_bitmap_t *result = roaring_bitmap_or(rb_lhs, rb_rhs);
        roaring_bitmap_free(rb_lhs);
        roaring_bitmap_free(rb_rhs);

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeRoaringBlobBytes(result, &blob_bytes, &blob_size)) {
            roaring_bitmap_free(result);
            SetFunctionError(info, "out of memory while serializing bitmap union");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        roaring_bitmap_free(result);
    }
}

static void BitmapAndFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector lhs_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector rhs_vector = duckdb_data_chunk_get_vector(input, 1);
    uint64_t *lhs_validity = duckdb_vector_get_validity(lhs_vector);
    uint64_t *rhs_validity = duckdb_vector_get_validity(rhs_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView lhs = {0};
        BitmapView rhs = {0};
        if (!ParseBitmapBlob(info, lhs_vector, row, &lhs)) {
            return;
        }
        if (!ParseBitmapBlob(info, rhs_vector, row, &rhs)) {
            return;
        }

        roaring_bitmap_t *rb_lhs = BitmapViewToRoaring(&lhs);
        if (rb_lhs == NULL) {
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }
        roaring_bitmap_t *rb_rhs = BitmapViewToRoaring(&rhs);
        if (rb_rhs == NULL) {
            roaring_bitmap_free(rb_lhs);
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }

        roaring_bitmap_t *result = roaring_bitmap_and(rb_lhs, rb_rhs);
        roaring_bitmap_free(rb_lhs);
        roaring_bitmap_free(rb_rhs);

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeRoaringBlobBytes(result, &blob_bytes, &blob_size)) {
            roaring_bitmap_free(result);
            SetFunctionError(info, "out of memory while serializing bitmap intersection");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        roaring_bitmap_free(result);
    }
}

static void BitmapAndNotFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector lhs_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector rhs_vector = duckdb_data_chunk_get_vector(input, 1);
    uint64_t *lhs_validity = duckdb_vector_get_validity(lhs_vector);
    uint64_t *rhs_validity = duckdb_vector_get_validity(rhs_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView lhs = {0};
        BitmapView rhs = {0};
        if (!ParseBitmapBlob(info, lhs_vector, row, &lhs)) {
            return;
        }
        if (!ParseBitmapBlob(info, rhs_vector, row, &rhs)) {
            return;
        }

        roaring_bitmap_t *rb_lhs = BitmapViewToRoaring(&lhs);
        if (rb_lhs == NULL) {
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }
        roaring_bitmap_t *rb_rhs = BitmapViewToRoaring(&rhs);
        if (rb_rhs == NULL) {
            roaring_bitmap_free(rb_lhs);
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }

        roaring_bitmap_t *result = roaring_bitmap_andnot(rb_lhs, rb_rhs);
        roaring_bitmap_free(rb_lhs);
        roaring_bitmap_free(rb_rhs);

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeRoaringBlobBytes(result, &blob_bytes, &blob_size)) {
            roaring_bitmap_free(result);
            SetFunctionError(info, "out of memory while serializing bitmap difference");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        roaring_bitmap_free(result);
    }
}

static void BitmapCountFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(bitmap_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);
    uint64_t *out_data = (uint64_t *)duckdb_vector_get_data(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }
        out_data[row] = BitmapViewCardinality(&bitmap);
    }
}

static void BitmapCountBinaryCommon(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output,
                                    BitmapBinaryOp op) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector lhs_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector rhs_vector = duckdb_data_chunk_get_vector(input, 1);
    uint64_t *lhs_validity = duckdb_vector_get_validity(lhs_vector);
    uint64_t *rhs_validity = duckdb_vector_get_validity(rhs_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);
    uint64_t *out_data = (uint64_t *)duckdb_vector_get_data(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView lhs = {0};
        BitmapView rhs = {0};
        if (!ParseBitmapBlob(info, lhs_vector, row, &lhs)) {
            return;
        }
        if (!ParseBitmapBlob(info, rhs_vector, row, &rhs)) {
            return;
        }

        roaring_bitmap_t *rb_lhs = BitmapViewToRoaring(&lhs);
        if (rb_lhs == NULL) {
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }
        roaring_bitmap_t *rb_rhs = BitmapViewToRoaring(&rhs);
        if (rb_rhs == NULL) {
            roaring_bitmap_free(rb_lhs);
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }

        switch (op) {
        case BITMAP_OP_OR:
            out_data[row] = roaring_bitmap_or_cardinality(rb_lhs, rb_rhs);
            break;
        case BITMAP_OP_AND:
            out_data[row] = roaring_bitmap_and_cardinality(rb_lhs, rb_rhs);
            break;
        case BITMAP_OP_ANDNOT:
            out_data[row] = roaring_bitmap_andnot_cardinality(rb_lhs, rb_rhs);
            break;
        }

        roaring_bitmap_free(rb_lhs);
        roaring_bitmap_free(rb_rhs);
    }
}

static void BitmapCountOrFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    BitmapCountBinaryCommon(info, input, output, BITMAP_OP_OR);
}

static void BitmapCountAndFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    BitmapCountBinaryCommon(info, input, output, BITMAP_OP_AND);
}

static void BitmapCountAndNotFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    BitmapCountBinaryCommon(info, input, output, BITMAP_OP_ANDNOT);
}

static void BitmapReencodeRoaringFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(bitmap_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }

        if (bitmap.encoding == BITMAP_ENCODING_ROARING32) {
            const uint8_t *blob_data = NULL;
            idx_t blob_size = 0;
            blob_data = BlobDataAt(bitmap_vector, row, &blob_size);
            duckdb_vector_assign_string_element_len(output, row, (const char *)blob_data, blob_size);
        } else {
            roaring_bitmap_t *rb = BitmapViewToRoaring(&bitmap);
            if (rb == NULL) {
                SetFunctionError(info, "out of memory while re-encoding bitmap");
                return;
            }
            uint8_t *blob_bytes = NULL;
            idx_t blob_size = 0;
            if (!MakeRoaringBlobBytes(rb, &blob_bytes, &blob_size)) {
                roaring_bitmap_free(rb);
                SetFunctionError(info, "out of memory while serializing re-encoded bitmap");
                return;
            }
            duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);
            free(blob_bytes);
            roaring_bitmap_free(rb);
        }
    }
}

static void BitmapIntersectsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector lhs_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector rhs_vector = duckdb_data_chunk_get_vector(input, 1);
    uint64_t *lhs_validity = duckdb_vector_get_validity(lhs_vector);
    uint64_t *rhs_validity = duckdb_vector_get_validity(rhs_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);
    bool *out_data = (bool *)duckdb_vector_get_data(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(lhs_validity, row) || !RowIsValid(rhs_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView lhs = {0};
        BitmapView rhs = {0};
        if (!ParseBitmapBlob(info, lhs_vector, row, &lhs)) {
            return;
        }
        if (!ParseBitmapBlob(info, rhs_vector, row, &rhs)) {
            return;
        }

        roaring_bitmap_t *rb_lhs = BitmapViewToRoaring(&lhs);
        if (rb_lhs == NULL) {
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }
        roaring_bitmap_t *rb_rhs = BitmapViewToRoaring(&rhs);
        if (rb_rhs == NULL) {
            roaring_bitmap_free(rb_lhs);
            SetFunctionError(info, "out of memory while converting bitmap to roaring");
            return;
        }

        out_data[row] = roaring_bitmap_intersect(rb_lhs, rb_rhs);

        roaring_bitmap_free(rb_lhs);
        roaring_bitmap_free(rb_rhs);
    }
}

static void BitmapContainsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector rowid_vector = duckdb_data_chunk_get_vector(input, 1);
    uint64_t *bitmap_validity = duckdb_vector_get_validity(bitmap_vector);
    uint64_t *rowid_validity = duckdb_vector_get_validity(rowid_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(bitmap_validity, row) || !RowIsValid(rowid_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);
    bool *out_data = (bool *)duckdb_vector_get_data(output);
    uint64_t *rowid_data = (uint64_t *)duckdb_vector_get_data(rowid_vector);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(bitmap_validity, row) || !RowIsValid(rowid_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }
        out_data[row] = BitmapContainsValue(&bitmap, rowid_data[row]);
    }
}

static const char *BitmapFormatName(const BitmapView *bitmap) {
    if (bitmap->version == BITMAP_FORMAT_VERSION) {
        if (bitmap->encoding == BITMAP_ENCODING_SORTED_U32) {
            return "sorted_u32_v1";
        }
        if (bitmap->encoding == BITMAP_ENCODING_ROARING32) {
            return "roaring32_v1";
        }
    }
    return "unknown";
}

static void BitmapFormatFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(bitmap_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }
        duckdb_vector_assign_string_element(output, row, BitmapFormatName(&bitmap));
    }
}

static void BitmapStatsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(bitmap_vector);
    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    uint64_t *output_validity = duckdb_vector_get_validity(output);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }

        char stats[512];
        if (bitmap.encoding == BITMAP_ENCODING_SORTED_U32) {
            int written = snprintf(stats, sizeof(stats),
                                   "encoding=%s;cardinality=%llu;serialized_bytes=%llu;payload_bytes=%llu",
                                   BitmapFormatName(&bitmap), (unsigned long long)bitmap.count,
                                   (unsigned long long)bitmap.total_size, (unsigned long long)bitmap.payload_size);
            if (written < 0 || (size_t)written >= sizeof(stats)) {
                SetFunctionError(info, "failed to format bitmap stats");
                return;
            }
        } else {
            roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(&bitmap);
            if (rb == NULL) {
                SetFunctionError(info, "failed to deserialize roaring bitmap for stats");
                return;
            }
            uint64_t card = roaring_bitmap_get_cardinality(rb);
            roaring_statistics_t roar_stats;
            roaring_bitmap_statistics(rb, &roar_stats);
            int written = snprintf(stats, sizeof(stats),
                                   "encoding=%s;cardinality=%llu;serialized_bytes=%llu;payload_bytes=%llu;"
                                   "containers=%u;array=%u;run=%u;bitset=%u",
                                   BitmapFormatName(&bitmap), (unsigned long long)card,
                                   (unsigned long long)bitmap.total_size, (unsigned long long)bitmap.payload_size,
                                   roar_stats.n_containers, roar_stats.n_array_containers,
                                   roar_stats.n_run_containers, roar_stats.n_bitset_containers);
            roaring_bitmap_free(rb);
            if (written < 0 || (size_t)written >= sizeof(stats)) {
                SetFunctionError(info, "failed to format bitmap stats");
                return;
            }
        }
        duckdb_vector_assign_string_element(output, row, stats);
    }
}

static void BitmapToRowsFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(bitmap_vector);
    bool has_null = false;
    idx_t total_rows = 0;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            has_null = true;
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }
        uint64_t card = BitmapViewCardinality(&bitmap);
        if (total_rows > (idx_t)(SIZE_MAX - card)) {
            SetFunctionError(info, "bitmap row output exceeds supported size");
            return;
        }
        total_rows += card;
    }

    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    if (duckdb_list_vector_reserve(output, total_rows) == DuckDBError) {
        SetFunctionError(info, "out of memory while reserving bitmap row output");
        return;
    }
    if (duckdb_list_vector_set_size(output, total_rows) == DuckDBError) {
        SetFunctionError(info, "failed to size bitmap row output");
        return;
    }

    uint64_t *output_validity = duckdb_vector_get_validity(output);
    duckdb_list_entry *out_entries = (duckdb_list_entry *)duckdb_vector_get_data(output);
    duckdb_vector child_vector = duckdb_list_vector_get_child(output);
    uint64_t *child_data = (uint64_t *)duckdb_vector_get_data(child_vector);

    idx_t child_offset = 0;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            out_entries[row].offset = 0;
            out_entries[row].length = 0;
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }

        if (bitmap.encoding == BITMAP_ENCODING_SORTED_U32) {
            out_entries[row].offset = (uint64_t)child_offset;
            out_entries[row].length = (uint64_t)bitmap.count;
            for (idx_t i = 0; i < bitmap.count; i++) {
                child_data[child_offset + i] = (uint64_t)BitmapViewValueAt(&bitmap, i);
            }
            child_offset += bitmap.count;
        } else {
            roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(&bitmap);
            if (rb == NULL) {
                SetFunctionError(info, "failed to deserialize roaring bitmap");
                return;
            }
            roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
            out_entries[row].offset = (uint64_t)child_offset;
            idx_t i = 0;
            while (it->has_value) {
                child_data[child_offset + i++] = (uint64_t)it->current_value;
                roaring_uint32_iterator_advance(it);
            }
            out_entries[row].length = (uint64_t)i;
            roaring_uint32_iterator_free(it);
            roaring_bitmap_free(rb);
            child_offset += i;
        }
    }
}

static void BitmapToRowsLimitedFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector limit_vector = duckdb_data_chunk_get_vector(input, 1);
    duckdb_vector start_after_vector = duckdb_data_chunk_get_vector(input, 2);
    uint64_t *bitmap_validity = duckdb_vector_get_validity(bitmap_vector);
    uint64_t *limit_validity = duckdb_vector_get_validity(limit_vector);
    uint64_t *start_after_validity = duckdb_vector_get_validity(start_after_vector);
    uint64_t *limit_data = (uint64_t *)duckdb_vector_get_data(limit_vector);
    int64_t *start_after_data = (int64_t *)duckdb_vector_get_data(start_after_vector);

    bool has_null = false;
    idx_t total_rows = 0;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(bitmap_validity, row) || !RowIsValid(limit_validity, row) ||
            !RowIsValid(start_after_validity, row)) {
            has_null = true;
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }

        int64_t threshold = start_after_data[row];
        idx_t limit = limit_data[row] > (uint64_t)SIZE_MAX ? (idx_t)SIZE_MAX : (idx_t)limit_data[row];
        idx_t emit_count = 0;

        if (bitmap.encoding == BITMAP_ENCODING_SORTED_U32) {
            idx_t start = BitmapLowerBoundGreaterThan(&bitmap, threshold);
            idx_t remaining = bitmap.count - start;
            emit_count = remaining < limit ? remaining : limit;
        } else {
            roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(&bitmap);
            if (rb == NULL) {
                SetFunctionError(info, "failed to deserialize roaring bitmap for limit");
                return;
            }
            uint32_t threshold_u32 = (threshold < 0 || (uint64_t)threshold >= UINT32_MAX) ? UINT32_MAX : (uint32_t)(threshold + 1);
            roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
            if (threshold_u32 < UINT32_MAX) {
                while (it->has_value && it->current_value < threshold_u32) {
                    roaring_uint32_iterator_advance(it);
                }
            }
            while (it->has_value && emit_count < limit) {
                emit_count++;
                roaring_uint32_iterator_advance(it);
            }
            roaring_uint32_iterator_free(it);
            roaring_bitmap_free(rb);
        }

        if (total_rows > (idx_t)(SIZE_MAX - emit_count)) {
            SetFunctionError(info, "bitmap row output exceeds supported size");
            return;
        }
        total_rows += emit_count;
    }

    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }
    if (duckdb_list_vector_reserve(output, total_rows) == DuckDBError) {
        SetFunctionError(info, "out of memory while reserving limited bitmap row output");
        return;
    }
    if (duckdb_list_vector_set_size(output, total_rows) == DuckDBError) {
        SetFunctionError(info, "failed to size limited bitmap row output");
        return;
    }

    uint64_t *output_validity = duckdb_vector_get_validity(output);
    duckdb_list_entry *out_entries = (duckdb_list_entry *)duckdb_vector_get_data(output);
    duckdb_vector child_vector = duckdb_list_vector_get_child(output);
    uint64_t *child_data = (uint64_t *)duckdb_vector_get_data(child_vector);

    idx_t child_offset = 0;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(bitmap_validity, row) || !RowIsValid(limit_validity, row) ||
            !RowIsValid(start_after_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            out_entries[row].offset = 0;
            out_entries[row].length = 0;
            continue;
        }

        BitmapView bitmap = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &bitmap)) {
            return;
        }

        int64_t threshold = start_after_data[row];
        idx_t limit = limit_data[row] > (uint64_t)SIZE_MAX ? (idx_t)SIZE_MAX : (idx_t)limit_data[row];
        out_entries[row].offset = (uint64_t)child_offset;
        idx_t i = 0;

        if (bitmap.encoding == BITMAP_ENCODING_SORTED_U32) {
            idx_t start = BitmapLowerBoundGreaterThan(&bitmap, threshold);
            idx_t remaining = bitmap.count - start;
            idx_t emit_count = remaining < limit ? remaining : limit;
            for (idx_t j = 0; j < emit_count; j++) {
                child_data[child_offset + j] = (uint64_t)BitmapViewValueAt(&bitmap, start + j);
            }
            i = emit_count;
        } else {
            roaring_bitmap_t *rb = BitmapViewDeserializeRoaring(&bitmap);
            if (rb == NULL) {
                SetFunctionError(info, "failed to deserialize roaring bitmap");
                return;
            }
            uint32_t threshold_u32 = (threshold < 0 || (uint64_t)threshold >= UINT32_MAX) ? UINT32_MAX : (uint32_t)(threshold + 1);
            roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
            if (threshold_u32 < UINT32_MAX) {
                while (it->has_value && it->current_value < threshold_u32) {
                    roaring_uint32_iterator_advance(it);
                }
            }
            while (it->has_value && i < limit) {
                child_data[child_offset + i++] = (uint64_t)it->current_value;
                roaring_uint32_iterator_advance(it);
            }
            roaring_uint32_iterator_free(it);
            roaring_bitmap_free(rb);
        }

        out_entries[row].length = (uint64_t)i;
        child_offset += i;
    }
}

static void BitmapBuildFunction(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector list_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *input_validity = duckdb_vector_get_validity(list_vector);

    bool has_null = false;
    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            has_null = true;
            break;
        }
    }
    if (has_null) {
        duckdb_vector_ensure_validity_writable(output);
    }

    uint64_t *output_validity = duckdb_vector_get_validity(output);
    duckdb_list_entry *in_entries = (duckdb_list_entry *)duckdb_vector_get_data(list_vector);
    duckdb_vector child_vector = duckdb_list_vector_get_child(list_vector);
    idx_t child_size = duckdb_list_vector_get_size(list_vector);
    uint64_t *child_data = (uint64_t *)duckdb_vector_get_data(child_vector);
    uint64_t *child_validity = duckdb_vector_get_validity(child_vector);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(input_validity, row)) {
            duckdb_validity_set_row_invalid(output_validity, row);
            continue;
        }

        idx_t offset = (idx_t)in_entries[row].offset;
        idx_t length = (idx_t)in_entries[row].length;
        if (offset > child_size || length > child_size - offset) {
            SetFunctionError(info, "bm_build received invalid LIST offsets");
            return;
        }

        roaring_bitmap_t *rb = roaring_bitmap_create();
        if (rb == NULL) {
            SetFunctionError(info, "out of memory while building bitmap");
            return;
        }

        for (idx_t i = 0; i < length; i++) {
            idx_t child_index = offset + i;
            if (!RowIsValid(child_validity, child_index)) {
                roaring_bitmap_free(rb);
                SetFunctionError(info, "bm_build does not accept NULL row ids");
                return;
            }

            uint64_t row_id = child_data[child_index];
            if (row_id > UINT32_MAX) {
                roaring_bitmap_free(rb);
                SetFunctionError(info, "bm_build row id exceeds UINT32 range");
                return;
            }
            roaring_bitmap_add(rb, (uint32_t)row_id);
        }

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeRoaringBlobBytes(rb, &blob_bytes, &blob_size)) {
            roaring_bitmap_free(rb);
            SetFunctionError(info, "out of memory while serializing bm_build output");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        roaring_bitmap_free(rb);
    }
}

static idx_t BitmapAggStateSize(duckdb_function_info info) {
    (void)info;
    return sizeof(BitmapAggState);
}

static void BitmapAggInit(duckdb_function_info info, duckdb_aggregate_state state) {
    (void)info;
    BitmapAggState *agg = (BitmapAggState *)state;
    agg->rb = NULL;
    agg->failed = false;
}

static void BitmapAggDestroy(duckdb_aggregate_state *states, idx_t count) {
    for (idx_t i = 0; i < count; i++) {
        BitmapAggState *state = (BitmapAggState *)states[i];
        if (state == NULL) {
            continue;
        }
        if (state->rb != NULL) {
            roaring_bitmap_free(state->rb);
            state->rb = NULL;
        }
    }
}

static void BitmapBuildAggUpdate(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector rowid_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *rowid_validity = duckdb_vector_get_validity(rowid_vector);
    uint64_t *rowid_data = (uint64_t *)duckdb_vector_get_data(rowid_vector);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(rowid_validity, row)) {
            continue;
        }

        uint64_t row_id = rowid_data[row];
        if (row_id > UINT32_MAX) {
            BitmapAggState *state = (BitmapAggState *)states[row];
            state->failed = true;
            duckdb_aggregate_function_set_error(info, "bm_build_agg row id exceeds UINT32 range");
            return;
        }

        BitmapAggState *state = (BitmapAggState *)states[row];
        if (state->rb == NULL) {
            state->rb = roaring_bitmap_create();
            if (state->rb == NULL) {
                state->failed = true;
                duckdb_aggregate_function_set_error(info, "out of memory while updating bm_build_agg");
                return;
            }
        }
        roaring_bitmap_add(state->rb, (uint32_t)row_id);
    }
}

static void BitmapOrAggUpdate(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *bitmap_validity = duckdb_vector_get_validity(bitmap_vector);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(bitmap_validity, row)) {
            continue;
        }

        BitmapView view = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &view)) {
            BitmapAggState *state = (BitmapAggState *)states[row];
            state->failed = true;
            return;
        }

        BitmapAggState *state = (BitmapAggState *)states[row];
        if (state->rb == NULL) {
            state->rb = roaring_bitmap_create();
            if (state->rb == NULL) {
                state->failed = true;
                duckdb_aggregate_function_set_error(info, "out of memory while updating bm_or_agg");
                return;
            }
        }

        if (view.encoding == BITMAP_ENCODING_SORTED_U32) {
            /* Fast path: bulk-add sorted u32 array (all DuckDB targets are LE) */
            roaring_bitmap_add_many(state->rb, (size_t)view.count,
                                    (const uint32_t *)view.payload);
        } else {
            /* roaring32 path: deserialize, OR in, free */
            roaring_bitmap_t *incoming = BitmapViewDeserializeRoaring(&view);
            if (incoming == NULL) {
                state->failed = true;
                duckdb_aggregate_function_set_error(info, "failed to deserialize bitmap in bm_or_agg");
                return;
            }
            roaring_bitmap_or_inplace(state->rb, incoming);
            roaring_bitmap_free(incoming);
        }
    }
}

static void BitmapAndAggUpdate(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
    idx_t row_count = duckdb_data_chunk_get_size(input);
    duckdb_vector bitmap_vector = duckdb_data_chunk_get_vector(input, 0);
    uint64_t *bitmap_validity = duckdb_vector_get_validity(bitmap_vector);

    for (idx_t row = 0; row < row_count; row++) {
        if (!RowIsValid(bitmap_validity, row)) {
            continue;
        }

        BitmapView view = {0};
        if (!ParseBitmapBlob(info, bitmap_vector, row, &view)) {
            BitmapAggState *state = (BitmapAggState *)states[row];
            state->failed = true;
            return;
        }

        BitmapAggState *state = (BitmapAggState *)states[row];
        roaring_bitmap_t *incoming = BitmapViewToRoaring(&view);
        if (incoming == NULL) {
            state->failed = true;
            duckdb_aggregate_function_set_error(info, "out of memory while updating bm_and_agg");
            return;
        }

        if (state->rb == NULL) {
            state->rb = incoming;
        } else {
            roaring_bitmap_and_inplace(state->rb, incoming);
            roaring_bitmap_free(incoming);
        }
    }
}

static void BitmapAggCombine(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_aggregate_state *target,
                             idx_t count) {
    for (idx_t i = 0; i < count; i++) {
        BitmapAggState *source_state = (BitmapAggState *)source[i];
        BitmapAggState *target_state = (BitmapAggState *)target[i];

        if (source_state->failed || target_state->failed) {
            target_state->failed = true;
            duckdb_aggregate_function_set_error(info, "bitmap aggregate state is failed");
            return;
        }
        if (source_state->rb == NULL) {
            continue;
        }

        if (target_state->rb == NULL) {
            target_state->rb = roaring_bitmap_copy(source_state->rb);
            if (target_state->rb == NULL) {
                target_state->failed = true;
                duckdb_aggregate_function_set_error(info, "out of memory while combining bitmap aggregate states");
                return;
            }
        } else {
            roaring_bitmap_or_inplace(target_state->rb, source_state->rb);
        }
    }
}

static void BitmapOrAggCombine(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_aggregate_state *target,
                               idx_t count) {
    /* bm_or_agg combine is identical to bm_build_agg combine (both OR roaring bitmaps) */
    BitmapAggCombine(info, source, target, count);
}

static void BitmapAndAggCombine(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_aggregate_state *target,
                                idx_t count) {
    for (idx_t i = 0; i < count; i++) {
        BitmapAggState *source_state = (BitmapAggState *)source[i];
        BitmapAggState *target_state = (BitmapAggState *)target[i];

        if (source_state->failed || target_state->failed) {
            target_state->failed = true;
            duckdb_aggregate_function_set_error(info, "bitmap aggregate state is failed");
            return;
        }
        if (source_state->rb == NULL) {
            continue;
        }

        if (target_state->rb == NULL) {
            target_state->rb = roaring_bitmap_copy(source_state->rb);
            if (target_state->rb == NULL) {
                target_state->failed = true;
                duckdb_aggregate_function_set_error(info, "out of memory while combining bitmap aggregate states");
                return;
            }
        } else {
            roaring_bitmap_and_inplace(target_state->rb, source_state->rb);
        }
    }
}

static void BitmapAggFinalize(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_vector result,
                              idx_t count, idx_t offset) {
    for (idx_t i = 0; i < count; i++) {
        BitmapAggState *state = (BitmapAggState *)source[i];
        if (state->failed) {
            duckdb_aggregate_function_set_error(info, "bitmap aggregate state is failed");
            return;
        }

        roaring_bitmap_t *rb = state->rb;
        bool created_empty = false;
        if (rb == NULL) {
            rb = roaring_bitmap_create();
            if (rb == NULL) {
                duckdb_aggregate_function_set_error(info, "out of memory while finalizing bitmap aggregate");
                return;
            }
            created_empty = true;
        }

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeRoaringBlobBytes(rb, &blob_bytes, &blob_size)) {
            if (created_empty) {
                roaring_bitmap_free(rb);
            }
            duckdb_aggregate_function_set_error(info, "out of memory while finalizing bitmap aggregate");
            return;
        }

        duckdb_vector_assign_string_element_len(result, offset + i, (const char *)blob_bytes, blob_size);
        free(blob_bytes);
        if (created_empty) {
            roaring_bitmap_free(rb);
        }
    }
}

static void BitmapCountAndAggFinalize(duckdb_function_info info, duckdb_aggregate_state *source, duckdb_vector result,
                                      idx_t count, idx_t offset) {
    uint64_t *out_data = (uint64_t *)duckdb_vector_get_data(result);

    for (idx_t i = 0; i < count; i++) {
        BitmapAggState *state = (BitmapAggState *)source[i];
        if (state->failed) {
            duckdb_aggregate_function_set_error(info, "bitmap aggregate state is failed");
            return;
        }

        out_data[offset + i] = state->rb == NULL ? 0 : roaring_bitmap_get_cardinality(state->rb);
    }
}

static void RegisterBitmapHello(duckdb_connection connection) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "bitmap_hello");

    duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_scalar_function_add_parameter(function, varchar_type);
    duckdb_scalar_function_set_return_type(function, varchar_type);
    duckdb_destroy_logical_type(&varchar_type);

    duckdb_scalar_function_set_function(function, BitmapHello);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterBinaryBitmapFunction(duckdb_connection connection, const char *name, void (*function_ptr)(duckdb_function_info,
                                                                                                            duckdb_data_chunk,
                                                                                                            duckdb_vector)) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, name);

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_scalar_function_set_return_type(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_scalar_function_set_function(function, function_ptr);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterUnaryBitmapFunction(duckdb_connection connection, const char *name, void (*function_ptr)(duckdb_function_info,
                                                                                                        duckdb_data_chunk,
                                                                                                        duckdb_vector)) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, name);

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_scalar_function_set_return_type(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_scalar_function_set_function(function, function_ptr);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterCountBitmapFunction(duckdb_connection connection) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "bm_count");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_scalar_function_set_return_type(function, ubigint_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_scalar_function_set_function(function, BitmapCountFunction);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterBinaryCountBitmapFunction(duckdb_connection connection, const char *name,
                                              void (*function_ptr)(duckdb_function_info, duckdb_data_chunk,
                                                                   duckdb_vector)) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, name);

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_scalar_function_set_return_type(function, ubigint_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_scalar_function_set_function(function, function_ptr);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterIntersectsBitmapFunction(duckdb_connection connection) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "bm_intersects");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type boolean_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
    duckdb_scalar_function_set_return_type(function, boolean_type);
    duckdb_destroy_logical_type(&boolean_type);

    duckdb_scalar_function_set_function(function, BitmapIntersectsFunction);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterContainsBitmapFunction(duckdb_connection connection) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "bm_contains");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_scalar_function_add_parameter(function, ubigint_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_logical_type boolean_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);
    duckdb_scalar_function_set_return_type(function, boolean_type);
    duckdb_destroy_logical_type(&boolean_type);

    duckdb_scalar_function_set_function(function, BitmapContainsFunction);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterBitmapVarcharFunction(duckdb_connection connection, const char *name,
                                          void (*function_ptr)(duckdb_function_info, duckdb_data_chunk,
                                                               duckdb_vector)) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, name);

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_scalar_function_set_return_type(function, varchar_type);
    duckdb_destroy_logical_type(&varchar_type);

    duckdb_scalar_function_set_function(function, function_ptr);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterToRowsBitmapFunction(duckdb_connection connection) {
    duckdb_scalar_function_set set = duckdb_create_scalar_function_set("bm_to_rows");

    duckdb_scalar_function full_function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(full_function, "bm_to_rows");
    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(full_function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_logical_type list_type = duckdb_create_list_type(ubigint_type);
    duckdb_scalar_function_set_return_type(full_function, list_type);
    duckdb_destroy_logical_type(&list_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_scalar_function_set_function(full_function, BitmapToRowsFunction);
    if (duckdb_add_scalar_function_to_set(set, full_function) == DuckDBError) {
        duckdb_destroy_scalar_function(&full_function);
        duckdb_destroy_scalar_function_set(&set);
        return;
    }

    duckdb_scalar_function limited_function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(limited_function, "bm_to_rows");
    blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(limited_function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_scalar_function_add_parameter(limited_function, ubigint_type);

    duckdb_logical_type bigint_type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    duckdb_scalar_function_add_parameter(limited_function, bigint_type);
    duckdb_destroy_logical_type(&bigint_type);

    list_type = duckdb_create_list_type(ubigint_type);
    duckdb_scalar_function_set_return_type(limited_function, list_type);
    duckdb_destroy_logical_type(&list_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_scalar_function_set_function(limited_function, BitmapToRowsLimitedFunction);
    if (duckdb_add_scalar_function_to_set(set, limited_function) == DuckDBError) {
        duckdb_destroy_scalar_function(&full_function);
        duckdb_destroy_scalar_function(&limited_function);
        duckdb_destroy_scalar_function_set(&set);
        return;
    }

    if (duckdb_register_scalar_function_set(connection, set) == DuckDBError) {
        duckdb_destroy_scalar_function(&full_function);
        duckdb_destroy_scalar_function(&limited_function);
        duckdb_destroy_scalar_function_set(&set);
        return;
    }

    duckdb_destroy_scalar_function(&full_function);
    duckdb_destroy_scalar_function(&limited_function);
    duckdb_destroy_scalar_function_set(&set);
}

static void RegisterBuildBitmapFunction(duckdb_connection connection) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "bm_build");

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_logical_type ubigint_list_type = duckdb_create_list_type(ubigint_type);
    duckdb_scalar_function_add_parameter(function, ubigint_list_type);
    duckdb_destroy_logical_type(&ubigint_list_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_set_return_type(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_scalar_function_set_function(function, BitmapBuildFunction);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
}

static void RegisterBuildAggBitmapFunction(duckdb_connection connection) {
    duckdb_aggregate_function function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, "bm_build_agg");

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_aggregate_function_add_parameter(function, ubigint_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_aggregate_function_set_return_type(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_aggregate_function_set_functions(function, BitmapAggStateSize, BitmapAggInit, BitmapBuildAggUpdate,
                                            BitmapAggCombine, BitmapAggFinalize);
    duckdb_aggregate_function_set_destructor(function, BitmapAggDestroy);
    duckdb_aggregate_function_set_special_handling(function);

    if (duckdb_register_aggregate_function(connection, function) == DuckDBError) {
        duckdb_destroy_aggregate_function(&function);
        return;
    }

    duckdb_destroy_aggregate_function(&function);
}

static void RegisterOrAggBitmapFunction(duckdb_connection connection) {
    duckdb_aggregate_function function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, "bm_or_agg");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_aggregate_function_add_parameter(function, blob_type);
    duckdb_aggregate_function_set_return_type(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_aggregate_function_set_functions(function, BitmapAggStateSize, BitmapAggInit, BitmapOrAggUpdate,
                                            BitmapOrAggCombine, BitmapAggFinalize);
    duckdb_aggregate_function_set_destructor(function, BitmapAggDestroy);
    duckdb_aggregate_function_set_special_handling(function);

    if (duckdb_register_aggregate_function(connection, function) == DuckDBError) {
        duckdb_destroy_aggregate_function(&function);
        return;
    }

    duckdb_destroy_aggregate_function(&function);
}

static void RegisterAndAggBitmapFunction(duckdb_connection connection) {
    duckdb_aggregate_function function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, "bm_and_agg");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_aggregate_function_add_parameter(function, blob_type);
    duckdb_aggregate_function_set_return_type(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_aggregate_function_set_functions(function, BitmapAggStateSize, BitmapAggInit, BitmapAndAggUpdate,
                                            BitmapAndAggCombine, BitmapAggFinalize);
    duckdb_aggregate_function_set_destructor(function, BitmapAggDestroy);
    duckdb_aggregate_function_set_special_handling(function);

    if (duckdb_register_aggregate_function(connection, function) == DuckDBError) {
        duckdb_destroy_aggregate_function(&function);
        return;
    }

    duckdb_destroy_aggregate_function(&function);
}

static void RegisterCountAndAggBitmapFunction(duckdb_connection connection) {
    duckdb_aggregate_function function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, "bm_count_and_agg");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_aggregate_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_aggregate_function_set_return_type(function, ubigint_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_aggregate_function_set_functions(function, BitmapAggStateSize, BitmapAggInit, BitmapAndAggUpdate,
                                            BitmapAndAggCombine, BitmapCountAndAggFinalize);
    duckdb_aggregate_function_set_destructor(function, BitmapAggDestroy);
    duckdb_aggregate_function_set_special_handling(function);

    if (duckdb_register_aggregate_function(connection, function) == DuckDBError) {
        duckdb_destroy_aggregate_function(&function);
        return;
    }

    duckdb_destroy_aggregate_function(&function);
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
    (void)info;
    (void)access;
    RegisterBitmapHello(connection);
    RegisterBinaryBitmapFunction(connection, "bm_or", BitmapOrFunction);
    RegisterBinaryBitmapFunction(connection, "bm_and", BitmapAndFunction);
    RegisterBinaryBitmapFunction(connection, "bm_andnot", BitmapAndNotFunction);
    RegisterCountBitmapFunction(connection);
    RegisterBinaryCountBitmapFunction(connection, "bm_count_or", BitmapCountOrFunction);
    RegisterBinaryCountBitmapFunction(connection, "bm_count_and", BitmapCountAndFunction);
    RegisterBinaryCountBitmapFunction(connection, "bm_count_andnot", BitmapCountAndNotFunction);
    RegisterIntersectsBitmapFunction(connection);
    RegisterContainsBitmapFunction(connection);
    RegisterBitmapVarcharFunction(connection, "bm_format", BitmapFormatFunction);
    RegisterBitmapVarcharFunction(connection, "bm_stats", BitmapStatsFunction);
    RegisterUnaryBitmapFunction(connection, "bm_reencode_roaring", BitmapReencodeRoaringFunction);
    RegisterToRowsBitmapFunction(connection);
    RegisterBuildBitmapFunction(connection);
    RegisterBuildAggBitmapFunction(connection);
    RegisterOrAggBitmapFunction(connection);
    RegisterAndAggBitmapFunction(connection);
    RegisterCountAndAggBitmapFunction(connection);
    return true;
}
