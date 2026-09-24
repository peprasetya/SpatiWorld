"""Take Firestorm's own services out of the main menu, in every language.

SpatiWorld is a fork, not a Firestorm build: it does not send people to Firestorm's support
groups, classes, events calendar, blog or social media accounts, and the Content menu held
nothing else. Run from indra/newview; rerun after a rebase brings the entries back.
"""
import pathlib
import re
import sys

MENUS = ["Content"]
ITEMS = ["firestorm_support_group", "Firestorm Classes Schedule", "Firestorm Events Calendar"]


def element_span(text, start, tag):
    """The end of the element whose start tag begins at `start`, nested ones counted."""
    head_end = text.index(">", start)
    if text[head_end - 1] == "/":
        return head_end + 1
    depth = 1
    pattern = re.compile(r"<(/?)%s(?=[\s>/])" % re.escape(tag))
    pos = head_end + 1
    while depth:
        m = pattern.search(text, pos)
        if not m:
            raise ValueError(f"unclosed <{tag}>")
        if m.group(1):
            depth -= 1
            pos = text.index(">", m.end()) + 1
        else:
            end = text.index(">", m.end())
            if text[end - 1] != "/":
                depth += 1
            pos = end + 1
    return pos


def remove(text, tag, name):
    removed = 0
    while True:
        m = re.search(r'<%s\b[^>]*\bname="%s"' % (re.escape(tag), re.escape(name)), text)
        if not m:
            return text, removed
        start = m.start()
        end = element_span(text, start, tag)
        # Take the element's own line with it.
        line_start = text.rfind("\n", 0, start) + 1
        if text[line_start:start].strip() == "":
            start = line_start
        if text[end:end + 1] == "\n":
            end += 1
        text = text[:start] + text[end:]
        removed += 1


total = 0
for path in sorted(pathlib.Path("skins").glob("*/xui/*/menu_viewer.xml")):
    text = path.read_text(encoding="utf-8")
    before = text
    for name in MENUS:
        text, n = remove(text, "menu", name)
        total += n
    for name in ITEMS:
        text, n = remove(text, "menu_item_call", name)
        total += n
    text = re.sub(r"\n[ \t]*<!-- Content Menu -->[ \t]*\n", "\n", text)
    if text != before:
        path.write_text(text, encoding="utf-8")
        print("stripped", path)
print(total, "entries removed")
if "--check" in sys.argv and total:
    sys.exit(1)
