#!/usr/bin/env python3
"""bench_report.py - summarise a replay benchmark (bench\\run_bench.bat on the test PC).

  python3 bench_report.py RESULTS_DIR [--map-a A.map] [--map-b B.map] [--map-c C.map] [--from-repeat N]

RESULTS_DIR holds one folder per run, <capture>_<label> (labels A1 A2 [B1 B2 [C1 C2]] Aprof [Bprof] Agpu [Bgpu], cold-pipeline runs Acold / Bcold, trace runs Btrace), each with
the replay's results file (replay_<capture>_run1.txt) and its console output (console_try<n>.txt).

Prints, per capture: the median over the steady repeats (from --from-repeat on, default 10) of the frame's wall time,
the renderer's submit time and its parts (renderer CPU = the submit time without its waits for the GPU), and GPU busy
time, for each timing pass and per build; the difference
between the two passes of one build (the noise) and between the builds; whether the replayed images of the builds
differ; and, for the profile runs, the renderer's hottest functions (with the build's linker map) and other modules.
"""
import os, re, sys, statistics, collections

REP = re.compile(r'^\s*repeat (\d+): ([\d.]+) ms wall; game submit calls ([\d.]+) ms \(draw recording ([\d.]+): translation ([\d.]+), '
                 r'textures ([\d.]+), pipelines ([\d.]+), GPU waits ([\d.]+)\); GPU busy ([\d.]+) ms; draws (\d+), barriers (\d+), batches (\d+)')
REP2 = re.compile(r'also in the submit calls: dispatches ([\d.]+) ms, submit-start memory syncs ([\d.]+), vkQueueSubmit ([\d.]+), '
                  r'write-backs after batches ([\d.]+); shader setups reused (\d+)(?: \(relocated (\d+)\))?, translated in full (\d+)')
REP3 = re.compile(r'texture checks \(t107e\) (\d+): unchanged (\d+).*?; ms: checks ([\d.]+), tracking restarts ([\d.]+), hashing ([\d.]+), uploads ([\d.]+)')
IMG = re.compile(r'^\s*(0x[0-9a-f]+) (\d+x\d+x\d+) fmt (\d+)( depth)?: (identical, hash (0x[0-9a-f]+)|DIFFERS: .*replay hash (0x[0-9a-f]+), captured)')
KEYS = ['wall', 'submit', 'cpu', 'rec', 'tr', 'tex', 'pipe', 'gpuwait', 'gpu', 'draws', 'bar', 'batches', 'disp', 'memsync', 'qsub', 'wb',
        'reused', 'full', 'texchecks', 'tex_unch', 'tx_checks', 'tx_restart', 'tx_hash', 'tx_up']
SHOW = [('wall', 'frame'), ('submit', 'renderer'), ('cpu', 'renderer CPU'), ('tr', 'shader checks'), ('tex', 'textures'), ('memsync', 'memory syncs'),
        ('qsub', 'vkQueueSubmit'), ('gpuwait', 'GPU waits'), ('gpu', 'GPU busy')]


def parse_run(d, cap):
    f = os.path.join(d, 'replay_%s_run1.txt' % cap)
    reps, imgs = [], {}
    if not os.path.exists(f):
        return reps, imgs
    cur = None
    for line in open(f, errors='replace'):
        m = REP.match(line)
        if m:
            g = m.groups()
            cur = dict(zip(['k', 'wall', 'submit', 'rec', 'tr', 'tex', 'pipe', 'gpuwait', 'gpu', 'draws', 'bar', 'batches'],
                           [int(g[0])] + [float(x) for x in g[1:9]] + [int(x) for x in g[9:12]]))
            cur['cpu'] = cur['submit'] - cur['gpuwait']   # the renderer's own work: the submit time without its waits for the GPU
            reps.append(cur)
            continue
        m = REP2.search(line)
        if m and cur is not None:
            g = m.groups()
            cur.update(disp=float(g[0]), memsync=float(g[1]), qsub=float(g[2]), wb=float(g[3]), reused=int(g[4]), full=int(g[6]))
            if g[5] is not None:
                cur['reloc'] = int(g[5])
            continue
        m = REP3.search(line)
        if m and cur is not None:
            g = m.groups()
            cur.update(texchecks=int(g[0]), tex_unch=int(g[1]), tx_checks=float(g[2]), tx_restart=float(g[3]), tx_hash=float(g[4]), tx_up=float(g[5]))
            continue
        m = IMG.match(line)
        if m and cur is None:   # the image list comes before the repeats
            key = (m.group(1), m.group(2), m.group(3), bool(m.group(4)))
            n = 0
            while key + (n,) in imgs:   # the same address can appear more than once (reused memory)
                n += 1
            imgs[key + (n,)] = m.group(6) or m.group(7)
    return reps, imgs


def console(d):
    tries = sorted([x for x in os.listdir(d) if x.startswith('console_try')]) if os.path.isdir(d) else []
    return open(os.path.join(d, tries[-1]), errors='replace').read() if tries else ''


def med(reps, key, lo):
    v = [r[key] for r in reps if r['k'] >= lo and key in r]
    return statistics.median(v) if v else None


def load_map(path):
    syms = []
    if not path:
        return syms
    for line in open(path, errors='replace'):
        m = re.match(r'\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s', line)
        if m:
            syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
    syms.sort()
    return syms


def sym_of(syms, rva):
    lo, hi = 0, len(syms) - 1
    if not syms or rva < syms[0][0]:
        return '?'
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if syms[mid][0] <= rva:
            lo = mid
        else:
            hi = mid - 1
    return syms[lo][1]


def profile(text, syms):
    out = []
    i = text.find('=== where the replay thread spends its time')
    if i < 0:
        return ['  (no profile section)']
    sect = text[i:]
    m = re.search(r'\((?:the repeats after the first, )?(\d+) samples\)', sect)
    out.append('  samples: %s' % (m.group(1) if m else '?'))
    m = re.search(r'other modules ([\d.]+)%:([^\n]*)', sect)
    if m:
        out.append('  other modules %s%%:%s' % (m.group(1), m.group(2)))
    m = re.search(r'our code ([\d.]+)%, hottest 16-byte blocks \(exe\+offset\):([^\n]*)', sect)
    if m:
        total = float(m.group(1))
        blocks = re.findall(r'\+(?:0x)?([0-9a-fA-F]+):(\d+)', m.group(2))
        nsum = sum(int(n) for _, n in blocks)
        by = collections.Counter()
        for off, n in blocks:
            by[sym_of(syms, int(off, 16))] += int(n)
        out.append('  our code %.1f%% of the samples; by function (share of our code\'s listed samples):' % total)
        for name, n in by.most_common(25):
            out.append('    %5.1f%%  %s' % (100.0 * n / nsum if nsum else 0, name))
    m = re.search(r'other modules by our calling site:([^\n]*)', sect)
    if m and syms:
        calls = re.findall(r'(\S+?)<\+(?:0x)?([0-9a-fA-F]+) ([\d.]+)%', m.group(1))
        by = collections.Counter()
        for mod, off, pct in calls:
            by[(mod, sym_of(syms, int(off, 16)))] += float(pct)
        out.append('  calls into other modules, by our calling function (% of all samples):')
        for (mod, name), p in by.most_common(15):
            out.append('    %5.1f%%  %s -> %s' % (p, name, mod))
    return out


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    root = a[0]
    opt = {'--map-a': None, '--map-b': None, '--from-repeat': '10'}
    for i, x in enumerate(a):
        if x in opt and i + 1 < len(a):
            opt[x] = a[i + 1]
    lo = int(opt['--from-repeat'])
    opt.setdefault('--map-c', None)
    for i, x in enumerate(a):
        if x == '--map-c' and i + 1 < len(a):
            opt['--map-c'] = a[i + 1]
    maps = {'A': load_map(opt['--map-a']), 'B': load_map(opt['--map-b'] or opt['--map-a']), 'C': load_map(opt['--map-c'] or opt['--map-b'] or opt['--map-a'])}
    runs = collections.defaultdict(dict)
    for name in sorted(os.listdir(root)):
        m = re.match(r'^(\d+)_([A-Z]\w*)$', name)
        if m and os.path.isdir(os.path.join(root, name)):
            runs[m.group(1)][m.group(2)] = os.path.join(root, name)
    print('Replay benchmark %s (medians of repeats %d and later, ms per frame)\n' % (root, lo))
    total = collections.defaultdict(list)
    for cap in sorted(runs, key=int):
        labels = runs[cap]
        print('== capture %s' % cap)
        print('  %-6s %6s ' % ('pass', 'draws') + ' '.join('%13s' % t for _, t in SHOW))
        per = {}
        imgs = {}
        for lab in sorted(l for l in labels if re.match(r'^[A-C]\d$', l)):
            reps, im = parse_run(labels[lab], cap)
            imgs[lab] = im
            if not reps:
                print('  %-6s (no repeats - see %s)' % (lab, labels[lab]))
                continue
            per[lab] = {k: med(reps, k, lo) for k in KEYS}
            print('  %-6s %6d ' % (lab, per[lab]['draws'] or 0) + ' '.join('%13.1f' % (per[lab][k] or 0) for k, _ in SHOW))
        for b in 'ABC':
            ps = [per[l] for l in per if l[0] == b]
            if len(ps) >= 2:
                print('  %-6s %6s ' % (b + ' noise', '') + ' '.join('%13.1f' % abs((ps[0][k] or 0) - (ps[1][k] or 0)) for k, _ in SHOW))
            if ps:
                avg = {k: statistics.mean([p[k] for p in ps if p[k] is not None]) for k, _ in SHOW}
                total[b].append(avg)
                print('  %-6s %6s ' % (b + ' mean', '') + ' '.join('%13.1f' % avg[k] for k, _ in SHOW))
        for x, y in (('B', 'A'), ('C', 'B'), ('C', 'A')):
            if any(l[0] == x for l in per) and any(l[0] == y for l in per):
                d = {k: total[x][-1][k] - total[y][-1][k] for k, _ in SHOW}
                print('  %-6s %6s ' % (x + ' - ' + y, '') + ' '.join('%+13.1f' % d[k] for k, _ in SHOW))
        # images: builds compared through their replay hashes (both passes of A against both of B)
        la = [l for l in imgs if l[0] == 'A' and imgs[l]]
        lb = [l for l in imgs if l[0] == 'B' and imgs[l]]
        if len(la) >= 2:
            diff = [k for k in imgs[la[0]] if imgs[la[1]].get(k) != imgs[la[0]][k]]
            print('  images A vs A (run-to-run variation): %d of %d differ %s' % (len(diff), len(imgs[la[0]]), ' '.join(x[0] for x in diff[:8])))
        if la:
            # run-to-run variation: images that differ between passes of one build (any build)
            var = set()
            for b in 'ABC':
                lx = [l for l in imgs if l[0] == b and imgs[l]]
                var |= set(k for k in imgs[la[0]] if len(set(imgs[l].get(k) for l in lx)) > 1)
            for b in 'BC':
                lx = [l for l in imgs if l[0] == b and imgs[l]]
                if not lx:
                    continue
                diff = [k for k in imgs[la[0]] if any(imgs[l].get(k) != imgs[la[0]][k] for l in lx)]
                vara = set(k for k in imgs[la[0]] if any(imgs[l].get(k) != imgs[la[0]][k] for l in la))
                newa = [k for k in diff if k not in vara]
                new = [k for k in diff if k not in var]
                same = len(lx) >= 2 and all(imgs[lx[0]].get(k) == imgs[l].get(k) for l in lx for k in newa)
                print('  images %s vs A: %d differ; outside the A-vs-A variation %d%s; outside the run-to-run variation of every build %d %s' % (
                    b, len(diff), len(newa), (' (the same in every %s pass: a real difference)' % b if same and newa else ''), len(new), ' '.join(x[0] for x in (new or newa)[:8])))
        for lab in ('Aprof', 'Bprof', 'Cprof'):
            if lab in labels:
                print('  -- profile %s' % lab)
                for line in profile(console(labels[lab]), maps[lab[0]]):
                    print(line)
        for lab in ('Acold', 'Bcold', 'Bcold2', 'Ccold', 'Ccold2', 'Dcold', 'Ecold', 'Hcold', 'Ncold'):   # t154: every pipeline compiled by the driver (--cold-pipelines); t158: second runs, --gs-hoist (H), --gs-noinline (N)
            if lab in labels:
                c = console(labels[lab])
                reps, _ = parse_run(labels[lab], cap)
                r1 = reps[0]['wall'] if reps else None
                print('  -- cold pipelines %s: first repeat %s ms' % (lab, '%.0f' % r1 if r1 else '?'))
                for pat in (r'parallel pipeline compiles: [^\n]*', r'pipeline compiles of 250 ms and more[^\n]*', r'pipelines not compiled again \(t154[^\n]*',
                            r'shaders translated under more than one key \(t154\)[^\n]*', r'full translations not kept for reuse \(t154\)[^\n]*'):
                    m = re.search(pat, c)
                    if m:
                        print('     ' + m.group(0).strip())
                slow = re.findall(r'slow pipeline compile: (\d+) ms, ([^\n]*)', c)
                for ms, rest in sorted(slow, key=lambda x: -int(x[0]))[:12]:
                    print('       %6s ms  %s' % (ms, rest.strip()))
        for lab in ('Btrace', 'Atrace'):
            if lab in labels:
                c = console(labels[lab])
                m = re.search(r'translation trace: (\d+) setups, (\d+) MB written', c)
                print('  -- translation trace %s: %s' % (lab, ('%s setups, %s MB' % m.groups()) if m else 'not found in the console'))
        for lab in ('Agpu', 'Bgpu', 'Cgpu'):
            if lab in labels:
                c = console(labels[lab])
                m = re.search(r'GPU time measured:[^\n]*\n((?:      [^\n]*\n){0,8})', c)
                reps, _ = parse_run(labels[lab], cap)
                g = med(reps, 'gpu', 5) if reps else None
                print('  -- GPU timestamps %s: GPU busy %s ms (median of repeats 5+)' % (lab, '%.1f' % g if g else '?'))
                if m:
                    print('     ' + c[m.start():m.end()].strip().replace('\n', '\n     '))
        print()
    for b in 'ABC':
        if total[b]:
            s = {k: sum(t[k] for t in total[b]) for k, _ in SHOW}
            print('all captures, build %s (sum of the per-capture means): ' % b + ', '.join('%s %.1f' % (t, s[k]) for k, t in SHOW))
    return 0


if __name__ == '__main__':
    sys.exit(main())
