"""Checks that QML uses only members the exposed C++ objects have.

QML reaches C++ objects by name (singletons such as StreamingPreferences, context properties
such as `panel`, and model instances such as computerModel). qmllint cannot see their C++ side, so
a misspelt or removed property, method or signal compiles, loads and only fails when that line
runs (a TypeError in the log, a button that does nothing, a binding that stays undefined).

For each name below, every `name.member` in app/gui/*.qml and every `function onSignal()` /
`onSignal:` inside a Connections block whose target is that name must match a Q_PROPERTY,
Q_INVOKABLE method, public slot, signal or enum value of the C++ class (or its C++ bases in the
repo), or a QObject / model member.

Usage: python hermit/tests/check-qml-members.py   (exit code 1 on an unknown member)
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
APP = os.path.join(ROOT, 'app')

# QML name -> C++ class
OBJECTS = {
    'StreamingPreferences': 'StreamingPreferences',
    'SystemProperties': 'SystemProperties',
    'ComputerManager': 'ComputerManager',
    'SdlGamepadKeyNavigation': 'SdlGamepadKeyNavigation',
    'HermitTheme': 'HermitTheme',
    'ConnectionProfiles': 'ConnectionProfiles',
    'SessionHistory': 'SessionHistory',
    'panel': 'StreamPanel',
    'computerModel': 'ComputerModel',
    'appModel': 'AppModel',
    'session': 'Session',
    'launcher': 'Launcher',
}

# Members every QObject / list model has in QML
COMMON = {'objectName', 'objectNameChanged', 'destroyed', 'deleteLater', 'toString', 'connect',
          'disconnect', 'rowCount', 'count', 'index', 'data', 'dataChanged', 'modelReset',
          'rowsInserted', 'rowsRemoved', 'layoutChanged', 'parent'}


def headers():
    for base, _, files in os.walk(APP):
        for f in files:
            if f.endswith('.h'):
                yield os.path.join(base, f)


def class_bodies():
    """Class name -> list of (body text, base class names)."""
    found = {}
    for path in headers():
        text = open(path, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'\bclass\s+(?:\w+\s+)?(\w+)\s*(?::\s*([^{;]+))?\{', text):
            name = m.group(1)
            depth, i = 0, m.end() - 1
            for i in range(m.end() - 1, len(text)):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
            bases = re.findall(r'(?:public|protected|private)?\s*([\w:]+)', m.group(2) or '')
            # Several classes can share a name in different namespaces (cli::*::Launcher); QML
            # sees whichever is set, so their members are taken together
            found.setdefault(name, []).append((text[m.end():i], [b.split('::')[-1] for b in bases if b]))
    return found


def members(cls, classes, seen=None):
    seen = seen or set()
    if cls in seen or cls not in classes:
        return set()
    seen.add(cls)
    names = set()
    for body, bases in classes[cls]:
        names |= members_of_body(body, bases, classes, seen)
    return names


def members_of_body(body, bases, classes, seen):
    names = set(re.findall(r'Q_PROPERTY\(\s*[\w:<>*&, ]+?\s+(\w+)\s+(?:READ|MEMBER|WRITE)', body))
    names |= set(re.findall(r'Q_INVOKABLE\s+[\w:<>*&, ]+?\s*\**&?\s*(\w+)\s*\(', body))
    # Everything declared under signals: / public slots: (until the next access label)
    for section in re.finditer(r'\b(?:signals|Q_SIGNALS|public\s+slots|public\s+Q_SLOTS)\s*:(.*?)(?=\n\s*(?:public|private|protected|signals|Q_SIGNALS)\b[^:]*:|\Z)', body, re.S):
        names |= set(re.findall(r'(\w+)\s*\(', section.group(1)))
    # Enum values (StreamingPreferences.VCC_AUTO and the like)
    for e in re.finditer(r'\benum\s+(?:class\s+)?\w*\s*\{([^}]*)\}', body):
        names |= set(re.findall(r'^\s*(\w+)', e.group(1), re.M))
        names |= set(re.findall(r',\s*(\w+)', e.group(1)))
    # Property change signals named in NOTIFY
    names |= set(re.findall(r'NOTIFY\s+(\w+)', body))
    for b in bases:
        names |= members(b, classes, seen)
    return names


def main():
    classes = class_bodies()
    known = {}
    for qml_name, cls in OBJECTS.items():
        if cls in classes:
            known[qml_name] = members(cls, classes) | COMMON
    problems = []
    used = 0
    gui = os.path.join(APP, 'gui')
    for f in sorted(os.listdir(gui)):
        if not f.endswith('.qml'):
            continue
        path = os.path.join(gui, f)
        text = open(path, encoding='utf-8').read()
        # Strip comments and string literals (they mention names in prose)
        code = re.sub(r'//[^\n]*', '', text)
        code = re.sub(r'/\*.*?\*/', '', code, flags=re.S)
        code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
        for m in re.finditer(r'(?<![\w.])(' + '|'.join(known) + r')\.(\w+)', code):
            used += 1
            name, member = m.group(1), m.group(2)
            if member not in known[name]:
                line = code.count('\n', 0, m.start()) + 1
                problems.append('%s:%d: %s.%s is not a member of %s' % (f, line, name, member, OBJECTS[name]))
        # Connections { target: name; function onX() / onX: } -> signal x
        for c in re.finditer(r'Connections\s*\{', code):
            depth, end = 0, c.end()
            for end in range(c.end() - 1, len(code)):
                if code[end] == '{':
                    depth += 1
                elif code[end] == '}':
                    depth -= 1
                    if depth == 0:
                        break
            block = code[c.end():end]
            t = re.search(r'target\s*:\s*(\w+)\s*$', block, re.M)
            if not t or t.group(1) not in known:
                continue
            for h in re.finditer(r'(?:function\s+on(\w+)\s*\(|^\s*on(\w+)\s*:)', block, re.M):
                sig = h.group(1) or h.group(2)
                sig = sig[0].lower() + sig[1:]
                if sig not in known[t.group(1)]:
                    line = code.count('\n', 0, c.end() + h.start()) + 1
                    problems.append('%s:%d: no signal %s on %s' % (f, line, sig, OBJECTS[t.group(1)]))
    for p in problems:
        print('ERROR ' + p)
    print('QML member check: %d uses checked, %d problem(s)' % (used, len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
