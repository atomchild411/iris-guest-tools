product iristools
    id "IRIS guest tools: host OpenGL for IRIX under the IRIS emulator"
    image sw
        id "IRIS guest tools Software"
        version VERSION
        order 9999
        subsys base default
            id "OpenGL and IRIS GL libraries that draw on the IRIS host's GPU"
            replaces self
            exp IRISTOOLS_BASE
            prereq (
                eoe.sw.base 1289434520 maxint
            )
        endsubsys
    endimage
endproduct
