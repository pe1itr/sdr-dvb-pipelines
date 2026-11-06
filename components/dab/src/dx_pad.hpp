// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "dynamic_label.hpp"
#include "mot_slideshow.hpp"
#include <functional>

namespace dab::dx
{
// DAB+ DSE extraction: TS 102 563 V2.1.1, 5.4.1/5.4.3.
// X-PAD transport: EN 300 401 V2.1.1, 7.4.1--7.4.5.
class PadDecoder
{
  public:
    void reset();
    void push(const std::vector<std::uint8_t> &au);
    std::function<void(DynamicLabel)> label;
    std::function<void(MotSlide)> slide;

  private:
    void field(unsigned type, const std::uint8_t *bytes, std::size_t size);
    unsigned last_type_ = 0, expected_group_ = 0;
    std::vector<std::uint8_t> dls_, length_, mot_;
    DynamicLabelDecoder labels_;
    MotSlideshowDecoder slides_;
};
} // namespace dab::dx
