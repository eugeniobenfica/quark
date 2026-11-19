#ifndef SHAPES_CONFIG_H
#define SHAPES_CONFIG_H

#define SHAPES_BITS 4

#if defined(_WIN32) && !defined(SHAPES_STATIC)
#  define SHAPES_CALL __cdecl
#  define SHAPES_IMPORT __declspec(dllimport)
#else
#  define SHAPES_CALL
#  define SHAPES_IMPORT
#endif

/* each declaration: its linkage, its result, its calling convention */
#define SHAPES_API(result) \
    extern SHAPES_IMPORT result SHAPES_CALL

#endif
