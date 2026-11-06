#include "mot_slideshow.hpp"

#include <algorithm>
#include <limits>

namespace dab {
namespace {

constexpr std::size_t maximum_objects = 32;
constexpr std::size_t maximum_segments = 8192;
constexpr std::size_t maximum_entity_bytes = 4 * 1024 * 1024;

bool crc_valid(const std::uint8_t* data, std::size_t size)
{
    if (size < 2) return false;
    std::uint16_t crc = 0xffff;
    for (std::size_t byte = 0; byte + 2 < size; ++byte) {
        for (int bit = 7; bit >= 0; --bit) {
            const std::uint16_t input = (data[byte] >> bit) & 1U;
            const std::uint16_t feedback = input ^ ((crc >> 15) & 1U);
            crc = std::uint16_t(crc << 1);
            if (feedback) crc ^= 0x1021;
        }
    }
    const std::uint16_t transmitted = std::uint16_t(
        (std::uint16_t(data[size - 2]) << 8) | data[size - 1]);
    return crc == std::uint16_t(~transmitted);
}

std::string safe_text(const std::uint8_t* data, std::size_t size)
{
    std::string result;
    result.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char value = data[index];
        result.push_back(value >= 0x20 && value != 0x7f ? char(value) : '_');
    }
    return result;
}

std::optional<MotSlide> decode_object(
    const MotSlideshowDecoder::Entity& header_entity,
    const MotSlideshowDecoder::Entity& body_entity)
{
    if (!header_entity.complete() || !body_entity.complete()) return std::nullopt;
    const auto header = header_entity.assemble();
    if (header.size() < 7) return std::nullopt;
    const std::size_t body_size =
        (std::size_t(header[0]) << 20) |
        (std::size_t(header[1]) << 12) |
        (std::size_t(header[2]) << 4) | (header[3] >> 4);
    const std::size_t header_size =
        (std::size_t(header[3] & 0x0f) << 9) |
        (std::size_t(header[4]) << 1) | (header[5] >> 7);
    const unsigned content_type = (header[5] >> 1) & 0x3f;
    const unsigned content_subtype =
        (unsigned(header[5] & 1U) << 8) | header[6];
    if (header_size != header.size() || body_size > maximum_entity_bytes ||
        content_type != 2 || (content_subtype != 1 && content_subtype != 3))
        return std::nullopt;

    MotSlide slide;
    slide.format = content_subtype == 3
        ? MotSlide::Format::Png : MotSlide::Format::Jpeg;
    bool compressed = false;
    bool conditional_access = false;
    for (std::size_t offset = 7; offset < header.size();) {
        const std::uint8_t descriptor = header[offset++];
        const unsigned pli = descriptor >> 6;
        const unsigned parameter = descriptor & 0x3f;
        std::size_t length = 0;
        if (pli == 1) length = 1;
        else if (pli == 2) length = 4;
        else if (pli == 3) {
            if (offset >= header.size()) return std::nullopt;
            const bool extended = (header[offset] & 0x80) != 0;
            length = header[offset++] & 0x7f;
            if (extended) {
                if (offset >= header.size()) return std::nullopt;
                length = (length << 8) | header[offset++];
            }
        }
        if (length > header.size() - offset) return std::nullopt;
        if (parameter == 0x0c && length > 1)
            slide.content_name = safe_text(
                header.data() + offset + 1, length - 1);
        else if (parameter == 0x11)
            compressed = true;
        else if (parameter == 0x23)
            conditional_access = true;
        offset += length;
    }
    if (compressed || conditional_access) return std::nullopt;
    slide.encoded = body_entity.assemble();
    if (slide.encoded.size() != body_size || slide.encoded.empty())
        return std::nullopt;
    return slide;
}

} // namespace

bool MotSlideshowDecoder::Entity::add(
    std::uint16_t number, bool is_last,
    const std::uint8_t* data, std::size_t length)
{
    if (number >= maximum_segments || length > maximum_entity_bytes ||
        size > maximum_entity_bytes - length)
        return false;
    if (is_last) last = number;
    auto [entry, inserted] = segments.emplace(
        number, std::vector<std::uint8_t>{});
    if (!inserted) return true;
    entry->second.assign(data, data + length);
    size += length;
    return true;
}

bool MotSlideshowDecoder::Entity::complete() const
{
    if (!last || segments.size() < std::size_t(*last) + 1) return false;
    for (std::uint32_t number = 0; number <= *last; ++number)
        if (segments.find(std::uint16_t(number)) == segments.end()) return false;
    return true;
}

std::vector<std::uint8_t> MotSlideshowDecoder::Entity::assemble() const
{
    std::vector<std::uint8_t> result;
    if (!complete()) return result;
    result.reserve(size);
    for (std::uint32_t number = 0; number <= *last; ++number) {
        const auto& segment = segments.at(std::uint16_t(number));
        result.insert(result.end(), segment.begin(), segment.end());
    }
    return result;
}

std::optional<MotSlide> MotSlideshowDecoder::push_data_group(
    const std::uint8_t* data, std::size_t size)
{
    if (!data || size < 2) return std::nullopt;
    const bool extension = (data[0] & 0x80) != 0;
    const bool has_crc = (data[0] & 0x40) != 0;
    const bool segmented = (data[0] & 0x20) != 0;
    const bool user_access = (data[0] & 0x10) != 0;
    const unsigned type = data[0] & 0x0f;
    if (!segmented || !user_access || (type != 3 && type != 4) ||
        (has_crc && !crc_valid(data, size)))
        return std::nullopt;

    const std::size_t payload_end = size - (has_crc ? 2 : 0);
    std::size_t offset = 2;
    if (extension) offset += 2;
    if (offset + 3 > payload_end) return std::nullopt;
    const bool last = (data[offset] & 0x80) != 0;
    const std::uint16_t segment_number = std::uint16_t(
        (std::uint16_t(data[offset] & 0x7f) << 8) | data[offset + 1]);
    offset += 2;
    const bool has_transport_id = (data[offset] & 0x10) != 0;
    const std::size_t access_length = data[offset++] & 0x0f;
    if (!has_transport_id || access_length < 2 ||
        access_length > payload_end - offset)
        return std::nullopt;
    const std::uint16_t transport_id = std::uint16_t(
        (std::uint16_t(data[offset]) << 8) | data[offset + 1]);
    offset += access_length;
    if (offset + 2 > payload_end) return std::nullopt;
    const std::size_t segment_size =
        (std::size_t(data[offset] & 0x1f) << 8) | data[offset + 1];
    offset += 2;
    if (segment_size != payload_end - offset) return std::nullopt;

    if (objects_.size() >= maximum_objects &&
        objects_.find(transport_id) == objects_.end())
        objects_.erase(objects_.begin());
    auto& object = objects_[transport_id];
    auto& entity = type == 3 ? object.header : object.body;
    if (!entity.add(segment_number, last, data + offset, segment_size)) {
        objects_.erase(transport_id);
        return std::nullopt;
    }
    auto slide = decode_object(object.header, object.body);
    if (slide) objects_.erase(transport_id);
    return slide;
}

void MotSlideshowDecoder::reset()
{
    objects_.clear();
}

} // namespace dab
