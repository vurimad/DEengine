#pragma once

// Compiler differences live here. OS differences live in engine/platform/.

// Lets clang and gcc check printf-style format strings against their arguments.
// Indices are 1-based: the format parameter and the first variadic argument.
#if defined(__clang__) || defined(__GNUC__)
#define DE_PRINTF_FORMAT(format_index, first_arg_index) __attribute__((format(printf, format_index, first_arg_index)))
#else
#define DE_PRINTF_FORMAT(format_index, first_arg_index)
#endif
