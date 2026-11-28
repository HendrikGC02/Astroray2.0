# SPDX-License-Identifier: Apache-2.0
"""Tables from the landing history for a milestone retrospective.

Usage: <python> -I tools/land_history.py <history.jsonl> [--ledger tasks/README.md ...] [--after NNN] [--before NNN] [--self-test]
  --ledger is repeatable: an older README (`git show <sha>:tasks/README.md` saved under out/tmp) adds the
  declared gate seconds of rows that later landings deleted. --after / --before keep the lines after /
  before task NNN's line, for a phase's numbers either side of the landing that changed it.

<history.jsonl> is the file `land.ps1 submit` appends to after each merge (one JSON object per line:
task, sha, date, verdict PASS|SKIP, attempts, and phase seconds rebase_s ... perf_shares_s). It holds
merged landings only: `attempts` counts the prepare passes of one submit (a pass after the first means
the merge refused and asked for a fresh prepare); a submit refused before its merge writes no line, so
refusals and their causes come from the ledger (`--ledger`), whose entries say "second try", "third
try" and so on, and "+X measured ... against +Y declared" for gate seconds.
Prints at most 40 lines. Reads the paths it is given and nothing else; standard library only.
"""
import argparse
import datetime
import json
import re
import statistics
import sys

PHASES = ('rebase_s', 'scenes_s', 'canary_s', 'blender_suites_s', 'gate_s', 'ci_wait_s', 'merge_s',
          'lock_wait_s', 'perf_s', 'perf_shares_s')
NUM = r'\d+(?:\.\d+)?'  # a sentence's closing period is not part of the number
TRY_WORDS = {'second': 2, 'third': 3, 'fourth': 4, 'fifth': 5, 'sixth': 6, 'seventh': 7}


def load(lines):
    """Parse history lines; an empty history or a malformed line raises (a silent empty table would read as 'no landings')."""
    recs = []
    for i, raw in enumerate(lines, 1):
        if not raw.strip():
            continue
        d = json.loads(raw)
        for k in ('task', 'date', 'verdict', 'attempts'):
            if k not in d:
                raise ValueError(f'history line {i}: no "{k}"')
        if d['verdict'] not in ('PASS', 'SKIP'):
            raise ValueError(f'history line {i}: verdict {d["verdict"]!r} is not PASS or SKIP')
        if int(d['attempts']) < 1:
            raise ValueError(f'history line {i}: attempts {d["attempts"]} < 1')
        recs.append(d)
    if not recs:
        raise ValueError('history has no landing lines')
    return recs


def ledger_entries(text):
    """Yield (task, parenthetical text) for each 'NNN [x s] (...)' entry, with balanced parentheses."""
    for m in re.finditer(r'(?<![\d.+\-/])(\d{3})(?:\'s)? (?:[\d,.]+ (?:then [\d,.]+ )?s(?: total)? )?\(', text):
        depth, j = 1, m.end()
        while j < len(text) and depth:
            depth += {'(': 1, ')': -1}.get(text[j], 0)
            j += 1
        yield int(m.group(1)), text[m.end():j - 1]


def parse_ledger(text):
    tries, measured, declared, lanes = {}, {}, {}, {}
    for task, body in ledger_entries(text):
        for w in re.findall(r'\b(' + '|'.join(TRY_WORDS) + r') (?:try|submit)\b', body):
            tries[task] = max(tries.get(task, 1), TRY_WORDS[w])
        if re.search(r'\bfirst (?:try|submit)\b[^;]*?\b(?:refused|failed)|\bfirst (?:try:|refused)|\brefused once|\brelanded', body):
            tries[task] = max(tries.get(task, 1), 2)
        m = re.match(r'(?:[^;()]*; )?\+(' + NUM + r')(?: s)? measured', body)
        if m:
            measured[task] = float(m.group(1))
        m = re.search(r'against \+(' + NUM + r') declared', body)
        if m:
            declared[task] = float(m.group(1))
    # Batch rows: "NNN title lane model; depends; +X."
    for m in re.finditer(r'\b(\d{3}) [^;]*? (kernel|cpu|gpu|blender|tools|docs) ([SO])\b[^;]*; [^;]*; \+(' + NUM + ')', text):
        declared.setdefault(int(m.group(1)), float(m.group(4)))
        lanes.setdefault(int(m.group(1)), (m.group(2), m.group(3)))
    # Table rows of an older README (`git show <sha>:tasks/README.md`): "| NNN title | lane | S | deps | +X |".
    for m in re.finditer(r'^\| (\d{3}) [^|]*\| (\w+) \| ([SO]) \| [^|]*\| \+(' + NUM + ')', text, re.M):
        declared.setdefault(int(m.group(1)), float(m.group(4)))
        lanes.setdefault(int(m.group(1)), (m.group(2), m.group(3)))
    return tries, measured, declared, lanes


def fmt(x):
    return f'{x:.1f}'


def report(recs, ledger_text=None):
    out = []
    tasks = sorted({r['task'] for r in recs})
    landings = len(recs)
    attempts = sum(int(r['attempts']) for r in recs)
    rerun = sorted(r['task'] for r in recs if int(r['attempts']) > 1)
    out.append(f'landings {landings} (tasks {len(tasks)}, {tasks[0]}-{tasks[-1]}); prepare passes {attempts}; '
               f'tasks with a refused merge and a fresh prepare: {", ".join(map(str, rerun)) or "none"}')
    out.append(f'verdicts: PASS {sum(r["verdict"] == "PASS" for r in recs)}, SKIP {sum(r["verdict"] == "SKIP" for r in recs)}')
    out.append(f'first-try rate in history (attempts 1): {landings - len(rerun)}/{landings}')
    out.append('phase             n   median      max  (max task)    sum')
    for k in PHASES:
        v = [(float(r[k]), r['task']) for r in recs if k in r]
        if not v:
            out.append(f'{k:<15} {0:>3}        -        -')
            continue
        mx = max(v)
        out.append(f'{k:<15} {len(v):>3} {statistics.median(x for x, _ in v):>8.1f} {mx[0]:>8.1f}  ({mx[1]:>3}) '
                   f'{sum(x for x, _ in v):>10.1f}')
    tot = [sum(float(r.get(k, 0.0)) for k in PHASES) for r in recs]
    out.append(f'landing total (sum of phases): median {fmt(statistics.median(tot))} s, max {fmt(max(tot))} s, '
               f'all {fmt(sum(tot) / 3600)} h')
    by = {}
    for r in recs:
        d = datetime.datetime.fromisoformat(r['date'])
        by.setdefault(('week', '%d-W%02d' % d.isocalendar()[:2]), []).append(float(r.get('gate_s', 0.0)))
        by.setdefault(('day', d.date().isoformat()), []).append(float(r.get('gate_s', 0.0)))
    for (kind, key), v in sorted(by.items(), key=lambda kv: (kv[0][0] != 'week', kv[0][1])):
        out.append(f'gate_s {kind} {key}: n {len(v):>2}, median {statistics.median(v):6.1f}, max {max(v):6.1f}')
    if ledger_text is not None:
        tries, measured, declared, lanes = parse_ledger(ledger_text)
        retried = {t: n for t, n in tries.items() if n > 1}
        out.append(f'ledger: tasks landed after a refused submit {len(retried)}; refused submits {sum(n - 1 for n in retried.values())}')
        out.append('  ' + ', '.join(f'{t}:{n}' for t, n in sorted(retried.items())))
        lt = set(tasks)
        hit = len(lt - set(retried))
        out.append(f'first-try rate (history tasks, ledger tries): {hit}/{len(lt)}')
        per = {}
        for t in tasks:
            lane, model = lanes.get(t, ('no row', '-'))
            per.setdefault(lane, []).append(model)
        out.append('lane (model O) of history tasks: ' + ', '.join(
            f'{k} {len(v)} ({v.count("O")})' for k, v in sorted(per.items(), key=lambda kv: -len(kv[1]))))
        both = sorted(set(measured) & set(declared))
        if both:
            out.append('gate s declared -> measured: ' + ', '.join(f'{t} {declared[t]:g}->{measured[t]:g}' for t in both))
            out.append(f'  sum declared {sum(declared[t] for t in both):g} s, sum measured {sum(measured[t] for t in both):g} s, '
                       f'ratio {sum(measured[t] for t in both) / max(sum(declared[t] for t in both), 1e-9):.2f}')
    if len(out) > 40:
        raise RuntimeError(f'report has {len(out)} lines, cap 40')
    return out


CANNED = [
    '{"task":301,"sha":"a","date":"2026-01-05T10:00:00","verdict":"PASS","attempts":1,"rebase_s":1.0,"gate_s":100.0,"ci_wait_s":10.0}',
    # A merge refused once (main moved), landed by the second prepare pass; no perf_s key.
    '{"task":302,"sha":"b","date":"2026-01-06T10:00:00","verdict":"SKIP","attempts":2,"rebase_s":3.0,"gate_s":120.0,"ci_wait_s":30.0}',
    '{"task":303,"sha":"c","date":"2026-01-13T10:00:00","verdict":"PASS","attempts":1,"gate_s":140.0,"perf_s":5.0}',
]
CANNED_LEDGER = ('| 303 old row | tools | S | none | +4 |\n'
                 'Landed: 301 (+0.7 measured tools tests, against +0.5 declared; decision 38), 302 150.0 s '
                 '(second try; first refused: a rebase), 303 160.0 s (+3.5 measured; third try: B-08 (cold lint)), '
                 '305 (first try 99.0 s failed on a race).\n'
                 '304 a later task tools S; none; +0.3.')


def self_test():
    recs = load(CANNED)
    out = report(recs, CANNED_LEDGER)
    text = '\n'.join(out)
    checks = [
        ('landings counted per merged line', out[0].startswith('landings 3 (tasks 3, 301-303); prepare passes 4;')),
        ('refused merge named', out[0].endswith(': 302')),
        ('first-try rate from history', 'first-try rate in history (attempts 1): 2/3' in text),
        ('missing phase key skipped', re.search(r'^perf_s\s+1\s+5\.0\s+5\.0', text, re.M) is not None),
        ('absent phase prints n 0', re.search(r'^scenes_s\s+0\s', text, re.M) is not None),
        ('median of a phase', re.search(r'^rebase_s\s+2\s+2\.0\s+3\.0\s+\(302\)', text, re.M) is not None),
        ('gate by week', 'gate_s week 2026-W02: n  2, median  110.0, max  120.0' in text),
        ('ledger tries', 'refused submits 4' in text and '302:2, 303:3, 305:2' in text),
        ('first-try rate from ledger', 'first-try rate (history tasks, ledger tries): 1/3' in text),
        ('lanes from the row forms', 'lane (model O) of history tasks: no row 2 (0), tools 1 (0)' in text),
        ('declared vs measured', 'gate s declared -> measured: 301 0.5->0.7, 303 4->3.5' in text),
    ]
    for lines, why in (([], 'empty'), (['', '  '], 'blank'), (['{"task":1,"date":"2026-01-01T00:00:00","verdict":"REFUSE","attempts":1}'], 'bad verdict')):
        try:
            load(lines)
            checks.append((f'{why} history fails loudly', False))
        except ValueError:
            checks.append((f'{why} history fails loudly', True))
    bad = [n for n, ok in checks if not ok]
    for n, ok in checks:
        print(('ok   ' if ok else 'FAIL ') + n)
    if bad:
        print('\n'.join(out))
        print(f'FAIL land_history self-test: {len(bad)} of {len(checks)}')
        return 1
    print(f'PASS land_history self-test ({len(checks)} checks)')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('history', nargs='?')
    ap.add_argument('--ledger', action='append', default=[], help='repeatable: the current README and older ones')
    ap.add_argument('--after', type=int, help='only the lines after this task\'s line (file order)')
    ap.add_argument('--before', type=int, help='only the lines before this task\'s line (file order)')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.history:
        ap.error('a history file is required')
    with open(a.history, encoding='utf-8') as f:
        recs = load(f.readlines())
    for opt, task in (('after', a.after), ('before', a.before)):
        if task is None:
            continue
        idx = [i for i, r in enumerate(recs) if r['task'] == task]
        if not idx:
            raise SystemExit(f'--{opt} {task}: no history line for that task')
        recs = load(json.dumps(r) for r in (recs[idx[-1] + 1:] if opt == 'after' else recs[:idx[0]]))
    ledger = None
    if a.ledger:
        texts = []
        for p in a.ledger:
            with open(p, encoding='utf-8') as f:
                texts.append(f.read())
        ledger = '\n'.join(texts)
    print('\n'.join(report(recs, ledger)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
