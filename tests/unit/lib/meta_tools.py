#!/usr/bin/env python3
"""Rekayasa state cache untuk unit test DSL.

Dipakai lewat `run.exec python3 repo(tests/unit/lib/meta_tools.py) <cmd> <path>`
sehingga berkas .test tidak perlu shell inline (heredoc/pipe) untuk mengubah
isi cache secara presisi.

Perintah:
  strip-meta          Ubah baris P bermetadata menjadi format lama (tanpa kolom
                      metadata), lalu pad baris pertama dengan spasi agar ukuran
                      berkas tetap sama (regresi "size sama, isi beda").
  corrupt-same-second Ubah 1 heksadesimal pertama entri pertama (ukuran tetap)
                      dan set mtime ke detik yang sama, nanodetik berbeda.
Exit: 0 sukses, 3 tidak ada entri, 4 ukuran tak dapat dipertahankan.
"""
import os
import re
import sys

META = re.compile(rb'^P ([0-9a-f]{16}) ([A-Z]) ([0-9a-f]{16}) ([0-9]+) ([0-9]+) ')
ENTRY = re.compile(rb'^([0-9a-f]{16}) (S|D) ')


def strip_meta(path):
    with open(path, 'rb') as f:
        data = f.read()
    lines = []
    for ln in data.split(b'\n'):
        m = META.match(ln)
        if m:
            ln = b'P ' + m.group(1) + b' ' + m.group(2) + b' ' + ln[m.end():]
        lines.append(ln)
    body = b'\n'.join(lines)
    pad = len(data) - len(body)
    if pad < 0:
        return 4
    if pad:
        first, sep, rest = body.partition(b'\n')
        body = first + b' ' * pad + sep + rest
    with open(path, 'wb') as f:
        f.write(body)
    return 0


def corrupt_same_second(path):
    with open(path, 'rb') as f:
        data = f.read()
    sec = int(os.stat(path).st_mtime)
    lines = data.split(b'\n')
    for i, ln in enumerate(lines):
        m = ENTRY.match(ln)
        if m:
            a = m.group(1)
            lines[i] = (b'0' if a[:1] != b'0' else b'1') + a[1:] + ln[16:]
            break
    else:
        return 3
    out = b'\n'.join(lines)
    if len(out) != len(data):
        return 4
    with open(path, 'wb') as f:
        f.write(out)
    # Detik sama seperti sebelum tulis, nanodetik pasti berbeda.
    os.utime(path, ns=(sec * 10**9 + 123456789, sec * 10**9 + 123456789))
    return 0


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'strip-meta':
        return strip_meta(path)
    if cmd == 'corrupt-same-second':
        return corrupt_same_second(path)
    print('perintah tidak dikenal: ' + cmd, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
