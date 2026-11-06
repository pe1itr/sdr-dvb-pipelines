#include "channel_filter.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <numeric>
#include <vector>

int main()
{
    constexpr double sample_rate = 2048000.0;
    const auto taps = dab::make_channel_filter_taps(sample_rate);
    assert(taps.size() == 81);
    const double dc = std::accumulate(taps.begin(), taps.end(), 0.0);
    assert(std::abs(dc - 1.0) < 1e-6);
    assert(std::abs(dab::channel_filter_response_db(
        taps, sample_rate, 0.0)) < 0.001);
    assert(dab::channel_filter_response_db(
        taps, sample_rate, 768000.0) > -0.10);
    assert(dab::channel_filter_response_db(
        taps, sample_rate, 850000.0) < -40.0);
    assert(dab::channel_filter_response_db(
        taps, sample_rate, 944000.0) < -40.0);

    // Centred filtering must not move an impulse in sample time.
    std::vector<std::complex<float>> impulse(257);
    impulse[128] = {1.0f, 0.0f};
    dab::apply_channel_filter(impulse, sample_rate);
    std::size_t peak = 0;
    for (std::size_t index = 1; index < impulse.size(); ++index)
        if (std::abs(impulse[index]) > std::abs(impulse[peak])) peak = index;
    assert(peak == 128);

    // The streaming form must be continuous across arbitrary input chunks.
    // It is causal, so it equals the centred form after the FIR group
    // delay, away from the unflushed end boundary.
    std::vector<std::complex<float>> input(257);
    for (std::size_t index = 0; index < input.size(); ++index)
        input[index] = {float(std::sin(0.17 * double(index))),
                        float(std::cos(0.11 * double(index)))};
    auto centred = input;
    dab::apply_channel_filter(centred, sample_rate);
    dab::StreamingChannelFilter streaming(sample_rate);
    std::vector<std::complex<float>> first, second;
    streaming.process(input.data(), 73, first);
    streaming.process(input.data() + 73, input.size() - 73, second);
    first.insert(first.end(), second.begin(), second.end());
    const std::size_t delay = taps.size() / 2;
    for (std::size_t index = 0; index + delay < input.size(); ++index)
        assert(std::abs(first[index + delay] - centred[index]) < 2e-6f);

    constexpr double wide_rate = 4096000.0;
    const auto wide_taps = dab::make_channel_filter_taps(wide_rate);
    assert(wide_taps.size() == 161);
    assert(dab::channel_filter_response_db(
        wide_taps, wide_rate, 768000.0) > -0.10);
    assert(dab::channel_filter_response_db(
        wide_taps, wide_rate, 944000.0) < -40.0);

    const auto channelized_rms = [](double frequency_hz) {
        constexpr std::size_t count = 65536;
        std::vector<std::complex<float>> signal(count);
        for (std::size_t index = 0; index < count; ++index)
            signal[index] = std::polar(
                1.0f, float(2.0 * M_PI * frequency_hz * index / wide_rate));
        dab::channelize_to_mode_i(signal, wide_rate, sample_rate);
        double power = 0.0;
        const std::size_t edge = 256;
        for (std::size_t index = edge; index + edge < signal.size(); ++index)
            power += std::norm(signal[index]);
        return std::sqrt(power / double(signal.size() - 2 * edge));
    };
    const double wanted_rms = channelized_rms(500000.0);
    const double adjacent_edge_rms = channelized_rms(944000.0);
    // 9D is 1.872 MHz below 10A. Without pre-decimation filtering it would
    // fold to 176 kHz at 2.048 MS/s, directly inside the wanted channel.
    const double folded_9d_rms = channelized_rms(1872000.0);
    assert(wanted_rms > 0.99);
    assert(adjacent_edge_rms < 0.01);
    assert(folded_9d_rms < 0.01);

    // Streaming channelization must be chunk-independent and equal the
    // centred block channelizer after its normal causal group delay.
    std::vector<std::complex<float>> wide_input(8192);
    for (std::size_t index = 0; index < wide_input.size(); ++index)
        wide_input[index] = {float(std::sin(0.071 * double(index))),
                             float(std::cos(0.053 * double(index)))};
    auto block_output = wide_input;
    dab::channelize_to_mode_i(block_output, wide_rate, sample_rate);

    std::vector<std::complex<float>> cancelled_signal(1 << 20, {1.0f, 0.0f});
    std::size_t cancellation_polls = 0;
    const auto cancelled_size = cancelled_signal.size();
    const bool completed = dab::channelize_to_mode_i(
        cancelled_signal, wide_rate, sample_rate, [&] {
            return ++cancellation_polls >= 2;
        });
    if (completed || cancellation_polls < 2 ||
        cancelled_signal.size() != cancelled_size) {
        std::cerr << "channelizer cancellation failed\n";
        return 1;
    }
    dab::StreamingChannelizer live_channelizer(wide_rate, sample_rate);
    std::vector<std::complex<float>> live_first, live_second;
    live_channelizer.process(wide_input.data(), 3011, live_first);
    live_channelizer.process(wide_input.data() + 3011,
                             wide_input.size() - 3011, live_second);
    live_first.insert(live_first.end(), live_second.begin(), live_second.end());
    const std::size_t output_delay = wide_taps.size() / 2 / 2;
    for (std::size_t index = 128;
         index + output_delay + 128 < live_first.size(); ++index)
        assert(std::abs(live_first[index + output_delay] -
                        block_output[index]) < 3e-6f);

    std::cout << "channel-filter-test: PASS"
              << " edge768=" << dab::channel_filter_response_db(
                     taps, sample_rate, 768000.0)
              << " stop850=" << dab::channel_filter_response_db(
                     taps, sample_rate, 850000.0)
              << " adjacent944=" << dab::channel_filter_response_db(
                     taps, sample_rate, 944000.0)
              << " wide_adjacent944=" << dab::channel_filter_response_db(
                     wide_taps, wide_rate, 944000.0)
              << " wanted_rms=" << wanted_rms
              << " folded_9d_rms=" << folded_9d_rms
              << '\n';
}
