file(TO_CMAKE_PATH "${HEXAGON_SDK_ROOT}"   HEXAGON_SDK_ROOT)
file(TO_CMAKE_PATH "${HEXAGON_TOOLS_ROOT}" HEXAGON_TOOLS_ROOT)

if (NOT IS_DIRECTORY "${HEXAGON_SDK_ROOT}")
    message(FATAL_ERROR "Make sure HEXAGON_SDK_ROOT point to the correct Hexagon SDK installation.")
endif()

if (NOT IS_DIRECTORY "${HEXAGON_TOOLS_ROOT}")
    message("Try to read HEXAGON_TOOLS_ROOT from hexagon_sdk.json")
    file(READ "${HEXAGON_SDK_ROOT}/hexagon_sdk.json" HEXAGON_SDK_CONFIG_PATH)
    string(JSON HEXAGON_TOOLS_PATH GET ${HEXAGON_SDK_CONFIG_PATH} "root" "tools" "info" 0 "path")
    message("Found HEXAGON_TOOLS_PATH: ${HEXAGON_TOOLS_PATH}")
    set(HEXAGON_TOOLS_ROOT "${HEXAGON_SDK_ROOT}/${HEXAGON_TOOLS_PATH}")
    file(TO_CMAKE_PATH "${HEXAGON_TOOLS_ROOT}" HEXAGON_TOOLS_ROOT)
    if (NOT IS_DIRECTORY "${HEXAGON_TOOLS_ROOT}")
        message(FATAL_ERROR "Make sure HEXAGON_TOOLS_ROOT point to the correct Hexagon SDK installation.")
    endif()
endif()

message(STATUS "hexagon: using ${HEXAGON_SDK_ROOT} and ${HEXAGON_TOOLS_ROOT} for building libggml-htp skels")

include(${HEXAGON_SDK_ROOT}/build/cmake/hexagon_fun.cmake)
include(ExternalProject)

option(GGML_HEXAGON_USE_MEMPOOL "ggml-hexagon: mempool/FastRPC-invoke AP implementation (single mempool, synchronous invoke); OFF = dspqueue/per-buffer implementation" OFF)
option(GGML_HEXAGON_HTP_DEBUG   "ggml-hexagon: enable HTP debug output" OFF)
option(GGML_HEXAGON_SKIP_SKELS  "ggml-hexagon: skip DSP skel build (prebuilt libggml-htp-*.so deployed to device separately)" OFF)

# Prefer checked-in pre-generated qaic stubs (htp/gen) when present so the
# AP-side build does not require running the Linux-only qaic tool.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen/ggml_htp_stub.c)
    message(STATUS "hexagon: using pre-generated IDL stubs from htp/gen")
    set(GGML_HTP_STUB_SRC     ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen/ggml_htp_stub.c)
    set(GGML_HTP_GEN_INCDIR   ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen)
else()
    set(GGML_HTP_STUB_SRC     ${CMAKE_CURRENT_BINARY_DIR}/ggml_htp_stub.c)
    set(GGML_HTP_GEN_INCDIR   ${CMAKE_CURRENT_BINARY_DIR})
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen/htp_iface_stub.c)
    set(GGML_HTI_STUB_SRC     ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen/htp_iface_stub.c)
else()
    set(GGML_HTI_STUB_SRC     ${CMAKE_CURRENT_BINARY_DIR}/htp_iface_stub.c)
endif()

set(TARGET_NAME ggml-hexagon)

set(GGML_HEXAGON_HTP_CERT  "$ENV{HEXAGON_HTP_CERT}" CACHE PATH "ggml-hexagon: enable HTP library signing using certificate")

if (GGML_HEXAGON_USE_MEMPOOL)
    add_library(ggml_htp OBJECT
        ${GGML_HTP_STUB_SRC})

    set_target_properties(ggml_htp PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_include_directories(ggml_htp PUBLIC
        ${HEXAGON_SDK_ROOT}/incs
        ${HEXAGON_SDK_ROOT}/incs/stddef
        ${HEXAGON_SDK_ROOT}/utils/examples
        ${CMAKE_CURRENT_SOURCE_DIR}/htp
        ${GGML_HTP_GEN_INCDIR}
        ${CMAKE_CURRENT_BINARY_DIR})

    if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen/ggml_htp_stub.c)
        build_idl(htp/ggml_htp.idl ggml_htp)
    endif()

    if (CMAKE_SYSTEM_NAME MATCHES Android)
        target_link_options(ggml_htp PUBLIC -llog -ldl)
    elseif (CMAKE_SYSTEM_NAME MATCHES Windows)
        target_precompile_headers(ggml_htp PUBLIC <sal.h>)
    else()
        target_link_options(ggml_htp PUBLIC -ldl)
    endif()

    ggml_add_backend_library(${TARGET_NAME}
        ggml-hexagon-fastrpc.cpp
        htp-drv.cpp
        htp-drv.h
        libdl.h
        ../../include/ggml-hexagon.h)

    target_include_directories(${TARGET_NAME} PRIVATE
        ${HEXAGON_SDK_ROOT}/incs
        ${HEXAGON_SDK_ROOT}/incs/stddef
        ${CMAKE_CURRENT_SOURCE_DIR}/htp
        ${GGML_HTP_GEN_INCDIR}
        ${CMAKE_CURRENT_BINARY_DIR})

    target_link_libraries(${TARGET_NAME} PRIVATE ggml_htp)
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/htp ${GGML_HTP_GEN_INCDIR} ${CMAKE_CURRENT_BINARY_DIR})

    # Build HTP skels
    set(HTP_SKELS)
    if (NOT GGML_HEXAGON_SKIP_SKELS)
    function(build_htp_skel_mempool V)
        ExternalProject_Add(htp-mempool-${V}
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/htp BUILD_ALWAYS ON
            BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/bin/libggml-htp-${V}.so
            CMAKE_ARGS
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/htp/cmake-toolchain.cmake
                -DCMAKE_INSTALL_LIBDIR=${CMAKE_BINARY_DIR}/bin
                -DHEXAGON_SDK_ROOT=${HEXAGON_SDK_ROOT}
                -DHEXAGON_TOOLS_ROOT=${HEXAGON_TOOLS_ROOT}
                -DHEXAGON_HTP_DEBUG=${GGML_HEXAGON_HTP_DEBUG}
                -DDSP_VERSION=${V}
                -DGGML_HEXAGON_USE_MEMPOOL=ON
                -DPREBUILT_LIB_DIR="toolv19_${V}")
        list(APPEND HTP_SKELS ${CMAKE_CURRENT_BINARY_DIR}/libggml-htp-${V}.so)
        set(HTP_SKELS ${HTP_SKELS} PARENT_SCOPE)
    endfunction()

    build_htp_skel_mempool(v73)
    build_htp_skel_mempool(v75)
    build_htp_skel_mempool(v79)
    build_htp_skel_mempool(v81)
    endif() # NOT GGML_HEXAGON_SKIP_SKELS

    if (EXISTS ${CMAKE_SOURCE_DIR}/scripts/ggml-hexagon.cfg)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/scripts/ggml-hexagon.cfg ${CMAKE_BINARY_DIR}/bin/
            COMMENT "copy ggml-hexagon.cfg")
    endif()
else()
    add_library(htp_iface OBJECT
        ${GGML_HTI_STUB_SRC})

    set_target_properties(htp_iface PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(htp_iface PUBLIC
        ${HEXAGON_SDK_ROOT}/incs
        ${HEXAGON_SDK_ROOT}/incs/stddef
        ${HEXAGON_SDK_ROOT}/utils/examples
        ${CMAKE_CURRENT_SOURCE_DIR}/htp
        ${GGML_HTP_GEN_INCDIR}
        ${CMAKE_CURRENT_BINARY_DIR})

    if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/htp/gen/htp_iface_stub.c)
        build_idl(htp/htp_iface.idl htp_iface)
    endif()

    if (CMAKE_SYSTEM_NAME MATCHES Android)
        target_link_options(htp_iface PUBLIC -llog -ldl)
    elseif (CMAKE_SYSTEM_NAME MATCHES Windows)
        target_precompile_headers(htp_iface PUBLIC <sal.h>)
    else()
        target_link_options(htp_iface PUBLIC -ldl)
    endif()

    ggml_add_backend_library(${TARGET_NAME}
        ggml-hexagon.cpp
        htp-drv.cpp
        htp-drv.h
        libdl.h
        ../../include/ggml-hexagon.h)

    target_link_libraries(${TARGET_NAME} PRIVATE htp_iface)
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/htp ${CMAKE_CURRENT_BINARY_DIR})

    # Build HTP skels
    set(HTP_SKELS)
    function(build_htp_skel V)
        ExternalProject_Add(htp-${V}
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/htp BUILD_ALWAYS ON
            BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/libggml-htp-${V}.so
            CMAKE_ARGS
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/htp/cmake-toolchain.cmake
                -DCMAKE_INSTALL_LIBDIR=${CMAKE_CURRENT_BINARY_DIR}
                -DHEXAGON_SDK_ROOT=${HEXAGON_SDK_ROOT}
                -DHEXAGON_TOOLS_ROOT=${HEXAGON_TOOLS_ROOT}
                -DHEXAGON_HTP_DEBUG=${GGML_HEXAGON_HTP_DEBUG}
                -DDSP_VERSION=${V}
                -DPREBUILT_LIB_DIR="toolv19_${V}")
        list(APPEND HTP_SKELS ${CMAKE_CURRENT_BINARY_DIR}/libggml-htp-${V}.so)
        set(HTP_SKELS ${HTP_SKELS} PARENT_SCOPE)
    endfunction()

    build_htp_skel(v73)
    build_htp_skel(v75)
    build_htp_skel(v79)
    build_htp_skel(v81)

endif()

# Install Hexagon skels required at runtime
if (HTP_SKELS)
    install(FILES ${HTP_SKELS} TYPE LIB)
endif()

if (CMAKE_SYSTEM_NAME MATCHES Windows AND GGML_HEXAGON_HTP_CERT)
    file(TO_CMAKE_PATH "$ENV{WINDOWS_SDK_BIN}/arm64"      WINSDK_BIN0_ARM64)
    file(TO_CMAKE_PATH "$ENV{WINDOWS_SDK_BIN}/x86"        WINSDK_BIN0_X86)
    file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}/arm64" WINSDK_BIN1_ARM64)
    file(TO_CMAKE_PATH "$ENV{WindowsSdkVerBinPath}/x86"   WINSDK_BIN1_X86)

    set(WINSDK_PATHS ${WINSDK_BIN0_ARM64} ${WINSDK_BIN0_X86} ${WINSDK_BIN1_ARM64} ${WINSDK_BIN1_X86})

    find_program(INF2CAT  NAMES inf2cat.exe  PATHS ${WINSDK_PATHS} REQUIRED)
    find_program(SIGNTOOL NAMES signtool.exe PATHS ${WINSDK_PATHS} REQUIRED)

    message(STATUS "hexagon: using ${GGML_HEXAGON_HTP_CERT} to sign libggml-htp skels")

    set(LIBGGML_HTP_CAT ${CMAKE_CURRENT_BINARY_DIR}/libggml-htp.cat)
    add_custom_target(libggml-htp-cat
        BYPRODUCTS ${LIBGGML_HTP_CAT}
        DEPENDS libggml-htp.inf ${HTP_SKELS}
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/libggml-htp.inf ${CMAKE_CURRENT_BINARY_DIR}
        COMMAND ${INF2CAT} /driver:${CMAKE_CURRENT_BINARY_DIR} /os:10_25H2_ARM64
        COMMAND ${SIGNTOOL} sign /fd sha256 /f ${GGML_HEXAGON_HTP_CERT} ${LIBGGML_HTP_CAT}
        COMMENT "generating and signing libggml-htp.cat file"
        VERBATIM
    )

    add_dependencies(${TARGET_NAME} libggml-htp-cat)
    install(FILES ${LIBGGML_HTP_CAT} TYPE LIB)
endif()
