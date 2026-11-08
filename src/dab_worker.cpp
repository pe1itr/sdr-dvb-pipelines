// DAB process adapter. DSP belongs to components/dab; no GTK or SDR ownership here.
#include "../components/dab/channels.h"
#include "candidate_fic.hpp"
#include "channel_filter.hpp"
#include "libdxdabsdr.hpp"
#include "dab_resampler.hpp"
#include "fic_identity_accumulator.hpp"
#ifdef HAVE_DAB_RADIO
#include "dab_plus_radio.hpp"
#endif
#include <json-c/json.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
volatile sig_atomic_t interrupted=0;
void stop_signal(int) { interrupted=1; }
using IQ=std::vector<std::complex<float>>;
void str(json_object *o,const char *key,const std::string& value) {
    json_object_object_add(o,key,json_object_new_string(value.c_str()));
}
void num(json_object *o,const char *key,double value) {
    json_object_object_add(o,key,std::isfinite(value) ? json_object_new_double(value) : nullptr);
}
void atomic_file(const std::string& path,const void *data,size_t count) {
    std::string tmp=path+".tmp";
    std::ofstream file(tmp,std::ios::binary|std::ios::trunc);
    file.write(static_cast<const char*>(data),count); file.close();
    if (!file || std::rename(tmp.c_str(),path.c_str())) throw std::runtime_error("Cannot write "+path);
}
struct Diagnostics {
    std::mutex mutex;
    std::condition_variable ready;
    IQ pending, scope_pending;
    std::atomic<uint64_t> generation{0};
    uint64_t pending_generation=0, scope_generation=0, scope_sequence=0;
    uint64_t pending_hz=0;
    dab::dx::ConstellationSnapshot scope;
    dab::Detection detection;
    std::chrono::steady_clock::time_point scope_time{};
    bool eof=false;
    std::atomic<bool> cancel{false};
    dab::EnsembleInfo ensemble;
    dab::TiiInfo tii;
    dab::FicIdentityEvidence fic;
    unsigned dropped=0, captures=0, mux_count=0;
    double decode_ms=NAN;
    time_t observed=0, pending_observed=0;
    std::string error, route, ensembles;
};
void decode(Diagnostics& d,bool weak,bool aircraft) {
    try {
        auto backend=dab::make_decoder_backend("libdxdabsdr");
        uint64_t generation=0, previous=0;
        auto cancelled=[&] {return interrupted || d.cancel.load() || generation!=d.generation.load();};
        backend->set_cancel_callback(cancelled);
        auto accumulator=std::make_unique<dab::FicIdentityAccumulator>();
        while (!interrupted && !d.cancel) {
            IQ samples;time_t observed=0;uint64_t hz=0;
            {
                std::unique_lock<std::mutex> lock(d.mutex);
                d.ready.wait_for(lock,std::chrono::milliseconds(100),[&] {return d.eof || !d.pending.empty() || d.cancel;});
                if (d.cancel || (d.pending.empty() && d.eof)) break;
                if (d.pending.empty()) continue;
                samples.swap(d.pending);observed=d.pending_observed;hz=d.pending_hz;generation=d.pending_generation;
            }
            if (generation!=previous) {accumulator=std::make_unique<dab::FicIdentityAccumulator>();previous=generation;}
            auto decode_start=std::chrono::steady_clock::now();
            auto detection=dab::detect_mode_i(samples,{});
            dab::CaptureDecodeResult result;
            if (weak || aircraft) {
                dab::TimingSearchConfig timing;
                timing.cancel_requested=cancelled;
                timing.prs_temporal_rank=aircraft;
                result=dab::decode_capture_with_candidate_fallback(samples,hz/1000,detection,*backend,timing,10,
                    aircraft ? 2048000*5/2 : 0,aircraft ? 6 : 0);
            } else {
                auto e=backend->decode_ensemble(samples,hz/1000);
                result.normal_result=e;
                if (e.decoded) { dab::CaptureMuxResult mux; mux.ensemble=e; result.muxes.push_back(mux); }
            }
            accumulator->begin_capture(std::to_string(hz));
            auto accumulate=[&](const dab::EnsembleInfo& e,long phase,int cfo,bool inverted) {
                dab::FicIdentityFragment fragment{std::to_string(hz),phase,cfo,inverted,e.fic};
                auto found=accumulator->add(fragment);
                if (found && result.muxes.empty()) {
                    dab::CaptureMuxResult mux; mux.ensemble=found->ensemble; mux.accumulated=true;
                    result.muxes.push_back(mux);
                }
            };
            if (weak || aircraft) {
                if (result.normal_result) accumulate(*result.normal_result,0,0,false);
                for (const auto& c:result.candidate_attempts)
                    accumulate(c.decode.ensemble,c.refined_frame_phase_sample,
                        c.timing.prs_integer_carrier_offset,c.timing.prs_spectrum_inverted);
            }
            dab::TiiInfo tii;
            // Do not attribute an unseparated TII signal to one of several co-channel ensembles.
            if (result.muxes.size()==1 && !cancelled())
                tii=backend->decode_tii(samples,hz/1000,dab::default_tii_decode_settings(true,2));
            std::lock_guard<std::mutex> lock(d.mutex);
            if (cancelled()) continue;
            d.decode_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-decode_start).count();
            ++d.captures;
            d.fic=result.normal_result ? result.normal_result->fic : dab::FicIdentityEvidence{};
            for (const auto& candidate:result.candidate_attempts)
                if (candidate.decode.ensemble.fic.valid_fibs>d.fic.valid_fibs) d.fic=candidate.decode.ensemble.fic;
            d.mux_count=result.muxes.size(); d.tii=std::move(tii);
            d.ensemble={}; d.route.clear(); d.ensembles.clear(); d.observed=observed;
            for (const auto& mux:result.muxes) {
                char id[24];snprintf(id,sizeof(id)," [%06X]",mux.ensemble.ueid);
                if (!d.ensembles.empty()) d.ensembles+="; ";
                d.ensembles+=mux.ensemble.label+id;
            }
            if (!result.muxes.empty()) {
                d.ensemble=result.muxes.front().ensemble;
                if (d.ensemble.fic.valid_fibs>d.fic.valid_fibs) d.fic=d.ensemble.fic;
                d.route=result.muxes.front().accumulated ? "Accumulated FIC" :
                    result.muxes.front().temporal_subscan ? "Temporal subscan" :
                    result.muxes.front().timing ? "PRS candidate" : "FIC";
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(d.mutex); d.error=e.what();
    }
}
void scope_worker(Diagnostics& d) {
    try {
        while (!interrupted && !d.cancel) {
            IQ samples;uint64_t generation;
            {
                std::unique_lock<std::mutex> lock(d.mutex);
                d.ready.wait_for(lock,std::chrono::milliseconds(100),[&] {return d.eof || !d.scope_pending.empty() || d.cancel;});
                if (d.cancel || (d.scope_pending.empty() && d.eof)) break;
                if (d.scope_pending.empty()) continue;
                samples.swap(d.scope_pending);generation=d.scope_generation;
            }
            dab::dx::FicDecodeSettings settings;
            settings.cancel_requested=[&] {return interrupted || d.cancel.load() || generation!=d.generation.load();};
            auto detection=dab::detect_mode_i(samples,{});
            auto points=dab::dx::extract_constellation(samples,768,settings);
            std::lock_guard<std::mutex> lock(d.mutex);
            if (settings.cancel_requested()) continue;
            d.detection=detection;d.scope=std::move(points);++d.scope_sequence;
            d.scope_time=std::chrono::steady_clock::now();
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(d.mutex);d.error=e.what();
    }
}

}

int main(int argc,char **argv) {
    std::string directory, format="s16", channel="12C", profile="normal", device="default";
    unsigned rate=2048000; bool status_stdout=false, audio=true, filter=false, explicit_channel=false, explicit_frequency=false;
    std::string frequency;
    try {
        for (int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if (arg=="--status-stdout") {status_stdout=true;continue;}
            if (arg=="--no-audio") {audio=false;continue;}
            if (arg=="--channel-filter") {filter=true;continue;}
            if (i+1==argc) throw std::runtime_error("Missing value for "+arg);
            std::string value=argv[++i];
            if (arg=="--state-dir") directory=value;
            else if (arg=="--input-format") format=value;
            else if (arg=="--channel") {channel=value;explicit_channel=true;}
            else if (arg=="--frequency") {frequency=value;explicit_frequency=true;}
            else if (arg=="--profile") profile=value;
            else if (arg=="--audio-device") device=value;
            else if (arg=="--sample-rate") rate=std::stoul(value);
            else throw std::runtime_error("Unknown argument "+arg);
        }
        int index=dab_channel_index(channel.c_str());
        uint64_t hz=0;
        if (explicit_frequency) {
            if (explicit_channel || !dab_parse_frequency(frequency.c_str(),&hz))
                throw std::runtime_error("Choose --channel or --frequency (whole Hz, 1..6000000000)");
            channel="free";
        } else if (index>=0) hz=dab_channels[index].hz;
        if (!hz || directory.empty() || (rate<2048000 || rate>100000000) ||
            (format!="s16" && format!="u8") || (profile!="normal" && profile!="weak" && profile!="aircraft"))
            throw std::runtime_error("DAB requires a channel or frequency, state directory, s16/u8 and 2.048..100 MS/s");
        signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal);
        Diagnostics diagnostics;
        std::thread worker(decode,std::ref(diagnostics),profile=="weak",profile=="aircraft");
        // Joining even on an input, decoder or file error keeps radio cleanup deterministic.
        struct Join { Diagnostics& d; std::thread& t; ~Join() {d.cancel=true;d.ready.notify_all();if (t.joinable()) t.join();} } join{diagnostics,worker};
        std::thread scope_thread(scope_worker,std::ref(diagnostics));
        Join scope_join{diagnostics,scope_thread};
#ifdef HAVE_DAB_RADIO
        auto radio=std::make_unique<dab::DabPlusRadioSession>("0x0",device,audio);
        radio->start(hz/1000);
#else
        (void)audio;
        std::cerr<<"DAB+ audio unavailable: build with FAAD2 and ALSA\n";
#endif
        DabResampler resampler(rate);
        std::unique_ptr<dab::StreamingChannelFilter> fir;
        if (filter && rate==2048000) fir=std::make_unique<dab::StreamingChannelFilter>(rate);
        std::vector<unsigned char> raw(65536*4+4); size_t used=0;
        IQ capture, scope_capture, samples, filtered; const size_t capture_size=2048000*8;
        capture.reserve(capture_size+65536);scope_capture.reserve(196608*3+65536);
        uint64_t generation=0, total_bytes=0, boundary=0, input_samples=0;
        bool tuning=false;
        double rms=NAN,peak=NAN;
#ifdef HAVE_DAB_RADIO
        size_t audio_frames=0;
        auto audio_time=std::chrono::steady_clock::time_point{};
#endif
        auto input_time=std::chrono::steady_clock::time_point{};
        std::string selected, dynamic_label; unsigned slide_generation=0;
        auto next=std::chrono::steady_clock::now(),next_control=next;
        auto submit=[&] {
            std::lock_guard<std::mutex> lock(diagnostics.mutex);
            if (!diagnostics.pending.empty()) ++diagnostics.dropped;
            diagnostics.pending.swap(capture); diagnostics.pending_observed=time(nullptr);
            diagnostics.pending_hz=hz;diagnostics.pending_generation=generation;capture.clear();diagnostics.ready.notify_all();
        };
        auto publish=[&] {
            bool input_live=input_time.time_since_epoch().count() && std::chrono::steady_clock::now()-input_time<std::chrono::seconds(1);
            json_object *root=json_object_new_object();
            num(root,"generation",generation);num(root,"tuning",tuning);num(root,"input_samples",input_samples);
            num(root,"rms",input_live ? rms : NAN);num(root,"peak",input_live ? peak : NAN);num(root,"capture_percent",100.0*capture.size()/capture_size);
            str(root,"channel",channel); num(root,"frequency",hz); str(root,"profile",profile);
#ifdef HAVE_DAB_RADIO
            std::ifstream command(directory+"/station"); std::string requested;
            if (!(command>>requested) && selected.empty()) {
                const auto services=radio->services();
                if (!services.empty()) {
                    char sid[24];std::snprintf(sid,sizeof(sid),"0x%X",services.front().service_id);
                    requested=sid;
                }
            }
            // Select once per tuned ensemble. Later discovery updates must not
            // override the listener's explicit station choice.
            if (!requested.empty() && requested!=selected) {
                radio->select_station(requested); selected=requested;
                dynamic_label.clear(); slide_generation=0; unlink((directory+"/slide").c_str());
            }
            std::ifstream mute_file(directory+"/mute");int muted=0;mute_file>>muted;radio->set_muted(muted!=0);
            radio->check_error();
            if (auto slide=radio->take_slide()) {
                atomic_file(directory+"/slide",slide->encoded.data(),slide->encoded.size()); ++slide_generation;
            }
            if (auto label=radio->take_dynamic_label()) dynamic_label=label->text;
            str(root,"radio_ensemble",radio->ensemble_label()); str(root,"station",radio->selected_station());
            auto frames=radio->decoded_audio_frames();
            if (frames>audio_frames) audio_time=std::chrono::steady_clock::now();
            audio_frames=frames;
            num(root,"audio_active",input_live && radio->fic_synced() && audio_time.time_since_epoch().count() &&
                std::chrono::steady_clock::now()-audio_time<std::chrono::seconds(1));
            num(root,"signal_synced",input_live && radio->dab_signal_synced());
            num(root,"synced",input_live && radio->fic_synced()); num(root,"audio_frames",radio->decoded_audio_frames());
            auto metrics=radio->metrics();
            num(root,"radio_available",1); num(root,"dropped_iq",metrics.dropped_iq);
            num(root,"iq_queue_ms",metrics.iq_samples/2048.0);
            num(root,"iq_queue_peak_ms",metrics.iq_peak_samples/2048.0);
            num(root,"iq_queue_capacity_ms",metrics.iq_capacity_samples/2048.0);
            num(root,"audio_queue_frames",metrics.audio_frames);
            num(root,"audio_queue_peak_frames",metrics.audio_peak_frames);
            num(root,"audio_queue_capacity_frames",metrics.audio_capacity_frames);
            num(root,"dropped_audio",metrics.dropped_audio);
            num(root,"alsa_underruns",audio ? double(metrics.alsa_underruns) : NAN);
            num(root,"rejected_superframe_candidates",metrics.rejected_superframe_candidates);
            num(root,"damaged_audio_units",metrics.damaged_audio_units);
            num(root,"aac_decode_errors",metrics.aac_decode_errors);
            num(root,"concealed_audio_frames",metrics.concealed_audio_frames);
            auto *services=json_object_new_array();
            for (const auto& s:radio->services()) {
                auto *item=json_object_new_object(); num(item,"id",s.service_id); str(item,"label",s.label);
                num(item,"bitrate",s.bitrate_kbps); json_object_array_add(services,item);
            }
            json_object_object_add(root,"services",services);
#else
            num(root,"radio_available",0);
#endif
            str(root,"selection",selected); str(root,"dynamic_label",dynamic_label); num(root,"slide",slide_generation);
            {
                std::lock_guard<std::mutex> lock(diagnostics.mutex);
                str(root,"ensemble",diagnostics.ensemble.label); num(root,"ueid",diagnostics.ensemble.ueid);
                double age=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-diagnostics.scope_time).count();
                bool fresh=diagnostics.scope_sequence && age<1500;
                num(root,"snr",fresh ? diagnostics.detection.estimated_snr_db : NAN);
                num(root,"cp",fresh ? diagnostics.detection.cp_metric_median : NAN);
                num(root,"cfo",fresh && !diagnostics.scope.points.empty() ? diagnostics.scope.fine_offset_hz+1000*diagnostics.scope.integer_carrier_offset : NAN);
                num(root,"mer",fresh ? diagnostics.scope.quality.mean_mer_db : NAN);
                num(root,"scope_sequence",diagnostics.scope_sequence);num(root,"scope_age_ms",age);
                auto *points=json_object_new_array();
                for (const auto& p:diagnostics.scope.points) {
                    json_object_array_add(points,json_object_new_double(p.value.real()));
                    json_object_array_add(points,json_object_new_double(p.value.imag()));
                }
                json_object_object_add(root,"constellation",points);
                const auto& fic=diagnostics.fic;
                num(root,"fib_ok",fic.valid_fibs);num(root,"figs",fic.parsed_figs);
                num(root,"fib_bad",fic.codewords_decoded*3>=fic.valid_fibs ? fic.codewords_decoded*3-fic.valid_fibs : 0);
#ifndef HAVE_DAB_RADIO
                num(root,"signal_synced",fresh && diagnostics.detection.phase_sync);
                num(root,"synced",diagnostics.ensemble.decoded);
#endif
                 num(root,"observed",diagnostics.observed);
                num(root,"mux_count",diagnostics.mux_count); num(root,"captures",diagnostics.captures);
                num(root,"dropped_dx",diagnostics.dropped); str(root,"route",diagnostics.route);str(root,"dx_ensembles",diagnostics.ensembles);
                num(root,"dx_decode_ms",diagnostics.decode_ms);
                str(root,"error",diagnostics.error);
                auto *items=json_object_new_array();
                for (const auto& t:diagnostics.tii.candidates) {
                    auto *item=json_object_new_object();
                    num(item,"main",t.main_id); num(item,"sub",t.sub_id); num(item,"level",t.level);
                    num(item,"confidence",t.confidence); num(item,"hits",t.hits);
                    num(item,"confirmed",t.confirmed); num(item,"ambiguous",t.ambiguous);
                    json_object_array_add(items,item);
                }
                json_object_object_add(root,"tii",items);
            }
            const char *json=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN);
            try {
                if (status_stdout) {
                    if (std::fputs(json,stdout)==EOF || std::fputc('\n',stdout)==EOF || std::fflush(stdout))
                        throw std::runtime_error("Cannot publish DAB status to parent");
                } else atomic_file(directory+"/state.json",json,strlen(json));
            } catch (...) {json_object_put(root);throw;}
            json_object_put(root);
        };
        while (!interrupted) {
            // Control files are not part of the per-IQ-block hot path.
            if (tuning || std::chrono::steady_clock::now()>=next_control) {
                next_control=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
                uint64_t requested_generation=0;std::string requested_channel;
                std::ifstream request(directory+"/channel");request>>requested_generation>>requested_channel;
                int requested_index=dab_channel_index(requested_channel.c_str());
                uint64_t requested_hz=requested_index>=0 ? dab_channels[requested_index].hz : 0;
                if (requested_channel=="free") {
                    std::string value;
                    if (request>>value) dab_parse_frequency(value.c_str(),&requested_hz);
                }
                if (requested_generation>generation && requested_hz) {
                    generation=requested_generation;channel=requested_channel;hz=requested_hz;
                    {
                        std::lock_guard<std::mutex> lock(diagnostics.mutex);
                        diagnostics.generation=generation;diagnostics.pending.clear();diagnostics.scope_pending.clear();
                        diagnostics.ensemble={};diagnostics.tii={};diagnostics.fic={};diagnostics.scope={};diagnostics.detection={};
                        diagnostics.scope_sequence=0;diagnostics.observed=0;diagnostics.mux_count=0;
                        diagnostics.captures=diagnostics.dropped=0;diagnostics.route.clear();diagnostics.ensembles.clear();diagnostics.error.clear();
                        diagnostics.decode_ms=NAN;
                    }
                    tuning=true;used=0;input_samples=0;capture.clear();scope_capture.clear();resampler.reset();if (fir) fir->reset();
                    selected.clear();dynamic_label.clear();slide_generation=0;rms=peak=NAN;
                    input_time={};
                    unlink((directory+"/station").c_str());unlink((directory+"/slide").c_str());
#ifdef HAVE_DAB_RADIO
                    audio_frames=0;audio_time={};
                    radio.reset();radio=std::make_unique<dab::DabPlusRadioSession>("0x0",device,audio);radio->start(hz/1000);
#endif
                    publish();
                }
                if (tuning) {
                    uint64_t ack_generation=0,ack_boundary=0,ack_hz=0;
                    std::ifstream ack(directory+"/tuned");
                    if (ack>>ack_generation>>ack_hz>>ack_boundary && ack_generation==generation && ack_hz==hz) {
                        boundary=ack_boundary;tuning=false;
                    }
                }
            }
            struct pollfd fd{STDIN_FILENO,POLLIN,0};
            int result=poll(&fd,1,100);
            if (result<0 && errno!=EINTR) throw std::runtime_error("DAB IQ poll failed");
            if (result>0) {
                ssize_t count=read(STDIN_FILENO,raw.data()+used,raw.size()-used);
                if (count<0 && errno!=EINTR) throw std::runtime_error("DAB IQ read failed");
                if (!count) break;
                if (count>0) {used+=count;total_bytes+=count;}
                size_t stride=format=="u8" ? 2 : 4;
                if (tuning) {used=0;continue;}
                uint64_t start=total_bytes-used;
                uint64_t aligned=std::max(boundary,((start+stride-1)/stride)*stride);
                size_t skip=std::min<uint64_t>(used,aligned-start);
                used-=skip;memmove(raw.data(),raw.data()+skip,used);
                size_t n=used/stride;
                samples.resize(n);
                for (size_t i=0;i<n;++i) {
                    if (stride==2) samples[i]={(raw[2*i]-127.5f)/128,(raw[2*i+1]-127.5f)/128};
                    else {
                        int16_t re=int16_t(unsigned(raw[4*i])|(unsigned(raw[4*i+1])<<8));
                        int16_t im=int16_t(unsigned(raw[4*i+2])|(unsigned(raw[4*i+3])<<8));
                        samples[i]={re/32768.0f,im/32768.0f};
                    }
                }
                used-=n*stride; memmove(raw.data(),raw.data()+n*stride,used);
                resampler.process(samples);input_samples+=samples.size();
                if (fir) {fir->process(samples.data(),samples.size(),filtered);samples.swap(filtered);}
#ifdef HAVE_DAB_RADIO
                radio->push_samples(samples.data(),samples.size());
#endif
                if (!samples.empty()) {
                    input_time=std::chrono::steady_clock::now();
                    double sum=0,maximum=0;
                    for (auto value:samples) {double power=std::norm(value);sum+=power;maximum=std::max(maximum,power);}
                    rms=10*log10(std::max(1e-20,sum/samples.size()));peak=10*log10(std::max(1e-20,maximum));
                }
                scope_capture.insert(scope_capture.end(),samples.begin(),samples.end());
                if (scope_capture.size()>=196608*3) {
                    std::lock_guard<std::mutex> lock(diagnostics.mutex);
                    diagnostics.scope_pending.swap(scope_capture);scope_capture.clear();
                    diagnostics.scope_generation=generation;diagnostics.ready.notify_all();
                }
                capture.insert(capture.end(),samples.begin(),samples.end());
                if (capture.size()>=capture_size) submit();
            }
            if (std::chrono::steady_clock::now()>=next) {
                publish(); next=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            }
        }
        if (!interrupted && capture.size()>=196608) submit();
        {std::lock_guard<std::mutex> lock(diagnostics.mutex); diagnostics.eof=true; diagnostics.ready.notify_all();}
#ifdef HAVE_DAB_RADIO
        if (!interrupted) radio->finish();
#endif
        // At EOF retain the final diagnostic result, including short IQ fixtures.
        worker.join();scope_thread.join();
        // Join guard expects a joinable thread only during exception cleanup.
        publish();
        return 0;
    } catch (const std::exception& e) {std::cerr<<"DAB: "<<e.what()<<'\n';return 1;}
}
