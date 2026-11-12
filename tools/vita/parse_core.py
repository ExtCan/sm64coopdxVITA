#!/usr/bin/env python3
"""Decodes a PS Vita crash dump (ux0:data/psp2core-*.psp2dmp) of the game.

Prints every thread that was stopped by an exception with its registers, resolves code
addresses against the unstripped ELF and lists the return addresses found on its stack
(a poor man's backtrace: every word on the stack that points into the game's code).

usage: parse_core.py <psp2core.psp2dmp> <build/us_vita/sm64coopdx.elf> [addr2line command]

Only needs the Python standard library plus arm-vita-eabi-addr2line from VitaSDK.
The layout of the notes was taken from https://github.com/xyzz/vita-parse-core
"""

import gzip
import struct
import subprocess
import sys

STOP_REASONS = {
    0x00000: 'none',
    0x10002: 'undefined instruction',
    0x20003: 'prefetch abort',
    0x30004: 'data abort',
    0x60080: 'division by zero',
}


def u16(data, off):
    return struct.unpack_from('<H', data, off)[0]


def u32(data, off):
    return struct.unpack_from('<I', data, off)[0]


def c_str(data, off):
    end = data.index(b'\0', off)
    return data[off:end].decode('ascii', 'replace')


class Core:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        if raw[:2] == b'\x1f\x8b':
            raw = gzip.decompress(raw)
        self.raw = raw
        self.notes = {}
        self.segments = []

        phoff = u32(raw, 0x1C)
        phentsize = u16(raw, 0x2A)
        phnum = u16(raw, 0x2C)
        for i in range(phnum):
            ph = phoff + i * phentsize
            p_type, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from('<IIIIII', raw, ph)
            if p_type == 4:    # PT_NOTE
                pos = p_offset
                end = p_offset + p_filesz
                while pos + 12 <= end:
                    namesz, descsz, _ = struct.unpack_from('<III', raw, pos)
                    pos += 12
                    name = raw[pos:pos + namesz].rstrip(b'\0').decode('ascii', 'replace')
                    pos += (namesz + 3) & ~3
                    self.notes[name] = raw[pos:pos + descsz]
                    pos += (descsz + 3) & ~3
            elif p_type == 1:  # PT_LOAD
                self.segments.append((p_vaddr, raw[p_offset:p_offset + p_filesz]))

        self.modules = []
        data = self.notes.get('MODULE_INFO', b'')
        if data:
            off = 8
            for _ in range(u32(data, 4)):
                name = c_str(data, off + 0x24)
                num_segs = u32(data, off + 0x4C)
                off += 0x50
                segs = []
                for s in range(num_segs):
                    segs.append((u32(data, off + 8), u32(data, off + 12)))
                    off += 0x14
                off += 0x10
                self.modules.append((name, segs))

        self.threads = {}
        data = self.notes.get('THREAD_INFO', b'')
        if data:
            off = 8
            for _ in range(u32(data, 4)):
                sz = u32(data, off)
                t = data[off:off + sz]
                self.threads[u32(t, 4)] = {
                    'uid': u32(t, 4), 'name': c_str(t, 8), 'status': u16(t, 0x30),
                    'stop_reason': u32(t, 0x74), 'pc': u32(t, 0x9C), 'regs': None,
                }
                off += sz

        data = self.notes.get('THREAD_REG_INFO', b'')
        if data:
            off = 8
            for _ in range(u32(data, 4)):
                sz = u32(data, off)
                r = data[off:off + sz]
                tid = u32(r, 4)
                if tid in self.threads:
                    self.threads[tid]['regs'] = [u32(r, 8 + 4 * x) for x in range(16)]
                off += sz

    def locate(self, vaddr):
        for name, segs in self.modules:
            for num, (start, size) in enumerate(segs, 1):
                if start <= vaddr < start + size:
                    return name, num, vaddr - start
        return None

    def read(self, vaddr, size):
        for start, data in self.segments:
            if start <= vaddr < start + len(data):
                return data[vaddr - start:vaddr - start + size]
        return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    core = Core(sys.argv[1])
    elf_path = sys.argv[2]
    addr2line = sys.argv[3] if len(sys.argv) > 3 else 'arm-vita-eabi-addr2line'

    # the game's own module is the one named after the eboot; its code segment is linked at 0x81000000
    game = None
    for name, segs in core.modules:
        if 'eboot' in name.lower() or 'sm64' in name.lower():
            game = (name, segs)
    if game is None and core.modules:
        game = core.modules[0]

    def resolve(vaddr):
        loc = core.locate(vaddr & ~1)
        if not loc:
            return '0x%08x' % vaddr
        name, seg, offset = loc
        text = '0x%08x (%s@%d + 0x%x' % (vaddr, name, seg, offset)
        if game and name == game[0] and seg == 1:
            try:
                out = subprocess.check_output([addr2line, '-f', '-i', '-C', '-e', elf_path, '0x%x' % (0x81000000 + offset)])
                lines = [l.strip() for l in out.decode('utf-8', 'replace').splitlines() if l.strip()]
                pairs = ['%s %s' % (lines[i], lines[i + 1].replace('\\', '/').split('/sm64coopdx/')[-1]) for i in range(0, len(lines) - 1, 2)]
                text += ' => ' + ' <- '.join(pairs)
            except Exception as e:  # addr2line missing, etc.
                text += ' => ?? (%s)' % e
        return text + ')'

    print('modules:')
    for name, segs in core.modules:
        print('  %-28s %s' % (name, ', '.join('0x%08x+0x%x' % s for s in segs)))

    crashed = [t for t in core.threads.values() if t['stop_reason'] != 0]
    print()
    print('%d threads, %d stopped by an exception' % (len(core.threads), len(crashed)))
    for t in core.threads.values():
        marker = '*' if t['stop_reason'] else ' '
        print(' %s %-32s uid 0x%08x status 0x%04x pc %s' % (marker, t['name'], t['uid'], t['status'], '0x%08x' % t['pc']))

    for t in crashed:
        print()
        print('=== %s: %s (0x%x) ===' % (t['name'], STOP_REASONS.get(t['stop_reason'], 'unknown'), t['stop_reason']))
        print('PC: ' + resolve(t['pc']))
        regs = t['regs']
        if not regs:
            continue
        for i in range(0, 16, 4):
            print('  ' + '  '.join('%-3s 0x%08x' % (('r%d' % (i + j)) if i + j < 13 else ['sp', 'lr', 'pc'][i + j - 13], regs[i + j]) for j in range(4)))
        print('LR: ' + resolve(regs[14]))

        print('stack (words that point into code):')
        sp = regs[13]
        data = core.read(sp, 0x1800) or b''
        shown = 0
        for off in range(0, len(data) - 3, 4):
            value = u32(data, off)
            loc = core.locate(value & ~1)
            if loc and loc[1] == 1 and (value & 1):
                print('  sp+0x%04x: %s' % (off, resolve(value)))
                shown += 1
                if shown >= 40:
                    break

    return 0


if __name__ == '__main__':
    sys.exit(main())
