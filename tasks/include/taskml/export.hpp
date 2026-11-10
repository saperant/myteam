// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
#pragma once

// Symbol visibility for the taskml library.
//
// - When building or consuming the shared library, TASKML_API marks the
//   public symbols (the library is compiled with -fvisibility=hidden).
// - When building or consuming the static library, define TASKML_STATIC
//   (the CMake target does this automatically).

#if defined(TASKML_STATIC)
#  define TASKML_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(TASKML_BUILDING)
#    define TASKML_API __declspec(dllexport)
#  else
#    define TASKML_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define TASKML_API __attribute__((visibility("default")))
#else
#  define TASKML_API
#endif
