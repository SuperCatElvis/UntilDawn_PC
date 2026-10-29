#!/usr/bin/env python3
"""
ps4_xref.py - helper library + CLI for a decrypted PS4 ELF:
  * applies R_RELATIVE / symbol relocations to a virtual image (read-only model)
  * finds PLT stubs (jmp qword [rip+X] -> JMP_SLOT GOT entry) and maps them to imports
  * counts direct call sites per import (E8 rel32 -> stub) across the RX segment
  * disassembles VA ranges with GNU objdump and annotates import calls

Usage:
  python ps4_xref.py <elf> --nids aerolib.inl --stubs stubs.csv --callcounts calls.csv
  python ps4_xref.py <elf> --nids aerolib.inl --disasm 0xa0 0x200
"""
import sys, os, struct, subprocess, argparse, tempfile, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps4_inspect import PS4Elf, load_nid_db


class Image:
    def __init__(self, path, nids):
        self.d = open(path, 'rb').read()
        self.e = PS4Elf(self.d)
        self.e.parse(nids)
        self.rel = {ro: (si, t, a) for ro, si, t, a in self.e.rela + self.e.jmprel}
        self.text = next(p for p in self.e.ph if p['type'] == 'PT_LOAD' and 'X' in p['flags'])
        self.symname = lambda si: self.e.syms[si].get('name') or self.e.syms[si]['raw']

    def q(self, va):
        if va in self.rel:
            si, t, a = self.rel[va]
            if t == 8:
                return a
            return ('SYM', self.symname(si), a)
        o = self.e.va2off(va)
        return None if o is None else struct.unpack_from('<Q', self.d, o)[0]

    def stubs(self):
        """Return {stub_va: import_name} for PLT-style 'jmp [rip+disp32]' to JUMP_SLOT/GLOB_DAT slots."""
        got = {ro: self.symname(si) for ro, si, t, a in self.e.jmprel}
        got.update({ro: self.symname(si) for ro, si, t, a in self.e.rela if t in (6, 1) and si and self.e.syms[si]['shndx'] == 0})
        t = self.text
        blob = self.d[t['p_offset']:t['p_offset'] + t['p_filesz']]
        out = {}
        for m in re.finditer(b'\xff\x25', blob):
            i = m.start()
            disp = struct.unpack_from('<i', blob, i + 2)[0]
            va = t['p_vaddr'] + i
            tgt = va + 6 + disp
            if tgt in got:
                out[va] = got[tgt]
        self.got = got
        return out

    def call_counts(self, stubs):
        t = self.text
        blob = self.d[t['p_offset']:t['p_offset'] + t['p_filesz']]
        cnt = collections.Counter()
        sites = collections.defaultdict(list)
        for m in re.finditer(b'[\xe8\xe9]', blob):  # call rel32 and tail-call jmp rel32
            i = m.start()
            if i + 5 > len(blob):
                break
            tgt = t['p_vaddr'] + i + 5 + struct.unpack_from('<i', blob, i + 1)[0]
            if tgt in stubs:
                cnt[stubs[tgt]] += 1
                if len(sites[stubs[tgt]]) < 8:
                    sites[stubs[tgt]].append(t['p_vaddr'] + i)
        # also indirect calls through GOT: ff 15 disp32
        for m in re.finditer(b'\xff\x15', blob):
            i = m.start()
            tgt = t['p_vaddr'] + i + 6 + struct.unpack_from('<i', blob, i + 2)[0]
            if tgt in self.got:
                cnt[self.got[tgt]] += 1
                if len(sites[self.got[tgt]]) < 8:
                    sites[self.got[tgt]].append(t['p_vaddr'] + i)
        return cnt, sites

    def disasm(self, start, end, stubs=None):
        t = self.text
        o = t['p_offset'] + (start - t['p_vaddr'])
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(self.d[o:o + (end - start)])
        out = subprocess.run(['objdump', '-D', '-b', 'binary', '-mi386:x86-64', '-M', 'intel',
                              '--adjust-vma=%#x' % start, f.name], capture_output=True, text=True).stdout
        os.unlink(f.name)
        lines = []
        for ln in out.splitlines()[7:]:
            m = re.search(r'(call|jmp)\s+0x([0-9a-f]+)', ln)
            if m and stubs and int(m.group(2), 16) in stubs:
                ln += '    ; -> ' + stubs[int(m.group(2), 16)]
            lines.append(ln)
        return '\n'.join(lines)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('elf'); ap.add_argument('--nids')
    ap.add_argument('--stubs'); ap.add_argument('--callcounts')
    ap.add_argument('--disasm', nargs=2)
    a = ap.parse_args()
    img = Image(a.elf, load_nid_db(a.nids) if a.nids else None)
    st = img.stubs()
    if a.stubs:
        with open(a.stubs, 'w') as f:
            f.write('stub_va,import\n')
            for va, n in sorted(st.items()):
                f.write('%#x,%s\n' % (va, n))
    if a.callcounts:
        cnt, sites = img.call_counts(st)
        imp = {v['sym'].get('name') or k: v['sym'].get('lib') for k, v in img.e.imports.items()}
        with open(a.callcounts, 'w') as f:
            f.write('import,library,direct_call_or_tailjmp_sites,example_sites\n')
            for n in sorted(imp, key=lambda n: (-cnt.get(n, 0), n)):
                f.write('%s,%s,%d,%s\n' % (n, imp[n], cnt.get(n, 0), ' '.join('%#x' % s for s in sites.get(n, []))))
    if a.disasm:
        print(img.disasm(int(a.disasm[0], 0), int(a.disasm[1], 0), st))


def rip_refs(img, target_va, limit=50):
    """Find instructions in the RX segment whose RIP-relative disp32 resolves to target_va.
    Heuristic byte scan (lea/mov/cmp with ModRM mod=00 rm=101), verified for the common encodings."""
    t = img.text
    blob = img.d[t['p_offset']:t['p_offset'] + t['p_filesz']]
    hits = []
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', blob):
        i = m.start()
        disp = struct.unpack_from('<i', blob, i + 3)[0]
        if t['p_vaddr'] + i + 7 + disp == target_va:
            hits.append(t['p_vaddr'] + i)
            if len(hits) >= limit:
                break
    return hits


def find_string(img, s):
    t = img.text
    out = []
    i = img.d.find(s.encode() + b'\0')
    while i != -1:
        for p in img.e.ph:
            if p['type'] == 'PT_LOAD' and p['p_offset'] <= i < p['p_offset'] + p['p_filesz']:
                out.append(p['p_vaddr'] + i - p['p_offset'])
        i = img.d.find(s.encode() + b'\0', i + 1)
    return out


def summarize(img, start, end, stubs):
    """List calls (with import names) and referenced C strings in [start,end)."""
    from ps4_inspect import cstr
    out = []
    for ln in img.disasm(start, end, stubs).splitlines():
        m = re.search(r'#\s*0x([0-9a-f]+)', ln)
        s = ''
        if m:
            o = img.e.va2off(int(m.group(1), 16))
            if o is not None:
                c = cstr(img.d, o)
                if 4 <= len(c) < 200 and all(32 <= ord(ch) < 127 or ch in '\n\t' for ch in c):
                    s = '  "%s"' % c[:80].replace('\n', '\\n')
        if '\tcall' in ln or ('; ->' in ln) or s:
            out.append(ln.split('\t')[0] + ' ' + ln.split('\t')[-1].strip()[:90] + s)
    return out
