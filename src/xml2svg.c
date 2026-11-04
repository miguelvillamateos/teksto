#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// ============================================================================
// 1. Constantes y Límites (C23)
// ============================================================================

constexpr size_t MAX_FONT_VARIANTS = 256;
constexpr size_t MAX_PARA_STYLES   = 256;
constexpr size_t MAX_CHAR_STYLES   = 512;
constexpr size_t MAX_XML_FRAMES    = 512;
constexpr size_t MAX_FLOW_FRAMES   = 1024;
constexpr size_t MAX_ATTRS         = 24;
constexpr double MM_PER_CM         = 10.0;
constexpr double MM_PER_M          = 1000.0;
constexpr double MM_PER_PT         = 25.4 / 72.0;

// Geometría del escenario ViewBox Sliding (en mm)
constexpr double DESK_PAD_X        = 16.0;
constexpr double DESK_PAD_TOP      = 14.0;
constexpr double DESK_PAD_BOTTOM   = 26.0;
constexpr double PAGE_STRIDE_GAP   = 36.0;

// ============================================================================
// 2. Tipos de Dominio (Estilos, Marcos y Elementos del Documento)
// ============================================================================

typedef enum : unsigned char {
    ALIGN_LEFT = 0,
    ALIGN_CENTER,
    ALIGN_RIGHT,
    ALIGN_JUSTIFY
} ParaAlign;

typedef struct {
    char id[48];
    char family[64];
    bool bold;
    bool italic;
} FontVariant;

typedef struct {
    char id[32];
    ParaAlign align;
    double line_spacing_mm;
    double space_before_mm;
    double space_after_mm;
    double indent_first_mm;
    double indent_left_mm;
    double indent_right_mm;
} ParaStyle;

typedef struct {
    char id[32];
    size_t font_variant_idx;
    double size_mm;
    char color[32];
    bool underline;
    bool small_caps;
    int shift; // 0=normal, 1=super, 2=sub
} CharStyle;

typedef struct {
    size_t page_num; // 1-based según el XML
    double x_mm, y_mm, w_mm, h_mm;
} XmlFrameDef;

typedef enum : unsigned char {
    RUN_TEXT = 0,
    RUN_LINE_BREAK,
    RUN_HSPACE,
    RUN_RULE,
    RUN_BOX
} RunKind;

typedef enum : unsigned char {
    FILL_NONE = 0,
    FILL_SPACE,
    FILL_DOTS,
    FILL_RULE
} HFillKind;

typedef struct {
    RunKind kind;
    size_t style_idx;
    char *text;
    double w_mm;
    double h_mm;
    double raise_mm;
    double border_mm;
    double pad_mm;
    char box_align;
    HFillKind fill;
} SpanRun;

typedef struct {
    size_t span;
    char align;
    int bl;
    int br;
    double fixed_w_mm;
    SpanRun *runs;
    size_t run_count;
    size_t run_cap;
} TableCell;

typedef enum : unsigned char {
    TROW_DATA = 0,
    TROW_HLINE,
    TROW_CLINE
} TableRowKind;

typedef struct {
    TableRowKind kind;
    int cline_from;
    int cline_to;
    double extra_space_mm;
    TableCell *cells;
    size_t cell_count;
    size_t cell_cap;
} TableRowItem;

typedef struct {
    size_t cols;
    ParaAlign table_align;
    double tabcolsep_mm;
    double rulewidth_mm;
    TableRowItem *rows;
    size_t row_count;
    size_t row_cap;
} TableDef;

typedef enum : unsigned char {
    ITEM_PARA = 0,
    ITEM_PAGE_BREAK,
    ITEM_VSPACE,
    ITEM_TABLE,
    ITEM_FOOTNOTE,
    ITEM_MARGINPAR
} StoryItemType;

typedef struct {
    StoryItemType type;
    size_t para_style_idx;
    char label[128];
    size_t label_style_idx;
    double vspace_mm;
    bool vspace_starred;
    bool vspace_fill;
    TableDef *table;
    SpanRun *runs;
    size_t run_count;
    size_t run_cap;
} StoryItem;

typedef struct {
    double page_w_mm;
    double page_h_mm;
    double margin_top_mm;
    double margin_bottom_mm;
    double margin_left_mm;
    double margin_right_mm;
    size_t declared_pages;

    char page_style[32];
    char page_numbering[32];
    char left_mark[256];
    char right_mark[256];

    FontVariant variants[MAX_FONT_VARIANTS];
    size_t variant_count;

    ParaStyle para_styles[MAX_PARA_STYLES];
    size_t para_style_count;

    CharStyle char_styles[MAX_CHAR_STYLES];
    size_t char_style_count;

    XmlFrameDef xml_frames[MAX_XML_FRAMES];
    size_t xml_frame_count;

    StoryItem *items;
    size_t item_count;
    size_t item_cap;
} IrDocument;

// ============================================================================
// 3. Estructuras del Motor de Composición Tipográfica y Flujo SVG
// ============================================================================

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

typedef struct {
    size_t page_idx; // 0-based
    double x_mm, y_mm, w_mm, h_mm;
    double orig_h_mm;
    const StoryItem *footnotes[16];
    size_t footnote_count;
    StrBuf svg_content;
} FlowFrame;

typedef struct {
    RunKind kind;
    size_t char_style_idx;
    char *word;
    double word_w_mm;
    double space_w_mm;
    double h_mm;
    double raise_mm;
    double border_mm;
    double pad_mm;
    char box_align;
    HFillKind fill;
    bool space_after;
} WordToken;

typedef struct {
    FlowFrame frames[MAX_FLOW_FRAMES];
    size_t frame_count;
    size_t total_pages;

    size_t cur_frame;
    double cursor_y_mm;
    bool at_frame_top;
} Typesetter;

// ============================================================================
// 4. Utilidades de Cadena, UTF-8 y Conversión de Unidades
// ============================================================================

static bool sb_init(StrBuf *sb, size_t cap) {
    sb->data = malloc(cap);
    if (!sb->data) return false;
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = cap;
    return true;
}

static void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = nullptr;
    sb->len = 0;
    sb->cap = 0;
}

static bool sb_reserve(StrBuf *sb, size_t extra) {
    size_t needed = sb->len + extra + 1;
    if (needed <= sb->cap) return true;
    size_t new_cap = (sb->cap > 0) ? sb->cap : 256;
    while (new_cap < needed) new_cap *= 2;
    char *p = realloc(sb->data, new_cap);
    if (!p) return false;
    sb->data = p;
    sb->cap = new_cap;
    return true;
}

static void sb_append_mem(StrBuf *sb, const char *s, size_t n) {
    if (n == 0 || !sb_reserve(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append_cstr(StrBuf *sb, const char *s) {
    sb_append_mem(sb, s, strlen(s));
}

static void sb_append_escaped_xml(StrBuf *sb, const char *s) {
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
            case '&':  sb_append_cstr(sb, "&amp;");  break;
            case '<':  sb_append_cstr(sb, "&lt;");   break;
            case '>':  sb_append_cstr(sb, "&gt;");   break;
            case '\"': sb_append_cstr(sb, "&quot;"); break;
            case '\'': sb_append_cstr(sb, "&apos;"); break;
            default:   sb_append_mem(sb, p, 1);      break;
        }
    }
}

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *copy = malloc(n + 1);
    if (copy) memcpy(copy, s, n + 1);
    return copy;
}

static void replace_extension(const char *path, const char *new_ext, char *out_buf, size_t buf_size) {
    strncpy(out_buf, path, buf_size - 1);
    out_buf[buf_size - 1] = '\0';
    char *dot = strrchr(out_buf, '.');
    if (dot) *dot = '\0';
    strncat(out_buf, new_ext, buf_size - strlen(out_buf) - 1);
}

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return nullptr; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

static double parse_mm(const char *str) {
    if (!str || *str == '\0') return 0.0;
    char *end = nullptr;
    double val = strtod(str, &end);
    while (end && *end != '\0' && isspace((unsigned char)*end)) end++;
    if (!end || *end == '\0' || strcmp(end, "mm") == 0) return val;
    if (strcmp(end, "cm") == 0) return val * MM_PER_CM;
    if (strcmp(end, "m") == 0)  return val * MM_PER_M;
    if (strcmp(end, "pt") == 0) return val * MM_PER_PT;
    return val;
}

static bool str_contains_ci(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

// ============================================================================
// 5. Analizador Ligero del XML Intermedio (v0.2)
// ============================================================================

typedef struct {
    char name[48];
    char value[128];
} XmlAttr;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
} XmlReader;

static void xml_unescape_into(StrBuf *out, const char *raw, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (raw[i] == '&') {
            if (len - i >= 5 && strncmp(raw + i, "&amp;", 5) == 0)  { sb_append_cstr(out, "&");  i += 5; continue; }
            if (len - i >= 4 && strncmp(raw + i, "&lt;", 4) == 0)   { sb_append_cstr(out, "<");  i += 4; continue; }
            if (len - i >= 4 && strncmp(raw + i, "&gt;", 4) == 0)   { sb_append_cstr(out, ">");  i += 4; continue; }
            if (len - i >= 6 && strncmp(raw + i, "&quot;", 6) == 0) { sb_append_cstr(out, "\""); i += 6; continue; }
            if (len - i >= 6 && strncmp(raw + i, "&apos;", 6) == 0) { sb_append_cstr(out, "'");  i += 6; continue; }
        }
        sb_append_mem(out, &raw[i], 1);
        i++;
    }
}

static const char *get_attr(const XmlAttr *attrs, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(attrs[i].name, name) == 0) return attrs[i].value;
    }
    return nullptr;
}

static size_t find_variant_idx(const IrDocument *doc, const char *id) {
    for (size_t i = 0; i < doc->variant_count; i++) {
        if (strcmp(doc->variants[i].id, id) == 0) return i;
    }
    return 0;
}

static size_t find_para_style_idx(const IrDocument *doc, const char *id) {
    for (size_t i = 0; i < doc->para_style_count; i++) {
        if (strcmp(doc->para_styles[i].id, id) == 0) return i;
    }
    return 0;
}

static size_t find_char_style_idx(const IrDocument *doc, const char *id) {
    for (size_t i = 0; i < doc->char_style_count; i++) {
        if (strcmp(doc->char_styles[i].id, id) == 0) return i;
    }
    return 0;
}

static void doc_push_item(IrDocument *doc, StoryItem item) {
    if (doc->item_count >= doc->item_cap) {
        doc->item_cap = (doc->item_cap == 0) ? 64 : doc->item_cap * 2;
        doc->items = realloc(doc->items, doc->item_cap * sizeof(StoryItem));
    }
    doc->items[doc->item_count++] = item;
}

static void para_push_special_run(StoryItem *para, SpanRun run) {
    if (para->run_count >= para->run_cap) {
        para->run_cap = (para->run_cap == 0) ? 8 : para->run_cap * 2;
        para->runs = realloc(para->runs, para->run_cap * sizeof(SpanRun));
    }
    para->runs[para->run_count++] = run;
}

static void para_push_run(StoryItem *para, size_t style_idx, const char *text) {
    if (!text || text[0] == '\0') return;
    para_push_special_run(para, (SpanRun){
        .kind      = RUN_TEXT,
        .style_idx = style_idx,
        .text      = dup_cstr(text)
    });
}

static void table_push_row(TableDef *tbl, TableRowItem row) {
    if (tbl->row_count >= tbl->row_cap) {
        tbl->row_cap = (tbl->row_cap == 0) ? 8 : tbl->row_cap * 2;
        tbl->rows = realloc(tbl->rows, tbl->row_cap * sizeof(TableRowItem));
    }
    tbl->rows[tbl->row_count++] = row;
}

static void row_push_cell(TableRowItem *row, TableCell cell) {
    if (row->cell_count >= row->cell_cap) {
        row->cell_cap = (row->cell_cap == 0) ? 8 : row->cell_cap * 2;
        row->cells = realloc(row->cells, row->cell_cap * sizeof(TableCell));
    }
    row->cells[row->cell_count++] = cell;
}

static bool parse_ir_xml(const char *xml_src, size_t xml_len, IrDocument *doc) {
    memset(doc, 0, sizeof(*doc));
    doc->page_w_mm        = 210.0;
    doc->page_h_mm        = 297.0;
    doc->margin_top_mm    = 20.0;
    doc->margin_bottom_mm = 20.0;
    doc->margin_left_mm   = 20.0;
    doc->margin_right_mm  = 20.0;
    doc->declared_pages   = 1;
    snprintf(doc->page_style, sizeof(doc->page_style), "plain");
    snprintf(doc->page_numbering, sizeof(doc->page_numbering), "arabic");

    XmlReader r = { .src = xml_src, .pos = 0, .len = xml_len };
    char current_family[256] = "sans-serif";
    bool in_para = false;
    bool in_span = false;
    bool in_box  = false;
    size_t active_cs_idx = 0;
    SpanRun pending_box  = {0};
    StoryItem current_para = {0};

    TableDef *active_table   = nullptr;
    TableRowItem current_row = {0};
    TableCell current_cell   = {0};
    bool in_trow = false;
    bool in_cell = false;

    StrBuf span_buf;
    StrBuf box_buf;
    sb_init(&span_buf, 256);
    sb_init(&box_buf, 128);

    while (r.pos < r.len) {
        size_t lt = r.pos;
        while (lt < r.len && r.src[lt] != '<') lt++;

        if (lt > r.pos && in_para) {
            if (in_box) {
                xml_unescape_into(&box_buf, r.src + r.pos, lt - r.pos);
            } else if (in_span) {
                xml_unescape_into(&span_buf, r.src + r.pos, lt - r.pos);
            } else {
                StrBuf tmp;
                sb_init(&tmp, lt - r.pos + 8);
                xml_unescape_into(&tmp, r.src + r.pos, lt - r.pos);
                para_push_run(&current_para, current_para.label_style_idx, tmp.data);
                sb_free(&tmp);
            }
        }

        if (lt >= r.len) break;
        r.pos = lt;

        // Declaración XML <?...?>
        if (r.pos + 1 < r.len && r.src[r.pos + 1] == '?') {
            while (r.pos + 1 < r.len && !(r.src[r.pos] == '?' && r.src[r.pos + 1] == '>')) r.pos++;
            r.pos += 2;
            continue;
        }

        // Comentarios <!--...-->
        if (r.pos + 3 < r.len && strncmp(r.src + r.pos, "<!--", 4) == 0) {
            r.pos += 4;
            while (r.pos + 2 < r.len && strncmp(r.src + r.pos, "-->", 3) != 0) r.pos++;
            r.pos += 3;
            continue;
        }

        // Etiqueta de cierre </tag>
        if (r.pos + 1 < r.len && r.src[r.pos + 1] == '/') {
            r.pos += 2;
            size_t name_start = r.pos;
            while (r.pos < r.len && r.src[r.pos] != '>' && !isspace((unsigned char)r.src[r.pos])) r.pos++;
            char tag[48] = {0};
            size_t tlen = r.pos - name_start;
            if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
            memcpy(tag, r.src + name_start, tlen);
            while (r.pos < r.len && r.src[r.pos] != '>') r.pos++;
            if (r.pos < r.len) r.pos++;

            if (strcmp(tag, "box") == 0 && in_box) {
                pending_box.text = dup_cstr(box_buf.data);
                para_push_special_run(&current_para, pending_box);
                box_buf.len = 0;
                box_buf.data[0] = '\0';
                in_box = false;
            } else if (strcmp(tag, "span") == 0 && in_span) {
                para_push_run(&current_para, active_cs_idx, span_buf.data);
                span_buf.len = 0;
                span_buf.data[0] = '\0';
                in_span = false;
            } else if (strcmp(tag, "cell") == 0 && in_cell) {
                current_cell.runs      = current_para.runs;
                current_cell.run_count = current_para.run_count;
                current_cell.run_cap   = current_para.run_cap;
                if (in_trow) {
                    row_push_cell(&current_row, current_cell);
                }
                memset(&current_cell, 0, sizeof(current_cell));
                memset(&current_para, 0, sizeof(current_para));
                in_cell = false;
                in_para = false;
            } else if (strcmp(tag, "row") == 0 && in_trow) {
                if (active_table) {
                    table_push_row(active_table, current_row);
                }
                memset(&current_row, 0, sizeof(current_row));
                in_trow = false;
            } else if (strcmp(tag, "table") == 0 && active_table) {
                StoryItem titem = {
                    .type  = ITEM_TABLE,
                    .table = active_table
                };
                doc_push_item(doc, titem);
                active_table = nullptr;
            } else if ((strcmp(tag, "p") == 0 || strcmp(tag, "footnote") == 0 || strcmp(tag, "marginpar") == 0) && in_para) {
                doc_push_item(doc, current_para);
                memset(&current_para, 0, sizeof(current_para));
                in_para = false;
            }
            continue;
        }

        // Etiqueta de apertura o autocierre <tag attr="val" ... [/]>
        r.pos++; // saltar '<'
        size_t name_start = r.pos;
        while (r.pos < r.len && !isspace((unsigned char)r.src[r.pos]) &&
               r.src[r.pos] != '>' && r.src[r.pos] != '/') {
            r.pos++;
        }
        char tag[48] = {0};
        size_t tlen = r.pos - name_start;
        if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
        memcpy(tag, r.src + name_start, tlen);

        XmlAttr attrs[MAX_ATTRS];
        size_t attr_count = 0;
        bool self_closing = false;

        while (r.pos < r.len) {
            while (r.pos < r.len && isspace((unsigned char)r.src[r.pos])) r.pos++;
            if (r.pos >= r.len) break;
            if (r.src[r.pos] == '/') {
                self_closing = true;
                r.pos++;
                while (r.pos < r.len && r.src[r.pos] != '>') r.pos++;
                if (r.pos < r.len) r.pos++;
                break;
            }
            if (r.src[r.pos] == '>') {
                r.pos++;
                break;
            }

            size_t an_start = r.pos;
            while (r.pos < r.len && r.src[r.pos] != '=' && !isspace((unsigned char)r.src[r.pos]) && r.src[r.pos] != '>') {
                r.pos++;
            }
            size_t an_len = r.pos - an_start;
            while (r.pos < r.len && isspace((unsigned char)r.src[r.pos])) r.pos++;
            if (r.pos < r.len && r.src[r.pos] == '=') r.pos++;
            while (r.pos < r.len && isspace((unsigned char)r.src[r.pos])) r.pos++;

            char quote = (r.pos < r.len && (r.src[r.pos] == '"' || r.src[r.pos] == '\'')) ? r.src[r.pos++] : '\0';
            size_t av_start = r.pos;
            if (quote != '\0') {
                while (r.pos < r.len && r.src[r.pos] != quote) r.pos++;
            }
            size_t av_len = r.pos - av_start;
            if (quote != '\0' && r.pos < r.len) r.pos++;

            if (attr_count < MAX_ATTRS) {
                XmlAttr *a = &attrs[attr_count++];
                if (an_len >= sizeof(a->name)) an_len = sizeof(a->name) - 1;
                memcpy(a->name, r.src + an_start, an_len);
                a->name[an_len] = '\0';

                StrBuf val_sb;
                sb_init(&val_sb, av_len + 8);
                xml_unescape_into(&val_sb, r.src + av_start, av_len);
                snprintf(a->value, sizeof(a->value), "%s", val_sb.data);
                sb_free(&val_sb);
            }
        }

        // Interpretación semántica de nodos del IR v0.2
        if (strcmp(tag, "page-defaults") == 0) {
            const char *w = get_attr(attrs, attr_count, "width");
            const char *h = get_attr(attrs, attr_count, "height");
            if (w) doc->page_w_mm = parse_mm(w);
            if (h) doc->page_h_mm = parse_mm(h);
        } else if (strcmp(tag, "margins") == 0) {
            const char *t = get_attr(attrs, attr_count, "top");
            const char *b = get_attr(attrs, attr_count, "bottom");
            const char *l = get_attr(attrs, attr_count, "left");
            const char *rg = get_attr(attrs, attr_count, "right");
            if (t)  doc->margin_top_mm    = parse_mm(t);
            if (b)  doc->margin_bottom_mm = parse_mm(b);
            if (l)  doc->margin_left_mm   = parse_mm(l);
            if (rg) doc->margin_right_mm  = parse_mm(rg);
        } else if (strcmp(tag, "page-style") == 0) {
            const char *md = get_attr(attrs, attr_count, "mode");
            const char *nm = get_attr(attrs, attr_count, "numbering");
            const char *lm = get_attr(attrs, attr_count, "left-mark");
            const char *rm = get_attr(attrs, attr_count, "right-mark");
            if (md && md[0]) snprintf(doc->page_style, sizeof(doc->page_style), "%.31s", md);
            if (nm && nm[0]) snprintf(doc->page_numbering, sizeof(doc->page_numbering), "%.31s", nm);
            if (lm && lm[0]) snprintf(doc->left_mark, sizeof(doc->left_mark), "%.255s", lm);
            if (rm && rm[0]) snprintf(doc->right_mark, sizeof(doc->right_mark), "%.255s", rm);
        } else if (strcmp(tag, "font-family") == 0) {
            const char *name = get_attr(attrs, attr_count, "name");
            if (name) snprintf(current_family, sizeof(current_family), "%s", name);
        } else if (strcmp(tag, "variant") == 0 && doc->variant_count < MAX_FONT_VARIANTS) {
            FontVariant *fv = &doc->variants[doc->variant_count++];
            const char *id = get_attr(attrs, attr_count, "id");
            const char *wt = get_attr(attrs, attr_count, "weight");
            const char *sl = get_attr(attrs, attr_count, "slant");
            snprintf(fv->id, sizeof(fv->id), "%s", id ? id : "");
            snprintf(fv->family, sizeof(fv->family), "%s", current_family);
            fv->bold   = (wt && strcmp(wt, "bold") == 0);
            fv->italic = (sl && strcmp(sl, "italic") == 0);
        } else if (strcmp(tag, "para-style") == 0 && doc->para_style_count < MAX_PARA_STYLES) {
            ParaStyle *ps = &doc->para_styles[doc->para_style_count++];
            const char *id = get_attr(attrs, attr_count, "id");
            const char *al = get_attr(attrs, attr_count, "align");
            snprintf(ps->id, sizeof(ps->id), "%s", id ? id : "");
            ps->align = ALIGN_LEFT;
            if (al) {
                if (strcmp(al, "center") == 0)       ps->align = ALIGN_CENTER;
                else if (strcmp(al, "right") == 0)   ps->align = ALIGN_RIGHT;
                else if (strcmp(al, "justify") == 0) ps->align = ALIGN_JUSTIFY;
            }
            ps->line_spacing_mm = parse_mm(get_attr(attrs, attr_count, "line-spacing"));
            ps->space_before_mm = parse_mm(get_attr(attrs, attr_count, "space-before"));
            ps->space_after_mm  = parse_mm(get_attr(attrs, attr_count, "space-after"));
            ps->indent_first_mm = parse_mm(get_attr(attrs, attr_count, "indent-first"));
            ps->indent_left_mm  = parse_mm(get_attr(attrs, attr_count, "indent-left"));
            ps->indent_right_mm = parse_mm(get_attr(attrs, attr_count, "indent-right"));
            if (ps->line_spacing_mm <= 0.1) ps->line_spacing_mm = 5.0;
        } else if (strcmp(tag, "char-style") == 0 && doc->char_style_count < MAX_CHAR_STYLES) {
            CharStyle *cs = &doc->char_styles[doc->char_style_count++];
            const char *id   = get_attr(attrs, attr_count, "id");
            const char *fref = get_attr(attrs, attr_count, "font-ref");
            const char *sz   = get_attr(attrs, attr_count, "size");
            const char *col  = get_attr(attrs, attr_count, "color");
            const char *ul   = get_attr(attrs, attr_count, "underline");
            const char *sc   = get_attr(attrs, attr_count, "small-caps");
            const char *sh   = get_attr(attrs, attr_count, "shift");
            snprintf(cs->id, sizeof(cs->id), "%s", id ? id : "");
            cs->font_variant_idx = fref ? find_variant_idx(doc, fref) : 0;
            cs->size_mm          = sz ? parse_mm(sz) : (11.0 * MM_PER_PT);
            snprintf(cs->color, sizeof(cs->color), "%s", col ? col : "Black");
            cs->underline        = (ul && strcmp(ul, "true") == 0);
            cs->small_caps       = (sc && strcmp(sc, "true") == 0);
            cs->shift            = 0;
            if (sh) {
                if (strcmp(sh, "super") == 0)    cs->shift = 1;
                else if (strcmp(sh, "sub") == 0) cs->shift = 2;
            }
        } else if (strcmp(tag, "layout") == 0) {
            const char *tp = get_attr(attrs, attr_count, "total-pages");
            if (tp) {
                size_t p = (size_t)strtoul(tp, nullptr, 10);
                if (p > 0) doc->declared_pages = p;
            }
        } else if (strcmp(tag, "frame") == 0 && doc->xml_frame_count < MAX_XML_FRAMES) {
            XmlFrameDef *fr = &doc->xml_frames[doc->xml_frame_count++];
            const char *pg = get_attr(attrs, attr_count, "page");
            fr->page_num = pg ? (size_t)strtoul(pg, nullptr, 10) : 1;
            fr->x_mm     = parse_mm(get_attr(attrs, attr_count, "x"));
            fr->y_mm     = parse_mm(get_attr(attrs, attr_count, "y"));
            fr->w_mm     = parse_mm(get_attr(attrs, attr_count, "width"));
            fr->h_mm     = parse_mm(get_attr(attrs, attr_count, "height"));
        } else if (strcmp(tag, "table") == 0 && !self_closing) {
            active_table = calloc(1, sizeof(TableDef));
            if (active_table) {
                const char *cols = get_attr(attrs, attr_count, "cols");
                const char *al   = get_attr(attrs, attr_count, "align");
                const char *tcs  = get_attr(attrs, attr_count, "tabcolsep");
                const char *rw   = get_attr(attrs, attr_count, "rulewidth");
                active_table->cols         = cols ? (size_t)strtoul(cols, nullptr, 10) : 1;
                active_table->tabcolsep_mm = tcs ? parse_mm(tcs) : (6.0 * MM_PER_PT);
                active_table->rulewidth_mm = rw ? parse_mm(rw) : (0.4 * MM_PER_PT);
                active_table->table_align  = ALIGN_LEFT;
                if (al) {
                    if (strcmp(al, "center") == 0)     active_table->table_align = ALIGN_CENTER;
                    else if (strcmp(al, "right") == 0) active_table->table_align = ALIGN_RIGHT;
                }
            }
        } else if (strcmp(tag, "hline") == 0 && active_table) {
            table_push_row(active_table, (TableRowItem){ .kind = TROW_HLINE });
        } else if (strcmp(tag, "cline") == 0 && active_table) {
            const char *fr_s = get_attr(attrs, attr_count, "from");
            const char *to_s = get_attr(attrs, attr_count, "to");
            table_push_row(active_table, (TableRowItem){
                .kind       = TROW_CLINE,
                .cline_from = fr_s ? atoi(fr_s) : 1,
                .cline_to   = to_s ? atoi(to_s) : 1
            });
        } else if (strcmp(tag, "row") == 0 && active_table && !self_closing) {
            memset(&current_row, 0, sizeof(current_row));
            current_row.kind = TROW_DATA;
            in_trow = true;
        } else if (strcmp(tag, "row-space") == 0 && in_trow) {
            const char *ex = get_attr(attrs, attr_count, "extra");
            if (ex) current_row.extra_space_mm += parse_mm(ex);
        } else if (strcmp(tag, "cell") == 0 && in_trow && !self_closing) {
            const char *sp = get_attr(attrs, attr_count, "span");
            const char *al = get_attr(attrs, attr_count, "align");
            const char *bl = get_attr(attrs, attr_count, "bl");
            const char *br = get_attr(attrs, attr_count, "br");
            const char *w  = get_attr(attrs, attr_count, "width");
            memset(&current_cell, 0, sizeof(current_cell));
            current_cell.span       = sp ? (size_t)strtoul(sp, nullptr, 10) : 1;
            current_cell.align      = (al && al[0]) ? al[0] : 'l';
            current_cell.bl         = bl ? atoi(bl) : 0;
            current_cell.br         = br ? atoi(br) : 0;
            current_cell.fixed_w_mm = w ? parse_mm(w) : 0.0;

            memset(&current_para, 0, sizeof(current_para));
            in_cell = true;
            in_para = true;
        } else if (strcmp(tag, "p") == 0 && !self_closing) {
            const char *st  = get_attr(attrs, attr_count, "style");
            const char *lbl = get_attr(attrs, attr_count, "label");
            const char *lst = get_attr(attrs, attr_count, "label-style");
            memset(&current_para, 0, sizeof(current_para));
            current_para.type            = ITEM_PARA;
            current_para.para_style_idx  = st ? find_para_style_idx(doc, st) : 0;
            current_para.label_style_idx = lst ? find_char_style_idx(doc, lst) : 0;
            if (lbl) {
                snprintf(current_para.label, sizeof(current_para.label), "%s", lbl);
            }
            in_para = true;
        } else if (strcmp(tag, "footnote") == 0 && !self_closing) {
            const char *lbl = get_attr(attrs, attr_count, "label");
            const char *st  = get_attr(attrs, attr_count, "style");
            memset(&current_para, 0, sizeof(current_para));
            current_para.type            = ITEM_FOOTNOTE;
            current_para.label_style_idx = st ? find_char_style_idx(doc, st) : 0;
            if (lbl) {
                snprintf(current_para.label, sizeof(current_para.label), "%s", lbl);
            }
            in_para = true;
        } else if (strcmp(tag, "marginpar") == 0 && !self_closing) {
            const char *st = get_attr(attrs, attr_count, "style");
            memset(&current_para, 0, sizeof(current_para));
            current_para.type            = ITEM_MARGINPAR;
            current_para.label_style_idx = st ? find_char_style_idx(doc, st) : 0;
            in_para = true;
        } else if (strcmp(tag, "span") == 0 && in_para && !self_closing) {
            const char *st = get_attr(attrs, attr_count, "style");
            active_cs_idx    = st ? find_char_style_idx(doc, st) : 0;
            span_buf.len     = 0;
            span_buf.data[0] = '\0';
            in_span          = true;
        } else if (strcmp(tag, "box") == 0 && in_para && !self_closing) {
            if (in_span && span_buf.len > 0) {
                para_push_run(&current_para, active_cs_idx, span_buf.data);
                span_buf.len = 0;
                span_buf.data[0] = '\0';
            }
            const char *w   = get_attr(attrs, attr_count, "width");
            const char *al  = get_attr(attrs, attr_count, "align");
            const char *brd = get_attr(attrs, attr_count, "border");
            const char *pad = get_attr(attrs, attr_count, "pad");
            const char *rs  = get_attr(attrs, attr_count, "raise");
            const char *st  = get_attr(attrs, attr_count, "style");
            memset(&pending_box, 0, sizeof(pending_box));
            pending_box.kind      = RUN_BOX;
            pending_box.style_idx = st ? find_char_style_idx(doc, st) : active_cs_idx;
            pending_box.w_mm      = w ? parse_mm(w) : 0.0;
            pending_box.border_mm = brd ? parse_mm(brd) : 0.0;
            pending_box.pad_mm    = pad ? parse_mm(pad) : 0.0;
            pending_box.raise_mm  = rs ? parse_mm(rs) : 0.0;
            pending_box.box_align = (al && al[0]) ? al[0] : 'c';
            box_buf.len           = 0;
            box_buf.data[0]       = '\0';
            in_box                = true;
        } else if (strcmp(tag, "br") == 0 && in_para) {
            if (in_span && span_buf.len > 0) {
                para_push_run(&current_para, active_cs_idx, span_buf.data);
                span_buf.len = 0;
                span_buf.data[0] = '\0';
            }
            const char *ex = get_attr(attrs, attr_count, "extra");
            para_push_special_run(&current_para, (SpanRun){
                .kind = RUN_LINE_BREAK,
                .h_mm = ex ? parse_mm(ex) : 0.0
            });
        } else if (strcmp(tag, "hspace") == 0 && in_para) {
            if (in_span && span_buf.len > 0) {
                para_push_run(&current_para, active_cs_idx, span_buf.data);
                span_buf.len = 0;
                span_buf.data[0] = '\0';
            }
            const char *w  = get_attr(attrs, attr_count, "width");
            const char *fl = get_attr(attrs, attr_count, "fill");
            const char *st = get_attr(attrs, attr_count, "style");
            HFillKind fk   = FILL_NONE;
            if (fl) {
                if (strcmp(fl, "space") == 0)     fk = FILL_SPACE;
                else if (strcmp(fl, "dots") == 0) fk = FILL_DOTS;
                else if (strcmp(fl, "rule") == 0) fk = FILL_RULE;
            }
            para_push_special_run(&current_para, (SpanRun){
                .kind      = RUN_HSPACE,
                .style_idx = st ? find_char_style_idx(doc, st) : active_cs_idx,
                .w_mm      = w ? parse_mm(w) : 0.0,
                .fill      = fk
            });
        } else if (strcmp(tag, "rule") == 0 && in_para) {
            if (in_span && span_buf.len > 0) {
                para_push_run(&current_para, active_cs_idx, span_buf.data);
                span_buf.len = 0;
                span_buf.data[0] = '\0';
            }
            const char *w  = get_attr(attrs, attr_count, "width");
            const char *h  = get_attr(attrs, attr_count, "height");
            const char *rs = get_attr(attrs, attr_count, "raise");
            const char *st = get_attr(attrs, attr_count, "style");
            para_push_special_run(&current_para, (SpanRun){
                .kind      = RUN_RULE,
                .style_idx = st ? find_char_style_idx(doc, st) : active_cs_idx,
                .w_mm      = w ? parse_mm(w) : 0.0,
                .h_mm      = h ? parse_mm(h) : 0.0,
                .raise_mm  = rs ? parse_mm(rs) : 0.0
            });
        } else if (strcmp(tag, "vspace") == 0) {
            const char *h  = get_attr(attrs, attr_count, "height");
            const char *st = get_attr(attrs, attr_count, "starred");
            const char *fl = get_attr(attrs, attr_count, "fill");
            StoryItem vs = {
                .type           = ITEM_VSPACE,
                .vspace_mm      = h ? parse_mm(h) : 0.0,
                .vspace_starred = (st && strcmp(st, "true") == 0),
                .vspace_fill    = (fl && strcmp(fl, "true") == 0)
            };
            doc_push_item(doc, vs);
        } else if (strcmp(tag, "break") == 0) {
            const char *tp = get_attr(attrs, attr_count, "type");
            if (!tp || strcmp(tp, "page") == 0) {
                StoryItem brk = { .type = ITEM_PAGE_BREAK };
                doc_push_item(doc, brk);
            }
        }
    }

    sb_free(&span_buf);
    sb_free(&box_buf);
    return true;
}

static void free_ir_document(IrDocument *doc) {
    for (size_t i = 0; i < doc->item_count; i++) {
        for (size_t j = 0; j < doc->items[i].run_count; j++) {
            free(doc->items[i].runs[j].text);
        }
        free(doc->items[i].runs);
        if (doc->items[i].table) {
            TableDef *tbl = doc->items[i].table;
            for (size_t r = 0; r < tbl->row_count; r++) {
                for (size_t c = 0; c < tbl->rows[r].cell_count; c++) {
                    for (size_t k = 0; k < tbl->rows[r].cells[c].run_count; k++) {
                        free(tbl->rows[r].cells[c].runs[k].text);
                    }
                    free(tbl->rows[r].cells[c].runs);
                }
                free(tbl->rows[r].cells);
            }
            free(tbl->rows);
            free(tbl);
        }
    }
    free(doc->items);
}

// ============================================================================
// 6. Métrica Tipográfica, División de Líneas y Flujo Multicaja
// ============================================================================

static double estimate_word_width_mm(const char *word, const CharStyle *cs, const FontVariant *fv) {
    bool is_mono = str_contains_ci(fv->family, "Mono") || str_contains_ci(fv->family, "Courier");
    double em = cs->size_mm;
    double total_em = 0.0;

    for (size_t i = 0; word[i] != '\0'; ) {
        unsigned char c = (unsigned char)word[i];
        size_t step = 1;
        if ((c & 0xE0) == 0xC0)      step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;

        if (is_mono) {
            total_em += 0.60;
        } else if (step == 2 && c == 0xC2 && (unsigned char)word[i + 1] == 0xA0) {
            total_em += 0.27;
        } else if (step == 1) {
            if (c == ' ')                                  total_em += 0.27;
            else if (strchr("ilIjtfr.,:;!|'()[]", (int)c)) total_em += 0.30;
            else if (strchr("mwMW@%", (int)c))             total_em += 0.80;
            else if (isupper((int)c))                      total_em += 0.64;
            else                                           total_em += 0.50;
        } else {
            total_em += 0.52;
        }

        for (size_t k = 0; k < step && word[i] != '\0'; k++) i++;
    }

    if (fv->bold && !is_mono) total_em *= 1.06;
    if (cs->small_caps && !is_mono) total_em *= 1.05;
    if (cs->shift != 0) total_em *= 0.72;
    return total_em * em;
}

static double estimate_space_width_mm(const CharStyle *cs, const FontVariant *fv) {
    bool is_mono = str_contains_ci(fv->family, "Mono") || str_contains_ci(fv->family, "Courier");
    double scale = (cs->shift != 0) ? 0.72 : 1.0;
    return cs->size_mm * scale * (is_mono ? 0.60 : 0.27);
}

static FlowFrame make_default_frame(const IrDocument *doc, size_t page_idx) {
    double h = doc->page_h_mm - doc->margin_top_mm - doc->margin_bottom_mm;
    FlowFrame f = {
        .page_idx  = page_idx,
        .x_mm      = doc->margin_left_mm,
        .y_mm      = doc->margin_top_mm,
        .w_mm      = doc->page_w_mm - doc->margin_left_mm - doc->margin_right_mm,
        .h_mm      = h,
        .orig_h_mm = h
    };
    sb_init(&f.svg_content, 2048);
    return f;
}

static void typesetter_init(Typesetter *ts, const IrDocument *doc) {
    memset(ts, 0, sizeof(*ts));
    ts->total_pages = (doc->declared_pages > 0) ? doc->declared_pages : 1;

    for (size_t p = 0; p < ts->total_pages; p++) {
        bool found_explicit = false;
        for (size_t i = 0; i < doc->xml_frame_count; i++) {
            if (doc->xml_frames[i].page_num == p + 1 && ts->frame_count < MAX_FLOW_FRAMES) {
                FlowFrame f = {
                    .page_idx  = p,
                    .x_mm      = doc->xml_frames[i].x_mm,
                    .y_mm      = doc->xml_frames[i].y_mm,
                    .w_mm      = doc->xml_frames[i].w_mm,
                    .h_mm      = doc->xml_frames[i].h_mm,
                    .orig_h_mm = doc->xml_frames[i].h_mm
                };
                sb_init(&f.svg_content, 2048);
                ts->frames[ts->frame_count++] = f;
                found_explicit = true;
            }
        }
        if (!found_explicit && ts->frame_count < MAX_FLOW_FRAMES) {
            ts->frames[ts->frame_count++] = make_default_frame(doc, p);
        }
    }

    ts->cur_frame    = 0;
    ts->cursor_y_mm  = 0.0;
    ts->at_frame_top = true;
}

static void typesetter_advance_frame(Typesetter *ts, const IrDocument *doc) {
    if (ts->cur_frame + 1 < ts->frame_count) {
        ts->cur_frame++;
    } else if (ts->frame_count < MAX_FLOW_FRAMES) {
        size_t new_page = ts->total_pages++;
        ts->frames[ts->frame_count++] = make_default_frame(doc, new_page);
        ts->cur_frame = ts->frame_count - 1;
    }
    ts->cursor_y_mm  = 0.0;
    ts->at_frame_top = true;
}

static void typesetter_page_break(Typesetter *ts, const IrDocument *doc) {
    size_t current_page = ts->frames[ts->cur_frame].page_idx;
    size_t target_page  = current_page + 1;

    for (size_t i = ts->cur_frame + 1; i < ts->frame_count; i++) {
        if (ts->frames[i].page_idx >= target_page) {
            ts->cur_frame    = i;
            ts->cursor_y_mm  = 0.0;
            ts->at_frame_top = true;
            return;
        }
    }

    if (ts->frame_count < MAX_FLOW_FRAMES) {
        while (ts->total_pages <= target_page) {
            ts->frames[ts->frame_count++] = make_default_frame(doc, ts->total_pages++);
        }
        ts->cur_frame    = ts->frame_count - 1;
        ts->cursor_y_mm  = 0.0;
        ts->at_frame_top = true;
    }
}

static void emit_svg_line(
    FlowFrame *frame,
    const IrDocument *doc,
    const ParaStyle *ps,
    const char *label,
    size_t label_cs_idx,
    const WordToken *words,
    size_t start_idx,
    size_t end_idx,
    double natural_w_mm,
    double line_indent_mm,
    double baseline_y_mm,
    bool is_last_line
) {
    // 1. Si es la primera línea de un elemento de lista con etiqueta (label="..."), trazarla en el margen izquierdo
    if (label && label[0] != '\0') {
        double lbl_x = frame->x_mm + ps->indent_left_mm - 2.2;
        const char *cs_id = (label_cs_idx < doc->char_style_count) ? doc->char_styles[label_cs_idx].id : "cs1";
        char lhdr[256];
        snprintf(lhdr, sizeof(lhdr),
                 "        <text x=\"%.3f\" y=\"%.3f\" text-anchor=\"end\"><tspan class=\"cs-%s\">",
                 lbl_x, frame->y_mm + baseline_y_mm, cs_id);
        sb_append_cstr(&frame->svg_content, lhdr);
        sb_append_escaped_xml(&frame->svg_content, label);
        sb_append_cstr(&frame->svg_content, "</tspan></text>\n");
    }

    if (start_idx >= end_idx) return;

    double avail_w = frame->w_mm - ps->indent_left_mm - ps->indent_right_mm - line_indent_mm;
    if (avail_w < 5.0) avail_w = frame->w_mm;

    bool has_special_inline = false;
    size_t fill_count = 0;
    for (size_t i = start_idx; i < end_idx; i++) {
        if (words[i].kind != RUN_TEXT) {
            has_special_inline = true;
            if (words[i].kind == RUN_HSPACE && words[i].fill != FILL_NONE) {
                fill_count++;
            }
        }
    }

    double x_pos = frame->x_mm + ps->indent_left_mm + line_indent_mm;
    if (fill_count == 0) {
        if (ps->align == ALIGN_CENTER) {
            x_pos += fmax(0.0, (avail_w - natural_w_mm) * 0.5);
        } else if (ps->align == ALIGN_RIGHT) {
            x_pos += fmax(0.0, avail_w - natural_w_mm);
        }
    }

    if (!has_special_inline) {
        size_t gaps = 0;
        for (size_t i = start_idx; i + 1 < end_idx; i++) {
            if (words[i].space_after) gaps++;
        }

        bool do_justify = (ps->align == ALIGN_JUSTIFY && !is_last_line && gaps > 0 && natural_w_mm > avail_w * 0.55);
        double extra_word_spacing = 0.0;
        if (do_justify && avail_w > natural_w_mm) {
            extra_word_spacing = (avail_w - natural_w_mm) / (double)gaps;
        }

        char hdr[256];
        if (do_justify && extra_word_spacing > 0.001) {
            snprintf(hdr, sizeof(hdr),
                     "        <text x=\"%.3f\" y=\"%.3f\" xml:space=\"preserve\" style=\"word-spacing:%.3fpx;\">",
                     x_pos, frame->y_mm + baseline_y_mm, extra_word_spacing);
        } else {
            snprintf(hdr, sizeof(hdr),
                     "        <text x=\"%.3f\" y=\"%.3f\" xml:space=\"preserve\">",
                     x_pos, frame->y_mm + baseline_y_mm);
        }
        sb_append_cstr(&frame->svg_content, hdr);

        size_t i = start_idx;
        while (i < end_idx) {
            size_t cs_idx = words[i].char_style_idx;
            char tspan_open[96];
            snprintf(tspan_open, sizeof(tspan_open), "<tspan class=\"cs-%s\">", doc->char_styles[cs_idx].id);
            sb_append_cstr(&frame->svg_content, tspan_open);

            while (i < end_idx && words[i].char_style_idx == cs_idx) {
                sb_append_escaped_xml(&frame->svg_content, words[i].word);
                if (i + 1 < end_idx && words[i].space_after) {
                    sb_append_cstr(&frame->svg_content, " ");
                }
                i++;
            }
            sb_append_cstr(&frame->svg_content, "</tspan>");
        }

        sb_append_cstr(&frame->svg_content, "</text>\n");
        return;
    }

    // 2. Línea con elementos inline especiales (<hspace>, \hfill, \dotfill, \hrulefill, <rule>)
    double each_fill_w = (fill_count > 0 && avail_w > natural_w_mm)
                         ? ((avail_w - natural_w_mm) / (double)fill_count)
                         : 0.0;
    double cur_x = x_pos;
    double abs_y = frame->y_mm + baseline_y_mm;

    size_t i = start_idx;
    while (i < end_idx) {
        if (words[i].kind == RUN_TEXT) {
            char hdr[256];
            snprintf(hdr, sizeof(hdr),
                     "        <text x=\"%.3f\" y=\"%.3f\" xml:space=\"preserve\">",
                     cur_x, abs_y);
            sb_append_cstr(&frame->svg_content, hdr);

            while (i < end_idx && words[i].kind == RUN_TEXT) {
                size_t cs_idx = words[i].char_style_idx;
                char tspan_open[96];
                snprintf(tspan_open, sizeof(tspan_open), "<tspan class=\"cs-%s\">", doc->char_styles[cs_idx].id);
                sb_append_cstr(&frame->svg_content, tspan_open);

                while (i < end_idx && words[i].kind == RUN_TEXT && words[i].char_style_idx == cs_idx) {
                    sb_append_escaped_xml(&frame->svg_content, words[i].word);
                    cur_x += words[i].word_w_mm;
                    if (i + 1 < end_idx && words[i].space_after) {
                        if (words[i + 1].kind == RUN_TEXT) {
                            sb_append_cstr(&frame->svg_content, " ");
                        }
                        cur_x += words[i].space_w_mm;
                    }
                    i++;
                }
                sb_append_cstr(&frame->svg_content, "</tspan>");
            }
            sb_append_cstr(&frame->svg_content, "</text>\n");
        } else if (words[i].kind == RUN_HSPACE) {
            const CharStyle *cs = (words[i].char_style_idx < doc->char_style_count)
                                  ? &doc->char_styles[words[i].char_style_idx]
                                  : &(CharStyle){ .color = "Black" };
            if (words[i].fill == FILL_NONE) {
                cur_x += words[i].word_w_mm;
            } else if (words[i].fill == FILL_SPACE) {
                cur_x += each_fill_w;
            } else if (words[i].fill == FILL_DOTS) {
                if (each_fill_w > 2.0) {
                    char line_svg[256];
                    snprintf(line_svg, sizeof(line_svg),
                             "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                             "stroke=\"%s\" stroke-width=\"0.45\" stroke-dasharray=\"0.5, 1.8\" stroke-linecap=\"round\"/>\n",
                             cur_x + 0.8, abs_y - 0.35, cur_x + each_fill_w - 0.8, abs_y - 0.35, cs->color);
                    sb_append_cstr(&frame->svg_content, line_svg);
                }
                cur_x += each_fill_w;
            } else if (words[i].fill == FILL_RULE) {
                if (each_fill_w > 1.0) {
                    char line_svg[256];
                    snprintf(line_svg, sizeof(line_svg),
                             "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                             "stroke=\"%s\" stroke-width=\"0.35\"/>\n",
                             cur_x + 0.4, abs_y - 0.35, cur_x + each_fill_w - 0.4, abs_y - 0.35, cs->color);
                    sb_append_cstr(&frame->svg_content, line_svg);
                }
                cur_x += each_fill_w;
            }
            i++;
        } else if (words[i].kind == RUN_RULE) {
            const CharStyle *cs = (words[i].char_style_idx < doc->char_style_count)
                                  ? &doc->char_styles[words[i].char_style_idx]
                                  : &(CharStyle){ .color = "Black" };
            double rw = words[i].word_w_mm;
            double rh = words[i].h_mm;
            double rr = words[i].raise_mm;
            if (rw > 0.01 && rh > 0.01) {
                char rect_svg[256];
                snprintf(rect_svg, sizeof(rect_svg),
                         "        <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" fill=\"%s\"/>\n",
                         cur_x, abs_y - rr - rh, rw, rh, cs->color);
                sb_append_cstr(&frame->svg_content, rect_svg);
            }
            cur_x += rw;
            if (i + 1 < end_idx && words[i].space_after) {
                cur_x += words[i].space_w_mm;
            }
            i++;
        } else if (words[i].kind == RUN_BOX) {
            size_t cs_idx = (words[i].char_style_idx < doc->char_style_count) ? words[i].char_style_idx : 0;
            const CharStyle *cs = &doc->char_styles[cs_idx];
            double box_w   = words[i].word_w_mm;
            double text_w  = words[i].h_mm;
            double brd     = words[i].border_mm;
            double pad     = words[i].pad_mm;
            double raise   = words[i].raise_mm;
            double inner_w = fmax(0.0, box_w - 2.0 * pad - 2.0 * brd);
            double font_h  = (cs->size_mm > 0.1) ? cs->size_mm : 3.88;

            if (brd > 0.005) {
                double rx = cur_x + brd * 0.5;
                double rw = fmax(0.1, box_w - brd);
                double rh = font_h * 1.15 + 2.0 * pad;
                double ry = (abs_y - raise) - font_h * 0.84 - pad;
                char rect_svg[320];
                snprintf(rect_svg, sizeof(rect_svg),
                         "        <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" "
                         "fill=\"none\" stroke=\"%s\" stroke-width=\"%.3f\"/>\n",
                         rx, ry, rw, rh, cs->color, brd);
                sb_append_cstr(&frame->svg_content, rect_svg);
            }

            if (words[i].word && words[i].word[0] != '\0') {
                double tx = cur_x + brd + pad;
                if (words[i].box_align == 'c') {
                    tx += fmax(0.0, (inner_w - text_w) * 0.5);
                } else if (words[i].box_align == 'r') {
                    tx += fmax(0.0, inner_w - text_w);
                }
                char btxt[256];
                snprintf(btxt, sizeof(btxt),
                         "        <text x=\"%.3f\" y=\"%.3f\" xml:space=\"preserve\"><tspan class=\"cs-%s\">",
                         tx, abs_y - raise, cs->id);
                sb_append_cstr(&frame->svg_content, btxt);
                sb_append_escaped_xml(&frame->svg_content, words[i].word);
                sb_append_cstr(&frame->svg_content, "</tspan></text>\n");
            }

            cur_x += box_w;
            if (i + 1 < end_idx && words[i].space_after) {
                cur_x += words[i].space_w_mm;
            }
            i++;
        } else {
            i++;
        }
    }
}

static WordToken *tokenize_runs(const IrDocument *doc, const SpanRun *runs, size_t run_count, size_t *out_count) {
    *out_count = 0;
    size_t cap = 64, count = 0;
    WordToken *words = malloc(cap * sizeof(WordToken));
    if (!words) return nullptr;

    for (size_t r = 0; r < run_count; r++) {
        const SpanRun *run = &runs[r];
        size_t cs_idx = (run->style_idx < doc->char_style_count) ? run->style_idx : 0;
        const CharStyle *cs = &doc->char_styles[cs_idx];
        const FontVariant *fv = &doc->variants[cs->font_variant_idx];

        if (run->kind == RUN_BOX) {
            if (count >= cap) {
                cap *= 2;
                words = realloc(words, cap * sizeof(WordToken));
            }
            const char *bstr = run->text ? run->text : "";
            double text_w  = estimate_word_width_mm(bstr, cs, fv);
            double inner_w = fmax(run->w_mm, text_w);
            double total_w = inner_w + 2.0 * run->pad_mm + 2.0 * run->border_mm;
            words[count++] = (WordToken){
                .kind           = RUN_BOX,
                .char_style_idx = cs_idx,
                .word           = dup_cstr(bstr),
                .word_w_mm      = total_w,
                .space_w_mm     = estimate_space_width_mm(cs, fv),
                .h_mm           = text_w,
                .raise_mm       = run->raise_mm,
                .border_mm      = run->border_mm,
                .pad_mm         = run->pad_mm,
                .box_align      = run->box_align ? run->box_align : 'c',
                .fill           = FILL_NONE,
                .space_after    = false
            };
            continue;
        }

        if (run->kind != RUN_TEXT) {
            if (count >= cap) {
                cap *= 2;
                words = realloc(words, cap * sizeof(WordToken));
            }
            words[count++] = (WordToken){
                .kind           = run->kind,
                .char_style_idx = cs_idx,
                .word           = nullptr,
                .word_w_mm      = (run->kind == RUN_HSPACE && run->fill != FILL_NONE) ? 0.0 : run->w_mm,
                .space_w_mm     = estimate_space_width_mm(cs, fv),
                .h_mm           = run->h_mm,
                .raise_mm       = run->raise_mm,
                .fill           = run->fill,
                .space_after    = false
            };
            continue;
        }

        const char *s = run->text;
        if (!s) continue;
        size_t pos = 0;

        while (s[pos] != '\0') {
            if (isspace((unsigned char)s[pos])) {
                if (count > 0 && words[count - 1].kind != RUN_LINE_BREAK && !words[count - 1].space_after) {
                    words[count - 1].space_after = true;
                }
                while (s[pos] != '\0' && isspace((unsigned char)s[pos])) pos++;
                continue;
            }

            size_t w_start = pos;
            while (s[pos] != '\0' && !isspace((unsigned char)s[pos])) pos++;
            size_t w_len = pos - w_start;

            bool trailing_space = false;
            while (s[pos] != '\0' && isspace((unsigned char)s[pos])) {
                trailing_space = true;
                pos++;
            }

            char *w_str = malloc(w_len + 1);
            memcpy(w_str, s + w_start, w_len);
            w_str[w_len] = '\0';

            if (count >= cap) {
                cap *= 2;
                words = realloc(words, cap * sizeof(WordToken));
            }
            words[count++] = (WordToken){
                .kind           = RUN_TEXT,
                .char_style_idx = cs_idx,
                .word           = w_str,
                .word_w_mm      = estimate_word_width_mm(w_str, cs, fv),
                .space_w_mm     = estimate_space_width_mm(cs, fv),
                .h_mm           = 0.0,
                .raise_mm       = 0.0,
                .fill           = FILL_NONE,
                .space_after    = trailing_space
            };
        }
    }

    *out_count = count;
    return words;
}

static void free_word_tokens(WordToken *words, size_t count) {
    if (!words) return;
    for (size_t i = 0; i < count; i++) free(words[i].word);
    free(words);
}

static void typeset_paragraph(Typesetter *ts, const IrDocument *doc, const StoryItem *item) {
    if (item->run_count == 0 && item->label[0] == '\0') return;

    const ParaStyle *ps = (item->para_style_idx < doc->para_style_count)
                          ? &doc->para_styles[item->para_style_idx]
                          : &(ParaStyle){ .align = ALIGN_LEFT, .line_spacing_mm = 5.2 };

    size_t count = 0;
    WordToken *words = tokenize_runs(doc, item->runs, item->run_count, &count);
    if (!words || count == 0) {
        free_word_tokens(words, count);
        return;
    }

    if (!ts->at_frame_top) {
        ts->cursor_y_mm += ps->space_before_mm;
    }

    // 2. Composición voraz línea a línea respetando márgenes, sangría, <br/> y desbordamiento de caja
    size_t idx = 0;
    bool first_line = true;

    while (idx < count) {
        FlowFrame *fr = &ts->frames[ts->cur_frame];
        double line_h = ps->line_spacing_mm;

        if (ts->cursor_y_mm + line_h > fr->h_mm + 0.05 && !ts->at_frame_top) {
            typesetter_advance_frame(ts, doc);
            fr = &ts->frames[ts->cur_frame];
        }

        double indent = first_line ? ps->indent_first_mm : 0.0;
        double max_w  = fr->w_mm - ps->indent_left_mm - ps->indent_right_mm - indent;
        if (max_w < 10.0) max_w = fr->w_mm;

        size_t line_end      = idx;
        double acc_w         = 0.0;
        bool forced_break    = false;
        double brk_extra_mm  = 0.0;

        while (line_end < count) {
            if (words[line_end].kind == RUN_LINE_BREAK) {
                forced_break = true;
                brk_extra_mm = words[line_end].h_mm;
                break;
            }
            double space_before_word = (line_end > idx && words[line_end - 1].space_after)
                                       ? words[line_end - 1].space_w_mm
                                       : 0.0;
            double candidate_w = acc_w + space_before_word + words[line_end].word_w_mm;
            if (line_end > idx && candidate_w > max_w) {
                break;
            }
            acc_w = candidate_w;
            line_end++;
        }

        size_t next_idx = forced_break ? (line_end + 1) : line_end;
        bool is_last    = (next_idx >= count) || forced_break;

        double max_font_mm = 0.0;
        for (size_t k = idx; k < line_end; k++) {
            double sz = doc->char_styles[words[k].char_style_idx].size_mm;
            if (sz > max_font_mm) max_font_mm = sz;
        }
        if (max_font_mm <= 0.1 && doc->char_style_count > 0) {
            max_font_mm = doc->char_styles[0].size_mm;
        }
        double baseline_offset = fmax(max_font_mm * 0.82, line_h * 0.76);

        emit_svg_line(fr, doc, ps,
                      first_line ? item->label : nullptr,
                      item->label_style_idx,
                      words, idx, line_end, acc_w, indent,
                      ts->cursor_y_mm + baseline_offset, is_last);

        ts->cursor_y_mm += line_h + brk_extra_mm;
        ts->at_frame_top = false;
        first_line       = false;
        idx              = next_idx;
    }

    ts->cursor_y_mm += ps->space_after_mm;
    free_word_tokens(words, count);
}

static double measure_tokens_natural_width(const WordToken *words, size_t count) {
    double max_line_w = 0.0;
    double cur_w = 0.0;
    for (size_t i = 0; i < count; i++) {
        if (words[i].kind == RUN_LINE_BREAK) {
            if (cur_w > max_line_w) max_line_w = cur_w;
            cur_w = 0.0;
            continue;
        }
        cur_w += words[i].word_w_mm;
        if (i + 1 < count && words[i].space_after && words[i + 1].kind != RUN_LINE_BREAK) {
            cur_w += words[i].space_w_mm;
        }
    }
    if (cur_w > max_line_w) max_line_w = cur_w;
    return max_line_w;
}

static size_t count_wrapped_lines(const WordToken *words, size_t count, double max_w) {
    if (count == 0) return 1;
    if (max_w < 5.0) max_w = 5.0;
    size_t lines = 0;
    size_t idx = 0;
    while (idx < count) {
        size_t line_end = idx;
        double acc_w = 0.0;
        bool forced = false;
        while (line_end < count) {
            if (words[line_end].kind == RUN_LINE_BREAK) {
                forced = true;
                break;
            }
            double sp = (line_end > idx && words[line_end - 1].space_after) ? words[line_end - 1].space_w_mm : 0.0;
            if (line_end > idx && acc_w + sp + words[line_end].word_w_mm > max_w) break;
            acc_w += sp + words[line_end].word_w_mm;
            line_end++;
        }
        lines++;
        idx = forced ? (line_end + 1) : line_end;
    }
    return (lines > 0) ? lines : 1;
}

static void emit_vborder_svg(StrBuf *sb, double x, double y1, double y2, int count, double rw) {
    if (count <= 0) return;
    char buf[256];
    if (count == 1) {
        snprintf(buf, sizeof(buf),
                 "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"Black\" stroke-width=\"%.3f\"/>\n",
                 x, y1, x, y2, rw);
        sb_append_cstr(sb, buf);
    } else {
        snprintf(buf, sizeof(buf),
                 "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"Black\" stroke-width=\"%.3f\"/>\n"
                 "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"Black\" stroke-width=\"%.3f\"/>\n",
                 x - 0.40, y1, x - 0.40, y2, rw,
                 x + 0.40, y1, x + 0.40, y2, rw);
        sb_append_cstr(sb, buf);
    }
}

static void typeset_table(Typesetter *ts, const IrDocument *doc, const TableDef *tbl) {
    if (!tbl || tbl->cols == 0 || tbl->row_count == 0) return;
    size_t ncols = (tbl->cols <= 32) ? tbl->cols : 32;
    double pad_x = tbl->tabcolsep_mm;
    double rw    = (tbl->rulewidth_mm > 0.05) ? tbl->rulewidth_mm : 0.15;

    // 1. Calcular el ancho de cada columna en dos pasadas (celdas de 1 col y luego multicolumna)
    double col_w[32];
    for (size_t c = 0; c < ncols; c++) col_w[c] = 5.0;

    for (size_t r = 0; r < tbl->row_count; r++) {
        const TableRowItem *row = &tbl->rows[r];
        if (row->kind != TROW_DATA) continue;
        size_t cur_c = 0;
        for (size_t i = 0; i < row->cell_count && cur_c < ncols; i++) {
            const TableCell *cell = &row->cells[i];
            size_t span = (cell->span >= 1) ? cell->span : 1;
            if (cur_c + span > ncols) span = ncols - cur_c;
            if (span == 1) {
                if (cell->fixed_w_mm > 0.01) {
                    if (cell->fixed_w_mm > col_w[cur_c]) col_w[cur_c] = cell->fixed_w_mm;
                } else {
                    size_t wcount = 0;
                    WordToken *wt = tokenize_runs(doc, cell->runs, cell->run_count, &wcount);
                    double nw = measure_tokens_natural_width(wt, wcount);
                    free_word_tokens(wt, wcount);
                    if (nw > col_w[cur_c]) col_w[cur_c] = nw;
                }
            }
            cur_c += span;
        }
    }

    for (size_t r = 0; r < tbl->row_count; r++) {
        const TableRowItem *row = &tbl->rows[r];
        if (row->kind != TROW_DATA) continue;
        size_t cur_c = 0;
        for (size_t i = 0; i < row->cell_count && cur_c < ncols; i++) {
            const TableCell *cell = &row->cells[i];
            size_t span = (cell->span >= 1) ? cell->span : 1;
            if (cur_c + span > ncols) span = ncols - cur_c;
            if (span > 1) {
                size_t wcount = 0;
                WordToken *wt = tokenize_runs(doc, cell->runs, cell->run_count, &wcount);
                double nw = measure_tokens_natural_width(wt, wcount);
                free_word_tokens(wt, wcount);

                double cur_span_w = (double)(span - 1) * 2.0 * pad_x;
                for (size_t k = 0; k < span; k++) cur_span_w += col_w[cur_c + k];
                if (nw > cur_span_w) {
                    double add_each = (nw - cur_span_w) / (double)span;
                    for (size_t k = 0; k < span; k++) col_w[cur_c + k] += add_each;
                }
            }
            cur_c += span;
        }
    }

    double total_w = 0.0;
    for (size_t c = 0; c < ncols; c++) total_w += col_w[c] + 2.0 * pad_x;

    if (!ts->at_frame_top) {
        ts->cursor_y_mm += 2.2;
    }

    // 2. Recorrer filas y trazos horizontales
    bool prev_was_hline = false;
    for (size_t r = 0; r < tbl->row_count; r++) {
        FlowFrame *fr = &ts->frames[ts->cur_frame];
        double table_x = fr->x_mm;
        if (tbl->table_align == ALIGN_CENTER) {
            table_x += fmax(0.0, (fr->w_mm - total_w) * 0.5);
        } else if (tbl->table_align == ALIGN_RIGHT) {
            table_x += fmax(0.0, fr->w_mm - total_w);
        }

        double col_x[33];
        col_x[0] = table_x;
        for (size_t c = 0; c < ncols; c++) {
            col_x[c + 1] = col_x[c] + col_w[c] + 2.0 * pad_x;
        }

        const TableRowItem *row = &tbl->rows[r];
        if (row->kind == TROW_HLINE) {
            if (prev_was_hline) {
                ts->cursor_y_mm += 0.85;
            }
            double y = fr->y_mm + ts->cursor_y_mm;
            char lbuf[256];
            snprintf(lbuf, sizeof(lbuf),
                     "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"Black\" stroke-width=\"%.3f\"/>\n",
                     col_x[0], y, col_x[ncols], y, rw);
            sb_append_cstr(&fr->svg_content, lbuf);
            prev_was_hline = true;
            continue;
        }

        if (row->kind == TROW_CLINE) {
            size_t c1 = (row->cline_from >= 1) ? (size_t)(row->cline_from - 1) : 0;
            size_t c2 = (row->cline_to >= 1) ? (size_t)row->cline_to : 1;
            if (c1 > ncols) c1 = ncols;
            if (c2 > ncols) c2 = ncols;
            if (c2 > c1) {
                double y = fr->y_mm + ts->cursor_y_mm;
                char lbuf[256];
                snprintf(lbuf, sizeof(lbuf),
                         "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"Black\" stroke-width=\"%.3f\"/>\n",
                         col_x[c1], y, col_x[c2], y, rw);
                sb_append_cstr(&fr->svg_content, lbuf);
            }
            prev_was_hline = false;
            continue;
        }

        prev_was_hline = false;

        // Calcular la altura de la fila según el número máximo de líneas en sus celdas
        size_t max_lines = 1;
        size_t cur_c = 0;
        for (size_t i = 0; i < row->cell_count && cur_c < ncols; i++) {
            const TableCell *cell = &row->cells[i];
            size_t span = (cell->span >= 1) ? cell->span : 1;
            if (cur_c + span > ncols) span = ncols - cur_c;
            double cw = (col_x[cur_c + span] - col_x[cur_c]) - 2.0 * pad_x;

            size_t wcount = 0;
            WordToken *wt = tokenize_runs(doc, cell->runs, cell->run_count, &wcount);
            size_t nl = count_wrapped_lines(wt, wcount, cw + 0.5);
            free_word_tokens(wt, wcount);
            if (nl > max_lines) max_lines = nl;
            cur_c += span;
        }

        double line_h = 4.6;
        double row_h  = (double)max_lines * line_h + 1.6 + row->extra_space_mm;

        if (ts->cursor_y_mm + row_h > fr->h_mm + 0.05 && !ts->at_frame_top) {
            typesetter_advance_frame(ts, doc);
            fr = &ts->frames[ts->cur_frame];
            table_x = fr->x_mm;
            if (tbl->table_align == ALIGN_CENTER) {
                table_x += fmax(0.0, (fr->w_mm - total_w) * 0.5);
            } else if (tbl->table_align == ALIGN_RIGHT) {
                table_x += fmax(0.0, fr->w_mm - total_w);
            }
            col_x[0] = table_x;
            for (size_t c = 0; c < ncols; c++) {
                col_x[c + 1] = col_x[c] + col_w[c] + 2.0 * pad_x;
            }
        }

        double y_top = fr->y_mm + ts->cursor_y_mm;
        double y_bot = y_top + row_h;

        cur_c = 0;
        for (size_t i = 0; i < row->cell_count && cur_c < ncols; i++) {
            const TableCell *cell = &row->cells[i];
            size_t span = (cell->span >= 1) ? cell->span : 1;
            if (cur_c + span > ncols) span = ncols - cur_c;

            double x_left  = col_x[cur_c];
            double x_right = col_x[cur_c + span];
            emit_vborder_svg(&fr->svg_content, x_left,  y_top, y_bot, cell->bl, rw);
            emit_vborder_svg(&fr->svg_content, x_right, y_top, y_bot, cell->br, rw);

            size_t wcount = 0;
            WordToken *words = tokenize_runs(doc, cell->runs, cell->run_count, &wcount);
            if (words && wcount > 0) {
                ParaStyle cell_ps = {
                    .align           = (cell->align == 'c') ? ALIGN_CENTER
                                     : (cell->align == 'r') ? ALIGN_RIGHT
                                     : (cell->align == 'p') ? ALIGN_JUSTIFY
                                     : ALIGN_LEFT,
                    .line_spacing_mm = line_h,
                    .indent_left_mm  = (x_left + pad_x) - fr->x_mm,
                    .indent_right_mm = (fr->x_mm + fr->w_mm) - (x_right - pad_x)
                };
                double max_w = (x_right - x_left) - 2.0 * pad_x + 0.5;
                size_t idx = 0;
                size_t line_no = 0;
                while (idx < wcount) {
                    size_t line_end = idx;
                    double acc_w = 0.0;
                    bool forced = false;
                    while (line_end < wcount) {
                        if (words[line_end].kind == RUN_LINE_BREAK) {
                            forced = true;
                            break;
                        }
                        double sp = (line_end > idx && words[line_end - 1].space_after)
                                    ? words[line_end - 1].space_w_mm
                                    : 0.0;
                        if (line_end > idx && acc_w + sp + words[line_end].word_w_mm > max_w) break;
                        acc_w += sp + words[line_end].word_w_mm;
                        line_end++;
                    }
                    size_t next_idx = forced ? (line_end + 1) : line_end;
                    bool is_last = (next_idx >= wcount) || forced;
                    double base_y = ts->cursor_y_mm + 0.8 + (double)line_no * line_h + line_h * 0.76;
                    emit_svg_line(fr, doc, &cell_ps, nullptr, 0, words, idx, line_end, acc_w, 0.0, base_y, is_last);
                    line_no++;
                    idx = next_idx;
                }
            }
            free_word_tokens(words, wcount);
            cur_c += span;
        }

        ts->cursor_y_mm += row_h;
        ts->at_frame_top = false;
    }

    ts->cursor_y_mm += 2.2;
}

static void typeset_footnote(Typesetter *ts, const IrDocument *doc, const StoryItem *item) {
    FlowFrame *fr = &ts->frames[ts->cur_frame];
    if (fr->footnote_count >= 16) return;

    size_t wcount = 0;
    WordToken *wt = tokenize_runs(doc, item->runs, item->run_count, &wcount);
    size_t nl = count_wrapped_lines(wt, wcount, fr->w_mm - 5.0);
    free_word_tokens(wt, wcount);

    double fn_h = (double)nl * 3.8 + 0.8;
    if (fr->footnote_count == 0) {
        fn_h += 3.2; // espacio para la fileta separadora de notas al pie
    }
    if (fr->h_mm > ts->cursor_y_mm + fn_h + 4.0) {
        fr->h_mm -= fn_h;
    }
    fr->footnotes[fr->footnote_count++] = item;
}

static void finalize_frame_footnotes(FlowFrame *fr, const IrDocument *doc) {
    if (fr->footnote_count == 0) return;

    double total_fn_h = 0.0;
    for (size_t f = 0; f < fr->footnote_count; f++) {
        const StoryItem *item = fr->footnotes[f];
        size_t wcount = 0;
        WordToken *wt = tokenize_runs(doc, item->runs, item->run_count, &wcount);
        size_t nl = count_wrapped_lines(wt, wcount, fr->w_mm - 5.0);
        free_word_tokens(wt, wcount);
        total_fn_h += (double)nl * 3.8 + 0.8;
    }

    double cur_y = fr->orig_h_mm - total_fn_h;
    double rule_y = fr->y_mm + cur_y - 1.4;
    char rbuf[256];
    snprintf(rbuf, sizeof(rbuf),
             "        <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"#334155\" stroke-width=\"0.28\"/>\n",
             fr->x_mm, rule_y, fr->x_mm + fr->w_mm * 0.38, rule_y);
    sb_append_cstr(&fr->svg_content, rbuf);

    ParaStyle fn_ps = {
        .align           = ALIGN_JUSTIFY,
        .line_spacing_mm = 3.8,
        .indent_left_mm  = 4.5,
        .indent_right_mm = 0.0
    };

    for (size_t f = 0; f < fr->footnote_count; f++) {
        const StoryItem *item = fr->footnotes[f];
        size_t wcount = 0;
        WordToken *words = tokenize_runs(doc, item->runs, item->run_count, &wcount);
        double max_w = fr->w_mm - fn_ps.indent_left_mm;
        size_t idx = 0;
        bool first = true;
        while (idx < wcount) {
            size_t line_end = idx;
            double acc_w = 0.0;
            bool forced = false;
            while (line_end < wcount) {
                if (words[line_end].kind == RUN_LINE_BREAK) { forced = true; break; }
                double sp = (line_end > idx && words[line_end - 1].space_after) ? words[line_end - 1].space_w_mm : 0.0;
                if (line_end > idx && acc_w + sp + words[line_end].word_w_mm > max_w) break;
                acc_w += sp + words[line_end].word_w_mm;
                line_end++;
            }
            size_t next_idx = forced ? (line_end + 1) : line_end;
            bool is_last = (next_idx >= wcount) || forced;
            emit_svg_line(fr, doc, &fn_ps,
                          first ? item->label : nullptr,
                          item->label_style_idx,
                          words, idx, line_end, acc_w, 0.0,
                          cur_y + 2.9, is_last);
            cur_y += 3.8;
            first = false;
            idx = next_idx;
        }
        cur_y += 0.8;
        free_word_tokens(words, wcount);
    }
}

static void typeset_marginpar(Typesetter *ts, const IrDocument *doc, const StoryItem *item) {
    FlowFrame *fr = &ts->frames[ts->cur_frame];
    size_t wcount = 0;
    WordToken *words = tokenize_runs(doc, item->runs, item->run_count, &wcount);
    if (!words || wcount == 0) {
        free_word_tokens(words, wcount);
        return;
    }

    double mp_w = fmax(12.0, doc->margin_right_mm - 5.0);
    ParaStyle mp_ps = {
        .align           = ALIGN_LEFT,
        .line_spacing_mm = 3.5,
        .indent_left_mm  = fr->w_mm + 2.5,
        .indent_right_mm = -(mp_w + 2.5)
    };

    double y_off = ts->cursor_y_mm;
    size_t idx = 0;
    while (idx < wcount) {
        size_t line_end = idx;
        double acc_w = 0.0;
        bool forced = false;
        while (line_end < wcount) {
            if (words[line_end].kind == RUN_LINE_BREAK) { forced = true; break; }
            double sp = (line_end > idx && words[line_end - 1].space_after) ? words[line_end - 1].space_w_mm : 0.0;
            if (line_end > idx && acc_w + sp + words[line_end].word_w_mm > mp_w) break;
            acc_w += sp + words[line_end].word_w_mm;
            line_end++;
        }
        size_t next_idx = forced ? (line_end + 1) : line_end;
        emit_svg_line(fr, doc, &mp_ps, nullptr, 0, words, idx, line_end, acc_w, 0.0, y_off + 2.7, true);
        y_off += 3.5;
        idx = next_idx;
    }
    free_word_tokens(words, wcount);
}

static void format_page_num(size_t page_num, const char *numbering, char *out, size_t out_sz) {
    if (!numbering || strcmp(numbering, "arabic") == 0) {
        snprintf(out, out_sz, "%zu", page_num);
        return;
    }
    if (strcmp(numbering, "alph") == 0 || strcmp(numbering, "Alph") == 0) {
        char base = (numbering[0] == 'A') ? 'A' : 'a';
        size_t idx = (page_num >= 1 && page_num <= 26) ? (page_num - 1) : 0;
        snprintf(out, out_sz, "%c", (char)(base + idx));
        return;
    }
    if (strcmp(numbering, "roman") == 0 || strcmp(numbering, "Roman") == 0) {
        bool upper = (numbering[0] == 'R');
        static const int vals[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
        static const char *const syms_l[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
        static const char *const syms_u[] = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
        const char *const *syms = upper ? syms_u : syms_l;
        int v = (int)page_num;
        out[0] = '\0';
        size_t len = 0;
        for (size_t i = 0; i < 13 && v > 0; i++) {
            while (v >= vals[i]) {
                size_t sl = strlen(syms[i]);
                if (len + sl < out_sz - 1) {
                    memcpy(out + len, syms[i], sl);
                    len += sl;
                    out[len] = '\0';
                }
                v -= vals[i];
            }
        }
        return;
    }
    snprintf(out, out_sz, "%zu", page_num);
}

// ============================================================================
// 7. Emisión del Fichero Autocontenido SVG + JS ("ViewBox Sliding" Effect)
// ============================================================================

static bool write_standalone_svg(const char *svg_path, const IrDocument *doc, const Typesetter *ts) {
    FILE *out = fopen(svg_path, "w");
    if (!out) return false;

    double view_w   = doc->page_w_mm + 2.0 * DESK_PAD_X;
    double view_h   = doc->page_h_mm + DESK_PAD_TOP + DESK_PAD_BOTTOM;
    double stride_x = view_w + PAGE_STRIDE_GAP;
    double strip_w  = (double)ts->total_pages * stride_x;

    fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(out,
            "<svg id=\"doc-root\" xmlns=\"http://www.w3.org/2000/svg\" "
            "width=\"100%%\" height=\"100%%\" "
            "viewBox=\"0 0 %.3f %.3f\" "
            "preserveAspectRatio=\"xMidYMid meet\">\n",
            view_w, view_h);

    // 1. Definiciones gráficas (sombra de hoja) y hoja de estilos CSS desde <definitions>
    fprintf(out, "  <defs>\n");
    fprintf(out, "    <filter id=\"page-shadow\" x=\"-8%%\" y=\"-5%%\" width=\"116%%\" height=\"114%%\">\n");
    fprintf(out, "      <feDropShadow dx=\"0\" dy=\"2.2\" stdDeviation=\"2.8\" flood-color=\"#0b0f19\" flood-opacity=\"0.38\"/>\n");
    fprintf(out, "    </filter>\n");
    fprintf(out, "  </defs>\n");

    fprintf(out, "  <style><![CDATA[\n");
    fprintf(out, "    svg { background-color: #1e2430; user-select: text; -webkit-user-select: text; }\n");
    fprintf(out, "    .page-sheet { fill: #ffffff; stroke: #cbd5e1; stroke-width: 0.25px; }\n");
    fprintf(out, "    .page-num-footer { font-family: sans-serif; font-size: 3.2px; fill: #64748b; user-select: none; }\n");
    fprintf(out, "    .hud-pill { fill: #0f172a; fill-opacity: 0.88; stroke: #334155; stroke-width: 0.35px; }\n");
    fprintf(out, "    .hud-btn { cursor: pointer; fill: #1e293b; stroke: #475569; stroke-width: 0.3px; transition: fill 0.15s; }\n");
    fprintf(out, "    .hud-btn:hover { fill: #334155; }\n");
    fprintf(out, "    .hud-btn-disabled { opacity: 0.38; cursor: default; pointer-events: none; }\n");
    fprintf(out, "    .hud-label { font-family: system-ui, sans-serif; font-size: 3.6px; fill: #f8fafc; user-select: none; pointer-events: none; }\n");

    for (size_t i = 0; i < doc->char_style_count; i++) {
        const CharStyle *cs = &doc->char_styles[i];
        const FontVariant *fv = (cs->font_variant_idx < doc->variant_count)
                                ? &doc->variants[cs->font_variant_idx]
                                : &(FontVariant){ .family = "sans-serif" };
        double eff_size = (cs->shift != 0) ? (cs->size_mm * 0.72) : cs->size_mm;
        const char *shift_css = "";
        if (cs->shift == 1)      shift_css = " baseline-shift: super;";
        else if (cs->shift == 2) shift_css = " baseline-shift: sub;";

        fprintf(out,
                "    .cs-%s { font-family: \"%s\"; font-size: %.3fpx; fill: %s; font-weight: %s; font-style: %s;%s%s%s }\n",
                cs->id,
                fv->family,
                eff_size,
                cs->color,
                fv->bold   ? "bold"   : "normal",
                fv->italic ? "italic" : "normal",
                cs->underline  ? " text-decoration: underline;" : "",
                cs->small_caps ? " font-variant: small-caps;"   : "",
                shift_css);
    }
    fprintf(out, "  ]]></style>\n\n");

    // 2. Fondo continuo del escritorio para cubrir todo el recorrido del deslizamiento
    fprintf(out, "  <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" fill=\"#1e2430\"/>\n\n",
            -view_w, -view_h, strip_w + 2.0 * view_w, view_h * 3.0);

    // 3. Tira horizontal de páginas para el efecto ViewBox Sliding
    fprintf(out, "  <g id=\"pages-strip\">\n");
    for (size_t p = 0; p < ts->total_pages; p++) {
        double page_origin_x = (double)p * stride_x + DESK_PAD_X;
        double page_origin_y = DESK_PAD_TOP;

        fprintf(out, "    <g id=\"page-%zu\" transform=\"translate(%.3f, %.3f)\">\n",
                p + 1, page_origin_x, page_origin_y);
        fprintf(out, "      <rect class=\"page-sheet\" x=\"0\" y=\"0\" width=\"%.3f\" height=\"%.3f\" filter=\"url(#page-shadow)\"/>\n",
                doc->page_w_mm, doc->page_h_mm);

        // Recorte de seguridad al borde físico de la hoja
        fprintf(out, "      <clipPath id=\"clip-page-%zu\"><rect x=\"0\" y=\"0\" width=\"%.3f\" height=\"%.3f\"/></clipPath>\n",
                p + 1, doc->page_w_mm, doc->page_h_mm);
        fprintf(out, "      <g clip-path=\"url(#clip-page-%zu)\">\n", p + 1);

        for (size_t f = 0; f < ts->frame_count; f++) {
            if (ts->frames[f].page_idx == p && ts->frames[f].svg_content.len > 0) {
                fputs(ts->frames[f].svg_content.data, out);
            }
        }

        fprintf(out, "      </g>\n");

        if (strcmp(doc->page_style, "empty") != 0) {
            char pnum_str[32];
            format_page_num(p + 1, doc->page_numbering, pnum_str, sizeof(pnum_str));

            if (strcmp(doc->page_style, "headings") == 0 || strcmp(doc->page_style, "myheadings") == 0) {
                double hy = fmax(8.0, doc->margin_top_mm * 0.62);
                const char *mark = ((p % 2 == 0) && doc->right_mark[0]) ? doc->right_mark
                                 : (doc->left_mark[0] ? doc->left_mark : doc->right_mark);
                if (mark && mark[0] != '\0') {
                    fprintf(out, "      <text class=\"page-num-footer\" style=\"font-style:italic;\" x=\"%.3f\" y=\"%.3f\" text-anchor=\"start\">",
                            doc->margin_left_mm, hy);
                    for (const char *mp = mark; *mp; mp++) {
                        if (*mp == '&') fputs("&amp;", out);
                        else if (*mp == '<') fputs("&lt;", out);
                        else if (*mp == '>') fputs("&gt;", out);
                        else fputc(*mp, out);
                    }
                    fprintf(out, "</text>\n");
                }
                fprintf(out, "      <text class=\"page-num-footer\" x=\"%.3f\" y=\"%.3f\" text-anchor=\"end\">%s</text>\n",
                        doc->page_w_mm - doc->margin_right_mm, hy, pnum_str);
                fprintf(out, "      <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" stroke=\"#cbd5e1\" stroke-width=\"0.25\"/>\n",
                        doc->margin_left_mm, hy + 1.6, doc->page_w_mm - doc->margin_right_mm, hy + 1.6);
            }

            fprintf(out, "      <text class=\"page-num-footer\" x=\"%.3f\" y=\"%.3f\" text-anchor=\"middle\">%s / %zu</text>\n",
                    doc->page_w_mm * 0.5, doc->page_h_mm - 6.0, pnum_str, ts->total_pages);
        }
        fprintf(out, "    </g>\n");
    }
    fprintf(out, "  </g>\n\n");

    // 4. Barra HUD anclada dinámicamente al viewBox en curso
    double hud_w = 68.0;
    double hud_h = 10.5;
    double hud_x = (view_w - hud_w) * 0.5;
    double hud_y = view_h - hud_h - 6.5;

    fprintf(out, "  <g id=\"hud\" transform=\"translate(0, 0)\">\n");
    fprintf(out, "    <rect class=\"hud-pill\" x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" rx=\"5.25\"/>\n",
            hud_x, hud_y, hud_w, hud_h);

    // Botón Anterior
    fprintf(out, "    <g id=\"btn-prev\" class=\"hud-btn\" transform=\"translate(%.3f, %.3f)\">\n",
            hud_x + 2.2, hud_y + 1.55);
    fprintf(out, "      <rect x=\"0\" y=\"0\" width=\"14\" height=\"7.4\" rx=\"3.7\"/>\n");
    fprintf(out, "      <text class=\"hud-label\" x=\"7\" y=\"4.95\" text-anchor=\"middle\">&#x2190;</text>\n");
    fprintf(out, "    </g>\n");

    // Indicador central
    fprintf(out, "    <text id=\"hud-indicator\" class=\"hud-label\" x=\"%.3f\" y=\"%.3f\" text-anchor=\"middle\">P&#xE1;g. 1 / %zu</text>\n",
            view_w * 0.5, hud_y + 6.5, ts->total_pages);

    // Botón Siguiente
    fprintf(out, "    <g id=\"btn-next\" class=\"hud-btn\" transform=\"translate(%.3f, %.3f)\">\n",
            hud_x + hud_w - 16.2, hud_y + 1.55);
    fprintf(out, "      <rect x=\"0\" y=\"0\" width=\"14\" height=\"7.4\" rx=\"3.7\"/>\n");
    fprintf(out, "      <text class=\"hud-label\" x=\"7\" y=\"4.95\" text-anchor=\"middle\">&#x2192;</text>\n");
    fprintf(out, "    </g>\n");
    fprintf(out, "  </g>\n\n");

    // 5. Controlador JavaScript embebido: ViewBox Sliding + Teclado + Rueda + Hash URL
    fprintf(out, "  <script><![CDATA[\n");
    fprintf(out, "  (() => {\n");
    fprintf(out, "    const svg = document.getElementById('doc-root');\n");
    fprintf(out, "    const hud = document.getElementById('hud');\n");
    fprintf(out, "    const indicator = document.getElementById('hud-indicator');\n");
    fprintf(out, "    const btnPrev = document.getElementById('btn-prev');\n");
    fprintf(out, "    const btnNext = document.getElementById('btn-next');\n\n");
    fprintf(out, "    const TOTAL_PAGES = %zu;\n", ts->total_pages);
    fprintf(out, "    const VIEW_W      = %.4f;\n", view_w);
    fprintf(out, "    const VIEW_H      = %.4f;\n", view_h);
    fprintf(out, "    const STRIDE_X    = %.4f;\n", stride_x);
    fprintf(out, "    const DURATION_MS = 380;\n\n");
    fprintf(out, "    let currentPage = 0;\n");
    fprintf(out, "    let currentX    = 0;\n");
    fprintf(out, "    let animId      = null;\n");
    fprintf(out, "    let wheelLock   = false;\n\n");
    fprintf(out, "    function applyViewport(x) {\n");
    fprintf(out, "      currentX = x;\n");
    fprintf(out, "      svg.setAttribute('viewBox', `${x.toFixed(3)} 0 ${VIEW_W} ${VIEW_H}`);\n");
    fprintf(out, "      hud.setAttribute('transform', `translate(${x.toFixed(3)}, 0)`);\n");
    fprintf(out, "    }\n\n");
    fprintf(out, "    function syncHudState() {\n");
    fprintf(out, "      indicator.textContent = `P\\u00e1g. ${currentPage + 1} / ${TOTAL_PAGES}`;\n");
    fprintf(out, "      btnPrev.classList.toggle('hud-btn-disabled', currentPage <= 0);\n");
    fprintf(out, "      btnNext.classList.toggle('hud-btn-disabled', currentPage >= TOTAL_PAGES - 1);\n");
    fprintf(out, "    }\n\n");
    fprintf(out, "    function goToPage(targetIdx, immediate = false) {\n");
    fprintf(out, "      const clamped = Math.max(0, Math.min(TOTAL_PAGES - 1, targetIdx));\n");
    fprintf(out, "      currentPage = clamped;\n");
    fprintf(out, "      const targetX = clamped * STRIDE_X;\n");
    fprintf(out, "      syncHudState();\n\n");
    fprintf(out, "      try { history.replaceState(null, '', `#page=${clamped + 1}`); } catch (_) {}\n\n");
    fprintf(out, "      if (animId !== null) {\n");
    fprintf(out, "        cancelAnimationFrame(animId);\n");
    fprintf(out, "        animId = null;\n");
    fprintf(out, "      }\n\n");
    fprintf(out, "      if (immediate || Math.abs(targetX - currentX) < 0.05) {\n");
    fprintf(out, "        applyViewport(targetX);\n");
    fprintf(out, "        return;\n");
    fprintf(out, "      }\n\n");
    fprintf(out, "      const startX = currentX;\n");
    fprintf(out, "      const deltaX = targetX - startX;\n");
    fprintf(out, "      const startTime = performance.now();\n\n");
    fprintf(out, "      function tick(now) {\n");
    fprintf(out, "        const elapsed = now - startTime;\n");
    fprintf(out, "        const t = Math.min(1, elapsed / DURATION_MS);\n");
    fprintf(out, "        const ease = 1 - Math.pow(1 - t, 3);\n");
    fprintf(out, "        applyViewport(startX + deltaX * ease);\n");
    fprintf(out, "        if (t < 1) {\n");
    fprintf(out, "          animId = requestAnimationFrame(tick);\n");
    fprintf(out, "        } else {\n");
    fprintf(out, "          animId = null;\n");
    fprintf(out, "        }\n");
    fprintf(out, "      }\n");
    fprintf(out, "      animId = requestAnimationFrame(tick);\n");
    fprintf(out, "    }\n\n");
    fprintf(out, "    btnPrev.addEventListener('click', () => goToPage(currentPage - 1));\n");
    fprintf(out, "    btnNext.addEventListener('click', () => goToPage(currentPage + 1));\n\n");
    fprintf(out, "    window.addEventListener('keydown', (ev) => {\n");
    fprintf(out, "      if (ev.key === 'ArrowRight' || ev.key === 'PageDown' || ev.key === ' ') {\n");
    fprintf(out, "        ev.preventDefault();\n");
    fprintf(out, "        goToPage(currentPage + 1);\n");
    fprintf(out, "      } else if (ev.key === 'ArrowLeft' || ev.key === 'PageUp') {\n");
    fprintf(out, "        ev.preventDefault();\n");
    fprintf(out, "        goToPage(currentPage - 1);\n");
    fprintf(out, "      } else if (ev.key === 'Home') {\n");
    fprintf(out, "        ev.preventDefault();\n");
    fprintf(out, "        goToPage(0);\n");
    fprintf(out, "      } else if (ev.key === 'End') {\n");
    fprintf(out, "        ev.preventDefault();\n");
    fprintf(out, "        goToPage(TOTAL_PAGES - 1);\n");
    fprintf(out, "      }\n");
    fprintf(out, "    });\n\n");
    fprintf(out, "    window.addEventListener('wheel', (ev) => {\n");
    fprintf(out, "      if (wheelLock) return;\n");
    fprintf(out, "      const delta = Math.abs(ev.deltaX) > Math.abs(ev.deltaY) ? ev.deltaX : ev.deltaY;\n");
    fprintf(out, "      if (Math.abs(delta) < 18) return;\n");
    fprintf(out, "      wheelLock = true;\n");
    fprintf(out, "      goToPage(currentPage + (delta > 0 ? 1 : -1));\n");
    fprintf(out, "      setTimeout(() => { wheelLock = false; }, 320);\n");
    fprintf(out, "    }, { passive: true });\n\n");
    fprintf(out, "    const match = /^#page=(\\d+)$/.exec(window.location.hash);\n");
    fprintf(out, "    const initial = match ? parseInt(match[1], 10) - 1 : 0;\n");
    fprintf(out, "    goToPage(isNaN(initial) ? 0 : initial, true);\n");
    fprintf(out, "  })();\n");
    fprintf(out, "  ]]></script>\n");
    fprintf(out, "</svg>\n");

    fclose(out);
    return true;
}

// ============================================================================
// 8. Función Principal (main)
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <documento_intermedio.tks> [salida.svg]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *tks_path = argv[1];
    char svg_path[256];
    if (argc >= 3) {
        snprintf(svg_path, sizeof(svg_path), "%s", argv[2]);
    } else {
        replace_extension(tks_path, ".svg", svg_path, sizeof(svg_path));
    }

    size_t xml_len = 0;
    char *xml_src = read_entire_file(tks_path, &xml_len);
    if (!xml_src) {
        fprintf(stderr, "Error: no se pudo abrir '%s'\n", tks_path);
        return EXIT_FAILURE;
    }

    IrDocument doc;
    if (!parse_ir_xml(xml_src, xml_len, &doc)) {
        fprintf(stderr, "Error procesando el formato TKS (XML intermedio) '%s'\n", tks_path);
        free(xml_src);
        return EXIT_FAILURE;
    }
    free(xml_src);

    Typesetter ts;
    typesetter_init(&ts, &doc);

    for (size_t i = 0; i < doc.item_count; i++) {
        if (doc.items[i].type == ITEM_PAGE_BREAK) {
            typesetter_page_break(&ts, &doc);
        } else if (doc.items[i].type == ITEM_VSPACE) {
            FlowFrame *fr = &ts.frames[ts.cur_frame];
            if (doc.items[i].vspace_fill) {
                // Estimar la altura de los elementos que restan antes del siguiente salto de página
                double upcoming_h = 0.0;
                for (size_t k = i + 1; k < doc.item_count; k++) {
                    if (doc.items[k].type == ITEM_PAGE_BREAK) break;
                    if (doc.items[k].type == ITEM_VSPACE) {
                        upcoming_h += doc.items[k].vspace_mm;
                    } else if (doc.items[k].type == ITEM_PARA && doc.items[k].para_style_idx < doc.para_style_count) {
                        const ParaStyle *ps = &doc.para_styles[doc.items[k].para_style_idx];
                        upcoming_h += ps->space_before_mm + ps->line_spacing_mm * 1.5 + ps->space_after_mm;
                    }
                }
                double target_y = fr->h_mm - upcoming_h;
                if (target_y > ts.cursor_y_mm) {
                    ts.cursor_y_mm = target_y;
                    ts.at_frame_top = false;
                }
            } else if (!ts.at_frame_top || doc.items[i].vspace_starred) {
                ts.cursor_y_mm = fmax(0.0, ts.cursor_y_mm + doc.items[i].vspace_mm);
                if (doc.items[i].vspace_starred) {
                    ts.at_frame_top = false;
                }
            }
        } else if (doc.items[i].type == ITEM_TABLE) {
            typeset_table(&ts, &doc, doc.items[i].table);
        } else if (doc.items[i].type == ITEM_FOOTNOTE) {
            typeset_footnote(&ts, &doc, &doc.items[i]);
        } else if (doc.items[i].type == ITEM_MARGINPAR) {
            typeset_marginpar(&ts, &doc, &doc.items[i]);
        } else {
            typeset_paragraph(&ts, &doc, &doc.items[i]);
        }
    }

    for (size_t f = 0; f < ts.frame_count; f++) {
        finalize_frame_footnotes(&ts.frames[f], &doc);
    }

    if (!write_standalone_svg(svg_path, &doc, &ts)) {
        fprintf(stderr, "Error escribiendo fichero SVG '%s'\n", svg_path);
        for (size_t i = 0; i < ts.frame_count; i++) sb_free(&ts.frames[i].svg_content);
        free_ir_document(&doc);
        return EXIT_FAILURE;
    }

    printf("SVG multipágina (ViewBox Sliding) generado en '%s' (%zu páginas, %zu marcos).\n",
           svg_path, ts.total_pages, ts.frame_count);

    for (size_t i = 0; i < ts.frame_count; i++) {
        sb_free(&ts.frames[i].svg_content);
    }
    free_ir_document(&doc);
    return EXIT_SUCCESS;
}
