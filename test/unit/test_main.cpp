// One process for all unit tests. Cases self-register via SQ2_TEST() in the
// other translation units; squeeze2raop2::test::runAll() runs them (optionally filtered) and
// reports a single pass/fail summary.
//
//   squeeze2raop2_tests              # everything
//   squeeze2raop2_tests --list       # list "suite.name" cases
//   squeeze2raop2_tests --filter config
//   SQ2_FILTER=wire squeeze2raop2_tests

#include "check.h"

int main(int argc, char** argv) { return squeeze2raop2::test::runAll(argc, argv); }