# SPDX-License-Identifier: GPL-2.0-only

function(hexsplash_configure_build_info_header template_path output_path)
  # In multi-config generators, CMAKE_BUILD_TYPE is typically empty.
  set(HEXSPL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
  if(NOT HEXSPL_BUILD_TYPE)
    set(HEXSPL_BUILD_TYPE "MultiConfig")
  endif()

  configure_file("${template_path}" "${output_path}" @ONLY)
endfunction()
