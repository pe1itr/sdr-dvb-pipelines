#include "dynamic_label.hpp"

#include <algorithm>

namespace dab {
namespace {

constexpr std::size_t maximum_label_bytes = 128;

void append_utf8(std::string& output, std::uint32_t value)
{
    if (value <= 0x7f) output.push_back(char(value));
    else if (value <= 0x7ff) {
        output.push_back(char(0xc0 | (value >> 6)));
        output.push_back(char(0x80 | (value & 0x3f)));
    } else {
        output.push_back(char(0xe0 | (value >> 12)));
        output.push_back(char(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(char(0x80 | (value & 0x3f)));
    }
}

// ETSI TS 101 756 EBU Latin differs from ISO-8859-1 mainly outside ASCII.
// The explicit exceptions cover the characters commonly encountered in
// western-European DLS; unknown control positions are replaced safely.
std::uint32_t ebu_latin(std::uint8_t value)
{
    if (value >= 0x20 && value <= 0x7a) return value;
    switch (value) {
    case 0x7b: return 0x00ab; case 0x7d: return 0x00bb;
    case 0x80: return 0x00e1; case 0x81: return 0x00e0;
    case 0x82: return 0x00e9; case 0x83: return 0x00e8;
    case 0x84: return 0x00ed; case 0x85: return 0x00ec;
    case 0x86: return 0x00f3; case 0x87: return 0x00f2;
    case 0x88: return 0x00fa; case 0x89: return 0x00f9;
    case 0x8a: return 0x00d1; case 0x8b: return 0x00c7;
    case 0x8d: return 0x00df; case 0x90: return 0x00e2;
    case 0x91: return 0x00e4; case 0x92: return 0x00ea;
    case 0x93: return 0x00eb; case 0x94: return 0x00ee;
    case 0x95: return 0x00ef; case 0x96: return 0x00f4;
    case 0x97: return 0x00f6; case 0x98: return 0x00fb;
    case 0x99: return 0x00fc; case 0x9a: return 0x00f1;
    case 0x9b: return 0x00e7; case 0x9f: return 0x00ff;
    case 0xa2: return 0x00a9; case 0xa9: return 0x20ac;
    case 0xaa: return 0x00a3; case 0xb9: return 0x00bf;
    case 0xbb: return 0x00b0; case 0xc0: return 0x00c1;
    case 0xc1: return 0x00c0; case 0xc2: return 0x00c9;
    case 0xc3: return 0x00c8; case 0xc4: return 0x00cd;
    case 0xc5: return 0x00cc; case 0xc6: return 0x00d3;
    case 0xc7: return 0x00d2; case 0xc8: return 0x00da;
    case 0xc9: return 0x00d9; case 0xd0: return 0x00c2;
    case 0xd1: return 0x00c4; case 0xd2: return 0x00ca;
    case 0xd3: return 0x00cb; case 0xd4: return 0x00ce;
    case 0xd5: return 0x00cf; case 0xd6: return 0x00d4;
    case 0xd7: return 0x00d6; case 0xd8: return 0x00db;
    case 0xd9: return 0x00dc; case 0xe0: return 0x00c3;
    case 0xe1: return 0x00c5; case 0xe2: return 0x00c6;
    case 0xe5: return 0x00dd; case 0xe6: return 0x00d5;
    case 0xe7: return 0x00d8; case 0xef: return 0x00f0;
    case 0xf0: return 0x00e3; case 0xf1: return 0x00e5;
    case 0xf2: return 0x00e6; case 0xf5: return 0x00fd;
    case 0xf6: return 0x00f5; case 0xf7: return 0x00f8;
    case 0xf8: return 0x00fe;
    default: return 0xfffd;
    }
}

std::vector<std::uint32_t> decode_characters(
    const std::vector<std::uint8_t>& input, std::uint8_t charset)
{
    std::vector<std::uint32_t> output;
    if (charset == 0x0f) {
        for (std::size_t i = 0; i < input.size();) {
            const auto first = input[i++];
            if (first < 0x80) output.push_back(first);
            else if ((first & 0xe0) == 0xc0 && i < input.size()) {
                output.push_back(((first & 0x1f) << 6) | (input[i++] & 0x3f));
            } else if ((first & 0xf0) == 0xe0 && i + 1 < input.size()) {
                output.push_back(((first & 0x0f) << 12) |
                    ((input[i] & 0x3f) << 6) | (input[i + 1] & 0x3f));
                i += 2;
            } else output.push_back(0xfffd);
        }
    } else if (charset == 0x06) {
        for (std::size_t i = 0; i + 1 < input.size(); i += 2)
            output.push_back((std::uint32_t(input[i]) << 8) | input[i + 1]);
    } else {
        for (const auto byte : input)
            output.push_back(charset == 0x04 ? byte : ebu_latin(byte));
    }
    return output;
}

std::string encode_utf8(const std::vector<std::uint32_t>& input,
                        std::size_t start = 0,
                        std::size_t count = std::size_t(-1))
{
    std::string output;
    const auto end = std::min(input.size(), start + std::min(count, input.size()));
    for (std::size_t i = start; i < end; ++i) append_utf8(output, input[i]);
    while (!output.empty() && (output.back() == ' ' || output.back() == '\0'))
        output.pop_back();
    return output;
}

} // namespace

bool DynamicLabel::operator==(const DynamicLabel& other) const
{
    return text == other.text && dl_plus == other.dl_plus &&
           item_running == other.item_running;
}

bool DynamicLabel::operator!=(const DynamicLabel& other) const
{
    return !(*this == other);
}

std::string dl_plus_content_type_name(std::uint8_t type)
{
    static const char* names[] = {
        "", "Title", "Album", "Track", "Artist", "Composition",
        "Movement", "Conductor", "Composer", "Band", "Comment", "Genre",
        "News", "Local news", "Stock market", "Sport", "Lottery",
        "Horoscope", "Daily diversion", "Health", "Event", "Scene",
        "Cinema", "TV", "Date/time", "Weather", "Traffic", "Alarm",
        "Advertisement", "URL", "Other info", "Station short",
        "Station", "Programme now", "Programme next", "Programme part",
        "Host", "Editorial staff", "Frequency", "Homepage", "Subchannel",
        "Hotline", "Studio phone", "Phone", "Studio SMS", "SMS",
        "Hotline email", "Studio email", "Email", "MMS", "Chat",
        "Chat centre", "Vote question", "Vote centre", "Type 54", "Type 55",
        "Private 1", "Private 2", "Private 3", "Place", "Appointment",
        "Identifier", "Purchase", "Get data"};
    if (type < sizeof(names) / sizeof(names[0])) return names[type];
    return "Type " + std::to_string(type);
}

void DynamicLabelDecoder::reset()
{
    label_bytes_.clear();
    command_bytes_.clear();
    charset_ = 0;
    label_segment_ = command_segment_ = 0;
    label_assembling_ = command_assembling_ = false;
    label_toggle_ = command_link_ = -1;
    current_ = {};
    emitted_.reset();
}

std::optional<DynamicLabel> DynamicLabelDecoder::emit_if_changed(
    DynamicLabel value)
{
    if (emitted_ && *emitted_ == value) return std::nullopt;
    emitted_ = value;
    return value;
}

std::optional<DynamicLabel> DynamicLabelDecoder::push_data_group(
    const std::uint8_t* data, std::size_t size)
{
    if (!data || size < 2) return std::nullopt;
    if ((data[0] & 0x10) == 0) return push_label(data, size);
    if ((data[0] & 0x0f) == 1) {
        current_ = {};
        label_bytes_.clear();
        command_bytes_.clear();
        label_assembling_ = command_assembling_ = false;
        return emit_if_changed(current_);
    }
    if ((data[0] & 0x0f) == 2) return push_command(data, size);
    return std::nullopt;
}

std::optional<DynamicLabel> DynamicLabelDecoder::push_label(
    const std::uint8_t* data, std::size_t size)
{
    const std::size_t count = (data[0] & 0x0f) + 1;
    if (size < 2 + count) return std::nullopt;
    const bool first = (data[0] & 0x40) != 0;
    if (first) {
        label_bytes_.clear();
        label_segment_ = 0;
        label_assembling_ = true;
        charset_ = (data[1] >> 4) & 0x0f;
    } else if (!label_assembling_ || label_bytes_.empty() ||
               ((data[1] >> 4) & 0x07) != label_segment_) {
        label_bytes_.clear();
        label_segment_ = 0;
        label_assembling_ = false;
        return std::nullopt;
    }
    if (label_bytes_.size() + count > maximum_label_bytes) {
        label_bytes_.clear();
        label_assembling_ = false;
        return std::nullopt;
    }
    label_bytes_.insert(label_bytes_.end(), data + 2, data + 2 + count);
    ++label_segment_;
    if ((data[0] & 0x20) == 0) return std::nullopt;
    label_assembling_ = false;
    const int incoming_toggle = (data[0] >> 7) & 1;
    const auto incoming_text =
        encode_utf8(decode_characters(label_bytes_, charset_));
    // The carousel normally repeats the same DLS before repeating its DL+
    // command. Keep the already decoded tags across that duplicate DLS to
    // avoid alternating Artist/Title between filled and empty once per cycle.
    if (incoming_toggle != label_toggle_ || incoming_text != current_.text) {
        current_.dl_plus.clear();
        current_.item_running = false;
    }
    label_toggle_ = incoming_toggle;
    current_.text = incoming_text;
    return emit_if_changed(current_);
}

std::optional<DynamicLabel> DynamicLabelDecoder::push_command(
    const std::uint8_t* data, std::size_t size)
{
    if (current_.text.empty()) return std::nullopt;
    const std::size_t count = (data[1] & 0x0f) + 1;
    if (size < 2 + count) return std::nullopt;
    const int link = (data[1] >> 7) & 1;
    const bool first = (data[0] & 0x40) != 0;
    if (first) {
        command_bytes_.clear();
        command_segment_ = 0;
        command_assembling_ = true;
        command_link_ = link;
    } else if (!command_assembling_ || command_bytes_.empty() ||
               command_link_ != link ||
               ((data[1] >> 4) & 0x07) != command_segment_) {
        command_bytes_.clear();
        command_segment_ = 0;
        command_assembling_ = false;
        return std::nullopt;
    }
    if (command_bytes_.size() + count > maximum_label_bytes) {
        command_bytes_.clear();
        command_assembling_ = false;
        return std::nullopt;
    }
    command_bytes_.insert(command_bytes_.end(), data + 2, data + 2 + count);
    ++command_segment_;
    if ((data[0] & 0x20) == 0) return std::nullopt;
    command_assembling_ = false;
    if (command_link_ != label_toggle_ ||
        command_bytes_.empty() || (command_bytes_[0] >> 4) != 0)
        return std::nullopt;
    const std::size_t tags = (command_bytes_[0] & 0x03) + 1;
    if (command_bytes_.size() < 1 + 3 * tags) return std::nullopt;
    const auto characters = decode_characters(label_bytes_, charset_);
    std::map<std::uint8_t, std::string> parsed;
    for (std::size_t index = 0; index < tags; ++index) {
        const std::uint8_t type = command_bytes_[1 + 3 * index] & 0x7f;
        const std::size_t start = command_bytes_[2 + 3 * index] & 0x7f;
        const std::size_t length =
            (command_bytes_[3 + 3 * index] & 0x7f) + 1;
        if (type == 0 || start >= characters.size() ||
            start + length > characters.size())
            continue;
        parsed[type] = encode_utf8(characters, start, length);
    }
    current_.dl_plus = std::move(parsed);
    current_.item_running = (command_bytes_[0] & 0x04) != 0;
    return emit_if_changed(current_);
}

} // namespace dab
