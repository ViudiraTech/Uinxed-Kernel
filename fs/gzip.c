/*
 *
 *      gzip.c
 *      Gzip/DEFLATE decompression
 *
 */

#include <fs/gzip.h>
#include <kernel/errno.h>
#include <mem/alloc.h>

#define DEFLATE_MAX_BITS    15
#define DEFLATE_MAX_SYMBOLS 288

typedef struct {
        const uint8_t *data;
        size_t         size;
        size_t         offset;
        uint32_t       bits;
        unsigned int   bit_count;
} deflate_stream_t;

typedef struct {
        uint16_t counts[DEFLATE_MAX_BITS + 1];
        uint16_t symbols[DEFLATE_MAX_SYMBOLS];
} deflate_huffman_t;

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};

static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static uint32_t load_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int read_bits(deflate_stream_t *stream, unsigned int count, uint32_t *value)
{
    while (stream->bit_count < count) {
        if (stream->offset >= stream->size) return -EINVAL;
        stream->bits |= (uint32_t)stream->data[stream->offset++] << stream->bit_count;
        stream->bit_count += 8;
    }

    *value = stream->bits & ((1U << count) - 1U);
    stream->bits >>= count;
    stream->bit_count -= count;
    return EOK;
}

static void align_to_byte(deflate_stream_t *stream)
{
    unsigned int discard = stream->bit_count & 7U;
    stream->bits >>= discard;
    stream->bit_count -= discard;
}

static int build_huffman(deflate_huffman_t *tree, const uint8_t *lengths, size_t symbol_count)
{
    uint16_t offsets[DEFLATE_MAX_BITS + 1];
    int      remaining = 1;

    for (size_t i = 0; i <= DEFLATE_MAX_BITS; i++) tree->counts[i] = 0;
    for (size_t i = 0; i < symbol_count; i++) {
        if (lengths[i] > DEFLATE_MAX_BITS) return -EINVAL;
        tree->counts[lengths[i]]++;
    }
    if (tree->counts[0] == symbol_count) return -EINVAL;

    for (size_t bits = 1; bits <= DEFLATE_MAX_BITS; bits++) {
        remaining = (remaining << 1) - tree->counts[bits];
        if (remaining < 0) return -EINVAL;
    }

    offsets[1] = 0;
    for (size_t bits = 1; bits < DEFLATE_MAX_BITS; bits++) offsets[bits + 1] = offsets[bits] + tree->counts[bits];
    for (size_t symbol = 0; symbol < symbol_count; symbol++) {
        uint8_t length = lengths[symbol];
        if (length) tree->symbols[offsets[length]++] = (uint16_t)symbol;
    }
    return EOK;
}

static int decode_symbol(deflate_stream_t *stream, const deflate_huffman_t *tree, uint16_t *symbol)
{
    unsigned int code  = 0;
    unsigned int first = 0;
    unsigned int index = 0;

    for (size_t length = 1; length <= DEFLATE_MAX_BITS; length++) {
        uint32_t bit;
        if (read_bits(stream, 1, &bit) != EOK) return -EINVAL;
        code |= bit;

        unsigned int count = tree->counts[length];
        if (code < first + count) {
            *symbol = tree->symbols[index + code - first];
            return EOK;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -EINVAL;
}

static int build_fixed_trees(deflate_huffman_t *literal_tree, deflate_huffman_t *distance_tree)
{
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];

    for (size_t i = 0; i <= 143; i++) literal_lengths[i] = 8;
    for (size_t i = 144; i <= 255; i++) literal_lengths[i] = 9;
    for (size_t i = 256; i <= 279; i++) literal_lengths[i] = 7;
    for (size_t i = 280; i <= 287; i++) literal_lengths[i] = 8;
    for (size_t i = 0; i < 32; i++) distance_lengths[i] = 5;

    if (build_huffman(literal_tree, literal_lengths, 288) != EOK) return -EINVAL;
    return build_huffman(distance_tree, distance_lengths, 32);
}

static int build_dynamic_trees(deflate_stream_t *stream, deflate_huffman_t *literal_tree, deflate_huffman_t *distance_tree)
{
    static const uint8_t code_order[19]   = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t              code_lengths[19] = {0};
    uint8_t              lengths[286 + 32];
    deflate_huffman_t    code_tree;
    uint32_t             value;

    if (read_bits(stream, 5, &value) != EOK) return -EINVAL;
    size_t literal_count = value + 257;
    if (read_bits(stream, 5, &value) != EOK) return -EINVAL;
    size_t distance_count = value + 1;
    if (read_bits(stream, 4, &value) != EOK) return -EINVAL;
    size_t code_count = value + 4;
    if (literal_count > 286 || distance_count > 32) return -EINVAL;

    for (size_t i = 0; i < code_count; i++) {
        if (read_bits(stream, 3, &value) != EOK) return -EINVAL;
        code_lengths[code_order[i]] = (uint8_t)value;
    }
    if (build_huffman(&code_tree, code_lengths, 19) != EOK) return -EINVAL;

    size_t total = literal_count + distance_count;
    size_t index = 0;
    while (index < total) {
        uint16_t symbol;
        if (decode_symbol(stream, &code_tree, &symbol) != EOK) return -EINVAL;

        if (symbol <= 15) {
            lengths[index++] = (uint8_t)symbol;
            continue;
        }

        size_t  repeat;
        uint8_t repeated_length;
        if (symbol == 16) {
            if (!index || read_bits(stream, 2, &value) != EOK) return -EINVAL;
            repeat          = value + 3;
            repeated_length = lengths[index - 1];
        } else if (symbol == 17) {
            if (read_bits(stream, 3, &value) != EOK) return -EINVAL;
            repeat          = value + 3;
            repeated_length = 0;
        } else if (symbol == 18) {
            if (read_bits(stream, 7, &value) != EOK) return -EINVAL;
            repeat          = value + 11;
            repeated_length = 0;
        } else {
            return -EINVAL;
        }

        if (repeat > total - index) return -EINVAL;
        while (repeat--) lengths[index++] = repeated_length;
    }

    if (!lengths[256]) return -EINVAL;
    if (build_huffman(literal_tree, lengths, literal_count) != EOK) return -EINVAL;
    return build_huffman(distance_tree, lengths + literal_count, distance_count);
}

static int inflate_compressed_block(deflate_stream_t *stream, uint8_t *output, size_t output_capacity, size_t *output_offset,
                                    const deflate_huffman_t *literal_tree, const deflate_huffman_t *distance_tree)
{
    while (1) {
        uint16_t symbol;
        uint32_t extra;
        if (decode_symbol(stream, literal_tree, &symbol) != EOK) return -EINVAL;

        if (symbol < 256) {
            if (*output_offset >= output_capacity) return -EOVERFLOW;
            output[(*output_offset)++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256) return EOK;
        if (symbol < 257 || symbol > 285) return -EINVAL;

        size_t length_index = symbol - 257;
        size_t length       = length_base[length_index];
        if (length_extra[length_index]) {
            if (read_bits(stream, length_extra[length_index], &extra) != EOK) return -EINVAL;
            length += extra;
        }

        if (decode_symbol(stream, distance_tree, &symbol) != EOK || symbol >= 30) return -EINVAL;
        size_t distance = distance_base[symbol];
        if (distance_extra[symbol]) {
            if (read_bits(stream, distance_extra[symbol], &extra) != EOK) return -EINVAL;
            distance += extra;
        }

        if (!distance || distance > *output_offset || length > output_capacity - *output_offset) return -EOVERFLOW;
        for (size_t i = 0; i < length; i++) {
            output[*output_offset] = output[*output_offset - distance];
            (*output_offset)++;
        }
    }
}

static int inflate_stored_block(deflate_stream_t *stream, uint8_t *output, size_t output_capacity, size_t *output_offset)
{
    uint32_t length;
    uint32_t inverted_length;
    align_to_byte(stream);
    if (read_bits(stream, 16, &length) != EOK || read_bits(stream, 16, &inverted_length) != EOK) return -EINVAL;
    if ((length ^ 0xffffU) != inverted_length || length > output_capacity - *output_offset) return -EINVAL;

    for (size_t i = 0; i < length; i++) {
        uint32_t byte;
        if (read_bits(stream, 8, &byte) != EOK) return -EINVAL;
        output[(*output_offset)++] = (uint8_t)byte;
    }
    return EOK;
}

static int inflate_data(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_capacity, size_t *output_size)
{
    deflate_stream_t stream  = {.data = input, .size = input_size};
    size_t           written = 0;
    uint32_t         final;

    do {
        uint32_t type;
        if (read_bits(&stream, 1, &final) != EOK || read_bits(&stream, 2, &type) != EOK) return -EINVAL;

        if (type == 0) {
            if (inflate_stored_block(&stream, output, output_capacity, &written) != EOK) return -EINVAL;
        } else if (type == 1 || type == 2) {
            deflate_huffman_t literal_tree;
            deflate_huffman_t distance_tree;
            int               status
                = type == 1 ? build_fixed_trees(&literal_tree, &distance_tree) : build_dynamic_trees(&stream, &literal_tree, &distance_tree);
            if (status != EOK) return status;
            status = inflate_compressed_block(&stream, output, output_capacity, &written, &literal_tree, &distance_tree);
            if (status != EOK) return status;
        } else {
            return -EINVAL;
        }
    } while (!final);

    *output_size = written;
    return EOK;
}

static uint32_t gzip_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (size_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}

static int skip_zero_terminated_field(const uint8_t *input, size_t limit, size_t *offset)
{
    while (*offset < limit && input[*offset]) (*offset)++;
    if (*offset >= limit) return -EINVAL;
    (*offset)++;
    return EOK;
}

int gzip_decompress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size)
{
    if (!input || !output || !output_size || input_size < 18) return -EINVAL;
    *output      = 0;
    *output_size = 0;

    if (input[0] != 0x1f || input[1] != 0x8b || input[2] != 8 || (input[3] & 0xe0)) return -EINVAL;

    size_t  offset  = 10;
    size_t  trailer = input_size - 8;
    uint8_t flags   = input[3];
    if (flags & 0x04) {
        if (offset + 2 > trailer) return -EINVAL;
        size_t extra_size = (size_t)input[offset] | ((size_t)input[offset + 1] << 8);
        offset += 2;
        if (extra_size > trailer - offset) return -EINVAL;
        offset += extra_size;
    }
    if ((flags & 0x08) && skip_zero_terminated_field(input, trailer, &offset) != EOK) return -EINVAL;
    if ((flags & 0x10) && skip_zero_terminated_field(input, trailer, &offset) != EOK) return -EINVAL;
    if (flags & 0x02) {
        if (offset + 2 > trailer) return -EINVAL;
        offset += 2;
    }
    if (offset >= trailer) return -EINVAL;

    size_t   expected_size = load_le32(input + input_size - 4);
    uint8_t *result        = malloc(expected_size ? expected_size : 1);
    if (!result) return -ENOMEM;

    size_t actual_size;
    int    status = inflate_data(input + offset, trailer - offset, result, expected_size, &actual_size);
    if (status != EOK || actual_size != expected_size || gzip_crc32(result, actual_size) != load_le32(input + trailer)) {
        free(result);
        return status != EOK ? status : -EINVAL;
    }

    *output      = result;
    *output_size = actual_size;
    return EOK;
}
