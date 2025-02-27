/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <AK/Try.h>
#include <AK/Vector.h>
#include <LibGfx/JPGLoader.h>

#define JPG_INVALID 0X0000

#define JPG_APPN0 0XFFE0
#define JPG_APPN1 0XFFE1
#define JPG_APPN2 0XFFE2
#define JPG_APPN3 0XFFE3
#define JPG_APPN4 0XFFE4
#define JPG_APPN5 0XFFE5
#define JPG_APPN6 0XFFE6
#define JPG_APPN7 0XFFE7
#define JPG_APPN8 0XFFE8
#define JPG_APPN9 0XFFE9
#define JPG_APPNA 0XFFEA
#define JPG_APPNB 0XFFEB
#define JPG_APPNC 0XFFEC
#define JPG_APPND 0XFFED
#define JPG_APPNE 0xFFEE
#define JPG_APPNF 0xFFEF

#define JPG_RESERVED1 0xFFF1
#define JPG_RESERVED2 0xFFF2
#define JPG_RESERVED3 0xFFF3
#define JPG_RESERVED4 0xFFF4
#define JPG_RESERVED5 0xFFF5
#define JPG_RESERVED6 0xFFF6
#define JPG_RESERVED7 0xFFF7
#define JPG_RESERVED8 0xFFF8
#define JPG_RESERVED9 0xFFF9
#define JPG_RESERVEDA 0xFFFA
#define JPG_RESERVEDB 0xFFFB
#define JPG_RESERVEDC 0xFFFC
#define JPG_RESERVEDD 0xFFFD

#define JPG_RST0 0xFFD0
#define JPG_RST1 0xFFD1
#define JPG_RST2 0xFFD2
#define JPG_RST3 0xFFD3
#define JPG_RST4 0xFFD4
#define JPG_RST5 0xFFD5
#define JPG_RST6 0xFFD6
#define JPG_RST7 0xFFD7

#define JPG_DHP 0xFFDE
#define JPG_EXP 0xFFDF

#define JPG_DHT 0XFFC4
#define JPG_DQT 0XFFDB
#define JPG_EOI 0xFFD9
#define JPG_RST 0XFFDD
#define JPG_SOF0 0XFFC0
#define JPG_SOF2 0xFFC2
#define JPG_SOI 0XFFD8
#define JPG_SOS 0XFFDA
#define JPG_COM 0xFFFE

namespace Gfx {

constexpr static u8 zigzag_map[64] {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

using Marker = u16;

/**
 * MCU means group of data units that are coded together. A data unit is an 8x8
 * block of component data. In interleaved scans, number of non-interleaved data
 * units of a component C is Ch * Cv, where Ch and Cv represent the horizontal &
 * vertical subsampling factors of the component, respectively. A MacroBlock is
 * an 8x8 block of RGB values before encoding, and 8x8 block of YCbCr values when
 * we're done decoding the huffman stream.
 */
struct Macroblock {
    union {
        i32 y[64] = { 0 };
        i32 r[64];
    };

    union {
        i32 cb[64] = { 0 };
        i32 g[64];
    };

    union {
        i32 cr[64] = { 0 };
        i32 b[64];
    };
};

struct MacroblockMeta {
    u32 total { 0 };
    u32 padded_total { 0 };
    u32 hcount { 0 };
    u32 vcount { 0 };
    u32 hpadded_count { 0 };
    u32 vpadded_count { 0 };
};

struct ComponentSpec {
    u8 id { 0 };
    u8 hsample_factor { 1 }; // Horizontal sampling factor.
    u8 vsample_factor { 1 }; // Vertical sampling factor.
    u8 ac_destination_id { 0 };
    u8 dc_destination_id { 0 };
    u8 qtable_id { 0 }; // Quantization table id.
};

struct StartOfFrame {

    // Of these, only the first 3 are in mainstream use, and refers to SOF0-2.
    enum class FrameType {
        Baseline_DCT = 0,
        Extended_Sequential_DCT = 1,
        Progressive_DCT = 2,
        Sequential_Lossless = 3,
        Differential_Sequential_DCT = 5,
        Differential_Progressive_DCT = 6,
        Differential_Sequential_Lossless = 7,
        Extended_Sequential_DCT_Arithmetic = 9,
        Progressive_DCT_Arithmetic = 10,
        Sequential_Lossless_Arithmetic = 11,
        Differential_Sequential_DCT_Arithmetic = 13,
        Differential_Progressive_DCT_Arithmetic = 14,
        Differential_Sequential_Lossless_Arithmetic = 15,
    };

    FrameType type { FrameType::Baseline_DCT };
    u8 precision { 0 };
    u16 height { 0 };
    u16 width { 0 };
};

struct HuffmanTableSpec {
    u8 type { 0 };
    u8 destination_id { 0 };
    u8 code_counts[16] = { 0 };
    Vector<u8> symbols;
    Vector<u16> codes;
};

struct HuffmanStreamState {
    Vector<u8> stream;
    u8 bit_offset { 0 };
    size_t byte_offset { 0 };
};

struct ICCMultiChunkState {
    u8 seen_number_of_icc_chunks { 0 };
    FixedArray<ByteBuffer> chunks;
};

struct JPGLoadingContext {
    enum State {
        NotDecoded = 0,
        Error,
        FrameDecoded,
        HeaderDecoded,
        BitmapDecoded
    };

    State state { State::NotDecoded };
    u8 const* data { nullptr };
    size_t data_size { 0 };
    u32 luma_table[64] = { 0 };
    u32 chroma_table[64] = { 0 };
    StartOfFrame frame;
    u8 hsample_factor { 0 };
    u8 vsample_factor { 0 };
    u8 component_count { 0 };
    Vector<ComponentSpec, 3> components;
    RefPtr<Gfx::Bitmap> bitmap;
    u16 dc_reset_interval { 0 };
    HashMap<u8, HuffmanTableSpec> dc_tables;
    HashMap<u8, HuffmanTableSpec> ac_tables;
    HuffmanStreamState huffman_stream;
    i32 previous_dc_values[3] = { 0 };
    MacroblockMeta mblock_meta;
    OwnPtr<FixedMemoryStream> stream;

    Optional<ICCMultiChunkState> icc_multi_chunk_state;
    Optional<ByteBuffer> icc_data;
};

static void generate_huffman_codes(HuffmanTableSpec& table)
{
    unsigned code = 0;
    for (auto number_of_codes : table.code_counts) {
        for (int i = 0; i < number_of_codes; i++)
            table.codes.append(code++);
        code <<= 1;
    }
}

static ErrorOr<size_t> read_huffman_bits(HuffmanStreamState& hstream, size_t count = 1)
{
    if (count > (8 * sizeof(size_t))) {
        dbgln_if(JPG_DEBUG, "Can't read {} bits at once!", count);
        return Error::from_string_literal("Reading too much huffman bits at once");
    }
    size_t value = 0;
    while (count--) {
        if (hstream.byte_offset >= hstream.stream.size()) {
            dbgln_if(JPG_DEBUG, "Huffman stream exhausted. This could be an error!");
            return Error::from_string_literal("Huffman stream exhausted.");
        }
        u8 current_byte = hstream.stream[hstream.byte_offset];
        u8 current_bit = 1u & (u32)(current_byte >> (7 - hstream.bit_offset)); // MSB first.
        hstream.bit_offset++;
        value = (value << 1) | (size_t)current_bit;
        if (hstream.bit_offset == 8) {
            hstream.byte_offset++;
            hstream.bit_offset = 0;
        }
    }
    return value;
}

static ErrorOr<u8> get_next_symbol(HuffmanStreamState& hstream, HuffmanTableSpec const& table)
{
    unsigned code = 0;
    size_t code_cursor = 0;
    for (int i = 0; i < 16; i++) { // Codes can't be longer than 16 bits.
        auto result = TRY(read_huffman_bits(hstream));
        code = (code << 1) | (i32)result;
        for (int j = 0; j < table.code_counts[i]; j++) {
            if (code == table.codes[code_cursor])
                return table.symbols[code_cursor];
            code_cursor++;
        }
    }

    dbgln_if(JPG_DEBUG, "If you're seeing this...the jpeg decoder needs to support more kinds of JPEGs!");
    return Error::from_string_literal("This kind of JPEG is not yet supported by the decoder");
}

static inline i32* get_component(Macroblock& block, unsigned component)
{
    switch (component) {
    case 0:
        return block.y;
    case 1:
        return block.cb;
    default:
        return block.cr;
    }
}

/**
 * Build the macroblocks possible by reading single (MCU) subsampled pair of CbCr.
 * Depending on the sampling factors, we may not see triples of y, cb, cr in that
 * order. If sample factors differ from one, we'll read more than one block of y-
 * coefficients before we get to read a cb-cr block.

 * In the function below, `hcursor` and `vcursor` denote the location of the block
 * we're building in the macroblock matrix. `vfactor_i` and `hfactor_i` are cursors
 * that iterate over the vertical and horizontal subsampling factors, respectively.
 * When we finish one iteration of the innermost loop, we'll have the coefficients
 * of one of the components of block at position `mb_index`. When the outermost loop
 * finishes first iteration, we'll have all the luminance coefficients for all the
 * macroblocks that share the chrominance data. Next two iterations (assuming that
 * we are dealing with three components) will fill up the blocks with chroma data.
 */
static ErrorOr<void> build_macroblocks(JPGLoadingContext& context, Vector<Macroblock>& macroblocks, u32 hcursor, u32 vcursor)
{
    for (unsigned component_i = 0; component_i < context.component_count; component_i++) {
        auto& component = context.components[component_i];

        if (component.dc_destination_id >= context.dc_tables.size())
            return Error::from_string_literal("DC destination ID is greater than number of DC tables");
        if (component.ac_destination_id >= context.ac_tables.size())
            return Error::from_string_literal("AC destination ID is greater than number of AC tables");

        for (u8 vfactor_i = 0; vfactor_i < component.vsample_factor; vfactor_i++) {
            for (u8 hfactor_i = 0; hfactor_i < component.hsample_factor; hfactor_i++) {
                u32 mb_index = (vcursor + vfactor_i) * context.mblock_meta.hpadded_count + (hfactor_i + hcursor);
                Macroblock& block = macroblocks[mb_index];

                auto& dc_table = context.dc_tables.find(component.dc_destination_id)->value;
                auto& ac_table = context.ac_tables.find(component.ac_destination_id)->value;

                // For DC coefficients, symbol encodes the length of the coefficient.
                auto dc_length = TRY(get_next_symbol(context.huffman_stream, dc_table));
                if (dc_length > 11) {
                    dbgln_if(JPG_DEBUG, "DC coefficient too long: {}!", dc_length);
                    return Error::from_string_literal("DC coefficient too long");
                }

                // DC coefficients are encoded as the difference between previous and current DC values.
                i32 dc_diff = TRY(read_huffman_bits(context.huffman_stream, dc_length));

                // If MSB in diff is 0, the difference is -ve. Otherwise +ve.
                if (dc_length != 0 && dc_diff < (1 << (dc_length - 1)))
                    dc_diff -= (1 << dc_length) - 1;

                auto select_component = get_component(block, component_i);
                auto& previous_dc = context.previous_dc_values[component_i];
                select_component[0] = previous_dc += dc_diff;

                // Compute the AC coefficients.
                for (int j = 1; j < 64;) {
                    // AC symbols encode 2 pieces of information, the high 4 bits represent
                    // number of zeroes to be stuffed before reading the coefficient. Low 4
                    // bits represent the magnitude of the coefficient.
                    auto ac_symbol = TRY(get_next_symbol(context.huffman_stream, ac_table));
                    if (ac_symbol == 0)
                        break;

                    // ac_symbol = 0xF0 means we need to skip 16 zeroes.
                    u8 run_length = ac_symbol == 0xF0 ? 16 : ac_symbol >> 4;
                    j += run_length;

                    if (j >= 64) {
                        dbgln_if(JPG_DEBUG, "Run-length exceeded boundaries. Cursor: {}, Skipping: {}!", j, run_length);
                        return Error::from_string_literal("Run-length exceeded boundaries");
                    }

                    u8 coeff_length = ac_symbol & 0x0F;
                    if (coeff_length > 10) {
                        dbgln_if(JPG_DEBUG, "AC coefficient too long: {}!", coeff_length);
                        return Error::from_string_literal("AC coefficient too long");
                    }

                    if (coeff_length != 0) {
                        i32 ac_coefficient = TRY(read_huffman_bits(context.huffman_stream, coeff_length));
                        if (ac_coefficient < (1 << (coeff_length - 1)))
                            ac_coefficient -= (1 << coeff_length) - 1;

                        select_component[zigzag_map[j++]] = ac_coefficient;
                    }
                }
            }
        }
    }

    return {};
}

static ErrorOr<Vector<Macroblock>> decode_huffman_stream(JPGLoadingContext& context)
{
    Vector<Macroblock> macroblocks;
    macroblocks.resize(context.mblock_meta.padded_total);

    if constexpr (JPG_DEBUG) {
        dbgln("Image width: {}", context.frame.width);
        dbgln("Image height: {}", context.frame.height);
        dbgln("Macroblocks in a row: {}", context.mblock_meta.hpadded_count);
        dbgln("Macroblocks in a column: {}", context.mblock_meta.vpadded_count);
        dbgln("Macroblock meta padded total: {}", context.mblock_meta.padded_total);
    }

    // Compute huffman codes for DC and AC tables.
    for (auto it = context.dc_tables.begin(); it != context.dc_tables.end(); ++it)
        generate_huffman_codes(it->value);

    for (auto it = context.ac_tables.begin(); it != context.ac_tables.end(); ++it)
        generate_huffman_codes(it->value);

    for (u32 vcursor = 0; vcursor < context.mblock_meta.vcount; vcursor += context.vsample_factor) {
        for (u32 hcursor = 0; hcursor < context.mblock_meta.hcount; hcursor += context.hsample_factor) {
            u32 i = vcursor * context.mblock_meta.hpadded_count + hcursor;
            if (context.dc_reset_interval > 0) {
                if (i % context.dc_reset_interval == 0) {
                    context.previous_dc_values[0] = 0;
                    context.previous_dc_values[1] = 0;
                    context.previous_dc_values[2] = 0;

                    // Restart markers are stored in byte boundaries. Advance the huffman stream cursor to
                    //  the 0th bit of the next byte.
                    if (context.huffman_stream.byte_offset < context.huffman_stream.stream.size()) {
                        if (context.huffman_stream.bit_offset > 0) {
                            context.huffman_stream.bit_offset = 0;
                            context.huffman_stream.byte_offset++;
                        }

                        // Skip the restart marker (RSTn).
                        context.huffman_stream.byte_offset++;
                    }
                }
            }

            if (auto result = build_macroblocks(context, macroblocks, hcursor, vcursor); result.is_error()) {
                if constexpr (JPG_DEBUG) {
                    dbgln("Failed to build Macroblock {}", i);
                    dbgln("Huffman stream byte offset {}", context.huffman_stream.byte_offset);
                    dbgln("Huffman stream bit offset {}", context.huffman_stream.bit_offset);
                }
                return result.release_error();
            }
        }
    }

    return macroblocks;
}

static inline ErrorOr<void> ensure_bounds_okay(const size_t cursor, const size_t delta, const size_t bound)
{
    if (Checked<size_t>::addition_would_overflow(delta, cursor))
        return Error::from_string_literal("Bounds are not ok: addition would overflow");
    if (delta + cursor >= bound)
        return Error::from_string_literal("Bounds are not ok");
    return {};
}

static inline bool is_valid_marker(const Marker marker)
{
    if (marker >= JPG_APPN0 && marker <= JPG_APPNF) {

        if (marker != JPG_APPN0)
            dbgln_if(JPG_DEBUG, "{:#04x} not supported yet. The decoder may fail!", marker);
        return true;
    }
    if (marker >= JPG_RESERVED1 && marker <= JPG_RESERVEDD)
        return true;
    if (marker >= JPG_RST0 && marker <= JPG_RST7)
        return true;
    switch (marker) {
    case JPG_COM:
    case JPG_DHP:
    case JPG_EXP:
    case JPG_DHT:
    case JPG_DQT:
    case JPG_RST:
    case JPG_SOF0:
    case JPG_SOI:
    case JPG_SOS:
        return true;
    }

    if (marker >= 0xFFC0 && marker <= 0xFFCF) {
        if (marker != 0xFFC4 && marker != 0xFFC8 && marker != 0xFFCC) {
            dbgln_if(JPG_DEBUG, "Decoding this frame-type (SOF{}) is not currently supported. Decoder will fail!", marker & 0xf);
            return false;
        }
    }

    return false;
}

static inline ErrorOr<Marker> read_marker_at_cursor(Stream& stream)
{
    u16 marker = TRY(stream.read_value<BigEndian<u16>>());
    if (is_valid_marker(marker))
        return marker;
    if (marker != 0xFFFF)
        return JPG_INVALID;
    u8 next;
    do {
        next = TRY(stream.read_value<u8>());
        if (next == 0x00)
            return JPG_INVALID;
    } while (next == 0xFF);
    marker = 0xFF00 | (u16)next;
    return is_valid_marker(marker) ? marker : JPG_INVALID;
}

static ErrorOr<void> read_start_of_scan(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    if (context.state < JPGLoadingContext::State::FrameDecoded) {
        dbgln_if(JPG_DEBUG, "{}: SOS found before reading a SOF!", TRY(stream.tell()));
        return Error::from_string_literal("SOS found before reading a SOF");
    }

    u16 bytes_to_read = TRY(stream.read_value<BigEndian<u16>>()) - 2;
    TRY(ensure_bounds_okay(TRY(stream.tell()), bytes_to_read, context.data_size));
    u8 component_count = TRY(stream.read_value<u8>());
    if (component_count != context.component_count) {
        dbgln_if(JPG_DEBUG, "{}: Unsupported number of components: {}!", TRY(stream.tell()), component_count);
        return Error::from_string_literal("Unsupported number of components");
    }

    for (int i = 0; i < component_count; i++) {
        u8 component_id = TRY(stream.read_value<u8>());

        auto& component = context.components[i];
        if (component.id != component_id) {
            dbgln("JPEG decode failed (component.id != component_id)");
            return Error::from_string_literal("JPEG decode failed (component.id != component_id)");
        }

        u8 table_ids = TRY(stream.read_value<u8>());

        component.dc_destination_id = table_ids >> 4;
        component.ac_destination_id = table_ids & 0x0F;

        if (context.dc_tables.size() != context.ac_tables.size()) {
            dbgln_if(JPG_DEBUG, "{}: DC & AC table count mismatch!", TRY(stream.tell()));
            return Error::from_string_literal("DC & AC table count mismatch");
        }

        if (!context.dc_tables.contains(component.dc_destination_id)) {
            dbgln_if(JPG_DEBUG, "DC table (id: {}) does not exist!", component.dc_destination_id);
            return Error::from_string_literal("DC table does not exist");
        }

        if (!context.ac_tables.contains(component.ac_destination_id)) {
            dbgln_if(JPG_DEBUG, "AC table (id: {}) does not exist!", component.ac_destination_id);
            return Error::from_string_literal("AC table does not exist");
        }
    }

    u8 spectral_selection_start = TRY(stream.read_value<u8>());
    u8 spectral_selection_end = TRY(stream.read_value<u8>());
    u8 successive_approximation = TRY(stream.read_value<u8>());

    // The three values should be fixed for baseline JPEGs utilizing sequential DCT.
    if (spectral_selection_start != 0 || spectral_selection_end != 63 || successive_approximation != 0) {
        dbgln_if(JPG_DEBUG, "{}: ERROR! Start of Selection: {}, End of Selection: {}, Successive Approximation: {}!",
            TRY(stream.tell()),
            spectral_selection_start,
            spectral_selection_end,
            successive_approximation);
        return Error::from_string_literal("Spectral selection is not [0,63] or successive approximation is not null");
    }
    return {};
}

static ErrorOr<void> read_reset_marker(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    u16 bytes_to_read = TRY(stream.read_value<BigEndian<u16>>()) - 2;
    if (bytes_to_read != 2) {
        dbgln_if(JPG_DEBUG, "{}: Malformed reset marker found!", TRY(stream.tell()));
        return Error::from_string_literal("Malformed reset marker found");
    }
    context.dc_reset_interval = TRY(stream.read_value<BigEndian<u16>>());
    return {};
}

static ErrorOr<void> read_huffman_table(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    i32 bytes_to_read = TRY(stream.read_value<BigEndian<u16>>());
    TRY(ensure_bounds_okay(TRY(stream.tell()), bytes_to_read, context.data_size));
    bytes_to_read -= 2;
    while (bytes_to_read > 0) {
        HuffmanTableSpec table;
        u8 table_info = TRY(stream.read_value<u8>());
        u8 table_type = table_info >> 4;
        u8 table_destination_id = table_info & 0x0F;
        if (table_type > 1) {
            dbgln_if(JPG_DEBUG, "{}: Unrecognized huffman table: {}!", TRY(stream.tell()), table_type);
            return Error::from_string_literal("Unrecognized huffman table");
        }
        if (table_destination_id > 1) {
            dbgln_if(JPG_DEBUG, "{}: Invalid huffman table destination id: {}!", TRY(stream.tell()), table_destination_id);
            return Error::from_string_literal("Invalid huffman table destination id");
        }

        table.type = table_type;
        table.destination_id = table_destination_id;
        u32 total_codes = 0;

        // Read code counts. At each index K, the value represents the number of K+1 bit codes in this header.
        for (int i = 0; i < 16; i++) {
            u8 count = TRY(stream.read_value<u8>());
            total_codes += count;
            table.code_counts[i] = count;
        }

        table.codes.ensure_capacity(total_codes);

        // Read symbols. Read X bytes, where X is the sum of the counts of codes read in the previous step.
        for (u32 i = 0; i < total_codes; i++) {
            u8 symbol = TRY(stream.read_value<u8>());
            table.symbols.append(symbol);
        }

        auto& huffman_table = table.type == 0 ? context.dc_tables : context.ac_tables;
        huffman_table.set(table.destination_id, table);
        VERIFY(huffman_table.size() <= 2);

        bytes_to_read -= 1 + 16 + total_codes;
    }

    if (bytes_to_read != 0) {
        dbgln_if(JPG_DEBUG, "{}: Extra bytes detected in huffman header!", TRY(stream.tell()));
        return Error::from_string_literal("Extra bytes detected in huffman header");
    }
    return {};
}

static ErrorOr<void> read_icc_profile(SeekableStream& stream, JPGLoadingContext& context, int bytes_to_read)
{
    if (bytes_to_read <= 2)
        return Error::from_string_literal("icc marker too small");

    auto chunk_sequence_number = TRY(stream.read_value<u8>()); // 1-based
    auto number_of_chunks = TRY(stream.read_value<u8>());
    bytes_to_read -= 2;

    if (!context.icc_multi_chunk_state.has_value())
        context.icc_multi_chunk_state.emplace(ICCMultiChunkState { 0, TRY(FixedArray<ByteBuffer>::create(number_of_chunks)) });
    auto& chunk_state = context.icc_multi_chunk_state;

    if (chunk_state->seen_number_of_icc_chunks >= number_of_chunks)
        return Error::from_string_literal("Too many ICC chunks");

    if (chunk_state->chunks.size() != number_of_chunks)
        return Error::from_string_literal("Inconsistent number of total ICC chunks");

    if (chunk_sequence_number == 0)
        return Error::from_string_literal("ICC chunk sequence number not 1 based");
    u8 index = chunk_sequence_number - 1;

    if (index >= chunk_state->chunks.size())
        return Error::from_string_literal("ICC chunk sequence number larger than number of chunks");

    if (!chunk_state->chunks[index].is_empty())
        return Error::from_string_literal("Duplicate ICC chunk at sequence number");

    chunk_state->chunks[index] = TRY(ByteBuffer::create_zeroed(bytes_to_read));
    TRY(stream.read_entire_buffer(chunk_state->chunks[index]));

    chunk_state->seen_number_of_icc_chunks++;

    if (chunk_state->seen_number_of_icc_chunks != chunk_state->chunks.size())
        return {};

    if (number_of_chunks == 1) {
        context.icc_data = move(chunk_state->chunks[0]);
        return {};
    }

    size_t total_size = 0;
    for (auto const& chunk : chunk_state->chunks)
        total_size += chunk.size();

    auto icc_bytes = TRY(ByteBuffer::create_zeroed(total_size));
    size_t start = 0;
    for (auto const& chunk : chunk_state->chunks) {
        memcpy(icc_bytes.data() + start, chunk.data(), chunk.size());
        start += chunk.size();
    }

    context.icc_data = move(icc_bytes);

    return {};
}

static ErrorOr<void> read_app_marker(SeekableStream& stream, JPGLoadingContext& context, int app_marker_number)
{
    i32 bytes_to_read = TRY(stream.read_value<BigEndian<u16>>());
    TRY(ensure_bounds_okay(TRY(stream.tell()), bytes_to_read, context.data_size));

    if (bytes_to_read <= 2)
        return Error::from_string_literal("app marker size too small");
    bytes_to_read -= 2;

    StringBuilder builder;
    for (;;) {
        if (bytes_to_read == 0)
            return Error::from_string_literal("app marker size too small for identifier");

        auto c = TRY(stream.read_value<char>());
        bytes_to_read--;

        if (c == '\0')
            break;

        TRY(builder.try_append(c));
    }

    auto app_id = TRY(builder.to_string());

    if (app_marker_number == 2 && app_id == "ICC_PROFILE"sv)
        return read_icc_profile(stream, context, bytes_to_read);

    return stream.discard(bytes_to_read);
}

static inline bool validate_luma_and_modify_context(ComponentSpec const& luma, JPGLoadingContext& context)
{
    if ((luma.hsample_factor == 1 || luma.hsample_factor == 2) && (luma.vsample_factor == 1 || luma.vsample_factor == 2)) {
        context.mblock_meta.hpadded_count += luma.hsample_factor == 1 ? 0 : context.mblock_meta.hcount % 2;
        context.mblock_meta.vpadded_count += luma.vsample_factor == 1 ? 0 : context.mblock_meta.vcount % 2;
        context.mblock_meta.padded_total = context.mblock_meta.hpadded_count * context.mblock_meta.vpadded_count;
        // For easy reference to relevant sample factors.
        context.hsample_factor = luma.hsample_factor;
        context.vsample_factor = luma.vsample_factor;

        if constexpr (JPG_DEBUG) {
            dbgln("Horizontal Subsampling Factor: {}", luma.hsample_factor);
            dbgln("Vertical Subsampling Factor: {}", luma.vsample_factor);
        }

        return true;
    }
    return false;
}

static inline void set_macroblock_metadata(JPGLoadingContext& context)
{
    context.mblock_meta.hcount = (context.frame.width + 7) / 8;
    context.mblock_meta.vcount = (context.frame.height + 7) / 8;
    context.mblock_meta.hpadded_count = context.mblock_meta.hcount;
    context.mblock_meta.vpadded_count = context.mblock_meta.vcount;
    context.mblock_meta.total = context.mblock_meta.hcount * context.mblock_meta.vcount;
}

static ErrorOr<void> read_start_of_frame(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    if (context.state == JPGLoadingContext::FrameDecoded) {
        dbgln_if(JPG_DEBUG, "{}: SOF repeated!", TRY(stream.tell()));
        return Error::from_string_literal("SOF repeated");
    }

    i32 bytes_to_read = TRY(stream.read_value<BigEndian<u16>>());

    bytes_to_read -= 2;
    TRY(ensure_bounds_okay(TRY(stream.tell()), bytes_to_read, context.data_size));

    context.frame.precision = TRY(stream.read_value<u8>());
    if (context.frame.precision != 8) {
        dbgln_if(JPG_DEBUG, "{}: SOF precision != 8!", TRY(stream.tell()));
        return Error::from_string_literal("SOF precision != 8");
    }

    context.frame.height = TRY(stream.read_value<BigEndian<u16>>());
    context.frame.width = TRY(stream.read_value<BigEndian<u16>>());
    if (!context.frame.width || !context.frame.height) {
        dbgln_if(JPG_DEBUG, "{}: ERROR! Image height: {}, Image width: {}!", TRY(stream.tell()), context.frame.height, context.frame.width);
        return Error::from_string_literal("Image frame height of width null");
    }

    if (context.frame.width > maximum_width_for_decoded_images || context.frame.height > maximum_height_for_decoded_images) {
        dbgln("This JPEG is too large for comfort: {}x{}", context.frame.width, context.frame.height);
        return Error::from_string_literal("JPEG too large for comfort");
    }

    set_macroblock_metadata(context);

    context.component_count = TRY(stream.read_value<u8>());
    if (context.component_count != 1 && context.component_count != 3) {
        dbgln_if(JPG_DEBUG, "{}: Unsupported number of components in SOF: {}!", TRY(stream.tell()), context.component_count);
        return Error::from_string_literal("Unsupported number of components in SOF");
    }

    for (u8 i = 0; i < context.component_count; i++) {
        ComponentSpec component;
        component.id = TRY(stream.read_value<u8>());

        u8 subsample_factors = TRY(stream.read_value<u8>());
        component.hsample_factor = subsample_factors >> 4;
        component.vsample_factor = subsample_factors & 0x0F;

        if (i == 0) {
            // If there is only a single component, i.e. grayscale, the macroblocks will not be interleaved, even if
            // the horizontal or vertical sample factor is larger than 1.
            if (context.component_count == 1) {
                component.hsample_factor = 1;
                component.vsample_factor = 1;
            }
            // By convention, downsampling is applied only on chroma components. So we should
            //  hope to see the maximum sampling factor in the luma component.
            if (!validate_luma_and_modify_context(component, context)) {
                dbgln_if(JPG_DEBUG, "{}: Unsupported luma subsampling factors: horizontal: {}, vertical: {}",
                    TRY(stream.tell()),
                    component.hsample_factor,
                    component.vsample_factor);
                return Error::from_string_literal("Unsupported luma subsampling factors");
            }
        } else {
            if (component.hsample_factor != 1 || component.vsample_factor != 1) {
                dbgln_if(JPG_DEBUG, "{}: Unsupported chroma subsampling factors: horizontal: {}, vertical: {}",
                    TRY(stream.tell()),
                    component.hsample_factor,
                    component.vsample_factor);
                return Error::from_string_literal("Unsupported chroma subsampling factors");
            }
        }

        component.qtable_id = TRY(stream.read_value<u8>());
        if (component.qtable_id > 1) {
            dbgln_if(JPG_DEBUG, "{}: Unsupported quantization table id: {}!", TRY(stream.tell()), component.qtable_id);
            return Error::from_string_literal("Unsupported quantization table id");
        }

        context.components.append(move(component));
    }

    return {};
}

static ErrorOr<void> read_quantization_table(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    i32 bytes_to_read = TRY(stream.read_value<BigEndian<u16>>()) - 2;
    TRY(ensure_bounds_okay(TRY(stream.tell()), bytes_to_read, context.data_size));
    while (bytes_to_read > 0) {
        u8 info_byte = TRY(stream.read_value<u8>());
        u8 element_unit_hint = info_byte >> 4;
        if (element_unit_hint > 1) {
            dbgln_if(JPG_DEBUG, "{}: Unsupported unit hint in quantization table: {}!", TRY(stream.tell()), element_unit_hint);
            return Error::from_string_literal("Unsupported unit hint in quantization table");
        }
        u8 table_id = info_byte & 0x0F;
        if (table_id > 1) {
            dbgln_if(JPG_DEBUG, "{}: Unsupported quantization table id: {}!", TRY(stream.tell()), table_id);
            return Error::from_string_literal("Unsupported quantization table id");
        }
        u32* table = table_id == 0 ? context.luma_table : context.chroma_table;
        for (int i = 0; i < 64; i++) {
            if (element_unit_hint == 0) {
                u8 tmp = TRY(stream.read_value<u8>());
                table[zigzag_map[i]] = tmp;
            } else {
                table[zigzag_map[i]] = TRY(stream.read_value<BigEndian<u16>>());
            }
        }

        bytes_to_read -= 1 + (element_unit_hint == 0 ? 64 : 128);
    }
    if (bytes_to_read != 0) {
        dbgln_if(JPG_DEBUG, "{}: Invalid length for one or more quantization tables!", TRY(stream.tell()));
        return Error::from_string_literal("Invalid length for one or more quantization tables");
    }

    return {};
}

static ErrorOr<void> skip_marker_with_length(Stream& stream)
{
    u16 bytes_to_skip = TRY(stream.read_value<BigEndian<u16>>()) - 2;
    TRY(stream.discard(bytes_to_skip));
    return {};
}

static void dequantize(JPGLoadingContext& context, Vector<Macroblock>& macroblocks)
{
    for (u32 vcursor = 0; vcursor < context.mblock_meta.vcount; vcursor += context.vsample_factor) {
        for (u32 hcursor = 0; hcursor < context.mblock_meta.hcount; hcursor += context.hsample_factor) {
            for (u32 i = 0; i < context.component_count; i++) {
                auto& component = context.components[i];
                u32 const* table = component.qtable_id == 0 ? context.luma_table : context.chroma_table;
                for (u32 vfactor_i = 0; vfactor_i < component.vsample_factor; vfactor_i++) {
                    for (u32 hfactor_i = 0; hfactor_i < component.hsample_factor; hfactor_i++) {
                        u32 mb_index = (vcursor + vfactor_i) * context.mblock_meta.hpadded_count + (hfactor_i + hcursor);
                        Macroblock& block = macroblocks[mb_index];
                        int* block_component = get_component(block, i);
                        for (u32 k = 0; k < 64; k++)
                            block_component[k] *= table[k];
                    }
                }
            }
        }
    }
}

static void inverse_dct(JPGLoadingContext const& context, Vector<Macroblock>& macroblocks)
{
    static float const m0 = 2.0f * AK::cos(1.0f / 16.0f * 2.0f * AK::Pi<float>);
    static float const m1 = 2.0f * AK::cos(2.0f / 16.0f * 2.0f * AK::Pi<float>);
    static float const m3 = 2.0f * AK::cos(2.0f / 16.0f * 2.0f * AK::Pi<float>);
    static float const m5 = 2.0f * AK::cos(3.0f / 16.0f * 2.0f * AK::Pi<float>);
    static float const m2 = m0 - m5;
    static float const m4 = m0 + m5;
    static float const s0 = AK::cos(0.0f / 16.0f * AK::Pi<float>) * AK::rsqrt(8.0f);
    static float const s1 = AK::cos(1.0f / 16.0f * AK::Pi<float>) / 2.0f;
    static float const s2 = AK::cos(2.0f / 16.0f * AK::Pi<float>) / 2.0f;
    static float const s3 = AK::cos(3.0f / 16.0f * AK::Pi<float>) / 2.0f;
    static float const s4 = AK::cos(4.0f / 16.0f * AK::Pi<float>) / 2.0f;
    static float const s5 = AK::cos(5.0f / 16.0f * AK::Pi<float>) / 2.0f;
    static float const s6 = AK::cos(6.0f / 16.0f * AK::Pi<float>) / 2.0f;
    static float const s7 = AK::cos(7.0f / 16.0f * AK::Pi<float>) / 2.0f;

    for (u32 vcursor = 0; vcursor < context.mblock_meta.vcount; vcursor += context.vsample_factor) {
        for (u32 hcursor = 0; hcursor < context.mblock_meta.hcount; hcursor += context.hsample_factor) {
            for (u32 component_i = 0; component_i < context.component_count; component_i++) {
                auto& component = context.components[component_i];
                for (u8 vfactor_i = 0; vfactor_i < component.vsample_factor; vfactor_i++) {
                    for (u8 hfactor_i = 0; hfactor_i < component.hsample_factor; hfactor_i++) {
                        u32 mb_index = (vcursor + vfactor_i) * context.mblock_meta.hpadded_count + (hfactor_i + hcursor);
                        Macroblock& block = macroblocks[mb_index];
                        i32* block_component = get_component(block, component_i);
                        for (u32 k = 0; k < 8; ++k) {
                            float const g0 = block_component[0 * 8 + k] * s0;
                            float const g1 = block_component[4 * 8 + k] * s4;
                            float const g2 = block_component[2 * 8 + k] * s2;
                            float const g3 = block_component[6 * 8 + k] * s6;
                            float const g4 = block_component[5 * 8 + k] * s5;
                            float const g5 = block_component[1 * 8 + k] * s1;
                            float const g6 = block_component[7 * 8 + k] * s7;
                            float const g7 = block_component[3 * 8 + k] * s3;

                            float const f0 = g0;
                            float const f1 = g1;
                            float const f2 = g2;
                            float const f3 = g3;
                            float const f4 = g4 - g7;
                            float const f5 = g5 + g6;
                            float const f6 = g5 - g6;
                            float const f7 = g4 + g7;

                            float const e0 = f0;
                            float const e1 = f1;
                            float const e2 = f2 - f3;
                            float const e3 = f2 + f3;
                            float const e4 = f4;
                            float const e5 = f5 - f7;
                            float const e6 = f6;
                            float const e7 = f5 + f7;
                            float const e8 = f4 + f6;

                            float const d0 = e0;
                            float const d1 = e1;
                            float const d2 = e2 * m1;
                            float const d3 = e3;
                            float const d4 = e4 * m2;
                            float const d5 = e5 * m3;
                            float const d6 = e6 * m4;
                            float const d7 = e7;
                            float const d8 = e8 * m5;

                            float const c0 = d0 + d1;
                            float const c1 = d0 - d1;
                            float const c2 = d2 - d3;
                            float const c3 = d3;
                            float const c4 = d4 + d8;
                            float const c5 = d5 + d7;
                            float const c6 = d6 - d8;
                            float const c7 = d7;
                            float const c8 = c5 - c6;

                            float const b0 = c0 + c3;
                            float const b1 = c1 + c2;
                            float const b2 = c1 - c2;
                            float const b3 = c0 - c3;
                            float const b4 = c4 - c8;
                            float const b5 = c8;
                            float const b6 = c6 - c7;
                            float const b7 = c7;

                            block_component[0 * 8 + k] = b0 + b7;
                            block_component[1 * 8 + k] = b1 + b6;
                            block_component[2 * 8 + k] = b2 + b5;
                            block_component[3 * 8 + k] = b3 + b4;
                            block_component[4 * 8 + k] = b3 - b4;
                            block_component[5 * 8 + k] = b2 - b5;
                            block_component[6 * 8 + k] = b1 - b6;
                            block_component[7 * 8 + k] = b0 - b7;
                        }
                        for (u32 l = 0; l < 8; ++l) {
                            float const g0 = block_component[l * 8 + 0] * s0;
                            float const g1 = block_component[l * 8 + 4] * s4;
                            float const g2 = block_component[l * 8 + 2] * s2;
                            float const g3 = block_component[l * 8 + 6] * s6;
                            float const g4 = block_component[l * 8 + 5] * s5;
                            float const g5 = block_component[l * 8 + 1] * s1;
                            float const g6 = block_component[l * 8 + 7] * s7;
                            float const g7 = block_component[l * 8 + 3] * s3;

                            float const f0 = g0;
                            float const f1 = g1;
                            float const f2 = g2;
                            float const f3 = g3;
                            float const f4 = g4 - g7;
                            float const f5 = g5 + g6;
                            float const f6 = g5 - g6;
                            float const f7 = g4 + g7;

                            float const e0 = f0;
                            float const e1 = f1;
                            float const e2 = f2 - f3;
                            float const e3 = f2 + f3;
                            float const e4 = f4;
                            float const e5 = f5 - f7;
                            float const e6 = f6;
                            float const e7 = f5 + f7;
                            float const e8 = f4 + f6;

                            float const d0 = e0;
                            float const d1 = e1;
                            float const d2 = e2 * m1;
                            float const d3 = e3;
                            float const d4 = e4 * m2;
                            float const d5 = e5 * m3;
                            float const d6 = e6 * m4;
                            float const d7 = e7;
                            float const d8 = e8 * m5;

                            float const c0 = d0 + d1;
                            float const c1 = d0 - d1;
                            float const c2 = d2 - d3;
                            float const c3 = d3;
                            float const c4 = d4 + d8;
                            float const c5 = d5 + d7;
                            float const c6 = d6 - d8;
                            float const c7 = d7;
                            float const c8 = c5 - c6;

                            float const b0 = c0 + c3;
                            float const b1 = c1 + c2;
                            float const b2 = c1 - c2;
                            float const b3 = c0 - c3;
                            float const b4 = c4 - c8;
                            float const b5 = c8;
                            float const b6 = c6 - c7;
                            float const b7 = c7;

                            block_component[l * 8 + 0] = b0 + b7;
                            block_component[l * 8 + 1] = b1 + b6;
                            block_component[l * 8 + 2] = b2 + b5;
                            block_component[l * 8 + 3] = b3 + b4;
                            block_component[l * 8 + 4] = b3 - b4;
                            block_component[l * 8 + 5] = b2 - b5;
                            block_component[l * 8 + 6] = b1 - b6;
                            block_component[l * 8 + 7] = b0 - b7;
                        }
                    }
                }
            }
        }
    }
}

static void ycbcr_to_rgb(JPGLoadingContext const& context, Vector<Macroblock>& macroblocks)
{
    for (u32 vcursor = 0; vcursor < context.mblock_meta.vcount; vcursor += context.vsample_factor) {
        for (u32 hcursor = 0; hcursor < context.mblock_meta.hcount; hcursor += context.hsample_factor) {
            const u32 chroma_block_index = vcursor * context.mblock_meta.hpadded_count + hcursor;
            Macroblock const& chroma = macroblocks[chroma_block_index];
            // Overflows are intentional.
            for (u8 vfactor_i = context.vsample_factor - 1; vfactor_i < context.vsample_factor; --vfactor_i) {
                for (u8 hfactor_i = context.hsample_factor - 1; hfactor_i < context.hsample_factor; --hfactor_i) {
                    u32 mb_index = (vcursor + vfactor_i) * context.mblock_meta.hpadded_count + (hcursor + hfactor_i);
                    i32* y = macroblocks[mb_index].y;
                    i32* cb = macroblocks[mb_index].cb;
                    i32* cr = macroblocks[mb_index].cr;
                    for (u8 i = 7; i < 8; --i) {
                        for (u8 j = 7; j < 8; --j) {
                            const u8 pixel = i * 8 + j;
                            const u32 chroma_pxrow = (i / context.vsample_factor) + 4 * vfactor_i;
                            const u32 chroma_pxcol = (j / context.hsample_factor) + 4 * hfactor_i;
                            const u32 chroma_pixel = chroma_pxrow * 8 + chroma_pxcol;
                            int r = y[pixel] + 1.402f * chroma.cr[chroma_pixel] + 128;
                            int g = y[pixel] - 0.344f * chroma.cb[chroma_pixel] - 0.714f * chroma.cr[chroma_pixel] + 128;
                            int b = y[pixel] + 1.772f * chroma.cb[chroma_pixel] + 128;
                            y[pixel] = r < 0 ? 0 : (r > 255 ? 255 : r);
                            cb[pixel] = g < 0 ? 0 : (g > 255 ? 255 : g);
                            cr[pixel] = b < 0 ? 0 : (b > 255 ? 255 : b);
                        }
                    }
                }
            }
        }
    }
}

static ErrorOr<void> compose_bitmap(JPGLoadingContext& context, Vector<Macroblock> const& macroblocks)
{
    context.bitmap = TRY(Bitmap::create(BitmapFormat::BGRx8888, { context.frame.width, context.frame.height }));

    for (u32 y = context.frame.height - 1; y < context.frame.height; y--) {
        const u32 block_row = y / 8;
        const u32 pixel_row = y % 8;
        for (u32 x = 0; x < context.frame.width; x++) {
            const u32 block_column = x / 8;
            auto& block = macroblocks[block_row * context.mblock_meta.hpadded_count + block_column];
            const u32 pixel_column = x % 8;
            const u32 pixel_index = pixel_row * 8 + pixel_column;
            const Color color { (u8)block.y[pixel_index], (u8)block.cb[pixel_index], (u8)block.cr[pixel_index] };
            context.bitmap->set_pixel(x, y, color);
        }
    }

    return {};
}

static ErrorOr<void> parse_header(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    auto marker = TRY(read_marker_at_cursor(stream));
    if (marker != JPG_SOI) {
        dbgln_if(JPG_DEBUG, "{}: SOI not found: {:x}!", TRY(stream.tell()), marker);
        return Error::from_string_literal("SOI not found");
    }
    for (;;) {
        marker = TRY(read_marker_at_cursor(stream));

        // Set frame type if the marker marks a new frame.
        if (marker >= 0xFFC0 && marker <= 0xFFCF) {
            // Ignore interleaved markers.
            if (marker != 0xFFC4 && marker != 0xFFC8 && marker != 0xFFCC) {
                context.frame.type = static_cast<StartOfFrame::FrameType>(marker & 0xF);
            }
        }

        switch (marker) {
        case JPG_INVALID:
        case JPG_RST0:
        case JPG_RST1:
        case JPG_RST2:
        case JPG_RST3:
        case JPG_RST4:
        case JPG_RST5:
        case JPG_RST6:
        case JPG_RST7:
        case JPG_SOI:
        case JPG_EOI:
            dbgln_if(JPG_DEBUG, "{}: Unexpected marker {:x}!", TRY(stream.tell()), marker);
            return Error::from_string_literal("Unexpected marker");
        case JPG_APPN0:
        case JPG_APPN1:
        case JPG_APPN2:
        case JPG_APPN3:
        case JPG_APPN4:
        case JPG_APPN5:
        case JPG_APPN6:
        case JPG_APPN7:
        case JPG_APPN8:
        case JPG_APPN9:
        case JPG_APPNA:
        case JPG_APPNB:
        case JPG_APPNC:
        case JPG_APPND:
        case JPG_APPNE:
        case JPG_APPNF:
            TRY(read_app_marker(stream, context, marker - JPG_APPN0));
            break;
        case JPG_SOF0:
            TRY(read_start_of_frame(stream, context));
            context.state = JPGLoadingContext::FrameDecoded;
            break;
        case JPG_DQT:
            TRY(read_quantization_table(stream, context));
            break;
        case JPG_RST:
            TRY(read_reset_marker(stream, context));
            break;
        case JPG_DHT:
            TRY(read_huffman_table(stream, context));
            break;
        case JPG_SOS:
            return read_start_of_scan(stream, context);
        default:
            if (auto result = skip_marker_with_length(stream); result.is_error()) {
                dbgln_if(JPG_DEBUG, "{}: Error skipping marker: {:x}!", TRY(stream.tell()), marker);
                return result.release_error();
            }
            break;
        }
    }

    VERIFY_NOT_REACHED();
}

static ErrorOr<void> scan_huffman_stream(AK::SeekableStream& stream, JPGLoadingContext& context)
{
    u8 last_byte;
    u8 current_byte = TRY(stream.read_value<u8>());

    for (;;) {
        last_byte = current_byte;
        current_byte = TRY(stream.read_value<u8>());

        if (last_byte == 0xFF) {
            if (current_byte == 0xFF)
                continue;
            if (current_byte == 0x00) {
                current_byte = TRY(stream.read_value<u8>());
                context.huffman_stream.stream.append(last_byte);
                continue;
            }
            Marker marker = 0xFF00 | current_byte;
            if (marker == JPG_EOI)
                return {};
            if (marker >= JPG_RST0 && marker <= JPG_RST7) {
                context.huffman_stream.stream.append(marker);
                current_byte = TRY(stream.read_value<u8>());
                continue;
            }
            dbgln_if(JPG_DEBUG, "{}: Invalid marker: {:x}!", TRY(stream.tell()), marker);
            return Error::from_string_literal("Invalid marker");
        } else {
            context.huffman_stream.stream.append(last_byte);
        }
    }

    VERIFY_NOT_REACHED();
}

static ErrorOr<void> decode_header(JPGLoadingContext& context)
{
    if (context.state < JPGLoadingContext::State::HeaderDecoded) {
        context.stream = TRY(try_make<FixedMemoryStream>(ReadonlyBytes { context.data, context.data_size }));

        if (auto result = parse_header(*context.stream, context); result.is_error()) {
            context.state = JPGLoadingContext::State::Error;
            return result.release_error();
        }
        context.state = JPGLoadingContext::State::HeaderDecoded;
    }
    return {};
}

static ErrorOr<void> decode_jpg(JPGLoadingContext& context)
{
    TRY(decode_header(context));
    TRY(scan_huffman_stream(*context.stream, context));
    auto macroblocks = TRY(decode_huffman_stream(context));
    dequantize(context, macroblocks);
    inverse_dct(context, macroblocks);
    ycbcr_to_rgb(context, macroblocks);
    TRY(compose_bitmap(context, macroblocks));
    context.stream.clear();
    return {};
}

JPGImageDecoderPlugin::JPGImageDecoderPlugin(u8 const* data, size_t size)
{
    m_context = make<JPGLoadingContext>();
    m_context->data = data;
    m_context->data_size = size;
    m_context->huffman_stream.stream.ensure_capacity(50 * KiB);
}

JPGImageDecoderPlugin::~JPGImageDecoderPlugin() = default;

IntSize JPGImageDecoderPlugin::size()
{
    if (m_context->state == JPGLoadingContext::State::Error)
        return {};
    if (m_context->state >= JPGLoadingContext::State::FrameDecoded)
        return { m_context->frame.width, m_context->frame.height };

    return {};
}

void JPGImageDecoderPlugin::set_volatile()
{
    if (m_context->bitmap)
        m_context->bitmap->set_volatile();
}

bool JPGImageDecoderPlugin::set_nonvolatile(bool& was_purged)
{
    if (!m_context->bitmap)
        return false;
    return m_context->bitmap->set_nonvolatile(was_purged);
}

bool JPGImageDecoderPlugin::initialize()
{
    return true;
}

ErrorOr<bool> JPGImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return data.size() > 3
        && data.data()[0] == 0xFF
        && data.data()[1] == 0xD8
        && data.data()[2] == 0xFF;
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> JPGImageDecoderPlugin::create(ReadonlyBytes data)
{
    return adopt_nonnull_own_or_enomem(new (nothrow) JPGImageDecoderPlugin(data.data(), data.size()));
}

bool JPGImageDecoderPlugin::is_animated()
{
    return false;
}

size_t JPGImageDecoderPlugin::loop_count()
{
    return 0;
}

size_t JPGImageDecoderPlugin::frame_count()
{
    return 1;
}

ErrorOr<ImageFrameDescriptor> JPGImageDecoderPlugin::frame(size_t index)
{
    if (index > 0)
        return Error::from_string_literal("JPGImageDecoderPlugin: Invalid frame index");

    if (m_context->state == JPGLoadingContext::State::Error)
        return Error::from_string_literal("JPGImageDecoderPlugin: Decoding failed");

    if (m_context->state < JPGLoadingContext::State::BitmapDecoded) {
        if (auto result = decode_jpg(*m_context); result.is_error()) {
            m_context->state = JPGLoadingContext::State::Error;
            return result.release_error();
        }
        m_context->state = JPGLoadingContext::State::BitmapDecoded;
    }

    return ImageFrameDescriptor { m_context->bitmap, 0 };
}

ErrorOr<Optional<ReadonlyBytes>> JPGImageDecoderPlugin::icc_data()
{
    TRY(decode_header(*m_context));

    if (m_context->icc_data.has_value())
        return *m_context->icc_data;
    return OptionalNone {};
}

}
