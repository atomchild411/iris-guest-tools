#!/usr/bin/env python3
"""irisapi_from_gl_h.py GL_H OUT_JSON -- the IRIS GL API, as data, from a gl.h.

Reads the IRIS GL header of an IRIX development system (/usr/include/gl/gl.h,
fetched by build.sh into build/, never into the repository) and writes the
list of its entry points that irisglshim.py turns into irisgl_stubs.c:

  typedefs   every plain `typedef <type> <Name>;` (not arrays or structs)
  functions  one entry per `extern <ret> <name>( <params> );`, sorted by name:
             its return type and, per parameter, the type (const kept), how
             many `*` follow it, and its array dimensions (`[]` is "")

Only what a declaration says about calling the function is kept: no text,
comments or values from the header. Both this list and the stubs made from it
are build products, regenerated from the build host's own header each build.
"""
import json
import re
import sys

EXTERN = re.compile(r"^extern\s+(.+?)\b(\w+)\s*\(\s*(.*?)\s*\)\s*;")
TYPEDEF = re.compile(r"^typedef\s+([\w\s\*]+?)\s*\b(\w+)\s*;")


def param(i, text):
    text = text.strip()
    if text == "...":
        return {"type": "...", "ptr": 0, "array": 0, "name": "varargs"}
    dims = re.findall(r"\[\s*(\w*)\s*\]", text)
    base = re.sub(r"\[.*\]", "", text).strip()
    ptr = base.count("*")
    base = re.sub(r"\s+", " ", base.replace("*", " ")).strip()
    return {"type": base, "ptr": ptr, "array": len(dims), "dims": dims, "name": f"a{i}"}


def parse(lines):
    typedefs, functions = {}, []
    for line in lines:
        line = line.strip()
        m = TYPEDEF.match(line)
        if m:
            typedefs[m.group(2)] = re.sub(r"\s+", " ", m.group(1)).strip()
            continue
        m = EXTERN.match(line)
        if not m:
            continue
        ret, name, args = m.groups()
        # The return type as the header spells it: a pointer ends at its '*',
        # anything else keeps the one space before the name.
        ret = re.sub(r"\s+", " ", ret)
        ret = ret.rstrip() if ret.rstrip().endswith("*") else ret.rstrip() + " "
        params = [] if args in ("", "void") else [param(i, a) for i, a in enumerate(args.split(","))]
        functions.append({"name": name, "ret": ret, "params": params})
    functions.sort(key=lambda f: f["name"])
    return {"typedefs": typedefs, "functions": functions}


if __name__ == "__main__":
    api = parse(open(sys.argv[1], encoding="latin-1"))
    with open(sys.argv[2], "w") as f:
        json.dump(api, f, indent=1)
    print(f"{len(api['functions'])} entry points, {len(api['typedefs'])} typedefs")
