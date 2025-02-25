#pragma once
#include <iostream>

#include "string.h"

namespace EloqKV
{

// this func match string with *(match any character of any length), ?(match
// any single character), [](default match character set, ^ match not this
// character set), \ (escape character)
int stringmatchlen_impl(const char *pattern,
                        int patternLen,
                        const char *string,
                        int stringLen,
                        int nocase,
                        int *skipLongerMatches);

// if pattern match string return 1, else return false.
// if nocase equal 1, it will ignore case
int stringmatchlen(const char *pattern,
                   int patternLen,
                   const char *string,
                   int stringLen,
                   int nocase);

// this func compare pattern and string, if nocase equal 1 will ignore case
bool stringcomp(std::string_view string, std::string_view pattern, int nocase);

};  // namespace EloqKV