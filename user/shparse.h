#ifndef AXYS_SHPARSE_H
#define AXYS_SHPARSE_H

/*
 * Command-line parsing for the shell, kept free of system calls so it can be
 * unit-tested and fuzzed on the host. The includer must provide `size_t`.
 * Every function edits the line in place and only ever shrinks it or blanks
 * bytes, so no result points outside the original buffer.
 */

/* Next argument: words are separated by spaces; single and double quotes group
 * words and are removed, and a backslash escapes the next byte (inside double
 * quotes only \\ and \" are escapes). The token is unquoted in place. */
static char *next_arg(char **p)
{
    char *s = *p;
    char *tok;
    char *w;
    char quote = 0;

    while (*s == ' ') {
        ++s;
    }
    if (*s == '\0') {
        *p = s;
        return 0;
    }
    tok = w = s;
    for (; *s; ++s) {
        if (quote) {
            if (*s == quote) {
                quote = 0;
            } else if (quote == '"' && *s == '\\' && (s[1] == '"' || s[1] == '\\')) {
                *w++ = *++s;
            } else {
                *w++ = *s;
            }
            continue;
        }
        if (*s == '"' || *s == '\'') {
            quote = *s;
        } else if (*s == '\\' && s[1] != '\0') {
            *w++ = *++s;
        } else if (*s == ' ') {
            break;
        } else {
            *w++ = *s;
        }
    }
    if (*s) {
        ++s;
    }
    *w = '\0';
    *p = s;
    return tok;
}

/* Cut `s` at the first `sep` outside quotes (and not backslash-escaped):
 * terminate the left part and return the start of the right part, or 0. */
static char *split_at(char *s, char sep)
{
    char quote = 0;

    for (; *s; ++s) {
        if (quote) {
            if (*s == quote) {
                quote = 0;
            } else if (quote == '"' && *s == '\\' && s[1] != '\0') {
                ++s;
            }
            continue;
        }
        if (*s == '"' || *s == '\'') {
            quote = *s;
        } else if (*s == '\\' && s[1] != '\0') {
            ++s;
        } else if (*s == sep) {
            *s = '\0';
            return s + 1;
        }
    }
    return 0;
}

/* Remove `< file`, `> file` and `>> file` from a stage, leaving the command and
 * its arguments. File names are copied (unquoted) into `in_name`/`out_name`.
 * Returns 0, or -1 for a redirection without a file name or a name too long. */
static int extract_redirs(char *s, char *in_name, char *out_name, size_t cap, int *append)
{
    char quote = 0;

    *append = 0;
    in_name[0] = out_name[0] = '\0';
    for (char *p = s; *p; ++p) {
        char op;
        char *dst;
        size_t len = 0;
        char wq = 0;

        if (quote) {
            if (*p == quote) {
                quote = 0;
            } else if (quote == '"' && *p == '\\' && p[1] != '\0') {
                ++p;
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            quote = *p;
            continue;
        }
        if (*p == '\\' && p[1] != '\0') {
            ++p;
            continue;
        }
        if (*p != '<' && *p != '>') {
            continue;
        }
        op = *p;
        *p++ = ' ';
        if (op == '>' && *p == '>') {
            *append = 1;
            *p++ = ' ';
        }
        while (*p == ' ') {
            ++p;
        }
        if (*p == '\0' || *p == '<' || *p == '>') {
            return -1;
        }
        dst = op == '<' ? in_name : out_name;
        for (; *p; ++p) {
            if (wq) {
                if (*p == wq) {
                    wq = 0;
                    *p = ' ';
                    continue;
                }
            } else if (*p == '"' || *p == '\'') {
                wq = *p;
                *p = ' ';
                continue;
            } else if (*p == ' ' || *p == '<' || *p == '>') {
                break;
            }
            if (len + 1 >= cap) {
                return -1;
            }
            dst[len++] = *p;
            *p = ' ';
        }
        dst[len] = '\0';
        --p; /* the loop increments again */
    }
    return 0;
}

#endif
