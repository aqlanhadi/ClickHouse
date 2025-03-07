#include <DataTypes/Serializations/SerializationString.h>

#include <Columns/ColumnString.h>

#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>

#include <Core/Field.h>

#include <Formats/FormatSettings.h>

#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <IO/VarInt.h>
#include <IO/ReadBufferFromString.h>

#include <base/unit.h>

#ifdef __SSE2__
    #include <emmintrin.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int TOO_LARGE_STRING_SIZE;
}

void SerializationString::serializeBinary(const Field & field, WriteBuffer & ostr, const FormatSettings & settings) const
{
    const String & s = field.get<const String &>();
    if (settings.max_binary_string_size && s.size() > settings.max_binary_string_size)
        throw Exception(
            ErrorCodes::TOO_LARGE_STRING_SIZE,
            "Too large string size: {}. The maximum is: {}. To increase the maximum, use setting "
            "format_binary_max_string_size",
            s.size(),
            settings.max_binary_string_size);

    writeVarUInt(s.size(), ostr);
    writeString(s, ostr);
}


void SerializationString::deserializeBinary(Field & field, ReadBuffer & istr, const FormatSettings & settings) const
{
    UInt64 size;
    readVarUInt(size, istr);
    if (settings.max_binary_string_size && size > settings.max_binary_string_size)
        throw Exception(
            ErrorCodes::TOO_LARGE_STRING_SIZE,
            "Too large string size: {}. The maximum is: {}. To increase the maximum, use setting "
            "format_binary_max_string_size",
            size,
            settings.max_binary_string_size);

    field = String();
    String & s = field.get<String &>();
    s.resize(size);
    istr.readStrict(s.data(), size);
}


void SerializationString::serializeBinary(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    const StringRef & s = assert_cast<const ColumnString &>(column).getDataAt(row_num);
    if (settings.max_binary_string_size && s.size > settings.max_binary_string_size)
        throw Exception(
            ErrorCodes::TOO_LARGE_STRING_SIZE,
            "Too large string size: {}. The maximum is: {}. To increase the maximum, use setting "
            "format_binary_max_string_size",
            s.size,
            settings.max_binary_string_size);

    writeVarUInt(s.size, ostr);
    writeString(s, ostr);
}


void SerializationString::deserializeBinary(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    ColumnString & column_string = assert_cast<ColumnString &>(column);
    ColumnString::Chars & data = column_string.getChars();
    ColumnString::Offsets & offsets = column_string.getOffsets();

    UInt64 size;
    readVarUInt(size, istr);
    if (settings.max_binary_string_size && size > settings.max_binary_string_size)
        throw Exception(
            ErrorCodes::TOO_LARGE_STRING_SIZE,
            "Too large string size: {}. The maximum is: {}. To increase the maximum, use setting "
            "format_binary_max_string_size",
            size,
            settings.max_binary_string_size);

    size_t old_chars_size = data.size();
    size_t offset = old_chars_size + size + 1;
    offsets.push_back(offset);

    try
    {
        data.resize(offset);
        istr.readStrict(reinterpret_cast<char*>(&data[offset - size - 1]), size);
        data.back() = 0;
    }
    catch (...)
    {
        offsets.pop_back();
        data.resize_assume_reserved(old_chars_size);
        throw;
    }
}


void SerializationString::serializeBinaryBulk(const IColumn & column, WriteBuffer & ostr, size_t offset, size_t limit) const
{
    const ColumnString & column_string = typeid_cast<const ColumnString &>(column);
    const ColumnString::Chars & data = column_string.getChars();
    const ColumnString::Offsets & offsets = column_string.getOffsets();

    size_t size = column_string.size();
    if (!size)
        return;

    size_t end = limit && offset + limit < size
        ? offset + limit
        : size;

    if (offset == 0)
    {
        UInt64 str_size = offsets[0] - 1;
        writeVarUInt(str_size, ostr);
        ostr.write(reinterpret_cast<const char *>(data.data()), str_size);

        ++offset;
    }

    for (size_t i = offset; i < end; ++i)
    {
        UInt64 str_size = offsets[i] - offsets[i - 1] - 1;
        writeVarUInt(str_size, ostr);
        ostr.write(reinterpret_cast<const char *>(&data[offsets[i - 1]]), str_size);
    }
}


template <int UNROLL_TIMES>
static NO_INLINE void deserializeBinarySSE2(ColumnString::Chars & data, ColumnString::Offsets & offsets, ReadBuffer & istr, size_t limit)
{
    size_t offset = data.size();
    /// Avoiding calling resize in a loop improves the performance.
    data.resize(std::max(data.capacity(), static_cast<size_t>(4096)));

    for (size_t i = 0; i < limit; ++i)
    {
        if (istr.eof())
            break;

        UInt64 size;
        readVarUInt(size, istr);

        static constexpr size_t max_string_size = 16_GiB;   /// Arbitrary value to prevent logical errors and overflows, but large enough.
        if (size > max_string_size)
            throw Exception(
                ErrorCodes::TOO_LARGE_STRING_SIZE,
                "Too large string size: {}. The maximum is: {}.",
                size,
                max_string_size);

        offset += size + 1;
        offsets.push_back(offset);

        if (unlikely(offset > data.size()))
            data.resize_exact(roundUpToPowerOfTwoOrZero(std::max(offset, data.size() * 2)));

        if (size)
        {
#ifdef __SSE2__
            /// An optimistic branch in which more efficient copying is possible.
            if (offset + 16 * UNROLL_TIMES <= data.capacity() && istr.position() + size + 16 * UNROLL_TIMES <= istr.buffer().end())
            {
                const __m128i * sse_src_pos = reinterpret_cast<const __m128i *>(istr.position());
                const __m128i * sse_src_end = sse_src_pos + (size + (16 * UNROLL_TIMES - 1)) / 16 / UNROLL_TIMES * UNROLL_TIMES;
                __m128i * sse_dst_pos = reinterpret_cast<__m128i *>(&data[offset - size - 1]);

                while (sse_src_pos < sse_src_end)
                {
                    for (size_t j = 0; j < UNROLL_TIMES; ++j)
                        _mm_storeu_si128(sse_dst_pos + j, _mm_loadu_si128(sse_src_pos + j));

                    sse_src_pos += UNROLL_TIMES;
                    sse_dst_pos += UNROLL_TIMES;
                }

                istr.position() += size;
            }
            else
#endif
            {
                istr.readStrict(reinterpret_cast<char*>(&data[offset - size - 1]), size);
            }
        }

        data[offset - 1] = 0;
    }

    data.resize(offset);
}


void SerializationString::deserializeBinaryBulk(IColumn & column, ReadBuffer & istr, size_t limit, double avg_value_size_hint) const
{
    ColumnString & column_string = typeid_cast<ColumnString &>(column);
    ColumnString::Chars & data = column_string.getChars();
    ColumnString::Offsets & offsets = column_string.getOffsets();

    double avg_chars_size = 1; /// By default reserve only for empty strings.

    if (avg_value_size_hint > 0.0 && avg_value_size_hint > sizeof(offsets[0]))
    {
        /// Randomly selected.
        constexpr auto avg_value_size_hint_reserve_multiplier = 1.2;

        avg_chars_size = (avg_value_size_hint - sizeof(offsets[0])) * avg_value_size_hint_reserve_multiplier;
    }

    size_t size_to_reserve = data.size() + static_cast<size_t>(std::ceil(limit * avg_chars_size));

    /// Never reserve for too big size.
    if (size_to_reserve < 256 * 1024 * 1024)
    {
        try
        {
            data.reserve(size_to_reserve);
        }
        catch (Exception & e)
        {
            e.addMessage(
                "(avg_value_size_hint = " + toString(avg_value_size_hint)
                + ", avg_chars_size = " + toString(avg_chars_size)
                + ", limit = " + toString(limit) + ")");
            throw;
        }
    }

    offsets.reserve(offsets.size() + limit);

    if (avg_chars_size >= 64)
        deserializeBinarySSE2<4>(data, offsets, istr, limit);
    else if (avg_chars_size >= 48)
        deserializeBinarySSE2<3>(data, offsets, istr, limit);
    else if (avg_chars_size >= 32)
        deserializeBinarySSE2<2>(data, offsets, istr, limit);
    else
        deserializeBinarySSE2<1>(data, offsets, istr, limit);
}


void SerializationString::serializeText(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const
{
    writeString(assert_cast<const ColumnString &>(column).getDataAt(row_num), ostr);
}


void SerializationString::serializeTextEscaped(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const
{
    writeEscapedString(assert_cast<const ColumnString &>(column).getDataAt(row_num).toView(), ostr);
}


template <typename Reader>
static inline void read(IColumn & column, Reader && reader)
{
    ColumnString & column_string = assert_cast<ColumnString &>(column);
    ColumnString::Chars & data = column_string.getChars();
    ColumnString::Offsets & offsets = column_string.getOffsets();
    size_t old_chars_size = data.size();
    size_t old_offsets_size = offsets.size();
    try
    {
        reader(data);
        data.push_back(0);
        offsets.push_back(data.size());
    }
    catch (...)
    {
        offsets.resize_assume_reserved(old_offsets_size);
        data.resize_assume_reserved(old_chars_size);
        throw;
    }
}


void SerializationString::deserializeWholeText(IColumn & column, ReadBuffer & istr, const FormatSettings &) const
{
    read(column, [&](ColumnString::Chars & data) { readStringUntilEOFInto(data, istr); });
}


void SerializationString::deserializeTextEscaped(IColumn & column, ReadBuffer & istr, const FormatSettings &) const
{
    read(column, [&](ColumnString::Chars & data) { readEscapedStringInto(data, istr); });
}


void SerializationString::serializeTextQuoted(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const
{
    writeQuotedString(assert_cast<const ColumnString &>(column).getDataAt(row_num), ostr);
}


void SerializationString::deserializeTextQuoted(IColumn & column, ReadBuffer & istr, const FormatSettings &) const
{
    read(column, [&](ColumnString::Chars & data) { readQuotedStringInto<true>(data, istr); });
}


void SerializationString::serializeTextJSON(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    writeJSONString(assert_cast<const ColumnString &>(column).getDataAt(row_num).toView(), ostr, settings);
}


void SerializationString::deserializeTextJSON(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    if (settings.json.read_objects_as_strings && !istr.eof() && *istr.position() == '{')
    {
        read(column, [&](ColumnString::Chars & data) { readJSONObjectPossiblyInvalid(data, istr); });
    }
    else if (settings.json.read_arrays_as_strings && !istr.eof() && *istr.position() == '[')
    {
        read(column, [&](ColumnString::Chars & data) { readJSONArrayInto(data, istr); });
    }
    else if (settings.json.read_bools_as_strings && !istr.eof() && (*istr.position() == 't' || *istr.position() == 'f'))
    {
        String str_value;
        if (*istr.position() == 't')
        {
            assertString("true", istr);
            str_value = "true";
        }
        else if (*istr.position() == 'f')
        {
            assertString("false", istr);
            str_value = "false";
        }

        read(column, [&](ColumnString::Chars & data) { data.insert(str_value.begin(), str_value.end()); });
    }
    else if (settings.json.read_numbers_as_strings && !istr.eof() && *istr.position() != '"')
    {
        String field;
        readJSONField(field, istr);
        Float64 tmp;
        ReadBufferFromString buf(field);
        if (tryReadFloatText(tmp, buf) && buf.eof())
            read(column, [&](ColumnString::Chars & data) { data.insert(field.begin(), field.end()); });
        else
            throw Exception(ErrorCodes::INCORRECT_DATA, "Cannot parse JSON String value here: {}", field);
    }
    else
        read(column, [&](ColumnString::Chars & data) { readJSONStringInto(data, istr); });
}


void SerializationString::serializeTextXML(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const
{
    writeXMLStringForTextElement(assert_cast<const ColumnString &>(column).getDataAt(row_num).toView(), ostr);
}


void SerializationString::serializeTextCSV(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const
{
    writeCSVString<>(assert_cast<const ColumnString &>(column).getDataAt(row_num), ostr);
}


void SerializationString::deserializeTextCSV(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    read(column, [&](ColumnString::Chars & data) { readCSVStringInto(data, istr, settings.csv); });
}

void SerializationString::serializeTextMarkdown(
    const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    if (settings.markdown.escape_special_characters)
        writeMarkdownEscapedString(assert_cast<const ColumnString &>(column).getDataAt(row_num).toView(), ostr);
    else
        serializeTextEscaped(column, row_num, ostr, settings);
}

}
