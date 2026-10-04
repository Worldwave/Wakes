# Reproduce firmware/CMakeLists.txt's sp1_plaits_override() on the host, by parsing the
# CMakeLists itself so the harness can never drift from the firmware.
import os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
ER   = os.path.join(ROOT, 'third_party/eurorack')
OUT  = sys.argv[1]
txt  = open(os.path.join(ROOT, 'firmware/CMakeLists.txt'), encoding='utf-8').read()

# CONFIG_* substitutions the Kconfig would supply.
CFG = {'CONFIG_SP1_STRING_VOICES': '2', 'CONFIG_SP1_PARTICLES': '2',
       'CONFIG_SP1_MODAL_MODES': '12',
       # #32: hostbuild.sh exports the block size it builds for (default 24, the firmware's).
       'CONFIG_SP1_PLAITS_BLOCK': os.environ.get('SP1_PLAITS_BLOCK', '24')}

def unquote(s):
    # CMake's own unescaping for a quoted argument, in ONE pass: \n \t \" \\.
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            nxt = s[i + 1]
            out.append({'n': '\n', 't': '\t', 'r': '\r'}.get(nxt, nxt))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)

calls = []
i = 0
while True:
    m = re.search(r'(?m)^[\t ]*sp1_plaits_override\(', txt[i:])
    if not m: break
    start = i + m.end()
    # scan balanced parens, honouring quoted strings
    depth, j, inq = 1, start, False
    while depth:
        c = txt[j]
        if inq:
            if c == '\\': j += 1
            elif c == '"': inq = False
        else:
            if c == '"': inq = True
            elif c == '(': depth += 1
            elif c == ')': depth -= 1
        j += 1
    body = txt[start:j-1]
    i = j
    # args: bare rel, then two quoted strings
    mm = re.match(r'\s*(\S+)\s*', body)
    rel = mm.group(1)
    rest = body[mm.end():]
    args = []
    k = 0
    while len(args) < 2:
        while rest[k] in ' \t\n': k += 1
        assert rest[k] == '"', (rel, rest[k:k+40])
        k += 1
        buf = []
        while rest[k] != '"' or rest[k-1] == '\\':
            buf.append(rest[k]); k += 1
        k += 1
        args.append(''.join(buf))
    calls.append((rel, unquote(args[0]), unquote(args[1])))

n = 0
for rel, frm, to in calls:
    for key, val in CFG.items():
        to = to.replace('${' + key + '}', val)
    src = open(os.path.join(ER, rel), encoding='utf-8').read()
    if src.count(frm) != 1:
        sys.exit('override %s: pattern found %d times' % (rel, src.count(frm)))
    dst = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'w', encoding='utf-8').write(src.replace(frm, to))
    n += 1
    print('  override %s' % rel)
# ⚠️ Remove any generated file that is no longer an override. OUT comes FIRST on the
# include path, so a stale copy -- say voice.h after it became a full replacement in
# firmware/src/plaits_ovr -- would silently shadow the file that replaced it.
wanted = {os.path.normpath(os.path.join(OUT, rel)) for rel, _, _ in calls}
for root, _, files in os.walk(OUT):
    for f in files:
        path = os.path.normpath(os.path.join(root, f))
        if path not in wanted:
            os.remove(path)
            print('  removed stale %s' % os.path.relpath(path, OUT))
print('%d overrides written to %s' % (n, OUT))
