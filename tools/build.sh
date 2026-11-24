#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Check the complete Linux build, then run make. No Python build dependency.
set -euo pipefail
PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

# Parse compiler/flag words without evaluating substitutions or expanding globs.
# Supports quotes and backslash escapes, as used by CC='ccache clang'.
split_words() {
    local -n result=$1
    local input=$2 word='' quote='' char next started=0 i
    result=()
    for ((i=0; i<${#input}; i++)); do
        char=${input:i:1}
        if [[ $quote == "'" ]]; then
            if [[ $char == "'" ]]; then quote=''; else word+=$char; fi
        elif [[ $quote == '"' ]]; then
            case $char in
                '"') quote='' ;;
                \\)
                    next=${input:i+1:1}
                    if [[ $next == '"' || $next == '\' || $next == '$' || $next == '`' ]]; then
                        word+=$next; i=$((i+1))
                    else word+=$char; fi ;;
                *) word+=$char ;;
            esac
        else
            case $char in
                "'"|'"') quote=$char; started=1 ;;
                \\)
                    i=$((i+1))
                    ((i<${#input})) || { echo 'ERROR: trailing backslash in compiler flags' >&2; return 2; }
                    word+=${input:i:1}; started=1 ;;
                ' '|$'\t'|$'\n')
                    if ((started)); then result+=("$word"); word=''; started=0; fi ;;
                *) word+=$char; started=1 ;;
            esac
        fi
    done
    [[ -z $quote ]] || { echo 'ERROR: unterminated quote in compiler flags' >&2; return 2; }
    if ((started)); then result+=("$word"); fi
}

build_usage() {
    cat <<'HELP'
Usage: bash tools/build.sh [--check] [-j N] [--build-dir DIR]
  --check          Check dependencies only; do not run make.
  -j, --jobs N     Parallel build jobs (default: 1).
  --build-dir DIR  Build directory relative to the repository (default: build).
  -h, --help       Show this help.
Environment: CC, CXX, CPPFLAGS, CFLAGS, CXXFLAGS, LDFLAGS, LDLIBS,
             CARGO, PKG_CONFIG_PATH. Compiler wrappers and quoted flags work.
HELP
}

build_main() (
    local check=0 jobs=1 build_dir=build option errors=0 warnings=0 temp
    while (($#)); do
        option=$1
        case $option in
            --check) check=1; shift ;;
            -j|--jobs|--build-dir)
                (($#>=2)) || { echo "Missing value for $option" >&2; return 2; }
                if [[ $option == --build-dir ]]; then build_dir=$2; else jobs=$2; fi
                shift 2 ;;
            -j[0-9]*) jobs=${option#-j}; shift ;;
            --jobs=*) jobs=${option#*=}; shift ;;
            --build-dir=*) build_dir=${option#*=}; shift ;;
            -h|--help) build_usage; return 0 ;;
            *) echo "Unknown option: $option" >&2; return 2 ;;
        esac
    done
    [[ $jobs =~ ^[1-9][0-9]*$ && -n $build_dir ]] || {
        echo 'Jobs must be a positive integer; build directory must not be empty.' >&2; return 2;
    }
    [[ $(uname -s) == Linux ]] || { echo 'ERROR: this build requires Linux (ALSA and hidraw).' >&2; return 1; }
    cd -- "$PROJECT_ROOT"
    report() {
        printf '[%s] %s\n' "$1" "$2"
        case $1 in ERROR) errors=$((errors+1)) ;; WARNING) warnings=$((warnings+1)) ;; esac
    }
    local cc_text=${CC-cc} cxx_text=${CXX-g++} cpp_text=${CPPFLAGS-}
    local c_text=${CFLAGS-'-O2 -Wall -Wextra -std=c11'}
    local cxxflags_text=${CXXFLAGS-'-O3 -Wall -Wextra -std=gnu++17'}
    local ld_text=${LDFLAGS-} libs_text=${LDLIBS-}
    local -a cc cxx cppflags cflags cxxflags ldflags ldlibs pc_cflags pc_libs
    split_words cc "$cc_text"
    split_words cxx "$cxx_text"
    split_words cppflags "$cpp_text"
    split_words cflags "$c_text"
    split_words cxxflags "$cxxflags_text"
    split_words ldflags "$ld_text"
    split_words ldlibs "$libs_text"
    local program
    for program in make pkg-config "${cc[0]-}" "${cxx[0]-}" "${CARGO:-cargo}" rustc; do
        if [[ -n $program ]] && command -v -- "$program" >/dev/null; then
            report OK "$program: found"
        else report ERROR "${program:-(empty compiler)}: executable missing"; fi
    done
    local source
    for source in third_party/leansdr/src/apps/leandvb.cc third_party/ldpc/ldpc_tool.cc \
                  components/dvbt/src/dvbt_outer.c components/dvbt2/rust/Cargo.toml \
                  components/tsscatterfix/src/recovery.c; do
        [[ -f $source ]] || report ERROR "Bundled source missing: $source"
    done
    ((errors==0)) || { echo 'Build blocked: install the required tools and restore missing sources.'; return 1; }
    temp=$(mktemp -d "${TMPDIR:-/tmp}/linrad-build-check.XXXXXXXX")
    trap 'rm -rf -- "$temp"' EXIT
    probe() {
        local label=$1
        shift
        if "$@" >"$temp/probe.log" 2>&1; then report OK "$label: headers/link check"
        else report ERROR "$label: compile/link failed"; cat "$temp/probe.log"; fi
    }
    cat >"$temp/probe.c" <<'C'
#include <linux/hidraw.h>
#include <pthread.h>
#include <math.h>
int main(void) { return 0; }
C
    probe C "${cc[@]}" "${cppflags[@]}" "${cflags[@]}" "$temp/probe.c" -o "$temp/probe" "${ldflags[@]}" -pthread -lm "${ldlibs[@]}"
    cat >"$temp/probe.cc" <<'CXX'
#include <bit>
#include <thread>
int main() { std::thread t([] {}); t.join(); return std::popcount(7u) != 3; }
CXX
    probe 'DVB-T2 C++20' "${cxx[@]}" "${cppflags[@]}" "${cxxflags[@]}" -std=gnu++20 "$temp/probe.cc" -o "$temp/probe" "${ldflags[@]}" -pthread "${ldlibs[@]}"
    local module header flags libraries
    while IFS='|' read -r module header; do
        if ! pkg-config --print-errors --exists "$module"; then
            report ERROR "$module: development package missing or not found in PKG_CONFIG_PATH"
            continue
        fi
        if ! flags=$(pkg-config --cflags "$module") || ! libraries=$(pkg-config --libs "$module"); then
            report ERROR "$module: cannot read build flags"; continue
        fi
        split_words pc_cflags "$flags"
        split_words pc_libs "$libraries"
        printf '#define _POSIX_C_SOURCE 200809L\n#include <sys/types.h>\n#include <%s>\nint main(void) { return 0; }\n' "$header" >"$temp/probe.c"
        probe "$module" "${cc[@]}" "${cppflags[@]}" "${cflags[@]}" "${pc_cflags[@]}" "$temp/probe.c" -o "$temp/probe" "${ldflags[@]}" "${pc_libs[@]}" "${ldlibs[@]}"
    done <<'LIBRARIES'
gtk+-3.0|gtk/gtk.h
glib-2.0|glib.h
json-c|json-c/json.h
fftw3f|fftw3.h
SoapySDR|SoapySDR/Device.h
uhd|uhd.h
alsa|alsa/asoundlib.h
samplerate|samplerate.h
libvlc|vlc/vlc.h
zvbi-0.2|libzvbi.h
LIBRARIES
    local -a ffmpeg_modules=(libavformat libavcodec libswscale libavutil) ffmpeg_pkg_flags
    split_words ffmpeg_pkg_flags "${FFMPEG_PKG_CONFIG_FLAGS-}"
    if flags=$(pkg-config --print-errors --cflags "${ffmpeg_modules[@]}") &&
       libraries=$(pkg-config "${ffmpeg_pkg_flags[@]}" --print-errors --libs "${ffmpeg_modules[@]}"); then
        split_words pc_cflags "$flags"
        split_words pc_libs "$libraries"
        printf 'int main(void) { return 0; }\n' >"$temp/probe.c"
        probe 'FFmpeg scatterfix API' "${cc[@]}" "${cppflags[@]}" "${cflags[@]}" "${pc_cflags[@]}" \
            components/tsscatterfix/src/media.c "$temp/probe.c" -o "$temp/probe" \
            "${ldflags[@]}" "${pc_libs[@]}" -lm "${ldlibs[@]}"
    else
        report ERROR "Missing FFmpeg build metadata: check ${ffmpeg_modules[*]} with pkg-config"
    fi
    if pkg-config --exists faad2 alsa; then
        report OK 'DAB+: audio and MOT slideshow enabled'
    else report WARNING 'DAB+: FAAD2 or ALSA unavailable; FIC/TII decoding only'; fi
    if pkg-config --exists LimeSuite; then report OK 'LimeSuite: native Lime RX enabled'
    else report WARNING 'LimeSuite unavailable; native LimeSDR Mini RX unavailable'; fi
    for program in ffmpeg vlc cvlc ffplay direwolf; do
        if command -v -- "$program" >/dev/null; then report OK "$program: found"
        else report WARNING "$program missing; its optional feature is unavailable"; fi
    done
    local drivers driver
    if command -v SoapySDRUtil >/dev/null && drivers=$(SoapySDRUtil --info 2>&1); then
        drivers=$(printf '%s\n' "$drivers" | sed -n '/Available factories/p')
        for driver in hackrf rfspace rtlsdr sdrplay airspyhf airspy; do
            if [[ $drivers =~ (^|[^[:alnum:]_])$driver([^[:alnum:]_]|$) ]]; then report OK "Soapy driver $driver"
            else report WARNING "Soapy driver $driver missing"; fi
        done
    else report WARNING 'SoapySDRUtil unavailable; cannot check optional Soapy drivers'; fi
    printf '\nDependency check: %s error(s), %s warning(s).\n' "$errors" "$warnings"
    ((errors==0)) || { echo 'Build blocked. Install the required development packages.'; return 1; }
    ((check==0)) || return 0
    make -C "$PROJECT_ROOT" "-j$jobs" all "BUILD_DIR=$build_dir" \
        "CC=$cc_text" "CXX=$cxx_text" "CPPFLAGS=$cpp_text" "CFLAGS=$c_text" \
        "CXXFLAGS=$cxxflags_text" "LDFLAGS=$ld_text" "LDLIBS=$libs_text"
)

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    build_main "$@"
fi
