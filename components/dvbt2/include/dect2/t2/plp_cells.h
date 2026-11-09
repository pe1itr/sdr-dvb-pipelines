#pragma once
#include "dect2/common/ring.h"
#include <algorithm>
#include <limits>
#include <vector>

namespace dect2 {

// Select from the logical concatenation of gathered P2 and data-symbol cells.
// The job owns its output: the frontend may reuse the data-symbol buffers as
// soon as this returns. Avoid materialising the full concatenated stream.
inline bool selectPlpCells(const std::vector<cf32>& p2, const std::vector<cf32>& p2Power,
                           const std::vector<cf32>& data, const std::vector<float>& dataNoise,
                           size_t start, size_t count, double sigma2,
                           std::vector<cf32>& cells, std::vector<float>& noise) {
    if (p2Power.size() != p2.size() || dataNoise.size() != data.size() ||
        data.size() > std::numeric_limits<size_t>::max() - p2.size()) return false;
    const size_t total = p2.size() + data.size();
    if (start > total || count > total - start) return false;
    const size_t p2Count = start < p2.size() ? std::min(count, p2.size() - start) : 0;
    const size_t dataStart = start < p2.size() ? 0 : start - p2.size();
    const size_t dataCount = count - p2Count;
    cells.clear(); noise.clear();
    cells.reserve(count); noise.reserve(count);
    if (p2Count) {
        cells.insert(cells.end(), p2.begin() + start, p2.begin() + start + p2Count);
        for (size_t i = start; i < start + p2Count; ++i)
            noise.push_back((float)(sigma2 / (2.0 * std::max(1e-12f, p2Power[i].real()))));
    }
    if (dataCount) {
        cells.insert(cells.end(), data.begin() + dataStart, data.begin() + dataStart + dataCount);
        noise.insert(noise.end(), dataNoise.begin() + dataStart, dataNoise.begin() + dataStart + dataCount);
    }
    return true;
}

} // namespace dect2
