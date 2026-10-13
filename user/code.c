#include "axys.h"
#include "code_key.h"
#include "code_buf.h"

/* /bin/code: native TUI file browser + text editor (axysCode design, C).
 *
 * Tree left, single document right, central gap; modes normal/search/
 * help/confirm-discard; same keymap as the reference minus clipboard,
 * undo and selection (v1 scope, said in `help code` and the status bar
 * when pressed). One document open at a time. UTF-8 is never split.
 * Every capacity refuses loudly instead of truncating: 64 KiB files,
 * 4096 lines, 512 tree nodes.
 *
 * Runs raw + alternate screen, restored on every exit path (single
 * `done` label). It is a separate process, so even a kill cannot leave
 * the shell's terminal modes stuck: the flags are per-process.
 */

#define CODE_PATH_MAX 256
#define CODE_NAME_MAX 64
#define CODE_TREE_MAX 512
#define CODE_QUERY_MAX 128
#define CODE_STATUS_MAX 256
#define CODE_SCR_ROWS 25
#define CODE_SCR_COLS 80
#define CODE_BODY_ROWS 24 /* row 24 is the status bar */

/* ---- tiny output layer: lines built in obuf, one write each ---- */

static char obuf[256];
static axys_size_t olen;

static void o_flush(void)
{
    if (olen > 0) {
        write(1, obuf, olen);
        olen = 0;
    }
}

static void o_bytes(const char *s, axys_size_t n)
{
    /* Never overflows: callers clip to the line; this is the backstop. */
    if (olen + n > sizeof(obuf)) {
        n = sizeof(obuf) - olen;
    }
    for (axys_size_t i = 0; i < n; ++i) {
        obuf[olen++] = s[i];
    }
}

static void o_str(const char *s)
{
    axys_size_t n = 0;

    while (s[n] != '\0') {
        ++n;
    }
    o_bytes(s, n);
}

/* Cursor to 1-based row/col. */
static void o_cup(int row, int col)
{
    char b[16];
    int n = 0;
    char nr[10], nc[10];
    int lr = 0, lc = 0;
    int v;

    o_flush();
    v = row;
    if (v == 0) {
        nr[lr++] = '0';
    } else {
        while (v > 0) {
            nr[lr++] = (char)('0' + v % 10);
            v /= 10;
        }
    }
    v = col;
    if (v == 0) {
        nc[lc++] = '0';
    } else {
        while (v > 0) {
            nc[lc++] = (char)('0' + v % 10);
            v /= 10;
        }
    }
    b[n++] = 0x1b;
    b[n++] = '[';
    while (lr > 0) {
        b[n++] = nr[--lr];
    }
    b[n++] = ';';
    while (lc > 0) {
        b[n++] = nc[--lc];
    }
    b[n++] = 'H';
    write(1, b, (axys_size_t)n);
}

/* Bytes of s (a path or name) clipped to max_cols display columns
 * without splitting UTF-8; returns columns used. Tabs count as one. */
static int o_clip(const char *s, int max_cols)
{
    int cols = 0;

    while (*s != '\0' && cols < max_cols) {
        axys_uint32_t n = code_utf8_len((unsigned char)*s);

        if (*s == '\t') {
            n = 1;
        }
        /* Never emit a partial sequence: stop first. */
        {
            axys_size_t k;

            for (k = 0; k < n && s[k] != '\0'; ++k) {
            }
            if (k < n) {
                break;
            }
        }
        o_bytes(s, n);
        s += n;
        ++cols;
    }
    return cols;
}

static void o_pad(int n)
{
    while (n-- > 0) {
        o_bytes(" ", 1);
    }
}

/* ---- application state ---- */

static int focus; /* 0 tree, 1 editor */
static int mode;  /* 0 normal, 1 search, 2 help, 3 confirm */
static int pending_kind; /* 0 quit, 1 close, 2 open */
static char pending_path[CODE_PATH_MAX];

struct cnode {
    char name[CODE_NAME_MAX];
    u8 is_dir;
    u8 expanded;
    u8 depth;
};

static struct cnode nodes[CODE_TREE_MAX];
static int nnodes;
static int tcursor;
static int show_hidden;
static char tree_root[CODE_PATH_MAX];

static struct code_doc doc;
static int doc_open;
static char doc_path[CODE_PATH_MAX];

static char search_q[CODE_QUERY_MAX];
static long last_hit; /* -1 none */

static char status_msg[CODE_STATUS_MAX];
static int has_status;

static int scr_row; /* first visible doc row */
static int scr_col; /* first visible doc column (chars) */

static void set_status(const char *s)
{
    axys_size_t i = 0;

    while (s[i] != '\0' && i + 1 < sizeof(status_msg)) {
        status_msg[i] = s[i];
        ++i;
    }
    status_msg[i] = '\0';
    has_status = 1;
}

/* ---- tree ---- */

/* Full path of node idx into out (cap CODE_PATH_MAX): 0 ok, -1 overflow. */
static int node_path(int idx, char *out)
{
    int chain[128];
    int n = 0;
    int want;
    axys_size_t pos;
    axys_size_t rl;

    if (idx < 0 || idx >= nnodes) {
        return -1;
    }
    want = nodes[idx].depth;
    chain[n++] = idx;
    for (int j = idx - 1; j >= 0 && want > 0; --j) {
        if (nodes[j].depth == want - 1) {
            if (n >= 128) {
                return -1;
            }
            chain[n++] = j;
            --want;
        }
    }
    if (want != 0) {
        return -1; /* index disagreement: never rewrite, just refuse */
    }
    rl = 0;
    while (tree_root[rl] != '\0') {
        ++rl;
    }
    if (rl + 1 >= CODE_PATH_MAX) {
        return -1;
    }
    for (axys_size_t i = 0; i <= rl; ++i) {
        out[i] = tree_root[i];
    }
    pos = rl;
    for (int k = n - 1; k >= 0; --k) {
        axys_size_t nl = 0;
        const char *nm = nodes[chain[k]].name;

        while (nm[nl] != '\0') {
            ++nl;
        }
        if (pos + 1 + nl >= CODE_PATH_MAX) {
            return -1;
        }
        out[pos++] = '/';
        for (axys_size_t i = 0; i <= nl; ++i) {
            out[pos++] = nm[i];
        }
        --pos; /* keep pos at the NUL for the next join... fix below */
        out[pos] = '\0';
    }
    return 0;
}

/* List dir into the tree at position `at` (children of depth+1), sorted
 * dirs-first. 0 ok, -1 on error (state untouched on read errors). */
static int tree_insert_children(const char *dir, int at, int depth)
{
    /* Two passes: count first (bounded), then insert sorted. A static
     * staging array avoids disturbing the live list on failure. */
    static char sname[256][CODE_NAME_MAX];
    static u8 sisdir[256];
    int n = 0;
    int r;

    for (int i = 0;; ++i) {
        char nm[CODE_NAME_MAX];

        r = readdir(dir, (u64)i, nm, sizeof(nm));
        if (r <= 0) {
            break;
        }
        if (n >= 256) {
            set_status("Directory too big (256 shown)");
            break;
        }
        if (!show_hidden && nm[0] == '.') {
            continue;
        }
        {
            axys_size_t k = 0;

            while (nm[k] != '\0' && k + 1 < sizeof(sname[0])) {
                sname[n][k] = nm[k];
                ++k;
            }
            sname[n][k] = '\0';
        }
        sisdir[n] = r == 2 ? 1 : 0;
        ++n;
    }
    if (r < 0) {
        return -1;
    }
    /* Insertion sort, dirs first then names. */
    for (int i = 1; i < n; ++i) {
        int j = i;

        while (j > 0) {
            int swap = 0;

            if (sisdir[j] && !sisdir[j - 1]) {
                swap = 1;
            } else if (sisdir[j] == sisdir[j - 1]) {
                const char *a = sname[j - 1];
                const char *b = sname[j];

                while (*a && *a == *b) {
                    ++a;
                    ++b;
                }
                if ((unsigned char)*a > (unsigned char)*b) {
                    swap = 1;
                }
            }
            if (!swap) {
                break;
            }
            {
                char t[CODE_NAME_MAX];
                axys_uint8_t td;

                for (int k = 0; k < CODE_NAME_MAX; ++k) {
                    t[k] = sname[j][k];
                }
                td = sisdir[j];
                for (int k = 0; k < CODE_NAME_MAX; ++k) {
                    sname[j][k] = sname[j - 1][k];
                }
                sisdir[j] = sisdir[j - 1];
                for (int k = 0; k < CODE_NAME_MAX; ++k) {
                    sname[j - 1][k] = t[k];
                }
                sisdir[j - 1] = td;
            }
            --j;
        }
    }
    if (nnodes + n > CODE_TREE_MAX) {
        return -1;
    }
    if (depth > 200) {
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        int k;

        for (k = nnodes + i; k > at + i; --k) {
            nodes[k] = nodes[k - 1];
        }
        {
            axys_size_t k = 0;

            while (sname[i][k] != '\0') {
                nodes[at + i].name[k] = sname[i][k];
                ++k;
            }
            nodes[at + i].name[k] = '\0';
        }
        nodes[at + i].is_dir = sisdir[i];
        nodes[at + i].expanded = 0;
        nodes[at + i].depth = (u8)depth;
    }
    nnodes += n;
    return 0;
}

static int tree_load_root(void)
{
    nnodes = 0;
    tcursor = 0;
    return tree_insert_children(tree_root, 0, 0);
}

static int tree_toggle(int idx)
{
    char path[CODE_PATH_MAX];

    if (idx < 0 || idx >= nnodes || !nodes[idx].is_dir) {
        return 0;
    }
    if (!nodes[idx].expanded) {
        if (node_path(idx, path) != 0) {
            set_status("Path too long");
            return 0;
        }
        if (tree_insert_children(path, idx + 1, nodes[idx].depth + 1) != 0) {
            set_status("Could not open directory");
            return 0;
        }
        nodes[idx].expanded = 1;
    } else {
        int end = idx + 1;

        nodes[idx].expanded = 0;
        while (end < nnodes && nodes[end].depth > nodes[idx].depth) {
            ++end;
        }
        for (int k = end; k < nnodes; ++k) {
            nodes[k - (end - idx - 1)] = nodes[k];
        }
        nnodes -= end - idx - 1;
        if (tcursor >= nnodes) {
            tcursor = nnodes - 1;
        }
        if (tcursor < 0) {
            tcursor = 0;
        }
    }
    return 0;
}

/* Heuristic from the reference: no NUL in the first 512 bytes is text. */
static int is_text_path(const char *path)
{
    int fd = open(path, 0);
    char buf[512];
    ssize_t r;

    if (fd < 0) {
        return 0;
    }
    r = read(fd, buf, sizeof(buf));
    close(fd);
    if (r < 0) {
        return 0;
    }
    for (ssize_t i = 0; i < r; ++i) {
        if (buf[i] == '\0') {
            return 0;
        }
    }
    return 1;
}

/* ---- document file IO (never a phantom-empty open) ---- */

static char stage_buf[CODE_MAX_FILE + 1u];

static int doc_open_path(const char *path)
{
    int fd = open(path, 0);
    axys_size_t total = 0;

    if (fd < 0) {
        /* Missing file: start a new empty document (created on save). */
        struct stat_info st;

        if (stat(path, &st) == 0) {
            set_status("Could not open file");
            return -1;
        }
        {
            axys_size_t k = 0;

            while (path[k] != '\0' && k + 1 < sizeof(doc_path)) {
                doc_path[k] = path[k];
                ++k;
            }
            if (path[k] != '\0') {
                set_status("Path too long");
                return -1;
            }
            doc_path[k] = '\0';
        }
        doc.len = 0;
        doc.text[0] = '\0';
        doc.dirty = 0;
        doc.cursor = 0;
        doc.goal = 0;
        doc_rebuild(&doc);
        doc_open = 1;
        last_hit = -1;
        search_q[0] = '\0';
        scr_row = 0;
        scr_col = 0;
        return 0;
    }
    for (;;) {
        ssize_t r = read(fd, stage_buf + total, sizeof(stage_buf) - 1 - total);

        if (r < 0) {
            close(fd);
            set_status("Could not read file");
            return -1;
        }
        if (r == 0) {
            break;
        }
        total += (axys_size_t)r;
        if (total >= sizeof(stage_buf) - 1) {
            char probe;

            if (read(fd, &probe, 1) > 0) {
                close(fd);
                set_status("File too big (64 KiB max)");
                return -1;
            }
            break;
        }
    }
    close(fd);
    if (!is_text_path(path)) {
        set_status("Binary file: cannot edit");
        return -1;
    }
    if (doc_load(&doc, stage_buf, total) != 0) {
        set_status("File too big (64 KiB / 4096 lines max)");
        return -1;
    }
    {
        axys_size_t k = 0;

        while (path[k] != '\0' && k + 1 < sizeof(doc_path)) {
            doc_path[k] = path[k];
            ++k;
        }
        if (path[k] != '\0') {
            set_status("Path too long");
            return -1;
        }
        doc_path[k] = '\0';
    }
    doc_open = 1;
    last_hit = -1;
    search_q[0] = '\0';
    scr_row = 0;
    scr_col = 0;
    return 0;
}

static ssize_t write_all(int fd, const char *buf, axys_size_t len)
{
    axys_size_t done = 0;

    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);

        if (w <= 0) {
            return w == 0 ? (ssize_t)done : w;
        }
        done += (axys_size_t)w;
    }
    return (ssize_t)done;
}

static int doc_save(void)
{
    int fd = open(doc_path, O_WRONLY | O_TRUNC);
    ssize_t w;

    if (fd < 0) {
        fd = open(doc_path, O_WRONLY | O_CREAT);
        if (fd < 0) {
            return (int)fd;
        }
    }
    w = write_all(fd, doc.text, doc.len);
    close(fd);
    if (w < 0) {
        return (int)w;
    }
    if ((axys_size_t)w != doc.len) {
        return -28; /* NOSPC */
    }
    doc.dirty = 0;
    return 0;
}

/* ---- search ---- */

static void search_from_top(void)
{
    long hit = doc_find_from(&doc, search_q, 0);

    if (hit < 0) {
        last_hit = -1;
        set_status("No matches");
        return;
    }
    last_hit = hit;
    has_status = 0;
    doc.cursor = (axys_size_t)hit;
    {
        axys_size_t start;
        axys_uint32_t l = doc_line_of(&doc, doc.cursor);

        (void)doc_line_span(&doc, l, &start);
        doc.goal = doc_chars(doc.text + start, doc.cursor - start);
    }
}

static void search_next(void)
{
    long hit;

    if (search_q[0] == '\0' || last_hit < 0) {
        search_from_top();
        return;
    }
    hit = doc_find_from(&doc, search_q, (axys_size_t)(last_hit + 1));
    if (hit < 0) {
        set_status("No more matches");
        return;
    }
    last_hit = hit;
    has_status = 0;
    doc.cursor = (axys_size_t)hit;
    {
        axys_size_t start;
        axys_uint32_t l = doc_line_of(&doc, doc.cursor);

        (void)doc_line_span(&doc, l, &start);
        doc.goal = doc_chars(doc.text + start, doc.cursor - start);
    }
}

/* ---- renderer (full redraw, 80x25, ASCII borders) ---- */

static void draw_hline(int row, int x0, int w, char edge_l, char edge_r)
{
    o_cup(row + 1, x0 + 1);
    o_bytes(&edge_l, 1);
    for (int i = 0; i < w - 2; ++i) {
        o_bytes("-", 1);
    }
    o_bytes(&edge_r, 1);
    o_flush();
}

static void draw_title(int row, int x0, int w, const char *text, int focused)
{
    int inner = w - 2;

    o_cup(row + 1, x0 + 1);
    if (focused) {
        o_str("\x1b[36m");
    }
    o_bytes("| ", 2);
    {
        int used = o_clip(text, inner - 2);

        o_pad(inner - 2 - used);
    }
    o_bytes("|", 1);
    if (focused) {
        o_str("\x1b[0m");
    }
    o_flush();
}

static void draw_tree_panel(int x0, int w, int height)
{
    int inner = w - 2;
    int rows = height - 2;
    int first = 0;

    if (tcursor - first >= rows) {
        first = tcursor - rows + 1;
    }
    draw_hline(0, x0, w, '+', '+');
    {
        char title[CODE_PATH_MAX + 4];
        axys_size_t i = 0;

        title[i++] = ' ';
        if (focus == 0) {
            title[i++] = '>';
            title[i++] = ' ';
        }
        for (axys_size_t k = 0; tree_root[k] != '\0' && i + 2 < sizeof(title); ++k) {
            title[i++] = tree_root[k];
        }
        title[i++] = ' ';
        title[i] = '\0';
        draw_title(0, x0, w, title, focus == 0);
    }
    for (int r = 0; r < rows; ++r) {
        int idx = first + r;

        o_cup(r + 2, x0 + 1);
        o_bytes("|", 1);
        if (idx < nnodes) {
            int sel = idx == tcursor;
            char line[128];
            int li = 0;

            for (int d = 0; d < nodes[idx].depth && li < 60; ++d) {
                line[li++] = ' ';
                line[li++] = ' ';
            }
            if (nodes[idx].is_dir) {
                line[li++] = nodes[idx].expanded ? 'v' : '>';
                line[li++] = ' ';
            } else {
                line[li++] = ' ';
                line[li++] = ' ';
            }
            for (int k = 0; nodes[idx].name[k] != '\0' && li < 100; ++k) {
                line[li++] = nodes[idx].name[k];
            }
            if (nodes[idx].is_dir) {
                line[li++] = '/';
            }
            line[li] = '\0';
            if (sel) {
                o_str(focus == 0 ? "\x1b[37;44;1m" : "\x1b[36m");
            } else if (nodes[idx].is_dir) {
                o_str("\x1b[36m");
            }
            {
                int used = o_clip(line, inner);

                o_pad(inner - used);
            }
            o_str("\x1b[0m");
        } else {
            o_pad(inner);
        }
        o_bytes("|", 1);
        o_flush();
    }
    draw_hline(height - 1, x0, w, '+', '+');
}

static axys_size_t search_qlen(void)
{
    axys_size_t n = 0;

    while (search_q[n] != '\0') {
        ++n;
    }
    return n;
}

static void draw_editor_panel(int x0, int w, int height)
{
    int inner = w - 2;
    int rows = height - 2;
    int text_rows = rows;
    int cur_l;
    axys_size_t cur_start;

    draw_hline(0, x0, w, '+', '+');
    {
        char title[CODE_NAME_MAX + 8];
        axys_size_t i = 0;
        const char *nm = doc_path;
        const char *slash = doc_path;

        for (const char *p = doc_path; *p; ++p) {
            if (*p == '/') {
                slash = p + 1;
            }
        }
        nm = slash;
        title[i++] = ' ';
        if (focus == 1) {
            title[i++] = '>';
            title[i++] = ' ';
        }
        while (*nm && i + 4 < sizeof(title)) {
            title[i++] = *nm++;
        }
        if (doc.dirty && i + 3 < sizeof(title)) {
            title[i++] = ' ';
            title[i++] = '*';
        }
        title[i++] = ' ';
        title[i] = '\0';
        draw_title(0, x0, w, title, focus == 1);
    }
    if (mode == 1) {
        text_rows = rows - 1;
    }
    cur_l = (int)doc_line_of(&doc, doc.cursor);
    (void)doc_line_span(&doc, (axys_uint32_t)cur_l, &cur_start);
    /* Keep the cursor visible. */
    if (cur_l < scr_row) {
        scr_row = cur_l;
    }
    if (cur_l >= scr_row + text_rows) {
        scr_row = cur_l - text_rows + 1;
    }
    {
        axys_size_t cstart;
        axys_uint32_t l = doc_line_of(&doc, doc.cursor);
        axys_size_t span = doc_line_span(&doc, l, &cstart);
        axys_size_t cur_col = doc_chars(doc.text + cstart, doc.cursor - cstart);

        (void)span;
        if ((int)cur_col < scr_col) {
            scr_col = (int)cur_col;
        }
        if ((int)cur_col >= scr_col + (inner - 6)) {
            scr_col = (int)cur_col - (inner - 6) + 1;
        }
        if (scr_col < 0) {
            scr_col = 0;
        }
    }
    for (int r = 0; r < text_rows; ++r) {
        int l = scr_row + r;

        o_cup(r + 2, x0 + 1);
        o_bytes("|", 1);
        if (l < (int)doc.nlines) {
            axys_size_t start;
            axys_size_t span = doc_line_span(&doc, (axys_uint32_t)l, &start);
            /* Gutter: " 12 " (4 wide). */
            {
                char nb[16];
                int nl = 0;
                unsigned v = (unsigned)(l + 1);

                if (v == 0) {
                    nb[nl++] = '0';
                } else {
                    char rev[10];
                    int m = 0;

                    while (v > 0) {
                        rev[m++] = (char)('0' + v % 10);
                        v /= 10;
                    }
                    while (m > 0) {
                        nb[nl++] = rev[--m];
                    }
                }
                o_pad(3 - nl);
                o_bytes(nb, (axys_size_t)nl);
                o_bytes(" ", 1);
            }
            {
                int width = inner - 4;
                axys_size_t off = start;
                axys_size_t end = start + span;
                int dcol = 0;
                /* Skip scrolled columns (tabs expand). */
                int skip = scr_col;

                while (off < end && skip > 0) {
                    if (doc.text[off] == '\t') {
                        dcol = (dcol / 8 + 1) * 8;
                    } else {
                        ++dcol;
                    }
                    off += code_utf8_len((unsigned char)doc.text[off]);
                    --skip;
                }
                {
                    int col = 0;

                    while (off < end && col < width) {
                        /* Search hit highlight (current match only). */
                        if (mode == 1 && search_q[0] != '\0' && last_hit >= 0 &&
                            off == (axys_size_t)last_hit) {
                            o_str("\x1b[43;30m");
                        }
                        if (doc.text[off] == '\t') {
                            int tab = 8 - (dcol % 8);

                            if (col + tab > width) {
                                break;
                            }
                            o_pad(tab);
                            col += tab;
                            dcol += tab;
                            ++off;
                        } else {
                            axys_uint32_t n = code_utf8_len((unsigned char)doc.text[off]);

                            if (col + 1 > width) {
                                break;
                            }
                            o_bytes(doc.text + off, n);
                            off += n;
                            ++col;
                            ++dcol;
                        }
                        if (mode == 1 && search_q[0] != '\0' && last_hit >= 0 &&
                            off == (axys_size_t)last_hit +
                                     (axys_size_t)search_qlen()) {
                            o_str("\x1b[0m");
                        }
                    }
                    o_str("\x1b[0m");
                    o_pad(width - col);
                }
            }
        } else {
            o_pad(inner);
        }
        o_bytes("|", 1);
        o_flush();
    }
    if (mode == 1) {
        o_cup(text_rows + 2, x0 + 1);
        o_bytes("|", 1);
        o_str("\x1b[33m");
        o_bytes("/", 1);
        {
            int used = o_clip(search_q, inner - 1);

            o_pad(inner - 1 - used);
        }
        o_str("\x1b[0m");
        o_bytes("|", 1);
        o_flush();
    }
    draw_hline(height - 1, x0, w, '+', '+');
}

static void draw_status(void)
{
    int tree_w = doc_open ? 39 : CODE_SCR_COLS;

    o_cup(CODE_SCR_ROWS, 1);
    if (has_status) {
        int used = o_clip(status_msg, CODE_SCR_COLS);

        o_pad(CODE_SCR_COLS - used);
    } else {
        o_str("\x1b[36m");
        {
            int used = o_clip("?: Help", tree_w);

            o_pad(tree_w - used);
        }
        o_str("\x1b[0m");
        if (doc_open) {
            char bar[48];
            int bi = 0;
            axys_size_t start;
            axys_uint32_t l = doc_line_of(&doc, doc.cursor);
            axys_size_t span = doc_line_span(&doc, l, &start);
            axys_size_t col = doc_chars(doc.text + start, doc.cursor - start);
            unsigned row1 = l + 1;
            unsigned col1 = (unsigned)(col + 1);

            (void)span;
            bi = 0;
            {
                const char *pre = "Ln ";

                while (*pre) {
                    bar[bi++] = *pre++;
                }
            }
            {
                char rev[10];
                int m = 0;

                if (row1 == 0) {
                    rev[m++] = '0';
                } else {
                    while (row1 > 0) {
                        rev[m++] = (char)('0' + row1 % 10);
                        row1 /= 10;
                    }
                }
                while (m > 0) {
                    bar[bi++] = rev[--m];
                }
            }
            {
                const char *mid = "  Col ";

                while (*mid) {
                    bar[bi++] = *mid++;
                }
            }
            {
                char rev[10];
                int m = 0;

                if (col1 == 0) {
                    rev[m++] = '0';
                } else {
                    while (col1 > 0) {
                        rev[m++] = (char)('0' + col1 % 10);
                        col1 /= 10;
                    }
                }
                while (m > 0) {
                    bar[bi++] = rev[--m];
                }
            }
            bar[bi] = '\0';
            {
                int used = o_clip(bar, CODE_SCR_COLS - tree_w);

                o_pad(CODE_SCR_COLS - tree_w - used);
            }
        }
    }
    o_flush();
}

static void draw_help(void)
{
    static const char *const lines[] = {
        "Tree",
        "  Up/Down       Move cursor",
        "  Enter         Expand/collapse dir, or open file",
        "  h             Toggle hidden files",
        "  ?             Show this help",
        "  Tab           Focus editor (if open)",
        "  q / Esc       Quit",
        "",
        "Editor",
        "  Arrows        Move cursor",
        "  Home/End      Line start/end",
        "  Ctrl+S        Save",
        "  Ctrl+F        Search (Enter: next, Esc: close)",
        "  Ctrl+W        Close editor",
        "  Ctrl+Tab      Focus tree",
        "  Esc           Quit",
        "",
        "Copy/paste/undo need selection: later.",
        "Press any key to close",
    };
    int w = 72;
    int h = 24;
    int x0 = (CODE_SCR_COLS - w) / 2;
    int y0 = (CODE_BODY_ROWS - h) / 2;
    unsigned n = sizeof(lines) / sizeof(lines[0]);

    if ((int)n > h - 2) {
        n = (unsigned)(h - 2);
    }
    o_cup(y0 + 1, x0 + 1);
    o_str("\x1b[36m+");
    for (int i = 0; i < w - 2; ++i) {
        o_str("-");
    }
    o_str("+\x1b[0m");
    o_flush();
    for (int r = 0; r < h - 2; ++r) {
        o_cup(y0 + 2 + r, x0 + 1);
        o_str("\x1b[36m|\x1b[0m");
        if ((unsigned)r < n) {
            int used = o_clip(lines[r], w - 2);

            o_pad(w - 2 - used);
        } else {
            o_pad(w - 2);
        }
        o_str("\x1b[36m|\x1b[0m");
        o_flush();
    }
    o_cup(y0 + h, x0 + 1);
    o_str("\x1b[36m+");
    for (int i = 0; i < w - 2; ++i) {
        o_str("-");
    }
    o_str("+\x1b[0m");
    o_flush();
}

static void draw_confirm(void)
{
    const char *verb = "quit";
    const char *verb2 = "quit";

    if (pending_kind == 1) {
        verb = "close";
        verb2 = "close";
    } else if (pending_kind == 2) {
        verb = "open another";
        verb2 = "open";
    }
    {
        int w = 64;
        int h = 6;
        int x0 = (CODE_SCR_COLS - w) / 2;
        int y0 = (CODE_BODY_ROWS - h) / 2;

        o_cup(y0 + 1, x0 + 1);
        o_str("\x1b[33m+ Unsaved changes ");
        for (int i = 0; i < w - 19; ++i) {
            o_str("-");
        }
        o_str("+\x1b[0m");
        o_flush();
        o_cup(y0 + 2, x0 + 1);
        o_str("\x1b[33m|\x1b[0m");
        {
            int used = o_clip("Save before continuing?", w - 2);

            o_pad(w - 2 - used);
        }
        o_str("\x1b[33m|\x1b[0m");
        o_flush();
        o_cup(y0 + 3, x0 + 1);
        o_str("\x1b[33m|\x1b[0m");
        o_str(" [Y] Save and ");
        o_str(verb);
        o_str("  [N] Discard  [C] Cancel");
        {
            /* Row is " [Y] Save and "(13) + verb + "  [N] Discard  [C] Cancel"(25). */
            int vlen = 0;

            while (verb[vlen] != '\0') {
                ++vlen;
            }
            o_pad(w - 2 - 13 - vlen - 25);
        }
        o_str("\x1b[33m|\x1b[0m");
        o_flush();
        for (int r = 3; r < h - 1; ++r) {
            o_cup(y0 + 1 + r, x0 + 1);
            o_str("\x1b[33m|\x1b[0m");
            o_pad(w - 2);
            o_str("\x1b[33m|\x1b[0m");
            o_flush();
        }
        o_cup(y0 + h, x0 + 1);
        o_str("\x1b[33m+");
        for (int i = 0; i < w - 2; ++i) {
            o_str("-");
        }
        o_str("+\x1b[0m");
        o_flush();
        (void)verb2;
    }
}

static void draw_frame(void)
{
    /* Hide the hardware cursor while painting, show it at the end. */
    o_str("\x1b[?25l");
    o_flush();
    if (doc_open) {
        draw_tree_panel(0, 39, CODE_BODY_ROWS);
        /* Gap columns. */
        for (int r = 0; r < CODE_BODY_ROWS; ++r) {
            o_cup(r + 1, 40);
            o_bytes("  ", 2);
            o_flush();
        }
        draw_editor_panel(41, 39, CODE_BODY_ROWS);
    } else {
        draw_tree_panel(0, CODE_SCR_COLS, CODE_BODY_ROWS);
    }
    draw_status();
    if (mode == 2) {
        draw_help();
    } else if (mode == 3) {
        draw_confirm();
    }
    /* Park the hardware cursor: editor cursor, or tree row. */
    if (mode == 2 || mode == 3) {
        o_cup(CODE_SCR_ROWS, 1);
    } else if (focus == 1 && doc_open) {
        int l = (int)doc_line_of(&doc, doc.cursor);
        int erow = l - scr_row;
        axys_size_t start;
        axys_uint32_t ll = doc_line_of(&doc, doc.cursor);
        axys_size_t span = doc_line_span(&doc, ll, &start);
        axys_size_t col = doc_chars(doc.text + start, doc.cursor - start);
        int maxr = CODE_BODY_ROWS - 2 - (mode == 1 ? 1 : 0);

        (void)span;
        if (erow < 0) {
            erow = 0;
        }
        if (erow > maxr) {
            erow = maxr;
        }
        o_cup(erow + 2, 41 + 1 + 4 + (int)(col - (axys_size_t)scr_col) + 1);
    } else {
        int trow = tcursor;

        /* Tree scrolls like the single-panel list. */
        {
            int rows = CODE_BODY_ROWS - 2;
            int first = 0;

            if (tcursor - first >= rows) {
                first = tcursor - rows + 1;
            }
            trow = tcursor - first;
        }
        o_cup(trow + 2, 3);
    }
    o_str("\x1b[?25h");
    o_flush();
}

/* ---- input pump: blocking reads, lone-ESC timeout, refeed protocol ---- */

static struct code_keydec keydec;
static int keydec_init;
static struct code_ev keystash;
static int have_keystash;

static int read_key(struct code_ev *ev)
{
    if (!keydec_init) {
        code_key_reset(&keydec);
        keydec_init = 1;
    }
    if (have_keystash) {
        *ev = keystash;
        have_keystash = 0;
        return 1;
    }
    for (;;) {
        char b;
        ssize_t n = read(0, &b, 1);

        if (n <= 0) {
            return 0;
        }
        if (b == 0x1b) {
            /* ESC alone (quit) vs sequence start: brief sleep, then ask
             * the kernel whether more bytes are already here. */
            sleep_ms(25);
            if (console_pending() == 0) {
                code_key_reset(&keydec);
                ev->kind = CODE_K_ESC;
                ev->ch = 0;
                ev->raw_len = 0;
                ev->shift = 0;
                return 1;
            }
        }
        {
            int r = code_key_feed(&keydec, b, ev);

            if (r == 1) {
                return 1;
            }
            if (r == 2 || r == 3) {
                /* ev is complete; the byte goes again. The decoder keeps
                 * any partial second event across calls. */
                struct code_ev first = *ev;

                r = code_key_feed(&keydec, b, ev);
                if (r == 1) {
                    keystash = *ev;
                    *ev = first;
                    have_keystash = 1;
                } else {
                    *ev = first;
                }
                return 1;
            }
            /* r == 0 (need more) or -1 (dropped): keep reading. */
        }
    }
}

/* ---- actions (return 1 means quit the program) ---- */

static int do_quit(void)
{
    if (doc_open && doc.dirty) {
        pending_kind = 0;
        mode = 3;
        return 0;
    }
    return 1;
}

static int do_open_node(int idx)
{
    char path[CODE_PATH_MAX];

    if (idx < 0 || idx >= nnodes) {
        return 0;
    }
    if (nodes[idx].is_dir) {
        if (tree_toggle(idx) != 0) {
            set_status("Could not open directory");
        }
        return 0;
    }
    if (node_path(idx, path) != 0) {
        set_status("Path too long");
        return 0;
    }
    if (doc_open && doc.dirty) {
        axys_size_t k = 0;

        while (path[k] != '\0' && k + 1 < sizeof(pending_path)) {
            pending_path[k] = path[k];
            ++k;
        }
        pending_path[k] = '\0';
        pending_kind = 2;
        mode = 3;
        return 0;
    }
    {
        int same = doc_open;

        if (same) {
            const char *a = doc_path;
            const char *bb = path;

            same = 1;
            while (*a || *bb) {
                if (*a != *bb) {
                    same = 0;
                    break;
                }
                ++a;
                ++bb;
            }
        }
        if (same) {
            focus = 1;
            return 0;
        }
    }
    if (!is_text_path(path)) {
        set_status("Binary file: cannot edit");
        return 0;
    }
    if (doc_open_path(path) == 0) {
        focus = 1;
    }
    return 0;
}

static int apply_pending(void)
{
    if (pending_kind == 0) {
        return 1;
    }
    if (pending_kind == 1) {
        doc_open = 0;
        focus = 0;
        mode = 0;
        return 0;
    }
    mode = 0;
    {
        int same = doc_open;

        if (same) {
            const char *a = doc_path;
            const char *bb = pending_path;

            same = 1;
            while (*a || *bb) {
                if (*a != *bb) {
                    same = 0;
                    break;
                }
                ++a;
                ++bb;
            }
        }
        if (same) {
            focus = 1;
            return 0;
        }
    }
    if (doc_open_path(pending_path) == 0) {
        focus = 1;
    }
    return 0;
}

static int save_current(void)
{
    int r;

    if (!doc_open) {
        return 0;
    }
    r = doc_save();
    if (r < 0) {
        set_status("Could not save file");
        return -1;
    }
    set_status("Saved.");
    return 0;
}

static void move_tree(int dir)
{
    if (dir < 0) {
        if (tcursor > 0) {
            --tcursor;
        }
    } else if (tcursor + 1 < nnodes) {
        ++tcursor;
    }
}

static int handle_tree_key(struct code_ev *ev)
{
    has_status = 0;
    if (ev->kind == CODE_K_UP) {
        move_tree(-1);
    } else if (ev->kind == CODE_K_DOWN) {
        move_tree(1);
    } else if (ev->kind == CODE_K_ENTER) {
        return do_open_node(tcursor);
    } else if (ev->kind == CODE_K_CHAR && (ev->ch == 'h')) {
        show_hidden = !show_hidden;
        if (tree_load_root() != 0) {
            set_status("Could not read directory");
        }
    } else if (ev->kind == CODE_K_CHAR && ev->ch == '?') {
        mode = 2;
    } else if (ev->kind == CODE_K_TAB || ev->kind == CODE_K_FOCUS) {
        if (doc_open) {
            focus = 1;
        } else {
            set_status("No document open");
        }
    } else if ((ev->kind == CODE_K_CHAR && ev->ch == 'q') || ev->kind == CODE_K_ESC) {
        return do_quit();
    }
    return 0;
}

static void not_yet(void)
{
    set_status("Copy/paste/undo comes later");
}

static int handle_editor_key(struct code_ev *ev)
{
    has_status = 0;
    if (!doc_open) {
        focus = 0;
        return 0;
    }
    if (ev->kind == CODE_K_FOCUS) {
        focus = 0;
    } else if (ev->kind == CODE_K_CTRL && ev->ch == 'w') {
        if (doc.dirty) {
            pending_kind = 1;
            mode = 3;
        } else {
            doc_open = 0;
            focus = 0;
        }
    } else if (ev->kind == CODE_K_CTRL && ev->ch == 's') {
        save_current();
    } else if (ev->kind == CODE_K_CTRL && ev->ch == 'f') {
        mode = 1;
        search_q[0] = '\0';
        last_hit = -1;
    } else if (ev->kind == CODE_K_CTRL &&
               (ev->ch == 'c' || ev->ch == 'x' || ev->ch == 'v' || ev->ch == 'z' ||
                ev->ch == 'y')) {
        not_yet();
    } else if (ev->kind == CODE_K_ESC) {
        return do_quit();
    } else if (ev->kind == CODE_K_LEFT) {
        doc_left(&doc);
    } else if (ev->kind == CODE_K_RIGHT) {
        doc_right(&doc);
    } else if (ev->kind == CODE_K_UP) {
        doc_up(&doc);
    } else if (ev->kind == CODE_K_DOWN) {
        doc_down(&doc);
    } else if (ev->kind == CODE_K_HOME) {
        doc_home(&doc);
    } else if (ev->kind == CODE_K_END) {
        doc_end(&doc);
    } else if (ev->kind == CODE_K_BSPACE) {
        doc_backspace(&doc);
    } else if (ev->kind == CODE_K_DEL) {
        doc_delete(&doc);
    } else if (ev->kind == CODE_K_ENTER) {
        if (doc_insert(&doc, "\n", 1) != 0) {
            set_status("Document full");
        }
    } else if (ev->kind == CODE_K_TAB) {
        if (doc_insert(&doc, "\t", 1) != 0) {
            set_status("Document full");
        }
    } else if (ev->kind == CODE_K_CHAR) {
        if (doc_insert(&doc, ev->raw, ev->raw_len) != 0) {
            set_status("Document full");
        }
    }
    return 0;
}

static int handle_search_key(struct code_ev *ev)
{
    if (ev->kind == CODE_K_ESC) {
        mode = 0;
        search_q[0] = '\0';
        last_hit = -1;
    } else if (ev->kind == CODE_K_ENTER) {
        search_next();
    } else if (ev->kind == CODE_K_BSPACE) {
        axys_size_t n = 0;

        while (search_q[n] != '\0') {
            ++n;
        }
        if (n > 0) {
            search_q[n - 1] = '\0';
            search_from_top();
        }
    } else if (ev->kind == CODE_K_CHAR) {
        axys_size_t n = 0;

        while (search_q[n] != '\0') {
            ++n;
        }
        if (n + ev->raw_len + 1 > sizeof(search_q)) {
            set_status("Query too long");
        } else {
            for (axys_uint32_t i = 0; i < ev->raw_len; ++i) {
                search_q[n++] = ev->raw[i];
            }
            search_q[n] = '\0';
            search_from_top();
        }
    }
    return 0;
}

static int handle_confirm_key(struct code_ev *ev)
{
    if (ev->kind == CODE_K_CHAR && (ev->ch == 'y' || ev->ch == 'Y')) {
        if (doc_open && doc.dirty) {
            if (save_current() != 0) {
                mode = 0;
                return 0;
            }
        }
        return apply_pending();
    }
    if (ev->kind == CODE_K_CHAR && (ev->ch == 'n' || ev->ch == 'N')) {
        return apply_pending();
    }
    if ((ev->kind == CODE_K_CHAR && (ev->ch == 'c' || ev->ch == 'C')) ||
        ev->kind == CODE_K_ESC) {
        mode = 0;
    }
    return 0;
}

static int handle_key(struct code_ev *ev)
{
    if (mode == 2) {
        mode = 0;
        return 0;
    }
    if (mode == 3) {
        return handle_confirm_key(ev);
    }
    if (mode == 1) {
        return handle_search_key(ev);
    }
    if (focus == 0) {
        return handle_tree_key(ev);
    }
    return handle_editor_key(ev);
}

/* ---- entry ---- */

static void restore_term(void)
{
    o_str("\x1b[0m");
    o_str("\x1b[?25h");
    o_str("\x1b[?1049l");
    o_flush();
    console_raw(0);
}

static int split_arg(const char *args, char *out, axys_size_t cap)
{
    axys_size_t i = 0;
    axys_size_t o = 0;
    int quoted = 0;
    char qc = '\0';

    while (args[i] == ' ') {
        ++i;
    }
    if (args[i] == '\'' || args[i] == '"') {
        quoted = 1;
        qc = args[i++];
    }
    while (args[i] != '\0') {
        if (quoted && args[i] == qc) {
            break;
        }
        if (!quoted && args[i] == ' ') {
            break;
        }
        if (o + 1 >= cap) {
            return -1;
        }
        out[o++] = args[i++];
    }
    out[o] = '\0';
    return o == 0 ? -1 : 0;
}

int main(const char *args, size_t len)
{
    char target[CODE_PATH_MAX];
    struct stat_info st;
    int rc = 1;

    (void)len;
    if (!args || split_arg(args, target, sizeof(target)) != 0) {
        puts("usage: code [PATH]\n");
        return 1;
    }
    /* Root the tree at the dir itself, or at the file's dir. */
    if (stat(target, &st) != 0) {
        /* Missing path: its parent becomes the root when it exists
         * ("/newfile" roots at "/"). */
        axys_size_t k = 0;
        axys_size_t slash = 0;
        int seen = 0;

        while (target[k] != '\0') {
            if (target[k] == '/') {
                slash = k;
                seen = 1;
            }
            ++k;
        }
        if (!seen || slash >= sizeof(tree_root)) {
            puts("code: bad path\n");
            return 1;
        }
        if (slash == 0) {
            tree_root[0] = '/';
            tree_root[1] = '\0';
        } else {
            for (axys_size_t i = 0; i < slash; ++i) {
                tree_root[i] = target[i];
            }
            tree_root[slash] = '\0';
        }
        if (stat(tree_root, &st) != 0 || st.type != 2) {
            puts("code: bad path\n");
            return 1;
        }
    } else if (st.type == 2) {
        axys_size_t k = 0;

        while (target[k] != '\0' && k + 1 < sizeof(tree_root)) {
            tree_root[k] = target[k];
            ++k;
        }
        if (target[k] != '\0') {
            puts("code: path too long\n");
            return 1;
        }
        tree_root[k] = '\0';
    } else {
        axys_size_t k = 0;
        axys_size_t slash = 0;
        int seen = 0;

        while (target[k] != '\0') {
            if (target[k] == '/') {
                slash = k;
                seen = 1;
            }
            ++k;
        }
        if (!seen || slash == 0) {
            /* No directory part (or root itself): root at "/". */
            tree_root[0] = '/';
            tree_root[1] = '\0';
        } else {
            if (slash >= sizeof(tree_root)) {
                puts("code: path too long\n");
                return 1;
            }
            for (axys_size_t i = 0; i < slash; ++i) {
                tree_root[i] = target[i];
            }
            tree_root[slash] = '\0';
        }
    }
    if (tree_load_root() != 0) {
        puts("code: cannot read directory\n");
        return 1;
    }
    /* Open the file argument up front (new files start empty). */
    if (stat(target, &st) == 0 && st.type == 1) {
        if (!is_text_path(target)) {
            puts("code: binary file\n");
            return 1;
        }
        if (doc_open_path(target) != 0) {
            puts("code: cannot open file\n");
            return 1;
        }
        focus = 1;
    } else if (stat(target, &st) != 0) {
        /* The missing-file case validated above: start it empty. */
        if (doc_open_path(target) != 0) {
            puts("code: cannot open file\n");
            return 1;
        }
        focus = 1;
    }
    if (console_raw(1) != 0) {
        puts("code: raw mode failed\n");
        return 1;
    }
    o_str("\x1b[?1049h");
    o_flush();
    has_status = 0;
    for (;;) {
        struct code_ev ev;

        draw_frame();
        if (!read_key(&ev)) {
            rc = 0; /* stdin ended (e.g. redirected): nothing wrong */
            break;
        }
        if (handle_key(&ev)) {
            rc = 0;
            break;
        }
    }
    restore_term();
    return rc;
}
