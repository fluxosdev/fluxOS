#!/usr/bin/env python3
"""
Generate the browser's Unicode tables as C: user/desktop/wave/js/jsunicode.h.
The output is checked in, so building fluxOS does not need this script.

  python3 tools/mkunicode.py UCD_DIR > user/desktop/wave/js/jsunicode.h

UCD_DIR holds these files of the Unicode Character Database
(https://www.unicode.org/Public/16.0.0/ucd/, emoji-data.txt from its emoji/
folder): UnicodeData.txt, Scripts.txt, PropList.txt, DerivedCoreProperties.txt,
emoji-data.txt, PropertyAliases.txt, PropertyValueAliases.txt, CaseFolding.txt,
SpecialCasing.txt and DerivedNormalizationProps.txt.

The tables (read by jsuni.c):
  uc_ranges        code point ranges, lo | (length - 1) << 21; long ranges
                   are split, so a range is at most 2048 code points
  uc_props         each general category, binary property and script, with
                   its slice of uc_ranges; uc_aliases gives the other names
  uc_upper_runs, uc_lower_runs, uc_fold_runs, uc_orbit_runs
                   simple case mappings as runs: lo, count, stride, delta.
                   uc_orbit maps each character to the next one with the
                   same case folding, round in a circle, for /i matching
  uc_special       full case mappings that are more than one character
  uc_ccc           canonical combining classes other than 0, as ranges
  uc_decomp        decompositions: code point | length << 21 | compat << 26
                   | excluded from composition << 27, and the offset of the
                   characters (as UTF-16) in uc_decomp_text
"""
import os
import sys

MAX = 0x10FFFF


def ucd(path):
    """(lo, hi, fields) for each data line of a UCD file."""
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        cps = fields[0]
        if ".." in cps:
            lo, hi = (int(x, 16) for x in cps.split(".."))
        else:
            lo = hi = int(cps, 16)
        yield lo, hi, fields[1:]


def merge(ranges):
    out = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def to_ranges(cps):
    return merge((c, c) for c in cps)


def main(d):
    path = lambda name: os.path.join(d, name)

    # ---- UnicodeData: categories, simple case mappings, decompositions, ccc ----
    gc, upper, lower, decomp, ccc = {}, {}, {}, {}, {}
    first = None
    for line in open(path("UnicodeData.txt"), encoding="utf-8"):
        f = line.rstrip("\n").split(";")
        cp = int(f[0], 16)
        if f[1].endswith(", First>"):
            first = cp
            continue
        lo = first if f[1].endswith(", Last>") else cp
        first = None
        for c in range(lo, cp + 1):
            gc[c] = f[2]
        if f[3] != "0":
            ccc[cp] = int(f[3])
        if f[5]:
            parts = f[5].split()
            compat = parts[0].startswith("<")
            if compat:
                parts = parts[1:]
            decomp[cp] = (compat, [int(x, 16) for x in parts])
        if f[12]:
            upper[cp] = int(f[12], 16)
        if f[13]:
            lower[cp] = int(f[13], 16)

    props = {}                          # name -> merged ranges
    cats = sorted(set(gc.values()))
    for cat in cats:
        props[cat] = to_ranges(c for c, v in gc.items() if v == cat)
    props["Cn"] = merge((c, c) for c in range(MAX + 1) if c not in gc)

    binary = ["ASCII_Hex_Digit", "Alphabetic", "Bidi_Control", "Case_Ignorable", "Cased", "Dash",
              "Default_Ignorable_Code_Point", "Deprecated", "Diacritic", "Emoji", "Emoji_Component",
              "Emoji_Modifier", "Emoji_Modifier_Base", "Emoji_Presentation", "Extended_Pictographic",
              "Extender", "Grapheme_Extend", "Hex_Digit", "IDS_Binary_Operator", "IDS_Trinary_Operator",
              "ID_Continue", "ID_Start", "Ideographic", "Join_Control", "Logical_Order_Exception",
              "Lowercase", "Math", "Noncharacter_Code_Point", "Pattern_Syntax", "Pattern_White_Space",
              "Quotation_Mark", "Radical", "Regional_Indicator", "Sentence_Terminal", "Soft_Dotted",
              "Terminal_Punctuation", "Unified_Ideograph", "Uppercase", "Variation_Selector", "White_Space",
              "XID_Continue", "XID_Start"]
    found = {name: [] for name in binary}
    for name in ("PropList.txt", "DerivedCoreProperties.txt", "emoji-data.txt"):
        for lo, hi, f in ucd(path(name)):
            if f[0] in found and len(f) == 1:
                found[f[0]].append((lo, hi))
    for name in binary:
        assert found[name], name
        props[name] = merge(found[name])

    scripts = {}
    for lo, hi, f in ucd(path("Scripts.txt")):
        scripts.setdefault(f[0], []).append((lo, hi))
    for name, r in scripts.items():
        props["sc:" + name] = merge(r)

    # Other names: categories' and scripts' long and short names, properties' short ones.
    aliases = {}
    for line in open(path("PropertyValueAliases.txt"), encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        if f[0] == "gc":
            canon = f[1]
            for alias in f[2:]:
                if alias and alias != canon:
                    aliases[alias] = canon
        elif f[0] == "sc":
            canon = f[2]
            for alias in [f[1]] + f[3:]:
                if alias and alias != canon and "sc:" + canon in props:
                    aliases["sc:" + alias] = "sc:" + canon
    for line in open(path("PropertyAliases.txt"), encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        if f[1] in binary:
            for alias in [f[0]] + f[2:]:
                if alias != f[1]:
                    aliases[alias] = f[1]
    # Groups of categories (L, LC, ...) are unions the C code makes from these.
    groups = {"L": "Lu Ll Lt Lm Lo", "LC": "Lu Ll Lt", "M": "Mn Mc Me", "N": "Nd Nl No",
              "P": "Pc Pd Ps Pe Pi Pf Po", "S": "Sm Sc Sk So", "Z": "Zs Zl Zp", "C": "Cc Cf Cs Co Cn"}
    aliases = {k: v for k, v in aliases.items() if v in props or v in groups}

    # ---- case ----
    folding = {}
    for lo, hi, f in ucd(path("CaseFolding.txt")):
        if f[0] in ("C", "S"):
            folding[lo] = int(f[1], 16)
    classes = {}
    for c, t in folding.items():
        classes.setdefault(t, {t}).add(c)
    orbit = {}
    for members in classes.values():
        m = sorted(members)
        for i, c in enumerate(m):
            orbit[c] = m[(i + 1) % len(m)]
    special = []
    for lo, hi, f in ucd(path("SpecialCasing.txt")):
        if len(f) > 4 and f[3]:
            continue                    # conditional (Final_Sigma, languages): in code
        low, up = [int(x, 16) for x in f[0].split()], [int(x, 16) for x in f[2].split()]
        if len(low) > 1 or len(up) > 1:
            special.append((lo, low if len(low) > 1 else [], up if len(up) > 1 else []))

    # ---- normalization ----
    excluded = set()
    for lo, hi, f in ucd(path("DerivedNormalizationProps.txt")):
        if f[0] == "Full_Composition_Exclusion":
            excluded.update(range(lo, hi + 1))

    out = []
    w = out.append
    w("/* Generated by tools/mkunicode.py from the Unicode Character Database 16.0.0. Do not edit. */")
    w("#include <stdint.h>")
    w("")

    # ranges and properties
    flat, entries = [], []
    def add_ranges(rs):
        start = len(flat)
        for lo, hi in rs:
            while hi - lo >= 2048:
                flat.append(lo | (2047 << 21))
                lo += 2048
            flat.append(lo | ((hi - lo) << 21))
        return start, len(flat) - start
    for name in sorted(props):
        entries.append((name, *add_ranges(props[name])))
    w("static const uint32_t uc_ranges[%d] = {" % len(flat))
    for i in range(0, len(flat), 8):
        w("    " + ", ".join("0x%08x" % v for v in flat[i:i + 8]) + ",")
    w("};")
    w("")
    w("struct uc_prop { const char *name; uint16_t first, count; };")
    w("static const struct uc_prop uc_props[%d] = {   /* sorted by name; scripts are \"sc:Name\" */" % len(entries))
    for name, start, count in entries:
        w('    { "%s", %d, %d },' % (name, start, count))
    w("};")
    w("")
    w("struct uc_alias { const char *name, *canon; };")
    items = sorted(aliases.items())
    w("static const struct uc_alias uc_aliases[%d] = {    /* sorted by name */" % len(items))
    for k, v in items:
        w('    { "%s", "%s" },' % (k, v))
    w("};")
    w("")
    w("struct uc_group { const char *name, *members; };")
    w("static const struct uc_group uc_groups[%d] = {" % len(groups))
    for k, v in sorted(groups.items()):
        w('    { "%s", "%s" },' % (k, v))
    w("};")
    w("")

    # case runs
    def runs(mapping):
        keys = sorted(mapping)
        out, i = [], 0
        while i < len(keys):
            c = keys[i]
            delta = mapping[c] - c
            best = (1, 1)
            for stride in (1, 2):
                n = 1
                while (i + n < len(keys) and keys[i + n] == c + n * stride and
                       mapping[keys[i + n]] - keys[i + n] == delta and n < 65535):
                    n += 1
                if n > best[0]:
                    best = (n, stride)
            out.append((c, best[0], best[1], delta))
            i += best[0]
        return out
    w("struct uc_run { uint32_t lo; uint16_t count; uint8_t stride; int32_t delta; };")
    for name, mapping in (("upper", upper), ("lower", lower), ("fold", folding), ("orbit", orbit)):
        r = runs(mapping)
        w("static const struct uc_run uc_%s_runs[%d] = {" % (name, len(r)))
        for i in range(0, len(r), 4):
            w("    " + " ".join("{ 0x%x, %d, %d, %d }," % x for x in r[i:i + 4]))
        w("};")
    w("")
    w("struct uc_special { uint32_t cp; uint32_t lower[3], upper[3]; };")
    w("static const struct uc_special uc_special[%d] = {" % len(special))
    for cp, low, up in sorted(special):
        pad = lambda l: ", ".join("0x%x" % x for x in (l + [0, 0, 0])[:3])
        w("    { 0x%x, { %s }, { %s } }," % (cp, pad(low), pad(up)))
    w("};")
    w("")

    # combining classes
    cr = []
    for c in sorted(ccc):
        if cr and cr[-1][1] == c - 1 and cr[-1][2] == ccc[c]:
            cr[-1][1] = c
        else:
            cr.append([c, c, ccc[c]])
    w("struct uc_ccc { uint32_t lo; uint16_t count; uint8_t ccc; };")
    w("static const struct uc_ccc uc_ccc[%d] = {" % len(cr))
    for i in range(0, len(cr), 4):
        w("    " + " ".join("{ 0x%x, %d, %d }," % (lo, hi - lo + 1, v) for lo, hi, v in cr[i:i + 4]))
    w("};")
    w("")

    # decompositions, the characters as UTF-16
    text, keys = [], []
    for cp in sorted(decomp):
        compat, parts = decomp[cp]
        units = []
        for p in parts:
            if p >= 0x10000:
                p -= 0x10000
                units += [0xD800 + (p >> 10), 0xDC00 + (p & 0x3FF)]
            else:
                units.append(p)
        assert len(parts) < 32
        keys.append((cp | len(parts) << 21 | int(compat) << 26 | int(cp in excluded) << 27, len(text)))
        text += units
    assert len(text) < 65536
    w("static const uint32_t uc_decomp[%d] = {" % len(keys))
    for i in range(0, len(keys), 8):
        w("    " + ", ".join("0x%08x" % k for k, _ in keys[i:i + 8]) + ",")
    w("};")
    w("static const uint16_t uc_decomp_at[%d] = {" % len(keys))
    for i in range(0, len(keys), 12):
        w("    " + ", ".join(str(o) for _, o in keys[i:i + 12]) + ",")
    w("};")
    w("static const uint16_t uc_decomp_text[%d] = {" % len(text))
    for i in range(0, len(text), 12):
        w("    " + ", ".join("0x%04x" % u for u in text[i:i + 12]) + ",")
    w("};")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
