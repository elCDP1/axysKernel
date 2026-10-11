/*
 * The shell's line parser (user/shparse.h): quoting, `;` and `|` splitting and
 * redirection extraction. Directed cases first, then a fuzzer that feeds random
 * strings of the interesting bytes in exactly-sized buffers so AddressSanitizer
 * reports any access outside the line. Hosted build only.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/shparse.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void args(const char *line, const char *const *want, size_t n)
{
    size_t bytes = strlen(line) + 1;
    char *copy = malloc(bytes); /* exactly sized */

    memcpy(copy, line, bytes);
    char *p = copy;
    char *tok;
    size_t i = 0;

    while ((tok = next_arg(&p)) != NULL) {
        CHECK(i < n);
        if (strcmp(tok, want[i]) != 0) {
            fprintf(stderr, "line [%s]: token %zu is [%s], wanted [%s]\n", line, i, tok, want[i]);
            exit(1);
        }
        ++i;
    }
    CHECK(i == n);
    free(copy);
}

static void directed(void)
{
    {
        const char *w[] = {"a", "b c", "d e", "f g"};
        args("a \"b c\" 'd e' f\\ g", w, 4);
    }
    {
        const char *w[] = {"a  b", "c"};
        args("  \"a  b\"   c  ", w, 2);
    }
    {
        const char *w[] = {"", "x"};
        args("\"\" x", w, 2); /* an empty quoted word is a word */
    }
    {
        const char *w[] = {"ab"};
        args("a\"\"b", w, 1);          /* quotes join adjacent text */
    }
    {
        const char *w[] = {"say \"hi\"", "it's"};
        args("\"say \\\"hi\\\"\" \"it's\"", w, 2);
    }
    {
        const char *w[] = {"a\\b"};
        args("'a\\b'", w, 1);          /* no escapes inside single quotes */
    }
    {
        const char *w[] = {"unterminated quote"};
        args("\"unterminated quote", w, 1); /* lenient: runs to the end */
    }
    {
        const char *w[] = {"x\\"};
        args("x\\", w, 1);             /* a trailing backslash is kept */
    }
    {
        char s1[] = "a;b";
        char *r = split_at(s1, ';');
        CHECK(r && strcmp(s1, "a") == 0 && strcmp(r, "b") == 0);
        char s2[] = "echo 'a;b';c";
        r = split_at(s2, ';');
        CHECK(r && strcmp(s2, "echo 'a;b'") == 0 && strcmp(r, "c") == 0);
        char s3[] = "echo a\\;b;c";
        r = split_at(s3, ';');
        CHECK(r && strcmp(s3, "echo a\\;b") == 0 && strcmp(r, "c") == 0);
        char s4[] = "no separator";
        CHECK(split_at(s4, ';') == NULL && strcmp(s4, "no separator") == 0);
        char s5[] = "\"a|b\" | c";
        r = split_at(s5, '|');
        CHECK(r && strcmp(r, " c") == 0);
    }
    {
        char in[32], out[32];
        int app;
        char l1[] = "cat < in > out";
        CHECK(extract_redirs(l1, in, out, sizeof(in), &app) == 0);
        CHECK(strcmp(in, "in") == 0 && strcmp(out, "out") == 0 && !app);
        for (char *c = l1 + 3; *c; ++c) CHECK(*c == ' '); /* only the command is left */
        char l2[] = "echo hi >> log";
        CHECK(extract_redirs(l2, in, out, sizeof(in), &app) == 0 && strcmp(out, "log") == 0 && app);
        char l3[] = "echo \">\" '<' > f";
        CHECK(extract_redirs(l3, in, out, sizeof(in), &app) == 0 && strcmp(out, "f") == 0 && in[0] == '\0');
        CHECK(strstr(l3, "\">\"") && strstr(l3, "'<'")); /* quoted operators are text */
        char l4[] = "echo > \"my file\"";
        CHECK(extract_redirs(l4, in, out, sizeof(in), &app) == 0 && strcmp(out, "my file") == 0);
        char l5[] = "echo >";
        CHECK(extract_redirs(l5, in, out, sizeof(in), &app) == -1);
        char l6[] = "echo > > f";
        CHECK(extract_redirs(l6, in, out, sizeof(in), &app) == -1);
        char l7[] = "echo > aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        CHECK(extract_redirs(l7, in, out, 8, &app) == -1);   /* name longer than the buffer */
        char l8[] = "echo a>b";
        CHECK(extract_redirs(l8, in, out, sizeof(in), &app) == 0 && strcmp(out, "b") == 0);
        char l9[] = "echo a\\>b";
        CHECK(extract_redirs(l9, in, out, sizeof(in), &app) == 0 && out[0] == '\0'); /* escaped */
    }
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng % n); }

static void fuzz(unsigned iterations)
{
    static const char alphabet[] = " ab\"'\\<>|;\t";

    for (unsigned it = 0; it < iterations; ++it) {
        size_t len = rnd(48);
        char *line = malloc(len + 1); /* exactly sized: ASan sees any overrun */
        char in[16], out[16];
        int app;

        for (size_t i = 0; i < len; ++i) line[i] = alphabet[rnd(sizeof(alphabet) - 1)];
        line[len] = '\0';

        switch (rnd(3)) {
        case 0: {
            char *p = line;
            size_t tokens = 0;
            char *tok;

            while ((tok = next_arg(&p)) != NULL) {
                CHECK(tok >= line && tok <= line + len);
                CHECK(p >= line && p <= line + len + 1);
                CHECK(++tokens <= len + 1);   /* every call consumes input: no endless loop */
            }
            break;
        }
        case 1: {
            char *cur = line;
            size_t parts = 0;

            while (cur != NULL) {
                CHECK(++parts <= len + 1);
                cur = split_at(cur, rnd(2) ? ';' : '|');
                if (cur != NULL) CHECK(cur >= line && cur <= line + len + 1);
            }
            break;
        }
        default:
            if (extract_redirs(line, in, out, sizeof(in), &app) == 0) {
                CHECK(strlen(in) < sizeof(in) && strlen(out) < sizeof(out));
                CHECK(strlen(line) <= len);
            }
            break;
        }
        free(line);
    }
}

int main(int argc, char **argv)
{
    directed();
    fuzz(argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 300000);
    puts("test_shparse: ok");
    return 0;
}
