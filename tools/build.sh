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

build_manager() {
    case " $1 " in
        *' debian '*|*' ubuntu '*|*' linuxmint '*|*' raspbian '*) echo apt ;;
        *' fedora '*|*' rhel '*) echo dnf ;;
        *' suse '*|*' opensuse '*|*' opensuse-leap '*|*' opensuse-tumbleweed '*) echo zypper ;;
        *' arch '*|*' manjaro '*) echo pacman ;;
        *) echo unknown ;;
    esac
}

# Optional runtime drivers are installed individually for the receiver in use.
build_sdr_hint() {
    local manager=$1 driver=$2 package plugin
    case $driver in
        lime)
            case $manager in
                apt) package='sudo apt install liblimesuite-dev' ;;
                pacman) package='sudo pacman -S --needed limesuite' ;;
                dnf|zypper) package="sudo $manager install 'pkgconfig(LimeSuite)'" ;;
                *) package='install the LimeSuite development files' ;;
            esac
            printf '  For native LimeSDR Mini RX: %s\n' "$package"
            echo '  Then rebuild in a fresh build directory.'
            return ;;
        utility)
            case $manager in
                apt) package='sudo apt install soapysdr-tools' ;;
                dnf) package='sudo dnf install SoapySDR' ;;
                pacman) package='sudo pacman -S --needed soapysdr' ;;
                *) package='install the package providing SoapySDRUtil' ;;
            esac
            printf '  To inspect Soapy drivers: %s\n' "$package"
            echo '  Then run SoapySDRUtil --info.'
            return ;;
        sdrplay) echo '  For SDRplay RSP: install the SDRplay API v3, then SoapySDRPlay3 (see docs/receivers.md).'; return ;;
        airspyhf) plugin=SoapyAirspyHF ;;
        rfspace) plugin='SoapyOsmo RFSpace module' ;;
        hackrf) plugin=SoapyHackRF ;;
        rtlsdr) plugin=SoapyRTLSDR ;;
        airspy) plugin=SoapyAirspy ;;
    esac
    case $manager:$driver in
        apt:*) package="sudo apt install soapysdr-module-$driver" ;;
        pacman:rfspace) package='sudo pacman -S --needed soapyosmo' ;;
        pacman:hackrf|pacman:airspy|pacman:rtlsdr) package="sudo pacman -S --needed soapy$driver" ;;
        dnf:airspyhf|dnf:rtlsdr) package="sudo dnf install soapy-$driver" ;;
        *) package="install $plugin for the SoapySDR version shown by SoapySDRUtil --info" ;;
    esac
    printf '  For %s: %s\n' "$driver" "$package"
}

build_package_help() {
    local manager=$1 key package
    shift
    local -a packages=() install
    case $manager in
        apt|dnf|zypper) install=("$manager" install) ;;
        pacman) install=(pacman -S --needed) ;;
        *) echo 'Use your package manager to find the tools and development files named above.'; return ;;
    esac
    # RPM distributions can resolve pkg-config capabilities directly.
    local -A apt=([cc]=build-essential [gcc]=build-essential [g++]=build-essential
        [pkg-config]=pkg-config [rustc]=rustc [cargo]=cargo [gtk+-3.0]=libgtk-3-dev
        [glib-2.0]=libglib2.0-dev [json-c]=libjson-c-dev [fftw3f]=libfftw3-dev
        [SoapySDR]=libsoapysdr-dev [uhd]=libuhd-dev [alsa]=libasound2-dev
        [samplerate]=libsamplerate0-dev [sdl2]=libsdl2-dev [libvlc]=libvlc-dev [vlc-plugin-base]=vlc-plugin-base [zvbi-0.2]=libzvbi-dev)
    local -A arch=([cc]=gcc [g++]=gcc [pkg-config]=pkgconf [rustc]=rust [cargo]=rust
        [gtk+-3.0]=gtk3 [glib-2.0]=glib2 [fftw3f]=fftw [SoapySDR]=soapysdr [uhd]=libuhd
        [alsa]=alsa-lib [samplerate]=libsamplerate [zvbi-0.2]=zvbi)
    for key do
        case $key in
            tool:*) key=${key#tool:}
                case $key in
                    gcc-[0-9]*|g++-[0-9]*)
                        if [[ $manager != apt || ! $key =~ ^(gcc|g\+\+)-[0-9]+$ ]]; then
                            printf 'Check the configured compiler %s and its PATH.\n' "$key"; continue
                        fi ;;
                    make|cc|gcc|g++|pkg-config|rustc|cargo) ;;
                    *) printf 'Check the configured tool %s and its PATH.\n' "$key"; continue ;;
                esac
                case $manager:$key in
                    dnf:g++|zypper:g++) package=gcc-c++ ;;
                    dnf:cc|zypper:cc) package=gcc ;;
                    dnf:pkg-config) package=pkgconf-pkg-config ;;
                    dnf:rustc|zypper:rustc) package=rust ;;
                    apt:*) package=${apt[$key]:-$key} ;;
                    pacman:*) package=${arch[$key]:-$key} ;;
                    *) package=$key ;;
                esac ;;
            *) case $manager in
                apt) package=${apt[$key]:-${key}-dev} ;;
                dnf|zypper) package="pkgconfig($key)" ;;
                pacman) case $key in
                    libav*|libswscale) package=ffmpeg ;;
                    *) package=${arch[$key]:-$key} ;;
                esac ;;
            esac ;;
        esac
        [[ " ${packages[*]} " == *" $package "* ]] || packages+=("$package")
    done
    if ((${#packages[@]})); then
        echo 'Install the missing build dependencies with:'
        printf '  sudo'; printf ' %q' "${install[@]}" "${packages[@]}"; printf '\n'
    fi
}

# Shared by the desktop and native ODROID apt installers.
build_install_apt() {
    local -a prefix=() packages=(build-essential pkg-config libgtk-3-dev libglib2.0-dev
        libjson-c-dev libfftw3-dev libsoapysdr-dev libuhd-dev libboost-system-dev
        libasound2-dev libsamplerate0-dev libsdl2-dev libvlc-dev vlc-plugin-base libavcodec-dev libavformat-dev
        libavutil-dev libswscale-dev libzvbi-dev)
    local compiler
    for compiler in "${CC:-cc}" "${CXX:-g++}"; do
        if [[ $compiler =~ ^(gcc|g\+\+)-[0-9]+$ ]] && ! command -v "$compiler" >/dev/null; then
            packages+=("$compiler")
        fi
    done
    if ! command -v rustup >/dev/null; then
        command -v rustc >/dev/null || packages+=(rustc)
        command -v "${CARGO:-cargo}" >/dev/null || packages+=(cargo)
    fi
    command -v apt-get >/dev/null || { echo 'ERROR: apt-get is required to install Debian packages.' >&2; return 1; }
    if ((EUID!=0)); then
        command -v sudo >/dev/null || { echo 'ERROR: sudo is unavailable. Ask an administrator to install the packages listed by --check.' >&2; return 1; }
        prefix=(sudo)
    fi
    "${prefix[@]}" apt-get update || return
    local package
    for package do
        # Older Ubuntu ODROID images have the original unsuffixed SRT package.
        if [[ $package == libsrt-dev ]] && apt-cache show libsrt-openssl-dev >/dev/null 2>&1; then
            package=libsrt-openssl-dev
        fi
        packages+=("$package")
    done
    "${prefix[@]}" apt-get install --no-install-recommends "${packages[@]}"
}

build_usage() {
    cat <<'HELP'
Bouwen op Linux
  Eerste installatie op Mint, Debian of Ubuntu     bash tools/build.sh --install-deps
  Alleen controleren wat er ontbreekt             bash tools/build.sh --check
  Bouwen met de geïnstalleerde pakketten          bash tools/build.sh

Opties
  --install-deps     Installeer de vereiste pakketten met APT, controleer en bouw.
  --check            Controleer de bouwomgeving. Toont hulp bij ontbrekende pakketten.
  -j, --jobs N       Aantal gelijktijdige bouwtaken (standaard 1).
  --build-dir MAP    Bouwmap vanaf de projectmap (standaard build).
  -h, --help         Toon deze uitleg.

Voor een ODROID-N2/N2+ met 64-bits Debian of Ubuntu:
  bash tools/build_odroid_n2.sh --help

Voor een eigen compiler of bibliotheekpad worden CC, CXX, CPPFLAGS, CFLAGS,
CXXFLAGS, LDFLAGS, LDLIBS, CARGO en PKG_CONFIG_PATH uit de omgeving gebruikt.
HELP
}

build_main() (
    local check=0 install_deps=0 jobs=1 build_dir=build option errors=0 warnings=0 temp
    if (($# == 0)); then echo 'Hulp en voorbeelden: bash tools/build.sh --help'; fi
    while (($#)); do
        option=$1
        case $option in
            --check) check=1; shift ;;
            --install-deps) install_deps=1; shift ;;
            -j|--jobs|--build-dir)
                (($#>=2)) || { echo "Missing value for $option" >&2; return 2; }
                if [[ $option == --build-dir ]]; then build_dir=$2; else jobs=$2; fi
                shift 2 ;;
            -j[0-9]*) jobs=${option#-j}; shift ;;
            --jobs=*) jobs=${option#*=}; shift ;;
            --build-dir=*) build_dir=${option#*=}; shift ;;
            -h|--help) build_usage; return 0 ;;
            *) echo "Unknown option: $option" >&2; build_usage >&2; return 2 ;;
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
    local program version
    local -a missing=()
    local ID='' ID_LIKE='' PRETTY_NAME=Linux
    if [[ -r /etc/os-release ]]; then source /etc/os-release; fi
    local manager
    manager=$(build_manager "$ID $ID_LIKE")
    if ((install_deps)); then
        if ((check)); then echo 'Choose --check or --install-deps.' >&2; return 2; fi
        if [[ $manager != apt ]]; then echo 'Use --check for package instructions on this distribution.' >&2; return 2; fi
        build_install_apt || return
    fi
    printf 'Checking build dependencies on %s.\n' "$PRETTY_NAME"
    for program in make pkg-config "${cc[0]-}" "${cxx[0]-}" "${CARGO:-cargo}" rustc; do
        if [[ -n $program ]] && command -v -- "$program" >/dev/null; then
            if [[ $program == rustc || $program == "${CARGO:-cargo}" ]]; then
                if ! version=$("$program" --version 2>&1) ||
                   ! [[ $version =~ ^(rustc|cargo)[[:space:]]+([0-9]+)\.([0-9]+) ]] ||
                   (( BASH_REMATCH[2] < 1 || (BASH_REMATCH[2] == 1 && BASH_REMATCH[3] < 70) )); then
                    report ERROR "$program: Rust/Cargo 1.70+ required ($version)"
                    if command -v rustup >/dev/null; then echo 'Update and select the Rust toolchain: rustup update stable && rustup default stable'
                    else echo 'Install a current Rust toolchain from your distribution or https://rustup.rs'; fi
                    continue
                fi
            fi
            report OK "$program: found"
        else
            report ERROR "${program:-(empty compiler)}: executable missing"
            missing+=("tool:$program")
        fi
    done
    if [[ $manager == apt ]]; then
        if [[ $(dpkg-query -W -f='${db:Status-Status}' vlc-plugin-base 2>/dev/null) == installed ]]; then
            report OK 'vlc-plugin-base: embedded player runtime plugins installed'
        else
            report ERROR 'vlc-plugin-base: runtime plugins for the embedded VLC player missing'
            missing+=(vlc-plugin-base)
        fi
    fi
    local source
    for source in third_party/leansdr/src/apps/leandvb.cc third_party/ldpc/ldpc_tool.cc \
                  components/dvbt/src/dvbt_outer.c components/dvbt2/rust/Cargo.toml \
                  components/tsscatterfix/src/recovery.c; do
        [[ -f $source ]] || report ERROR "Bundled source missing: $source (extract the complete project archive again)"
    done
    temp=$(mktemp -d "${TMPDIR:-/tmp}/linrad-build-check.XXXXXXXX")
    trap 'rm -rf -- "$temp"' EXIT
    probe() {
        local label=$1
        shift
        command -v -- "${1:-}" >/dev/null || return 0
        if "$@" >"$temp/probe.log" 2>&1; then report OK "$label: headers/link check"
        else report ERROR "$label: compile/link failed"; cat "$temp/probe.log"
            if [[ $label == 'DVB-T2 C++20' ]]; then
                echo 'Select a C++20 compiler with <bit> support using CXX (for example CXX=g++-10 on Ubuntu 20.04).'
            else
                echo 'Check the first compiler/linker error above and your compiler and library paths.'
            fi
        fi
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
    local module header flags libraries probe_body label errors_before
    if command -v pkg-config >/dev/null; then
        while IFS='|' read -r module header; do
            label=$module
            probe_body='return 0;'
            case $module in
                sdl2) label='sdl2 (FM-ATV receiver threads)'; probe_body='SDL_mutex *m = SDL_CreateMutex(); SDL_DestroyMutex(m); return 0;' ;;
                fftw3f) label='fftw3f (FM-ATV and IQ filtering)'; probe_body='fftwf_free(fftwf_alloc_real(16)); return 0;' ;;
            esac
            if ! pkg-config --exists "$module"; then
                report ERROR "$label: development package missing (custom installation? Add its .pc directory to PKG_CONFIG_PATH)"
                missing+=("$module")
                continue
            fi
            if ! flags=$(pkg-config --cflags "$module") || ! libraries=$(pkg-config --libs "$module"); then
                report ERROR "$label: cannot read build flags"; missing+=("$module"); continue
            fi
            split_words pc_cflags "$flags"
            split_words pc_libs "$libraries"
            printf '#define _POSIX_C_SOURCE 200809L\n#include <sys/types.h>\n#include <%s>\nint main(void) { %s }\n' "$header" "$probe_body" >"$temp/probe.c"
            errors_before=$errors
            probe "$label" "${cc[@]}" "${cppflags[@]}" "${cflags[@]}" "${pc_cflags[@]}" "$temp/probe.c" -o "$temp/probe" "${ldflags[@]}" "${pc_libs[@]}" "${ldlibs[@]}"
            if ((errors>errors_before)); then missing+=("$module"); fi
        done <<'LIBRARIES'
gtk+-3.0|gtk/gtk.h
glib-2.0|glib.h
json-c|json-c/json.h
fftw3f|fftw3.h
SoapySDR|SoapySDR/Device.h
uhd|uhd.h
alsa|alsa/asoundlib.h
samplerate|samplerate.h
sdl2|SDL.h
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
            report ERROR "Cannot read FFmpeg build flags for ${ffmpeg_modules[*]}"
            for module in "${ffmpeg_modules[@]}"; do
                pkg-config --exists "$module" || missing+=("$module")
            done
        fi
        if pkg-config --exists faad2 alsa; then
            report OK 'DAB+: audio and MOT slideshow enabled'
        else report WARNING 'DAB+: FAAD2 or ALSA unavailable; FIC/TII decoding only'; fi
        if pkg-config --exists LimeSuite; then report OK 'LimeSuite: native Lime RX enabled'
        else report WARNING 'LimeSuite unavailable; native LimeSDR Mini RX unavailable'; build_sdr_hint "$manager" lime; fi
    else
        echo 'Library checks require pkg-config. Rerun the check after installing it.'
    fi
    for program in ffmpeg vlc cvlc ffplay direwolf; do
        if command -v -- "$program" >/dev/null; then report OK "$program: found"
        else report WARNING "$program missing; its optional feature is unavailable"; fi
    done
    local drivers driver missing_drivers=0
    if command -v SoapySDRUtil >/dev/null && drivers=$(SoapySDRUtil --info 2>&1); then
        drivers=$(printf '%s\n' "$drivers" | sed -n '/Available factories/p')
        for driver in hackrf rfspace rtlsdr sdrplay airspyhf airspy; do
            if [[ $drivers =~ (^|[^[:alnum:]_])$driver([^[:alnum:]_]|$) ]]; then report OK "Soapy driver $driver"
            else
                report WARNING "Soapy driver $driver missing"
                build_sdr_hint "$manager" "$driver"
                missing_drivers=1
            fi
        done
        if ((missing_drivers)); then echo '  After adding a driver, check Available factories in SoapySDRUtil --info.'; fi
    else
        report WARNING 'Cannot inspect optional Soapy drivers'
        build_sdr_hint "$manager" utility
    fi
    printf '\nDependency check: %s error(s), %s warning(s).\n' "$errors" "$warnings"
    if ((errors)); then
        build_package_help "$manager" "${missing[@]}"
        echo 'After fixing the errors, rerun your build command.'
        return 1
    fi
    ((check==0)) || return 0
    make -C "$PROJECT_ROOT" "-j$jobs" all "BUILD_DIR=$build_dir" \
        "CC=$cc_text" "CXX=$cxx_text" "CPPFLAGS=$cpp_text" "CFLAGS=$c_text" \
        "CXXFLAGS=$cxxflags_text" "LDFLAGS=$ld_text" "LDLIBS=$libs_text"
)

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    build_main "$@"
fi
