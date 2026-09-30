#!/usr/bin/env python3
"""Generate the parts of the IRIS GL shim that are not written by hand.

  python3 tools/irisglshim.py API_JSON OUT_C

Reads API_JSON -- the declarations of IRIX's /usr/include/gl/gl.h, which
tools/irisapi_from_gl_h.py extracts on each build from the build host's own
copy -- and writes OUT_C (irisgl_stubs.c): one definition for every entry
point that is not hand-written, each saying so once. build.sh runs both into
build/; neither the header nor anything made from it is kept in the
repository.

The stubs are the point, not an afterthought. A shim that defines only what it
implements is worse than no shim: rld resolves the rest from SGI's real
libgl.so, which initialises, asks the kernel to enumerate graphics boards and
fails with "Can't determine board count" -- the wall the shim exists to get
past. Defining all of them keeps the real library out of the process
entirely, and a program that reaches an unimplemented call says which one
instead of dying somewhere else.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Written by hand in irisgl_rt.c (window, configuration and the event queue)
# and irisgl_draw.c (everything that turns into OpenGL).
HANDWRITTEN = {
    # Exactly what `atlantis` imports, which is the first whole program the
    # shim aims at. The list grows one program at a time: a name in here that
    # is not defined in the hand-written sources is a link error, which is the
    # right way round -- it cannot silently fall through to SGI's library.
    #
    # --- window, configuration and the event queue (irisgl_rt.c) ---
    "winopen", "noborder", "prefposition", "foreground", "reshapeviewport",
    "RGBmode", "doublebuffer", "gconfig", "getgconfig", "getgdesc", "gversion",
    "mssize", "zbsize", "subpixel", "swapbuffers",
    "qdevice", "qread", "qtest", "getbutton", "getvaluator",
    # --- drawing and state (irisgl_draw.c) ---
    "clear", "zclear", "zbuffer", "lsetdepth", "backface", "cpack",
    "bgnpolygon", "endpolygon", "v3f", "n3f",
    "mmode", "perspective", "ortho", "ortho2", "translate", "rot", "scale",
    "pushmatrix", "popmatrix", "loadmatrix", "getmatrix",
    "lmdef", "lmbind", "tevdef", "tevbind", "texdef2d", "texbind", "texgen",
    # --- what the demos on this image asked for ---
    "ortho", "ortho2", "rect", "rectf", "pnt", "move2i", "draw2i",
    # The integer and short spellings of rect: a program working in pixels
    # reaches for these rather than the float ones. `jot` stopped at recti.
    "recti", "rects", "rectfi", "rectfs",
    "bgnline", "endline", "bgnclosedline", "endclosedline",
    "bgnpoint", "endpoint", "bgntmesh", "endtmesh", "swaptmesh",
    "bgnqstrip", "endqstrip",
    "v2f", "v2i", "v2s", "v4f", "c3f", "c4f", "t2f", "t2i", "t2s",
    "RGBcolor", "color", "mapcolor", "czclear",
    "blendfunction", "zfunction", "shademodel", "dither",
    "linewidth", "pntsize", "linesmooth", "pntsmooth", "polysmooth",
    "multmatrix", "rotate",
    "genobj", "makeobj", "closeobj", "callobj", "delobj", "isobj",
    "getsize", "getorigin", "viewport",
    "keepaspect", "iconsize", "icontitle", "winconstraints", "glcompat",
    "noise", "tie", "setvaluator", "unqdevice",
    "curorigin", "defcursor", "setcursor", "curstype", "cursoff", "curson",
    "overlay", "underlay", "drawmode", "swapinterval",
    "defpup", "newpup", "addtopup", "setpup", "freepup", "dopup",
    "gflush", "finish",
    # --- what flight asked for: text, the pen in every spelling, patterns,
    #     fog, and the window queries ---
    "move2", "move2s", "move", "rmv2", "draw2", "draw2s", "draw", "rdr2",
    "cmov", "cmov2", "cmov2i", "cmov2s", "font", "charstr", "getcpos",
    "strwidth", "getheight", "getdescender",
    "pushviewport", "popviewport", "getviewport",
    "deflinestyle", "setlinestyle", "defpattern", "setpattern",
    "getmcolor", "fogvertex", "frontbuffer", "getplanes", "getdisplaymode",
    "afunction", "pntsizef", "wintitle", "minsize", "winset", "winattach",
    "qgetfd", "qreset", "qenter",
    # --- the pixel calls (irisgl_pixels.c): mandel's writepixels, libfm and
    #     libil's lrectwrite/lrectread, gr_osview's rectcopy ---
    "pixmode", "rectzoom", "readsource", "lrectwrite", "rectwrite", "lrectread",
    "rectread", "rectcopy", "writepixels", "writeRGB", "readpixels", "readRGB",
    "readdisplay",
    # --- and the small calls those programs and libraries import ---
    "prefsize", "winposition", "winpop", "winclose", "backbuffer", "gexit", "cmode",
    "lmcolor", "polf", "pmv", "pdr", "pclos", "pnt2i", "cmovi", "cmovs",
    # --- windows as SGI's library makes them (irisgl_rt.c): every
    #     constraint, several windows, and the GLX mixed model that libSgm's
    #     GL widgets, showcase and iconsmith draw through ---
    "maxsize", "stepunit", "fudge", "noport", "imakebackground", "singlebuffer",
    "swinopen", "winget", "winclose", "scrmask", "getscrmask", "isqueued", "ringbell",
    "GLXgetconfig", "GLXlink", "GLXunlink", "GLXwinset",
    # --- raster fonts (irisgl_font.c): jot and showcase define their own ---
    "defrasterfont", "getfont", "lcharstr", "lstrwidth", "getlwidth",
    # --- the rest (irisgl_extra.c) ---
    "drawmode", "getdrawmode", "getmmode",
    # --- the most-imported stubs, by the importer scan (irisgl_extra.c): the
    #     pen and polygon spellings, circles, arcs and screen boxes, colours and
    #     write masks, viewing, stencil, attributes, and the hardware controls
    #     this display does not have, accepted and ignored ---
    "RGBcursor", "RGBsize", "RGBwritemask", "arc", "arcf", "arcfi", "arcfs", "arci", "arcs",
    "attachcursor", "bbox2", "bbox2i", "bbox2s", "blankscreen", "blanktime", "blink", "c3i",
    "c3s", "c4i", "c4s", "chunksize", "circ", "circf", "circfi", "circfs", "circi", "circs",
    "clipplane", "clkoff", "clkon", "concave", "cyclemap", "depthcue", "dglclose",
    "dglopen", "displacepolygon", "drawi", "draws", "endfullscrn", "frontface", "fullscrn",
    "gRGBcolor", "gRGBmask", "gammaramp", "gbegin", "getbackface", "getbuffer", "getcmmode",
    "getcolor", "getcursor", "getdcm", "getdepth", "getdev", "getgpos", "getlsbackup",
    "getlsrepeat", "getlstyle", "getmap", "getmonitor", "getmultisample", "getothermonitor",
    "getpattern", "getport", "getresetls", "getshade", "getvideo", "getwritemask",
    "getwscrn", "getzbuffer", "ginit", "greset", "gsync", "ismex", "lampoff", "lampon",
    "leftbuffer", "linewidthf", "logicop", "lookat", "lsbackup", "lsrepeat", "monobuffer",
    "movei", "moves", "msalpha", "msmask", "mspattern", "mssample", "mswapbuffers",
    "multimap", "multisample", "nmode", "normal", "onemap", "pagecolor", "pdr2", "pdr2i",
    "pdr2s", "pdri", "pdrs", "pmv2", "pmv2i", "pmv2s", "pmvi", "pmvs", "pnt2", "pnt2s",
    "pnti", "pnts", "polarview", "polf2", "polf2i", "polf2s", "polfi", "polfs", "poly",
    "poly2", "poly2i", "poly2s", "polyi", "polymode", "polys", "popattributes",
    "pushattributes", "rdr", "rdr2i", "rdr2s", "rdri", "rdrs", "resetls", "rightbuffer",
    "rmv", "rmv2i", "rmv2s", "rmvi", "rmvs", "rpdr", "rpdr2", "rpdr2i", "rpdr2s", "rpdri",
    "rpdrs", "rpmv", "rpmv2", "rpmv2i", "rpmv2s", "rpmvi", "rpmvs", "sbox", "sboxf",
    "sboxfi", "sboxfs", "sboxi", "sboxs", "sclear", "screenspace", "scrnattach",
    "scrnselect", "setbell", "setdblights", "setmap", "setmonitor", "setshade", "setvideo",
    "smoothline", "spclos", "splf", "splf2", "splf2i", "splf2s", "splfi", "splfs",
    "stencil", "stensize", "stereobuffer", "swritemask", "t2d", "t3d", "t3f", "t3i", "t3s",
    "t4d", "t4f", "t4i", "t4s", "textcolor", "textinit", "textport", "tpoff", "tpon", "v2d",
    "v3d", "v3i", "v3s", "v4d", "v4i", "v4s", "videocmd", "window", "winmove", "winpush",
    "wmpack", "writemask", "zwritemask",
    # --- NURBS curves and surfaces, through GLU (irisgl_nurbs.c): powerflip ---
    "bgnsurface", "endsurface", "nurbssurface", "bgntrim", "endtrim", "pwlcurve",
    "nurbscurve", "bgncurve", "endcurve", "setnurbsproperty", "getnurbsproperty",
}

# The three varargs entry points: a stub still has to match the declaration, so
# each is written out rather than generated from the parameter list.
VARARGS = {
    "callfunc": "void callfunc(__PFV_ a0, long a1, ...)",
}


def c_type(p, typedefs):
    """The parameter as C, for a definition that has to match the header.

    An array parameter decays to a pointer, and only the *first* dimension
    does: `const Coord[4][3]` is a pointer to rows of three, which has to be
    written that way or the definition does not match the declaration.
    """
    bare = p["type"].replace("const ", "")
    t = typedefs.get(bare, bare)
    const = "const " if p["type"].startswith("const") else ""
    dims = p.get("dims") or []
    if len(dims) > 1:
        inner = "".join(f"[{d}]" for d in dims[1:] if d)
        return f"{const}{t} (*%s){inner}"      # %s is where the name goes
    return const + t + "*" * p["ptr"] + ("*" if p.get("array") else "")


def main():
    api = json.load(open(sys.argv[1]))
    td = api["typedefs"]
    out = [
        "/* Generated by tools/irisglshim.py from the build host's gl.h -- do not edit. */",
        '#include "irisgl_shim.h"',
        "",
    ]
    n = 0
    for fn in api["functions"]:
        name = fn["name"]
        if name in HANDWRITTEN:
            continue
        ret = td.get(fn["ret"], fn["ret"])
        if name in VARARGS:
            sig = VARARGS[name]
        else:
            parts = []
            for i, p in enumerate(fn["params"]):
                ct = c_type(p, td)
                parts.append(ct % f"a{i}" if "%s" in ct else f"{ct} a{i}")
            args = ", ".join(parts) or "void"
            sig = f"{ret} {name}({args})"
        body = ["{", f'\thgl_irisgl_missing("{name}");']
        if ret.replace(" ", "") not in ("void",):
            body.append("\treturn 0;")
        body.append("}")
        out.append(sig)
        out += body
        out.append("")
        n += 1
    path = sys.argv[2]
    open(path, "w").write("\n".join(out))
    total = len(api["functions"])
    print(f"{total} entry points: {total - n} written by hand, {n} stubbed ({path})")
    described = {f["name"] for f in api["functions"]}
    unknown = sorted(HANDWRITTEN - described)
    if unknown:
        print(f"named as hand-written but not in the header: {' '.join(unknown)}")


main()
