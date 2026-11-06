#pragma once
#include "channel_filter.hpp"
#include "dect2/dsp/resampler.h"
#include <samplerate.h>
#include <cmath>
#include <memory>
#include <stdexcept>

// Reduce wide IQ with the DAB anti-alias FIR first. Only the remaining
// fractional ratio needs a polyphase or two-channel (I/Q) sinc resampler.
class DabResampler {
    std::unique_ptr<dab::StreamingChannelizer> decimator;
    std::unique_ptr<SRC_STATE,decltype(&src_delete)> fractional{nullptr,src_delete};
    double ratio=1;
    std::unique_ptr<dect2::RationalResampler> polyphase;
    std::vector<std::complex<float>> reduced;
public:
    explicit DabResampler(unsigned rate) {
        if (rate<2048000 || rate>100000000) throw std::invalid_argument("DAB input rate outside 2.048..100 MS/s");
        unsigned factor=rate/2048000;
        double intermediate=double(rate)/factor;
        if (factor>1) decimator=std::make_unique<dab::StreamingChannelizer>(rate,intermediate);
        ratio=2048000.0/intermediate;
        if (rate%2048000) {
            // Reuse OnAir's generic DSP resampler already vendored for DVB-T2.
            // After the DAB anti-alias FIR only the fractional conversion remains.
            // Never approximate an unusual hardware rate: even a small persistent
            // rate error smears DAB carriers over time. Keep the exact-rate fallback.
            polyphase=std::make_unique<dect2::RationalResampler>();
            polyphase->allowFastX86(true);
            double error_ratio=0;
            if (!polyphase->configure(intermediate,2048000,&error_ratio,1600000) || error_ratio>1e-10) {
                polyphase.reset();
                int error=0;fractional.reset(src_new(SRC_SINC_MEDIUM_QUALITY,2,&error));
                if (!fractional) throw std::runtime_error(src_strerror(error));
            }
        }
    }
    void reset() {if (decimator) decimator->reset();if (polyphase) polyphase->reset();if (fractional) src_reset(fractional.get());}
    void process(std::vector<std::complex<float>>& samples) {
        if (decimator) {decimator->process(samples.data(),samples.size(),reduced);samples.swap(reduced);}
        if (polyphase) {
            reduced.clear();polyphase->process(samples.data(),samples.size(),reduced);samples.swap(reduced);return;
        }
        if (!fractional || samples.empty()) return;
        reduced.resize(size_t(std::ceil(samples.size()*ratio))+256);
        SRC_DATA data{};
        data.data_in=reinterpret_cast<const float*>(samples.data());data.input_frames=samples.size();
        data.data_out=reinterpret_cast<float*>(reduced.data());data.output_frames=reduced.size();data.src_ratio=ratio;
        int error=src_process(fractional.get(),&data);
        if (error || data.input_frames_used!=data.input_frames)
            throw std::runtime_error(error ? src_strerror(error) : "DAB resampler output capacity exceeded");
        reduced.resize(data.output_frames_gen);samples.swap(reduced);
    }
};
