#!/usr/bin/env python3
"""Collects every translatable text of the Robz Play interface.

Robz Play keeps its translations in one plain file per language,
gui/res/i18n/<language>.json, mapping the English text in the source code to its
translation. This script reads the QML and C++ sources, lists the English texts found in
qsTr("...") and tr("...") and reports which of them the language file does not cover yet.

    python gui/tools/i18n_extract.py                  # report for Turkish
    python gui/tools/i18n_extract.py --dump out.json  # all texts, with the file they are in
    python gui/tools/i18n_extract.py --prune          # drop translations no text uses any more
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', 'b': '\b', 'f': '\f', 'v': '\v'}


def read_literal(src, i):
    """Decodes the string literal starting at src[i]; returns (text, index after it)."""
    quote = src[i]
    out = []
    i += 1
    while i < len(src):
        c = src[i]
        if c == '\\':
            n = src[i + 1]
            if n == 'u':
                out.append(chr(int(src[i + 2:i + 6], 16)))
                i += 6
            elif n == 'x':
                out.append(chr(int(src[i + 2:i + 4], 16)))
                i += 4
            elif n == '\n':
                i += 2
            else:
                out.append(ESCAPES.get(n, n))
                i += 2
        elif c == quote:
            return ''.join(out), i + 1
        else:
            out.append(c)
            i += 1
    raise ValueError('unterminated string')


def texts_in(src, call, adjacent):
    """Yields the literal arguments of call("...") in src.

    adjacent: C++ joins "a" "b" into one literal.
    """
    for m in re.finditer(r'(?<![A-Za-z0-9_])' + call + r'\(\s*', src):
        i = m.end()
        if i >= len(src) or src[i] not in '"\'':
            continue
        if src[i] == "'" and adjacent:
            continue
        try:
            text, i = read_literal(src, i)
            while adjacent:
                j = i
                while j < len(src) and src[j] in ' \t\r\n':
                    j += 1
                if j < len(src) and src[j] == '"':
                    more, i = read_literal(src, j)
                    text += more
                else:
                    break
        except (ValueError, IndexError):
            continue
        j = i
        while j < len(src) and src[j] in ' \t\r\n':
            j += 1
        # only a call whose whole argument is the literal can be looked up at run time
        if j < len(src) and src[j] in '),':
            yield text


def collect():
    found = {}
    for folder, call, adjacent, endings in (
            ('src/qml', 'qsTr', False, ('.qml',)),
            ('src', 'tr', True, ('.cpp',)),
            ('include', 'tr', True, ('.h',))):
        base = os.path.join(ROOT, folder)
        for path, _, files in os.walk(base):
            if folder != 'src/qml' and os.path.abspath(path) != base:
                continue
            for name in sorted(files):
                if not name.endswith(endings):
                    continue
                src = io.open(os.path.join(path, name), encoding='utf-8').read()
                for text in texts_in(src, call, adjacent):
                    if text.strip():
                        found.setdefault(text, name)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--language', default='tr')
    parser.add_argument('--dump', help='write every text and the file it is in to this JSON file')
    parser.add_argument('--prune', action='store_true', help='remove translations of texts that are gone')
    args = parser.parse_args()

    found = collect()
    if args.dump:
        entries = [{'text': t, 'where': w} for t, w in sorted(found.items(), key=lambda kv: (kv[1], kv[0]))]
        io.open(args.dump, 'w', encoding='utf-8', newline='\n').write(json.dumps(entries, ensure_ascii=False, indent=1))
        print('%d texts written to %s' % (len(entries), args.dump))
        return 0

    path = os.path.join(ROOT, 'res', 'i18n', args.language + '.json')
    table = json.load(io.open(path, encoding='utf-8')) if os.path.exists(path) else {}
    missing = sorted(t for t in found if not table.get(t))
    unused = sorted(t for t in table if t not in found)
    print('%d texts in the interface, %d translated, %d missing, %d no longer used'
          % (len(found), len(found) - len(missing), len(missing), len(unused)))
    for text in missing:
        print('  missing: %r (%s)' % (text, found[text]))
    if args.prune and unused:
        for text in unused:
            del table[text]
        io.open(path, 'w', encoding='utf-8', newline='\n').write(
            json.dumps(dict(sorted(table.items())), ensure_ascii=False, indent=1) + '\n')
        print('%d unused translations removed' % len(unused))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
