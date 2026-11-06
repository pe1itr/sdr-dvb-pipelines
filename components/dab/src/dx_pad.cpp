// SPDX-License-Identifier: GPL-3.0-only
// Original PAD transport implementation. Protocol references:
// ETSI TS 102 563 V2.1.1 (2017-01), clauses 5.4.1 and 5.4.3 (leading AAC DSE).
// ETSI EN 300 401 V2.1.1 (2017-01), clauses 7.4.1--7.4.5, table 11 (X-PAD).
// https://www.etsi.org/deliver/etsi_ts/102500_102599/102563/02.01.01_60/ts_102563v020101p.pdf
// https://www.etsi.org/deliver/etsi_en/300400_300499/300401/02.01.01_60/en_300401v020101p.pdf
// DL Plus: ETSI TS 102 980 V2.1.1 (2017-01), 7.1.
// https://www.etsi.org/deliver/etsi_ts/102900_102999/102980/02.01.01_60/ts_102980v020101p.pdf
#include "dx_pad.hpp"
#include "dx_msc.hpp"
#include <algorithm>

namespace dab::dx
{
void PadDecoder::reset()
{
    last_type_ = expected_group_ = 0;
    dls_.clear();
    length_.clear();
    mot_.clear();
    labels_.reset();
    slides_.reset();
}
void PadDecoder::field(unsigned type, const std::uint8_t *data, std::size_t n)
{
    auto append = [&](auto &buffer)
    {
        if (buffer.size() + n > 16384)
        {
            buffer.clear();
            return false;
        }
        buffer.insert(buffer.end(), data, data + n);
        return true;
    };
    auto valid = [](const auto &b, unsigned size)
    {
        return size >= 2 && crc16(b.data(), size - 2) == unsigned((b[size - 2] << 8) | b[size - 1]);
    };
    if (type == 2 || type == 3)
    {
        if (type == 2)
            dls_.clear();
        else if (dls_.empty())
            return;
        if (!append(dls_) || dls_.size() < 2)
            return;
        unsigned size = 2 + (dls_[0] & 15) + 1 + 2;
        if (dls_[0] & 16)
        {
            if ((dls_[0] & 15) == 1)
                size = 4;
            else if ((dls_[0] & 15) == 2)
            {
                // TS 102 980 V2.1.1, 7.1: Field 3 is the low nibble
                // of the second prefix byte, not the DL Plus tag header.
                size = 2 + (dls_[1] & 15) + 1 + 2;
            }
            else
            {
                dls_.clear();
                return;
            }
        }
        if (dls_.size() >= size)
        {
            if (valid(dls_, size))
            {
                auto value = labels_.push_data_group(dls_.data(), size - 2);
                if (value && label)
                    label(std::move(*value));
            }
            dls_.clear();
        }
    }
    else if (type == 1)
    {
        if (length_.size() >= 4)
            length_.clear();
        if (!append(length_) || length_.size() < 4)
            return;
        expected_group_ = valid(length_, 4) ? unsigned((length_[0] & 63) * 256 + length_[1]) : 0;
    }
    else if (type == 12 || type == 13)
    {
        if (type == 12)
            mot_.clear();
        else if (mot_.empty())
            return;
        if (!expected_group_ || !append(mot_))
            return;
        if (mot_.size() >= expected_group_)
        {
            // The MOT parser checks the MSC data-group CRC and transport IDs.
            auto image = slides_.push_data_group(mot_.data(), expected_group_);
            if (image && slide)
                slide(std::move(*image));
            mot_.clear();
            expected_group_ = 0;
        }
    }
}
void PadDecoder::push(const std::vector<std::uint8_t> &au)
{
    if (au.size() < 2 || (au[0] >> 5) != 4)
        return;
    unsigned count = au[1], offset = 2;
    if (count == 255)
    {
        if (au.size() < 3)
            return;
        count += au[2];
        offset = 3;
    }
    if (count < 2 || count > 198 || offset + count > au.size())
        return;
    const auto *pad = au.data() + offset;
    const auto f0 = pad[count - 2], f1 = pad[count - 1];
    if (f0 & 0xc0)
        return;
    unsigned kind = (f0 >> 4) & 3;
    if (kind != 1 && kind != 2)
        return;
    std::vector<std::uint8_t> x(pad, pad + count - 2);
    std::reverse(x.begin(), x.end());
    unsigned pos = 0;
    std::vector<std::pair<unsigned, unsigned>> fields;
    if (f1 & 2)
    {
        if (kind == 1)
        {
            if (x.size() != 4)
                return;
            fields.push_back({x[pos++] & 31, 3});
        }
        else
        {
            constexpr unsigned lengths[] = {4, 6, 8, 12, 16, 24, 32, 48};
            for (unsigned i = 0; i < 4; ++i)
            {
                if (pos >= x.size())
                    return;
                auto ci = x[pos++];
                if (!(ci & 31))
                    break;
                fields.push_back({ci & 31, lengths[ci >> 5]});
            }
        }
    }
    else if (last_type_)
        fields.push_back({last_type_, unsigned(x.size())});
    // Validate all sub-fields before changing assembly state.
    unsigned end = pos;
    for (auto [type, n] : fields)
    {
        (void)type;
        end += n;
    }
    if (end > x.size())
        return;
    for (auto [type, n] : fields)
    {
        if (!type)
        {
            last_type_ = 0;
            break;
        }
        field(type, x.data() + pos, n);
        pos += n;
        last_type_ = type == 2 ? 3 : type == 12 ? 13 : type;
    }
}
} // namespace dab::dx
