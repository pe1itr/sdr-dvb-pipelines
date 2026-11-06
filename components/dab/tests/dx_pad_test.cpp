// SPDX-License-Identifier: GPL-3.0-only
#include "dx_msc.hpp"
#include "dx_pad.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
using namespace dab::dx;
std::vector<std::uint8_t> au(std::vector<std::uint8_t> x, bool ci = true)
{
    std::reverse(x.begin(), x.end());
    x.push_back(0x20);
    x.push_back(ci ? 2 : 0);
    std::vector<std::uint8_t> out{0x80, std::uint8_t(x.size())};
    out.insert(out.end(), x.begin(), x.end());
    return out;
}
int main()
{
    PadDecoder p;
    std::string text;
    unsigned calls = 0;
    dab::DynamicLabel latest;
    p.label = [&](dab::DynamicLabel l)
    {
        text = l.text;
        latest = l;
        ++calls;
    };
    std::vector<std::uint8_t> label{0x63, 0, 'T', 'e', 's', 't'};
    auto crc = crc16(label.data(), label.size());
    label.push_back(crc >> 8);
    label.push_back(crc);
    std::vector<std::uint8_t> x{0x42, 0};
    x.insert(x.end(), label.begin(), label.end());
    p.push(au(x));
    assert(text == "Test" && calls == 1);
    p.reset();
    text.clear();
    x[4] ^= 1;
    p.push(au(x));
    assert(text.empty());
    // Fragmented group; continuation without a CI. No label from an orphan.
    p.reset();
    p.push(au({0x02, 0, label[0], label[1], label[2], label[3]}));
    assert(text.empty());
    p.push(au({label[4], label[5], label[6], label[7]}, false));
    assert(text == "Test");
    p.reset();
    text.clear();
    p.push(au({label[4], label[5], label[6], label[7]}, false));
    assert(text.empty());
    // TS 102 980 7.1: command length is in prefix byte 1, not tag byte 2.
    p.reset();
    text.clear();
    p.push(au(
        {0x42, 0, label[0], label[1], label[2], label[3], label[4], label[5], label[6], label[7]}));
    std::vector<std::uint8_t> command{0x72, 0x03, 0x0c, 1, 0, 3};
    auto cc = crc16(command.data(), command.size());
    command.push_back(cc >> 8);
    command.push_back(cc);
    std::vector<std::uint8_t> cx{0x42, 0};
    cx.insert(cx.end(), command.begin(), command.end());
    p.push(au(cx));
    assert(latest.item_running && latest.dl_plus.at(1) == "Test");
    for (unsigned n = 0; n < 198; ++n)
        p.push(std::vector<std::uint8_t>(n, 0xff));
    std::cout << "dx-pad: CRC, fragments, resets and bounds PASS\n";
}
