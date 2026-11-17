#!/usr/bin/env python3
"""Generate the tbc-segments glossary outputs from glossary.json.

glossary.json is the single source for tbc-tools' segment vocabulary. This
script writes the two files generated from it, both committed so that a build
never needs Python:

    src/tbc-segments/core/glossary_generated.h   Glossary::Term and the text
                                                 tooltips/help/comments use
    docs/Tools/tbc-segments-glossary.md          the glossary page the docs link to

Usage:
    generate_glossary.py            regenerate both files
    generate_glossary.py --check    exit 1 if either file is out of date
    generate_glossary.py --lint     exit 1 if a retired word is used, or a term is never used
    generate_glossary.py --check --lint   both (the ctest)
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
SOURCE = HERE / "glossary.json"
HEADER = REPO / "src/tbc-segments/core/glossary_generated.h"
DOC = REPO / "docs/Tools/tbc-segments-glossary.md"
DOC_LINK = "docs/Tools/tbc-segments-glossary.md"

# Files that use segment vocabulary: the lint reads these
SCOPE = [
    "src/tbc-segments",
    "src/tbc-analyse/mainwindow.cpp",
    "src/tbc-analyse/mainwindow.h",
    "src/tbc-analyse/tbcsource.cpp",
    "src/tbc-analyse/tbcsource.h",
    "src/tbc-analyse/exportdialog.cpp",
    "src/tbc-analyse/exportdialog.h",
    "src/tbc-analyse/exportdialog.ui",
    "src/tbc-analyse/README.md",
    "AGENTS.md",
    "src/tbc-analyse/timelinemarkerslider.cpp",
    "src/tbc-analyse/timelinemarkerslider.h",
    "src/tbc-export-metadata",
    "src/tbc-video-export/src/tbc_video_export/opts/opts_ldtools.py",
    "src/tbc-video-export/src/tbc_video_export/process/wrapper/wrapper_ld_export_metadata.py",
    "src/library/tbc/tbcmetadata.h",
    "src/library/README.md",
    "src/tbc-metadata-converter/README.md",
    "docs/Tools/tbc-segments.md",
    "docs/Tools/tbc-analyse.md",
    "docs/Tools/tbc-export-metadata.md",
]
SUFFIXES = {".cpp", ".h", ".py", ".md", ".ui", ".txt", ".json"}
# Never linted: the glossary itself (it names retired words to retire them),
# and test fixtures
EXCLUDE = {SOURCE, HEADER, DOC, HERE / "generate_glossary.py"}
EXCLUDE_DIRS = {REPO / "test-data"}
# The nix build's source leaves docs/ (and the root .md files) out, so there
# the doc page cannot be checked and a term used only in the docs looks
# unused: the ctest then checks the header and the code's words only
HAS_DOCS = (REPO / "docs").is_dir()

# Retired words, and the module paths where the stricter ones apply. A line
# that must keep one (reading legacy data) carries the marker below.
LEGACY_MARKER = "glossary-legacy"
RETIRED_EVERYWHERE = [
    (re.compile(r"recording[- ]segments?", re.I), "\"recording segment\": say \"segment\""),
    (re.compile(r"recording[- ]boundar", re.I), "\"recording boundary\": say \"segment\" or \"shot boundary\""),
    (re.compile(r"restart[ _-]?marks?|restart[ _-]?chapters?|restartTicks|restartEvents|Split at restarts", re.I),
     "\"restart\": say \"shot boundary\""),
    (re.compile(r"\bSegmentMark\b|segment_mark|MarkStore|markstore|\bMarks tab\b"), "\"mark\": the object is a shot boundary"),
    (re.compile(r"\u00b7 take\b|\btake [0-9k]\b"), "\"take\": shots are numbered \"<title> \u00b7 n\""),
    (re.compile(r"enabled_clips|Enabled clips|minClipFields|min-clip-fields|minNonClipRun|min-non-clip"),
     "\"clip\": the kind is \"content_span\""),
    # The event kind and its threshold; scene_change as a shot boundary's flag is the glossary term
    (re.compile(r"\"scene_change\"|`scene_change`|scene_change event|sceneThresholdIre|scene-threshold-ire|Scene IRE"),
     "\"scene_change\" (the event): it is a \"field diff spike\"; a scene change is a person's mark"),
    (re.compile(r"cut here|cut at every shot boundary|cutAtEveryShotBoundary|shot[ -]chapters|shotChapters", re.I),
     "retired: exports choose a granularity (\"export by\", \"chapters by\")"),
]
RETIRED_IN_MODULE = [
    (re.compile(r"\brestarts?\b", re.I), "\"restart\": say \"shot boundary\""),
    (re.compile(r"\bclips?\b"), "\"clip\": the segment kind is \"content_span\""),
    (re.compile(r"\bmarks? (inside|at|on|that|which)\b|\b(a|the|each|every|its|restart) marks?\b", re.I),
     "\"mark\" (a noun): the object is a shot boundary"),
    (re.compile(r"\bsplit(s|ting)?\b(?!\(\))", re.I), "\"split\": a hand-made segment boundary is a \"cut\""),
]
MODULE_PATHS = ["src/tbc-segments", "docs/Tools/tbc-segments.md"]
# Upstream (harrypm) text that happens to use a retired word: never ours to
# change, so the lint skips lines holding these exact phrases
UPSTREAM_PHRASES = [
    "profile of SNR per recording segment",   # docs/Tools/tbc-analyse.md, upstream's wording
]
# Generic words the module legitimately uses in other senses
MODULE_ALLOWED = [re.compile(p) for p in [r"\.split\(", r"split\(\)", r"QString::split", r"SplitBehavior"]]


def load():
    with open(SOURCE, encoding="utf-8") as handle:
        data = json.load(handle)
    ids = [t["id"] for t in data["terms"]]
    if len(ids) != len(set(ids)):
        sys.exit("glossary.json: duplicate term id")
    for term in data["terms"]:
        for other in term.get("seeAlso", []):
            if other not in ids:
                sys.exit(f"glossary.json: {term['id']} sees also unknown term {other}")
    return data


def enum_name(term_id):
    return "".join(part.capitalize() for part in term_id.split("_"))


def anchor(term_id):
    return term_id.replace("_", "-")


def cpp_string(text):
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'QStringLiteral("{escaped}")'


def render_header(data):
    terms = data["terms"]
    out = []
    out.append("// Generated by src/tbc-segments/glossary/generate_glossary.py from glossary.json.")
    out.append("// Do not edit: change glossary.json and run the script (the tbc-segments-glossary")
    out.append("// ctest fails while this file is out of date).")
    out.append("")
    out.append("#ifndef GLOSSARY_GENERATED_H")
    out.append("#define GLOSSARY_GENERATED_H")
    out.append("")
    out.append("#include <QString>")
    out.append("")
    out.append("// The segment vocabulary (docs/Tools/tbc-segments-glossary.md). Tooltips, help")
    out.append("// text and comments use these instead of re-defining a term.")
    out.append("namespace Glossary {")
    out.append("")
    out.append("enum class Term {")
    for term in terms:
        out.append(f"    {enum_name(term['id'])},")
    out.append("};")
    out.append("")
    for function, field in (("term", "term"), ("tooltip", "tooltip"), ("definition", "definition")):
        out.append(f"inline QString {function}(Term term)")
        out.append("{")
        out.append("    switch (term) {")
        for term in terms:
            out.append(f"    case Term::{enum_name(term['id'])}: return {cpp_string(term[field])};")
        out.append("    }")
        out.append("    return QString();")
        out.append("}")
        out.append("")
    out.append("// The term's anchor on the glossary page")
    out.append("inline QString anchor(Term term)")
    out.append("{")
    out.append("    switch (term) {")
    for term in terms:
        out.append(f"    case Term::{enum_name(term['id'])}: return {cpp_string(anchor(term['id']))};")
    out.append("    }")
    out.append("    return QString();")
    out.append("}")
    out.append("")
    out.append("// The glossary page and anchor, for \"see ...\" in help text")
    out.append("inline QString link(Term term)")
    out.append("{")
    out.append(f"    return QStringLiteral(\"{DOC_LINK}#\") + anchor(term);")
    out.append("}")
    out.append("")
    out.append("} // namespace Glossary")
    out.append("")
    out.append("#endif // GLOSSARY_GENERATED_H")
    return "\n".join(out) + "\n"


def render_doc(data):
    terms = data["terms"]
    by_id = {t["id"]: t for t in terms}
    out = []
    out.append(f"# {data['title']}")
    out.append("")
    out.append("<!-- Generated by src/tbc-segments/glossary/generate_glossary.py from glossary.json. Do not edit. -->")
    out.append("")
    out.append(data["intro"])
    out.append("")
    for group in data["groups"]:
        members = [t for t in terms if t["group"] == group["id"]]
        if not members:
            continue
        out.append(f"## {group['title']}")
        out.append("")
        for term in members:
            out.append(f"<a id=\"{anchor(term['id'])}\"></a>")
            out.append(f"### {term['term']}")
            out.append("")
            out.append(term["definition"])
            out.append("")
            if term.get("example"):
                out.append(f"*Example:* {term['example']}")
                out.append("")
            see = term.get("seeAlso", [])
            if see:
                links = ", ".join(f"[{by_id[s]['term']}](#{anchor(s)})" for s in see)
                out.append(f"See also: {links}.")
                out.append("")
    return "\n".join(out).rstrip("\n") + "\n"


def scoped_files():
    for entry in SCOPE:
        path = REPO / entry
        candidates = [path] if path.is_file() else sorted(path.rglob("*")) if path.is_dir() else []
        for file in candidates:
            if not file.is_file() or file.suffix not in SUFFIXES or file in EXCLUDE:
                continue
            if any(parent in EXCLUDE_DIRS for parent in file.parents):
                continue
            yield file


def lint(data):
    problems = []
    used = {t["id"]: False for t in data["terms"]}
    for file in scoped_files():
        relative = file.relative_to(REPO).as_posix()
        in_module = any(relative == p or relative.startswith(p + "/") for p in MODULE_PATHS)
        text = file.read_text(encoding="utf-8", errors="replace")
        for term in data["terms"]:
            if used[term["id"]]:
                continue
            if (f"Term::{enum_name(term['id'])}" in text
                    or f"Glossary: {term['term']}" in text
                    or f"tbc-segments-glossary.md#{anchor(term['id'])}" in text):
                used[term["id"]] = True
        for number, line in enumerate(text.splitlines(), 1):
            if LEGACY_MARKER in line or any(phrase in line for phrase in UPSTREAM_PHRASES):
                continue
            rules = RETIRED_EVERYWHERE + (RETIRED_IN_MODULE if in_module else [])
            for pattern, why in rules:
                match = pattern.search(line)
                if not match:
                    continue
                if in_module and any(allowed.search(line) for allowed in MODULE_ALLOWED) and "split" in why:
                    continue
                problems.append(f"{relative}:{number}: {why}: {line.strip()[:120]}")
    for term_id, was_used in used.items():
        if not was_used and HAS_DOCS:
            problems.append(f"term '{term_id}' is never used (no Glossary::Term, \"Glossary: <term>\" tag or doc link)")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail if a generated file is out of date")
    parser.add_argument("--lint", action="store_true", help="fail on retired words or unused terms")
    args = parser.parse_args()
    data = load()
    outputs = {HEADER: render_header(data), DOC: render_doc(data)}
    failed = False
    if args.check or args.lint:
        if args.check:
            if not HAS_DOCS:
                print("glossary: no docs/ in this tree (a nix build); checking the header only")
            for path, text in outputs.items():
                if path == DOC and not HAS_DOCS:
                    continue
                current = path.read_text(encoding="utf-8") if path.exists() else None
                if current != text:
                    print(f"{path.relative_to(REPO)} is out of date: run {Path(__file__).relative_to(REPO)}")
                    failed = True
        if args.lint:
            problems = lint(data)
            for problem in problems:
                print(problem)
            failed = failed or bool(problems)
        if not failed:
            print("glossary: generated files in sync" if args.check else "", end="")
            print("; vocabulary clean" if args.lint else "")
        return 1 if failed else 0
    for path, text in outputs.items():
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
