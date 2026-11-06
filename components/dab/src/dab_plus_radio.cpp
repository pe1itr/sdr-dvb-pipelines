#include "dab_plus_radio.hpp"

#include <dabsdr.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace dab {
namespace {

constexpr std::uint8_t dab_plus_ascty = 63;
constexpr std::size_t input_block_samples = 32768;
constexpr std::size_t maximum_input_samples = 4096000; // two seconds
constexpr std::size_t maximum_audio_frames = 64;

std::mutex active_mutex;
DabPlusRadioSession* active_session = nullptr;

void input_callback(float output[], std::uint16_t count)
{
    DabPlusRadioSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(active_mutex);
        session = active_session;
    }
    if (session) session->supply_input(output, count);
    else std::fill(output, output + 2 * count, 0.0f);
}

void dummy_input_callback(float output[], std::uint16_t count)
{
    input_callback(output, count);
}

void notification_callback(dabsdrNotificationCBData_t* value, void* context)
{
    if (context) static_cast<DabPlusRadioSession*>(context)
        ->handle_notification(value);
}

void audio_callback(dabsdrAudioCBData_t* value, void* context)
{
    if (context) static_cast<DabPlusRadioSession*>(context)->handle_audio(value);
}

void dynamic_label_callback(dabsdrDynamicLabelCBData_t* value, void* context)
{
    if (context) static_cast<DabPlusRadioSession*>(context)
        ->handle_dynamic_label(value);
}

void data_group_callback(dabsdrDataGroupCBData_t* value, void* context)
{
    if (context) static_cast<DabPlusRadioSession*>(context)
        ->handle_data_group(value);
}

std::string label_text(const dabsdrLabel_t& label)
{
    std::string value(label.str,
        strnlen(label.str, DAB_LABEL_MAX_LENGTH));
    while (!value.empty() && value.back() == ' ') value.pop_back();
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [](unsigned char ch) { return !std::isspace(ch); }));
    return value;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return char(std::tolower(ch)); });
    return value;
}

struct StationSelector {
    bool by_id = false;
    std::uint32_t id = 0;
    std::string label;
};

StationSelector parse_selector(const std::string& text)
{
    StationSelector result;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(text.substr(2), &consumed, 16);
        if (consumed != text.size() - 2 || parsed > 0xffffffffUL)
            throw std::runtime_error("invalid --station SID: " + text);
        result.by_id = true;
        result.id = std::uint32_t(parsed);
        return result;
    }
    if (text.empty()) throw std::runtime_error("--station cannot be empty");
    result.label = lower(text);
    return result;
}

} // namespace

std::vector<std::uint8_t> dab_plus_audio_specific_config(
    std::uint8_t audio_header_raw)
{
    dabsdrAudioFrameHeader_t header{};
    header.raw = audio_header_raw;
    const std::uint8_t core_rate_index = header.bits.sbr_flag
        ? (header.bits.dac_rate ? 6 : 8)
        : (header.bits.dac_rate ? 3 : 5);
    const std::uint8_t channels = header.bits.aac_channel_mode + 1;
    const std::uint8_t extension_rate_index =
        std::uint8_t(5 - 2 * header.bits.dac_rate);

    // ETSI DAB+ uses AAC-LC with a 960-sample transform. SBR and PS are
    // signalled through the backwards-compatible MPEG-4 sync extensions.
    std::vector<std::uint8_t> asc;
    asc.push_back(std::uint8_t((2U << 3) | (core_rate_index >> 1)));
    asc.push_back(std::uint8_t(((core_rate_index & 1U) << 7) |
                              (channels << 3) | 0x04U));
    if (header.bits.sbr_flag) {
        asc.push_back(0x56);
        asc.push_back(0xE5);
        asc.push_back(std::uint8_t(0x80U | (extension_rate_index << 3)));
        if (header.bits.ps_flag) {
            asc.back() |= 0x05U;
            asc.push_back(0x48);
            asc.push_back(0x80);
        }
    }
    return asc;
}

struct DabPlusRadioSession::Impl {
    struct InputBlock {
        std::vector<std::complex<float>> samples;
        std::size_t position = 0;
    };
    struct EncodedAudio {
        std::uint8_t header = 0;
        bool conceal = false;
        std::vector<std::uint8_t> data;
    };
    struct ServiceRecord {
        std::string label;
        std::vector<DabPlusService> dab_plus_components;
    };

    explicit Impl(std::string selector_text, std::string device,
                  bool playback)
        : selector(parse_selector(selector_text)),
          selector_text(std::move(selector_text)),
          alsa_device(std::move(device)), playback_enabled(playback)
    {}

    StationSelector selector;
    std::string selector_text;
    std::string alsa_device;
    bool playback_enabled = true;
    std::atomic<bool> muted{false};
    dabsdrHandle_t handle = nullptr;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};
    std::atomic<int> sync_level{DABSDR_SYNC_LEVEL_NO_SYNC};
    std::atomic<bool> selected{false};
    std::atomic<std::size_t> service_generation{0};
    std::atomic<std::size_t> dropped_iq{0};
    std::atomic<std::size_t> dropped_audio{0};
    std::atomic<std::size_t> alsa_underruns{0};
    std::atomic<std::size_t> decoded_audio{0};
    std::atomic<std::size_t> decoded_slides{0};
    std::atomic<std::size_t> audio_reset_generation{0};

    mutable std::mutex input_mutex;
    std::condition_variable input_ready;
    std::deque<InputBlock> input_queue;
    std::size_t queued_input_samples = 0;
    std::size_t peak_input_samples = 0;

    mutable std::mutex state_mutex;
    std::condition_variable state_changed;
    std::map<std::uint32_t, ServiceRecord> service_records;
    std::set<std::uint32_t> pending_components;
    std::set<std::uint32_t> requested_components;
    bool service_list_received = false;
    bool selection_requested = false;
    std::optional<std::uint8_t> pending_xpad_start;
    std::optional<std::uint8_t> active_xpad_app;
    std::string selected_label;
    std::string ensemble_label;
    std::uint32_t selected_sid = 0;
    std::uint8_t selected_component = 0;

    std::mutex audio_mutex;
    std::condition_variable audio_ready;
    std::deque<EncodedAudio> audio_queue;
    std::size_t peak_audio_frames = 0;

    std::mutex slide_mutex;
    MotSlideshowDecoder slideshow_decoder;
    std::optional<MotSlide> latest_slide;

    std::mutex dynamic_label_mutex;
    DynamicLabelDecoder dynamic_label_decoder;
    std::optional<DynamicLabel> latest_dynamic_label;

    mutable std::mutex error_mutex;
    std::exception_ptr error;
    std::thread control_thread;
    std::thread audio_thread;

    void record_error(std::exception_ptr value)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) error = value;
    }

    bool matches(std::uint32_t sid, const std::string& label) const
    {
        return selector.by_id ? sid == selector.id
                              : lower(label) == selector.label;
    }

    void control_loop()
    {
        try {
            auto next_service_request = std::chrono::steady_clock::now();
            auto next_user_app_request = std::chrono::steady_clock::now();
            while (!stopping.load()) {
                const auto now = std::chrono::steady_clock::now();
                if (sync_level.load() == DABSDR_SYNC_LEVEL_FIC &&
                    now >= next_service_request) {
                    dabsdrRequest_GetEnsemble(handle);
                    dabsdrRequest_GetServiceList(handle);
                    next_service_request = now + std::chrono::seconds(1);
                }

                std::vector<std::uint32_t> request_components;
                std::optional<DabPlusService> request_selection;
                std::optional<std::pair<std::uint32_t, std::uint8_t>>
                    request_user_apps;
                std::optional<std::uint8_t> start_xpad;
                {
                    std::unique_lock<std::mutex> lock(state_mutex);
                    if (service_list_received) {
                        for (const auto& [sid, record] : service_records) {
                            (void)record;
                            if (!requested_components.insert(sid).second)
                                continue;
                            pending_components.insert(sid);
                            request_components.push_back(sid);
                        }
                    }
                    if (!selection_requested) {
                        for (const auto& [sid, record] : service_records) {
                            if (!matches(sid, record.label)) continue;
                            if (!record.dab_plus_components.empty()) {
                                request_selection = *std::max_element(
                                    record.dab_plus_components.begin(),
                                    record.dab_plus_components.end(),
                                    [](const auto& lhs, const auto& rhs) {
                                        return lhs.primary < rhs.primary;
                                    });
                                selection_requested = true;
                            }
                            break;
                        }
                    }
                    if (selected.load() && !active_xpad_app &&
                        now >= next_user_app_request) {
                        request_user_apps =
                            std::make_pair(selected_sid, selected_component);
                        next_user_app_request =
                            now + std::chrono::seconds(1);
                    }
                    if (pending_xpad_start) {
                        start_xpad = pending_xpad_start;
                        pending_xpad_start.reset();
                    }
                    state_changed.wait_for(lock, std::chrono::milliseconds(50));
                }
                for (const auto sid : request_components)
                    dabsdrRequest_GetServiceComponents(handle, sid);
                if (request_selection)
                    dabsdrRequest_ServiceSelection(
                        handle, request_selection->service_id,
                        request_selection->component_id,
                        DABSDR_ID_AUDIO_PRIMARY);
                if (request_user_apps)
                    dabsdrRequest_GetUserAppList(
                        handle, request_user_apps->first,
                        request_user_apps->second);
                if (start_xpad)
                    dabsdrRequest_XPadAppStart(
                        handle, *start_xpad, 1,
                        DABSDR_ID_AUDIO_PRIMARY);
            }
        } catch (...) {
            record_error(std::current_exception());
        }
    }

    void audio_loop()
    {
        HANDLE_AACDECODER decoder = nullptr;
        snd_pcm_t* pcm = nullptr;
        std::uint8_t configured_header = 0xff;
        std::size_t reset_generation = audio_reset_generation.load();
        int pcm_rate = 0;
        int pcm_channels = 0;
        std::vector<INT_PCM> pcm_buffer(8192);
        try {
            while (!stopping.load()) {
                EncodedAudio frame;
                {
                    std::unique_lock<std::mutex> lock(audio_mutex);
                    audio_ready.wait(lock, [&] {
                        return stopping.load() || !audio_queue.empty();
                    });
                    if (stopping.load() && audio_queue.empty()) break;
                    frame = std::move(audio_queue.front());
                    audio_queue.pop_front();
                }

                const auto wanted_reset = audio_reset_generation.load();
                if (wanted_reset != reset_generation) {
                    if (decoder) {
                        aacDecoder_Close(decoder);
                        decoder = nullptr;
                    }
                    configured_header = 0xff;
                    reset_generation = wanted_reset;
                }

                std::uint8_t format_header = frame.header;
                dabsdrAudioFrameHeader_t parsed{};
                parsed.raw = format_header;
                if (frame.conceal && configured_header != 0xff)
                    format_header = configured_header;
                if (!decoder || format_header != configured_header) {
                    if (decoder) aacDecoder_Close(decoder);
                    decoder = aacDecoder_Open(TT_MP4_RAW, 1);
                    if (!decoder)
                        throw std::runtime_error("cannot open FDK-AAC decoder");
                    parsed.raw = format_header;
                    const int wanted_channels =
                        parsed.bits.aac_channel_mode || parsed.bits.ps_flag ? 2 : 1;
                    if (aacDecoder_SetParam(decoder, AAC_PCM_MIN_OUTPUT_CHANNELS,
                                            wanted_channels) != AAC_DEC_OK ||
                        aacDecoder_SetParam(decoder, AAC_PCM_MAX_OUTPUT_CHANNELS,
                                            wanted_channels) != AAC_DEC_OK)
                        throw std::runtime_error(
                            "cannot configure FDK-AAC output channels");
                    auto asc = dab_plus_audio_specific_config(format_header);
                    UCHAR* config[] = {asc.data()};
                    const UINT lengths[] = {UINT(asc.size())};
                    if (aacDecoder_ConfigRaw(decoder, config, lengths) != AAC_DEC_OK)
                        throw std::runtime_error(
                            "cannot configure FDK-AAC DAB+ stream");
                    configured_header = format_header;
                }

                if (!frame.data.empty()) {
                    UCHAR* data[] = {frame.data.data()};
                    const UINT lengths[] = {UINT(frame.data.size())};
                    UINT valid = lengths[0];
                    const auto fill = aacDecoder_Fill(
                        decoder, data, lengths, &valid);
                    if (fill != AAC_DEC_OK || valid != 0)
                        throw std::runtime_error("FDK-AAC rejected a DAB+ access unit");
                }
                const auto result = aacDecoder_DecodeFrame(
                    decoder, pcm_buffer.data(), INT(pcm_buffer.size()),
                    frame.conceal ? AACDEC_CONCEAL : 0);
                if (!IS_OUTPUT_VALID(result)) continue;
                const auto* stream = aacDecoder_GetStreamInfo(decoder);
                if (!stream || stream->sampleRate <= 0 ||
                    stream->numChannels <= 0 || stream->frameSize <= 0)
                    throw std::runtime_error("FDK-AAC returned invalid PCM format");
                ++decoded_audio;
                if (!playback_enabled || muted.load()) continue;

                if (!pcm || pcm_rate != stream->sampleRate ||
                    pcm_channels != stream->numChannels) {
                    if (pcm) {
                        snd_pcm_drop(pcm);
                        snd_pcm_close(pcm);
                        pcm = nullptr;
                    }
                    int result_pcm = snd_pcm_open(
                        &pcm, alsa_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
                    if (result_pcm >= 0)
                        result_pcm = snd_pcm_set_params(
                            pcm, SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            unsigned(stream->numChannels),
                            unsigned(stream->sampleRate), 1, 100000);
                    if (result_pcm < 0)
                        throw std::runtime_error(
                            std::string("cannot open ALSA device '") +
                            alsa_device + "': " + snd_strerror(result_pcm));
                    pcm_rate = stream->sampleRate;
                    pcm_channels = stream->numChannels;
                }
                snd_pcm_sframes_t remaining = stream->frameSize;
                const INT_PCM* output = pcm_buffer.data();
                while (remaining > 0 && !stopping.load()) {
                    auto written = snd_pcm_writei(pcm, output, remaining);
                    if (written == -EPIPE) ++alsa_underruns;
                    if (written < 0) written = snd_pcm_recover(pcm, int(written), 1);
                    if (written < 0)
                        throw std::runtime_error(
                            std::string("ALSA playback failed: ") +
                            snd_strerror(int(written)));
                    output += written * pcm_channels;
                    remaining -= written;
                }
            }
        } catch (...) {
            record_error(std::current_exception());
        }
        if (pcm) {
            snd_pcm_drop(pcm);
            snd_pcm_close(pcm);
        }
        if (decoder) aacDecoder_Close(decoder);
    }
};

DabPlusRadioSession::DabPlusRadioSession(
    std::string station_selector, std::string alsa_device,
    bool playback_enabled)
    : impl_(std::make_unique<Impl>(
          std::move(station_selector), std::move(alsa_device),
          playback_enabled))
{}

DabPlusRadioSession::~DabPlusRadioSession()
{
    stop();
}

void DabPlusRadioSession::start(std::uint32_t frequency_khz)
{
    if (impl_->started.exchange(true))
        throw std::runtime_error("DAB+ radio session already started");
    {
        std::lock_guard<std::mutex> lock(active_mutex);
        if (active_session)
            throw std::runtime_error("only one libdabsdr radio session is supported");
        active_session = this;
    }
    try {
        if (dabsdrInit(&impl_->handle) != EXIT_SUCCESS || !impl_->handle)
            throw std::runtime_error("libdabsdr radio initialization failed");
        dabsdrRegisterInputFcn(impl_->handle, input_callback);
        dabsdrRegisterDummyInputFcn(impl_->handle, dummy_input_callback);
        dabsdrRegisterNotificationCb(
            impl_->handle, notification_callback, this);
        dabsdrRegisterAudioCb(impl_->handle, audio_callback, this);
        dabsdrRegisterDynamicLabelCb(
            impl_->handle, dynamic_label_callback, this);
        dabsdrRegisterDataGroupCb(
            impl_->handle, data_group_callback, this);
        dabsdr(impl_->handle);
        impl_->control_thread = std::thread([this] { impl_->control_loop(); });
        impl_->audio_thread = std::thread([this] { impl_->audio_loop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        dabsdrRequest_Tune(impl_->handle, frequency_khz);
    } catch (...) {
        stop();
        throw;
    }
}

void DabPlusRadioSession::stop() noexcept
{
    if (!impl_ || !impl_->started.exchange(false)) return;
    impl_->stopping = true;
    impl_->input_ready.notify_all();
    impl_->audio_ready.notify_all();
    impl_->state_changed.notify_all();
    if (impl_->handle) dabsdrRequest_Exit(impl_->handle);
    if (impl_->control_thread.joinable()) impl_->control_thread.join();
    if (impl_->audio_thread.joinable()) impl_->audio_thread.join();
    if (impl_->handle) dabsdrDeinit(&impl_->handle);
    {
        std::lock_guard<std::mutex> lock(active_mutex);
        if (active_session == this) active_session = nullptr;
    }
}

void DabPlusRadioSession::set_muted(bool muted) { impl_->muted.store(muted); }

void DabPlusRadioSession::select_station(
    const std::string& station_selector)
{
    const auto parsed = parse_selector(station_selector);
    std::uint32_t old_sid = 0;
    std::uint8_t old_component = 0;
    std::optional<std::uint8_t> old_xpad;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        old_sid = impl_->selected_sid;
        old_component = impl_->selected_component;
        old_xpad = impl_->active_xpad_app;
        impl_->selector = parsed;
        impl_->selector_text = station_selector;
        impl_->selection_requested = false;
        impl_->selected = false;
        impl_->selected_label.clear();
        impl_->selected_sid = 0;
        impl_->selected_component = 0;
        impl_->pending_xpad_start.reset();
        impl_->active_xpad_app.reset();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->audio_mutex);
        impl_->audio_queue.clear();
    }
    ++impl_->audio_reset_generation;
    {
        std::lock_guard<std::mutex> lock(impl_->slide_mutex);
        impl_->slideshow_decoder.reset();
        impl_->latest_slide.reset();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->dynamic_label_mutex);
        impl_->dynamic_label_decoder.reset();
        // Publish a clear immediately so the old station's text cannot remain
        // visible while the newly selected service is acquiring.
        impl_->latest_dynamic_label = DynamicLabel{};
    }
    if (impl_->handle && old_xpad)
        dabsdrRequest_XPadAppStart(
            impl_->handle, *old_xpad, 0,
            DABSDR_ID_AUDIO_PRIMARY);
    if (impl_->handle && old_sid != 0)
        dabsdrRequest_ServiceStop(
            impl_->handle, old_sid, old_component,
            DABSDR_ID_AUDIO_PRIMARY);
    impl_->state_changed.notify_all();
    impl_->audio_ready.notify_all();
}

void DabPlusRadioSession::push_samples(
    const std::complex<float>* samples, std::size_t count)
{
    if (!samples || count == 0 || impl_->stopping.load()) return;
    std::lock_guard<std::mutex> lock(impl_->input_mutex);
    while (count != 0) {
        const auto added = std::min(count, input_block_samples);
        while (!impl_->input_queue.empty() &&
               impl_->queued_input_samples + added > maximum_input_samples) {
            const auto& dropped = impl_->input_queue.front();
            impl_->queued_input_samples -=
                dropped.samples.size() - dropped.position;
            impl_->input_queue.pop_front();
            ++impl_->dropped_iq;
        }
        Impl::InputBlock block;
        block.samples.assign(samples, samples + added);
        impl_->input_queue.push_back(std::move(block));
        impl_->queued_input_samples += added;
        impl_->peak_input_samples = std::max(impl_->peak_input_samples, impl_->queued_input_samples);
        samples += added;
        count -= added;
    }
    impl_->input_ready.notify_one();
}

void DabPlusRadioSession::supply_input(float output[], std::uint16_t count)
{
    std::size_t produced = 0;
    std::unique_lock<std::mutex> lock(impl_->input_mutex);
    while (produced < count && !impl_->stopping.load()) {
        impl_->input_ready.wait(lock, [&] {
            return impl_->stopping.load() || !impl_->input_queue.empty();
        });
        if (impl_->stopping.load()) break;
        auto& block = impl_->input_queue.front();
        const auto available = block.samples.size() - block.position;
        const auto copied = std::min<std::size_t>(count - produced, available);
        for (std::size_t i = 0; i < copied; ++i) {
            const auto sample = block.samples[block.position + i];
            output[2 * (produced + i)] = 32768.0f * sample.real();
            output[2 * (produced + i) + 1] = 32768.0f * sample.imag();
        }
        produced += copied;
        block.position += copied;
        impl_->queued_input_samples -= copied;
        if (block.position == block.samples.size()) impl_->input_queue.pop_front();
    }
    std::fill(output + 2 * produced, output + 2 * count, 0.0f);
}

void DabPlusRadioSession::handle_notification(void* opaque)
{
    auto* notification = static_cast<dabsdrNotificationCBData_t*>(opaque);
    if (!notification) return;
    if (notification->status != DABSDR_NSTAT_SUCCESS) {
        if (notification->nid == DABSDR_NID_SERVICE_COMPONENT_LIST) {
            const auto* list =
                static_cast<const dabsdrNtfServiceComponentList_t*>(
                    notification->pData);
            if (list) {
                std::lock_guard<std::mutex> lock(impl_->state_mutex);
                impl_->pending_components.erase(list->SId);
                impl_->state_changed.notify_all();
            }
        } else if (notification->nid == DABSDR_NID_SERVICE_SELECTION) {
            impl_->record_error(std::make_exception_ptr(std::runtime_error(
                "libdabsdr could not select the requested DAB+ service")));
        } else if (notification->nid == DABSDR_NID_XPAD_APP_START_STOP) {
            const auto* xpad = static_cast<const dabsdrNtfXpadAppStartStop_t*>(
                notification->pData);
            if (xpad && xpad->start) {
                std::lock_guard<std::mutex> lock(impl_->state_mutex);
                if (impl_->active_xpad_app == xpad->appType)
                    impl_->active_xpad_app.reset();
                impl_->state_changed.notify_all();
            }
        }
        return;
    }
    if (notification->nid == DABSDR_NID_SYNC_STATUS) {
        const auto* sync = static_cast<const dabsdrNtfSyncStatus_t*>(
            notification->pData);
        if (sync) {
            impl_->sync_level = sync->syncLevel;
            impl_->state_changed.notify_all();
        }
    } else if (notification->nid == DABSDR_NID_ENSEMBLE_INFO) {
        const auto* ensemble = static_cast<const dabsdrNtfEnsemble_t*>(
            notification->pData);
        if (!ensemble) return;
        const auto incoming_label = label_text(ensemble->label);
        if (!incoming_label.empty()) {
            std::lock_guard<std::mutex> lock(impl_->state_mutex);
            impl_->ensemble_label = incoming_label;
        }
    } else if (notification->nid == DABSDR_NID_SERVICE_LIST) {
        const auto* list = static_cast<const dabsdrNtfServiceList_t*>(
            notification->pData);
        if (!list || !list->getServiceListItem) return;
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        for (std::size_t index = 0; index < list->numServices; ++index) {
            dabsdrServiceListItem_t item{};
            if (list->getServiceListItem(
                    impl_->handle, std::uint8_t(index), &item) != EXIT_SUCCESS)
                continue;
            auto& record = impl_->service_records[item.sid];
            const auto incoming_label = label_text(item.label);
            // FIC labels are repeated cyclically and can temporarily be absent
            // in a weak or short decode window. Never replace a known label
            // with an empty transient value within the same tuned ensemble.
            if (!incoming_label.empty()) {
                record.label = incoming_label;
                for (auto& component : record.dab_plus_components)
                    component.label = incoming_label;
            }
        }
        // An empty early response is not a completed discovery cycle. More
        // services can appear as additional FIC groups are accumulated.
        if (list->numServices != 0) impl_->service_list_received = true;
        ++impl_->service_generation;
        impl_->state_changed.notify_all();
    } else if (notification->nid == DABSDR_NID_SERVICE_COMPONENT_LIST) {
        const auto* list = static_cast<const dabsdrNtfServiceComponentList_t*>(
            notification->pData);
        if (!list || !list->getServiceComponentListItem) return;
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        auto& record = impl_->service_records[list->SId];
        std::vector<DabPlusService> incoming;
        for (std::size_t index = 0; index < list->numServiceComponents; ++index) {
            dabsdrServiceCompListItem_t item{};
            if (list->getServiceComponentListItem(
                    impl_->handle, std::uint8_t(index), &item) != EXIT_SUCCESS)
                continue;
            if (item.TMId != 0 || item.streamAudio.ASCTy != dab_plus_ascty ||
                item.CAflag)
                continue;
            incoming.push_back({
                list->SId, item.SCIdS, record.label,
                item.streamAudio.bitRate, item.ps != 0});
        }
        // An incomplete FIC cycle may yield an empty component callback. A
        // station already proven to be DAB+ remains valid until retune or an
        // explicit non-empty reconfiguration replaces its component set.
        if (!incoming.empty())
            record.dab_plus_components = std::move(incoming);
        impl_->pending_components.erase(list->SId);
        ++impl_->service_generation;
        impl_->state_changed.notify_all();
    } else if (notification->nid == DABSDR_NID_SERVICE_SELECTION) {
        const auto* selection = static_cast<const dabsdrNtfServiceSelection_t*>(
            notification->pData);
        if (!selection) return;
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        const auto found = impl_->service_records.find(selection->SId);
        impl_->selected_label = found == impl_->service_records.end()
            ? impl_->selector_text : found->second.label;
        impl_->selected_sid = selection->SId;
        impl_->selected_component = selection->SCIdS;
        impl_->selected = true;
        impl_->state_changed.notify_all();
    } else if (notification->nid == DABSDR_NID_USER_APP_UPDATE) {
        const auto* update = static_cast<const dabsdrNtfUserAppUpdate_t*>(
            notification->pData);
        if (!update) return;
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (update->SId == impl_->selected_sid &&
            update->SCIdS == impl_->selected_component) {
            impl_->state_changed.notify_all();
        }
    } else if (notification->nid == DABSDR_NID_USER_APP_LIST) {
        const auto* list = static_cast<const dabsdrNtfUserAppList_t*>(
            notification->pData);
        if (!list || !list->getUserAppListItem) return;
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (list->SId != impl_->selected_sid ||
            list->SCIdS != impl_->selected_component)
            return;
        for (std::size_t index = 0; index < list->numUserApps; ++index) {
            dabsdrUserAppListItem_t item{};
            if (list->getUserAppListItem(
                    impl_->handle, std::uint8_t(index), &item) != EXIT_SUCCESS)
                continue;
            constexpr std::uint16_t slideshow_user_app = 0x0002;
            if (item.type == slideshow_user_app && item.dataLen >= 2 &&
                (item.data[0] & 0x80) == 0) {
                const std::uint8_t app_type = item.data[0] & 0x1f;
                impl_->active_xpad_app = app_type;
                impl_->pending_xpad_start = app_type;
                impl_->state_changed.notify_all();
                break;
            }
        }
    }
}

void DabPlusRadioSession::handle_audio(void* opaque)
{
    const auto* audio = static_cast<dabsdrAudioCBData_t*>(opaque);
    if (!audio || audio->id != DABSDR_ID_AUDIO_PRIMARY ||
        audio->ASCTy != dab_plus_ascty)
        return;
    Impl::EncodedAudio frame;
    frame.header = audio->header.raw;
    frame.conceal = audio->header.bits.conceal != 0;
    if (audio->pAuData && audio->auLen)
        frame.data.assign(audio->pAuData, audio->pAuData + audio->auLen);
    std::lock_guard<std::mutex> lock(impl_->audio_mutex);
    if (impl_->audio_queue.size() == maximum_audio_frames) {
        impl_->audio_queue.pop_front();
        ++impl_->dropped_audio;
    }
    impl_->audio_queue.push_back(std::move(frame));
    impl_->peak_audio_frames = std::max(impl_->peak_audio_frames, impl_->audio_queue.size());
    impl_->audio_ready.notify_one();
}

void DabPlusRadioSession::handle_dynamic_label(void* opaque)
{
    const auto* label = static_cast<dabsdrDynamicLabelCBData_t*>(opaque);
    if (!label || label->id != DABSDR_ID_AUDIO_PRIMARY ||
        !label->pData || label->len == 0)
        return;
    std::lock_guard<std::mutex> lock(impl_->dynamic_label_mutex);
    if (auto decoded = impl_->dynamic_label_decoder.push_data_group(
            label->pData, label->len))
        impl_->latest_dynamic_label = std::move(decoded);
}

void DabPlusRadioSession::handle_data_group(void* opaque)
{
    const auto* group = static_cast<dabsdrDataGroupCBData_t*>(opaque);
    constexpr std::uint16_t slideshow_user_app = 0x0002;
    if (!group || group->id != DABSDR_ID_AUDIO_PRIMARY ||
        group->userAppType != slideshow_user_app || !group->pDgData ||
        group->dgLen == 0)
        return;
    std::lock_guard<std::mutex> lock(impl_->slide_mutex);
    if (auto slide = impl_->slideshow_decoder.push_data_group(
            group->pDgData, group->dgLen)) {
        impl_->latest_slide = std::move(slide);
        ++impl_->decoded_slides;
    }
}

void DabPlusRadioSession::check_error() const
{
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    if (impl_->error) std::rethrow_exception(impl_->error);
}

std::vector<DabPlusService> DabPlusRadioSession::services() const
{
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    std::vector<DabPlusService> result;
    for (const auto& [sid, record] : impl_->service_records) {
        (void)sid;
        result.insert(result.end(), record.dab_plus_components.begin(),
                      record.dab_plus_components.end());
    }
    return result;
}

std::size_t DabPlusRadioSession::services_generation() const
{
    return impl_->service_generation.load();
}

bool DabPlusRadioSession::dab_signal_synced() const
{
    return impl_->sync_level.load() != DABSDR_SYNC_LEVEL_NO_SYNC;
}

bool DabPlusRadioSession::fic_synced() const
{
    return impl_->sync_level.load() == DABSDR_SYNC_LEVEL_FIC;
}

bool DabPlusRadioSession::service_list_received() const
{
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->service_list_received;
}

std::string DabPlusRadioSession::ensemble_label() const
{
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->ensemble_label;
}

bool DabPlusRadioSession::discovery_complete() const
{
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->service_list_received &&
           impl_->requested_components.size() == impl_->service_records.size() &&
           impl_->pending_components.empty();
}

bool DabPlusRadioSession::station_selected() const
{
    return impl_->selected.load();
}

bool DabPlusRadioSession::station_available() const
{
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& [sid, record] : impl_->service_records)
        if (impl_->matches(sid, record.label) &&
            !record.dab_plus_components.empty())
            return true;
    return false;
}

std::string DabPlusRadioSession::selected_station() const
{
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->selected_label;
}

DabPlusRadioMetrics DabPlusRadioSession::metrics() const
{
    DabPlusRadioMetrics result;
    {
        std::lock_guard<std::mutex> lock(impl_->input_mutex);
        result.iq_samples = impl_->queued_input_samples;
        result.iq_peak_samples = impl_->peak_input_samples;
        result.iq_capacity_samples = maximum_input_samples;
        result.dropped_iq = impl_->dropped_iq.load();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->audio_mutex);
        result.audio_frames = impl_->audio_queue.size();
        result.audio_peak_frames = impl_->peak_audio_frames;
        result.audio_capacity_frames = maximum_audio_frames;
        result.dropped_audio = impl_->dropped_audio.load();
    }
    result.alsa_underruns = impl_->alsa_underruns.load();
    return result;
}

std::size_t DabPlusRadioSession::dropped_iq_blocks() const
{
    return impl_->dropped_iq.load();
}

std::size_t DabPlusRadioSession::dropped_audio_frames() const
{
    return impl_->dropped_audio.load();
}

std::size_t DabPlusRadioSession::decoded_audio_frames() const
{
    return impl_->decoded_audio.load();
}

std::size_t DabPlusRadioSession::decoded_slides() const
{
    return impl_->decoded_slides.load();
}

std::optional<MotSlide> DabPlusRadioSession::take_slide()
{
    std::lock_guard<std::mutex> lock(impl_->slide_mutex);
    auto result = std::move(impl_->latest_slide);
    impl_->latest_slide.reset();
    return result;
}

std::optional<DynamicLabel> DabPlusRadioSession::take_dynamic_label()
{
    std::lock_guard<std::mutex> lock(impl_->dynamic_label_mutex);
    auto result = std::move(impl_->latest_dynamic_label);
    impl_->latest_dynamic_label.reset();
    return result;
}

} // namespace dab
