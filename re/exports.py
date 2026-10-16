"""Demangle the 123 MSVC-mangled cUE4Interface exports and emit grouped Markdown tables."""
import json, re

EX = json.load(open(r"D:\INDAC\DGLab_IDAC\re\recon1.json"))["exports"]

BUILTIN = {
    'X': 'void', 'D': 'char', 'E': 'unsigned char', 'F': 'short', 'H': 'int',
    'I': 'unsigned int', 'J': 'long', 'K': 'unsigned long', 'M': 'float',
    'N': 'double', 'C': 'char', '_N': 'bool', '_J': 'long long',
    '_K': 'unsigned long long', '_W': 'wchar_t',
}
STATIC_RET = {'GetSteeringAdjustment': 'int',
              'getDebugParamFlag': 'ValueFlag*',
              'getDebugParamFloat': 'ValueFloat*',
              'DumpMemory': 'void'}


def fmt_type(name):
    parts = name.split('@')
    return '::'.join(reversed(parts)) if len(parts) > 1 else name


def dem_params(s):
    out, i, n = [], 0, len(s)
    while i < n:
        rest = s[i:]
        if rest.startswith('V?$function@') or rest.startswith('AEBV?$function@'):
            isref = rest.startswith('AEB')
            off = 22 if isref else 18
            j = rest.find('Z@std@@')
            sig = rest[off:j]
            if sig.endswith('@'):
                sig = sig[:-1]
            out.append('%sstd::function<void(%s)>%s' %
                       ('const ' if isref else '', dem_params(sig), '&' if isref else '&&'))
            i += j + 7
        elif rest.startswith('V?$basic_string') or rest.startswith('AEBV?$basic_string'):
            k = 3 if rest.startswith('AEB') else 0
            out.append('const std::string&' if k else 'std::string&&')
            i += rest.find('@std@@') + 6
        elif rest.startswith('PEBD'):
            out.append('const char*'); i += 4
        elif rest.startswith('PEB'):
            out.append('const char*'); i += 3
        elif rest.startswith('PEAU'):
            m = re.match(r'PEAU([\w@]+)@@', rest)
            out.append('const %s*' % fmt_type(m.group(1))); i += m.end()
        elif rest.startswith('PEA'):
            out.append('void*'); i += 3
        elif rest.startswith('AEBU'):
            m = re.match(r'AEBU([\w@]+)@@', rest)
            out.append('const %s&' % fmt_type(m.group(1))); i += m.end()
        elif rest.startswith('AEBV'):
            m = re.match(r'AEBV([\w@]+)@@', rest)
            out.append('const %s&' % fmt_type(m.group(1))); i += m.end()
        elif rest.startswith('W4'):
            m = re.match(r'W4([\w@]+)@@', rest)
            out.append(fmt_type(m.group(1))); i += m.end()
        elif rest.startswith('U'):
            m = re.match(r'U([\w@]+)@@', rest)
            out.append('%s&' % fmt_type(m.group(1))); i += m.end()
        elif rest.startswith('_N'):
            out.append('bool'); i += 2
        elif rest[0] in BUILTIN:
            out.append(BUILTIN[rest[0]]); i += 1
        else:
            out.append('?' + rest[0]); i += 1
    return ', '.join(out)


def demangle(m):
    if m.startswith('??0'):
        return 'cUE4Interface::cUE4Interface()'
    if m.startswith('??1'):
        return 'cUE4Interface::~cUE4Interface()'
    if m.startswith('??4'):
        return 'cUE4Interface& cUE4Interface::operator=(cUE4Interface const&)'
    b = m[1:]
    name = b.split('@', 1)[0]
    mm = re.search(r'@@(Q[A-Z]{3}|S[A-Z]|C[A-Z]|A[A-Z])(.*?)@?Z$', b)
    if not mm:
        return name + '()   /* [pattern not parsed] */'
    quals, rest = mm.group(1), mm.group(2)
    const = quals[2:3] == 'B'
    if quals.startswith('S'):
        ret = STATIC_RET.get(name, BUILTIN.get(rest[:1], 'void'))
        return '%s cUE4Interface::%s(%s)  [static]' % (ret, name, dem_params(rest[1:]))
    params = rest[1:] if (rest[:1] in BUILTIN or rest[:1] == '_') else rest
    return 'void cUE4Interface::%s(%s)%s' % (name, dem_params(params), ' const' if const else '')


rows = sorted(((e['ord'], e['rva'], e['name'], demangle(e['name'])) for e in EX),
              key=lambda r: (r[1], r[0]))

with open(r"D:\INDAC\DGLab_IDAC\re\_exports_table.md", 'w', encoding='utf-8') as f:
    f.write('| ord | RVA | mangled name | demangled |\n|----:|-----|--------------|-----------|\n')
    for o, r, m, d in rows:
        f.write('| %d | `0x%06x` | `%s` | `%s` |\n' % (o, r, m, d))

KEY = ('Bind', 'UnBind', 'Servo', 'Torque', 'Vibration', 'Viscosity', 'Center',
       'Steer', 'FB', 'OnTick', 'Accel', 'Brake', 'Shift', 'Tick', 'Pause', 'Timer')
fb = [r for r in rows if any(k in r[2].split('@')[0] for k in KEY)]
with open(r"D:\INDAC\DGLab_IDAC\re\_ffb_table.md", 'w', encoding='utf-8') as f:
    f.write('| ord | RVA | demangled |\n|----:|-----|-----------|\n')
    for o, r, m, d in fb:
        f.write('| %d | `0x%06x` | `%s` |\n' % (o, r, d))

print("exports:%d  distinct RVAs:%d  stub-aliases(0x3db0):%d" %
      (len(rows), len(set(r[1] for r in rows)), sum(1 for r in rows if r[1] == 0x3db0)))
print("\n=== FFB / input / tick related (%d) ===" % len(fb))
for o, r, m, d in fb:
    print("  ord %-4d 0x%06x  %s" % (o, r, d))
print("\n=== demangle spot-checks ===")
for o, r, m, d in rows[:6] + rows[50:56]:
    print("   %s" % d)
