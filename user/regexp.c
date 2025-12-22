// Simple grep.  Only supports ^ . * $ operators.

#include "user/user.h"
static int match_here(char *pattern, char *text);
static int match_star(int char_to_repeat, char *rest_of_pattern, char *text);
int regex_match(char *pattern, char *text)
{
    if (pattern[0] == '^')   // begin with ^, should start matching from the first char
        return match_here(pattern + 1, text);
    do
    { // must look at empty string
        if (match_here(pattern, text))
            return 1;
    } while (*text++ != '\0');  //slice text one by one and try to match
    return 0;
}

// matchhere: search for pattern at beginning of text
static int match_here(char *pattern, char *text)
{
    if (pattern[0] == '\0')
        return 1;   //reach the end of string
    if (pattern[1] == '*')   //the highest priority operator
        return match_star(pattern[0], pattern + 2, text); // skip the "a*" by plus 2
    if (pattern[0] == '$' && pattern[1] == '\0')
        return *text == '\0';   //should be end at the same time
    if (*text != '\0' && (pattern[0] == '.' || pattern[0] == *text))
        return match_here(pattern + 1, text + 1); //normal char or .,advance the regex and text both
    return 0;
}

// matchstar: search for c*re at beginning of text
static int match_star(int char_to_repeat, char *rest_of_pattern, char *text)
{
    do
    { // a * matches zero or more instances
        if (match_here(rest_of_pattern, text))    //try to match zero instances
            return 1;
    } while (*text != '\0' && (*text++ == char_to_repeat || char_to_repeat == '.'));  //current text==a or text==.
    // put the ++ in while can make sure if the condition is false, break immeditately(no increament)
    return 0;
}
