project(dec7z C)

add_library(dec7z STATIC
        7zAlloc.c
        7zArcIn.c
        7zBuf.c
        7zBuf2.c
        7zCrc.c
        7zCrcOpt.c
        7zDec.c
        7zFile.c
        7zStream.c
        Bcj2.c
        Bra.c
        Bra86.c
        BraIA64.c
        CpuArch.c
        Delta.c
        Lzma2Dec.c
        LzmaDec.c
        Ppmd7.c
        Ppmd7Dec.c)

set_target_properties(dec7z PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dec7z PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dec7z PRIVATE
        Z7_PPMD_SUPPORT
        Z7_EXTRACT_ONLY
        _FILE_OFFSET_BITS=64)
target_compile_options(dec7z PRIVATE -O2 -fvisibility=hidden -w)
