#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dab {

struct DynamicLabel {
    std::string text;
    std::map<std::uint8_t, std::string> dl_plus;
    bool item_running = false;

    bool operator==(const DynamicLabel& other) const;
    bool operator!=(const DynamicLabel& other) const;
};

std::string dl_plus_content_type_name(std::uint8_t type);

// Reassembles the bounded XPAD Dynamic Label and DL Plus data groups delivered
// by the PAD transport after it checks the data-group CRC.
class DynamicLabelDecoder {
public:
    std::optional<DynamicLabel> push_data_group(
        const std::uint8_t* data, std::size_t size);
    void reset();

private:
    std::optional<DynamicLabel> emit_if_changed(DynamicLabel value);
    std::optional<DynamicLabel> push_label(
        const std::uint8_t* data, std::size_t size);
    std::optional<DynamicLabel> push_command(
        const std::uint8_t* data, std::size_t size);

    std::vector<std::uint8_t> label_bytes_;
    std::vector<std::uint8_t> command_bytes_;
    std::uint8_t charset_ = 0;
    std::uint8_t label_segment_ = 0;
    std::uint8_t command_segment_ = 0;
    bool label_assembling_ = false;
    bool command_assembling_ = false;
    int label_toggle_ = -1;
    int command_link_ = -1;
    DynamicLabel current_;
    std::optional<DynamicLabel> emitted_;
};

} // namespace dab
