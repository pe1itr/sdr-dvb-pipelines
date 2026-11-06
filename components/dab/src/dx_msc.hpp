// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dab_ensemble.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace dab::dx
{
// Positive soft values mean bit zero; zero is an erasure throughout this API.
struct EepProfile
{
    unsigned bitrate = 0, coded_bits = 0;
    std::vector<unsigned> transmitted_positions;
};
EepProfile eep_profile(const MuxSubchannelInfo &sub);
std::vector<std::uint8_t> decode_msc_word(const float *soft, const EepProfile &profile);
std::uint16_t crc16(const std::uint8_t *data, std::size_t size);
int correct_rs120(std::array<std::uint8_t, 120> &word);

class MscSubchannel
{
  public:
    bool configure(const MuxSubchannelInfo &sub);
    void reset();
    // One 55,296-bit CIF. Empty output while the 16-CIF interleaver fills.
    std::vector<std::uint8_t> push(const float *cif, std::size_t count);

  private:
    EepProfile profile_;
    unsigned start_ = 0;
    std::array<std::vector<float>, 16> history_;
    std::uint64_t count_ = 0;
};

struct AudioUnit
{
    std::uint8_t header = 0;
    bool damaged = false;
    std::vector<std::uint8_t> bytes;
};
class SuperframeDecoder
{
  public:
    std::vector<AudioUnit> push(std::vector<std::uint8_t> logical_frame);
    void reset();
    std::uint64_t good = 0, rejected = 0, corrected_bytes = 0, bad_units = 0;

  private:
    std::deque<std::vector<std::uint8_t>> frames_;
};
} // namespace dab::dx
