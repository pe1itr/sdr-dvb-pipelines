#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Native ODROID-N2/N2+ build for 64-bit Ubuntu/Debian. No Python required.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/build.sh"
ODROID_BUILD_DIR=build-odroid-n2
FFMPEG_TAG=n6.1.6
FFMPEG_COMMIT=f1e3a2bf7a2f2cde936d1ed97f09a26853d20125

odroid_usage() {
    cat <<'HELP'
Usage: bash tools/build_odroid_n2.sh [options]
  --check          Check dependencies and repeater runtime only.
  --install-deps   Install Ubuntu/Debian packages with apt-get (sudo if needed).
  --build-ffmpeg   Build a pinned local FFmpeg with SRT in build-odroid-n2/.
  --ffmpeg PATH    Use an existing SRT-capable FFmpeg.
  --config PATH    Create a configuration after building; never overwrite.
  -j, --jobs N     Parallel build jobs (default: 1).
  -h, --help       Show this help.
Run on a 64-bit aarch64 ODROID; this is not a cross-compiler.
HELP
}

odroid_require_platform() {
    local machine ID='' ID_LIKE='' PRETTY_NAME='Linux'
    machine=$(uname -m)
    if [[ $(uname -s) != Linux || ! $machine =~ ^(aarch64|arm64)$ || $(getconf LONG_BIT) != 64 ]]; then
        echo 'ERROR: run on a 64-bit aarch64 Linux ODROID; this is not a cross-compiler.' >&2
        return 1
    fi
    source /etc/os-release
    if [[ " $ID $ID_LIKE " != *' ubuntu '* && " $ID $ID_LIKE " != *' debian '* ]]; then
        echo 'ERROR: package installation supports Ubuntu/Debian and their Armbian variants.' >&2
        return 1
    fi
    printf 'System: %s %s\n' "$PRETTY_NAME" "$machine"
}

odroid_install_dependencies() {
    local -a prefix=() packages=(
        build-essential pkg-config cargo rustc libgtk-3-dev libglib2.0-dev
        libjson-c-dev libfftw3-dev libsoapysdr-dev libuhd-dev libboost-system-dev
        libasound2-dev libsamplerate0-dev libvlc-dev libavcodec-dev libavformat-dev
        libavutil-dev libswscale-dev libzvbi-dev ffmpeg soapysdr-tools
        soapysdr-module-rtlsdr rtl-sdr
    )
    command -v apt-get >/dev/null || { echo 'ERROR: apt-get is unavailable' >&2; return 1; }
    if ((EUID!=0)); then prefix=(sudo); fi
    if (($1)); then packages+=(git ca-certificates libsrt-dev libssl-dev libx264-dev nasm); fi
    "${prefix[@]}" apt-get update
    "${prefix[@]}" apt-get install -y --no-install-recommends "${packages[@]}"
}

odroid_local_ffmpeg() (
    local jobs=$1 source="$PROJECT_ROOT/$ODROID_BUILD_DIR/ffmpeg-src"
    local prefix="$PROJECT_ROOT/$ODROID_BUILD_DIR/ffmpeg" commit
    mkdir -p -- "$PROJECT_ROOT/$ODROID_BUILD_DIR"
    if [[ ! -e $source ]]; then
        git clone --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git "$source"
    fi
    commit=$(git -C "$source" rev-parse HEAD)
    [[ $commit == "$FFMPEG_COMMIT" ]] || { echo "ERROR: unexpected FFmpeg checkout: $commit" >&2; return 1; }
    cd -- "$source"
    ./configure "--prefix=$prefix" --disable-autodetect --disable-doc --disable-debug \
        --disable-ffplay --disable-shared --enable-static --disable-everything \
        --enable-gpl --enable-libx264 --enable-libsrt --enable-ffmpeg --enable-ffprobe \
        --enable-protocol=file,pipe,libsrt,udp,tcp --enable-demuxer=mpegts,mpegtsraw \
        --enable-muxer=mpegts,null,rawvideo --enable-indev=lavfi \
        --enable-decoder=h264,hevc,mpeg2video,mpeg4,aac,aac_latm,mp2,mp3,ac3,eac3,rawvideo,pcm_s16le,pcm_f32le \
        --enable-encoder=libx264,aac,rawvideo \
        --enable-parser=h264,hevc,mpegvideo,mpeg4video,aac,aac_latm,mpegaudio,ac3 \
        --enable-filter=bwdif,scale,setsar,pad,fps,format,aformat,aresample,anull,null,buffer,buffersink,abuffer,abuffersink,testsrc2,sine,color \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata
    make "-j$jobs"
    make install
)

odroid_check_repeater() {
    local ffmpeg=$1 protocols encoders drivers
    protocols=$("$ffmpeg" -hide_banner -protocols 2>&1) || return
    if ! printf '%s\n' "$protocols" | awk '/^Output:/{out=1; next} out && $1=="srt"{found=1} END{exit !found}'; then
        echo 'ERROR: FFmpeg has no SRT output; use --ffmpeg PATH or --build-ffmpeg.' >&2; return 1
    fi
    encoders=$("$ffmpeg" -hide_banner -encoders 2>&1) || return
    local encoder
    for encoder in libx264 aac; do
        if [[ ! $encoders =~ [[:space:]][A-Z.]+[[:space:]]+$encoder[[:space:]] ]]; then
            echo "ERROR: FFmpeg is missing encoder $encoder" >&2; return 1
        fi
    done
    drivers=$(SoapySDRUtil --info 2>&1) || return
    drivers=$(printf '%s\n' "$drivers" | sed -n '/Available factories/p')
    [[ $drivers =~ (^|[^[:alnum:]_])rtlsdr([^[:alnum:]_]|$) ]] || {
        echo 'ERROR: SoapyRTLSDR is unavailable; install soapysdr-module-rtlsdr.' >&2; return 1;
    }
    echo 'SRT, H.264, AAC and RTL-SDR are available (no receiver or network connection opened).'
}

odroid_write_config() (
    local path=$1 ffmpeg=$2 temporary resolved_ffmpeg
    if [[ -e $path || -L $path ]]; then
        printf 'Preserving existing configuration: %s\n' "$path"
        return 0
    fi
    resolved_ffmpeg=$(command -v -- "$ffmpeg")
    resolved_ffmpeg=$(realpath -- "$resolved_ffmpeg")
    temporary=$(mktemp "${path}.XXXXXXXX")
    trap 'rm -f -- "$temporary"' EXIT
    # ENVIRON avoids awk -v escape interpretation in paths containing backslashes.
    LINRAD_CONFIG_ROOT="$PROJECT_ROOT/$ODROID_BUILD_DIR" LINRAD_CONFIG_FFMPEG="$resolved_ffmpeg" \
    awk '
        BEGIN {
            print "# ODROID-N2 PI6EHV template: set [rtlsdr] frequency (Hz) before reception."
            print "# The DVB-T bandwidth, FEC and GI are examples; match your transmitter."
            print "# frequency=0 prevents starting with an assumed RF frequency."
        }
        /^\[/ { section=$0 }
        {
            key=$0; sub(/=.*/, "", key)
            replacement=""
            if (section=="[paths]") {
                if (key=="sdr") replacement=ENVIRON["LINRAD_CONFIG_ROOT"] "/sdr_to_stdout"
                if (key=="linrad") replacement=ENVIRON["LINRAD_CONFIG_ROOT"] "/linrad_udp_to_stdout"
                if (key=="iq_file") replacement=ENVIRON["LINRAD_CONFIG_ROOT"] "/iq_file_to_stdout"
                if (key=="ffmpeg") replacement=ENVIRON["LINRAD_CONFIG_FFMPEG"]
            }
            if (section=="[session]") {
                if (key=="source") replacement="rtlsdr"
                if (key=="standard") replacement="DVB-T"
                if (key=="sr") replacement="250000"
                if (key=="fec") replacement="1/2"
                if (key=="gi") replacement="auto"
                if (key=="scatterfix") replacement="false"
                if (key=="renderer") replacement="srt"
            }
            if (section=="[rtlsdr]" && key=="frequency") replacement="0"
            if (section=="[srt]") {
                if (key=="url") replacement="srt://44.137.26.85:4001"
                if (key=="x264_preset") replacement="ultrafast"
                if (key=="video_threads") replacement="3"
                if (key=="filter_threads") replacement="1"
            }
            if (replacement!="") print key "=" replacement; else print
        }
    ' "$PROJECT_ROOT/linrad-dvb.example.ini" >"$temporary"
    # Atomically publish a complete file without replacing an existing path.
    if ln -T -- "$temporary" "$path"; then printf 'Created configuration: %s\n' "$path"
    elif [[ -e $path || -L $path ]]; then printf 'Preserving existing configuration: %s\n' "$path"
    else return 1; fi
)

odroid_main() (
    local install=0 build_ffmpeg=0 check=0 jobs=1 ffmpeg=ffmpeg
    local config="$PROJECT_ROOT/pi6ehv-odroid.ini" option
    while (($#)); do
        option=$1
        case $option in
            --check) check=1; shift ;;
            --install-deps) install=1; shift ;;
            --build-ffmpeg) build_ffmpeg=1; shift ;;
            -j|--jobs|--ffmpeg|--config)
                (($#>=2)) || { echo "Missing value for $option" >&2; return 2; }
                case $option in -j|--jobs) jobs=$2 ;; --ffmpeg) ffmpeg=$2 ;; --config) config=$2 ;; esac
                shift 2 ;;
            -j[0-9]*) jobs=${option#-j}; shift ;;
            -h|--help) odroid_usage; return 0 ;;
            *) echo "Unknown option: $option" >&2; return 2 ;;
        esac
    done
    [[ $jobs =~ ^[1-9][0-9]*$ && -n $config && -n $ffmpeg ]] || { echo 'ERROR: invalid jobs or empty path' >&2; return 2; }
    if ((check && (install || build_ffmpeg))); then
        echo 'ERROR: --check cannot install packages or build FFmpeg.' >&2; return 2
    fi
    if ((build_ffmpeg)) && [[ $ffmpeg != ffmpeg ]]; then
        echo 'ERROR: choose --build-ffmpeg or --ffmpeg PATH.' >&2; return 2
    fi
    odroid_require_platform
    if ((install)); then odroid_install_dependencies "$build_ffmpeg"; fi
    export CFLAGS="${CFLAGS-'-O2 -Wall -Wextra -std=c11'} -march=armv8-a -mtune=cortex-a73.cortex-a53"
    export CXXFLAGS="${CXXFLAGS-'-O3 -Wall -Wextra -std=gnu++17'} -march=armv8-a -mtune=cortex-a73.cortex-a53"
    local -a command=(bash "$PROJECT_ROOT/tools/build.sh" --build-dir "$ODROID_BUILD_DIR" -j "$jobs")
    "${command[@]}" --check
    if ((build_ffmpeg)); then
        odroid_local_ffmpeg "$jobs"
        ffmpeg="$PROJECT_ROOT/$ODROID_BUILD_DIR/ffmpeg/bin/ffmpeg"
    elif [[ $ffmpeg == ffmpeg && -f $PROJECT_ROOT/$ODROID_BUILD_DIR/ffmpeg/bin/ffmpeg ]]; then
        ffmpeg="$PROJECT_ROOT/$ODROID_BUILD_DIR/ffmpeg/bin/ffmpeg"
    fi
    odroid_check_repeater "$ffmpeg"
    ((check==0)) || return 0
    "${command[@]}"
    config=$(realpath -m -- "$config")
    odroid_write_config "$config" "$ffmpeg"
    printf '\nSet the RF frequency and DVB-T parameters, then start:\n'
    printf '%q ' "$PROJECT_ROOT/$ODROID_BUILD_DIR/linrad_dvb" --config "$config" --headless
    printf '\n1080p25 encoding: ultrafast / 3 video threads / 1 filter thread.\n'
    echo 'Measure sustained reception on the N2; this build does not prove realtime encoding capacity.'
)

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    odroid_main "$@"
fi
