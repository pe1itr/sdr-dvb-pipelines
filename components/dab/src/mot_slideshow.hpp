#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dab {

struct MotSlide {
    enum class Format { Jpeg, Png };

    Format format = Format::Jpeg;
    std::string content_name;
    std::vector<std::uint8_t> encoded;
};

// Bounded ETSI EN 301 234 header-mode MOT reassembler for DAB Slideshow.
// Directory mode, compressed and conditional-access objects are deliberately
// ignored. The decoder is independent from X11 and can be tested with raw MSC
// data groups.
class MotSlideshowDecoder {
public:
    std::optional<MotSlide> push_data_group(
        const std::uint8_t* data, std::size_t size);
    void reset();

    // Public for deterministic parser tests; callers normally only use
    // push_data_group().
    struct Entity {
        std::map<std::uint16_t, std::vector<std::uint8_t>> segments;
        std::optional<std::uint16_t> last;
        std::size_t size = 0;

        bool add(std::uint16_t number, bool is_last,
                 const std::uint8_t* data, std::size_t length);
        bool complete() const;
        std::vector<std::uint8_t> assemble() const;
    };
    struct Object {
        Entity header;
        Entity body;
    };

private:
    std::map<std::uint16_t, Object> objects_;
};

} // namespace dab
