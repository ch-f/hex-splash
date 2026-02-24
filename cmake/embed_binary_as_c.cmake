# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
  message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED SYMBOL OR SYMBOL STREQUAL "")
  message(FATAL_ERROR "SYMBOL is required")
endif()

if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "Input file does not exist: ${INPUT}")
endif()

get_filename_component(_input_name "${INPUT}" NAME)

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hex_len)

if(_hex_len EQUAL 0)
  set(_body "\n")
else()
  math(EXPR _byte_count "${_hex_len} / 2")
  math(EXPR _last_byte_index "${_byte_count} - 1")

  set(_body "")
  foreach(_i RANGE 0 ${_last_byte_index})
    math(EXPR _col "${_i} % 12")
    if(_col EQUAL 0)
      string(APPEND _body "\n  ")
    else()
      string(APPEND _body " ")
    endif()

    math(EXPR _off "${_i} * 2")
    string(SUBSTRING "${_hex}" ${_off} 2 _byte)
    string(APPEND _body "0x${_byte}")
    if(NOT _i EQUAL _last_byte_index)
      string(APPEND _body ",")
    endif()
  endforeach()
  string(APPEND _body "\n")
endif()

file(WRITE "${OUTPUT}" "/* Auto-generated from ${_input_name}. */\n#include <stddef.h>\n\nconst unsigned char ${SYMBOL}[] = {${_body}};\nconst size_t ${SYMBOL}_len = sizeof(${SYMBOL});\n")
