#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare measurement-only source files in an isolated kernel source copy."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('source anchor changed: ' + old[:100])
    return source.replace(old, new, 1)


def cstr(value):
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92)
                         else '\\%03o' % b for b in value.encode()) + '"'


def unlimited_scan(source):
    return replace_once(source,
        '  return work->budget.remaining < len ? work->budget.remaining : len;',
        '  return len; /* Finite measurement input; no scan allowance. */')


def prepare(args):
    dest = args.source.resolve()
    live = Path(__file__).resolve().parents[4]
    if dest == live:
        raise ValueError('refusing to instrument the shared source tree')
    target = dest / 'lib/lua/tests/benchmark'
    target.mkdir(exist_ok=True)
    for name in ('bench.c', 'observe.h', 'observe.inc', 'prepare.py', 'README.md'):
        shutil.copyfile(Path(__file__).with_name(name), target / name)
    current = (dest / 'lib/lua/lstrlib.c').read_text()
    patched = current.replace('#include "lstrwork.h"', '#include "../../lstrwork.h"')
    start = patched.index('static void strwork_charge (')
    end = patched.index('\nstatic void strwork_begin (', start)
    a = patched[:start] + '''static void strwork_charge (StrWork *work, enum StrWorkKind kind, u64 cost) {
  (void)work; (void)kind; (void)cost;
}
''' + patched[end:]
    start = a.index('static void strwork_begin (')
    end = a.index('\n#define strwork_bind', start)
    a = a[:start] + '''static void strwork_begin (lua_State *L, StrWork *work, enum StrWorkAPI api) {
  (void)L; (void)work; (void)api;
}
''' + a[end:]
    a = unlimited_scan(a)
    # Keep the current helper/control structure, with debit removed at compile time.
    (target / 'variant_a.c').write_text('#define luaopen_string luaopen_string_bench_a\n' + a)
    for mode in ('b', 'd'):
        source = patched
        if mode == 'b':
            source = replace_once(source, 'bool allowed = strwork_debit(&work->budget, cost);',
                                  'bool allowed = true; /* Count full finite demand; no debit. */')
            source = unlimited_scan(source)
        source = replace_once(source, 'static const char *match (MatchState *ms, const char *s, const char *p) {',
            '''static void bench_depth(StrWork *work, unsigned int depth, bool admitted);
static const char *match (MatchState *ms, const char *s, const char *p) {
  bench_depth(ms->work, LUA_PATTERN_MAXDEPTH - ms->matchdepth + 1, false);''')
        source = replace_once(source, '  ms->matchdepth--;',
            '  ms->matchdepth--;\n  bench_depth(ms->work, LUA_PATTERN_MAXDEPTH - ms->matchdepth, true);')
        prefix = ('#define LUA_STRING_WORK_TEST\n#include "observe.h"\n#include "../../lstate.h"\n'
                  '#define luaopen_string luaopen_string_bench_' + mode + '\n'
                  '#define BENCH_OBSERVE bench_observe_' + mode + '\n')
        (target / ('variant_' + mode + '.c')).write_text(prefix + source + '\n#include "observe.inc"\n')
    historical = args.historical.read_text()
    if 'LUA_PATTERN_MAXDEPTH 65' not in historical or 'strwork' in historical:
        raise ValueError('historical input must be post-M2, pre-M3 lstrlib.c')
    # Apply the same inherited cursor commit fix, so H does not compare old semantics.
    historical = replace_once(historical, '      lua_Integer newstart = e-s;', '      int n;\n      lua_Integer newstart = e-s;')
    historical = replace_once(historical,
        '''      lua_pushinteger(L, newstart);
      lua_replace(L, lua_upvalueindex(3));
      return push_captures(&ms, src, e);''',
        '''      n = push_captures(&ms, src, e);
      luaL_checkstack(L, 1, "updating gmatch cursor");
      lua_pushinteger(L, newstart);
      lua_replace(L, lua_upvalueindex(3));
      return n;''')
    (target / 'variant_h.c').write_text('#define luaopen_string luaopen_string_bench_h\n' + historical)
    manifest = json.loads(args.manifest.read_text())
    cases = manifest['samples'] + [dict(id='EMPTY', setup='', operation='return', expected=[], kernel_error=None)]
    lines = ['/* Generated from the versioned corpus; do not edit. */']
    entries = []
    for i, c in enumerate(cases):
        if c['id'] in ('S05', 'BZERO'):
            reset = 'function() it=string.gmatch(s,p) end'
        else:
            reset = 'nil'
        # A trailing newline keeps the last identifier/keyword clear of EOZ.
        chunk = c['setup'] + '\nreturn function() ' + c['operation'] + ' end, ' + reset + '\n'
        values = []
        for v in c['expected']:
            if v is None:
                values.append('{ .type = LUA_TNIL }')
            elif isinstance(v, bool):
                values.append('{ .type = LUA_TBOOLEAN, .number = ' + str(int(v)) + ' }')
            elif isinstance(v, str):
                values.append('{ .type = LUA_TSTRING, .string = ' + cstr(v) + ', .len = ' + str(len(v.encode())) + ' }')
            else:
                values.append('{ .type = LUA_TNUMBER, .number = ' + str(v) + ' }')
        if values:
            lines.append('static const struct bench_value values_' + str(i) + '[] = {\n' + ',\n'.join(values) + '\n};')
        error = c['kernel_error']
        unlimited = error if error and error != 'string work limit exceeded' else None
        entries.append('{ '+cstr(c['id'])+', '+cstr(chunk)+', '+
                       (cstr(error) if error else 'NULL')+', '+
                       (cstr(unlimited) if unlimited else 'NULL')+', '+
                       ('values_'+str(i) if values else 'NULL')+', '+str(len(values))+' }')
    lines.append('static const struct bench_sample samples[] = {\n'+',\n'.join(entries)+'\n};')
    (target / 'samples.h').write_text('\n'.join(lines)+'\n')
    (target / 'Makefile').write_text('obj-y += bench.o variant_a.o variant_b.o variant_d.o variant_h.o\n')
    makefile = dest/'lib/lua/tests/Makefile'
    if 'benchmark/' not in makefile.read_text():
        with makefile.open('a') as f:
            f.write('\n# Measurement-only source overlay.\nobj-y += benchmark/\n')
    meta = dict(generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                manifest_sha256=hashlib.sha256(args.manifest.read_bytes()).hexdigest(),
                historical_sha256=hashlib.sha256(args.historical.read_bytes()).hexdigest(),
                production_sha256=hashlib.sha256(current.encode()).hexdigest(),
                generated={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in sorted(target.iterdir()) if p.is_file() and p.name != 'generation.json'})
    (target/'generation.json').write_text(json.dumps(meta,indent=2)+'\n')
    print('Prepared isolated A/B/C/D/H measurement build:',target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--historical', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    prepare(parser.parse_args())
