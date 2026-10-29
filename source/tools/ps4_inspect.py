#!/usr/bin/env python3
"""
ps4_inspect.py - static inspector for PS4 SELF containers and decrypted PS4 ELF
(ET_SCE_EXEC / ET_SCE_DYNEXEC / ET_SCE_DYNAMIC) files.

No third-party dependencies. Read-only. Produces JSON + human-readable text.

Usage:
    python ps4_inspect.py <file> [--json out.json] [--nids nid_db.txt] [--imports out.csv]

What it reports
  SELF container : header, segment table, embedded plaintext ELF header and
                   program headers, SCE extended info (program auth id,
                   program type, app/firmware version, digest).
  Decrypted ELF  : ELF header, program headers (incl. SCE-specific types),
                   section headers if present, PT_DYNAMIC tags (incl. DT_SCE_*),
                   needed modules/libraries, import/export libraries,
                   symbol table (NID-encoded), relocation statistics,
                   PT_SCE_PROCPARAM contents, PT_SCE_COMMENT/VERSION strings.

The NID database (optional) is a text file with lines "NID name" or
"name NID" or shadPS4-style aerolib lines {"NID", "name"}; NIDs are also
computed locally for any name list given with --names so a database can be
built/verified without trusting third-party tables.
"""
import struct, sys, json, hashlib, base64, re, os, argparse, collections

SELF_MAGIC = b'\x4f\x15\x3d\x1d'
ELF_MAGIC = b'\x7fELF'

ET_NAMES = {0: 'ET_NONE', 1: 'ET_REL', 2: 'ET_EXEC', 3: 'ET_DYN',
            0xFE00: 'ET_SCE_EXEC', 0xFE0C: 'ET_SCE_STUBLIB',
            0xFE10: 'ET_SCE_DYNEXEC', 0xFE18: 'ET_SCE_DYNAMIC'}

PT_NAMES = {0: 'PT_NULL', 1: 'PT_LOAD', 2: 'PT_DYNAMIC', 3: 'PT_INTERP', 4: 'PT_NOTE',
            6: 'PT_PHDR', 7: 'PT_TLS',
            0x6474E550: 'PT_GNU_EH_FRAME', 0x6474E551: 'PT_GNU_STACK',
            0x61000000: 'PT_SCE_DYNLIBDATA', 0x61000001: 'PT_SCE_PROCPARAM',
            0x61000002: 'PT_SCE_MODULE_PARAM', 0x61000010: 'PT_SCE_RELRO',
            0x6FFFFF00: 'PT_SCE_COMMENT', 0x6FFFFF01: 'PT_SCE_LIBVERSION'}

DT_NAMES = {0: 'DT_NULL', 1: 'DT_NEEDED', 2: 'DT_PLTRELSZ', 3: 'DT_PLTGOT', 4: 'DT_HASH',
            5: 'DT_STRTAB', 6: 'DT_SYMTAB', 7: 'DT_RELA', 8: 'DT_RELASZ', 9: 'DT_RELAENT',
            10: 'DT_STRSZ', 11: 'DT_SYMENT', 12: 'DT_INIT', 13: 'DT_FINI', 14: 'DT_SONAME',
            16: 'DT_SYMBOLIC', 20: 'DT_PLTREL', 21: 'DT_DEBUG', 22: 'DT_TEXTREL', 23: 'DT_JMPREL',
            24: 'DT_BIND_NOW', 25: 'DT_INIT_ARRAY', 26: 'DT_FINI_ARRAY', 27: 'DT_INIT_ARRAYSZ',
            28: 'DT_FINI_ARRAYSZ', 30: 'DT_FLAGS', 32: 'DT_PREINIT_ARRAY', 33: 'DT_PREINIT_ARRAYSZ',
            0x6FFFFFFB: 'DT_FLAGS_1', 0x6FFFFFF9: 'DT_RELACOUNT',
            0x61000005: 'DT_SCE_IDTABENTSZ', 0x61000007: 'DT_SCE_FINGERPRINT',
            0x61000009: 'DT_SCE_ORIGINAL_FILENAME', 0x6100000D: 'DT_SCE_MODULE_INFO',
            0x6100000F: 'DT_SCE_NEEDED_MODULE', 0x61000011: 'DT_SCE_MODULE_ATTR',
            0x61000013: 'DT_SCE_EXPORT_LIB', 0x61000015: 'DT_SCE_IMPORT_LIB',
            0x61000017: 'DT_SCE_EXPORT_LIB_ATTR', 0x61000019: 'DT_SCE_IMPORT_LIB_ATTR',
            0x6100001B: 'DT_SCE_STUB_MODULE_NAME', 0x6100001D: 'DT_SCE_STUB_MODULE_VERSION',
            0x6100001F: 'DT_SCE_STUB_LIBRARY_NAME', 0x61000021: 'DT_SCE_STUB_LIBRARY_VERSION',
            0x61000025: 'DT_SCE_HASH', 0x61000027: 'DT_SCE_PLTGOT', 0x61000029: 'DT_SCE_JMPREL',
            0x6100002B: 'DT_SCE_PLTREL', 0x6100002D: 'DT_SCE_PLTRELSZ', 0x6100002F: 'DT_SCE_RELA',
            0x61000031: 'DT_SCE_RELASZ', 0x61000033: 'DT_SCE_RELAENT', 0x61000035: 'DT_SCE_STRTAB',
            0x61000037: 'DT_SCE_STRSZ', 0x61000039: 'DT_SCE_SYMTAB', 0x6100003B: 'DT_SCE_SYMENT',
            0x6100003D: 'DT_SCE_HASHSZ', 0x6100003F: 'DT_SCE_SYMTABSZ'}

R_X86_64 = {0: 'R_NONE', 1: 'R_64', 2: 'R_PC32', 6: 'R_GLOB_DAT', 7: 'R_JUMP_SLOT',
            8: 'R_RELATIVE', 16: 'R_DTPMOD64', 17: 'R_DTPOFF64', 18: 'R_TPOFF64'}

# Custom base64 alphabet used for NIDs and the #lib#mod suffixes
NID_ALPHA = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
NID_SUFFIX = bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')


def nid_of(name: str) -> str:
    d = hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:8][::-1]  # first 8 bytes read as LE u64, emitted big-endian
    return base64.b64encode(d, altchars=b'+-').decode()[:11]


def b64idx(s: str) -> int:
    v = 0
    for c in s:
        v = v * 64 + NID_ALPHA.index(c)
    return v


def cstr(b, off):
    e = b.find(b'\0', off)
    return b[off:e].decode('utf-8', 'replace')


# ----------------------------------------------------------------- SELF ---
def parse_self(d):
    r = {}
    (magic, version, mode, endian, attribs, key_type, header_size, meta_size,
     file_size, num_segments, flags) = struct.unpack_from('<4sBBBBIHHQHH', d, 0)
    r['header'] = dict(version=version, mode=mode, endian=endian, attribs=attribs,
                       key_type=hex(key_type), header_size=hex(header_size),
                       meta_size=hex(meta_size), file_size=file_size,
                       num_segments=num_segments, flags=hex(flags))
    segs = []
    off = 0x20
    for i in range(num_segments):
        sflags, soff, csize, dsize = struct.unpack_from('<QQQQ', d, off)
        off += 32
        segs.append(dict(index=i, flags=hex(sflags),
                         ordered=bool(sflags & 1), encrypted=bool(sflags & 2),
                         signed=bool(sflags & 4), compressed=bool(sflags & 8),
                         blocked=bool(sflags & 0x800),
                         is_meta=bool(sflags & 0xF0000),
                         phdr_id=(sflags >> 20) & 0xFFF,
                         file_offset=hex(soff), comp_size=csize, decomp_size=dsize))
    r['segments'] = segs
    elf_off = off
    r['embedded_elf_offset'] = hex(elf_off)
    if d[elf_off:elf_off + 4] == ELF_MAGIC:
        eh = parse_ehdr(d, elf_off)
        r['embedded_elf_header'] = eh
        ph = parse_phdrs(d, elf_off + eh['e_phoff'], eh['e_phnum'])
        r['embedded_program_headers'] = ph
        # extended info follows phdrs, aligned to 16
        ext = elf_off + eh['e_phoff'] + eh['e_phnum'] * 56
        ext = (ext + 15) & ~15
        paid, ptype, app_ver, fw_ver = struct.unpack_from('<QQQQ', d, ext)
        digest = d[ext + 32:ext + 64]
        r['extended_info'] = dict(offset=hex(ext), program_authority_id=hex(paid),
                                  program_type=hex(ptype),
                                  app_version=hex(app_ver), fw_version=hex(fw_ver),
                                  digest_sha256=digest.hex())
    return r


# ------------------------------------------------------------------ ELF ---
def parse_ehdr(d, base=0):
    f = struct.unpack_from('<16sHHIQQQIHHHHHH', d, base)
    ident = f[0]
    return dict(ei_class=ident[4], ei_data=ident[5], ei_osabi=ident[7],
                e_type=f[1], e_type_name=ET_NAMES.get(f[1], hex(f[1])),
                e_machine=f[2], e_version=f[3], e_entry=f[4], e_phoff=f[5],
                e_shoff=f[6], e_flags=f[7], e_ehsize=f[8], e_phentsize=f[9],
                e_phnum=f[10], e_shentsize=f[11], e_shnum=f[12], e_shstrndx=f[13])


def parse_phdrs(d, off, n):
    out = []
    for i in range(n):
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from('<IIQQQQQQ', d, off + i * 56)
        out.append(dict(index=i, p_type=p_type, type=PT_NAMES.get(p_type, hex(p_type)),
                        flags=''.join(c if p_flags & m else '-' for c, m in (('R', 4), ('W', 2), ('X', 1))),
                        p_flags=p_flags, p_offset=p_offset, p_vaddr=p_vaddr,
                        p_filesz=p_filesz, p_memsz=p_memsz, p_align=p_align))
    return out


class PS4Elf:
    def __init__(self, data):
        self.d = data
        self.eh = parse_ehdr(data)
        self.ph = parse_phdrs(data, self.eh['e_phoff'], self.eh['e_phnum'])
        self.byt = {p['type']: p for p in self.ph}
        self.dyn = []
        self.dynd = collections.defaultdict(list)
        self.result = {}

    def va2off(self, va):
        for p in self.ph:
            if p['type'] in ('PT_LOAD', 'PT_SCE_RELRO') and p['p_vaddr'] <= va < p['p_vaddr'] + p['p_filesz']:
                return p['p_offset'] + (va - p['p_vaddr'])
        return None

    def parse(self, nids=None):
        r = self.result
        r['elf_header'] = self.eh
        r['program_headers'] = self.ph
        r['has_section_headers'] = self.eh['e_shnum'] > 0 and self.eh['e_shoff'] != 0
        # dynamic
        pd = self.byt.get('PT_DYNAMIC')
        if pd:
            off = pd['p_offset']
            for i in range(pd['p_filesz'] // 16):
                tag, val = struct.unpack_from('<qQ', self.d, off + i * 16)
                self.dyn.append((tag, val))
                self.dynd[tag].append(val)
                if tag == 0:
                    break
        dl = self.byt.get('PT_SCE_DYNLIBDATA')
        dlo = dl['p_offset'] if dl else 0
        self.dlo = dlo

        def one(tag):
            v = self.dynd.get(tag)
            return v[0] if v else None
        strtab = one(0x61000035)
        strsz = one(0x61000037)
        self.strtab = (dlo + strtab) if strtab is not None else None

        def s(o):
            return cstr(self.d, self.strtab + o) if self.strtab is not None else None
        tags = []
        for tag, val in self.dyn:
            tags.append(dict(tag=DT_NAMES.get(tag, hex(tag)), value=hex(val)))
        r['dynamic'] = tags
        r['dt_init'] = one(12)
        r['dt_fini'] = one(13)
        r['init_array'] = (one(25), one(27))
        r['fini_array'] = (one(26), one(28))
        r['preinit_array'] = (one(32), one(33))
        r['fingerprint'] = None
        fp = one(0x61000007)
        if fp is not None and dl:
            r['fingerprint'] = self.d[dlo + fp:dlo + fp + 20].hex()
        of = one(0x61000009)
        r['original_filename'] = s(of) if of is not None else None
        r['needed'] = [s(v) for v in self.dynd.get(1, [])]
        mods = {}
        for v in self.dynd.get(0x6100000F, []):
            mods[v >> 48] = dict(id=v >> 48, name=s(v & 0xFFFFFFFF),
                                 version='%d.%d' % ((v >> 40) & 0xFF, (v >> 32) & 0xFF))
        mi = one(0x6100000D)
        r['module_info'] = dict(name=s(mi & 0xFFFFFFFF), id=mi >> 48,
                                version='%d.%d' % ((mi >> 40) & 0xFF, (mi >> 32) & 0xFF)) if mi is not None else None
        r['needed_modules'] = sorted(mods.values(), key=lambda x: x['id'])
        libs = {}
        for v in self.dynd.get(0x61000015, []):
            libs[v >> 48] = dict(id=v >> 48, name=s(v & 0xFFFFFFFF), version=(v >> 32) & 0xFFFF)
        for v in self.dynd.get(0x61000019, []):
            if (v >> 48) in libs:
                libs[v >> 48]['attr'] = hex(v & 0xFFFF)
        r['import_libraries'] = sorted(libs.values(), key=lambda x: x['id'])
        elibs = []
        for v in self.dynd.get(0x61000013, []):
            elibs.append(dict(id=v >> 48, name=s(v & 0xFFFFFFFF), version=(v >> 32) & 0xFFFF))
        r['export_libraries'] = elibs
        self.mods, self.libs = mods, libs

        # symbols
        symtab = one(0x61000039); symsz = one(0x6100003F); syment = one(0x6100003B) or 24
        syms = []
        if symtab is not None and symsz:
            base = dlo + symtab
            for i in range(symsz // syment):
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from('<IBBHQQ', self.d, base + i * syment)
                name = s(st_name)
                sym = dict(index=i, raw=name, bind=st_info >> 4, type=st_info & 0xF,
                           shndx=st_shndx, value=st_value, size=st_size)
                m = re.fullmatch(r'([A-Za-z0-9+\-]{11})#([A-Za-z0-9+\-]+)#([A-Za-z0-9+\-]+)', name or '')
                if m:
                    sym['nid'] = m.group(1)
                    lid, mid = b64idx(m.group(2)), b64idx(m.group(3))
                    sym['lib'] = libs.get(lid, {}).get('name') or next((e['name'] for e in elibs if e['id'] == lid), str(lid))
                    sym['module'] = mods.get(mid, {}).get('name') or (r['module_info'] or {}).get('name') if mid not in mods else mods[mid]['name']
                    if nids:
                        sym['name'] = nids.get(m.group(1))
                syms.append(sym)
        self.syms = syms
        r['symbol_count'] = len(syms)

        # relocations
        def rel_list(off_tag, sz_tag):
            o, n = one(off_tag), one(sz_tag)
            out = []
            if o is None or not n:
                return out
            base = dlo + o
            for i in range(n // 24):
                ro, info, add = struct.unpack_from('<QQq', self.d, base + i * 24)
                out.append((ro, info >> 32, info & 0xFFFFFFFF, add))
            return out
        self.rela = rel_list(0x6100002F, 0x61000031)
        self.jmprel = rel_list(0x61000029, 0x6100002D)
        r['rela_count'] = len(self.rela)
        r['jmprel_count'] = len(self.jmprel)
        r['rela_types'] = dict(collections.Counter(R_X86_64.get(t, str(t)) for _, _, t, _ in self.rela))
        r['jmprel_types'] = dict(collections.Counter(R_X86_64.get(t, str(t)) for _, _, t, _ in self.jmprel))
        r['pltgot'] = one(0x61000027)

        # imports = symbols referenced by relocations with shndx==0
        imports = {}
        for (ro, si, t, add) in self.rela + self.jmprel:
            if si == 0 or si >= len(syms):
                continue
            sy = syms[si]
            if sy['shndx'] != 0:
                continue
            k = sy['raw']
            e = imports.setdefault(k, dict(sym=sy, slots=[], types=set()))
            e['slots'].append(ro)
            e['types'].add(R_X86_64.get(t, str(t)))
        self.imports = imports
        per_lib = collections.Counter(v['sym'].get('lib') for v in imports.values())
        r['import_count'] = len(imports)
        r['imports_per_library'] = dict(per_lib.most_common())
        if nids:
            r['imports_resolved_by_nid_db'] = sum(1 for v in imports.values() if v['sym'].get('name'))
        exports = [sy for sy in syms if sy['shndx'] != 0 and sy.get('nid')]
        r['export_count'] = len(exports)
        self.exports = exports

        # procparam
        pp = self.byt.get('PT_SCE_PROCPARAM')
        if pp:
            o = pp['p_offset']
            size, magic, ver, sdk = struct.unpack_from('<QIII', self.d, o)
            ptrs = [struct.unpack_from('<Q', self.d, o + 0x18 + 8 * i)[0] for i in range((size - 0x18) // 8)]
            r['procparam'] = dict(vaddr=hex(pp['p_vaddr']), size=hex(size), magic=hex(magic),
                                  magic_ascii=struct.pack('<I', magic).decode('latin1'),
                                  version=hex(ver), sdk_version=hex(sdk),
                                  pointers=[hex(x) for x in ptrs])
            names = ['sceProcessName', 'sceUserMainThreadName', 'sceUserMainThreadPriority',
                     'sceUserMainThreadStackSize', 'sceLibcParam', 'sceKernelMemParam',
                     'sceKernelFsParam', 'sceProcessPrimaryCpu']
            deref = {}
            for nm, p in zip(names, ptrs):
                if not p:
                    deref[nm] = None; continue
                fo = self.va2off(p)
                if fo is None:
                    deref[nm] = 'unmapped ' + hex(p); continue
                if nm in ('sceProcessName', 'sceUserMainThreadName'):
                    deref[nm] = cstr(self.d, fo)
                elif nm in ('sceUserMainThreadPriority', 'sceProcessPrimaryCpu'):
                    deref[nm] = struct.unpack_from('<I', self.d, fo)[0]
                elif nm == 'sceUserMainThreadStackSize':
                    deref[nm] = hex(struct.unpack_from('<I', self.d, fo)[0])
                else:
                    deref[nm] = dict(va=hex(p), first_qwords=[hex(x) for x in struct.unpack_from('<6Q', self.d, fo)])
            r['procparam_fields'] = deref
            # libc param -> heap size etc.
            lp = ptrs[4] if len(ptrs) > 4 else 0
            if lp and self.va2off(lp) is not None:
                fo = self.va2off(lp)
                q = struct.unpack_from('<Q', self.d, fo)[0]
                fields = struct.unpack_from('<12Q', self.d, fo)
                r['sceLibcParam_raw'] = [hex(x) for x in fields]
                # derefs of pointers inside libc param (heap size, flags)
                der = {}
                for i, x in enumerate(fields[2:], 2):
                    fo2 = self.va2off(x) if x else None
                    if fo2 is not None:
                        der['field%d@%s' % (i, hex(x))] = hex(struct.unpack_from('<Q', self.d, fo2)[0])
                r['sceLibcParam_deref'] = der
            mp = ptrs[5] if len(ptrs) > 5 else 0
            if mp and self.va2off(mp) is not None:
                fo = self.va2off(mp)
                fields = struct.unpack_from('<6Q', self.d, fo)
                der = {}
                for i, x in enumerate(fields[1:], 1):
                    fo2 = self.va2off(x) if x else None
                    if fo2 is not None:
                        der['field%d@%s' % (i, hex(x))] = hex(struct.unpack_from('<Q', self.d, fo2)[0])
                r['sceKernelMemParam_raw'] = [hex(x) for x in fields]
                r['sceKernelMemParam_deref'] = der

        for t in ('PT_SCE_COMMENT', 'PT_SCE_LIBVERSION'):
            p = self.byt.get(t)
            if p:
                blob = self.d[p['p_offset']:p['p_offset'] + p['p_filesz']]
                r[t] = dict(size=len(blob), hex_head=blob[:96].hex(),
                            strings=re.findall(rb'[\x20-\x7e]{4,}', blob)[:40])
                r[t]['strings'] = [x.decode() for x in r[t]['strings']]
        return r


def load_nid_db(path):
    db = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'"([A-Za-z0-9+\-]{11})"\s*,\s*"?([^"\s)]+)"?\s*[)}]', line)
            if m:
                db[m.group(1)] = m.group(2); continue
            parts = line.split()
            if len(parts) >= 2:
                if re.fullmatch(r'[A-Za-z0-9+\-]{11}', parts[0]):
                    db[parts[0]] = parts[1]
                elif re.fullmatch(r'[A-Za-z0-9+\-]{11}', parts[-1]):
                    db[parts[-1]] = parts[0]
    return db


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--json')
    ap.add_argument('--nids', help='NID database text file')
    ap.add_argument('--names', help='file with one symbol name per line; NIDs computed locally and merged')
    ap.add_argument('--imports', help='write imports CSV')
    ap.add_argument('--exports', help='write exports CSV')
    a = ap.parse_args()
    d = open(a.file, 'rb').read()
    out = dict(file=os.path.basename(a.file), size=len(d), sha256=hashlib.sha256(d).hexdigest())
    nids = load_nid_db(a.nids) if a.nids else {}
    if a.names:
        for ln in open(a.names, encoding='utf-8', errors='replace'):
            ln = ln.strip()
            if ln:
                nids.setdefault(nid_of(ln), ln)
    if d[:4] == SELF_MAGIC:
        out['container'] = 'SELF (signed/encrypted PS4 executable)'
        out['self'] = parse_self(d)
    elif d[:4] == ELF_MAGIC:
        out['container'] = 'ELF (decrypted)'
        e = PS4Elf(d)
        out['elf'] = e.parse(nids or None)
        if a.imports:
            with open(a.imports, 'w') as f:
                f.write('nid,name,library,module,bind,type,slot_count,first_slot,reloc_types\n')
                for k, v in sorted(e.imports.items(), key=lambda kv: (kv[1]['sym'].get('lib') or '', kv[0])):
                    sy = v['sym']
                    f.write('%s,%s,%s,%s,%d,%d,%d,%s,%s\n' % (
                        sy.get('nid', k), sy.get('name') or '', sy.get('lib'), sy.get('module'),
                        sy['bind'], sy['type'], len(v['slots']), hex(min(v['slots'])),
                        '|'.join(sorted(v['types']))))
        if a.exports:
            with open(a.exports, 'w') as f:
                f.write('nid,name,library,value,size,type\n')
                for sy in e.exports:
                    f.write('%s,%s,%s,%s,%d,%d\n' % (sy['nid'], sy.get('name') or '', sy.get('lib'),
                                                    hex(sy['value']), sy['size'], sy['type']))
    else:
        out['container'] = 'unknown'
    js = json.dumps(out, indent=1, default=lambda o: sorted(o) if isinstance(o, set) else str(o))
    if a.json:
        open(a.json, 'w').write(js)
    else:
        print(js)


if __name__ == '__main__':
    main()
