// SPDX-License-Identifier: GPL-3.0-only
// Independent DX radio integration. Demodulation/MSC/PAD are project sources;
// FAAD2 is used solely for MPEG-4 AAC decoding. No libdabsdr ABI is used.
#include "dab_plus_radio.hpp"
#include "dx_msc.hpp"
#include "dx_pad.hpp"
#include "libdxdabsdr.hpp"
#include <algorithm>
#include <alsa/asoundlib.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <neaacdec.h>
#include <stdexcept>
#include <thread>

namespace dab
{
namespace
{
constexpr std::size_t input_capacity = 4096000, audio_capacity = 64, block_size = 32768;
// A 120 ms DAB+ superframe is assembled from 24 ms CIFs, delivered here in
// 96 ms radio frames. Consecutive audio batches can therefore be 192 ms apart.
// Keep room for that batching plus scheduling jitter (also after XRUN recovery).
constexpr unsigned playback_latency_us = 300000;
std::string lower(std::string s)
{
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool matches(const std::string &selector, std::uint32_t sid, const std::string &name)
{
    if (selector == "0x0")
        return !name.empty();
    if (selector.size() > 2 && selector.substr(0, 2) == "0x")
    {
        std::size_t used = 0;
        auto value = std::stoul(selector, &used, 16);
        return used == selector.size() && value == sid;
    }
    return lower(selector) == lower(name);
}
} // namespace
std::vector<std::uint8_t> dab_plus_audio_specific_config(std::uint8_t header)
{
    // TS 102 563 V2.1.1 5.1: AAC-LC 960-sample core; SBR/PS explicitly
    // signalled using MPEG-4 AudioSpecificConfig (ISO/IEC 14496-3).
    const bool dac = header & 64, sbr = header & 32, stereo = header & 16, ps = header & 8;
    unsigned core = sbr ? (dac ? 6 : 8) : (dac ? 3 : 5), channels = stereo && !ps ? 2 : 1;
    std::vector<std::uint8_t> out;
    unsigned pos = 0;
    auto put = [&](unsigned value, unsigned n)
    {
        for (unsigned i = n; i-- > 0;)
        {
            if (pos % 8 == 0)
                out.push_back(0);
            out.back() |= ((value >> i) & 1) << (7 - pos % 8);
            ++pos;
        }
    };
    put(sbr ? (ps ? 29 : 5) : 2, 5);
    put(core, 4);
    put(channels, 4);
    if (sbr)
    {
        put(dac ? 3 : 5, 4);
        put(2, 5);
    }
    put(1, 1);
    put(0, 1);
    put(0, 1);
    return out;
}
struct DabPlusRadioSession::Impl
{
    struct Block
    {
        std::vector<std::complex<float>> samples;
        std::uint64_t epoch;
    };
    struct Audio
    {
        dx::AudioUnit unit;
        std::uint64_t generation;
    };
    std::string selector, device;
    bool playback;
    std::atomic<bool> stopping{false}, started{false}, muted{false}, synced{false};
    std::atomic<std::size_t> iq_drops{0}, audio_drops{0}, underruns{0}, decoded{0}, slides{0},
        service_generation{0};
    std::atomic<std::size_t> rejected_superframes{0}, damaged_units{0},
        aac_errors{0}, concealed_frames{0};
    std::atomic<std::uint64_t> generation{0};
    mutable std::mutex state_mutex, input_mutex, audio_mutex, error_mutex;
    std::condition_variable input_ready, audio_ready;
    std::deque<Block> input;
    std::deque<Audio> audio;
    std::size_t queued = 0, iq_peak = 0, audio_peak = 0;
    std::uint64_t input_epoch = 0;
    bool eof = false, audio_eof = false;
    std::vector<DabPlusService> stations;
    std::string selected, ensemble;
    std::uint32_t selected_sid = 0;
    std::optional<MotSlide> latest_slide;
    std::optional<DynamicLabel> latest_label;
    std::exception_ptr error;
    std::thread receiver, audio_player;
    dx::StreamDemodulator demod;
    dx::MscSubchannel msc;
    dx::SuperframeDecoder superframes;
    dx::PadDecoder pad;
    std::string routing;
    std::uint64_t receiver_generation = 0;
    explicit Impl(std::string sel, std::string dev, bool play)
        : selector(std::move(sel)), device(std::move(dev)), playback(play)
    {
        pad.label = [this](DynamicLabel value)
        {
            std::lock_guard<std::mutex> l(state_mutex);
            if (receiver_generation == generation)
                latest_label = std::move(value);
        };
        pad.slide = [this](MotSlide value)
        {
            std::lock_guard<std::mutex> l(state_mutex);
            if (receiver_generation == generation)
            {
                latest_slide = std::move(value);
                ++slides;
            }
        };
    }
    void fail()
    {
        std::lock_guard<std::mutex> l(error_mutex);
        if (!error)
            error = std::current_exception();
    }
    void reset_transport()
    {
        msc.reset();
        superframes.reset();
        pad.reset();
        routing.clear();
    }
    void frame(dx::StreamFrame &&f)
    {
        synced = f.fic_valid;
        std::string wanted;
        {
            std::lock_guard<std::mutex> l(state_mutex);
            wanted = selector;
            if (receiver_generation != generation)
            {
                receiver_generation = generation;
                reset_transport();
            }
            if (f.discontinuity)
            {
                receiver_generation = ++generation;
                reset_transport();
                selected.clear();
                latest_slide.reset();
                latest_label = DynamicLabel{};
            }
            ensemble = f.ensemble.label;
            std::vector<DabPlusService> found;
            for (const auto &s : f.ensemble.metadata.services)
                for (const auto &c : s.components)
                    if (!c.transport_mode && c.component_type == 63 && !c.conditional_access &&
                        c.subchannel_id)
                        for (const auto &sub : f.ensemble.metadata.subchannels)
                            if (sub.id == *c.subchannel_id)
                            {
                                const auto rate = dx::eep_profile(sub).bitrate;
                                if (rate && rate <= 192)
                                    found.push_back({s.service_id, c.index, s.label,
                                                     std::uint16_t(rate), c.primary});
                            }
            bool changed = found.size() != stations.size();
            for (unsigned i = 0; !changed && i < found.size(); ++i)
                changed = found[i].label != stations[i].label ||
                          found[i].service_id != stations[i].service_id;
            stations = std::move(found);
            if (changed)
                ++service_generation;
        }
        const MuxSubchannelInfo *chosen = nullptr;
        std::string name, key;
        std::uint32_t sid = 0;
        for (const auto &s : f.ensemble.metadata.services)
        {
            if (!matches(wanted, s.service_id, s.label))
                continue;
            for (const auto &c : s.components)
                if (c.transport_mode == 0 && c.component_type == 63 && !c.conditional_access &&
                    c.subchannel_id)
                    for (const auto &sub : f.ensemble.metadata.subchannels)
                        if (sub.id == *c.subchannel_id && dx::eep_profile(sub).bitrate &&
                            dx::eep_profile(sub).bitrate <= 192)
                        {
                            chosen = &sub;
                            sid = s.service_id;
                            name = s.label;
                            key = std::to_string(s.service_id) + ":" + std::to_string(c.index) +
                                  ":" + std::to_string(sub.id) + ":" +
                                  std::to_string(sub.start_address) + ":" +
                                  std::to_string(sub.size_cu) + ":" +
                                  std::to_string(sub.protection_option) + ":" +
                                  std::to_string(sub.protection_level);
                            break;
                        }
            if (chosen)
                break;
        }
        if (!chosen)
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!routing.empty())
                receiver_generation = ++generation;
            selected.clear();
            latest_slide.reset();
            latest_label = DynamicLabel{};
            reset_transport();
            return;
        }
        if (key != routing)
        {
            if (!routing.empty())
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (receiver_generation != generation)
                    return;
                receiver_generation = ++generation;
            }
            reset_transport();
            routing = key;
        }
        if (!msc.configure(*chosen))
            return;
        {
            std::lock_guard<std::mutex> l(state_mutex);
            if (receiver_generation != generation)
                return;
            selected = name;
            selected_sid = sid;
        }
        for (unsigned cif = 0; cif < 4; ++cif)
        {
            auto word = msc.push(f.msc.data() + cif * 55296, 55296);
            if (word.empty())
                continue;
            auto rejected_before = superframes.rejected;
            auto units = superframes.push(std::move(word));
            rejected_superframes += superframes.rejected - rejected_before;
            for (auto &unit : units)
            {
                if (unit.damaged)
                {
                    ++damaged_units;
                    pad.reset();
                }
                else
                    pad.push(unit.bytes);
                std::lock_guard<std::mutex> l(audio_mutex);
                if (receiver_generation != generation)
                    continue;
                if (audio.size() == audio_capacity)
                {
                    audio.pop_front();
                    ++audio_drops;
                }
                audio.push_back({std::move(unit), receiver_generation});
                audio_peak = std::max(audio_peak, audio.size());
                audio_ready.notify_one();
            }
        }
    }
    void receive_loop()
    {
        std::uint64_t epoch = 0;
        try
        {
            for (;;)
            {
                Block b;
                {
                    std::unique_lock<std::mutex> l(input_mutex);
                    input_ready.wait(l, [&] { return stopping || eof || !input.empty(); });
                    if (stopping || (eof && input.empty()))
                        break;
                    b = std::move(input.front());
                    input.pop_front();
                    queued -= b.samples.size();
                }
                if (b.epoch != epoch)
                {
                    epoch = b.epoch;
                    demod.reset();
                    reset_transport();
                    synced = false;
                }
                demod.feed(b.samples.data(), b.samples.size(),
                           [this](dx::StreamFrame &&f)
                           {
                               if (!stopping)
                                   frame(std::move(f));
                           });
            }
        }
        catch (...)
        {
            fail();
        }
    }
    void audio_loop()
    {
        NeAACDecHandle decoder = nullptr;
        snd_pcm_t *pcm = nullptr;
        int header = -1;
        unsigned rate = 0, channels = 0;
        std::uint64_t epoch = ~std::uint64_t(0);
        std::size_t losses = 0;
        bool fade_in = true;
        std::int16_t last[2]{};
        try
        {
            for (;;)
            {
                Audio item;
                {
                    std::unique_lock<std::mutex> l(audio_mutex);
                    audio_ready.wait(l, [&] { return stopping || audio_eof || !audio.empty(); });
                    if (stopping || (audio_eof && audio.empty()))
                        break;
                    item = std::move(audio.front());
                    audio.pop_front();
                }
                if (item.generation != generation)
                    continue;
                if (epoch != item.generation || losses != audio_drops || header != item.unit.header)
                {
                    if (decoder)
                        NeAACDecClose(decoder);
                    decoder = nullptr;
                    if (pcm)
                    {
                        snd_pcm_drop(pcm);
                        snd_pcm_close(pcm);
                        pcm = nullptr;
                    }
                    header = item.unit.header;
                    epoch = item.generation;
                    losses = audio_drops;
                    fade_in = true;
                    last[0] = last[1] = 0;
                    decoder = NeAACDecOpen();
                    if (!decoder)
                        throw std::runtime_error("cannot open FAAD2");
                    auto config = NeAACDecGetCurrentConfiguration(decoder);
                    config->outputFormat = FAAD_FMT_16BIT;
                    if (!NeAACDecSetConfiguration(decoder, config))
                        throw std::runtime_error("cannot configure FAAD2");
                    auto asc = dab_plus_audio_specific_config(header);
                    unsigned long hz = 0;
                    unsigned char ch = 0;
                    if (static_cast<signed char>(
                            NeAACDecInit2(decoder, asc.data(), asc.size(), &hz, &ch)) < 0)
                    {
                        NeAACDecClose(decoder);
                        decoder = nullptr;
                        header = -1;
                        continue;
                    }
                    rate = hz;
                    channels = ch;
                }
                std::vector<std::int16_t> output;
                if (!item.unit.damaged && decoder)
                {
                    NeAACDecFrameInfo info{};
                    auto *data = static_cast<std::int16_t *>(NeAACDecDecode(
                        decoder, &info, item.unit.bytes.data(), item.unit.bytes.size()));
                    if (info.error)
                        ++aac_errors;
                    // FAAD2 primes its synthesis overlap on the first AU.
                    // No PCM without an error is not packet loss: resetting
                    // here would leave the codec permanently in that first AU.
                    if (!info.error && info.samples == 0)
                        continue;
                    if (!info.error && data && info.samples && info.channels >= 1 &&
                        info.channels <= 2 && info.samplerate >= 16000 && info.samplerate <= 48000)
                    {
                        if (rate != info.samplerate || channels != info.channels)
                        {
                            if (pcm)
                            {
                                snd_pcm_drop(pcm);
                                snd_pcm_close(pcm);
                                pcm = nullptr;
                            }
                            fade_in = true;
                        }
                        rate = info.samplerate;
                        channels = info.channels;
                        output.assign(data, data + info.samples);
                        ++decoded;
                    }
                }
                if (!rate || !channels || channels > 2)
                    continue;
                if (output.empty())
                {
                    ++concealed_frames;
                    // Own conservative concealment: a 5 ms ramp to silence,
                    // never label guessed audio as successfully decoded.
                    unsigned frames = (header & 32) ? 1920 : 960;
                    output.resize(frames * channels);
                    unsigned ramp = std::min(frames, rate / 200);
                    for (unsigned i = 0; i < ramp; ++i)
                        for (unsigned c = 0; c < channels; ++c)
                            output[i * channels + c] = last[c] * int(ramp - i) / int(ramp);
                    fade_in = true;
                    NeAACDecPostSeekReset(decoder, 0);
                }
                else if (fade_in)
                {
                    unsigned ramp = std::min<unsigned>(output.size() / channels, rate / 200);
                    for (unsigned i = 0; i < ramp; ++i)
                        for (unsigned c = 0; c < channels; ++c)
                            output[i * channels + c] =
                                output[i * channels + c] * int(i) / int(ramp);
                    fade_in = false;
                }
                for (unsigned c = 0; c < channels; ++c)
                    last[c] = output[output.size() - channels + c];
                if (!playback || muted || epoch != generation)
                {
                    if (pcm)
                    {
                        snd_pcm_drop(pcm);
                        snd_pcm_close(pcm);
                        pcm = nullptr;
                    }
                    fade_in = true;
                    continue;
                }
                if (!pcm)
                {
                    int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK,
                                           SND_PCM_NONBLOCK);
                    if (err >= 0)
                        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                                 SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1,
                                                 playback_latency_us);
                    if (err < 0)
                        throw std::runtime_error(std::string("ALSA: ") + snd_strerror(err));
                }
                std::size_t offset = 0;
                while (offset < output.size() && !stopping && epoch == generation && !muted)
                {
                    auto n = snd_pcm_writei(pcm, output.data() + offset,
                                            (output.size() - offset) / channels);
                    if (n == -EAGAIN)
                    {
                        snd_pcm_wait(pcm, 20);
                        continue;
                    }
                    if (n < 0)
                    {
                        if (n == -EPIPE)
                            ++underruns;
                        int err = snd_pcm_recover(pcm, n, 1);
                        if (err < 0)
                            throw std::runtime_error(std::string("ALSA: ") + snd_strerror(err));
                        continue;
                    }
                    if (n == 0)
                    {
                        snd_pcm_wait(pcm, 20);
                        continue;
                    }
                    offset += n * channels;
                }
            }
        }
        catch (...)
        {
            fail();
        }
        if (decoder)
            NeAACDecClose(decoder);
        if (pcm)
        {
            snd_pcm_drop(pcm);
            snd_pcm_close(pcm);
        }
    }
};
DabPlusRadioSession::DabPlusRadioSession(std::string s, std::string d, bool p)
    : impl_(std::make_unique<Impl>(std::move(s), std::move(d), p))
{
}
DabPlusRadioSession::~DabPlusRadioSession() { stop(); }
void DabPlusRadioSession::start(std::uint32_t frequency_khz)
{
    (void)frequency_khz;
    if (impl_->started.exchange(true))
        throw std::runtime_error("DAB radio already started");
    if (impl_->stopping)
        throw std::runtime_error("stopped DAB radio cannot restart");
    try
    {
        impl_->receiver = std::thread([this] { impl_->receive_loop(); });
        impl_->audio_player = std::thread([this] { impl_->audio_loop(); });
    }
    catch (...)
    {
        stop();
        throw;
    }
}
void DabPlusRadioSession::stop() noexcept
{
    if (!impl_)
        return;
    impl_->stopping = true;
    impl_->input_ready.notify_all();
    impl_->audio_ready.notify_all();
    if (impl_->receiver.joinable())
        impl_->receiver.join();
    if (impl_->audio_player.joinable())
        impl_->audio_player.join();
}
void DabPlusRadioSession::finish()
{
    {
        std::lock_guard<std::mutex> l(impl_->input_mutex);
        impl_->eof = true;
    }
    impl_->input_ready.notify_all();
    if (impl_->receiver.joinable())
        impl_->receiver.join();
    {
        std::lock_guard<std::mutex> lock(impl_->audio_mutex);
        impl_->audio_eof = true;
    }
    impl_->audio_ready.notify_all();
    if (impl_->audio_player.joinable())
        impl_->audio_player.join();
    check_error();
}
void DabPlusRadioSession::set_muted(bool v) { impl_->muted = v; }
void DabPlusRadioSession::select_station(const std::string &s)
{
    if (s.empty())
        throw std::runtime_error("empty DAB station selector");
    if (s.substr(0, 2) == "0x")
    {
        std::size_t n = 0;
        auto sid = std::stoul(s, &n, 16);
        if (n != s.size() || sid > 0xffffffffUL)
            throw std::runtime_error("invalid DAB service id");
    }
    {
        std::lock_guard<std::mutex> l(impl_->state_mutex);
        if (impl_->selector == s)
            return;
        if (!impl_->selected.empty() && matches(s, impl_->selected_sid, impl_->selected))
        {
            impl_->selector = s;
            return;
        }
        impl_->selector = s;
        ++impl_->generation;
        impl_->selected.clear();
        impl_->latest_slide.reset();
        impl_->latest_label = DynamicLabel{};
    }
    {
        std::lock_guard<std::mutex> l(impl_->audio_mutex);
        impl_->audio.clear();
    }
    impl_->audio_ready.notify_all();
}
void DabPlusRadioSession::push_samples(const std::complex<float> *iq, std::size_t count)
{
    if (!iq || !count || impl_->stopping)
        return;
    std::lock_guard<std::mutex> l(impl_->input_mutex);
    if (impl_->eof)
        return;
    while (count)
    {
        auto n = std::min(count, block_size);
        bool lost = false;
        while (!impl_->input.empty() && impl_->queued + n > input_capacity)
        {
            impl_->queued -= impl_->input.front().samples.size();
            impl_->input.pop_front();
            ++impl_->iq_drops;
            lost = true;
        }
        if (lost)
        {
            ++impl_->input_epoch;
            // Immediately invalidate audio preceding the gap, including any
            // frame currently being decoded on the receiver thread.
            {
                std::lock_guard<std::mutex> state_lock(impl_->state_mutex);
                ++impl_->generation;
                impl_->selected.clear();
                impl_->latest_slide.reset();
                impl_->latest_label = DynamicLabel{};
            }
            impl_->synced = false;
            for (auto &b : impl_->input)
                b.epoch = impl_->input_epoch;
        }
        impl_->input.push_back({{iq, iq + n}, impl_->input_epoch});
        impl_->queued += n;
        impl_->iq_peak = std::max(impl_->iq_peak, impl_->queued);
        iq += n;
        count -= n;
    }
    impl_->input_ready.notify_one();
}
void DabPlusRadioSession::check_error() const
{
    std::lock_guard<std::mutex> l(impl_->error_mutex);
    if (impl_->error)
        std::rethrow_exception(impl_->error);
}
std::vector<DabPlusService> DabPlusRadioSession::services() const
{
    std::lock_guard<std::mutex> l(impl_->state_mutex);
    return impl_->stations;
}
std::size_t DabPlusRadioSession::services_generation() const { return impl_->service_generation; }
bool DabPlusRadioSession::dab_signal_synced() const { return impl_->synced; }
bool DabPlusRadioSession::fic_synced() const { return impl_->synced; }
bool DabPlusRadioSession::service_list_received() const { return !services().empty(); }
bool DabPlusRadioSession::discovery_complete() const { return !services().empty(); }
bool DabPlusRadioSession::station_available() const
{
    std::lock_guard<std::mutex> l(impl_->state_mutex);
    for (auto &s : impl_->stations)
        if (matches(impl_->selector, s.service_id, s.label))
            return true;
    return false;
}
bool DabPlusRadioSession::station_selected() const { return !selected_station().empty(); }
std::string DabPlusRadioSession::selected_station() const
{
    std::lock_guard<std::mutex> l(impl_->state_mutex);
    return impl_->selected;
}
std::string DabPlusRadioSession::ensemble_label() const
{
    std::lock_guard<std::mutex> l(impl_->state_mutex);
    return impl_->ensemble;
}
std::size_t DabPlusRadioSession::dropped_iq_blocks() const { return impl_->iq_drops; }
std::size_t DabPlusRadioSession::dropped_audio_frames() const { return impl_->audio_drops; }
std::size_t DabPlusRadioSession::decoded_audio_frames() const { return impl_->decoded; }
std::size_t DabPlusRadioSession::decoded_slides() const { return impl_->slides; }
std::optional<MotSlide> DabPlusRadioSession::take_slide()
{
    std::lock_guard<std::mutex> l(impl_->state_mutex);
    auto v = std::move(impl_->latest_slide);
    impl_->latest_slide.reset();
    return v;
}
std::optional<DynamicLabel> DabPlusRadioSession::take_dynamic_label()
{
    std::lock_guard<std::mutex> l(impl_->state_mutex);
    auto v = std::move(impl_->latest_label);
    impl_->latest_label.reset();
    return v;
}
DabPlusRadioMetrics DabPlusRadioSession::metrics() const
{
    DabPlusRadioMetrics m;
    {
        std::lock_guard<std::mutex> l(impl_->input_mutex);
        m.iq_samples = impl_->queued;
        m.iq_peak_samples = impl_->iq_peak;
    }
    {
        std::lock_guard<std::mutex> l(impl_->audio_mutex);
        m.audio_frames = impl_->audio.size();
        m.audio_peak_frames = impl_->audio_peak;
    }
    m.iq_capacity_samples = input_capacity;
    m.audio_capacity_frames = audio_capacity;
    m.dropped_iq = impl_->iq_drops;
    m.dropped_audio = impl_->audio_drops;
    m.alsa_underruns = impl_->underruns;
    m.rejected_superframe_candidates = impl_->rejected_superframes;
    m.damaged_audio_units = impl_->damaged_units;
    m.aac_decode_errors = impl_->aac_errors;
    m.concealed_audio_frames = impl_->concealed_frames;
    return m;
}
} // namespace dab
