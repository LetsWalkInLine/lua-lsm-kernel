#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate finite x86_64, integer-number Lua 5.1 depth fixtures.

Use the host compiler to avoid the kernel parser's separate C-stack limit.
Only exact integer constants are accepted during double -> s64 conversion.
"""
import argparse
from pathlib import Path
import struct
import subprocess


def compile_chunk(source):
    result = subprocess.run(['lua5.1', '-e',
                             'io.write(string.dump(assert(loadstring(io.read("*a")))))' ],
                            input=source.encode(), stdout=subprocess.PIPE, check=True)
    blob = bytearray(result.stdout)
    if blob[:12] != b'\x1bLua\x51\0\x01\x04\x08\x04\x08\0':
        raise ValueError('requires little endian x86_64 Lua 5.1 with 64-bit doubles')
    blob[11] = 1
    pos = 12

    def integer():
        nonlocal pos
        value = struct.unpack_from('<I', blob, pos)[0]
        pos += 4
        return value

    def string():
        nonlocal pos
        size = struct.unpack_from('<Q', blob, pos)[0]
        pos += 8 + size

    def proto():
        nonlocal pos
        string()
        pos += 12  # line bounds, nups, params, vararg, maxstack
        count = integer()
        pos += 4 * count
        for _ in range(integer()):
            tag = blob[pos]
            pos += 1
            if tag == 1:
                pos += 1
            elif tag == 3:
                value = struct.unpack_from('<d', blob, pos)[0]
                if value != int(value):
                    raise ValueError('non-integer constant')
                struct.pack_into('<q', blob, pos, int(value))
                pos += 8
            elif tag == 4:
                string()
            elif tag != 0:
                raise ValueError('unexpected constant')
        children = [proto() for _ in range(integer())]
        count = integer()
        pos += 4 * count
        for _ in range(integer()):
            string()
            pos += 8
        for _ in range(integer()):
            string()
        return 1 + max(children, default=0)

    depth = proto()
    if pos != len(blob):
        raise ValueError('trailing bytecode')
    return bytes(blob), depth


def generate(root):
    blobs = {}
    for depth in (32, 33):
        chain = 'function() return ' * (depth-1)
        ends = ' end' * (depth-1)
        blob, actual = compile_chunk('local seed=41; return ' + chain + 'seed+1' + ends)
        assert actual == depth
        blobs[depth] = '"' + ''.join('\\%03d' % x for x in blob) + '"'
        source = ('local errno=require("errno");local unused=' + chain + '42' + ends +
                  '\nreturn {name="dump_reg%d",license="GPL",file_open=function(file) '
                  'if file:path()=="/dump32-reg-target" then return false,errno.EACCES end;'
                  'return true end}' % depth)
        blob, actual = compile_chunk(source)
        assert actual == depth
        (root / ('dump-register%d.luac' % depth)).write_bytes(blob)
    template = Path(__file__).with_name('fixtures') / 'dump-boundary.lua'
    (root / 'dump-boundary.lua').write_text(
        'local errno=require("errno")\nlocal blobs={\n' +
        ''.join('[%d]=%s,\n' % (d, blob) for d, blob in blobs.items()) +
        '}\n' + template.read_text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
