add_executable(sos2-web "${SOS2_ROOT}/programs/browser/web_main.cpp" "${SOS2_ROOT}/programs/browser/web_platform.cpp" "${SOS2_ROOT}/programs/browser/web_content.cpp" "${SOS2_ROOT}/programs/browser/web_input.cpp")
target_sources(sos2-web PRIVATE "${SOS2_ROOT}/programs/browser/http_parser.cpp" "${SOS2_ROOT}/programs/browser/http_client.cpp" "${SOS2_ROOT}/programs/browser/web_network.cpp" "${SOS2_ROOT}/programs/browser/web_navigation.cpp")
target_sources(sos2-web PRIVATE "${SOS2_ROOT}/programs/browser/http2_session.cpp")
target_sources(sos2-web PRIVATE "${SOS2_ROOT}/programs/browser/web_state.cpp" "${SOS2_ROOT}/programs/browser/web_storage.cpp")
target_sources(sos2-web PRIVATE "${SOS2_ROOT}/programs/browser/web_browser.cpp" "${SOS2_ROOT}/programs/browser/web_compositor.cpp")
if (USE_MIMALLOC)
    # Replaces the C library's malloc family with mimalloc (see web_malloc.c).
    target_sources(sos2-web PRIVATE "${SOS2_ROOT}/programs/browser/web_malloc.c")
endif ()
target_link_libraries(sos2-web PRIVATE WebCore JavaScriptCore WTF PAL bmalloc)
target_link_libraries(sos2-web PRIVATE "${SOS2_MBEDTLS}" "${SOS2_MBEDX509}" "${SOS2_MBEDCRYPTO}" ZLIB::ZLIB)
find_library(SOS2_NGHTTP2 nghttp2 REQUIRED)
target_link_libraries(sos2-web PRIVATE "${SOS2_NGHTTP2}")
target_link_libraries(sos2-web PRIVATE SQLite3::SQLite3)
# GPU rasterization and compositing (web_gpu.h, web_gpu_compositor.h): Mesa's
# EGL and OpenGL ES with the SOS2 libdrm shim, built by
# tools/build_native_mesa.py with 32-bit enums, which the GL interfaces never
# pass (ARM32's own enums are short; arm64 has only 32-bit ones). WebCore's own
# GL code links against the same libraries.
target_sources(sos2-web PRIVATE "${SOS2_ROOT}/programs/browser/web_gpu.cpp" "${SOS2_ROOT}/programs/browser/web_gpu_compositor.cpp")
set_source_files_properties("${SOS2_ROOT}/programs/browser/web_gpu.cpp" "${SOS2_ROOT}/programs/browser/web_gpu_compositor.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${SOS2_MESA_INCLUDE_DIRS}"
    COMPILE_DEFINITIONS "EGL_NO_PLATFORM_SPECIFIC_TYPES;SK_GANESH;SK_GL")
target_link_libraries(sos2-web PRIVATE -Wl,--start-group ${SOS2_MESA_LIBRARIES} -Wl,--end-group)
if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    target_link_libraries(sos2-web PRIVATE -Wl,--no-enum-size-warning)
endif ()
target_include_directories(sos2-web PRIVATE ${WebCore_PRIVATE_INCLUDE_DIRECTORIES} ${WebCore_INCLUDE_DIRECTORIES})
target_compile_definitions(sos2-web PRIVATE BUILDING_WEBKIT=1)
set_target_properties(sos2-web PROPERTIES SUFFIX ".elf")
target_link_options(sos2-web PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/bin/sos2-web.map")
# These archives enter through the toolchain's linker flags, which do not make
# them build dependencies. Runtime-only fixes must still relink the browser.
if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
    set(SOS2_LINKER_SCRIPT "${SOS2_ROOT}/sdk/runtime/linker.ld")
else ()
    set(SOS2_LINKER_SCRIPT "${SOS2_ROOT}/sdk/runtime/linker-arm64.ld")
endif ()
set_property(TARGET sos2-web APPEND PROPERTY LINK_DEPENDS
    "${SOS2_NATIVE}/sdk/libsos2.a" "${SOS2_LINKER_SCRIPT}"
    "${CMAKE_SYSROOT}/lib/libc.a" "${CMAKE_SYSROOT}/lib/libm.a"
    "${CMAKE_SYSROOT}/lib/libstdc++.a" ${SOS2_MESA_LIBRARIES})
