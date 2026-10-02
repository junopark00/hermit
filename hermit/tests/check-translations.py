"""Checks the Korean translations against the source text.

1. Placeholders: a Korean translation that drops %1 (or turns %2 into %1) still loads; the screen
   then shows the wrong value or a literal "%2". Every finished translation in
   app/languages/hermit_ko.ts and qml_ko.ts must use the same %1..%9 / %n as its source.
2. Coverage: a qsTr() string in QML, or a QCoreApplication::translate("Context", "...") string in
   C++, without a finished Korean translation shows in English. Rewording a source string loses
   its translation the same way, so every such string must have one.

Usage: python hermit/tests/check-translations.py   (exit code 1 on a problem)
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FILES = ['app/languages/hermit_ko.ts', 'app/languages/qml_ko.ts']

STRING = r'"((?:[^"\\]|\\.)*)"'
# One or more literals joined with + (lupdate joins them into one source text)
LITERAL = r'"(?:[^"\\]|\\.)*"'
JOINED = LITERAL + r'(?:\s*\+\s*' + LITERAL + r')*'
QSTR = re.compile(r'\bqsTr\(\s*(' + JOINED + r')\s*[,)]')
TRANSLATE = re.compile(r'\b(?:QCoreApplication::)?translate\(\s*"(\w+)"\s*,\s*(' + JOINED + r')\s*[,)]')
UNESCAPE = {'n': '\n', 't': '\t', '"': '"', "'": "'", '\\': '\\'}


def unescape(s):
    return re.sub(r'\\(.)', lambda m: UNESCAPE.get(m.group(1), m.group(1)), s)


def join(literals):
    """The text of "a" + "b" literals, still escaped."""
    return ''.join(re.findall(STRING, literals))


def main():
    problems = 0
    checked = 0
    finished = set()
    for rel in FILES:
        root = ET.parse(os.path.join(ROOT, rel)).getroot()
        for ctx in root.findall('context'):
            cname = ctx.findtext('name')
            for msg in ctx.findall('message'):
                tr = msg.find('translation')
                if tr is None or tr.get('type') in ('unfinished', 'obsolete', 'vanished'):
                    continue
                source = msg.findtext('source') or ''
                finished.add((cname, source))
                forms = [f.text or '' for f in tr.findall('numerusform')] or [''.join(tr.itertext())]
                want = sorted(set(re.findall(r'%(\d|n)', source)))
                for text in forms:
                    checked += 1
                    got = sorted(set(re.findall(r'%(\d|n)', text)))
                    # A plural form may leave %n out (e.g. a single fixed word), never a %1..%9
                    if [p for p in want if p != 'n'] != [p for p in got if p != 'n']:
                        problems += 1
                        print('ERROR %s [%s] %r -> %r' % (rel, cname, source[:70], text[:70]))

    used = 0
    app = os.path.join(ROOT, 'app')
    for base, _, files in os.walk(app):
        for f in files:
            path = os.path.join(base, f)
            if f.endswith('.qml'):
                context = f[:-4]
                text = open(path, encoding='utf-8').read()
                found = [(context, join(m.group(1)), m.start()) for m in QSTR.finditer(text)]
            elif f.endswith(('.cpp', '.h')):
                text = open(path, encoding='utf-8', errors='replace').read()
                found = [(m.group(1), join(m.group(2)), m.start()) for m in TRANSLATE.finditer(text)]
            else:
                continue
            for context, raw, pos in found:
                if raw == '':
                    continue
                used += 1
                if (context, unescape(raw)) not in finished:
                    problems += 1
                    line = text.count('\n', 0, pos) + 1
                    print('ERROR %s:%d [%s] no Korean translation for %r' % (os.path.relpath(path, ROOT), line, context, unescape(raw)[:80]))
    print('translation check: %d translations and %d source strings checked, %d problem(s)' % (checked, used, problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
