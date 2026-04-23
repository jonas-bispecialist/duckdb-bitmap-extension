#include "duckdb_extension.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

// Bitmap scalars use an FPBM envelope with sorted unique uint32 payloads.

#define BITMAP_MAGIC_0 'F'
#define BITMAP_MAGIC_1 'P'
#define BITMAP_MAGIC_2 'B'
#define BITMAP_MAGIC_3 'M'
#define BITMAP_FORMAT_VERSION 1
#define BITMAP_ENCODING_SORTED_U32 1
#define BITMAP_HEADER_SIZE 12

typedef struct BitmapView {
    const uint8_t *payload;
    idx_t count;
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
    if (data[5] != BITMAP_ENCODING_SORTED_U32) {
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
    if ((payload_len % 4U) != 0U) {
        SetFunctionError(info, "invalid bitmap blob: payload length must be a multiple of 4");
        return false;
    }
    out->payload = data + BITMAP_HEADER_SIZE;
    out->count = (idx_t)(payload_len / 4U);
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

static bool MergeOr(const BitmapView *lhs, const BitmapView *rhs, uint32_t *out, idx_t *out_count) {
    idx_t i = 0;
    idx_t j = 0;
    idx_t k = 0;
    while (i < lhs->count && j < rhs->count) {
        uint32_t lhs_value = BitmapViewValueAt(lhs, i);
        uint32_t rhs_value = BitmapViewValueAt(rhs, j);
        if (lhs_value < rhs_value) {
            out[k++] = lhs_value;
            i++;
        } else if (rhs_value < lhs_value) {
            out[k++] = rhs_value;
            j++;
        } else {
            out[k++] = lhs_value;
            i++;
            j++;
        }
    }
    while (i < lhs->count) {
        out[k++] = BitmapViewValueAt(lhs, i);
        i++;
    }
    while (j < rhs->count) {
        out[k++] = BitmapViewValueAt(rhs, j);
        j++;
    }
    *out_count = k;
    return true;
}

static bool MergeAnd(const BitmapView *lhs, const BitmapView *rhs, uint32_t *out, idx_t *out_count) {
    idx_t i = 0;
    idx_t j = 0;
    idx_t k = 0;
    while (i < lhs->count && j < rhs->count) {
        uint32_t lhs_value = BitmapViewValueAt(lhs, i);
        uint32_t rhs_value = BitmapViewValueAt(rhs, j);
        if (lhs_value < rhs_value) {
            i++;
        } else if (rhs_value < lhs_value) {
            j++;
        } else {
            out[k++] = lhs_value;
            i++;
            j++;
        }
    }
    *out_count = k;
    return true;
}

static bool MergeAndNot(const BitmapView *lhs, const BitmapView *rhs, uint32_t *out, idx_t *out_count) {
    idx_t i = 0;
    idx_t j = 0;
    idx_t k = 0;
    while (i < lhs->count && j < rhs->count) {
        uint32_t lhs_value = BitmapViewValueAt(lhs, i);
        uint32_t rhs_value = BitmapViewValueAt(rhs, j);
        if (lhs_value < rhs_value) {
            out[k++] = lhs_value;
            i++;
        } else if (rhs_value < lhs_value) {
            j++;
        } else {
            i++;
            j++;
        }
    }
    while (i < lhs->count) {
        out[k++] = BitmapViewValueAt(lhs, i);
        i++;
    }
    *out_count = k;
    return true;
}

static bool ApplyBinaryOp(const BitmapView *lhs, const BitmapView *rhs, BitmapBinaryOp op, uint32_t **out_values,
                          idx_t *out_count) {
    if (lhs->count > (idx_t)(SIZE_MAX - rhs->count)) {
        return false;
    }
    idx_t max_count = lhs->count + rhs->count;
    uint32_t *values = NULL;
    if (max_count > 0) {
        if (max_count > (idx_t)(SIZE_MAX / sizeof(uint32_t))) {
            return false;
        }
        values = (uint32_t *)malloc((size_t)max_count * sizeof(uint32_t));
        if (values == NULL) {
            return false;
        }
    }

    bool ok = true;
    switch (op) {
    case BITMAP_OP_OR:
        ok = MergeOr(lhs, rhs, values, out_count);
        break;
    case BITMAP_OP_AND:
        ok = MergeAnd(lhs, rhs, values, out_count);
        break;
    case BITMAP_OP_ANDNOT:
        ok = MergeAndNot(lhs, rhs, values, out_count);
        break;
    }

    if (!ok) {
        free(values);
        return false;
    }

    *out_values = values;
    return true;
}

static bool BitmapContainsValue(const BitmapView *view, uint64_t needle) {
    if (needle > UINT32_MAX) {
        return false;
    }

    uint32_t target = (uint32_t)needle;
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

static int CompareUint32(const void *lhs, const void *rhs) {
    uint32_t a = *(const uint32_t *)lhs;
    uint32_t b = *(const uint32_t *)rhs;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

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
        uint32_t *result_values = NULL;
        idx_t result_count = 0;
        if (!ApplyBinaryOp(&lhs, &rhs, BITMAP_OP_OR, &result_values, &result_count)) {
            SetFunctionError(info, "out of memory while computing bitmap union");
            return;
        }

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeBitmapBlobBytes(result_values, result_count, &blob_bytes, &blob_size)) {
            free(result_values);
            SetFunctionError(info, "out of memory while serializing bitmap union");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        free(result_values);
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
        uint32_t *result_values = NULL;
        idx_t result_count = 0;
        if (!ApplyBinaryOp(&lhs, &rhs, BITMAP_OP_AND, &result_values, &result_count)) {
            SetFunctionError(info, "out of memory while computing bitmap intersection");
            return;
        }

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeBitmapBlobBytes(result_values, result_count, &blob_bytes, &blob_size)) {
            free(result_values);
            SetFunctionError(info, "out of memory while serializing bitmap intersection");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        free(result_values);
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
        uint32_t *result_values = NULL;
        idx_t result_count = 0;
        if (!ApplyBinaryOp(&lhs, &rhs, BITMAP_OP_ANDNOT, &result_values, &result_count)) {
            SetFunctionError(info, "out of memory while computing bitmap difference");
            return;
        }

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeBitmapBlobBytes(result_values, result_count, &blob_bytes, &blob_size)) {
            free(result_values);
            SetFunctionError(info, "out of memory while serializing bitmap difference");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        free(result_values);
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
        out_data[row] = (uint64_t)bitmap.count;
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
        if (total_rows > (idx_t)(SIZE_MAX - bitmap.count)) {
            SetFunctionError(info, "bitmap row output exceeds supported size");
            return;
        }
        total_rows += bitmap.count;
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
        out_entries[row].offset = (uint64_t)child_offset;
        out_entries[row].length = (uint64_t)bitmap.count;
        for (idx_t i = 0; i < bitmap.count; i++) {
            child_data[child_offset + i] = (uint64_t)BitmapViewValueAt(&bitmap, i);
        }
        child_offset += bitmap.count;
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
        if (length > (idx_t)(SIZE_MAX / sizeof(uint32_t))) {
            SetFunctionError(info, "bm_build input list is too large");
            return;
        }

        uint32_t *values = NULL;
        if (length > 0) {
            values = (uint32_t *)malloc((size_t)length * sizeof(uint32_t));
            if (values == NULL) {
                SetFunctionError(info, "out of memory while building bitmap");
                return;
            }
        }

        for (idx_t i = 0; i < length; i++) {
            idx_t child_index = offset + i;
            if (!RowIsValid(child_validity, child_index)) {
                free(values);
                SetFunctionError(info, "bm_build does not accept NULL row ids");
                return;
            }

            uint64_t row_id = child_data[child_index];
            if (row_id > UINT32_MAX) {
                free(values);
                SetFunctionError(info, "bm_build row id exceeds UINT32 range");
                return;
            }
            values[i] = (uint32_t)row_id;
        }

        if (length > 1) {
            qsort(values, (size_t)length, sizeof(uint32_t), CompareUint32);
        }

        idx_t unique_count = 0;
        for (idx_t i = 0; i < length; i++) {
            if (i == 0 || values[i] != values[i - 1]) {
                values[unique_count++] = values[i];
            }
        }

        uint8_t *blob_bytes = NULL;
        idx_t blob_size = 0;
        if (!MakeBitmapBlobBytes(values, unique_count, &blob_bytes, &blob_size)) {
            free(values);
            SetFunctionError(info, "out of memory while serializing bm_build output");
            return;
        }

        duckdb_vector_assign_string_element_len(output, row, (const char *)blob_bytes, blob_size);

        free(blob_bytes);
        free(values);
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

static void RegisterToRowsBitmapFunction(duckdb_connection connection) {
    duckdb_scalar_function function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, "bm_to_rows");

    duckdb_logical_type blob_type = duckdb_create_logical_type(DUCKDB_TYPE_BLOB);
    duckdb_scalar_function_add_parameter(function, blob_type);
    duckdb_destroy_logical_type(&blob_type);

    duckdb_logical_type ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_logical_type list_type = duckdb_create_list_type(ubigint_type);
    duckdb_scalar_function_set_return_type(function, list_type);
    duckdb_destroy_logical_type(&list_type);
    duckdb_destroy_logical_type(&ubigint_type);

    duckdb_scalar_function_set_function(function, BitmapToRowsFunction);

    if (duckdb_register_scalar_function(connection, function) == DuckDBError) {
        duckdb_destroy_scalar_function(&function);
        return;
    }

    duckdb_destroy_scalar_function(&function);
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

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
    (void)info;
    (void)access;
    RegisterBitmapHello(connection);
    RegisterBinaryBitmapFunction(connection, "bm_or", BitmapOrFunction);
    RegisterBinaryBitmapFunction(connection, "bm_and", BitmapAndFunction);
    RegisterBinaryBitmapFunction(connection, "bm_andnot", BitmapAndNotFunction);
    RegisterCountBitmapFunction(connection);
    RegisterContainsBitmapFunction(connection);
    RegisterToRowsBitmapFunction(connection);
    RegisterBuildBitmapFunction(connection);
    return true;
}
