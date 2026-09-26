// main_as_lib.cpp
//
// Compiles cpp/main.cpp as a library by renaming its main() so that
// standalone tools (compare_dealias, bench_dealias, diagnose_velocity) can
// define their own entry point while still linking against the globals and
// parsing functions that live in main.cpp.
//
// Nothing in this translation unit is called at runtime; it exists purely to
// satisfy the linker.
//
#define main _nexrad_parse_entry_unused_do_not_call_
#include "cpp/main.cpp"
