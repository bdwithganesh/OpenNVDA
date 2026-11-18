#!/usr/bin/env python3
"""Compile real SPIR-V/AIR inputs to sm_89 code without accessing a GPU."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
AFFINE = '''#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(local_size_x=64) in;
layout(buffer_reference, std430, buffer_reference_align=4) buffer OutputBuffer { uint values[]; };
layout(push_constant, std430) uniform Arguments { OutputBuffer output_buffer; uint count; } args;
void main() {
 uint index = gl_GlobalInvocationID.x;
 if (index < args.count) args.output_buffer.values[index] = index * 3u + 7u;
}
'''
CASES = [('air_kernels', 'k_vadd', 64, 1, 1, 5),
         ('air_kernels', 'k_collatz', 64, 1, 1, 5),
         ('air_kernels', 'k_reduce', 256, 1, 1, 5),
         ('air_kernels', 'k_hist', 64, 1, 1, 5),
         ('air_kernels', 'k_simd', 64, 1, 1, 5),
         ('gfx', 'v_pull', 1, 1, 1, 0),
         ('gfx', 'f_flat', 1, 1, 1, 4)]


def run(arguments):
    result = subprocess.run([str(value) for value in arguments], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30)
    print(result.stdout, end='')
    if result.returncode:
        raise RuntimeError(f'compiler exited {result.returncode}: {arguments[0]}')


def validate(path, stage):
    data = path.read_bytes()
    if len(data) < 64:
        raise ValueError(f'truncated NAK header: {path}')
    header = struct.unpack_from('<16I', data)
    if (header[0] != 0x314b414e or not header[1] or header[1] % 16 or
            header[9] != stage or len(data) != 64 + 4 * header[10] + header[1]):
        raise ValueError(f'invalid NAK code/header/stage: {path}')
    return dict(code_bytes=header[1], registers=header[2], stage=stage,
                local_size=list(header[5:8]), sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nakc', type=Path, required=True)
    parser.add_argument('--lib', type=Path, required=True)
    parser.add_argument('--glslang', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('native macOS compiler binaries required')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipt = dict(scope='offline compilation; pre-existing AIR fixtures are not newly Apple-compiled',
                   compiler_target='AD103 sm_89', gpu_execution_tested=False,
                   passed=False, results=[])
    try:
        for name, path in [('nakc', args.nakc), ('libnakc', args.lib)]:
            receipt[name + '_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        source, spv, binary = [out / ('affine.' + suffix) for suffix in ('comp', 'spv', 'nak')]
        source.write_text(AFFINE, encoding='utf-8')
        run([args.glslang, '-V', '--target-env', 'vulkan1.2', '-S', 'comp', source, '-o', spv])
        run([args.nakc, spv, binary])
        affine = validate(binary, 5)
        if affine['local_size'] != [64, 1, 1]:
            raise ValueError('affine local size changed')
        library = ctypes.CDLL(str(args.lib.resolve()))
        entry = library.nakc_main
        entry.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
        entry.restype = ctypes.c_int
        in_process = out / 'affine-in-process.nak'
        values = [b'nakc', str(spv).encode(), str(in_process).encode()]
        argv = (ctypes.c_char_p * (len(values) + 1))(*values, None)
        if entry(len(values), argv) != 0 or in_process.read_bytes() != binary.read_bytes():
            raise ValueError('CLI/in-process compiler results differ')
        receipt['results'].append(dict(input='affine.comp', cli_in_process_identical=True, **affine))
        fixtures = ROOT / 'tools/nvaccel/air'
        receipt['fixture_sha256'] = {name: hashlib.sha256((fixtures / (name + '.metallib')).read_bytes()).hexdigest()
                                    for name in ('air_kernels', 'gfx')}
        for name, function, bx, by, bz, stage in CASES:
            target = out / (function + '.nak')
            run([args.nakc, '--air', fixtures / (name + '.metallib'), function, target, bx, by, bz])
            receipt['results'].append(dict(input=function, **validate(target, stage)))
        receipt['passed'] = True
    except Exception as error:
        receipt['error'] = str(error)
        raise
    finally:
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('Offline compiler checks passed; GPU execution was not tested.')


if __name__ == '__main__':
    main()
