#!/usr/bin/env python3
"""Checks SchemaDiffSuite's tables against a real schema-dump.json.

    python3 -I scripts/check-schema-overlap.py <schema-dump.json>

Two directions, both must be clean (exit 0):
  1. nothing registered that the schema lacks - every class, enum and field
     in BuildSdkClasses()/BuildSdkEnums() exists in the dump;
  2. nothing in the overlap left untested - every class/enum the dump and the
     SDK both declare is registered, with every field both sides name and every
     enumerator the SDK declares (an SDK enumerator the schema lacks is a
     finding the suite reports, so it must be registered).

"Overlap" is by name against s2sdk's headers, so Flags_t (declared three times
in the SDK) is resolved to the one in public/const.h.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUITE = os.path.join(ROOT, 'src', 'suites', 'SchemaDiffSuite.cpp')
SDK = os.path.join(ROOT, 's2sdk')
PREFER = {'Flags_t': 'public/const.h'}


def sdk_headers():
    for dp, _, fs in os.walk(SDK):
        for f in fs:
            if f.endswith(('.h', '.hpp', '.inl')):
                p = os.path.join(dp, f)
                yield os.path.relpath(p, SDK).replace(os.sep, '/'), open(p, errors='ignore').read()


def strip(t):
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//.*', '', t)


def brace_body(t, start):
    depth, i = 0, t.index('{', start)
    for j in range(i, len(t)):
        depth += t[j] == '{'
        depth -= t[j] == '}'
        if depth == 0:
            return t[i + 1:j]
    return ''


def main():
    dump = json.load(open(sys.argv[1]))
    classes = {}
    for c in dump['classes']:
        classes.setdefault(c['name'], []).append(c)
    enums = {}
    for e in dump['enums']:
        enums.setdefault(e['name'], []).append(e)

    sdk_class, sdk_enum = {}, {}
    for rel, text in sdk_headers():
        t = strip(text)
        for m in re.finditer(r'^\s*(?:class|struct)\s+([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^;{]*)?\{', t, re.M):
            sdk_class.setdefault(m.group(1), (rel, brace_body(t, m.start())))
        for m in re.finditer(r'^\s*enum(?:\s+class)?\s+([A-Za-z_]\w*)\s*(?::[^;{]*)?\{', t, re.M):
            n = m.group(1)
            if n in PREFER and PREFER[n] != rel:
                continue
            sdk_enum.setdefault(n, (rel, brace_body(t, m.start())))

    src = open(SUITE, errors='ignore').read()
    reg_classes, reg_enums = {}, {}
    cur = None
    for line in src.splitlines():
        m = re.search(r'\b\w+\.name = "(\w+)";', line)
        if m:
            cur = m.group(1)
            (reg_enums if 'enum' in line or cur in enums and cur not in classes else reg_classes).setdefault(cur, set())
            continue
        m = re.search(r'SDK_FIELD\(\w+,\s*(\w+)\)', line) or re.search(r'\{\s*"(\w+)",\s*PrivateFieldOffset', line)
        if m and cur:
            reg_classes.setdefault(cur, set()).add(m.group(1))
        m = re.search(r'SDK_ENUMERATOR\((\w+)\)', line)
        if m and cur:
            reg_enums.setdefault(cur, set()).add(m.group(1))

    problems = []
    for n, fields in reg_classes.items():
        if n not in classes:
            problems.append('registered class not in schema: %s' % n)
            continue
        schema_fields = {f['name'] for c in classes[n] for f in c['fields']}
        for f in sorted(fields - schema_fields):
            problems.append('registered field not in schema: %s::%s' % (n, f))
    for n, vals in reg_enums.items():
        if n not in enums:
            problems.append('registered enum not in schema: %s' % n)
            continue

    overlap_c = sorted(set(classes) & set(sdk_class))
    overlap_e = sorted(set(enums) & set(sdk_enum))
    for n in overlap_c:
        if n not in reg_classes:
            problems.append('overlap class not tested: %s (%s)' % (n, sdk_class[n][0]))
            continue
        schema_fields = {f['name'] for c in classes[n] for f in c['fields']}
        for f in sorted(schema_fields):
            if re.search(r'\b%s\b' % re.escape(f), sdk_class[n][1]) and f not in reg_classes[n]:
                problems.append('overlap field not tested: %s::%s' % (n, f))
    for n in overlap_e:
        if n not in reg_enums:
            problems.append('overlap enum not tested: %s (%s)' % (n, sdk_enum[n][0]))
            continue
        # Every enumerator the SDK declares, whether or not the schema has it:
        # one the schema lacks is exactly what the suite exists to report.
        for v in re.findall(r'^\s*([A-Za-z_]\w*)', sdk_enum[n][1], re.M):
            if v not in reg_enums[n]:
                problems.append('overlap enumerator not tested: %s::%s' % (n, v))

    print('overlap: %d classes, %d enums; registered: %d classes, %d enums'
          % (len(overlap_c), len(overlap_e), len(reg_classes), len(reg_enums)))
    for p in problems:
        print('  ' + p)
    print('OK' if not problems else '%d problem(s)' % len(problems))
    return 1 if problems else 0


sys.exit(main())
