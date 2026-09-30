# IRIS guest tools

Everything we build to run inside IRIX under the
[IRIS](https://github.com/techomancer/iris) emulator, as one suite: the
replacement GL libraries that hand OpenGL and IRIS GL to the host's GPU, the
host call trap, tests and install scripts. It plays the part guest additions
play for other emulators, and is built, versioned and installed on its own,
as IRIX software.

The host half -- the host call and the host GL service, Rust crates
`iris-hostcall` and `iris-hostgl` -- lives in our IRIS tree and is not in
upstream IRIS yet. Until it is, these libraries find no host GL and say so:
under an IRIS without it, or on a real SGI machine, the host call fails with
`EINVAL` and OpenGL calls do nothing.

Nothing here is SGI's: no headers, libraries or code of theirs are kept in
this repository. Where the build needs SGI's declarations -- IRIS GL's stubs
are made from `gl.h` -- it takes them from the IRIX build host each time, into
`build/`, which is not versioned.

Everything here is built with MIPSpro on an IRIX host (see `build.sh`), for
the o32, n32 and 64-bit ABIs where a piece applies to programs of each.

Status: the GL libraries are built for o32 and n32; 64-bit needs the wire
protocol to carry 64-bit addresses (in progress).

| directory | what | host counterpart |
|---|---|---|
| `include/` | the wire contract: `hostcall.h` | `iris-hostcall` |
| `hostcall/` | the host call trap (`hostcall_trap.c`, 64-bit arguments, n32 only; `hostcall_trap32.s`, 32-bit, both ABIs) and `hostcall_test.c` | `iris-hostcall` |
| `gl/` | the replacement `libGL.so` (`glshim_*`) and its tests | `iris-hostgl` |
| `irisgl/` | the replacement IRIS GL `libgl.so` (`irisgl_*`) | `iris-hostgl` |
| `xapp/` | `xephyr-app`, which runs a program that needs a newer X server than Xsgi in its own nested Xephyr window, and `fillwm`, the window manager inside it that keeps the program the window's size; packaged on its own as pkgsrc's `x11/xephyr-app` (`clang/build.sh xapp`) | none |
| `tools/` | the IRIS GL stub generator, and `sgidist.py`/`efs.py`, which read files out of SGI's CD images for the cross build | none |

Build: `./build.sh [gl|cross|all]`. The n32 GL libraries, the GL tests and
`hostcall_test` also build on a Mac with an LLVM 21 IRIX cross toolchain,
`clang/build.sh [build|check|package|all]`, into a package for `/opt/pkgsrc`
(`clang/install-iris-gl.sh` points IRIX's GL links at it and records SGI's; `-u`
undoes it). Same SONAMEs and exported symbols as the MIPSpro build; the IRIS GL
library loads at 0x500000 instead of 0x480000 (lld pads segments to 64 KB).
`clang/build.sh`'s header has the details, and `clang/common.sh` the paths it
expects (each can be set from the environment). The gl step compiles with
MIPSpro on a real (or emulated) IRIX host reached through `$IRIX_SSH`, which
needs the IRIS GL and OpenGL development headers (`gl_dev.sw.gldev`); the
cross step uses `irix-gcc`. Everything lands in `build/` (not versioned). The
script's header has the details: outputs, flags, and why the libraries are
linked the way they are. `build/gl/install-gl.sh` installs the libraries in a
guest.

## Generated at build time

- `irisgl_stubs.c`: `tools/irisapi_from_gl_h.py` lists the entry points of the
  build host's `/usr/include/gl/gl.h`, and `tools/irisglshim.py` writes a stub
  for every one not written by hand (its `HANDWRITTEN` list). Both run in
  `build/gl/gen/`.

## Keep in step with the host

These files and their Rust counterparts in IRIS describe one protocol; change
both or neither. IRIS owns the contract, and the host GL handshake checks its
protocol number, so a library and an emulator that disagree refuse each other.

- `gl/glshim_ops.h` and `gl/glshim_gen.c` are **generated in IRIS**, together
  with `iris-hostgl/src/calls.rs`, by `iris-hostgl/tools/glshim.py` from
  `iris-hostgl/tools/glapi.json`, and copied here. Regenerate there; never
  edit by hand.
- `include/hostcall.h` follows the contract in `iris-hostcall/src/lib.rs`.

## License

BSD 3-Clause; see `LICENSE`.

The OpenGL function list behind `gl/glshim_gen.c` and `gl/glshim_ops.h` is
derived from the Khronos OpenGL API registry (`gl.xml`), Copyright The
Khronos Group Inc., licensed under the Apache License 2.0.
