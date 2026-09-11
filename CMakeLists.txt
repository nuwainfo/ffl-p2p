cmake_minimum_required(VERSION 3.20)
project(ffl_p2p C CXX)

include(CheckCXXCompilerFlag)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

option(FFL_P2P_FAKE_PLUM "Build against the deterministic test libplum" OFF)
option(FFL_P2P_ENABLE_QUIC "Build the ngtcp2 + GnuTLS QUIC transport" ON)
option(FFL_P2P_ENABLE_DIAGNOSTICS "Compile native QUIC/datapath diagnostics" OFF)
option(FFL_P2P_MANYLINUX "Statically link the GNU C++ runtime for a manylinux wheel" OFF)
set(FFL_P2P_LIBJUICE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjuice" CACHE PATH "Patched libjuice source")
set(FFL_P2P_LIBPLUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libplum" CACHE PATH "libplum source")
set(FFL_P2P_NGTCP2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ngtcp2" CACHE PATH "ngtcp2 1.25.x source")

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(Threads REQUIRED)

if(NOT EXISTS "${FFL_P2P_LIBJUICE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "libjuice source not found: ${FFL_P2P_LIBJUICE_ROOT}")
endif()

set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${FFL_P2P_LIBJUICE_ROOT}" "${CMAKE_BINARY_DIR}/libjuice" EXCLUDE_FROM_ALL)

set(FFL_P2P_PLUM_SOURCES native/Plum.c)
set(FFL_P2P_PLUM_INCLUDE "${FFL_P2P_LIBPLUM_ROOT}/include")
set(FFL_P2P_PLUM_TARGET LibPlum::LibPlumStatic)
if(FFL_P2P_FAKE_PLUM)
    list(APPEND FFL_P2P_PLUM_SOURCES tests/native/FakePlum.c)
    set(FFL_P2P_PLUM_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/include")
    set(FFL_P2P_PLUM_TARGET "")
else()
    if(NOT EXISTS "${FFL_P2P_LIBPLUM_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "libplum source not found: ${FFL_P2P_LIBPLUM_ROOT}")
    endif()
    add_subdirectory("${FFL_P2P_LIBPLUM_ROOT}" "${CMAKE_BINARY_DIR}/libplum" EXCLUDE_FROM_ALL)
    # libjuice and libplum both expose internal addr_* symbols from their
    # static archives. Give libplum's private helpers an object-level prefix
    # so both libraries can be linked into the extension safely.
    target_compile_definitions(plum-static PRIVATE
        addr_get_len=fflP2PPlumAddrGetLen
        addr_get_host=fflP2PPlumAddrGetHost
        addr_get_port=fflP2PPlumAddrGetPort
        addr_set_port=fflP2PPlumAddrSetPort
        addr_is_any=fflP2PPlumAddrIsAny
        addr_is_local=fflP2PPlumAddrIsLocal
        addr_is_private=fflP2PPlumAddrIsPrivate
        addr_is_public=fflP2PPlumAddrIsPublic
        addr_is_temp_inet6=fflP2PPlumAddrIsTempInet6
        addr_unmap_inet6_v4mapped=fflP2PPlumAddrUnmapInet6V4Mapped
        addr_map_inet6_v4mapped=fflP2PPlumAddrMapInet6V4Mapped
        addr_is_equal=fflP2PPlumAddrIsEqual
        addr_to_string=fflP2PPlumAddrToString
        addr_set=fflP2PPlumAddrSet
        addr_set_binary=fflP2PPlumAddrSetBinary
        addr_resolve=fflP2PPlumAddrResolve
        addr_record_is_equal=fflP2PPlumAddrRecordIsEqual
        addr_record_to_string=fflP2PPlumAddrRecordToString
        current_timestamp=fflP2PPlumCurrentTimestamp
        udp_create_socket=fflP2PPlumUDPCreateSocket
        udp_recvfrom=fflP2PPlumUDPReceiveFrom
        udp_sendto=fflP2PPlumUDPSendTo
        udp_sendto_self=fflP2PPlumUDPSendToSelf
        udp_set_diffserv=fflP2PPlumUDPSetDiffserv
        udp_get_port=fflP2PPlumUDPGetPort
        udp_get_bound_addr=fflP2PPlumUDPGetBoundAddress
        udp_get_local_addr=fflP2PPlumUDPGetLocalAddress
    )
endif()

set(FFL_P2P_NATIVE_SOURCES
    native/P2P.c
    native/Juice.c
    native/datapath/Buffer.cpp
    native/datapath/Datapath.cpp
    native/platform/Platform.cpp
    ${FFL_P2P_PLUM_SOURCES}
)
if(WIN32)
    list(APPEND FFL_P2P_NATIVE_SOURCES native/datapath/DatapathWindows.cpp)
else()
    list(APPEND FFL_P2P_NATIVE_SOURCES
        native/datapath/DatapathPOSIX.cpp
        native/platform/DatagramBatch.cpp
    )
endif()

set(FFL_P2P_QUIC_LIBRARIES "")
set(FFL_P2P_QUIC_LINK_DIRECTORIES "")
set(FFL_P2P_QUIC_LINK_OPTIONS "")
set(FFL_P2P_QUIC_COMPILE_OPTIONS "")
if(FFL_P2P_ENABLE_QUIC)
    if(NOT EXISTS "${FFL_P2P_NGTCP2_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "ngtcp2 source not found: ${FFL_P2P_NGTCP2_ROOT}")
    endif()
    # GnuTLS is static on Windows, so FindGnuTLS alone is insufficient: it
    # omits static transitive libraries such as libtasn1, Nettle, and GMP.
    # Match the validated smoke build and obtain the complete closure from
    # pkgconf, while still building this project's pinned ngtcp2 source.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GNUTLS REQUIRED gnutls>=3.7.5)
    find_path(FFL_P2P_GNUTLS_INCLUDE_DIR gnutls/gnutls.h
        PATHS ${GNUTLS_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(FFL_P2P_GNUTLS_LIBRARY NAMES gnutls
        PATHS ${GNUTLS_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if(NOT FFL_P2P_GNUTLS_INCLUDE_DIR OR NOT FFL_P2P_GNUTLS_LIBRARY)
        message(FATAL_ERROR "pkg-config located GnuTLS but not its static headers or library")
    endif()
    set(GNUTLS_INCLUDE_DIR "${FFL_P2P_GNUTLS_INCLUDE_DIR}" CACHE PATH "" FORCE)
    set(GNUTLS_LIBRARY "${FFL_P2P_GNUTLS_LIBRARY}" CACHE FILEPATH "" FORCE)
    find_package(GnuTLS 3.7.5 REQUIRED)

    # Build only the static ngtcp2 core and GnuTLS crypto helper.  FFL owns the
    # UDP I/O (libjuice); ngtcp2 never opens or owns a socket in this project.
    set(ENABLE_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
    set(ENABLE_GNUTLS ON CACHE BOOL "" FORCE)
    set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_BORINGSSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_PICOTLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_WOLFSSL OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory("${FFL_P2P_NGTCP2_ROOT}" "${CMAKE_BINARY_DIR}/ngtcp2" EXCLUDE_FROM_ALL)

    list(APPEND FFL_P2P_NATIVE_SOURCES
        native/Quic.cpp
        native/core/Operation.cpp
        native/core/TimerWheel.cpp
        native/core/Worker.cpp
        native/ice/JuiceBinding.cpp
        native/quic/Credentials.cpp
        native/quic/Connection.cpp
    )
    list(APPEND FFL_P2P_QUIC_LIBRARIES ngtcp2_static ngtcp2_crypto_gnutls_static)
    if(WIN32)
        # vcpkg supplies static GnuTLS on Windows. Link its full pkg-config
        # closure because MSVC does not load static-library dependencies.
        list(APPEND FFL_P2P_QUIC_LIBRARIES ${GNUTLS_STATIC_LIBRARIES})
        list(APPEND FFL_P2P_QUIC_LINK_DIRECTORIES ${GNUTLS_STATIC_LIBRARY_DIRS})
        set(FFL_P2P_GNUTLS_RAW_LINK_OPTIONS ${GNUTLS_STATIC_LDFLAGS_OTHER})
        set(FFL_P2P_GNUTLS_COMPILE_OPTIONS ${GNUTLS_STATIC_CFLAGS_OTHER})
    else()
        # Homebrew and normal Unix distributions provide dynamic GnuTLS. Its
        # shared library owns the transitive crypto and internationalization
        # dependencies, so do not force their static pkg-config closure here.
        list(APPEND FFL_P2P_QUIC_LIBRARIES ${GNUTLS_LIBRARIES})
        list(APPEND FFL_P2P_QUIC_LINK_DIRECTORIES ${GNUTLS_LIBRARY_DIRS})
        set(FFL_P2P_GNUTLS_RAW_LINK_OPTIONS ${GNUTLS_LDFLAGS_OTHER})
        set(FFL_P2P_GNUTLS_COMPILE_OPTIONS ${GNUTLS_CFLAGS_OTHER})
    endif()

    # Normalize paired linker arguments before passing them to
    # target_link_options(). Conda can express an rpath as "-R <directory>",
    # while Homebrew's GnuTLS can use "-framework <name>". CMake may de-duplicate
    # a standalone -framework option, so keep each framework pair atomic.
    set(FFL_P2P_GNUTLS_LINK_OPTIONS "")
    set(FFL_P2P_GNUTLS_RPATH_PENDING OFF)
    set(FFL_P2P_GNUTLS_FRAMEWORK_PENDING OFF)
    foreach(FFL_P2P_GNUTLS_LINK_OPTION IN LISTS FFL_P2P_GNUTLS_RAW_LINK_OPTIONS)
        if(FFL_P2P_GNUTLS_RPATH_PENDING)
            list(APPEND FFL_P2P_GNUTLS_LINK_OPTIONS
                "-Wl,-rpath,${FFL_P2P_GNUTLS_LINK_OPTION}"
            )
            set(FFL_P2P_GNUTLS_RPATH_PENDING OFF)
        elseif(FFL_P2P_GNUTLS_FRAMEWORK_PENDING)
            list(APPEND FFL_P2P_GNUTLS_LINK_OPTIONS
                "SHELL:-framework ${FFL_P2P_GNUTLS_LINK_OPTION}"
            )
            set(FFL_P2P_GNUTLS_FRAMEWORK_PENDING OFF)
        elseif(FFL_P2P_GNUTLS_LINK_OPTION STREQUAL "-R")
            set(FFL_P2P_GNUTLS_RPATH_PENDING ON)
        elseif(FFL_P2P_GNUTLS_LINK_OPTION MATCHES "^-R(.+)")
            list(APPEND FFL_P2P_GNUTLS_LINK_OPTIONS "-Wl,-rpath,${CMAKE_MATCH_1}")
        elseif(FFL_P2P_GNUTLS_LINK_OPTION STREQUAL "-framework")
            set(FFL_P2P_GNUTLS_FRAMEWORK_PENDING ON)
        else()
            list(APPEND FFL_P2P_GNUTLS_LINK_OPTIONS "${FFL_P2P_GNUTLS_LINK_OPTION}")
        endif()
    endforeach()
    if(FFL_P2P_GNUTLS_RPATH_PENDING)
        message(FATAL_ERROR "GnuTLS pkg-config output ends with an incomplete -R flag")
    endif()
    if(FFL_P2P_GNUTLS_FRAMEWORK_PENDING)
        message(FATAL_ERROR "GnuTLS pkg-config output ends with an incomplete -framework flag")
    endif()

    list(APPEND FFL_P2P_QUIC_LINK_OPTIONS ${FFL_P2P_GNUTLS_LINK_OPTIONS})
    list(APPEND FFL_P2P_QUIC_COMPILE_OPTIONS ${FFL_P2P_GNUTLS_COMPILE_OPTIONS})
endif()

if(FFL_P2P_ENABLE_DIAGNOSTICS)
    list(APPEND FFL_P2P_NATIVE_SOURCES
        native/datapath/DatapathDiagnostics.cpp
    )
    if(FFL_P2P_ENABLE_QUIC)
        list(APPEND FFL_P2P_NATIVE_SOURCES
            native/core/WorkerDiagnostics.cpp
            native/quic/RuntimeDiagnostics.cpp
        )
    endif()
endif()

Python3_add_library(_ffl_p2p MODULE WITH_SOABI
    ${FFL_P2P_NATIVE_SOURCES}
)
target_include_directories(_ffl_p2p PRIVATE
    native
    "${FFL_P2P_LIBJUICE_ROOT}/include"
    "${FFL_P2P_PLUM_INCLUDE}"
)
target_link_libraries(_ffl_p2p PRIVATE
    LibJuice::LibJuiceStatic
    ${FFL_P2P_PLUM_TARGET}
    ${FFL_P2P_QUIC_LIBRARIES}
    Threads::Threads
)
if(FFL_P2P_ENABLE_DIAGNOSTICS)
    target_compile_definitions(_ffl_p2p PRIVATE FFL_P2P_DIAGNOSTICS=1)
endif()
if(FFL_P2P_ENABLE_QUIC)
    target_compile_definitions(_ffl_p2p PRIVATE FFL_P2P_HAVE_QUIC=1)
    target_link_directories(_ffl_p2p PRIVATE ${FFL_P2P_QUIC_LINK_DIRECTORIES})
    target_link_options(_ffl_p2p PRIVATE ${FFL_P2P_QUIC_LINK_OPTIONS})
    target_compile_options(_ffl_p2p PRIVATE ${FFL_P2P_QUIC_COMPILE_OPTIONS})
endif()
if(MSVC)
    target_compile_options(_ffl_p2p PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(_ffl_p2p PRIVATE -Wall -Wextra -Werror)

    check_cxx_compiler_flag(
        "-Wframe-larger-than=32768"
        FFL_P2P_HAS_FRAME_SIZE_WARNING
    )
    if(FFL_P2P_HAS_FRAME_SIZE_WARNING)
        target_compile_options(
            _ffl_p2p PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wframe-larger-than=32768>
        )
    endif()
endif()

if(FFL_P2P_MANYLINUX)
    if(MSVC)
        message(FATAL_ERROR "FFL_P2P_MANYLINUX requires a GNU-compatible Linux compiler")
    endif()
    target_link_options(_ffl_p2p PRIVATE -static-libgcc -static-libstdc++)
endif()
set_target_properties(_ffl_p2p PROPERTIES PREFIX "" OUTPUT_NAME "_ffl_p2p")

install(TARGETS _ffl_p2p
    LIBRARY DESTINATION ffl_p2p
    RUNTIME DESTINATION ffl_p2p
)
