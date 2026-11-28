project(Memory)

file(GLOB INCLUDE_FILES
        include/*.h)

file(GLOB SOURCE_FILES CONFIGURE_DEPENDS
        *.cpp
        include/*.cpp)

add_library(${PROJECT_NAME} INTERFACE)

target_sources(${PROJECT_NAME} INTERFACE
    ${INCLUDE_FILES}
    ${SOURCE_FILES})

set(glosshook_path "${CMAKE_CURRENT_SOURCE_DIR}/lib/${ANDROID_ABI}/libGlossHook.a")
add_library(GlossHook STATIC IMPORTED)
set_target_properties(GlossHook PROPERTIES IMPORTED_LOCATION ${glosshook_path})

target_link_libraries(${PROJECT_NAME} INTERFACE GlossHook)

# Neutralize libGlossHook.a's LinkerInit() (which hooks Bionic's do_dlopen and
# crashes with SIGSEGV on Android 16 / SDK >= 36 due to hardcoded api <= 35 check)
# and fail-loudly guard unused deferred linker-hook entry points.
target_link_options(${PROJECT_NAME} INTERFACE
        -Wl,--wrap,_Z10LinkerInitv
        -Wl,--wrap,GlossHookByName
        -Wl,--wrap,GlossPltHook
        -Wl,--wrap,GlossHookConstructor
        -Wl,--wrap,GlossLinkerHook)

target_include_directories(${PROJECT_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
