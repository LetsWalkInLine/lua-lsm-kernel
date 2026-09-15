#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Deterministic M5 corpus v1; emit data and assertions, never run a VM."""
import argparse
import hashlib
import json
import re
from pathlib import Path

VERSION = 1
BASE = '94d655819af226ae8bb96418bfd1eece827d778c'
WORK = 'string work limit exceeded'
DEPTH = 'pattern recursion limit exceeded'
CASES = []


def quote(value):
    # Three-digit decimal escapes preserve bytes (including NUL) in Lua 5.1.
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92)
                         else '\\%03d' % b for b in value.encode('utf-8')) + '"'


def string_expr(value):
    """Keep Lua sources small; all input construction precedes the operation."""
    parts = []
    start = 0
    for match in re.finditer(r"(.)\1{63,}", value, re.S):
        if match.start() > start:
            parts.append(quote(value[start:match.start()]))
        parts.append("string.rep(" + quote(match[1]) + "," + str(len(match[0])) + ")")
        start = match.end()
    if start < len(value) or not parts:
        parts.append(quote(value[start:]))
    return "..".join(parts)


def literal(value):
    if value is None:
        return 'nil'
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, str):
        return string_expr(value)
    return str(value)


def add(key, group, source, subject, pattern, expression, expected,
        coverage, *, setup='', error=None, host=True, note=''):
    CASES.append(dict(id=key, group=group, provenance=source,
                      assumption=note or 'Synthetic fixed bytes; no observed frequency.',
                      subject=subject, pattern=pattern,
                      subject_bytes=len(subject.encode('utf-8')),
                      pattern_bytes=len(pattern.encode('utf-8')),
                      setup='local s=' + string_expr(subject) + '; local p=' + string_expr(pattern) + '\n' + setup,
                      operation='return ' + expression, expected=expected,
                      kernel_error=error, host_safe=host, coverage=coverage))


DOC = 'security/lua/docs/API.md: module-format file_open example at source_base'
SYN = 'synthetic policy idiom; not a deployed policy'
SRC = 'lib/lua/tests/lua_work_kunit.c at source_base; finite derivative'
for key, subject, result in [('P01', '/etc/shadow', '/etc/shadow'),
                              ('P02', '/etc/shadow.bak', None),
                              ('P03', '/var/log/auth.log', None)]:
    add(key, 'path', DOC, subject, '^/etc/shadow$', 'string.match(s,p)',
        [result], ['anchor', 'literal', 'match-output'],
        note='Pattern copied from repository documentation; path is a synthetic hit/near-miss/miss.')
add('P04', 'path', SYN, '/srv/private/key.pem', '^/srv/private/',
    'string.find(s,p)', [1, 13], ['find-pattern', 'prefix'])
add('P05', 'path', SYN, '/var/cache/app/state.db', '/cache/',
    'string.find(s,p,1,true)', [5, 11], ['plain-explicit', 'compare'])
add('P06', 'path', SYN, '/var/cache/app/state.db', '/cache/',
    'string.find(s,p)', [5, 11], ['auto-classify', 'plain'])
add('F01', 'filename', SYN, 'audit-20260914.log', '%.([%w_]+)$',
    'string.match(s,p)', ['log'], ['search-start', 'capture', 'class'])
add('F02', 'filename', SYN, 'report.txt.exe', '%.txt$',
    'string.match(s,p)', [None], ['suffix-miss', 'escaped-dot'])
add('F03', 'filename', SYN, '.profile', '^%.([%w_]+)$',
    'string.match(s,p)', ['profile'], ['class', 'capture'])
add('F04', 'filename', SYN, '报告.txt', '%.txt$',
    'string.match(s,p)', ['.txt'], ['utf8-bytes', 'suffix'],
    note='UTF-8 byte string, no Unicode classification or character-count claim.')
add('X01', 'xattr', SYN, 'user.lua_lsm.label', '^user%.([%w_.]+)$',
    'string.match(s,p)', ['lua_lsm.label'], ['xattr-name', 'class', 'capture'])
add('X02', 'xattr', SYN, 'role=reader;level=3', '^role=(%a+);level=(%d+)$',
    'string.match(s,p)', ['reader', '3'], ['xattr-value', 'multi-capture'])
add('X03', 'xattr', SYN, 'role=reader;level=x', '^role=(%a+);level=(%d+)$',
    'string.match(s,p)', [None], ['xattr-value', 'validation-miss'])
add('X04', 'xattr', SYN, 'allow\0deny', '\0deny', 'string.find(s,p,1,true)',
    [6, 10], ['binary', 'plain-compare'], note='Synthetic binary value; no filesystem xattr I/O.')
add('S01', 'policy', DOC, '/etc/shadow', '^/etc/shadow$',
    'not (s and string.match(s,p))', [False], ['policy-decision', 'anchor'],
    note='Extracted boolean decision; nil guard and errno return are outside string timing.')
add('S02', 'policy', SYN, '/srv//private///key', '/+', 'string.gsub(s,p,"/")',
    ['/srv/private/key', 3], ['gsub', 'greedy', 'replacement'],
    note='String rewrite only; not a safe filesystem path canonicalizer.')
add('S03', 'policy', SYN, '${role}:${zone}', '%${(%a+)}', 'string.gsub(s,p,r)',
    ['reader:local', 2], ['gsub-table', 'capture-output'],
    setup='local r={role="reader",zone="local"}')
add('S04', 'policy', SYN, 'uid=12,gid=34', '%d+', 'string.gsub(s,p,r)',
    ['uid=[12],gid=[34]', 2], ['gsub-callback', 'nested-match'],
    setup='local r=function(x) return "["..string.match(x,"^%d+$").."]" end')
add('S05', 'policy', SYN, '/srv/private/key', '[^/]+', 'it(),it(),it(),it()',
    ['srv', 'private', 'key'], ['iterator-per-call', 'terminal-zero-results'],
    setup='local it=string.gmatch(s,p)',
    note='Four iterator calls, final call returns zero values; aggregate is not one budget.')
add('S06', 'policy', SYN, 'role=reader', '%f[%a]reader%f[%A]', 'string.match(s,p)',
    ['reader'], ['frontier', 'bracket-class'])
for n in (255, 256, 4095):
    s='/'+'a'*(n-5)+'.txt'
    add('BPATH'+str(n), 'normal-boundary', 'security/lua/auxlib.c: aux_dentry_path buffers',
        s, '%.txt$', 'string.match(s,p)', ['.txt'], ['long-path', 'search-start'],
        note='Synthetic Lua bytes of stated size; no assertion that this component layout is a valid VFS path.')
for n in (127, 128):
    add('BX'+str(n), 'normal-boundary', 'security/lua/lua_fs.c: fs_xattr buffer[128]',
        'r'*n, '^r+$', 'string.match(s,p)', ['r'*n], ['xattr-value', 'greedy', 'output'],
        note='Synthetic returned bytes at current buffer boundary; excludes __vfs_getxattr.')
add('BEMPTY', 'normal-boundary', SRC, '', '', 'string.find(s,p,1,true)',
    [1, 0], ['plain-empty'])
add('BNUL', 'normal-boundary', SRC, 'a\0[b', '\0[', 'string.find(s,p)',
    [2, 3], ['classification-stops-at-NUL', 'plain-binary'])
add('BZERO', 'normal-boundary', SRC, 'ab', '()', 'it(),it(),it(),it()',
    [1, 2, 3], ['iterator-zero-width', 'terminal-zero-results'], setup='local it=string.gmatch(s,p)')
add('BCAP32', 'normal-boundary', SRC, 'a'*32, '(a)'*32, 'string.match(s,p)',
    ['a']*32, ['capture32', 'depth65'], host=False,
    note='QEMU-only stack boundary already covered by M4; do not run in host kernel.')
for n in (65534, 65535, 65536):
    add('WPLAIN'+str(n), 'work-boundary', SRC, 'a'*n, 'z', 'string.find(s,p,1,true)',
        [None], ['plain-scan', 'W=n+1'], error=WORK if n==65536 else None, host=False,
        note='Synthetic bulk string, not xattr/path; unrestricted demand W=n+1; W<=65536 succeeds, W=65537 rejects under the current limit.')
for n in (65535, 65536):
    add('WTAIL'+str(n), 'work-boundary', SRC, 'a'*n, 'z', 'string.gsub(s,p,"x",0)',
        ['a'*n, 0], ['gsub-tail', 'output-precharge', 'W=n+1'],
        error=WORK if n==65536 else None, host=False)
for n in (8, 14):
    add('AOPT'+str(n), 'pathological', SRC, 'a'*n, '^'+'a?'*n+'b',
        'string.match(s,p)', [None], ['optional', 'retry', 'anchored'],
        error=WORK if n==14 else None, host=n==8)
for tag, p in [('GREEDY','^a*a*a*b'), ('MINIMAL','^a-a-a-b')]:
    add('A'+tag, 'pathological', SRC, 'a'*16, p, 'string.match(s,p)',
        [None], ['backtrack', tag.lower()])
add('ACLASS', 'pathological', SRC, 'x'*16, '['+'a'*64+']z', 'string.match(s,p)',
    [None], ['class-scan', 'unanchored-start'])
add('ABALANCE', 'pathological', SRC, '('*17+')'*17, '%b()', 'string.match(s,p)',
    ['('*17+')'*17], ['balance-scan', 'output'])
add('ACOMPARE', 'pathological', SRC, 'a'*32, '^(a+)%1b', 'string.match(s,p)',
    [None], ['capture-memcmp', 'greedy-retry'])
add('AOUTPUT', 'pathological', SRC, 'aaaa', 'a', 'string.gsub(s,p,"%0%0")',
    ['a'*8, 4], ['output-amplification', 'replacement-scan'])
add('ADEPTH66', 'pathological', SRC, 'a'*65, '^'+'a?'*65, 'string.match(s,p)',
    ['a'*65], ['depth-reject'], error=DEPTH, host=False)
add('AMALFORMED', 'error', SRC, 'x', '[', 'string.match(s,p)', [],
    ['original-error', 'classend'], error='malformed pattern', host=False)


def chunk(case, kernel):
    """Return a verifier closure; operation and setup remain separate in manifest."""
    error = case['kernel_error'] if kernel else None
    lines = [case['setup'], 'local operation=function() ' + case['operation'] + ' end',
             'return function()',
             'local function pack(...) return {n=select("#",...),...} end',
             'local result=pack(pcall(operation))']
    if error:
        lines += ['assert(result[1]==false)',
                  'assert(type(result[2])=="string" and string.find(result[2],'+quote(error)+',1,true))']
    else:
        expected=case['expected']
        lines += ['assert(result[1]==true)', 'assert(result.n=='+str(len(expected)+1)+')']
        for i, v in enumerate(expected, 2):
            lines.append('assert(result['+str(i)+']=='+literal(v)+')')
    lines += ['assert(string.match("ab","^a?b$")=="ab")', 'return true', 'end']
    return '\n'.join(lines)+'\n'


def generate(out):
    out.mkdir(parents=True, exist_ok=False)
    assert len({c['id'] for c in CASES}) == len(CASES)
    manifest=dict(schema_version=VERSION, source_base=BASE, samples=CASES)
    (out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=True,indent=2)+'\n')
    host=[]
    for c in CASES:
        name=c['id']+'.lua'
        (out/name).write_text(chunk(c, True))
        if c['host_safe']:
            # Self-contained host closure; no load-time numeric for statements.
            host.append('assert((function()\n'+chunk(c,False)+'end)()())\nprint('+quote('HOST PASS '+c['id'])+')')
    (out/'host.lua').write_text('\n'.join(host)+'\n')
    inventory=['# Lua-LSM string corpus v1', '',
               '| ID | Group | Subject bytes | Pattern bytes | Kernel outcome |',
               '| --- | --- | ---: | ---: | --- |']
    for c in CASES:
        inventory.append('| '+c['id']+' | '+c['group']+' | '+str(c['subject_bytes'])+' | '+
                         str(c['pattern_bytes'])+' | '+(c['kernel_error'] or 'exact return tuple')+' |')
    (out/'inventory.md').write_text('\n'.join(inventory)+'\n')
    hashes=[]
    for p in sorted(out.iterdir()):
        hashes.append(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name)
    (out/'SHA256SUMS').write_text('\n'.join(hashes)+'\n')
    print(f'corpus v{VERSION}: {len(CASES)} samples, {sum(c["host_safe"] for c in CASES)} host-safe; {out}')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True,
                        help='new output directory (refuses to overwrite historical data)')
    generate(parser.parse_args().output)
