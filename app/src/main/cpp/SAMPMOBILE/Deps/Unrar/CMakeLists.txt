project(decunrar CXX)

# Exact RARDLL translation unit list from upstream UnRAR makefile
# (helper .cpp files like unpack50.cpp / ulinks.cpp are #included by these units).
add_library(decunrar STATIC
        rar.cpp
        strlist.cpp
        strfn.cpp
        pathfn.cpp
        smallfn.cpp
        global.cpp
        file.cpp
        filefn.cpp
        filcreat.cpp
        archive.cpp
        arcread.cpp
        unicode.cpp
        system.cpp
        crypt.cpp
        crc.cpp
        rawread.cpp
        encname.cpp
        resource.cpp
        match.cpp
        timefn.cpp
        rdwrfn.cpp
        consio.cpp
        options.cpp
        errhnd.cpp
        rarvm.cpp
        secpassword.cpp
        rijndael.cpp
        getbits.cpp
        sha1.cpp
        sha256.cpp
        blake2s.cpp
        hash.cpp
        extinfo.cpp
        extract.cpp
        volume.cpp
        list.cpp
        find.cpp
        unpack.cpp
        headers.cpp
        threadpool.cpp
        rs16.cpp
        cmddata.cpp
        ui.cpp
        largepage.cpp
        filestr.cpp
        scantree.cpp
        dll.cpp
        qopen.cpp)

set_target_properties(decunrar PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)

target_include_directories(decunrar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(decunrar PUBLIC RARDLL SILENT PRIVATE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE UNIX_TIME_NS)
target_compile_options(decunrar PRIVATE -O2 -fexceptions -fvisibility=hidden -w)
