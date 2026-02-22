# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.16)
project(hex-splash VERSION 0.11 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Build mode: multi-config (${CMAKE_CONFIGURATION_TYPES})")
else()
  message(STATUS "Build mode: ${CMAKE_BUILD_TYPE}")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/configure_build_info_header.cmake")
hexsplash_configure_build_info_header(
  "${CMAKE_CURRENT_SOURCE_DIR}/config-cmake.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/config-cmake.h"
)

option(BUILD_STATIC "Build a statically linked binary" ON)

if(BUILD_STATIC)
  # Encourage pkg-config to provide static link flags when available.
  set(PKG_CONFIG_USE_STATIC_LIBS ON)
endif()

find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
include(CheckIPOSupported)
pkg_search_module(PNG REQUIRED libpng libpng16)
pkg_search_module(UBOOTENV REQUIRED libubootenv)
pkg_search_module(YAML REQUIRED yaml-0.1 yaml)

# ---- Embed built-in logo (logo.png -> embedded_logo.c) ----
set(HEX_SPLASH_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary_as_c.cmake")
set(HEX_SPLASH_EMBEDDED_LOGO_C "${CMAKE_CURRENT_BINARY_DIR}/embedded_logo.c")

add_custom_command(
        OUTPUT "${HEX_SPLASH_EMBEDDED_LOGO_C}"
        COMMAND "${CMAKE_COMMAND}"
        "-DINPUT:FILEPATH=${CMAKE_CURRENT_SOURCE_DIR}/logo.png"
        "-DOUTPUT:FILEPATH=${HEX_SPLASH_EMBEDDED_LOGO_C}"
        "-DSYMBOL:STRING=hex_splash_default_logo_png"
        -P "${HEX_SPLASH_EMBED_SCRIPT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/logo.png" "${HEX_SPLASH_EMBED_SCRIPT}"
        VERBATIM
)

add_executable(hex-splash
        hex-splash.c
        splash_util.c
        splash_image.c
        splash_framebuffer.c
        ubootenv_logo_source.c
        "${HEX_SPLASH_EMBEDDED_LOGO_C}"
)

option(ENABLE_LTO "Enable interprocedural optimization (LTO) for release builds" ON)
if(ENABLE_LTO)
  check_ipo_supported(RESULT HEXSPL_IPO_SUPPORTED OUTPUT HEXSPL_IPO_MSG LANGUAGES C)
  if(HEXSPL_IPO_SUPPORTED)
    set_target_properties(hex-splash PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE
    )
    message(STATUS "LTO: enabled for Release/RelWithDebInfo/MinSizeRel")
  else()
    message(WARNING "LTO requested but not supported: ${HEXSPL_IPO_MSG}")
  endif()
else()
  message(STATUS "LTO: disabled")
endif()

target_compile_options(hex-splash PRIVATE -Wall -Wextra -Wpedantic)

# Smaller binary (useful for initramfs)
target_compile_options(hex-splash PRIVATE -ffunction-sections -fdata-sections)
target_link_options(hex-splash PRIVATE -Wl,--gc-sections)

target_include_directories(hex-splash PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PNG_INCLUDE_DIRS}
        ${UBOOTENV_INCLUDE_DIRS}
        ${YAML_INCLUDE_DIRS}
)

# Helper: require a static archive when BUILD_STATIC=ON.
function(hex_require_static_lib out_var)
  set(oneValueArgs MESSAGE)
  set(multiValueArgs NAMES HINTS)
  cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(_save_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

  find_library(${out_var}
          NAMES ${ARG_NAMES}
          HINTS ${ARG_HINTS}
          NO_DEFAULT_PATH
  )
  if(NOT ${out_var})
    find_library(${out_var} NAMES ${ARG_NAMES})
  endif()

  set(CMAKE_FIND_LIBRARY_SUFFIXES "${_save_suffixes}")

  if(NOT ${out_var})
    message(FATAL_ERROR "${ARG_MESSAGE}")
  endif()

  set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

if(BUILD_STATIC)
  # Friendly checks to avoid confusing "missing .so" link errors later.
  hex_require_static_lib(HEX_SPLASH_PNG_A
          NAMES png16 png
          HINTS ${PNG_STATIC_LIBRARY_DIRS} ${PNG_LIBRARY_DIRS}
          MESSAGE "BUILD_STATIC=ON but no static libpng (libpng*.a) was found."
  )
  hex_require_static_lib(HEX_SPLASH_UBOOTENV_A
          NAMES ubootenv
          HINTS ${UBOOTENV_STATIC_LIBRARY_DIRS} ${UBOOTENV_LIBRARY_DIRS}
          MESSAGE "BUILD_STATIC=ON but no static libubootenv (libubootenv.a) was found."
  )
  hex_require_static_lib(HEX_SPLASH_YAML_A
          NAMES yaml
          HINTS ${YAML_STATIC_LIBRARY_DIRS} ${YAML_LIBRARY_DIRS} ${UBOOTENV_STATIC_LIBRARY_DIRS} ${UBOOTENV_LIBRARY_DIRS}
          MESSAGE "BUILD_STATIC=ON but no static libyaml (libyaml.a) was found."
  )

  target_link_directories(hex-splash PRIVATE
          ${PNG_STATIC_LIBRARY_DIRS}
          ${UBOOTENV_STATIC_LIBRARY_DIRS}
          ${YAML_STATIC_LIBRARY_DIRS}
  )

  target_link_libraries(hex-splash PRIVATE
          ${PNG_STATIC_LIBRARIES}
          ${UBOOTENV_STATIC_LIBRARIES}
          ${HEX_SPLASH_YAML_A}
  )

  target_link_options(hex-splash PRIVATE
          ${PNG_STATIC_LDFLAGS_OTHER}
          ${UBOOTENV_STATIC_LDFLAGS_OTHER}
          -static
  )

  target_compile_options(hex-splash PRIVATE
          ${PNG_STATIC_CFLAGS_OTHER}
          ${UBOOTENV_STATIC_CFLAGS_OTHER}
  )
else()
  target_link_directories(hex-splash PRIVATE
          ${PNG_LIBRARY_DIRS}
          ${UBOOTENV_LIBRARY_DIRS}
          ${YAML_LIBRARY_DIRS}
  )

  # Link YAML explicitly even for dynamic builds; avoids relying on transitive flags.
  target_link_libraries(hex-splash PRIVATE
          ${PNG_LIBRARIES}
          ${UBOOTENV_LIBRARIES}
          ${YAML_LIBRARIES}
  )

  target_link_options(hex-splash PRIVATE
          ${PNG_LDFLAGS_OTHER}
          ${UBOOTENV_LDFLAGS_OTHER}
          ${YAML_LDFLAGS_OTHER}
  )

  target_compile_options(hex-splash PRIVATE
          ${PNG_CFLAGS_OTHER}
          ${UBOOTENV_CFLAGS_OTHER}
          ${YAML_CFLAGS_OTHER}
  )
endif()

install(TARGETS hex-splash
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
