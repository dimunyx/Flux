cmake_minimum_required(VERSION 3.28)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FLUX_VERSION LIMIT_COUNT 1)
string(STRIP "${FLUX_VERSION}" FLUX_VERSION)
if(NOT FLUX_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as 0.1.0")
endif()

project(Flux
    VERSION ${FLUX_VERSION}
    DESCRIPTION "One interface. Every package manager."
    LANGUAGES CXX
)

include(GNUInstallDirs)

# Release is the default build type (matches the Justfile).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)

option(FLUX_NATIVE   "Tune the build for the host CPU (-march=native)" ON)
option(FLUX_WERROR   "Treat compiler warnings as errors" OFF)
option(FLUX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "Flux is tuned for Clang, but ${CMAKE_CXX_COMPILER_ID} is being used. "
        "Clang-specific flags (thin LTO) will be skipped.")
endif()

# CONFIGURE_DEPENDS makes the build re-glob when files are added or removed.
file(GLOB_RECURSE FLUX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

if(NOT FLUX_SOURCES)
    message(FATAL_ERROR
        "No C++ sources found in ${CMAKE_CURRENT_SOURCE_DIR}/src. "
        "Add at least one .cpp file (for example src/main.cpp).")
endif()

add_executable(flux ${FLUX_SOURCES})
add_executable(Flux::flux ALIAS flux)

target_include_directories(flux PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
)

find_package(OpenGL REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(Freetype REQUIRED)
find_package(PNG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
target_link_libraries(flux PRIVATE
    OpenGL::GL
    glfw
    Freetype::Freetype
    PNG::PNG
    PkgConfig::EPOXY
)
target_compile_definitions(flux PRIVATE
    FLUX_ASSET_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/src/assets\"
    FLUX_INSTALL_ASSET_DIR=\"${CMAKE_INSTALL_FULL_DATADIR}/flux/assets\"
    FLUX_VERSION=\"${PROJECT_VERSION}\"
)

set_target_properties(flux PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_compile_options(flux PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wformat=2
    -Wundef
    -Wcast-align
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wdouble-promotion
    -pipe
    $<$<BOOL:${FLUX_WERROR}>:-Werror>
    $<$<CONFIG:Release>:-O3>
)

if(FLUX_NATIVE)
    target_compile_options(flux PRIVATE -march=native)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(flux PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    target_link_options(flux PRIVATE $<$<CONFIG:Release>:-flto=thin>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(flux PRIVATE $<$<CONFIG:Release>:-flto>)
    target_link_options(flux PRIVATE $<$<CONFIG:Release>:-flto>)
endif()

if(FLUX_SANITIZE)
    target_compile_options(flux PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(flux PRIVATE -fsanitize=address,undefined)
endif()

install(TARGETS flux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY src/assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/flux/assets)

set(CPACK_PACKAGE_NAME "flux")
set(CPACK_PACKAGE_VENDOR "Flux contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight Linux software center")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "flux-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_CONTACT "Flux contributors")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Flux contributors")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
include(CPack)

include(CTest)
