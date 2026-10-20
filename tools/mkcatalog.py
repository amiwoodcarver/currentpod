#!/usr/bin/env python3
"""Katalogverktyg för CurrentPod (ersätter CatComp/FlexCat på värddatorn).

  mkcatalog.py header  CurrentPod.cd  strings.h           C-huvud med MSG_-ID:n och inbyggda (engelska) strängar
  mkcatalog.py catalog CurrentPod.cd  svenska.ct  ut.catalog   IFF CTLG-katalog för locale.library

.cd-formatet är CatComp:s: en rad "MSG_ID (nr//)" följd av strängen, ";" inleder kommentar.
.ct-formatet: "## language xx", "## version ..." och sedan "MSG_ID" följt av översättningen.
Filerna är UTF-8 på disk; katalogen och huvudet skrivs i ISO-8859-1.
"""
import re, struct, sys

def unescape(s):
    out = []; i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'n': out.append('\n'); i += 2; continue
            if n == 't': out.append('\t'); i += 2; continue
            if n == '"': out.append('"'); i += 2; continue
            if n == '\\': out.append('\\'); i += 2; continue
            m = re.match(r'[0-7]{1,3}', s[i + 1:])
            if m: out.append(chr(int(m.group(0), 8))); i += 1 + len(m.group(0)); continue
        out.append(c); i += 1
    return ''.join(out)

def read_cd(path):
    ids = []; lines = open(path, encoding='utf-8').read().split('\n'); i = 0
    while i < len(lines):
        l = lines[i].rstrip('\r')
        m = re.match(r'^(MSG_\w+)\s*\((\d*)/', l)
        if m:
            num = int(m.group(2)) if m.group(2) else (ids[-1][1] + 1 if ids else 0)
            ids.append((m.group(1), num, unescape(lines[i + 1].rstrip('\r'))))
            i += 2; continue
        i += 1
    return ids

def read_ct(path):
    tr = {}; lang = 'english'; ver = ''; lines = open(path, encoding='utf-8').read().split('\n'); i = 0
    while i < len(lines):
        l = lines[i].rstrip('\r')
        if l.startswith('## language'): lang = l.split(None, 2)[2].strip()
        elif l.startswith('## version'): ver = l.split(None, 2)[2].strip()
        elif re.match(r'^MSG_\w+$', l):
            tr[l] = unescape(lines[i + 1].rstrip('\r')); i += 2; continue
        i += 1
    return lang, ver, tr

def c_escape(s):
    out = ''
    for ch in s:
        if ch == '"': out += '\\"'
        elif ch == '\\': out += '\\\\'
        elif ch == '\n': out += '\\n'
        elif ord(ch) < 32: out += '\\%03o' % ord(ch)
        else: out += ch
    return out

def write_header(ids, path):
    maxid = max(n for _, n, _ in ids)
    table = [''] * (maxid + 1)
    with open(path, 'w', encoding='latin-1') as f:
        f.write('/* Genererad av tools/mkcatalog.py ur CurrentPod.cd. Redigera inte. */\n#ifndef CP_STRINGS_H\n#define CP_STRINGS_H\n')
        for name, n, text in ids:
            f.write('#define %s %d\n' % (name, n)); table[n] = text
        f.write('#define MSG_COUNT %d\n#ifdef CP_STRINGS_TABLE\nstatic const char *const cp_builtin[MSG_COUNT] = {\n' % (maxid + 1))
        for t in table: f.write('    "%s",\n' % c_escape(t))
        f.write('};\n#endif\n#endif\n')

def chunk(cid, data):
    if len(data) & 1: data += b'\0'
    return cid + struct.pack('>I', len(data) - (1 if False else 0)) + data if False else cid + struct.pack('>I', len(data)) + data

def write_catalog(ids, ct_path, out):
    lang, ver, tr = read_ct(ct_path)
    body = b''
    fver = (ver or '$VER: CurrentPod.catalog 1.0').encode('latin-1') + b'\0'
    body += b'FVER' + struct.pack('>I', len(fver)) + fver + (b'\0' if len(fver) & 1 else b'')
    l = lang.encode('latin-1') + b'\0'
    body += b'LANG' + struct.pack('>I', len(l)) + l + (b'\0' if len(l) & 1 else b'')
    body += b'CSET' + struct.pack('>I', 32) + b'\0' * 32
    strs = b''
    missing = []
    for name, n, text in ids:
        if name not in tr: missing.append(name); continue
        s = tr[name].encode('latin-1') + b'\0'
        pad = (-len(s)) % 4
        strs += struct.pack('>II', n, len(s)) + s + b'\0' * pad
    body += b'STRS' + struct.pack('>I', len(strs)) + strs
    with open(out, 'wb') as f:
        f.write(b'FORM' + struct.pack('>I', len(body) + 4) + b'CTLG' + body)
    return lang, len(tr), missing

if __name__ == '__main__':
    cmd = sys.argv[1]
    ids = read_cd(sys.argv[2])
    if cmd == 'header':
        write_header(ids, sys.argv[3]); print('%s: %d strängar' % (sys.argv[3], len(ids)))
    elif cmd == 'catalog':
        lang, n, missing = write_catalog(ids, sys.argv[3], sys.argv[4])
        print('%s: %s, %d strängar%s' % (sys.argv[4], lang, n, (', SAKNAS: ' + ' '.join(missing)) if missing else ''))
