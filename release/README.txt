IRIS guest tools -- host OpenGL for IRIX under the IRIS emulator

These are replacement OpenGL (libGL.so) and IRIS GL (libgl.so) libraries for
IRIX 6.5. Under IRIS, OpenGL and IRIS GL programs draw on the host
computer's GPU instead of the emulated graphics board. VERSION says which
IRIS this release needs: its "host GL protocol" must match IRIS's, or the
libraries say so and draw nothing.

On a real SGI machine, or an IRIS without host GL, the libraries find no
host and every GL program draws nothing: keep SGI's libraries there (or
undo the install, below).

Install, as root, from this directory:

    ./install.sh          switch IRIX's GL libraries to these
    ./install.sh -s       show where everything points; change nothing
    ./install.sh -u       switch back to SGI's libraries

install.sh records SGI's setup the first time it runs and never overwrites
that record, so -u always puts SGI's libraries back. It changes only the
links in /var/arch/lib32 (n32) and /var/arch/lib (o32); the libraries go to
/usr/local/iris-tools.

Check it, as a user logged in to the desktop:

    /usr/local/iris-tools/bin/n32/glcheck      frames drawn and read back
    /usr/local/iris-tools/bin/n32/gltest       OpenGL and GLX checks
    /usr/local/iris-tools/bin/n32/irisgltest   IRIS GL checks
    /usr/local/iris-tools/bin/n32/glbench      timings

Each prints "ok"/"FAIL" per check. IRIS_GL_DEBUG=1 makes the library say
what it is doing on stderr.

Source, license (BSD 3-Clause) and how to build:
https://github.com/atomchild411/iris-guest-tools
