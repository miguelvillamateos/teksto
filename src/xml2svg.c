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
} CharStyle;

typedef struct {
    size_t page_num; // 1-based según el XML
    double x_mm, y_mm, w_mm, h_mm;
} XmlFrameDef;

typedef struct {
    size_t style_idx;
    char *text;
} SpanRun;

typedef enum : unsigned char {
    ITEM_PARA = 0,
    ITEM_PAGE_BREAK
} StoryItemType;

typedef struct {
    StoryItemType type;
    size_t para_style_idx;
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
    StrBuf svg_content;
} FlowFrame;

typedef struct {
    size_t char_style_idx;
    char *word;
    double word_w_mm;
    double space_w_mm;
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

static void para_push_run(StoryItem *para, size_t style_idx, const char *text) {
    if (!text || text[0] == '\0') return;
    if (para->run_count >= para->run_cap) {
        para->run_cap = (para->run_cap == 0) ? 8 : para->run_cap * 2;
        para->runs = realloc(para->runs, para->run_cap * sizeof(SpanRun));
    }
    para->runs[para->run_count++] = (SpanRun){
        .style_idx = style_idx,
        .text      = dup_cstr(text)
    };
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

    XmlReader r = { .src = xml_src, .pos = 0, .len = xml_len };
    char current_family[256] = "sans-serif";
    bool in_para = false;
    bool in_span = false;
    size_t active_cs_idx = 0;
    StoryItem current_para = {0};
    StrBuf span_buf;
    sb_init(&span_buf, 256);

    while (r.pos < r.len) {
        size_t lt = r.pos;
        while (lt < r.len && r.src[lt] != '<') lt++;

        if (lt > r.pos && in_para) {
            if (in_span) {
                xml_unescape_into(&span_buf, r.src + r.pos, lt - r.pos);
            } else {
                StrBuf tmp;
                sb_init(&tmp, lt - r.pos + 8);
                xml_unescape_into(&tmp, r.src + r.pos, lt - r.pos);
                para_push_run(&current_para, 0, tmp.data);
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

            if (strcmp(tag, "span") == 0 && in_span) {
                para_push_run(&current_para, active_cs_idx, span_buf.data);
                span_buf.len = 0;
                span_buf.data[0] = '\0';
                in_span = false;
            } else if (strcmp(tag, "p") == 0 && in_para) {
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
            snprintf(cs->id, sizeof(cs->id), "%s", id ? id : "");
            cs->font_variant_idx = fref ? find_variant_idx(doc, fref) : 0;
            cs->size_mm          = sz ? parse_mm(sz) : (11.0 * MM_PER_PT);
            snprintf(cs->color, sizeof(cs->color), "%s", col ? col : "Black");
            cs->underline        = (ul && strcmp(ul, "true") == 0);
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
        } else if (strcmp(tag, "p") == 0 && !self_closing) {
            const char *st = get_attr(attrs, attr_count, "style");
            memset(&current_para, 0, sizeof(current_para));
            current_para.type           = ITEM_PARA;
            current_para.para_style_idx = st ? find_para_style_idx(doc, st) : 0;
            in_para = true;
        } else if (strcmp(tag, "span") == 0 && in_para && !self_closing) {
            const char *st = get_attr(attrs, attr_count, "style");
            active_cs_idx    = st ? find_char_style_idx(doc, st) : 0;
            span_buf.len     = 0;
            span_buf.data[0] = '\0';
            in_span          = true;
        } else if (strcmp(tag, "break") == 0) {
            const char *tp = get_attr(attrs, attr_count, "type");
            if (!tp || strcmp(tp, "page") == 0) {
                StoryItem brk = { .type = ITEM_PAGE_BREAK };
                doc_push_item(doc, brk);
            }
        }
    }

    sb_free(&span_buf);
    return true;
}

static void free_ir_document(IrDocument *doc) {
    for (size_t i = 0; i < doc->item_count; i++) {
        for (size_t j = 0; j < doc->items[i].run_count; j++) {
            free(doc->items[i].runs[j].text);
        }
        free(doc->items[i].runs);
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
        } else if (step == 1) {
            if (strchr("ilIjtfr.,:;!|'()[]", (int)c))      total_em += 0.30;
            else if (strchr("mwMW@%", (int)c))             total_em += 0.80;
            else if (isupper((int)c))                      total_em += 0.64;
            else                                           total_em += 0.50;
        } else {
            total_em += 0.52;
        }

        for (size_t k = 0; k < step && word[i] != '\0'; k++) i++;
    }

    if (fv->bold && !is_mono) total_em *= 1.06;
    return total_em * em;
}

static double estimate_space_width_mm(const CharStyle *cs, const FontVariant *fv) {
    bool is_mono = str_contains_ci(fv->family, "Mono") || str_contains_ci(fv->family, "Courier");
    return cs->size_mm * (is_mono ? 0.60 : 0.27);
}

static FlowFrame make_default_frame(const IrDocument *doc, size_t page_idx) {
    FlowFrame f = {
        .page_idx = page_idx,
        .x_mm     = doc->margin_left_mm,
        .y_mm     = doc->margin_top_mm,
        .w_mm     = doc->page_w_mm - doc->margin_left_mm - doc->margin_right_mm,
        .h_mm     = doc->page_h_mm - doc->margin_top_mm - doc->margin_bottom_mm
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
                    .page_idx = p,
                    .x_mm     = doc->xml_frames[i].x_mm,
                    .y_mm     = doc->xml_frames[i].y_mm,
                    .w_mm     = doc->xml_frames[i].w_mm,
                    .h_mm     = doc->xml_frames[i].h_mm
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
    const WordToken *words,
    size_t start_idx,
    size_t end_idx,
    double natural_w_mm,
    double line_indent_mm,
    double baseline_y_mm,
    bool is_last_line
) {
    double avail_w = frame->w_mm - ps->indent_left_mm - ps->indent_right_mm - line_indent_mm;
    if (avail_w < 5.0) avail_w = frame->w_mm;

    double x_pos = frame->x_mm + ps->indent_left_mm + line_indent_mm;
    if (ps->align == ALIGN_CENTER) {
        x_pos += fmax(0.0, (avail_w - natural_w_mm) * 0.5);
    } else if (ps->align == ALIGN_RIGHT) {
        x_pos += fmax(0.0, avail_w - natural_w_mm);
    }

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

    // Agrupar palabras adyacentes con idéntico estilo de carácter en un único <tspan>
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
}

static void typeset_paragraph(Typesetter *ts, const IrDocument *doc, const StoryItem *item) {
    if (item->run_count == 0) return;

    const ParaStyle *ps = (item->para_style_idx < doc->para_style_count)
                          ? &doc->para_styles[item->para_style_idx]
                          : &(ParaStyle){ .align = ALIGN_LEFT, .line_spacing_mm = 5.2 };

    // 1. Tokenizar todos los tramos del párrafo en palabras con su estilo
    size_t cap = 64, count = 0;
    WordToken *words = malloc(cap * sizeof(WordToken));
    if (!words) return;

    for (size_t r = 0; r < item->run_count; r++) {
        size_t cs_idx = item->runs[r].style_idx;
        const CharStyle *cs = &doc->char_styles[cs_idx];
        const FontVariant *fv = &doc->variants[cs->font_variant_idx];
        const char *s = item->runs[r].text;
        size_t pos = 0;

        while (s[pos] != '\0') {
            if (isspace((unsigned char)s[pos])) {
                if (count > 0 && !words[count - 1].space_after) {
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
                .char_style_idx = cs_idx,
                .word           = w_str,
                .word_w_mm      = estimate_word_width_mm(w_str, cs, fv),
                .space_w_mm     = estimate_space_width_mm(cs, fv),
                .space_after    = trailing_space
            };
        }
    }

    if (count == 0) {
        free(words);
        return;
    }

    if (!ts->at_frame_top) {
        ts->cursor_y_mm += ps->space_before_mm;
    }

    // 2. Composición voraz línea a línea respetando márgenes, sangría y desbordamiento de caja
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

        size_t line_end = idx;
        double acc_w    = 0.0;

        while (line_end < count) {
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

        bool is_last = (line_end >= count);
        double max_font_mm = 0.0;
        for (size_t k = idx; k < line_end; k++) {
            double sz = doc->char_styles[words[k].char_style_idx].size_mm;
            if (sz > max_font_mm) max_font_mm = sz;
        }
        double baseline_offset = fmax(max_font_mm * 0.82, line_h * 0.76);

        emit_svg_line(fr, doc, ps, words, idx, line_end, acc_w, indent,
                      ts->cursor_y_mm + baseline_offset, is_last);

        ts->cursor_y_mm += line_h;
        ts->at_frame_top = false;
        first_line       = false;
        idx              = line_end;
    }

    ts->cursor_y_mm += ps->space_after_mm;

    for (size_t i = 0; i < count; i++) free(words[i].word);
    free(words);
}

// ============================================================================
// 7. Emisión del Fichero Autocontenido SVG + JS ("ViewBox Sliding" Effect)
// ============================================================================

static void fprint_xml_attr_escaped(FILE *f, const char *s) {
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
            case '&':  fputs("&amp;", f);  break;
            case '<':  fputs("&lt;", f);   break;
            case '>':  fputs("&gt;", f);   break;
            case '\"': fputs("&quot;", f); break;
            default:   fputc(*p, f);       break;
        }
    }
}

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
        fprintf(out,
                "    .cs-%s { font-family: \"%s\"; font-size: %.3fpx; fill: %s; font-weight: %s; font-style: %s;%s }\n",
                cs->id,
                fv->family,
                cs->size_mm,
                cs->color,
                fv->bold   ? "bold"   : "normal",
                fv->italic ? "italic" : "normal",
                cs->underline ? " text-decoration: underline;" : "");
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
        fprintf(out, "      <text class=\"page-num-footer\" x=\"%.3f\" y=\"%.3f\" text-anchor=\"middle\">%zu / %zu</text>\n",
                doc->page_w_mm * 0.5, doc->page_h_mm - 6.0, p + 1, ts->total_pages);
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
        fprintf(stderr, "Uso: %s <documento_intermedio.xml> [salida.svg]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *xml_path = argv[1];
    char svg_path[256];
    if (argc >= 3) {
        snprintf(svg_path, sizeof(svg_path), "%s", argv[2]);
    } else {
        replace_extension(xml_path, ".svg", svg_path, sizeof(svg_path));
    }

    size_t xml_len = 0;
    char *xml_src = read_entire_file(xml_path, &xml_len);
    if (!xml_src) {
        fprintf(stderr, "Error: no se pudo abrir '%s'\n", xml_path);
        return EXIT_FAILURE;
    }

    IrDocument doc;
    if (!parse_ir_xml(xml_src, xml_len, &doc)) {
        fprintf(stderr, "Error procesando el formato XML intermedio '%s'\n", xml_path);
        free(xml_src);
        return EXIT_FAILURE;
    }
    free(xml_src);

    Typesetter ts;
    typesetter_init(&ts, &doc);

    for (size_t i = 0; i < doc.item_count; i++) {
        if (doc.items[i].type == ITEM_PAGE_BREAK) {
            typesetter_page_break(&ts, &doc);
        } else {
            typeset_paragraph(&ts, &doc, &doc.items[i]);
        }
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
