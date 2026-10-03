# IRIS guest tools

Everything we build to run inside IRIX under the
[IRIS](https://github.com/techomancer/iris) emulator, as one suite: the
replacement GL libraries that hand OpenGL and IRIS GL to the host's GPU, the
host call trap, tests and install scripts. It plays the part guest additions
play for other emulators, and is built, versioned and installed on its own,
as IRIX software.

The host half -- the host call and the host GL service, Rust crates
`iris-hostcall` and `iris-hostgl` -- is in IRIS (since 2026-09-30), behind
its `hostgl` feature. Under an IRIS built without it, or on a real SGI
machine, the host call fails with `EINVAL`, and these libraries say host GL
is not available and draw nothing.

## Quick start

You need:

- **IRIS with host GL.** It is not in IRIS's release builds yet: build IRIS
  on a Mac (host GL uses Apple's OpenGL, so macOS only for now), from commit
  `1a93808` (2026-09-30) or later, with `hostgl` (it brings `hostcall` with
  it) and whatever the machine you emulate wants:

  | IRIX machine | `cargo build --release --features ...` | notes |
  |---|---|---|
  | any | `hostgl` | the minimum: GL frames go back to the program, which puts them up through X |
  | Indigo2 IMPACT R10000 (IP28) | `hostgl,jitv2,tcache` | IP28 is in IRIS's default build now; `jitv2` the JIT, `tcache` its fast loads and stores on the R10000; frames composite straight into the IMPACT framebuffer |
  | Indy, other Indigo2 | `hostgl` plus the usual speed features, e.g. `lightning,rex-jit` or `jitv2` | |

  Two things need IRIS changes that are not merged yet: glAccum (IRIS keeps
  the accumulation buffer, since every host drawable is an offscreen
  framebuffer) and the packed pixel types `5_6_5`, `2_3_3_REV` and
  `8_8_8_8_REV` (IRIS stops swapping two of them, which only IRIX 6.5.7's
  `gl.h` had the wrong way round). Without them `gltest` fails those checks
  and everything else works.

  CHD disk images work in IRIS's default build. Tested so far on the IP28
  with IMPACT.
- **IRIX 6.5** running in it, with a desktop you can log in to.
- **A release** from this repository's Releases page. Its `VERSION` names the
  host GL protocol it speaks and the IRIS that speaks it; the library checks
  that when a program starts and says so if they differ.

Then, on the IRIX machine as root:

    gzcat iris-guest-tools-DATE-protoN.tgz | tar xf -
    cd iris-guest-tools-DATE-protoN
    ./install.sh          # switch IRIX's GL libraries to these
    ./install.sh -u       # ...and back to SGI's, any time

and, logged in to the desktop, `/usr/local/iris-tools/bin/n32/glcheck` should
end with `glcheck: PASSED`. `install.sh` records SGI's setup the first time
and only ever changes the links in `/var/arch`; `README.txt` in the release
has the details. On a real SGI machine keep SGI's libraries: these draw
nothing without IRIS.

`release/make-release.sh` makes a release from a checkout (see its header).

## About

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
| `tools/` | the IRIS GL stub generator | none |
| `release/` | `make-release.sh` (a binary release: the libraries, tests, `install.sh`, `VERSION`) and the release's `install.sh` and `README.txt` | none |

Build: `./build.sh [gl|cross|all]`. The n32 GL libraries, the GL tests and
`hostcall_test` also build on a Mac with an LLVM 21 IRIX cross toolchain,
`clang/build.sh [build|check|package|all]`, into a package for `/opt/pkgsrc`
(`clang/install-iris-gl.sh` points IRIX's GL links at it and records SGI's; `-u`
undoes it). Same SONAMEs and exported symbols as the MIPSpro build; the IRIS GL
library loads at 0x500000 instead of 0x480000 (lld pads segments to 64 KB).
`clang/build.sh`'s header has the details, and `clang/common.sh` the paths it
takes from the environment: the toolchain, an IRIX 6.5.7 sysroot, and SGI's
libX11, libXext, libGLU and OpenGL `gl.h` from the same release. The gl step compiles with
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
