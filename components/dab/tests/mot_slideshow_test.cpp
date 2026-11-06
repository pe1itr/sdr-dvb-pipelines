#include "mot_slideshow.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<std::uint8_t> group(
    unsigned type, std::uint16_t transport_id,
    const std::vector<std::uint8_t>& payload, bool with_crc = false)
{
    std::vector<std::uint8_t> result{
        std::uint8_t(0x30 | (with_crc ? 0x40 : 0) | type), 0x00,
        0x80, 0x00, // last segment, segment zero
        0x12,       // transport ID present, two-byte user-access field
        std::uint8_t(transport_id >> 8), std::uint8_t(transport_id),
        std::uint8_t((payload.size() >> 8) & 0x1f),
        std::uint8_t(payload.size())};
    result.insert(result.end(), payload.begin(), payload.end());
    if (with_crc) {
        std::uint16_t crc = 0xffff;
        for (const auto byte : result) {
            for (int bit = 7; bit >= 0; --bit) {
                const std::uint16_t input = (byte >> bit) & 1U;
                const std::uint16_t feedback =
                    input ^ ((crc >> 15) & 1U);
                crc = std::uint16_t(crc << 1);
                if (feedback) crc ^= 0x1021;
            }
        }
        const std::uint16_t transmitted = std::uint16_t(~crc);
        result.push_back(std::uint8_t(transmitted >> 8));
        result.push_back(std::uint8_t(transmitted));
    }
    return result;
}

std::vector<std::uint8_t> header(
    std::size_t body_size, unsigned subtype, const std::string& name)
{
    const std::size_t parameter_length = name.size() + 1;
    const std::size_t header_size = 7 + 2 + parameter_length;
    std::vector<std::uint8_t> result(7);
    result[0] = std::uint8_t(body_size >> 20);
    result[1] = std::uint8_t(body_size >> 12);
    result[2] = std::uint8_t(body_size >> 4);
    result[3] = std::uint8_t((body_size << 4) | (header_size >> 9));
    result[4] = std::uint8_t(header_size >> 1);
    result[5] = std::uint8_t(((header_size & 1) << 7) |
                             (2U << 1) | (subtype >> 8));
    result[6] = std::uint8_t(subtype);
    result.push_back(0xcc); // variable-length ContentName
    result.push_back(std::uint8_t(parameter_length));
    result.push_back(0xf0); // UTF-8 character set selector
    result.insert(result.end(), name.begin(), name.end());
    return result;
}

} // namespace

int main()
{
    dab::MotSlideshowDecoder decoder;
    const std::vector<std::uint8_t> png_body{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    const auto body_group = group(4, 0x1234, png_body, true);
    const auto header_group = group(
        3, 0x1234, header(png_body.size(), 3, "logo.png"));

    // Body and header may arrive in either order in the MOT carousel.
    auto corrupt = body_group;
    corrupt.back() ^= 1;
    assert(!decoder.push_data_group(corrupt.data(), corrupt.size()));
    assert(!decoder.push_data_group(
        body_group.data(), body_group.size()));
    const auto slide = decoder.push_data_group(
        header_group.data(), header_group.size());
    assert(slide);
    assert(slide->format == dab::MotSlide::Format::Png);
    assert(slide->content_name == "logo.png");
    assert(slide->encoded == png_body);

    // Unsupported image subtypes are ignored without retaining bad state.
    const auto unsupported = group(
        3, 9, header(png_body.size(), 7, "unknown.bin"));
    assert(!decoder.push_data_group(unsupported.data(), unsupported.size()));
    decoder.reset();

    std::cout << "mot-slideshow-test: PASS\n";
}
