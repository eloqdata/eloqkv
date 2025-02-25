#include "redis_string_match.h"

#include <string>

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
                        int *skipLongerMatches)
{
    // "" will be matched with "*".special judge here.
    // this judge is not exist in origin stringmatchlen_impl, redis use another
    // way to do.
    bool all_start = true;
    for (int i = 0; i < patternLen; i++)
    {
        if (*(pattern + i) != '*')
        {
            all_start = false;
        }
    }
    if (all_start)
    {
        return 1;
    }

    while (patternLen && stringLen)
    {
        switch (pattern[0])
        {
        case '*':
            while (patternLen && pattern[1] == '*')
            {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1)
                return 1; /* match */
            while (stringLen)
            {
                if (stringmatchlen_impl(pattern + 1,
                                        patternLen - 1,
                                        string,
                                        stringLen,
                                        nocase,
                                        skipLongerMatches))
                    return 1; /* match */
                if (*skipLongerMatches)
                    return 0; /* no match */
                string++;
                stringLen--;
            }
            /* There was no match for the rest of the pattern starting
             * from anywhere in the rest of the string. If there were
             * any '*' earlier in the pattern, we can terminate the
             * search early without trying to match them to longer
             * substrings. This is because a longer match for the
             * earlier part of the pattern would require the rest of the
             * pattern to match starting later in the string, and we
             * have just determined that there is no match for the rest
             * of the pattern starting from anywhere in the current
             * string. */
            *skipLongerMatches = 1;
            return 0; /* no match */
            break;
        case '?':
            string++;
            stringLen--;
            break;
        case '[':
        {
            int not_, match;

            pattern++;
            patternLen--;
            not_ = pattern[0] == '^';
            if (not_)
            {
                pattern++;
                patternLen--;
            }
            match = 0;
            while (1)
            {
                if (pattern[0] == '\\' && patternLen >= 2)
                {
                    pattern++;
                    patternLen--;
                    if (pattern[0] == string[0])
                        match = 1;
                }
                else if (pattern[0] == ']')
                {
                    break;
                }
                else if (patternLen == 0)
                {
                    pattern--;
                    patternLen++;
                    break;
                }
                else if (patternLen >= 3 && pattern[1] == '-')
                {
                    int start = pattern[0];
                    int end = pattern[2];
                    int c = string[0];
                    if (start > end)
                    {
                        int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase)
                    {
                        start = tolower(start);
                        end = tolower(end);
                        c = tolower(c);
                    }
                    pattern += 2;
                    patternLen -= 2;
                    if (c >= start && c <= end)
                        match = 1;
                }
                else
                {
                    if (!nocase)
                    {
                        if (pattern[0] == string[0])
                            match = 1;
                    }
                    else
                    {
                        if (tolower(static_cast<int>(pattern[0])) ==
                            tolower(static_cast<int>(string[0])))
                            match = 1;
                    }
                }
                pattern++;
                patternLen--;
            }
            if (not_)
                match = !match;
            if (!match)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        }
        case '\\':
            if (patternLen >= 2)
            {
                pattern++;
                patternLen--;
            }
            /* fall through */
        default:
            if (!nocase)
            {
                if (pattern[0] != string[0])
                    return 0; /* no match */
            }
            else
            {
                if (tolower(static_cast<int>(pattern[0])) !=
                    tolower(static_cast<int>(string[0])))
                    return 0; /* no match */
            }
            string++;
            stringLen--;
            break;
        }
        pattern++;
        patternLen--;
        if (stringLen == 0)
        {
            while (*pattern == '*')
            {
                pattern++;
                patternLen--;
            }
            break;
        }
    }
    if (patternLen == 0 && stringLen == 0)
        return 1;
    return 0;
}

// if pattern match string return 1, else return false.
// if nocase equal 1, it will ignore case
int stringmatchlen(const char *pattern,
                   int patternLen,
                   const char *string,
                   int stringLen,
                   int nocase)
{
    int skipLongerMatches = 0;
    return stringmatchlen_impl(
        pattern, patternLen, string, stringLen, nocase, &skipLongerMatches);
}

// this func compare pattern and string, if nocase equal 1 will ignore case
bool stringcomp(std::string_view string, std::string_view pattern, int nocase)
{
    if (pattern.size() != string.size())
    {
        return false;
    }
    int cnt = pattern.size();

    if (nocase)
    {
        for (int i = 0; i < cnt; i++)
        {
            if (tolower(pattern[i]) != tolower(string[i]))
            {
                return false;
            }
        }
    }
    else
    {
        for (int i = 0; i < cnt; i++)
        {
            if (pattern[i] != string[i])
            {
                return false;
            }
        }
    }

    return true;
}
};  // namespace EloqKV
