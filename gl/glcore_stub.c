/*
 * Host OpenGL for IRIS: the libGLcore.so installed in place of SGI's.
 *
 * SGI's libGL.so has only GLX; its gl* functions are in libGLcore.so, which
 * drives the graphics board itself, and SGI's linker binds a program's gl*
 * calls to it. IRIX's rld gives a program each function from the first
 * library in its list that has it, so a program listing libGLcore.so before
 * libGL.so would draw through SGI's driver beside the shim's GLX, and crash.
 *
 * This libGLcore.so has no functions: it needs libGL.so (the shim), so the
 * search for each one goes on to the shim, which has them all, whatever the
 * order of the program's list.
 */
int __iris_glcore_stub;
