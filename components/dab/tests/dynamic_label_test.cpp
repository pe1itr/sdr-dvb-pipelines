#include "dynamic_label.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> label_group(
    const std::string& text, bool toggle = false, std::uint8_t charset = 0)
{
    assert(!text.empty() && text.size() <= 16);
    std::vector<std::uint8_t> group{
        std::uint8_t((toggle ? 0x80 : 0) | 0x60 | (text.size() - 1)),
        std::uint8_t(charset << 4)};
    group.insert(group.end(), text.begin(), text.end());
    return group;
}

std::vector<std::uint8_t> dl_plus_group(bool link = false)
{
    // ITEM_ARTIST covers "Artist"; ITEM_TITLE covers "Title" in the label
    // "Artist - Title". Length markers carry length minus one.
    return {
        0x72, std::uint8_t((link ? 0x80 : 0) | 0x06),
        0x0d, 4, 0, 5, 1, 9, 4};
}

} // namespace

int main()
{
    assert(dab::dl_plus_content_type_name(1) == "Title");
    assert(dab::dl_plus_content_type_name(4) == "Artist");
    assert(dab::dl_plus_content_type_name(25) == "Weather");

    dab::DynamicLabelDecoder decoder;
    const auto label = label_group("Artist - Title");
    auto update = decoder.push_data_group(label.data(), label.size());
    assert(update && update->text == "Artist - Title");
    assert(update->dl_plus.empty());

    // A carousel repeat must not generate an update or UI redraw.
    assert(!decoder.push_data_group(label.data(), label.size()));

    const auto plus = dl_plus_group();
    update = decoder.push_data_group(plus.data(), plus.size());
    assert(update && update->item_running);
    assert(update->dl_plus.at(4) == "Artist");
    assert(update->dl_plus.at(1) == "Title");
    assert(!decoder.push_data_group(plus.data(), plus.size()));
    // Repeating the underlying DLS must not temporarily erase its DL+ tags.
    assert(!decoder.push_data_group(label.data(), label.size()));

    // A DL+ command linked to the opposite DLS toggle is not applicable.
    const auto wrong_link = dl_plus_group(true);
    assert(!decoder.push_data_group(wrong_link.data(), wrong_link.size()));

    // A missing first segment cannot leak fragments into the displayed label.
    const std::vector<std::uint8_t> orphan{0x21, 0x10, 'x', 'x'};
    assert(!decoder.push_data_group(orphan.data(), orphan.size()));

    // UTF-8 is preserved and DL+ offsets are interpreted as characters.
    decoder.reset();
    const auto utf8 = label_group("Caf\xc3\xa9", false, 0x0f);
    update = decoder.push_data_group(utf8.data(), utf8.size());
    assert(update && update->text == "Caf\xc3\xa9");

    const std::vector<std::uint8_t> clear{0x11, 0x00};
    update = decoder.push_data_group(clear.data(), clear.size());
    assert(update && update->text.empty() && update->dl_plus.empty());

    std::cout << "dynamic-label-test: PASS\n";
}
