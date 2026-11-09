#!/usr/bin/env python3
"""Regenerate the cached LDPC shader; shaderc is a build-time tool only."""
import ctypes as c
import ctypes.util
from pathlib import Path
import struct
stem = "ldpc_t2_cached"

directory = Path(__file__).resolve().parents[1] / 'src/dsp/vulkan'
lib = c.CDLL(ctypes.util.find_library('shaderc_shared') or 'libshaderc_shared.so')

def function(name, result, *args):
    f = getattr(lib, 'shaderc_' + name)
    f.restype, f.argtypes = result, args
    return f

init = function('compiler_initialize', c.c_void_p)
release = function('compiler_release', None, c.c_void_p)
options_init = function('compile_options_initialize', c.c_void_p)
options_release = function('compile_options_release', None, c.c_void_p)
optimize = function('compile_options_set_optimization_level', None, c.c_void_p, c.c_int)
compile_shader = function('compile_into_spv', c.c_void_p, c.c_void_p, c.c_char_p,
                          c.c_size_t, c.c_int, c.c_char_p, c.c_char_p, c.c_void_p)
status = function('result_get_compilation_status', c.c_int, c.c_void_p)
error = function('result_get_error_message', c.c_char_p, c.c_void_p)
length = function('result_get_length', c.c_size_t, c.c_void_p)
data = function('result_get_bytes', c.c_void_p, c.c_void_p)
result_release = function('result_release', None, c.c_void_p)
compiler, options, result = init(), options_init(), None
try:
    assert compiler and options, 'shaderc initialization failed'
    optimize(options, 2)  # performance; target defaults to Vulkan 1.0
    source = (directory/(stem+'.comp')).read_bytes()
    result = compile_shader(compiler, source, len(source), 2,
                            (stem+'.comp').encode(), b'main', options)
    assert result and status(result) == 0, error(result).decode() if result else 'no result'
    binary = c.string_at(data(result), length(result))
    words = struct.unpack('<%dI' % (len(binary)//4), binary)
    lines = ['// Generated from '+stem+'.comp by tools/gen_cached_spirv.py.',
             '#pragma once', '#include <cstdint>',
             'static const uint32_t k_'+stem+'_spv[] = {']
    lines += ['    '+', '.join('0x%08x' % v for v in words[i:i+8])+','
              for i in range(0, len(words), 8)]
    lines += ['};', '']
    (directory/(stem+'_spirv.h')).write_text('\n'.join(lines))
    print('wrote '+stem+'_spirv.h (%d bytes SPIR-V)' % len(binary))
finally:
    if result: result_release(result)
    if options: options_release(options)
    if compiler: release(compiler)
