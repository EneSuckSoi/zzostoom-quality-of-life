"""Lists the English UI strings of the menu that have no translation yet.

    py -X utf8 tools/list_strings.py [lang]

Collects every narrow string literal in the UI sources (adjacent literals joined) that has
letters in it; the ones missing from lang/<lang>.tsv are printed. Not every literal is UI text
(ini keys, ImGui IDs...), so the output is a to-do list for a human, not a hard error.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ["src/ui.cpp", "src/hud.cpp", "src/fps.cpp", "src/params.cpp", "src/features.cpp",
           "src/buffs.cpp"]
BS = chr(92)


def literals(text):
    out, i, n = [], 0, len(text)
    current = None
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if text.startswith("/*", i):
            i = text.find("*/", i) + 2
            continue
        if c == "'":
            j = i + 1
            while text[j] != "'":
                j += 2 if text[j] == BS else 1
            i = j + 1
            current = None
            continue
        if c == '"':
            wide = i > 0 and text[i - 1] in "LuU8"
            j, buf = i + 1, []
            while text[j] != '"':
                if text[j] == BS:
                    buf.append(text[j:j + 2])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            value = unescape("".join(buf))
            if current is not None and not wide:
                out[-1] += value
            elif not wide:
                out.append(value)
                current = True
            i = j + 1
            continue
        if not c.isspace():
            current = None
        i += 1
    return out


def unescape(s):
    return (s.replace(BS + '"', '"').replace(BS + "n", "\n").replace(BS + BS, BS))


def load_tsv(path):
    d = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\n")
            if not line or line.startswith("#") or "\t" not in line:
                continue
            k, v = line.split("\t", 1)
            d[k.replace(BS + "n", "\n")] = v
    return d


def main():
    lang = sys.argv[1] if len(sys.argv) > 1 else "de"
    have = load_tsv(os.path.join(ROOT, "lang", lang + ".tsv"))
    seen = []
    for src in SOURCES:
        text = open(os.path.join(ROOT, src), encoding="utf-8").read()
        for s in literals(text):
            if re.search("[A-Za-z]{2}", s) and s not in seen and not s.endswith(".h") and                     "ZZ_LOG" not in s:
                seen.append(s)
    for s in seen:
        if s not in have:
            print(s.replace("\n", BS + "n"))


if __name__ == "__main__":
    main()
