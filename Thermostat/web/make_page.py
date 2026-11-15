#!/usr/bin/env python3
"""Turns web/page.html into src/web_page.h, the copy the firmware serves.
Run after editing page.html:   python web\\make_page.py   (from the Thermostat folder)"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
html = (root / "web" / "page.html").read_text(encoding="utf-8")

# strip indentation and blank lines; the script and style blocks do not depend on either
lines = [l.strip() for l in html.splitlines()]
html = "\n".join(l for l in lines if l)

# keep the header plain ASCII: every other character only occurs inside JavaScript strings
head, script = html.split("<script>", 1)
if any(ord(c) > 127 for c in head):
    sys.exit("non-ASCII character outside the script block: use an HTML entity there")
script = "".join(c if ord(c) < 128 else "\\u%04x" % ord(c) for c in script)
html = head + "<script>" + script

if ')WEBPAGE"' in html:
    sys.exit("page contains the raw-string terminator")
out = ("// Generated from web/page.html by web/make_page.py. Edit the HTML, not this file.\n"
       "#pragma once\n"
       "static const char WEB_PAGE[] = R\"WEBPAGE(" + html + ")WEBPAGE\";\n")
(root / "src" / "web_page.h").write_text(out, encoding="ascii", newline="\n")
print("src/web_page.h written,", len(html), "bytes of page")
