#include "tex_types.h"
#include "tex_symbols.h"
#include "tex_lexer.h"

// ============================================================================
// 4. Prototipos de Funciones
// ============================================================================

static bool sv_equals(StrView sv, const char *literal);
static StrView sv_trim(StrView sv);
static StrView sv_strip_leading_backslash(StrView sv);
static long sv_to_long(const Parser *p, StrView sv);
static bool has_tex_extension(const char *path);
static void replace_extension(const char *path, const char *new_ext, char *out_buf, size_t buf_size);
static char *read_entire_file(const char *path, size_t *out_len);
static char *dup_sv(StrView sv);

static Measure measure_make(double value, Unit unit, double ref_em_mm);
static Measure measure_parse(const Parser *p, StrView sv, Unit default_unit);
static bool measure_equals(Measure a, Measure b);
static void measure_format(Measure m, char *buf, size_t buf_size);

static bool sb_init(StrBuf *sb, size_t initial_cap);
static void sb_free(StrBuf *sb);
static void sb_append_mem(StrBuf *sb, const char *data, size_t len);
static void sb_append_cstr(StrBuf *sb, const char *str);
static void sb_appendf(StrBuf *sb, const char *fmt, ...);
static void sb_append_escaped_xml(StrBuf *sb, const char *str, size_t len);
static void fprint_escaped_xml(FILE *f, const char *str);

static bool xml_gen_init(XmlGenerator *gen, const char *xml_path);
static void xml_push_group(XmlGenerator *gen);
static void xml_pop_group(XmlGenerator *gen);
static void xml_close_para(XmlGenerator *gen);
static void xml_emit_text(XmlGenerator *gen, StrView text, bool space_before);
static void xml_emit_page_break(XmlGenerator *gen);
static void xml_gen_finalize(XmlGenerator *gen);

static void macro_env_init(MacroEnv *env);
static void macro_env_free(MacroEnv *env);
static UserMacro *macro_env_find_mut(MacroEnv *env, StrView name);
static const UserMacro *macro_env_find(const MacroEnv *env, StrView name);
static bool macro_env_define(MacroEnv *env, StrView name, size_t params, bool has_opt, StrView opt_def, StrView body, bool overwrite);
static UserEnv *user_env_find_mut(MacroEnv *env, StrView name);
static bool user_env_define(MacroEnv *env, StrView name, size_t params, bool has_opt, StrView opt_def, StrView begin_body, StrView end_body, bool overwrite);

static CounterDef *counter_find_mut(MacroEnv *env, StrView name);
static CounterDef *counter_register(MacroEnv *env, StrView name, StrView parent);
static void counter_step(MacroEnv *env, StrView name);
static void format_counter_value(const MacroEnv *env, StrView name, char style, char *out_buf, size_t buf_size);

static LengthDef *length_find_mut(MacroEnv *env, StrView name);
static LengthDef *length_register(MacroEnv *env, StrView name, Measure initial);
static void sync_standard_length_to_generator(XmlGenerator *gen, StrView name, Measure val);

static void parser_advance(Parser *p);
static bool parser_consume_star(Parser *p);
static bool parse_opt_arg(Parser *p, StrView *out_opt);
static bool parse_arg(Parser *p, StrView *out_arg);
static bool parse_cmd_or_arg(Parser *p, StrView *out_arg);
static void parse_and_execute(Parser *p);
static void execute_tex_fragment(Parser *p, StrView fragment, bool preserve_space);
static void parse_inline_scoped(Parser *p, StrView fragment, CharState modified_state);
static bool process_file(const char *path, MacroEnv *env, XmlGenerator *gen, size_t macro_depth);

// ============================================================================
// 5. Utilidades de Cadena y Gestión de Unidades Métricas TeX
// ============================================================================

static bool sv_equals(StrView sv, const char *literal) {
    size_t lit_len = strlen(literal);
    return (sv.len == lit_len) && (strncmp(sv.data, literal, lit_len) == 0);
}

static StrView sv_trim(StrView sv) {
    while (sv.len > 0 && isspace((unsigned char)sv.data[0])) {
        sv.data++;
        sv.len--;
    }
    while (sv.len > 0 && isspace((unsigned char)sv.data[sv.len - 1])) {
        sv.len--;
    }
    return sv;
}

static StrView sv_strip_leading_backslash(StrView sv) {
    sv = sv_trim(sv);
    if (sv.len > 0 && sv.data[0] == '\\') {
        sv.data++;
        sv.len--;
    }
    return sv_trim(sv);
}

static char *dup_sv(StrView sv) {
    char *copy = malloc(sv.len + 1);
    if (!copy) return nullptr;
    if (sv.len > 0) memcpy(copy, sv.data, sv.len);
    copy[sv.len] = '\0';
    return copy;
}

static long sv_to_long(const Parser *p, StrView sv) {
    sv = sv_trim(sv);
    // Soportar \value{counter}
    if (sv.len > 7 && strncmp(sv.data, "\\value", 6) == 0) {
        StrView rest = { sv.data + 6, sv.len - 6 };
        rest = sv_trim(rest);
        if (rest.len >= 2 && rest.data[0] == '{' && rest.data[rest.len - 1] == '}') {
            StrView cname = { rest.data + 1, rest.len - 2 };
            CounterDef *c = (p && p->env) ? counter_find_mut(p->env, sv_trim(cname)) : nullptr;
            return c ? c->value : 0;
        }
    }
    char buf[64];
    size_t n = (sv.len < sizeof(buf) - 1) ? sv.len : (sizeof(buf) - 1);
    memcpy(buf, sv.data, n);
    buf[n] = '\0';
    return strtol(buf, nullptr, 10);
}

static Measure measure_make(double value, Unit unit, double ref_em_mm) {
    if (ref_em_mm <= 0.0) ref_em_mm = 11.0 * MM_PER_PT;
    double mm = value;
    switch (unit) {
        case UNIT_MM: mm = value;                      break;
        case UNIT_CM: mm = value * MM_PER_CM;          break;
        case UNIT_M:  mm = value * MM_PER_M;           break;
        case UNIT_PT: mm = value * MM_PER_PT;          break;
        case UNIT_BP: mm = value * MM_PER_BP;          break;
        case UNIT_IN: mm = value * MM_PER_IN;          break;
        case UNIT_PC: mm = value * MM_PER_PC;          break;
        case UNIT_DD: mm = value * MM_PER_DD;          break;
        case UNIT_CC: mm = value * MM_PER_CC;          break;
        case UNIT_SP: mm = value * MM_PER_SP;          break;
        case UNIT_EM: mm = value * ref_em_mm;          break;
        case UNIT_EX: mm = value * (ref_em_mm * 0.45); break;
    }
    // Normalizamos unidades relativas o exóticas a mm o pt para el XML intermedio
    if (unit == UNIT_MM || unit == UNIT_CM || unit == UNIT_M || unit == UNIT_PT) {
        return (Measure){ .value = value, .mm = mm, .unit = unit };
    }
    return (Measure){ .value = mm, .mm = mm, .unit = UNIT_MM };
}

static Measure measure_parse(const Parser *p, StrView sv, Unit default_unit) {
    sv = sv_trim(sv);
    double ref_em = (p && p->gen) ? p->gen->char_state.size.mm : (11.0 * MM_PER_PT);

    if (sv.len == 0) {
        return measure_make(0.0, default_unit, ref_em);
    }

    // Si es directamente un registro de longitud como \parindent
    if (sv.data[0] == '\\' && p && p->env) {
        StrView reg_name = sv_strip_leading_backslash(sv);
        if (sv_equals(reg_name, "fill")) {
            return measure_make(0.0, UNIT_MM, ref_em);
        }
        LengthDef *ld = length_find_mut(p->env, reg_name);
        if (ld) return ld->value;
    }

    char buf[128];
    size_t n = (sv.len < sizeof(buf) - 1) ? sv.len : (sizeof(buf) - 1);
    memcpy(buf, sv.data, n);
    buf[n] = '\0';

    char *endptr = nullptr;
    double val = strtod(buf, &endptr);
    if (endptr == buf) {
        return measure_make(0.0, default_unit, ref_em);
    }

    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    // Factor multiplicador sobre un registro: p. ej. 0.5\textwidth o 2\parindent
    if (*endptr == '\\' && p && p->env) {
        StrView reg_name = sv_trim((StrView){ endptr + 1, strlen(endptr + 1) });
        LengthDef *ld = length_find_mut(p->env, reg_name);
        if (ld) {
            return measure_make(val * ld->value.mm, UNIT_MM, ref_em);
        }
    }

    // Recortar posible 'plus ... minus ...' de longitudes elásticas de TeX
    char unit_tok[16] = {0};
    size_t u = 0;
    while (endptr[u] != '\0' && !isspace((unsigned char)endptr[u]) && u < sizeof(unit_tok) - 1) {
        unit_tok[u] = endptr[u];
        u++;
    }
    unit_tok[u] = '\0';

    Unit unit = default_unit;
    if (strcmp(unit_tok, "mm") == 0)      unit = UNIT_MM;
    else if (strcmp(unit_tok, "cm") == 0) unit = UNIT_CM;
    else if (strcmp(unit_tok, "m") == 0)  unit = UNIT_M;
    else if (strcmp(unit_tok, "pt") == 0) unit = UNIT_PT;
    else if (strcmp(unit_tok, "bp") == 0) unit = UNIT_BP;
    else if (strcmp(unit_tok, "in") == 0) unit = UNIT_IN;
    else if (strcmp(unit_tok, "pc") == 0) unit = UNIT_PC;
    else if (strcmp(unit_tok, "dd") == 0) unit = UNIT_DD;
    else if (strcmp(unit_tok, "cc") == 0) unit = UNIT_CC;
    else if (strcmp(unit_tok, "sp") == 0) unit = UNIT_SP;
    else if (strcmp(unit_tok, "em") == 0) unit = UNIT_EM;
    else if (strcmp(unit_tok, "ex") == 0) unit = UNIT_EX;

    return measure_make(val, unit, ref_em);
}

static bool measure_equals(Measure a, Measure b) {
    return fabs(a.mm - b.mm) < MEASURE_EPSILON;
}

static void measure_format(Measure m, char *buf, size_t buf_size) {
    const char *suffix = "mm";
    int decimals = 2;
    double v = m.value;

    switch (m.unit) {
        case UNIT_MM: suffix = "mm"; decimals = 2; v = m.value; break;
        case UNIT_CM: suffix = "cm"; decimals = 3; v = m.value; break;
        case UNIT_M:  suffix = "m";  decimals = 4; v = m.value; break;
        case UNIT_PT: suffix = "pt"; decimals = 2; v = m.value; break;
        default:      suffix = "mm"; decimals = 2; v = m.mm;    break;
    }

    char num[48];
    snprintf(num, sizeof(num), "%.*f", decimals, v);

    char *dot = strchr(num, '.');
    if (dot) {
        char *end = num + strlen(num) - 1;
        while (end > dot && *end == '0') {
            *end-- = '\0';
        }
        if (end == dot) {
            *dot = '\0';
        }
    }

    snprintf(buf, buf_size, "%s%s", num, suffix);
}

static bool has_tex_extension(const char *path) {
    size_t len = strlen(path);
    const char *ext = ".tex";
    size_t ext_len = strlen(ext);
    if (len < ext_len) return false;
    return strcmp(path + (len - ext_len), ext) == 0;
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

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return nullptr;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    buf[read_bytes] = '\0';
    fclose(f);

    if (out_len) *out_len = read_bytes;
    return buf;
}

// ============================================================================
// 6. Buffer Dinámico de Cadenas y Escape XML
// ============================================================================

static bool sb_init(StrBuf *sb, size_t initial_cap) {
    sb->data = malloc(initial_cap);
    if (!sb->data) {
        sb->len = 0;
        sb->cap = 0;
        return false;
    }
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = initial_cap;
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

    char *new_data = realloc(sb->data, new_cap);
    if (!new_data) {
        fprintf(stderr, "Error crítico: memoria agotada\n");
        return false;
    }
    sb->data = new_data;
    sb->cap = new_cap;
    return true;
}

static void sb_append_mem(StrBuf *sb, const char *data, size_t len) {
    if (len == 0 || !sb_reserve(sb, len)) return;
    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_append_cstr(StrBuf *sb, const char *str) {
    sb_append_mem(sb, str, strlen(str));
}

static void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int n = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (n <= 0 || !sb_reserve(sb, (size_t)n)) {
        va_end(copy);
        return;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, copy);
    va_end(copy);
    sb->len += (size_t)n;
}

static void sb_append_escaped_xml(StrBuf *sb, const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '&':  sb_append_cstr(sb, "&amp;");  break;
            case '<':  sb_append_cstr(sb, "&lt;");   break;
            case '>':  sb_append_cstr(sb, "&gt;");   break;
            case '\"': sb_append_cstr(sb, "&quot;"); break;
            case '\'': sb_append_cstr(sb, "&apos;"); break;
            default:   sb_append_mem(sb, &str[i], 1); break;
        }
    }
}

static void fprint_escaped_xml(FILE *f, const char *str) {
    for (const char *p = str; *p != '\0'; p++) {
        switch (*p) {
            case '&':  fputs("&amp;", f);  break;
            case '<':  fputs("&lt;", f);   break;
            case '>':  fputs("&gt;", f);   break;
            case '\"': fputs("&quot;", f); break;
            case '\'': fputs("&apos;", f); break;
            default:   fputc(*p, f);       break;
        }
    }
}

// ============================================================================
// 7. Control de Estado, Pila de Grupos TeX y Generador XML
// ============================================================================

static unsigned char variant_bit(FontWeight weight, FontSlant slant) {
    unsigned int idx = ((weight == WEIGHT_BOLD) ? 1u : 0u) |
                       ((slant == SLANT_ITALIC) ? 2u : 0u);
    return (unsigned char)(1u << idx);
}

static const char *variant_suffix(FontWeight weight, FontSlant slant) {
    if (weight == WEIGHT_BOLD && slant == SLANT_ITALIC) return "bold-italic";
    if (weight == WEIGHT_BOLD)                          return "bold";
    if (slant == SLANT_ITALIC)                          return "italic";
    return "regular";
}

static const char *align_to_str(ParaAlign align) {
    switch (align) {
        case ALIGN_LEFT:    return "left";
        case ALIGN_CENTER:  return "center";
        case ALIGN_RIGHT:   return "right";
        case ALIGN_JUSTIFY: return "justify";
    }
    return "left";
}

static const char *orientation_to_str(PageOrientation o) {
    return (o == ORIENT_LANDSCAPE) ? "landscape" : "portrait";
}

static void sync_page_dimensions_to_orientation(XmlGenerator *gen) {
    bool is_wider = gen->page_width.mm > gen->page_height.mm;
    if ((gen->orientation == ORIENT_LANDSCAPE && !is_wider) ||
        (gen->orientation == ORIENT_PORTRAIT  && is_wider)) {
        Measure tmp      = gen->page_width;
        gen->page_width  = gen->page_height;
        gen->page_height = tmp;
    }
}

static bool xml_gen_init(XmlGenerator *gen, const char *xml_path) {
    memset(gen, 0, sizeof(*gen));

    if (xml_path) {
        gen->file = fopen(xml_path, "w");
        if (!gen->file) return false;
    } else {
        gen->file = nullptr;
    }

    if (!sb_init(&gen->story, 8192)) {
        if (gen->file) fclose(gen->file);
        return false;
    }
    if (!sb_init(&gen->pending_footnotes, 512)) {
        sb_free(&gen->story);
        if (gen->file) fclose(gen->file);
        return false;
    }

    double default_em = 11.0 * MM_PER_PT;
    snprintf(gen->doc_class, sizeof(gen->doc_class), "article");
    snprintf(gen->page_style, sizeof(gen->page_style), "plain");
    snprintf(gen->page_numbering, sizeof(gen->page_numbering), "arabic");
    gen->base_font_pt      = 11.0;
    gen->linespread_factor = 1.0;

    gen->page_width    = measure_make(210.0, UNIT_MM, default_em);
    gen->page_height   = measure_make(297.0, UNIT_MM, default_em);
    gen->orientation   = ORIENT_PORTRAIT;
    gen->margin_top    = measure_make(25.0, UNIT_MM, default_em);
    gen->margin_bottom = measure_make(25.0, UNIT_MM, default_em);
    gen->margin_left   = measure_make(20.0, UNIT_MM, default_em);
    gen->margin_right  = measure_make(20.0, UNIT_MM, default_em);
    gen->page_count    = 1;

    gen->para_state = (ParaState){
        .align        = ALIGN_JUSTIFY,
        .line_spacing = measure_make(5.5, UNIT_MM, default_em),
        .space_before = measure_make(0.0, UNIT_MM, default_em),
        .space_after  = measure_make(2.5, UNIT_MM, default_em),
        .indent_first = measure_make(8.0, UNIT_MM, default_em),
        .indent_left  = measure_make(0.0, UNIT_MM, default_em),
        .indent_right = measure_make(0.0, UNIT_MM, default_em)
    };

    snprintf(gen->char_state.font_family, sizeof(gen->char_state.font_family), "%s", DEFAULT_FONT_FAMILY);
    gen->char_state.weight     = WEIGHT_NORMAL;
    gen->char_state.slant      = SLANT_NORMAL;
    gen->char_state.size       = measure_make(11.0, UNIT_PT, default_em);
    snprintf(gen->char_state.color, sizeof(gen->char_state.color), "%s", DEFAULT_COLOR);
    gen->char_state.underline  = false;
    gen->char_state.small_caps = false;
    gen->char_state.shift      = SHIFT_NORMAL;

    gen->active_char_style_id = -1;
    return true;
}

static void xml_push_group(XmlGenerator *gen) {
    if (gen->group_depth < MAX_GROUP_DEPTH) {
        gen->group_stack[gen->group_depth++] = (GroupFrame){
            .para_state = gen->para_state,
            .char_state = gen->char_state
        };
    }
}

static void xml_pop_group(XmlGenerator *gen) {
    if (gen->group_depth > 0) {
        GroupFrame restored = gen->group_stack[--gen->group_depth];
        gen->char_state = restored.char_state;
        gen->para_state = restored.para_state;
    }
}

static FontVariantRef xml_register_font_variant(XmlGenerator *gen, const char *family, FontWeight weight, FontSlant slant) {
    for (size_t i = 0; i < gen->font_count; i++) {
        if (strcmp(gen->fonts[i].family, family) == 0) {
            gen->fonts[i].used_variants_mask |= variant_bit(weight, slant);
            return (FontVariantRef){ .family_idx = i, .weight = weight, .slant = slant };
        }
    }

    size_t idx = (gen->font_count < MAX_FONT_FAMS) ? gen->font_count++ : (MAX_FONT_FAMS - 1);
    snprintf(gen->fonts[idx].family, sizeof(gen->fonts[idx].family), "%s", family);
    gen->fonts[idx].used_variants_mask = variant_bit(weight, slant);

    return (FontVariantRef){ .family_idx = idx, .weight = weight, .slant = slant };
}

static bool para_state_equals(const ParaState *a, const ParaState *b) {
    return a->align == b->align &&
           measure_equals(a->line_spacing, b->line_spacing) &&
           measure_equals(a->space_before, b->space_before) &&
           measure_equals(a->space_after,  b->space_after)  &&
           measure_equals(a->indent_first, b->indent_first) &&
           measure_equals(a->indent_left,  b->indent_left)  &&
           measure_equals(a->indent_right, b->indent_right);
}

static size_t xml_intern_para_style(XmlGenerator *gen, const ParaState *state) {
    for (size_t i = 0; i < gen->para_style_count; i++) {
        if (para_state_equals(&gen->para_styles[i], state)) {
            return i;
        }
    }
    size_t idx = (gen->para_style_count < MAX_PARA_STYLES) ? gen->para_style_count++ : (MAX_PARA_STYLES - 1);
    gen->para_styles[idx] = *state;
    return idx;
}

static size_t xml_intern_char_style(XmlGenerator *gen, const CharState *state) {
    FontVariantRef vref = xml_register_font_variant(gen, state->font_family, state->weight, state->slant);

    for (size_t i = 0; i < gen->char_style_count; i++) {
        const CharStyleDef *def = &gen->char_styles[i];
        if (def->variant.family_idx == vref.family_idx &&
            def->variant.weight     == vref.weight     &&
            def->variant.slant      == vref.slant      &&
            def->underline          == state->underline &&
            def->small_caps         == state->small_caps &&
            def->shift              == state->shift     &&
            measure_equals(def->size, state->size)     &&
            strcmp(def->color, state->color) == 0) {
            return i;
        }
    }

    size_t idx = (gen->char_style_count < MAX_CHAR_STYLES) ? gen->char_style_count++ : (MAX_CHAR_STYLES - 1);
    CharStyleDef *target = &gen->char_styles[idx];
    target->variant    = vref;
    target->size       = state->size;
    target->underline  = state->underline;
    target->small_caps = state->small_caps;
    target->shift      = state->shift;
    snprintf(target->color, sizeof(target->color), "%s", state->color);
    return idx;
}

static void xml_close_active_span(XmlGenerator *gen) {
    if (gen->active_char_style_id >= 0) {
        sb_append_cstr(&gen->story, "</span>");
        gen->active_char_style_id = -1;
        gen->active_href[0]       = '\0';
    }
}

static void xml_flush_pending_anchors(XmlGenerator *gen) {
    if (gen->pending_anchors[0] == '\0') return;
    const char *cur = gen->pending_anchors;
    while (*cur != '\0') {
        const char *semi = strchr(cur, ';');
        size_t len = semi ? (size_t)(semi - cur) : strlen(cur);
        if (len > 0) {
            sb_append_cstr(&gen->story, "<anchor id=\"");
            sb_append_escaped_xml(&gen->story, cur, len);
            sb_append_cstr(&gen->story, "\"/>");
        }
        cur = semi ? (semi + 1) : (cur + len);
    }
    gen->pending_anchors[0] = '\0';
}

static void xml_emit_anchor(XmlGenerator *gen, const char *anchor_id) {
    if (!anchor_id || anchor_id[0] == '\0' || gen->capture_plain_buf || gen->in_picture) return;
    if (gen->para_open) {
        xml_close_active_span(gen);
        sb_append_cstr(&gen->story, "<anchor id=\"");
        sb_append_escaped_xml(&gen->story, anchor_id, strlen(anchor_id));
        sb_append_cstr(&gen->story, "\"/>");
    } else {
        size_t cur_len = strlen(gen->pending_anchors);
        size_t id_len  = strlen(anchor_id);
        if (cur_len == 0 && id_len < sizeof(gen->pending_anchors)) {
            memcpy(gen->pending_anchors, anchor_id, id_len + 1);
        } else if (cur_len + 1 + id_len < sizeof(gen->pending_anchors)) {
            gen->pending_anchors[cur_len] = ';';
            memcpy(gen->pending_anchors + cur_len + 1, anchor_id, id_len + 1);
        }
    }
}

static void xml_ensure_para_open(XmlGenerator *gen) {
    if (gen->in_tabular) {
        if (!gen->tab_row_open) {
            sb_append_cstr(&gen->story, "        <row>\n");
            gen->tab_row_open = true;
            gen->tab_cur_col  = 0;
        }
        if (!gen->tab_cell_open) {
            size_t span = 1;
            char align  = 'l';
            int bl      = 0;
            int br      = 0;
            double w_mm = 0.0;

            if (gen->tab_has_mc_override) {
                span  = (gen->tab_mc_span > 0) ? gen->tab_mc_span : 1;
                align = gen->tab_mc_align ? gen->tab_mc_align : 'c';
                bl    = gen->tab_mc_bl;
                br    = gen->tab_mc_br;
                gen->tab_has_mc_override = false;
            } else if (gen->tab_col_count > 0) {
                size_t c = (gen->tab_cur_col < gen->tab_col_count)
                           ? gen->tab_cur_col
                           : (gen->tab_col_count - 1);
                align = gen->tab_cols[c].align ? gen->tab_cols[c].align : 'l';
                bl    = gen->tab_cols[c].border_left;
                br    = gen->tab_cols[c].border_right;
                w_mm  = gen->tab_cols[c].width_mm;
            }
            gen->tab_cur_span = span;
            if (w_mm > 0.01) {
                sb_appendf(&gen->story,
                           "          <cell span=\"%zu\" align=\"%c\" bl=\"%d\" br=\"%d\" width=\"%.2fmm\">",
                           span, align, bl, br, w_mm);
            } else {
                sb_appendf(&gen->story,
                           "          <cell span=\"%zu\" align=\"%c\" bl=\"%d\" br=\"%d\">",
                           span, align, bl, br);
            }
            gen->tab_cell_open        = true;
            gen->para_open            = true;
            gen->para_has_content     = false;
            gen->active_char_style_id = -1;
            gen->active_href[0]       = '\0';
            xml_flush_pending_anchors(gen);
        }
        return;
    }

    if (gen->para_open) return;

    ParaState effective = gen->para_state;
    if (gen->next_para_noindent || gen->has_pending_item_label) {
        effective.indent_first = measure_make(0.0, UNIT_MM, 1.0);
        gen->next_para_noindent = false;
    }

    size_t ps_id = xml_intern_para_style(gen, &effective);
    if (gen->has_pending_item_label) {
        size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
        sb_appendf(&gen->story, "      <p style=\"ps%zu\" label=\"", ps_id + 1);
        sb_append_escaped_xml(&gen->story, gen->pending_item_label, strlen(gen->pending_item_label));
        sb_appendf(&gen->story, "\" label-style=\"cs%zu\">", cs_id + 1);
        gen->has_pending_item_label = false;
        gen->pending_item_label[0] = '\0';
    } else {
        sb_appendf(&gen->story, "      <p style=\"ps%zu\">", ps_id + 1);
    }
    gen->para_open            = true;
    gen->para_has_content     = false;
    gen->active_char_style_id = -1;
    gen->active_href[0]       = '\0';
    xml_flush_pending_anchors(gen);
}

static void xml_table_next_cell(XmlGenerator *gen) {
    if (!gen->in_tabular) return;
    if (!gen->tab_cell_open) {
        xml_ensure_para_open(gen);
    }
    xml_close_active_span(gen);
    sb_append_cstr(&gen->story, "</cell>\n");
    gen->tab_cell_open    = false;
    gen->para_open        = false;
    gen->para_has_content = false;
    gen->tab_cur_col     += (gen->tab_cur_span > 0) ? gen->tab_cur_span : 1;
    gen->tab_cur_span     = 1;
    gen->char_state       = gen->tab_base_char_state;
}

static void xml_table_end_row(XmlGenerator *gen, Measure extra_v) {
    if (!gen->in_tabular) return;
    if (!gen->tab_row_open) {
        xml_ensure_para_open(gen);
    }
    if (gen->tab_cell_open) {
        xml_close_active_span(gen);
        sb_append_cstr(&gen->story, "</cell>\n");
        gen->tab_cell_open    = false;
        gen->para_open        = false;
        gen->para_has_content = false;
        gen->tab_cur_col     += (gen->tab_cur_span > 0) ? gen->tab_cur_span : 1;
        gen->tab_cur_span     = 1;
    }
    while (gen->tab_cur_col < gen->tab_col_count) {
        xml_ensure_para_open(gen);
        xml_close_active_span(gen);
        sb_append_cstr(&gen->story, "</cell>\n");
        gen->tab_cell_open    = false;
        gen->para_open        = false;
        gen->para_has_content = false;
        gen->tab_cur_col     += (gen->tab_cur_span > 0) ? gen->tab_cur_span : 1;
        gen->tab_cur_span     = 1;
    }
    if (fabs(extra_v.mm) > 0.01) {
        char b[32];
        measure_format(extra_v, b, sizeof(b));
        sb_appendf(&gen->story, "          <row-space extra=\"%s\"/>\n", b);
    }
    sb_append_cstr(&gen->story, "        </row>\n");
    gen->tab_row_open = false;
    gen->tab_cur_col  = 0;
    gen->char_state   = gen->tab_base_char_state;
}

static void xml_close_para(XmlGenerator *gen) {
    if (gen->in_tabular) return;
    if (!gen->para_open) {
        if (gen->pending_footnotes.len > 0) {
            sb_append_mem(&gen->story, gen->pending_footnotes.data, gen->pending_footnotes.len);
            gen->pending_footnotes.len = 0;
            gen->pending_footnotes.data[0] = '\0';
        }
        return;
    }

    xml_close_active_span(gen);
    sb_append_cstr(&gen->story, "</p>\n");
    gen->para_open        = false;
    gen->para_has_content = false;

    if (gen->pending_footnotes.len > 0) {
        sb_append_mem(&gen->story, gen->pending_footnotes.data, gen->pending_footnotes.len);
        gen->pending_footnotes.len = 0;
        gen->pending_footnotes.data[0] = '\0';
    }
}

static void xml_activate_current_char_style(XmlGenerator *gen) {
    size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
    if (gen->active_char_style_id != (int)cs_id || strcmp(gen->active_href, gen->current_href) != 0) {
        xml_close_active_span(gen);
        if (gen->current_href[0] != '\0') {
            sb_appendf(&gen->story, "<span style=\"cs%zu\" href=\"", cs_id + 1);
            sb_append_escaped_xml(&gen->story, gen->current_href, strlen(gen->current_href));
            sb_append_cstr(&gen->story, "\">");
        } else {
            sb_appendf(&gen->story, "<span style=\"cs%zu\">", cs_id + 1);
        }
        gen->active_char_style_id = (int)cs_id;
        snprintf(gen->active_href, sizeof(gen->active_href), "%s", gen->current_href);
    }
}

static bool is_math_upright_ascii(char c) {
    return isdigit((unsigned char)c) ||
           c == '+' || c == '-' || c == '=' || c == '<' || c == '>' ||
           c == '(' || c == ')' || c == '[' || c == ']' || c == '|' ||
           c == ',' || c == ';' || c == ':' || c == '!' || c == '/' || c == '*';
}

static void xml_emit_text(XmlGenerator *gen, StrView text, bool space_before) {
    if (text.len == 0) return;

    if (gen->capture_plain_buf) {
        if (!gen->has_captured_char_state) {
            gen->captured_char_state     = gen->char_state;
            gen->has_captured_char_state = true;
        }
        if (space_before && gen->capture_plain_buf->len > 0) {
            sb_append_cstr(gen->capture_plain_buf, " ");
        }
        sb_append_mem(gen->capture_plain_buf, text.data, text.len);
        return;
    }

    if (gen->in_picture) {
        size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
        double px = gen->pic_cur_x_mm - gen->pic_x0_mm;
        double py = gen->pic_h_mm - (gen->pic_cur_y_mm - gen->pic_y0_mm);
        sb_appendf(&gen->story, "        <ptext x=\"%.2fmm\" y=\"%.2fmm\" style=\"cs%zu\">",
                   px, py, cs_id + 1);
        sb_append_escaped_xml(&gen->story, text.data, text.len);
        sb_append_cstr(&gen->story, "</ptext>\n");
        return;
    }

    xml_ensure_para_open(gen);

    if (gen->in_math && gen->char_state.slant == SLANT_ITALIC) {
        if (space_before && gen->para_has_content) {
            xml_activate_current_char_style(gen);
            sb_append_cstr(&gen->story, " ");
        }
        size_t i = 0;
        while (i < text.len) {
            bool up = is_math_upright_ascii(text.data[i]);
            size_t start = i;
            while (i < text.len && is_math_upright_ascii(text.data[i]) == up) {
                i++;
            }
            FontSlant saved_slant = gen->char_state.slant;
            if (up) gen->char_state.slant = SLANT_NORMAL;
            xml_activate_current_char_style(gen);
            for (size_t k = start; k < i; k++) {
                char c = text.data[k];
                if ((c == '+' || c == '=' || c == '<' || c == '>') && gen->char_state.shift == SHIFT_NORMAL) {
                    sb_append_cstr(&gen->story, " ");
                    sb_append_escaped_xml(&gen->story, &c, 1);
                    sb_append_cstr(&gen->story, " ");
                } else if (c == '-' && gen->char_state.shift == SHIFT_NORMAL) {
                    sb_append_cstr(&gen->story, " − ");
                } else {
                    sb_append_escaped_xml(&gen->story, &c, 1);
                }
            }
            gen->char_state.slant = saved_slant;
        }
        gen->para_has_content = true;
        return;
    }

    xml_activate_current_char_style(gen);

    if (space_before && gen->para_has_content) {
        sb_append_cstr(&gen->story, " ");
    }

    sb_append_escaped_xml(&gen->story, text.data, text.len);
    gen->para_has_content = true;
}

static void xml_emit_page_ref(XmlGenerator *gen, const char *target, const char *href, bool space_before) {
    if (gen->capture_plain_buf) {
        if (space_before && gen->capture_plain_buf->len > 0) sb_append_cstr(gen->capture_plain_buf, " ");
        sb_append_cstr(gen->capture_plain_buf, "1");
        return;
    }
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
    const char *eff_href = (href && href[0]) ? href : gen->current_href;
    if (eff_href && eff_href[0]) {
        sb_appendf(&gen->story, "<page-ref ref=\"%s\" style=\"cs%zu\" href=\"%s\"/>",
                   target ? target : "", cs_id + 1, eff_href);
    } else {
        sb_appendf(&gen->story, "<page-ref ref=\"%s\" style=\"cs%zu\"/>",
                   target ? target : "", cs_id + 1);
    }
    gen->para_has_content = true;
}

static void xml_emit_line_break(XmlGenerator *gen, Measure extra_v) {
    if (gen->capture_plain_buf) {
        sb_append_cstr(gen->capture_plain_buf, " ");
        return;
    }
    xml_ensure_para_open(gen);
    xml_close_active_span(gen);
    if (fabs(extra_v.mm) > 0.001) {
        char b[32];
        measure_format(extra_v, b, sizeof(b));
        sb_appendf(&gen->story, "<br extra=\"%s\"/>", b);
    } else {
        sb_append_cstr(&gen->story, "<br/>");
    }
    gen->para_has_content = true;
}

static void xml_emit_hspace(XmlGenerator *gen, Measure width, const char *fill_mode, bool space_before) {
    if (gen->capture_plain_buf) {
        if ((fill_mode != nullptr || width.mm > 0.1) && gen->capture_plain_buf->len > 0) {
            sb_append_cstr(gen->capture_plain_buf, " ");
        }
        return;
    }
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
    if (fill_mode && fill_mode[0] != '\0') {
        sb_appendf(&gen->story, "<hspace fill=\"%s\" style=\"cs%zu\"/>", fill_mode, cs_id + 1);
    } else {
        char b[32];
        measure_format(width, b, sizeof(b));
        sb_appendf(&gen->story, "<hspace width=\"%s\" style=\"cs%zu\"/>", b, cs_id + 1);
    }
    gen->para_has_content = true;
}

static void xml_emit_rule(XmlGenerator *gen, Measure width, Measure height, Measure raise, bool space_before) {
    if (gen->capture_plain_buf) return;
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
    char bw[32], bh[32], br[32];
    measure_format(width,  bw, sizeof(bw));
    measure_format(height, bh, sizeof(bh));
    measure_format(raise,  br, sizeof(br));
    sb_appendf(&gen->story, "<rule width=\"%s\" height=\"%s\" raise=\"%s\" style=\"cs%zu\"/>",
               bw, bh, br, cs_id + 1);
    gen->para_has_content = true;
}

static void xml_emit_box(XmlGenerator *gen, Measure width, char align, Measure border, Measure pad, Measure raise, const CharState *box_cs, StrView text, bool space_before) {
    if (gen->capture_plain_buf) {
        if (space_before && gen->capture_plain_buf->len > 0) {
            sb_append_cstr(gen->capture_plain_buf, " ");
        }
        sb_append_mem(gen->capture_plain_buf, text.data, text.len);
        return;
    }
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, box_cs ? box_cs : &gen->char_state);
    char bw[32], bb[32], bp[32], br[32];
    measure_format(width,  bw, sizeof(bw));
    measure_format(border, bb, sizeof(bb));
    measure_format(pad,    bp, sizeof(bp));
    measure_format(raise,  br, sizeof(br));
    sb_appendf(&gen->story,
               "<box width=\"%s\" align=\"%c\" border=\"%s\" pad=\"%s\" raise=\"%s\" style=\"cs%zu\">",
               bw, align ? align : 'c', bb, bp, br, cs_id + 1);
    sb_append_escaped_xml(&gen->story, text.data, text.len);
    sb_append_cstr(&gen->story, "</box>");
    gen->para_has_content = true;
}

static void xml_emit_mfrac(XmlGenerator *gen, StrView num, StrView den, bool has_bar, bool has_paren, const CharState *cs, bool space_before) {
    if (gen->capture_plain_buf) {
        if (space_before && gen->capture_plain_buf->len > 0) sb_append_cstr(gen->capture_plain_buf, " ");
        sb_append_cstr(gen->capture_plain_buf, "(");
        sb_append_mem(gen->capture_plain_buf, num.data, num.len);
        sb_append_cstr(gen->capture_plain_buf, ")/(");
        sb_append_mem(gen->capture_plain_buf, den.data, den.len);
        sb_append_cstr(gen->capture_plain_buf, ")");
        return;
    }
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, cs ? cs : &gen->char_state);
    sb_appendf(&gen->story, "<mfrac bar=\"%s\" delim=\"%s\" style=\"cs%zu\" num=\"",
               has_bar ? "true" : "false", has_paren ? "paren" : "none", cs_id + 1);
    sb_append_escaped_xml(&gen->story, num.data, num.len);
    sb_append_cstr(&gen->story, "\" den=\"");
    sb_append_escaped_xml(&gen->story, den.data, den.len);
    sb_append_cstr(&gen->story, "\"/>");
    gen->para_has_content = true;
}

static void xml_emit_msqrt(XmlGenerator *gen, StrView index_sv, StrView body, const CharState *cs, bool space_before) {
    if (gen->capture_plain_buf) {
        if (space_before && gen->capture_plain_buf->len > 0) sb_append_cstr(gen->capture_plain_buf, " ");
        sb_append_cstr(gen->capture_plain_buf, "√(");
        sb_append_mem(gen->capture_plain_buf, body.data, body.len);
        sb_append_cstr(gen->capture_plain_buf, ")");
        return;
    }
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, cs ? cs : &gen->char_state);
    sb_appendf(&gen->story, "<msqrt style=\"cs%zu\" index=\"", cs_id + 1);
    sb_append_escaped_xml(&gen->story, index_sv.data, index_sv.len);
    sb_append_cstr(&gen->story, "\" body=\"");
    sb_append_escaped_xml(&gen->story, body.data, body.len);
    sb_append_cstr(&gen->story, "\"/>");
    gen->para_has_content = true;
}

static void xml_emit_mover(XmlGenerator *gen, const char *kind, StrView body, const CharState *cs, bool space_before) {
    if (gen->capture_plain_buf) {
        if (space_before && gen->capture_plain_buf->len > 0) sb_append_cstr(gen->capture_plain_buf, " ");
        sb_append_mem(gen->capture_plain_buf, body.data, body.len);
        return;
    }
    xml_ensure_para_open(gen);
    if (space_before && gen->para_has_content) {
        xml_activate_current_char_style(gen);
        sb_append_cstr(&gen->story, " ");
    }
    xml_close_active_span(gen);
    size_t cs_id = xml_intern_char_style(gen, cs ? cs : &gen->char_state);
    sb_appendf(&gen->story, "<mover kind=\"%s\" style=\"cs%zu\" body=\"", kind, cs_id + 1);
    sb_append_escaped_xml(&gen->story, body.data, body.len);
    sb_append_cstr(&gen->story, "\"/>");
    gen->para_has_content = true;
}

static void xml_emit_vspace(XmlGenerator *gen, Measure height, bool starred, bool fill) {
    if (gen->capture_plain_buf) return;
    xml_close_para(gen);
    if (fill) {
        sb_append_cstr(&gen->story, "      <vspace fill=\"true\"/>\n");
    } else {
        char b[32];
        measure_format(height, b, sizeof(b));
        if (starred) {
            sb_appendf(&gen->story, "      <vspace height=\"%s\" starred=\"true\"/>\n", b);
        } else {
            sb_appendf(&gen->story, "      <vspace height=\"%s\"/>\n", b);
        }
    }
}

static void xml_emit_page_break(XmlGenerator *gen) {
    if (gen->capture_plain_buf) return;
    xml_close_para(gen);
    sb_append_cstr(&gen->story, "      <break type=\"page\"/>\n");
}

static void xml_gen_finalize(XmlGenerator *gen) {
    xml_close_para(gen);
    if (!gen->file) {
        sb_free(&gen->story);
        sb_free(&gen->pending_footnotes);
        return;
    }

    char b1[32], b2[32], b3[32], b4[32], b5[32], b6[32];

    fprintf(gen->file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(gen->file, "<document version=\"0.2\">\n");

    fprintf(gen->file, "  <definitions>\n");

    measure_format(gen->page_width,    b1, sizeof(b1));
    measure_format(gen->page_height,   b2, sizeof(b2));
    fprintf(gen->file, "    <page-defaults width=\"%s\" height=\"%s\" orientation=\"%s\">\n",
            b1, b2, orientation_to_str(gen->orientation));

    measure_format(gen->margin_top,    b1, sizeof(b1));
    measure_format(gen->margin_bottom, b2, sizeof(b2));
    measure_format(gen->margin_left,   b3, sizeof(b3));
    measure_format(gen->margin_right,  b4, sizeof(b4));
    fprintf(gen->file, "      <margins top=\"%s\" bottom=\"%s\" left=\"%s\" right=\"%s\"/>\n",
            b1, b2, b3, b4);
    fprintf(gen->file, "      <page-style mode=\"");
    fprint_escaped_xml(gen->file, gen->page_style[0] ? gen->page_style : "plain");
    fprintf(gen->file, "\" numbering=\"");
    fprint_escaped_xml(gen->file, gen->page_numbering[0] ? gen->page_numbering : "arabic");
    fprintf(gen->file, "\" left-mark=\"");
    fprint_escaped_xml(gen->file, gen->left_mark);
    fprintf(gen->file, "\" right-mark=\"");
    fprint_escaped_xml(gen->file, gen->right_mark);
    fprintf(gen->file, "\"/>\n");
    fprintf(gen->file, "    </page-defaults>\n");

    fprintf(gen->file, "    <fonts>\n");
    for (size_t i = 0; i < gen->font_count; i++) {
        const FontFamilyDef *ff = &gen->fonts[i];
        fprintf(gen->file, "      <font-family id=\"f%zu\" name=\"", i + 1);
        fprint_escaped_xml(gen->file, ff->family);
        fprintf(gen->file, "\">\n");

        const FontWeight weights[4] = { WEIGHT_NORMAL, WEIGHT_BOLD,   WEIGHT_NORMAL, WEIGHT_BOLD };
        const FontSlant  slants[4]  = { SLANT_NORMAL,  SLANT_NORMAL,  SLANT_ITALIC,  SLANT_ITALIC };

        for (size_t v = 0; v < 4; v++) {
            if (ff->used_variants_mask & variant_bit(weights[v], slants[v])) {
                fprintf(gen->file,
                        "        <variant id=\"f%zu-%s\" weight=\"%s\" slant=\"%s\"/>\n",
                        i + 1,
                        variant_suffix(weights[v], slants[v]),
                        (weights[v] == WEIGHT_BOLD)  ? "bold"   : "normal",
                        (slants[v]  == SLANT_ITALIC) ? "italic" : "normal");
            }
        }
        fprintf(gen->file, "      </font-family>\n");
    }
    fprintf(gen->file, "    </fonts>\n");

    fprintf(gen->file, "    <styles>\n");
    for (size_t i = 0; i < gen->para_style_count; i++) {
        const ParaState *ps = &gen->para_styles[i];
        measure_format(ps->line_spacing, b1, sizeof(b1));
        measure_format(ps->space_before, b2, sizeof(b2));
        measure_format(ps->space_after,  b3, sizeof(b3));
        measure_format(ps->indent_first, b4, sizeof(b4));
        measure_format(ps->indent_left,  b5, sizeof(b5));
        measure_format(ps->indent_right, b6, sizeof(b6));

        fprintf(gen->file,
                "      <para-style id=\"ps%zu\" align=\"%s\" line-spacing=\"%s\" "
                "space-before=\"%s\" space-after=\"%s\" indent-first=\"%s\" "
                "indent-left=\"%s\" indent-right=\"%s\"/>\n",
                i + 1, align_to_str(ps->align), b1, b2, b3, b4, b5, b6);
    }

    for (size_t i = 0; i < gen->char_style_count; i++) {
        const CharStyleDef *cs = &gen->char_styles[i];
        measure_format(cs->size, b1, sizeof(b1));
        const char *vsuf = variant_suffix(cs->variant.weight, cs->variant.slant);

        fprintf(gen->file, "      <char-style id=\"cs%zu\" font-ref=\"f%zu-%s\" size=\"%s\" color=\"",
                i + 1, cs->variant.family_idx + 1, vsuf, b1);
        fprint_escaped_xml(gen->file, cs->color);
        fprintf(gen->file, "\" underline=\"%s\"", cs->underline ? "true" : "false");
        if (cs->small_caps) {
            fprintf(gen->file, " small-caps=\"true\"");
        }
        if (cs->shift == SHIFT_SUPER) {
            fprintf(gen->file, " shift=\"super\"");
        } else if (cs->shift == SHIFT_SUB) {
            fprintf(gen->file, " shift=\"sub\"");
        }
        fprintf(gen->file, "/>\n");
    }
    fprintf(gen->file, "    </styles>\n");
    fprintf(gen->file, "  </definitions>\n");

    if (gen->frame_count == 0) {
        fprintf(gen->file, "  <layout total-pages=\"%zu\"/>\n", gen->page_count);
    } else {
        fprintf(gen->file, "  <layout total-pages=\"%zu\">\n", gen->page_count);
        for (size_t i = 0; i < gen->frame_count; i++) {
            const LayoutFrame *fr = &gen->frames[i];
            measure_format(fr->x,      b1, sizeof(b1));
            measure_format(fr->y,      b2, sizeof(b2));
            measure_format(fr->width,  b3, sizeof(b3));
            measure_format(fr->height, b4, sizeof(b4));
            fprintf(gen->file,
                    "    <frame id=\"fr%zu\" page=\"%zu\" x=\"%s\" y=\"%s\" width=\"%s\" height=\"%s\"/>\n",
                    i + 1, fr->page_num + 1, b1, b2, b3, b4);
        }
        fprintf(gen->file, "  </layout>\n");
    }

    fprintf(gen->file, "  <body>\n");
    fprintf(gen->file, "    <story id=\"main\">\n");
    if (gen->story.len > 0) {
        fputs(gen->story.data, gen->file);
    }
    fprintf(gen->file, "    </story>\n");
    fprintf(gen->file, "  </body>\n");
    fprintf(gen->file, "</document>\n");

    fclose(gen->file);
    sb_free(&gen->story);
    sb_free(&gen->pending_footnotes);
}

// ============================================================================
// 8. Entorno de Macros, Entornos de Usuario, Contadores y Longitudes
// ============================================================================

static CounterDef *counter_find_mut(MacroEnv *env, StrView name) {
    name = sv_trim(name);
    for (size_t i = 0; i < env->counter_count; i++) {
        if (sv_equals(name, env->counters[i].name)) return &env->counters[i];
    }
    return nullptr;
}

static CounterDef *counter_register(MacroEnv *env, StrView name, StrView parent) {
    name   = sv_trim(name);
    parent = sv_trim(parent);
    CounterDef *existing = counter_find_mut(env, name);
    if (existing) return existing;
    if (env->counter_count >= MAX_COUNTERS || name.len == 0 || name.len >= sizeof(env->counters[0].name)) {
        return nullptr;
    }

    CounterDef *c = &env->counters[env->counter_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%.*s", (int)name.len, name.data);
    c->value = 0;

    if (parent.len > 0) {
        CounterDef *p = counter_find_mut(env, parent);
        if (p && p->reset_count < 8) {
            snprintf(p->resets[p->reset_count++], sizeof(p->resets[0]), "%.*s", (int)name.len, name.data);
        }
    }
    return c;
}

static void counter_reset_children(MacroEnv *env, CounterDef *c) {
    for (size_t i = 0; i < c->reset_count; i++) {
        StrView child_name = { c->resets[i], strlen(c->resets[i]) };
        CounterDef *child = counter_find_mut(env, child_name);
        if (child) {
            child->value = 0;
            counter_reset_children(env, child);
        }
    }
}

static void counter_step(MacroEnv *env, StrView name) {
    CounterDef *c = counter_find_mut(env, name);
    if (!c) c = counter_register(env, name, (StrView){nullptr, 0});
    if (!c) return;
    c->value++;
    counter_reset_children(env, c);
}

static void int_to_roman(int val, bool uppercase, char *out_buf, size_t buf_size) {
    if (val <= 0) {
        out_buf[0] = '\0';
        return;
    }
    static const int values[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char *const rom_low[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    static const char *const rom_up[]  = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
    const char *const *syms = uppercase ? rom_up : rom_low;

    out_buf[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < 13 && val > 0; i++) {
        while (val >= values[i]) {
            size_t slen = strlen(syms[i]);
            if (pos + slen < buf_size - 1) {
                memcpy(out_buf + pos, syms[i], slen);
                pos += slen;
                out_buf[pos] = '\0';
            }
            val -= values[i];
        }
    }
}

static void format_counter_value(const MacroEnv *env, StrView name, char style, char *out_buf, size_t buf_size) {
    CounterDef *c = counter_find_mut((MacroEnv *)env, name);
    int val = c ? c->value : 0;

    switch (style) {
        case 'r':
            int_to_roman(val, false, out_buf, buf_size);
            break;
        case 'R':
            int_to_roman(val, true, out_buf, buf_size);
            break;
        case 'a':
            if (val >= 1 && val <= 26) snprintf(out_buf, buf_size, "%c", 'a' + (val - 1));
            else out_buf[0] = '\0';
            break;
        case 'A':
            if (val >= 1 && val <= 26) snprintf(out_buf, buf_size, "%c", 'A' + (val - 1));
            else out_buf[0] = '\0';
            break;
        case 'f': {
            static const char *const fnsyms[] = { "", "∗", "†", "‡", "§", "¶", "‖", "∗∗", "††", "‡‡" };
            if (val >= 1 && val <= 9) snprintf(out_buf, buf_size, "%s", fnsyms[val]);
            else out_buf[0] = '\0';
            break;
        }
        case '1':
        default:
            snprintf(out_buf, buf_size, "%d", val);
            break;
    }
}

static void format_the_counter(const MacroEnv *env, const XmlGenerator *gen, StrView name, char *out_buf, size_t buf_size) {
    name = sv_trim(name);
    bool app_sec = (gen && gen->in_appendix && strcmp(gen->doc_class, "article") == 0);
    bool app_ch  = (gen && gen->in_appendix && strcmp(gen->doc_class, "article") != 0);

    if (sv_equals(name, "chapter")) {
        format_counter_value(env, name, app_ch ? 'A' : '1', out_buf, buf_size);
        return;
    }
    if (sv_equals(name, "section")) {
        format_counter_value(env, name, app_sec ? 'A' : '1', out_buf, buf_size);
        return;
    }
    if (sv_equals(name, "subsection")) {
        CounterDef *ss = counter_find_mut((MacroEnv *)env, name);
        char sbuf[16];
        format_counter_value(env, (StrView){"section", 7}, app_sec ? 'A' : '1', sbuf, sizeof(sbuf));
        snprintf(out_buf, buf_size, "%s.%d", sbuf, ss ? ss->value : 0);
        return;
    }
    if (sv_equals(name, "subsubsection")) {
        CounterDef *ss  = counter_find_mut((MacroEnv *)env, (StrView){"subsection", 10});
        CounterDef *sss = counter_find_mut((MacroEnv *)env, name);
        char sbuf[16];
        format_counter_value(env, (StrView){"section", 7}, app_sec ? 'A' : '1', sbuf, sizeof(sbuf));
        snprintf(out_buf, buf_size, "%s.%d.%d", sbuf, ss ? ss->value : 0, sss ? sss->value : 0);
        return;
    }
    if (sv_equals(name, "part")) {
        format_counter_value(env, name, 'R', out_buf, buf_size);
        return;
    }
    if (sv_equals(name, "enumii")) {
        format_counter_value(env, name, 'a', out_buf, buf_size);
        return;
    }
    if (sv_equals(name, "enumiii")) {
        format_counter_value(env, name, 'r', out_buf, buf_size);
        return;
    }
    if (sv_equals(name, "enumiv")) {
        format_counter_value(env, name, 'A', out_buf, buf_size);
        return;
    }
    format_counter_value(env, name, '1', out_buf, buf_size);
}

static LengthDef *length_find_mut(MacroEnv *env, StrView name) {
    name = sv_strip_leading_backslash(name);
    for (size_t i = 0; i < env->length_count; i++) {
        if (sv_equals(name, env->lengths[i].name)) return &env->lengths[i];
    }
    return nullptr;
}

static LengthDef *length_register(MacroEnv *env, StrView name, Measure initial) {
    name = sv_strip_leading_backslash(name);
    LengthDef *existing = length_find_mut(env, name);
    if (existing) return existing;
    if (env->length_count >= MAX_LENGTHS || name.len == 0 || name.len >= sizeof(env->lengths[0].name)) {
        return nullptr;
    }

    LengthDef *ld = &env->lengths[env->length_count++];
    snprintf(ld->name, sizeof(ld->name), "%.*s", (int)name.len, name.data);
    ld->value = initial;
    return ld;
}

static void prepare_para_state_mutation(XmlGenerator *gen) {
    if (gen->para_open && gen->para_has_content && gen->inline_depth == 0) {
        xml_close_para(gen);
    }
}

static void sync_standard_length_to_generator(XmlGenerator *gen, StrView name, Measure val) {
    name = sv_strip_leading_backslash(name);
    if (sv_equals(name, "parindent")) {
        prepare_para_state_mutation(gen);
        gen->para_state.indent_first = val;
    } else if (sv_equals(name, "parskip")) {
        prepare_para_state_mutation(gen);
        gen->para_state.space_after = val;
    } else if (sv_equals(name, "baselineskip")) {
        prepare_para_state_mutation(gen);
        gen->para_state.line_spacing = val;
    } else if (sv_equals(name, "leftskip") || sv_equals(name, "leftmargin")) {
        prepare_para_state_mutation(gen);
        gen->para_state.indent_left = val;
    } else if (sv_equals(name, "rightskip") || sv_equals(name, "rightmargin")) {
        prepare_para_state_mutation(gen);
        gen->para_state.indent_right = val;
    } else if (sv_equals(name, "itemsep") || sv_equals(name, "parsep")) {
        prepare_para_state_mutation(gen);
        gen->para_state.space_after = val;
    } else if (sv_equals(name, "topsep")) {
        prepare_para_state_mutation(gen);
        gen->para_state.space_before = val;
    } else if (sv_equals(name, "paperwidth")) {
        gen->page_width = val;
    } else if (sv_equals(name, "paperheight")) {
        gen->page_height = val;
    } else if (sv_equals(name, "topmargin")) {
        gen->margin_top = val;
    } else if (sv_equals(name, "oddsidemargin") || sv_equals(name, "evensidemargin")) {
        gen->margin_left = val;
    } else if (sv_equals(name, "textwidth")) {
        double rem = gen->page_width.mm - val.mm;
        if (rem >= 10.0) {
            gen->margin_left  = measure_make(rem * 0.5, UNIT_MM, 1.0);
            gen->margin_right = measure_make(rem * 0.5, UNIT_MM, 1.0);
        }
    } else if (sv_equals(name, "textheight")) {
        double rem = gen->page_height.mm - val.mm;
        if (rem >= 10.0) {
            gen->margin_top    = measure_make(rem * 0.5, UNIT_MM, 1.0);
            gen->margin_bottom = measure_make(rem * 0.5, UNIT_MM, 1.0);
        }
    }
}

static bool macro_env_define(MacroEnv *env, StrView name, size_t params, bool has_opt, StrView opt_def, StrView body, bool overwrite);

static void macro_env_init(MacroEnv *env) {
    memset(env, 0, sizeof(*env));

    // Contadores estándar de LaTeX2e
    counter_register(env, (StrView){"part", 4},           (StrView){nullptr, 0});
    counter_register(env, (StrView){"chapter", 7},        (StrView){nullptr, 0});
    counter_register(env, (StrView){"section", 7},        (StrView){"chapter", 7});
    counter_register(env, (StrView){"subsection", 10},    (StrView){"section", 7});
    counter_register(env, (StrView){"subsubsection", 13}, (StrView){"subsection", 10});
    counter_register(env, (StrView){"paragraph", 9},      (StrView){"subsubsection", 13});
    counter_register(env, (StrView){"subparagraph", 12},  (StrView){"paragraph", 9});
    counter_register(env, (StrView){"page", 4},           (StrView){nullptr, 0});
    counter_register(env, (StrView){"equation", 8},       (StrView){nullptr, 0});
    counter_register(env, (StrView){"figure", 6},         (StrView){nullptr, 0});
    counter_register(env, (StrView){"table", 5},          (StrView){nullptr, 0});
    counter_register(env, (StrView){"footnote", 8},       (StrView){nullptr, 0});
    counter_register(env, (StrView){"mpfootnote", 10},    (StrView){nullptr, 0});
    counter_register(env, (StrView){"enumi", 5},          (StrView){nullptr, 0});
    counter_register(env, (StrView){"enumii", 6},         (StrView){nullptr, 0});
    counter_register(env, (StrView){"enumiii", 7},        (StrView){nullptr, 0});
    counter_register(env, (StrView){"enumiv", 6},         (StrView){nullptr, 0});
    CounterDef *page_c = counter_find_mut(env, (StrView){"page", 4});
    if (page_c) page_c->value = 1;

    CounterDef *secnum = counter_register(env, (StrView){"secnumdepth", 11}, (StrView){nullptr, 0});
    if (secnum) secnum->value = 3;
    CounterDef *tocd = counter_register(env, (StrView){"tocdepth", 8}, (StrView){nullptr, 0});
    if (tocd) tocd->value = 3;

    // Longitudes estándar de LaTeX2e
    double em = 11.0 * MM_PER_PT;
    length_register(env, (StrView){"parindent", 9},       measure_make(8.0,   UNIT_MM, em));
    length_register(env, (StrView){"parskip", 7},         measure_make(2.5,   UNIT_MM, em));
    length_register(env, (StrView){"baselineskip", 12},   measure_make(5.5,   UNIT_MM, em));
    length_register(env, (StrView){"leftskip", 8},        measure_make(0.0,   UNIT_MM, em));
    length_register(env, (StrView){"rightskip", 9},       measure_make(0.0,   UNIT_MM, em));
    length_register(env, (StrView){"textwidth", 9},       measure_make(170.0, UNIT_MM, em));
    length_register(env, (StrView){"linewidth", 9},       measure_make(170.0, UNIT_MM, em));
    length_register(env, (StrView){"columnwidth", 11},    measure_make(170.0, UNIT_MM, em));
    length_register(env, (StrView){"textheight", 10},     measure_make(247.0, UNIT_MM, em));
    length_register(env, (StrView){"paperwidth", 10},     measure_make(210.0, UNIT_MM, em));
    length_register(env, (StrView){"paperheight", 11},    measure_make(297.0, UNIT_MM, em));
    length_register(env, (StrView){"topmargin", 9},       measure_make(25.0,  UNIT_MM, em));
    length_register(env, (StrView){"oddsidemargin", 13},  measure_make(20.0,  UNIT_MM, em));
    length_register(env, (StrView){"evensidemargin", 14}, measure_make(20.0,  UNIT_MM, em));
    length_register(env, (StrView){"fboxsep", 7},         measure_make(3.0,   UNIT_PT, em));
    length_register(env, (StrView){"fboxrule", 8},        measure_make(0.4,   UNIT_PT, em));
    length_register(env, (StrView){"tabcolsep", 9},       measure_make(6.0,   UNIT_PT, em));
    length_register(env, (StrView){"arrayrulewidth", 14}, measure_make(0.4,   UNIT_PT, em));
    length_register(env, (StrView){"unitlength", 10},     measure_make(1.0,   UNIT_PT, em));
    length_register(env, (StrView){"itemsep", 7},         measure_make(1.2,   UNIT_MM, em));
    length_register(env, (StrView){"parsep", 6},          measure_make(1.0,   UNIT_MM, em));
    length_register(env, (StrView){"topsep", 6},          measure_make(2.0,   UNIT_MM, em));
    length_register(env, (StrView){"partopsep", 9},       measure_make(1.0,   UNIT_MM, em));
    length_register(env, (StrView){"labelsep", 8},        measure_make(2.2,   UNIT_MM, em));
    length_register(env, (StrView){"labelwidth", 10},     measure_make(6.0,   UNIT_MM, em));
    length_register(env, (StrView){"leftmargin", 10},     measure_make(8.0,   UNIT_MM, em));
    length_register(env, (StrView){"rightmargin", 11},    measure_make(0.0,   UNIT_MM, em));
    length_register(env, (StrView){"listparindent", 13},  measure_make(0.0,   UNIT_MM, em));
    length_register(env, (StrView){"itemindent", 10},     measure_make(0.0,   UNIT_MM, em));

    // Macros estándar para viñetas y etiquetas de listas anidadas (itemize / enumerate)
    macro_env_define(env, (StrView){"labelitemi", 10},   0, false, (StrView){0}, (StrView){"\\textbullet", 11}, true);
    macro_env_define(env, (StrView){"labelitemii", 11},  0, false, (StrView){0}, (StrView){"\\textendash", 11}, true);
    macro_env_define(env, (StrView){"labelitemiii", 12}, 0, false, (StrView){0}, (StrView){"\\textasteriskcentered", 21}, true);
    macro_env_define(env, (StrView){"labelitemiv", 11},  0, false, (StrView){0}, (StrView){"\\textperiodcentered", 19}, true);

    macro_env_define(env, (StrView){"labelenumi", 10},   0, false, (StrView){0}, (StrView){"\\arabic{enumi}.", 15}, true);
    macro_env_define(env, (StrView){"labelenumii", 11},  0, false, (StrView){0}, (StrView){"(\\alph{enumii})", 15}, true);
    macro_env_define(env, (StrView){"labelenumiii", 12}, 0, false, (StrView){0}, (StrView){"\\roman{enumiii}.", 16}, true);
    macro_env_define(env, (StrView){"labelenumiv", 11},  0, false, (StrView){0}, (StrView){"\\Alph{enumiv}.", 14}, true);

    macro_env_define(env, (StrView){"figurename", 10},   0, false, (StrView){0}, (StrView){"Figura", 6}, true);
    macro_env_define(env, (StrView){"tablename", 9},     0, false, (StrView){0}, (StrView){"Cuadro", 6}, true);
    macro_env_define(env, (StrView){"contentsname", 12}, 0, false, (StrView){0}, (StrView){"Índice general", 15}, true);
    macro_env_define(env, (StrView){"listfigurename", 14}, 0, false, (StrView){0}, (StrView){"Índice de figuras", 18}, true);
    macro_env_define(env, (StrView){"listtablename", 13}, 0, false, (StrView){0}, (StrView){"Índice de cuadros", 18}, true);
    macro_env_define(env, (StrView){"refname", 7},       0, false, (StrView){0}, (StrView){"Referencias", 11}, true);
    macro_env_define(env, (StrView){"bibname", 7},       0, false, (StrView){0}, (StrView){"Bibliografía", 13}, true);
    macro_env_define(env, (StrView){"indexname", 9},     0, false, (StrView){0}, (StrView){"Índice alfabético", 19}, true);
}

static void macro_env_free(MacroEnv *env);

static void macro_env_reset_for_pass2(MacroEnv *env) {
    size_t saved_lbl_c = env->label_count;
    size_t saved_bib_c = env->bibitem_count;
    size_t saved_toc_c = env->toc_count;
    size_t saved_idx_c = env->idx_count;

    LabelDef *lbls = malloc(saved_lbl_c * sizeof(LabelDef));
    BibItemDef *bibs = malloc(saved_bib_c * sizeof(BibItemDef));
    TocEntryDef *tocs = malloc(saved_toc_c * sizeof(TocEntryDef));
    IndexEntryDef *idxs = malloc(saved_idx_c * sizeof(IndexEntryDef));

    if (lbls && saved_lbl_c > 0) memcpy(lbls, env->labels, saved_lbl_c * sizeof(LabelDef));
    if (bibs && saved_bib_c > 0) memcpy(bibs, env->bibitems, saved_bib_c * sizeof(BibItemDef));
    if (tocs && saved_toc_c > 0) memcpy(tocs, env->toc_entries, saved_toc_c * sizeof(TocEntryDef));
    if (idxs && saved_idx_c > 0) memcpy(idxs, env->idx_entries, saved_idx_c * sizeof(IndexEntryDef));

    macro_env_free(env);
    macro_env_init(env);

    if (lbls && saved_lbl_c > 0) { memcpy(env->labels, lbls, saved_lbl_c * sizeof(LabelDef)); env->label_count = saved_lbl_c; }
    if (bibs && saved_bib_c > 0) { memcpy(env->bibitems, bibs, saved_bib_c * sizeof(BibItemDef)); env->bibitem_count = saved_bib_c; }
    if (tocs && saved_toc_c > 0) { memcpy(env->toc_entries, tocs, saved_toc_c * sizeof(TocEntryDef)); env->toc_count = saved_toc_c; }
    if (idxs && saved_idx_c > 0) { memcpy(env->idx_entries, idxs, saved_idx_c * sizeof(IndexEntryDef)); env->idx_count = saved_idx_c; }

    free(lbls);
    free(bibs);
    free(tocs);
    free(idxs);
    env->is_prescan = false;
}

static void macro_env_free(MacroEnv *env) {
    for (size_t i = 0; i < env->count; i++) {
        free(env->items[i].opt_default);
        free(env->items[i].template_body);
    }
    for (size_t i = 0; i < env->env_count; i++) {
        free(env->envs[i].opt_default);
        free(env->envs[i].begin_body);
        free(env->envs[i].end_body);
    }
    for (size_t i = 0; i < env->savebox_count; i++) {
        free(env->saveboxes[i].content);
    }
    env->count         = 0;
    env->env_count     = 0;
    env->savebox_count = 0;
}

static UserMacro *macro_env_find_mut(MacroEnv *env, StrView name) {
    name = sv_strip_leading_backslash(name);
    for (size_t i = 0; i < env->count; i++) {
        if (sv_equals(name, env->items[i].name)) return &env->items[i];
    }
    return nullptr;
}

static const UserMacro *macro_env_find(const MacroEnv *env, StrView name) {
    return macro_env_find_mut((MacroEnv *)env, name);
}

static bool macro_env_define(MacroEnv *env, StrView name, size_t params, bool has_opt, StrView opt_def, StrView body, bool overwrite) {
    name = sv_strip_leading_backslash(name);
    if (name.len == 0 || name.len >= sizeof(env->items[0].name)) return false;
    if (params > MAX_PARAMS) params = MAX_PARAMS;

    UserMacro *m = macro_env_find_mut(env, name);
    if (m && !overwrite) {
        return true; // \providecommand no sobrescribe si ya existe
    }
    if (!m) {
        if (env->count >= MAX_MACROS) return false;
        m = &env->items[env->count++];
        memset(m, 0, sizeof(*m));
        snprintf(m->name, sizeof(m->name), "%.*s", (int)name.len, name.data);
    } else {
        free(m->opt_default);
        free(m->template_body);
        m->opt_default   = nullptr;
        m->template_body = nullptr;
    }

    m->param_count     = params;
    m->has_opt_default = has_opt;
    m->opt_default     = has_opt ? dup_sv(opt_def) : nullptr;
    m->template_body   = dup_sv(body);
    return (m->template_body != nullptr);
}

static UserEnv *user_env_find_mut(MacroEnv *env, StrView name) {
    name = sv_trim(name);
    for (size_t i = 0; i < env->env_count; i++) {
        if (sv_equals(name, env->envs[i].name)) return &env->envs[i];
    }
    return nullptr;
}

static bool user_env_define(MacroEnv *env, StrView name, size_t params, bool has_opt, StrView opt_def, StrView begin_body, StrView end_body, bool overwrite) {
    name = sv_trim(name);
    if (name.len == 0 || name.len >= sizeof(env->envs[0].name)) return false;
    if (params > MAX_PARAMS) params = MAX_PARAMS;

    UserEnv *ue = user_env_find_mut(env, name);
    if (ue && !overwrite) return true;
    if (!ue) {
        if (env->env_count >= MAX_USER_ENVS) return false;
        ue = &env->envs[env->env_count++];
        memset(ue, 0, sizeof(*ue));
        snprintf(ue->name, sizeof(ue->name), "%.*s", (int)name.len, name.data);
    } else {
        free(ue->opt_default);
        free(ue->begin_body);
        free(ue->end_body);
    }

    ue->param_count     = params;
    ue->has_opt_default = has_opt;
    ue->opt_default     = has_opt ? dup_sv(opt_def) : nullptr;
    ue->begin_body      = dup_sv(begin_body);
    ue->end_body        = dup_sv(end_body);
    return (ue->begin_body != nullptr && ue->end_body != nullptr);
}

static void substitute_macro_params(StrBuf *out, const char *tpl, size_t param_count, StrView args[]) {
    const char *ptr = tpl;
    while (*ptr != '\0') {
        if (*ptr == '#' && *(ptr + 1) == '#') {
            sb_append_mem(out, "#", 1);
            ptr += 2;
        } else if (*ptr == '#' && isdigit((unsigned char)*(ptr + 1))) {
            size_t idx = (size_t)(*(ptr + 1) - '1');
            if (idx < param_count) {
                sb_append_mem(out, args[idx].data, args[idx].len);
            }
            ptr += 2;
        } else {
            sb_append_mem(out, ptr, 1);
            ptr++;
        }
    }
}

static void execute_tex_fragment(Parser *p, StrView fragment, bool preserve_space) {
    if (p->macro_depth >= MAX_MACRO_DEPTH) {
        fprintf(stderr, "Error: recursión excesiva en expansión TeX\n");
        return;
    }

    Parser sub = {
        .lexer            = lexer_init(fragment.data, fragment.len, p->env->allow_at),
        .env              = p->env,
        .gen              = p->gen,
        .macro_depth      = p->macro_depth + 1,
        .cmd_space_before = false
    };

    parser_advance(&sub);
    if (preserve_space) {
        sub.current.space_before = true;
    }
    parse_and_execute(&sub);
}

static void eval_tex_to_plain(Parser *p, StrView expr, char *out, size_t out_sz) {
    if (out_sz == 0) return;
    out[0] = '\0';

    StrBuf plain_sb;
    if (!sb_init(&plain_sb, 128)) return;

    StrBuf *prev_capture    = p->gen->capture_plain_buf;
    size_t prev_inline      = p->gen->inline_depth;
    CharState saved_cs      = p->gen->char_state;
    ParaState saved_ps      = p->gen->para_state;

    p->gen->capture_plain_buf = &plain_sb;
    p->gen->inline_depth++;
    execute_tex_fragment(p, expr, false);
    p->gen->inline_depth      = prev_inline;
    p->gen->capture_plain_buf = prev_capture;
    p->gen->char_state        = saved_cs;
    p->gen->para_state        = saved_ps;

    snprintf(out, out_sz, "%s", plain_sb.data);
    sb_free(&plain_sb);
}

static void eval_tex_box(Parser *p, StrView expr, char *out, size_t out_sz, CharState *out_cs) {
    if (out_sz == 0) return;
    out[0] = '\0';
    if (out_cs) *out_cs = p->gen->char_state;

    StrBuf plain_sb;
    if (!sb_init(&plain_sb, 128)) return;

    StrBuf *prev_capture         = p->gen->capture_plain_buf;
    bool prev_has_cap_cs         = p->gen->has_captured_char_state;
    CharState prev_cap_cs        = p->gen->captured_char_state;
    size_t prev_inline           = p->gen->inline_depth;
    CharState saved_cs           = p->gen->char_state;
    ParaState saved_ps           = p->gen->para_state;

    p->gen->capture_plain_buf       = &plain_sb;
    p->gen->has_captured_char_state = false;
    p->gen->inline_depth++;
    execute_tex_fragment(p, expr, false);
    if (out_cs) {
        *out_cs = p->gen->has_captured_char_state ? p->gen->captured_char_state : p->gen->char_state;
    }
    p->gen->inline_depth            = prev_inline;
    p->gen->capture_plain_buf       = prev_capture;
    p->gen->has_captured_char_state = prev_has_cap_cs;
    p->gen->captured_char_state     = prev_cap_cs;
    p->gen->char_state              = saved_cs;
    p->gen->para_state              = saved_ps;

    snprintf(out, out_sz, "%s", plain_sb.data);
    sb_free(&plain_sb);
}

static SaveBoxDef *savebox_find_mut(MacroEnv *env, StrView name) {
    name = sv_strip_leading_backslash(name);
    for (size_t i = 0; i < env->savebox_count; i++) {
        if (sv_equals(name, env->saveboxes[i].name)) return &env->saveboxes[i];
    }
    return nullptr;
}

static SaveBoxDef *savebox_register(MacroEnv *env, StrView name) {
    name = sv_strip_leading_backslash(name);
    SaveBoxDef *existing = savebox_find_mut(env, name);
    if (existing) return existing;
    if (env->savebox_count >= MAX_SAVEBOXES || name.len == 0 || name.len >= sizeof(env->saveboxes[0].name)) {
        return nullptr;
    }
    SaveBoxDef *sb = &env->saveboxes[env->savebox_count++];
    memset(sb, 0, sizeof(*sb));
    snprintf(sb->name, sizeof(sb->name), "%.*s", (int)name.len, name.data);
    return sb;
}

static void label_register(MacroEnv *env, StrView key, const char *ref_str, int page_num) {
    key = sv_trim(key);
    if (key.len == 0 || key.len >= sizeof(env->labels[0].key)) return;
    for (size_t i = 0; i < env->label_count; i++) {
        if (sv_equals(key, env->labels[i].key)) {
            snprintf(env->labels[i].ref_str, sizeof(env->labels[i].ref_str), "%s", ref_str ? ref_str : "");
            env->labels[i].page_num = page_num;
            return;
        }
    }
    if (env->label_count >= MAX_LABELS) return;
    LabelDef *ld = &env->labels[env->label_count++];
    snprintf(ld->key, sizeof(ld->key), "%.*s", (int)key.len, key.data);
    snprintf(ld->ref_str, sizeof(ld->ref_str), "%s", ref_str ? ref_str : "");
    ld->page_num = page_num;
}

static const LabelDef *label_find(const MacroEnv *env, StrView key) {
    key = sv_trim(key);
    for (size_t i = 0; i < env->label_count; i++) {
        if (sv_equals(key, env->labels[i].key)) return &env->labels[i];
    }
    return nullptr;
}

static void bibitem_register(MacroEnv *env, StrView key, const char *label) {
    key = sv_trim(key);
    if (key.len == 0 || key.len >= sizeof(env->bibitems[0].key)) return;
    for (size_t i = 0; i < env->bibitem_count; i++) {
        if (sv_equals(key, env->bibitems[i].key)) {
            snprintf(env->bibitems[i].label, sizeof(env->bibitems[i].label), "%s", label ? label : "");
            return;
        }
    }
    if (env->bibitem_count >= MAX_BIBITEMS) return;
    BibItemDef *bd = &env->bibitems[env->bibitem_count++];
    snprintf(bd->key, sizeof(bd->key), "%.*s", (int)key.len, key.data);
    snprintf(bd->label, sizeof(bd->label), "%s", label ? label : "");
}

static const BibItemDef *bibitem_find(const MacroEnv *env, StrView key) {
    key = sv_trim(key);
    for (size_t i = 0; i < env->bibitem_count; i++) {
        if (sv_equals(key, env->bibitems[i].key)) return &env->bibitems[i];
    }
    return nullptr;
}

static void make_anchor_id(const char *prefix, StrView raw_key, char *out, size_t out_sz) {
    raw_key = sv_trim(raw_key);
    size_t pos = 0;
    int n = snprintf(out, out_sz, "%s", prefix);
    if (n > 0) pos = (size_t)n;
    for (size_t i = 0; i < raw_key.len && pos + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)raw_key.data[i];
        if (isalnum(c) || c == '-' || c == '_' || c == ':') {
            out[pos++] = (char)c;
        } else if (c == ' ' || c == '.') {
            out[pos++] = '_';
        }
    }
    out[pos] = '\0';
}

static void toc_record_entry(MacroEnv *env, const char *kind, const char *level, const char *number, const char *title, int page_num, char *out_anchor, size_t out_sz) {
    if (out_anchor && out_sz > 0) out_anchor[0] = '\0';
    if (env->is_prescan) {
        if (env->toc_count >= MAX_TOC_ENTRIES) return;
        size_t idx = env->toc_count++;
        TocEntryDef *te = &env->toc_entries[idx];
        snprintf(te->kind,      sizeof(te->kind),      "%.7s",   kind   ? kind   : "toc");
        snprintf(te->level,     sizeof(te->level),     "%.23s",  level  ? level  : "section");
        snprintf(te->number,    sizeof(te->number),    "%.31s",  number ? number : "");
        snprintf(te->title,     sizeof(te->title),     "%.255s", title  ? title  : "");
        snprintf(te->anchor_id, sizeof(te->anchor_id), "toc:%zu", idx);
        te->page_num = page_num;
        if (out_anchor && out_sz > 0) snprintf(out_anchor, out_sz, "dest-toc:%zu", idx);
    } else {
        size_t idx = env->toc_pass2_cursor++;
        if (out_anchor && out_sz > 0) snprintf(out_anchor, out_sz, "dest-toc:%zu", idx);
    }
}

static void index_record_entry(MacroEnv *env, StrView entry, int page_num) {
    if (!env->is_prescan || env->idx_count >= MAX_INDEX_ENTRIES) return;
    entry = sv_trim(entry);
    if (entry.len == 0) return;
    IndexEntryDef *ie = &env->idx_entries[env->idx_count++];
    snprintf(ie->entry, sizeof(ie->entry), "%.*s", (int)entry.len, entry.data);
    ie->page_num = page_num;
}

static void expand_user_macro(Parser *p, const UserMacro *m, StrView args[]) {
    if (p->macro_depth >= MAX_MACRO_DEPTH) {
        fprintf(stderr, "Error: recursión excesiva expandiendo macro '\\%s'\n", m->name);
        return;
    }

    StrBuf expanded;
    if (!sb_init(&expanded, 256)) return;
    substitute_macro_params(&expanded, m->template_body, m->param_count, args);

    execute_tex_fragment(p, (StrView){ expanded.data, expanded.len }, p->cmd_space_before);
    sb_free(&expanded);
}

// ============================================================================
// 9. Comandos Nativos de Fase 1 y Base Tipográfica LaTeX2e
// ============================================================================

// Helper para parsear \newcommand, \renewcommand, \providecommand:
// Sintaxis estándar LaTeX2e: \newcommand[*]{\cmd}[n][opt_default]{body}
static void handle_command_definition(Parser *p, bool overwrite, bool allow_existing) {
    parser_consume_star(p);

    StrView cmd_name = {0};
    if (!parse_cmd_or_arg(p, &cmd_name)) return;
    cmd_name = sv_strip_leading_backslash(cmd_name);

    size_t num_args = 0;
    bool has_opt_default = false;
    StrView opt_default = {0};
    StrView body = {0};

    StrView opt1 = {0};
    if (parse_opt_arg(p, &opt1)) {
        long n = sv_to_long(p, opt1);
        if (n < 0) n = 0;
        if (n > (long)MAX_PARAMS) n = (long)MAX_PARAMS;
        num_args = (size_t)n;

        if (parse_opt_arg(p, &opt_default)) {
            has_opt_default = true;
        }
        if (!parse_arg(p, &body)) return;
    } else {
        if (!parse_arg(p, &body)) return;
    }

    if (!allow_existing && macro_env_find(p->env, cmd_name) != nullptr) {
        fprintf(stderr, "Aviso: comando '\\%.*s' ya estaba definido; sobrescribiendo\n",
                (int)cmd_name.len, cmd_name.data);
    }
    macro_env_define(p->env, cmd_name, num_args, has_opt_default, opt_default, body, overwrite);
}

static void builtin_newcommand(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    handle_command_definition(p, true, false);
}

static void builtin_renewcommand(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    handle_command_definition(p, true, true);
}

static void builtin_providecommand(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    handle_command_definition(p, false, true);
}

// Primitiva TeX \def\cmd#1#2{cuerpo}
static void builtin_def(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView cmd_name = {0};
    if (!parse_cmd_or_arg(p, &cmd_name)) return;
    cmd_name = sv_strip_leading_backslash(cmd_name);

    size_t max_param = 0;
    while (p->current.type == TOKEN_TEXT) {
        for (size_t i = 0; i + 1 < p->current.text.len; i++) {
            if (p->current.text.data[i] == '#' && isdigit((unsigned char)p->current.text.data[i + 1])) {
                size_t p_idx = (size_t)(p->current.text.data[i + 1] - '0');
                if (p_idx > max_param) max_param = p_idx;
            }
        }
        parser_advance(p);
    }

    StrView body = {0};
    if (!parse_arg(p, &body)) return;
    macro_env_define(p->env, cmd_name, max_param, false, (StrView){0}, body, true);
}

// Primitiva TeX \let\nuevo=\antiguo o \let\nuevo\antiguo
static void builtin_let(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView dst = {0}, src = {0};
    if (!parse_cmd_or_arg(p, &dst)) return;
    if (p->current.type == TOKEN_TEXT && sv_equals(sv_trim(p->current.text), "=")) {
        parser_advance(p);
    }
    if (!parse_cmd_or_arg(p, &src)) return;

    dst = sv_strip_leading_backslash(dst);
    src = sv_strip_leading_backslash(src);

    const UserMacro *m = macro_env_find(p->env, src);
    if (m) {
        StrView opt_sv = m->opt_default ? (StrView){ m->opt_default, strlen(m->opt_default) } : (StrView){0};
        StrView body_sv = { m->template_body, strlen(m->template_body) };
        macro_env_define(p->env, dst, m->param_count, m->has_opt_default, opt_sv, body_sv, true);
    } else {
        char body_buf[64];
        int n = snprintf(body_buf, sizeof(body_buf), "\\%.*s", (int)src.len, src.data);
        if (n > 0) {
            macro_env_define(p->env, dst, 0, false, (StrView){0}, (StrView){ body_buf, (size_t)n }, true);
        }
    }
}

// \newenvironment[*]{name}[n][default]{begin_def}{end_def}
static void handle_env_definition(Parser *p, bool overwrite) {
    parser_consume_star(p);
    StrView env_name = {0};
    if (!parse_arg(p, &env_name)) return;

    size_t num_args = 0;
    bool has_opt_default = false;
    StrView opt_default = {0};
    StrView opt1 = {0};

    if (parse_opt_arg(p, &opt1)) {
        long n = sv_to_long(p, opt1);
        if (n < 0) n = 0;
        if (n > (long)MAX_PARAMS) n = (long)MAX_PARAMS;
        num_args = (size_t)n;
        if (parse_opt_arg(p, &opt_default)) {
            has_opt_default = true;
        }
    }

    StrView begin_body = {0}, end_body = {0};
    if (!parse_arg(p, &begin_body)) return;
    if (!parse_arg(p, &end_body)) return;

    user_env_define(p->env, env_name, num_args, has_opt_default, opt_default, begin_body, end_body, overwrite);
}

static void builtin_newenvironment(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    handle_env_definition(p, true);
}

static void builtin_renewenvironment(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    handle_env_definition(p, true);
}

static void builtin_makeatletter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->env->allow_at   = true;
    p->lexer.allow_at  = true;
}

static void builtin_makeatother(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->env->allow_at   = false;
    p->lexer.allow_at  = false;
}

static void builtin_noop(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    (void)p;
}

// --- Contadores ---

static void builtin_newcounter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView parent = {0};
    parse_opt_arg(p, &parent);
    counter_register(p->env, args[0], parent);
}

static void builtin_setcounter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CounterDef *c = counter_find_mut(p->env, args[0]);
    if (!c) c = counter_register(p->env, args[0], (StrView){0});
    if (c) c->value = (int)sv_to_long(p, args[1]);
}

static void builtin_addtocounter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CounterDef *c = counter_find_mut(p->env, args[0]);
    if (!c) c = counter_register(p->env, args[0], (StrView){0});
    if (c) c->value += (int)sv_to_long(p, args[1]);
}

static void builtin_stepcounter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    counter_step(p->env, args[0]);
}

static void builtin_refstepcounter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    counter_step(p->env, args[0]);
    format_the_counter(p->env, p->gen, args[0], p->env->current_ref, sizeof(p->env->current_ref));
}

static void emit_formatted_counter(Parser *p, StrView cname, char style) {
    char buf[64];
    format_counter_value(p->env, cname, style, buf, sizeof(buf));
    xml_emit_text(p->gen, (StrView){ buf, strlen(buf) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_arabic(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_formatted_counter(p, args[0], '1');
}

static void builtin_roman(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_formatted_counter(p, args[0], 'r');
}

static void builtin_Roman(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_formatted_counter(p, args[0], 'R');
}

static void builtin_alph(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_formatted_counter(p, args[0], 'a');
}

static void builtin_Alph(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_formatted_counter(p, args[0], 'A');
}

static void builtin_fnsymbol(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_formatted_counter(p, args[0], 'f');
}

// --- Longitudes ---

static void builtin_newlength(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    length_register(p->env, name, measure_make(0.0, UNIT_MM, 1.0));
}

static void builtin_setlength(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, val_sv = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    if (!parse_arg(p, &val_sv)) return;

    Measure m = measure_parse(p, val_sv, UNIT_MM);
    LengthDef *ld = length_find_mut(p->env, name);
    if (!ld) ld = length_register(p->env, name, m);
    else ld->value = m;

    sync_standard_length_to_generator(p->gen, name, m);
}

static void builtin_addtolength(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, delta_sv = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    if (!parse_arg(p, &delta_sv)) return;

    Measure d = measure_parse(p, delta_sv, UNIT_MM);
    LengthDef *ld = length_find_mut(p->env, name);
    if (!ld) ld = length_register(p->env, name, d);
    else {
        ld->value = measure_make(ld->value.mm + d.mm, UNIT_MM, 1.0);
    }

    sync_standard_length_to_generator(p->gen, name, ld->value);
}

static void builtin_settowidth(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, text = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    if (!parse_arg(p, &text)) return;

    char plain[256];
    eval_tex_to_plain(p, text, plain, sizeof(plain));
    double w_mm = (double)strlen(plain) * p->gen->char_state.size.mm * 0.50;
    Measure m = measure_make(w_mm, UNIT_MM, 1.0);
    LengthDef *ld = length_find_mut(p->env, name);
    if (!ld) length_register(p->env, name, m);
    else ld->value = m;
}

static void builtin_settoheight(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, text = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    if (!parse_arg(p, &text)) return;

    Measure m = measure_make(p->gen->char_state.size.mm * 0.72, UNIT_MM, 1.0);
    LengthDef *ld = length_find_mut(p->env, name);
    if (!ld) length_register(p->env, name, m);
    else ld->value = m;
}

static void builtin_settodepth(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, text = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    if (!parse_arg(p, &text)) return;

    Measure m = measure_make(p->gen->char_state.size.mm * 0.22, UNIT_MM, 1.0);
    LengthDef *ld = length_find_mut(p->env, name);
    if (!ld) length_register(p->env, name, m);
    else ld->value = m;
}

// --- Símbolos dinámicos (\today, \rule) ---

static void builtin_today(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    static const char *const meses[] = {
        "enero", "febrero", "marzo", "abril", "mayo", "junio",
        "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"
    };
    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    char buf[64];
    if (lt) {
        snprintf(buf, sizeof(buf), "%d de %s de %d",
                 lt->tm_mday, meses[lt->tm_mon % 12], lt->tm_year + 1900);
    } else {
        snprintf(buf, sizeof(buf), "4 de noviembre de 2026");
    }
    xml_emit_text(p->gen, (StrView){ buf, strlen(buf) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_rule(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, StrView args[]) {
    Measure raise = (opt_c > 0) ? measure_parse(p, opt[0], UNIT_MM) : measure_make(0.0, UNIT_MM, 1.0);
    Measure w     = measure_parse(p, args[0], UNIT_MM);
    Measure h     = measure_parse(p, args[1], UNIT_MM);
    xml_emit_rule(p->gen, w, h, raise, p->cmd_space_before);
    p->cmd_space_before = false;
}

// --- Escala de Tamaños de Fuente de LaTeX2e ---

typedef struct {
    const char *name;
    double pt_10;
    double pt_11;
    double pt_12;
} FontSizeScaleEntry;

static const FontSizeScaleEntry LATEX_FONT_SIZES[] = {
    { "tiny",         5.0,   6.0,   6.0   },
    { "scriptsize",   7.0,   8.0,   8.0   },
    { "footnotesize", 8.0,   9.0,   10.0  },
    { "small",        9.0,   10.0,  10.95 },
    { "normalsize",   10.0,  11.0,  12.0  },
    { "large",        12.0,  12.0,  14.4  },
    { "Large",        14.4,  14.4,  17.28 },
    { "LARGE",        17.28, 17.28, 20.74 },
    { "huge",         20.74, 20.74, 24.88 },
    { "Huge",         24.88, 24.88, 24.88 },
};
constexpr size_t LATEX_FONT_SIZE_COUNT = sizeof(LATEX_FONT_SIZES) / sizeof(LATEX_FONT_SIZES[0]);

static bool apply_named_font_size(XmlGenerator *gen, StrView name) {
    for (size_t i = 0; i < LATEX_FONT_SIZE_COUNT; i++) {
        if (sv_equals(name, LATEX_FONT_SIZES[i].name)) {
            double pt = LATEX_FONT_SIZES[i].pt_11;
            if (fabs(gen->base_font_pt - 10.0) < 0.2)      pt = LATEX_FONT_SIZES[i].pt_10;
            else if (fabs(gen->base_font_pt - 12.0) < 0.2) pt = LATEX_FONT_SIZES[i].pt_12;

            gen->char_state.size = measure_make(pt, UNIT_PT, 1.0);
            if (!gen->para_open || !gen->para_has_content) {
                double spread = (gen->linespread_factor > 0.1) ? gen->linespread_factor : 1.0;
                gen->para_state.line_spacing = measure_make(pt * 1.28 * spread * MM_PER_PT, UNIT_MM, 1.0);
            }
            return true;
        }
    }
    return false;
}

static void builtin_tiny(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"tiny", 4}); }
static void builtin_scriptsize(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"scriptsize", 10}); }
static void builtin_footnotesize(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"footnotesize", 12}); }
static void builtin_small(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"small", 5}); }
static void builtin_normalsize(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"normalsize", 10}); }
static void builtin_large(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"large", 5}); }
static void builtin_Large(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"Large", 5}); }
static void builtin_LARGE(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"LARGE", 5}); }
static void builtin_huge(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"huge", 4}); }
static void builtin_Huge(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) { apply_named_font_size(p->gen, (StrView){"Huge", 4}); }

// --- Entornos \begin{...} y \end{...} ---

static void builtin_newpage(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]);

static void parse_tabular_cols_into(Parser *p, StrView spec, int *pending_left_bars) {
    size_t i = 0;
    while (i < spec.len && p->gen->tab_col_count < MAX_TAB_COLS) {
        char c = spec.data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (c == '|') {
            if (p->gen->tab_col_count == 0) {
                (*pending_left_bars)++;
            } else {
                p->gen->tab_cols[p->gen->tab_col_count - 1].border_right++;
            }
            i++;
            continue;
        }
        if (c == '@') {
            i++;
            while (i < spec.len && (spec.data[i] == ' ' || spec.data[i] == '\t')) i++;
            if (i < spec.len && spec.data[i] == '{') {
                int depth = 1;
                i++;
                while (i < spec.len && depth > 0) {
                    if (spec.data[i] == '{') depth++;
                    else if (spec.data[i] == '}') depth--;
                    i++;
                }
            }
            continue;
        }
        if (c == '*') {
            i++;
            while (i < spec.len && (spec.data[i] == ' ' || spec.data[i] == '\t')) i++;
            long count = 0;
            if (i < spec.len && spec.data[i] == '{') {
                i++;
                size_t nstart = i;
                while (i < spec.len && spec.data[i] != '}') i++;
                count = sv_to_long(p, (StrView){ spec.data + nstart, i - nstart });
                if (i < spec.len) i++;
            }
            while (i < spec.len && (spec.data[i] == ' ' || spec.data[i] == '\t')) i++;
            if (i < spec.len && spec.data[i] == '{') {
                i++;
                size_t bstart = i;
                int depth = 1;
                while (i < spec.len && depth > 0) {
                    if (spec.data[i] == '{') depth++;
                    else if (spec.data[i] == '}') depth--;
                    if (depth > 0) i++;
                }
                StrView sub = { spec.data + bstart, i - bstart };
                if (i < spec.len) i++;
                for (long r = 0; r < count; r++) {
                    parse_tabular_cols_into(p, sub, pending_left_bars);
                }
            }
            continue;
        }
        if (c == 'l' || c == 'c' || c == 'r' || c == 'p' || c == 'm' || c == 'b') {
            double col_w_mm = 0.0;
            char align_ch = (c == 'c' || c == 'r') ? c : 'l';
            i++;
            if (c == 'p' || c == 'm' || c == 'b') {
                while (i < spec.len && (spec.data[i] == ' ' || spec.data[i] == '\t')) i++;
                if (i < spec.len && spec.data[i] == '{') {
                    i++;
                    size_t wstart = i;
                    int depth = 1;
                    while (i < spec.len && depth > 0) {
                        if (spec.data[i] == '{') depth++;
                        else if (spec.data[i] == '}') depth--;
                        if (depth > 0) i++;
                    }
                    Measure mw = measure_parse(p, (StrView){ spec.data + wstart, i - wstart }, UNIT_MM);
                    col_w_mm = mw.mm;
                    if (i < spec.len) i++;
                }
            }
            TabColSpec *col = &p->gen->tab_cols[p->gen->tab_col_count++];
            col->align        = align_ch;
            col->border_left  = *pending_left_bars;
            col->border_right = 0;
            col->width_mm     = col_w_mm;
            *pending_left_bars = 0;
            continue;
        }
        i++;
    }
}

static void parse_tabular_cols(Parser *p, StrView spec) {
    p->gen->tab_col_count = 0;
    int pending_left_bars = 0;
    parse_tabular_cols_into(p, spec, &pending_left_bars);
    if (p->gen->tab_col_count == 0) {
        p->gen->tab_cols[0] = (TabColSpec){ .align = 'l', .border_left = 0, .border_right = 0, .width_mm = 0.0 };
        p->gen->tab_col_count = 1;
    }
}

static double get_unitlength_mm(Parser *p) {
    LengthDef *ul = length_find_mut(p->env, (StrView){"unitlength", 10});
    return (ul && ul->value.mm > 0.0) ? ul->value.mm : MM_PER_PT;
}

// Parsea coordenadas (x,y) del entorno picture de LaTeX2e
static bool parse_pic_coords(Parser *p, double *out_x, double *out_y) {
    if (p->current.type != TOKEN_TEXT || p->current.text.len == 0) return false;
    const char *cur = p->current.text.data;
    const char *tok_end = p->current.text.data + p->current.text.len;
    while (cur < tok_end && (*cur == ' ' || *cur == '\t')) cur++;
    if (cur >= tok_end || *cur != '(') return false;

    const char *start = cur + 1;
    const char *src_end = p->lexer.src + p->lexer.len;
    const char *close_paren = start;
    while (close_paren < src_end && *close_paren != ')') {
        close_paren++;
    }
    if (close_paren >= src_end) return false;

    char buf[128];
    size_t len = (size_t)(close_paren - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    char *comma = strchr(buf, ',');
    if (!comma) return false;
    *comma = '\0';
    *out_x = strtod(buf, nullptr);
    *out_y = strtod(comma + 1, nullptr);

    p->lexer.cursor = (size_t)((close_paren + 1) - p->lexer.src);
    parser_advance(p);
    return true;
}

static void builtin_begin(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView env_name = sv_trim(args[0]);

    // document no abre grupo para que las definiciones globales persistan
    if (sv_equals(env_name, "document")) {
        return;
    }

    if (p->gen->env_depth < MAX_ENV_DEPTH) {
        ActiveEnvFrame *ef = &p->gen->env_stack[p->gen->env_depth++];
        memset(ef, 0, sizeof(*ef));
        snprintf(ef->name, sizeof(ef->name), "%.*s", (int)env_name.len, env_name.data);
        ef->saved_state = (GroupFrame){
            .para_state = p->gen->para_state,
            .char_state = p->gen->char_state
        };

        // ¿Es un entorno definido por el usuario con \newenvironment?
        UserEnv *ue = user_env_find_mut(p->env, env_name);
        if (ue) {
            StrView uargs[MAX_PARAMS] = {0};
            size_t start_idx = 0;
            if (ue->has_opt_default && ue->param_count > 0) {
                StrView opt_val = {0};
                if (parse_opt_arg(p, &opt_val)) {
                    uargs[0] = opt_val;
                } else {
                    uargs[0] = (StrView){ ue->opt_default, strlen(ue->opt_default) };
                }
                start_idx = 1;
            }
            for (size_t i = start_idx; i < ue->param_count; i++) {
                parse_arg(p, &uargs[i]);
            }
            ef->end_expansion = dup_sv((StrView){ ue->end_body, strlen(ue->end_body) });

            StrBuf beg_exp;
            if (sb_init(&beg_exp, 256)) {
                substitute_macro_params(&beg_exp, ue->begin_body, ue->param_count, uargs);
                execute_tex_fragment(p, (StrView){ beg_exp.data, beg_exp.len }, false);
                sb_free(&beg_exp);
            }
            return;
        }

        // ¿Es un entorno de tamaño de fuente (\begin{large}...\end{large})?
        if (apply_named_font_size(p->gen, env_name)) {
            return;
        }

        // Entornos estándar de bloque, portada y carta
        if (sv_equals(env_name, "center")) {
            xml_close_para(p->gen);
            p->gen->para_state.align        = ALIGN_CENTER;
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "flushleft")) {
            xml_close_para(p->gen);
            p->gen->para_state.align        = ALIGN_LEFT;
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "flushright")) {
            xml_close_para(p->gen);
            p->gen->para_state.align        = ALIGN_RIGHT;
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "quote") || sv_equals(env_name, "quotation") || sv_equals(env_name, "verse")) {
            xml_close_para(p->gen);
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + 10.0, UNIT_MM, 1.0);
            p->gen->para_state.indent_right = measure_make(p->gen->para_state.indent_right.mm + 10.0, UNIT_MM, 1.0);
            if (sv_equals(env_name, "quote") || sv_equals(env_name, "verse")) {
                p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            }
        } else if (sv_equals(env_name, "abstract")) {
            xml_close_para(p->gen);
            const char *abs_hdr = "\\par{\\centering\\small\\bfseries Resumen\\par}";
            execute_tex_fragment(p, (StrView){ abs_hdr, strlen(abs_hdr) }, false);
            apply_named_font_size(p->gen, (StrView){"small", 5});
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + 12.0, UNIT_MM, 1.0);
            p->gen->para_state.indent_right = measure_make(p->gen->para_state.indent_right.mm + 12.0, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "titlepage")) {
            xml_close_para(p->gen);
        } else if (sv_equals(env_name, "letter")) {
            xml_close_para(p->gen);
            StrView recipient = {0};
            parse_arg(p, &recipient);
            if (p->gen->sender_address[0] != '\0') {
                StrBuf hdr;
                if (sb_init(&hdr, 256)) {
                    sb_appendf(&hdr, "\\par{\\raggedleft %s\\par}", p->gen->sender_address);
                    execute_tex_fragment(p, (StrView){ hdr.data, hdr.len }, false);
                    sb_free(&hdr);
                }
            }
            if (recipient.len > 0) {
                StrBuf rec;
                if (sb_init(&rec, 256)) {
                    sb_appendf(&rec, "\\par{\\raggedright %.*s\\par}", (int)recipient.len, recipient.data);
                    execute_tex_fragment(p, (StrView){ rec.data, rec.len }, false);
                    sb_free(&rec);
                }
            }
        } else if (sv_equals(env_name, "itemize")) {
            xml_close_para(p->gen);
            p->gen->itemize_depth++;
            size_t lvl = (p->gen->itemize_depth <= 4) ? p->gen->itemize_depth : 4;
            if (p->gen->list_depth < 8) {
                ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth++];
                memset(ls, 0, sizeof(*ls));
                ls->kind  = LIST_ITEMIZE;
                ls->level = lvl;
            }
            double step_mm = (lvl == 1) ? 8.0 : 6.5;
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + step_mm, UNIT_MM, 1.0);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(0.8, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(1.2, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "enumerate")) {
            xml_close_para(p->gen);
            p->gen->enumerate_depth++;
            size_t lvl = (p->gen->enumerate_depth <= 4) ? p->gen->enumerate_depth : 4;
            static const char *const enum_ctrs[4] = { "enumi", "enumii", "enumiii", "enumiv" };
            const char *cname = enum_ctrs[lvl - 1];
            CounterDef *c = counter_find_mut(p->env, (StrView){ cname, strlen(cname) });
            if (c) c->value = 0;

            if (p->gen->list_depth < 8) {
                ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth++];
                memset(ls, 0, sizeof(*ls));
                ls->kind  = LIST_ENUMERATE;
                ls->level = lvl;
                snprintf(ls->counter_name, sizeof(ls->counter_name), "%s", cname);
            }
            double step_mm = (lvl == 1) ? 8.0 : 6.5;
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + step_mm, UNIT_MM, 1.0);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(0.8, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(1.2, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "description")) {
            xml_close_para(p->gen);
            if (p->gen->list_depth < 8) {
                ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth++];
                memset(ls, 0, sizeof(*ls));
                ls->kind  = LIST_DESCRIPTION;
                ls->level = 1;
            }
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + 8.0, UNIT_MM, 1.0);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(1.0, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(1.4, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "list")) {
            xml_close_para(p->gen);
            StrView def_lbl = {0}, decls = {0};
            parse_arg(p, &def_lbl);
            parse_arg(p, &decls);
            if (p->gen->list_depth < 8) {
                ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth++];
                memset(ls, 0, sizeof(*ls));
                ls->kind  = LIST_CUSTOM;
                ls->level = 1;
                snprintf(ls->default_label, sizeof(ls->default_label), "%.*s", (int)def_lbl.len, def_lbl.data);
            }
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + 8.0, UNIT_MM, 1.0);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(0.8, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(1.2, UNIT_MM, 1.0);
            if (decls.len > 0) {
                execute_tex_fragment(p, decls, false);
            }
        } else if (sv_equals(env_name, "verbatim") || sv_equals(env_name, "verbatim*")) {
            bool v_star = sv_equals(env_name, "verbatim*");
            const char *end_tag = v_star ? "\\end{verbatim*}" : "\\end{verbatim}";
            size_t end_tag_len = strlen(end_tag);

            xml_close_para(p->gen);
            const char *src_end = p->lexer.src + p->lexer.len;
            const char *raw_cur = args[0].data + args[0].len + 1;
            if (raw_cur > src_end) raw_cur = src_end;

            const char *found_end = nullptr;
            for (const char *scan = raw_cur; scan + end_tag_len <= src_end; scan++) {
                if (memcmp(scan, end_tag, end_tag_len) == 0) {
                    found_end = scan;
                    break;
                }
            }
            const char *body_end = found_end ? found_end : src_end;

            if (raw_cur < body_end && *raw_cur == '\r') raw_cur++;
            if (raw_cur < body_end && *raw_cur == '\n') raw_cur++;
            if (body_end > raw_cur && *(body_end - 1) == '\n') {
                body_end--;
                if (body_end > raw_cur && *(body_end - 1) == '\r') body_end--;
            }

            p->gen->para_state.align        = ALIGN_LEFT;
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + 4.0, UNIT_MM, 1.0);
            snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_MONO_FAMILY);
            p->gen->char_state.weight       = WEIGHT_NORMAL;
            p->gen->char_state.slant        = SLANT_NORMAL;
            p->gen->char_state.small_caps   = false;

            StrBuf line_buf;
            if (sb_init(&line_buf, 256)) {
                for (const char *c = raw_cur; c < body_end; c++) {
                    if (*c == '\r') continue;
                    if (*c == '\n') {
                        if (line_buf.len == 0) {
                            sb_append_cstr(&line_buf, "\xC2\xA0");
                        }
                        xml_emit_text(p->gen, (StrView){ line_buf.data, line_buf.len }, false);
                        line_buf.len = 0;
                        line_buf.data[0] = '\0';
                        xml_emit_line_break(p->gen, measure_make(0.0, UNIT_MM, 1.0));
                    } else if (*c == ' ') {
                        sb_append_cstr(&line_buf, v_star ? "␣" : "\xC2\xA0");
                    } else if (*c == '\t') {
                        sb_append_cstr(&line_buf, v_star ? "␣␣␣␣" : "\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0");
                    } else {
                        sb_append_mem(&line_buf, c, 1);
                    }
                }
                if (line_buf.len > 0) {
                    xml_emit_text(p->gen, (StrView){ line_buf.data, line_buf.len }, false);
                }
                sb_free(&line_buf);
            }

            xml_close_para(p->gen);
            ActiveEnvFrame popped = p->gen->env_stack[--p->gen->env_depth];
            p->gen->para_state = popped.saved_state.para_state;
            p->gen->char_state = popped.saved_state.char_state;

            const char *resume_ptr = found_end ? (found_end + end_tag_len) : src_end;
            p->lexer.cursor = (size_t)((args[0].data + args[0].len + 1) - p->lexer.src);
            size_t target_cur = (size_t)(resume_ptr - p->lexer.src);
            while (p->lexer.cursor < target_cur) {
                lexer_advance(&p->lexer);
            }
            parser_advance(p);
            return;
        } else if (sv_equals(env_name, "minipage")) {
            xml_close_para(p->gen);
            StrView opt_tmp = {0};
            while (parse_opt_arg(p, &opt_tmp)) {}
            StrView mp_w_sv = {0};
            parse_arg(p, &mp_w_sv);
            Measure mp_w = measure_parse(p, mp_w_sv, UNIT_MM);
            double usable_w = p->gen->page_width.mm - p->gen->margin_left.mm - p->gen->margin_right.mm;
            if (p->gen->twocolumn) usable_w = (usable_w - 6.0) * 0.5;
            double rem_right = fmax(0.0, usable_w - p->gen->para_state.indent_left.mm - mp_w.mm);
            p->gen->para_state.indent_right = measure_make(rem_right, UNIT_MM, 1.0);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "tabular") || sv_equals(env_name, "tabular*") || sv_equals(env_name, "array")) {
            xml_close_para(p->gen);
            if (sv_equals(env_name, "tabular*")) {
                StrView tw_sv = {0};
                parse_arg(p, &tw_sv);
            }
            StrView pos_opt = {0};
            parse_opt_arg(p, &pos_opt);
            StrView col_spec = {0};
            parse_arg(p, &col_spec);

            parse_tabular_cols(p, col_spec);
            p->gen->in_tabular          = true;
            p->gen->tab_cur_col         = 0;
            p->gen->tab_row_open        = false;
            p->gen->tab_cell_open       = false;
            p->gen->tab_cur_span        = 1;
            p->gen->tab_base_char_state = p->gen->char_state;
            p->gen->tab_has_mc_override = false;

            LengthDef *tcs = length_find_mut(p->env, sv_equals(env_name, "array") ? (StrView){"arraycolsep", 11} : (StrView){"tabcolsep", 9});
            LengthDef *arw = length_find_mut(p->env, (StrView){"arrayrulewidth", 14});
            char btcs[32] = "6pt", barw[32] = "0.4pt";
            if (tcs) measure_format(tcs->value, btcs, sizeof(btcs));
            if (arw) measure_format(arw->value, barw, sizeof(barw));

            sb_appendf(&p->gen->story, "      <table cols=\"%zu\" align=\"%s\" tabcolsep=\"%s\" rulewidth=\"%s\">\n",
                       p->gen->tab_col_count, align_to_str(p->gen->para_state.align), btcs, barw);
        } else if (sv_equals(env_name, "math")) {
            p->gen->in_math          = true;
            p->gen->char_state.slant = SLANT_ITALIC;
        } else if (sv_equals(env_name, "displaymath") || sv_equals(env_name, "equation") ||
                   sv_equals(env_name, "equation*")   || sv_equals(env_name, "eqnarray") ||
                   sv_equals(env_name, "eqnarray*")) {
            xml_close_para(p->gen);
            p->gen->para_state.align        = ALIGN_CENTER;
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(2.2, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(2.2, UNIT_MM, 1.0);
            p->gen->in_math                 = true;
            p->gen->in_display_math         = true;
            p->gen->eq_suppress_num         = (sv_equals(env_name, "displaymath") ||
                                               sv_equals(env_name, "equation*") ||
                                               sv_equals(env_name, "eqnarray*"));
            p->gen->in_eqnarray             = (sv_equals(env_name, "eqnarray") || sv_equals(env_name, "eqnarray*"));
            p->gen->char_state.slant        = SLANT_ITALIC;
            if (!p->gen->eq_suppress_num) {
                counter_step(p->env, (StrView){"equation", 8});
                format_the_counter(p->env, p->gen, (StrView){"equation", 8}, p->env->current_ref, sizeof(p->env->current_ref));
            }
        } else if (sv_equals(env_name, "thebibliography")) {
            xml_close_para(p->gen);
            StrView widest = {0};
            parse_arg(p, &widest);
            const char *bib_hdr = (strcmp(p->gen->doc_class, "article") == 0 || strcmp(p->gen->doc_class, "slides") == 0)
                                  ? "\\section*{\\refname}"
                                  : "\\chapter*{\\bibname}";
            execute_tex_fragment(p, (StrView){ bib_hdr, strlen(bib_hdr) }, false);

            CounterDef *c = counter_find_mut(p->env, (StrView){"enumiv", 6});
            if (c) c->value = 0;
            if (p->gen->list_depth < 8) {
                ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth++];
                memset(ls, 0, sizeof(*ls));
                ls->kind  = LIST_ENUMERATE;
                ls->level = 4;
                snprintf(ls->counter_name, sizeof(ls->counter_name), "enumiv");
            }
            p->gen->para_state.indent_left  = measure_make(p->gen->para_state.indent_left.mm + 8.0, UNIT_MM, 1.0);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(0.8, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(1.2, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "theindex")) {
            xml_close_para(p->gen);
            const char *idx_hdr = "\\section*{\\indexname}";
            execute_tex_fragment(p, (StrView){ idx_hdr, strlen(idx_hdr) }, false);
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
            p->gen->para_state.space_before = measure_make(0.4, UNIT_MM, 1.0);
            p->gen->para_state.space_after  = measure_make(0.6, UNIT_MM, 1.0);
        } else if (sv_equals(env_name, "picture")) {
            xml_close_para(p->gen);
            double ul = get_unitlength_mm(p);
            double w_u = 100.0, h_u = 45.0, x0_u = 0.0, y0_u = 0.0;
            parse_pic_coords(p, &w_u, &h_u);
            parse_pic_coords(p, &x0_u, &y0_u);

            p->gen->in_picture        = true;
            p->gen->pic_w_mm          = w_u * ul;
            p->gen->pic_h_mm          = h_u * ul;
            p->gen->pic_x0_mm         = x0_u * ul;
            p->gen->pic_y0_mm         = y0_u * ul;
            p->gen->pic_cur_x_mm      = 0.0;
            p->gen->pic_cur_y_mm      = 0.0;
            p->gen->pic_line_thick_mm = 0.4 * MM_PER_PT;

            if (!p->gen->capture_plain_buf) {
                char bw[32], bh[32];
                measure_format(measure_make(p->gen->pic_w_mm, UNIT_MM, 1.0), bw, sizeof(bw));
                measure_format(measure_make(p->gen->pic_h_mm, UNIT_MM, 1.0), bh, sizeof(bh));
                sb_appendf(&p->gen->story, "      <picture width=\"%s\" height=\"%s\" align=\"%s\">\n",
                           bw, bh, align_to_str(p->gen->para_state.align));
            }
        } else if (sv_equals(env_name, "figure") || sv_equals(env_name, "figure*") ||
                   sv_equals(env_name, "table")  || sv_equals(env_name, "table*")) {
            xml_close_para(p->gen);
            StrView pos_opt = {0};
            parse_opt_arg(p, &pos_opt);
            bool is_tbl = (sv_equals(env_name, "table") || sv_equals(env_name, "table*"));
            snprintf(p->gen->active_float_kind, sizeof(p->gen->active_float_kind),
                     "%s", is_tbl ? "table" : "figure");
            xml_emit_vspace(p->gen, measure_make(2.5, UNIT_MM, 1.0), false, false);
        } else if (sv_equals(env_name, "lrbox")) {
            StrView box_cmd = {0};
            parse_cmd_or_arg(p, &box_cmd);
            SaveBoxDef *sb = savebox_register(p->env, box_cmd);

            const char *end_tag = "\\end{lrbox}";
            size_t end_tag_len  = strlen(end_tag);
            const char *src_end = p->lexer.src + p->lexer.len;
            const char *raw_cur = &p->lexer.src[p->lexer.cursor];
            if (p->current.type != TOKEN_EOF && p->current.text.data >= p->lexer.src && p->current.text.data <= src_end) {
                raw_cur = p->current.text.data;
            }
            const char *found_end = nullptr;
            for (const char *scan = raw_cur; scan + end_tag_len <= src_end; scan++) {
                if (memcmp(scan, end_tag, end_tag_len) == 0) {
                    found_end = scan;
                    break;
                }
            }
            const char *body_end = found_end ? found_end : src_end;
            if (sb) {
                free(sb->content);
                sb->content = dup_sv(sv_trim((StrView){ raw_cur, (size_t)(body_end - raw_cur) }));
            }
            ActiveEnvFrame popped = p->gen->env_stack[--p->gen->env_depth];
            p->gen->para_state = popped.saved_state.para_state;
            p->gen->char_state = popped.saved_state.char_state;

            const char *resume_ptr = found_end ? (found_end + end_tag_len) : src_end;
            size_t target_cur = (size_t)(resume_ptr - p->lexer.src);
            while (p->lexer.cursor < target_cur) {
                lexer_advance(&p->lexer);
            }
            parser_advance(p);
            return;
        }
    }
}

static void builtin_end(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView env_name = sv_trim(args[0]);
    if (sv_equals(env_name, "document")) {
        xml_close_para(p->gen);
        return;
    }

    if (p->gen->env_depth > 0) {
        ActiveEnvFrame ef = p->gen->env_stack[--p->gen->env_depth];
        if (ef.end_expansion) {
            execute_tex_fragment(p, (StrView){ ef.end_expansion, strlen(ef.end_expansion) }, false);
            free(ef.end_expansion);
        }
        if (sv_equals(env_name, "tabular") || sv_equals(env_name, "tabular*") || sv_equals(env_name, "array")) {
            if (p->gen->tab_row_open) {
                if (p->gen->para_has_content || p->gen->tab_cur_col > 0) {
                    xml_table_end_row(p->gen, measure_make(0.0, UNIT_MM, 1.0));
                } else {
                    if (p->gen->tab_cell_open) {
                        xml_close_active_span(p->gen);
                        sb_append_cstr(&p->gen->story, "</cell>\n");
                        p->gen->tab_cell_open = false;
                        p->gen->para_open     = false;
                    }
                    sb_append_cstr(&p->gen->story, "        </row>\n");
                    p->gen->tab_row_open = false;
                }
            }
            sb_append_cstr(&p->gen->story, "      </table>\n");
            p->gen->in_tabular = false;
            p->gen->para_open  = false;
        } else if (sv_equals(env_name, "math")) {
            p->gen->in_math = false;
        } else if (sv_equals(env_name, "displaymath") || sv_equals(env_name, "equation") ||
                   sv_equals(env_name, "equation*")   || sv_equals(env_name, "eqnarray") ||
                   sv_equals(env_name, "eqnarray*")) {
            if ((sv_equals(env_name, "equation") || sv_equals(env_name, "eqnarray")) && !p->gen->eq_suppress_num) {
                char eq_num[80];
                snprintf(eq_num, sizeof(eq_num), "   (%s)", p->env->current_ref);
                CharState saved_cs = p->gen->char_state;
                bool saved_math    = p->gen->in_math;
                p->gen->in_math          = false;
                p->gen->char_state.slant = SLANT_NORMAL;
                xml_emit_text(p->gen, (StrView){ eq_num, strlen(eq_num) }, false);
                p->gen->char_state = saved_cs;
                p->gen->in_math    = saved_math;
            }
            xml_close_para(p->gen);
            p->gen->in_math         = false;
            p->gen->in_display_math = false;
            p->gen->in_eqnarray     = false;
            p->gen->eq_suppress_num = false;
        } else if (sv_equals(env_name, "picture")) {
            if (!p->gen->capture_plain_buf) {
                sb_append_cstr(&p->gen->story, "      </picture>\n");
            }
            p->gen->in_picture = false;
        } else if (sv_equals(env_name, "figure") || sv_equals(env_name, "figure*") ||
                   sv_equals(env_name, "table")  || sv_equals(env_name, "table*")) {
            xml_close_para(p->gen);
            xml_emit_vspace(p->gen, measure_make(2.5, UNIT_MM, 1.0), false, false);
            p->gen->active_float_kind[0] = '\0';
        } else if (sv_equals(env_name, "center") || sv_equals(env_name, "flushleft") ||
                   sv_equals(env_name, "flushright") || sv_equals(env_name, "quote") ||
                   sv_equals(env_name, "quotation") || sv_equals(env_name, "verse") ||
                   sv_equals(env_name, "abstract") || sv_equals(env_name, "letter") ||
                   sv_equals(env_name, "itemize") || sv_equals(env_name, "enumerate") ||
                   sv_equals(env_name, "description") || sv_equals(env_name, "list") ||
                   sv_equals(env_name, "minipage") || sv_equals(env_name, "thebibliography") ||
                   sv_equals(env_name, "theindex")) {
            xml_close_para(p->gen);
        } else if (sv_equals(env_name, "titlepage")) {
            xml_close_para(p->gen);
            builtin_newpage(p, false, nullptr, 0, nullptr);
        }

        if (sv_equals(env_name, "itemize") || sv_equals(env_name, "enumerate") ||
            sv_equals(env_name, "description") || sv_equals(env_name, "list") ||
            sv_equals(env_name, "thebibliography")) {
            p->gen->has_pending_item_label = false;
            if (p->gen->list_depth > 0) {
                ListLevelState ls = p->gen->list_stack[--p->gen->list_depth];
                if (ls.kind == LIST_ITEMIZE && p->gen->itemize_depth > 0) {
                    p->gen->itemize_depth--;
                } else if (ls.kind == LIST_ENUMERATE && p->gen->enumerate_depth > 0 && !sv_equals(env_name, "thebibliography")) {
                    p->gen->enumerate_depth--;
                }
            }
        }

        p->gen->para_state = ef.saved_state.para_state;
        p->gen->char_state = ef.saved_state.char_state;
    }
}

static void builtin_usecounter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView cname = sv_trim(args[0]);
    CounterDef *c = counter_find_mut(p->env, cname);
    if (!c) c = counter_register(p->env, cname, (StrView){0});
    if (c) c->value = 0;

    if (p->gen->list_depth > 0) {
        ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth - 1];
        snprintf(ls->counter_name, sizeof(ls->counter_name), "%.*s", (int)cname.len, cname.data);
    }
}

static void builtin_item(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, [[maybe_unused]] StrView args[]) {
    xml_close_para(p->gen);
    p->cmd_space_before = false;

    if (p->gen->list_depth == 0) {
        if (opt_c > 0) {
            eval_tex_to_plain(p, opt[0], p->gen->pending_item_label, sizeof(p->gen->pending_item_label));
            p->gen->has_pending_item_label = true;
        }
        return;
    }

    ListLevelState *ls = &p->gen->list_stack[p->gen->list_depth - 1];

    if (ls->kind == LIST_DESCRIPTION) {
        if (opt_c > 0) {
            p->gen->para_state.indent_first = measure_make(-4.0, UNIT_MM, 1.0);
            StrBuf dbuf;
            if (sb_init(&dbuf, opt[0].len + 48)) {
                sb_appendf(&dbuf, "{\\bfseries %.*s}\\hspace{2.2mm}", (int)opt[0].len, opt[0].data);
                execute_tex_fragment(p, (StrView){ dbuf.data, dbuf.len }, false);
                sb_free(&dbuf);
            }
            p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
        }
        return;
    }

    if (opt_c > 0) {
        eval_tex_to_plain(p, opt[0], p->gen->pending_item_label, sizeof(p->gen->pending_item_label));
        p->gen->has_pending_item_label = true;
        return;
    }

    if (ls->counter_name[0] != '\0') {
        StrView cname = { ls->counter_name, strlen(ls->counter_name) };
        counter_step(p->env, cname);
        format_the_counter(p->env, p->gen, cname, p->env->current_ref, sizeof(p->env->current_ref));
    }

    if (ls->kind == LIST_ITEMIZE) {
        static const char *const item_macros[4] = {
            "\\labelitemi", "\\labelitemii", "\\labelitemiii", "\\labelitemiv"
        };
        const char *mexpr = item_macros[(ls->level >= 1 && ls->level <= 4) ? (ls->level - 1) : 0];
        eval_tex_to_plain(p, (StrView){ mexpr, strlen(mexpr) },
                          p->gen->pending_item_label, sizeof(p->gen->pending_item_label));
        p->gen->has_pending_item_label = true;
    } else if (ls->kind == LIST_ENUMERATE) {
        static const char *const enum_macros[4] = {
            "\\labelenumi", "\\labelenumii", "\\labelenumiii", "\\labelenumiv"
        };
        const char *mexpr = enum_macros[(ls->level >= 1 && ls->level <= 4) ? (ls->level - 1) : 0];
        eval_tex_to_plain(p, (StrView){ mexpr, strlen(mexpr) },
                          p->gen->pending_item_label, sizeof(p->gen->pending_item_label));
        p->gen->has_pending_item_label = true;
    } else if (ls->kind == LIST_CUSTOM) {
        eval_tex_to_plain(p, (StrView){ ls->default_label, strlen(ls->default_label) },
                          p->gen->pending_item_label, sizeof(p->gen->pending_item_label));
        p->gen->has_pending_item_label = true;
    }
}

// --- Estructura, Clases de Documento, Archivos y Geometría ---

static void builtin_documentclass(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, StrView args[]) {
    StrView cls = sv_trim(args[0]);
    snprintf(p->gen->doc_class, sizeof(p->gen->doc_class), "%.*s", (int)cls.len, cls.data);

    double base_pt = 10.0;
    if (sv_equals(cls, "slides")) base_pt = 12.0;

    for (size_t i = 0; i < opt_c; i++) {
        const char *cur = opt[i].data;
        const char *end = opt[i].data + opt[i].len;
        while (cur < end) {
            const char *comma = memchr(cur, ',', (size_t)(end - cur));
            size_t len = comma ? (size_t)(comma - cur) : (size_t)(end - cur);
            StrView item = sv_trim((StrView){ cur, len });

            if (sv_equals(item, "10pt")) base_pt = 10.0;
            else if (sv_equals(item, "11pt")) base_pt = 11.0;
            else if (sv_equals(item, "12pt")) base_pt = 12.0;
            else if (sv_equals(item, "a4paper")) {
                p->gen->page_width  = measure_make(210.0, UNIT_MM, 1.0);
                p->gen->page_height = measure_make(297.0, UNIT_MM, 1.0);
            } else if (sv_equals(item, "a5paper")) {
                p->gen->page_width  = measure_make(148.0, UNIT_MM, 1.0);
                p->gen->page_height = measure_make(210.0, UNIT_MM, 1.0);
            } else if (sv_equals(item, "b5paper")) {
                p->gen->page_width  = measure_make(176.0, UNIT_MM, 1.0);
                p->gen->page_height = measure_make(250.0, UNIT_MM, 1.0);
            } else if (sv_equals(item, "letterpaper")) {
                p->gen->page_width  = measure_make(215.9, UNIT_MM, 1.0);
                p->gen->page_height = measure_make(279.4, UNIT_MM, 1.0);
            } else if (sv_equals(item, "legalpaper")) {
                p->gen->page_width  = measure_make(215.9, UNIT_MM, 1.0);
                p->gen->page_height = measure_make(355.6, UNIT_MM, 1.0);
            } else if (sv_equals(item, "executivepaper")) {
                p->gen->page_width  = measure_make(184.15, UNIT_MM, 1.0);
                p->gen->page_height = measure_make(266.7, UNIT_MM, 1.0);
            } else if (sv_equals(item, "landscape")) {
                p->gen->orientation = ORIENT_LANDSCAPE;
                sync_page_dimensions_to_orientation(p->gen);
            } else if (sv_equals(item, "portrait")) {
                p->gen->orientation = ORIENT_PORTRAIT;
                sync_page_dimensions_to_orientation(p->gen);
            } else if (sv_equals(item, "twocolumn")) {
                p->gen->twocolumn = true;
            } else if (sv_equals(item, "onecolumn")) {
                p->gen->twocolumn = false;
            } else if (sv_equals(item, "twoside")) {
                p->gen->twoside = true;
            } else if (sv_equals(item, "oneside")) {
                p->gen->twoside = false;
            }
            cur = comma ? (comma + 1) : end;
        }
    }
    p->gen->base_font_pt            = base_pt;
    p->gen->char_state.size         = measure_make(base_pt, UNIT_PT, 1.0);
    p->gen->para_state.line_spacing = measure_make(base_pt * 1.35 * MM_PER_PT, UNIT_MM, 1.0);
}

static void builtin_includeonly(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView list = sv_trim(args[0]);
    snprintf(p->env->include_only, sizeof(p->env->include_only), "%.*s", (int)list.len, list.data);
}

static bool is_allowed_by_includeonly(const MacroEnv *env, StrView name) {
    if (env->include_only[0] == '\0') return true;
    const char *cur = env->include_only;
    const char *end = cur + strlen(cur);
    while (cur < end) {
        const char *comma = strchr(cur, ',');
        size_t len = comma ? (size_t)(comma - cur) : (size_t)(end - cur);
        StrView item = sv_trim((StrView){ cur, len });
        if (item.len == name.len && strncmp(item.data, name.data, name.len) == 0) {
            return true;
        }
        cur = comma ? (comma + 1) : end;
    }
    return false;
}

static void builtin_input(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    char path_buf[256];
    StrView raw = sv_trim(args[0]);
    snprintf(path_buf, sizeof(path_buf), "%.*s", (int)raw.len, raw.data);

    if (!has_tex_extension(path_buf)) {
        if (strlen(path_buf) + 4 < sizeof(path_buf)) {
            strcat(path_buf, ".tex");
        } else {
            fprintf(stderr, "Error: ruta demasiado larga '%s'\n", path_buf);
            return;
        }
    }

    if (!process_file(path_buf, p->env, p->gen, p->macro_depth)) {
        fprintf(stderr, "Error: fallo al incluir '%s'\n", path_buf);
    }
}

static void builtin_include(Parser *p, bool starred, StrView opt[], size_t opt_c, StrView args[]) {
    StrView raw = sv_trim(args[0]);
    if (!is_allowed_by_includeonly(p->env, raw)) return;

    if (p->gen->para_open || p->gen->story.len > 0) {
        builtin_newpage(p, false, nullptr, 0, nullptr);
    }
    builtin_input(p, starred, opt, opt_c, args);
    xml_close_para(p->gen);
}

// --- Metadatos, Portada (\maketitle), Seccionado y Clase Letter ---

static void builtin_title(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView v = sv_trim(args[0]);
    snprintf(p->gen->title, sizeof(p->gen->title), "%.*s", (int)v.len, v.data);
}

static void builtin_author(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView v = sv_trim(args[0]);
    snprintf(p->gen->author, sizeof(p->gen->author), "%.*s", (int)v.len, v.data);
}

static void builtin_date(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView v = sv_trim(args[0]);
    snprintf(p->gen->date, sizeof(p->gen->date), "%.*s", (int)v.len, v.data);
}

static void builtin_thanks(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    StrView v = sv_trim(args[0]);
    snprintf(p->gen->thanks, sizeof(p->gen->thanks), "%.*s", (int)v.len, v.data);
    if (p->gen->para_open) {
        xml_emit_text(p->gen, (StrView){"∗", 3}, false);
    }
    p->cmd_space_before = false;
}

static void builtin_and(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    xml_emit_text(p->gen, (StrView){"   •   ", 9}, true);
    p->cmd_space_before = false;
}

static void builtin_maketitle(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    xml_close_para(p->gen);
    StrBuf mbuf;
    if (!sb_init(&mbuf, 512)) return;

    if (p->gen->title[0] != '\0') {
        sb_appendf(&mbuf, "\\par{\\centering\\LARGE\\bfseries %s\\par}", p->gen->title);
    }
    if (p->gen->author[0] != '\0') {
        sb_appendf(&mbuf, "\\par{\\centering\\large %s\\par}", p->gen->author);
    }
    if (p->gen->date[0] != '\0') {
        sb_appendf(&mbuf, "\\par{\\centering\\normalsize %s\\par}", p->gen->date);
    } else {
        sb_append_cstr(&mbuf, "\\par{\\centering\\normalsize \\today\\par}");
    }
    if (p->gen->thanks[0] != '\0') {
        sb_appendf(&mbuf, "\\par{\\centering\\footnotesize ∗ %s\\par}", p->gen->thanks);
    }

    execute_tex_fragment(p, (StrView){ mbuf.data, mbuf.len }, false);
    sb_free(&mbuf);
    p->cmd_space_before = false;
}

// Seccionado nativo de LaTeX2e: \part, \chapter, \section, \subsection, \subsubsection, \paragraph, \subparagraph
static void emit_section_heading(Parser *p, int level, const char *counter_name, bool starred, StrView title, const char *size_cmd, double before_mm, double after_mm, bool inline_runin) {
    CounterDef *secnum = counter_find_mut(p->env, (StrView){"secnumdepth", 11});
    int max_num_level = secnum ? secnum->value : 3;
    bool numbered = (!starred && level <= max_num_level && p->gen->matter_mode == 0);

    char num_prefix[64] = {0};
    if (numbered && counter_name != nullptr) {
        StrView cname = { counter_name, strlen(counter_name) };
        counter_step(p->env, cname);

        if (level == 0) { // \chapter
            if (p->gen->in_appendix) {
                char abuf[16];
                format_counter_value(p->env, cname, 'A', abuf, sizeof(abuf));
                snprintf(num_prefix, sizeof(num_prefix), "Apéndice %s: ", abuf);
                snprintf(p->env->current_ref, sizeof(p->env->current_ref), "%s", abuf);
            } else {
                char cbuf[16];
                format_counter_value(p->env, cname, '1', cbuf, sizeof(cbuf));
                snprintf(num_prefix, sizeof(num_prefix), "Capítulo %s. ", cbuf);
                snprintf(p->env->current_ref, sizeof(p->env->current_ref), "%s", cbuf);
            }
        } else if (level == -1) { // \part
            char rbuf[16];
            format_counter_value(p->env, cname, 'R', rbuf, sizeof(rbuf));
            snprintf(num_prefix, sizeof(num_prefix), "Parte %s — ", rbuf);
            snprintf(p->env->current_ref, sizeof(p->env->current_ref), "%s", rbuf);
        } else if (level == 1 && p->gen->in_appendix && strcmp(p->gen->doc_class, "article") == 0) {
            char abuf[16];
            format_counter_value(p->env, cname, 'A', abuf, sizeof(abuf));
            snprintf(num_prefix, sizeof(num_prefix), "%s. ", abuf);
            snprintf(p->env->current_ref, sizeof(p->env->current_ref), "%s", abuf);
        } else {
            char tbuf[32];
            format_the_counter(p->env, p->gen, cname, tbuf, sizeof(tbuf));
            snprintf(num_prefix, sizeof(num_prefix), "%s%s ", tbuf, (level == 1) ? "." : "");
            snprintf(p->env->current_ref, sizeof(p->env->current_ref), "%s", tbuf);
        }
    }

    if (level <= 3 && !starred) {
        char plain_title[128];
        eval_tex_to_plain(p, title, plain_title, sizeof(plain_title));
        if (level <= 1) {
            if (level <= 0) {
                snprintf(p->gen->left_mark, sizeof(p->gen->left_mark), "%s%s", num_prefix, plain_title);
            } else {
                snprintf(p->gen->right_mark, sizeof(p->gen->right_mark), "%s%s", num_prefix, plain_title);
                if (p->gen->left_mark[0] == '\0') {
                    snprintf(p->gen->left_mark, sizeof(p->gen->left_mark), "%s%s", num_prefix, plain_title);
                }
            }
        }
        const char *lvl_name = "section";
        if (level == -1)      lvl_name = "part";
        else if (level == 0)  lvl_name = "chapter";
        else if (level == 1)  lvl_name = "section";
        else if (level == 2)  lvl_name = "subsection";
        else if (level == 3)  lvl_name = "subsubsection";

        CounterDef *pc = counter_find_mut(p->env, (StrView){"page", 4});
        int pnum = pc ? pc->value : (int)p->gen->page_count;
        char toc_anchor[64];
        toc_record_entry(p->env, "toc", lvl_name, p->env->current_ref, plain_title, pnum, toc_anchor, sizeof(toc_anchor));
        xml_close_para(p->gen);
        xml_emit_anchor(p->gen, toc_anchor);
    }

    StrBuf hbuf;
    if (!sb_init(&hbuf, 256)) return;

    if (inline_runin) {
        xml_close_para(p->gen);
        if (level == 4) p->gen->next_para_noindent = true;
        sb_appendf(&hbuf, "{\\sffamily\\bfseries %s%.*s} ", num_prefix, (int)title.len, title.data);
        execute_tex_fragment(p, (StrView){ hbuf.data, hbuf.len }, false);
    } else {
        xml_close_para(p->gen);
        sb_appendf(&hbuf,
                   "\\par{\\raggedright\\parmargins{0mm}{0mm}\\parspacing{%.2fmm}{%.2fmm}\\sffamily\\%s\\bfseries %s%.*s\\par}",
                   before_mm, after_mm, size_cmd, num_prefix, (int)title.len, title.data);
        execute_tex_fragment(p, (StrView){ hbuf.data, hbuf.len }, false);
        p->gen->next_para_noindent = true;
    }

    sb_free(&hbuf);
    p->cmd_space_before = false;
}

static void builtin_part(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_section_heading(p, -1, "part", starred, args[0], "LARGE", 7.0, 4.0, false);
}

static void builtin_chapter(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    if (p->gen->para_open || p->gen->story.len > 0) {
        builtin_newpage(p, false, nullptr, 0, nullptr);
    }
    emit_section_heading(p, 0, "chapter", starred, args[0], " Huge" + 1, 8.0, 5.0, false);
}

static void builtin_section(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_section_heading(p, 1, "section", starred, args[0], "Large", 5.0, 2.5, false);
}

static void builtin_subsection(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_section_heading(p, 2, "subsection", starred, args[0], "large", 3.5, 1.8, false);
}

static void builtin_subsubsection(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_section_heading(p, 3, "subsubsection", starred, args[0], "normalsize", 2.5, 1.2, false);
}

static void builtin_paragraph(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_section_heading(p, 4, "paragraph", starred, args[0], "normalsize", 1.5, 0.0, true);
}

static void builtin_subparagraph(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    emit_section_heading(p, 5, "subparagraph", starred, args[0], "normalsize", 1.0, 0.0, true);
}

static void builtin_appendix(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->in_appendix = true;
    CounterDef *ch = counter_find_mut(p->env, (StrView){"chapter", 7});
    if (ch) ch->value = 0;
    CounterDef *sec = counter_find_mut(p->env, (StrView){"section", 7});
    if (sec) sec->value = 0;
}

static void builtin_frontmatter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->matter_mode = 1;
}

static void builtin_mainmatter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->matter_mode = 0;
}

static void builtin_backmatter(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->matter_mode = 2;
}

// Comandos de la clase letter
static void builtin_address(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    snprintf(p->gen->sender_address, sizeof(p->gen->sender_address), "%.*s", (int)args[0].len, args[0].data);
}
static void builtin_signature(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    snprintf(p->gen->sender_signature, sizeof(p->gen->sender_signature), "%.*s", (int)args[0].len, args[0].data);
}
static void builtin_name(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    snprintf(p->gen->sender_name, sizeof(p->gen->sender_name), "%.*s", (int)args[0].len, args[0].data);
}
static void builtin_location(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    snprintf(p->gen->sender_location, sizeof(p->gen->sender_location), "%.*s", (int)args[0].len, args[0].data);
}
static void builtin_telephone(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    snprintf(p->gen->sender_telephone, sizeof(p->gen->sender_telephone), "%.*s", (int)args[0].len, args[0].data);
}
static void builtin_opening(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrBuf b;
    if (sb_init(&b, 128)) {
        sb_appendf(&b, "\\par\\noindent %.*s\\par", (int)args[0].len, args[0].data);
        execute_tex_fragment(p, (StrView){ b.data, b.len }, false);
        sb_free(&b);
    }
}
static void builtin_closing(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrBuf b;
    if (sb_init(&b, 256)) {
        const char *sig = (p->gen->sender_signature[0] != '\0') ? p->gen->sender_signature : p->gen->sender_name;
        sb_appendf(&b, "\\par\\noindent %.*s\\par\\noindent %s\\par", (int)args[0].len, args[0].data, sig);
        execute_tex_fragment(p, (StrView){ b.data, b.len }, false);
        sb_free(&b);
    }
}
static void builtin_cc(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrBuf b;
    if (sb_init(&b, 128)) {
        sb_appendf(&b, "\\par\\noindent\\textbf{cc:} %.*s\\par", (int)args[0].len, args[0].data);
        execute_tex_fragment(p, (StrView){ b.data, b.len }, false);
        sb_free(&b);
    }
}
static void builtin_encl(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrBuf b;
    if (sb_init(&b, 128)) {
        sb_appendf(&b, "\\par\\noindent\\textbf{Adj.:} %.*s\\par", (int)args[0].len, args[0].data);
        execute_tex_fragment(p, (StrView){ b.data, b.len }, false);
        sb_free(&b);
    }
}
static void builtin_onecolumn(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->twocolumn = false;
    xml_close_para(p->gen);
}

static void builtin_pagesize(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    p->gen->page_width  = measure_parse(p, args[0], UNIT_MM);
    p->gen->page_height = measure_parse(p, args[1], UNIT_MM);
    p->gen->orientation = (p->gen->page_width.mm > p->gen->page_height.mm)
                          ? ORIENT_LANDSCAPE
                          : ORIENT_PORTRAIT;
}

static void builtin_orientation(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    if (sv_equals(args[0], "landscape") || sv_equals(args[0], "horizontal")) {
        p->gen->orientation = ORIENT_LANDSCAPE;
    } else if (sv_equals(args[0], "portrait") || sv_equals(args[0], "vertical")) {
        p->gen->orientation = ORIENT_PORTRAIT;
    }
    sync_page_dimensions_to_orientation(p->gen);
}

static void builtin_margins(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    p->gen->margin_top    = measure_parse(p, args[0], UNIT_MM);
    p->gen->margin_bottom = measure_parse(p, args[1], UNIT_MM);
    p->gen->margin_left   = measure_parse(p, args[2], UNIT_MM);
    p->gen->margin_right  = measure_parse(p, args[3], UNIT_MM);
}

static void builtin_newpage(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    if (p->gen->inline_depth > 0) return;
    if (p->gen->page_count < MAX_PAGES) {
        p->gen->page_count++;
    }
    counter_step(p->env, (StrView){"page", 4});
    xml_emit_page_break(p->gen);
}

static void builtin_newframe(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    if (p->gen->frame_count >= MAX_FRAMES) return;

    size_t idx = p->gen->frame_count++;
    p->gen->frames[idx] = (LayoutFrame){
        .page_num = p->gen->page_count - 1,
        .x        = measure_parse(p, args[0], UNIT_MM),
        .y        = measure_parse(p, args[1], UNIT_MM),
        .width    = measure_parse(p, args[2], UNIT_MM),
        .height   = measure_parse(p, args[3], UNIT_MM)
    };
}

static void builtin_twocolumn(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    xml_close_para(p->gen);
    double usable_w = p->gen->page_width.mm - p->gen->margin_left.mm - p->gen->margin_right.mm;
    double usable_h = p->gen->page_height.mm - p->gen->margin_top.mm - p->gen->margin_bottom.mm;
    double gap = 6.0;
    double col_w = (usable_w - gap) * 0.5;

    if (p->gen->frame_count + 2 <= MAX_FRAMES) {
        p->gen->frames[p->gen->frame_count++] = (LayoutFrame){
            .page_num = p->gen->page_count - 1,
            .x        = p->gen->margin_left,
            .y        = p->gen->margin_top,
            .width    = measure_make(col_w, UNIT_MM, 1.0),
            .height   = measure_make(usable_h, UNIT_MM, 1.0)
        };
        p->gen->frames[p->gen->frame_count++] = (LayoutFrame){
            .page_num = p->gen->page_count - 1,
            .x        = measure_make(p->gen->margin_left.mm + col_w + gap, UNIT_MM, 1.0),
            .y        = p->gen->margin_top,
            .width    = measure_make(col_w, UNIT_MM, 1.0),
            .height   = measure_make(usable_h, UNIT_MM, 1.0)
        };
    }
}

static void builtin_align(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    if (sv_equals(args[0], "left"))         p->gen->para_state.align = ALIGN_LEFT;
    else if (sv_equals(args[0], "center"))  p->gen->para_state.align = ALIGN_CENTER;
    else if (sv_equals(args[0], "right"))   p->gen->para_state.align = ALIGN_RIGHT;
    else if (sv_equals(args[0], "justify")) p->gen->para_state.align = ALIGN_JUSTIFY;
}

static void builtin_centering(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.align        = ALIGN_CENTER;
    p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
}

static void builtin_raggedright(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.align        = ALIGN_LEFT;
    p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
}

static void builtin_raggedleft(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.align        = ALIGN_RIGHT;
    p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
}

static void builtin_noindent(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->next_para_noindent = true;
}

static void builtin_indent(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->next_para_noindent = false;
}

static void builtin_linespacing(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.line_spacing = measure_parse(p, args[0], UNIT_MM);
}

static void builtin_parmargins(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.indent_left  = measure_parse(p, args[0], UNIT_MM);
    p->gen->para_state.indent_right = measure_parse(p, args[1], UNIT_MM);
}

static void builtin_parspacing(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.space_before = measure_parse(p, args[0], UNIT_MM);
    p->gen->para_state.space_after  = measure_parse(p, args[1], UNIT_MM);
}

static void builtin_par(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->cmd_space_before = false;
    if (p->gen->inline_depth > 0) return;
    xml_close_para(p->gen);
}

static bool sv_contains_cstr(StrView sv, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || sv.len < nlen) return false;
    for (size_t i = 0; i + nlen <= sv.len; i++) {
        if (memcmp(sv.data + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void builtin_linebreak_cmd(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, [[maybe_unused]] StrView args[]) {
    Measure extra = (opt_c > 0) ? measure_parse(p, opt[0], UNIT_MM) : measure_make(0.0, UNIT_MM, 1.0);
    if (p->gen->in_tabular) {
        xml_table_end_row(p->gen, extra);
    } else {
        xml_emit_line_break(p->gen, extra);
    }
    p->cmd_space_before = false;
}

static void builtin_linebreak_opt(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, [[maybe_unused]] StrView args[]) {
    long priority = (opt_c > 0) ? sv_to_long(p, opt[0]) : 4;
    if (priority >= 3) {
        xml_emit_line_break(p->gen, measure_make(0.0, UNIT_MM, 1.0));
    }
    p->cmd_space_before = false;
}

static void builtin_pagebreak_opt(Parser *p, bool starred, StrView opt[], size_t opt_c, StrView args[]) {
    long priority = (opt_c > 0) ? sv_to_long(p, opt[0]) : 4;
    if (priority >= 3) {
        builtin_newpage(p, starred, opt, opt_c, args);
    }
    p->cmd_space_before = false;
}

static void builtin_enlargethispage(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    Measure delta = measure_parse(p, args[0], UNIT_MM);
    double next_bot = fmax(5.0, p->gen->margin_bottom.mm - delta.mm);
    p->gen->margin_bottom = measure_make(next_bot, UNIT_MM, 1.0);
}

static void builtin_hspace(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    if (sv_contains_cstr(args[0], "\\fill") || sv_contains_cstr(args[0], "\\stretch")) {
        xml_emit_hspace(p->gen, measure_make(0.0, UNIT_MM, 1.0), "space", p->cmd_space_before);
    } else {
        Measure w = measure_parse(p, args[0], UNIT_MM);
        xml_emit_hspace(p->gen, w, nullptr, p->cmd_space_before);
    }
    p->cmd_space_before = false;
}

static void builtin_hfill(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.0, UNIT_MM, 1.0), "space", p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_dotfill(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.0, UNIT_MM, 1.0), "dots", p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_hrulefill(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.0, UNIT_MM, 1.0), "rule", p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_quad(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(1.0, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_qquad(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(2.0, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_enspace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.5, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_thinspace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.16667, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_medspace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.22222, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_thickspace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(0.27778, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_negthinspace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_hspace(p->gen, measure_make(-0.16667, UNIT_EM, p->gen->char_state.size.mm), nullptr, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_vspace(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    p->cmd_space_before = false;
    if (sv_contains_cstr(args[0], "\\fill") || sv_contains_cstr(args[0], "\\stretch")) {
        xml_emit_vspace(p->gen, measure_make(0.0, UNIT_MM, 1.0), starred, true);
    } else {
        Measure h = measure_parse(p, args[0], UNIT_MM);
        xml_emit_vspace(p->gen, h, starred, false);
    }
}

static void builtin_vfill(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->cmd_space_before = false;
    xml_emit_vspace(p->gen, measure_make(0.0, UNIT_MM, 1.0), false, true);
}

static void builtin_smallskip(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->cmd_space_before = false;
    xml_emit_vspace(p->gen, measure_make(3.0, UNIT_PT, 1.0), false, false);
}

static void builtin_medskip(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->cmd_space_before = false;
    xml_emit_vspace(p->gen, measure_make(6.0, UNIT_PT, 1.0), false, false);
}

static void builtin_bigskip(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->cmd_space_before = false;
    xml_emit_vspace(p->gen, measure_make(12.0, UNIT_PT, 1.0), false, false);
}

// --- Fuentes y Estilos Tipográficos ---

static void apply_nfss_family(XmlGenerator *gen, StrView fam) {
    fam = sv_trim(fam);
    if (sv_equals(fam, "cmr") || sv_equals(fam, "lmr") || sv_equals(fam, "ptm") || sv_equals(fam, "rm")) {
        snprintf(gen->char_state.font_family, sizeof(gen->char_state.font_family), "%s", DEFAULT_FONT_FAMILY);
    } else if (sv_equals(fam, "cmss") || sv_equals(fam, "lmss") || sv_equals(fam, "phv") || sv_equals(fam, "sf")) {
        snprintf(gen->char_state.font_family, sizeof(gen->char_state.font_family), "%s", DEFAULT_SANS_FAMILY);
    } else if (sv_equals(fam, "cmtt") || sv_equals(fam, "lmtt") || sv_equals(fam, "pcr") || sv_equals(fam, "tt")) {
        snprintf(gen->char_state.font_family, sizeof(gen->char_state.font_family), "%s", DEFAULT_MONO_FAMILY);
    } else {
        snprintf(gen->char_state.font_family, sizeof(gen->char_state.font_family),
                 "%.*s", (int)fam.len, fam.data);
    }
}

static void apply_nfss_series(XmlGenerator *gen, StrView ser) {
    ser = sv_trim(ser);
    if (sv_equals(ser, "b") || sv_equals(ser, "bx") || sv_equals(ser, "sbc") || sv_equals(ser, "bold")) {
        gen->char_state.weight = WEIGHT_BOLD;
    } else {
        gen->char_state.weight = WEIGHT_NORMAL;
    }
}

static void apply_nfss_shape(XmlGenerator *gen, StrView shp) {
    shp = sv_trim(shp);
    if (sv_equals(shp, "it") || sv_equals(shp, "sl") || sv_equals(shp, "italic")) {
        gen->char_state.slant      = SLANT_ITALIC;
        gen->char_state.small_caps = false;
    } else if (sv_equals(shp, "sc")) {
        gen->char_state.slant      = SLANT_NORMAL;
        gen->char_state.small_caps = true;
    } else {
        gen->char_state.slant      = SLANT_NORMAL;
        gen->char_state.small_caps = false;
    }
}

static void builtin_font(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    apply_nfss_family(p->gen, args[0]);
}

static void builtin_fontfamily(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    apply_nfss_family(p->gen, args[0]);
}

static void builtin_fontseries(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    apply_nfss_series(p->gen, args[0]);
}

static void builtin_fontshape(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    apply_nfss_shape(p->gen, args[0]);
}

static void builtin_fontsize(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    p->gen->char_state.size = measure_parse(p, args[0], UNIT_PT);
    // En NFSS de LaTeX2e, \fontsize{size}{skip} recibe un segundo argumento {baselineskip}
    if (p->current.type == TOKEN_LBRACE) {
        StrView skip_arg = {0};
        if (parse_arg(p, &skip_arg)) {
            p->gen->pending_nfss_baselineskip     = measure_parse(p, skip_arg, UNIT_PT);
            p->gen->has_pending_nfss_baselineskip = true;
        }
    }
}

static void builtin_selectfont(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    if (p->gen->has_pending_nfss_baselineskip) {
        if (!p->gen->para_open || !p->gen->para_has_content) {
            double spread = (p->gen->linespread_factor > 0.1) ? p->gen->linespread_factor : 1.0;
            p->gen->para_state.line_spacing = measure_make(p->gen->pending_nfss_baselineskip.mm * spread, UNIT_MM, 1.0);
        }
        p->gen->has_pending_nfss_baselineskip = false;
    }
}

static void builtin_usefont(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    (void)args[0]; // encoding
    apply_nfss_family(p->gen, args[1]);
    apply_nfss_series(p->gen, args[2]);
    apply_nfss_shape(p->gen, args[3]);
}

static void builtin_linespread(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    char buf[32];
    size_t n = (args[0].len < sizeof(buf) - 1) ? args[0].len : (sizeof(buf) - 1);
    memcpy(buf, args[0].data, n);
    buf[n] = '\0';
    double f = strtod(buf, nullptr);
    if (f > 0.1) p->gen->linespread_factor = f;
}

static void builtin_fontcolor(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    snprintf(p->gen->char_state.color, sizeof(p->gen->char_state.color),
             "%.*s", (int)args[0].len, args[0].data);
}

static void builtin_rmfamily(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_FONT_FAMILY);
}

static void builtin_sffamily(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_SANS_FAMILY);
}

static void builtin_ttfamily(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_MONO_FAMILY);
}

static void builtin_bfseries(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->char_state.weight = WEIGHT_BOLD;
}

static void builtin_mdseries(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->char_state.weight = WEIGHT_NORMAL;
}

static void builtin_itshape(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->char_state.slant      = SLANT_ITALIC;
    p->gen->char_state.small_caps = false;
}

static void builtin_scshape(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->char_state.slant      = SLANT_NORMAL;
    p->gen->char_state.small_caps = true;
}

static void builtin_upshape(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->char_state.slant      = SLANT_NORMAL;
    p->gen->char_state.small_caps = false;
}

static void builtin_normalfont(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_FONT_FAMILY);
    p->gen->char_state.weight     = WEIGHT_NORMAL;
    p->gen->char_state.slant      = SLANT_NORMAL;
    p->gen->char_state.underline  = false;
    p->gen->char_state.small_caps = false;
    p->gen->char_state.shift      = SHIFT_NORMAL;
}

static void builtin_em(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    p->gen->char_state.slant = (p->gen->char_state.slant == SLANT_ITALIC) ? SLANT_NORMAL : SLANT_ITALIC;
}

static void parse_inline_scoped(Parser *p, StrView fragment, CharState modified_state) {
    if (!p->gen->capture_plain_buf) {
        xml_ensure_para_open(p->gen);

        if (p->cmd_space_before && p->gen->para_has_content) {
            xml_activate_current_char_style(p->gen);
            sb_append_cstr(&p->gen->story, " ");
            p->cmd_space_before = false;
        }
    }

    CharState saved_state = p->gen->char_state;
    p->gen->char_state    = modified_state;
    p->gen->inline_depth++;

    Parser sub = {
        .lexer       = lexer_init(fragment.data, fragment.len, p->env->allow_at),
        .env         = p->env,
        .gen         = p->gen,
        .macro_depth = p->macro_depth
    };
    parser_advance(&sub);

    if (p->cmd_space_before) {
        sub.current.space_before = true;
        p->cmd_space_before = false;
    }

    parse_and_execute(&sub);

    p->gen->inline_depth--;
    p->gen->char_state = saved_state;
}

static void builtin_textbf(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    next.weight    = WEIGHT_BOLD;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textmd(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    next.weight    = WEIGHT_NORMAL;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textit(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next  = p->gen->char_state;
    next.slant      = SLANT_ITALIC;
    next.small_caps = false;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textsc(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next  = p->gen->char_state;
    next.slant      = SLANT_NORMAL;
    next.small_caps = true;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textup(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next  = p->gen->char_state;
    next.slant      = SLANT_NORMAL;
    next.small_caps = false;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textrm(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_FONT_FAMILY);
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textsf(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_SANS_FAMILY);
    parse_inline_scoped(p, args[0], next);
}

static void builtin_texttt(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_MONO_FAMILY);
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textnormal(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_FONT_FAMILY);
    next.weight     = WEIGHT_NORMAL;
    next.slant      = SLANT_NORMAL;
    next.small_caps = false;
    next.underline  = false;
    next.shift      = SHIFT_NORMAL;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_emph(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    next.slant     = (next.slant == SLANT_ITALIC) ? SLANT_NORMAL : SLANT_ITALIC;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_underline(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    next.underline = true;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textsuperscript(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    next.shift     = SHIFT_SUPER;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textsubscript(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    CharState next = p->gen->char_state;
    next.shift     = SHIFT_SUB;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_oldstylenums(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    parse_inline_scoped(p, args[0], p->gen->char_state);
}

// --- Fase 4: Cajas, Tablas, Flotantes, Notas al Pie y Estilos de Página ---

static void builtin_mbox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, args[0], plain, sizeof(plain), &box_cs);
    Measure zero = measure_make(0.0, UNIT_MM, 1.0);
    xml_emit_box(p->gen, zero, 'c', zero, zero, zero, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void emit_pic_box_helper(Parser *p, double pw_u, double ph_u, double border_mm, double dash_mm) {
    StrView pos_opt = {0}, body = {0};
    parse_opt_arg(p, &pos_opt);
    parse_arg(p, &body);

    if (p->gen->capture_plain_buf) return;

    double ul = get_unitlength_mm(p);
    double w_mm = pw_u * ul;
    double h_mm = ph_u * ul;
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, body, plain, sizeof(plain), &box_cs);
    size_t cs_id = xml_intern_char_style(p->gen, &box_cs);

    char bx[32], by[32], bw[32], bh[32], bb[32], bd[32], bal[8] = "c";
    if (pos_opt.len > 0) {
        StrView tr = sv_trim(pos_opt);
        snprintf(bal, sizeof(bal), "%.*s", (int)(tr.len < 6 ? tr.len : 6), tr.data);
    }
    measure_format(measure_make(p->gen->pic_cur_x_mm, UNIT_MM, 1.0), bx, sizeof(bx));
    measure_format(measure_make(p->gen->pic_cur_y_mm, UNIT_MM, 1.0), by, sizeof(by));
    measure_format(measure_make(w_mm, UNIT_MM, 1.0), bw, sizeof(bw));
    measure_format(measure_make(h_mm, UNIT_MM, 1.0), bh, sizeof(bh));
    measure_format(measure_make(border_mm, UNIT_MM, 1.0), bb, sizeof(bb));
    measure_format(measure_make(dash_mm, UNIT_MM, 1.0), bd, sizeof(bd));

    sb_appendf(&p->gen->story,
               "        <pbox x=\"%s\" y=\"%s\" width=\"%s\" height=\"%s\" border=\"%s\" dash=\"%s\" align=\"%s\" style=\"cs%zu\">",
               bx, by, bw, bh, bb, bd, bal, cs_id + 1);
    sb_append_escaped_xml(&p->gen->story, plain, strlen(plain));
    sb_append_cstr(&p->gen->story, "</pbox>\n");
}

static void builtin_makebox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    double pw = 0.0, ph = 0.0;
    if (p->gen->in_picture && parse_pic_coords(p, &pw, &ph)) {
        emit_pic_box_helper(p, pw, ph, 0.0, 0.0);
        p->cmd_space_before = false;
        return;
    }
    StrView o1 = {0}, o2 = {0}, body = {0};
    bool has_o1 = parse_opt_arg(p, &o1);
    bool has_o2 = has_o1 ? parse_opt_arg(p, &o2) : false;
    if (!parse_arg(p, &body)) return;

    Measure w = has_o1 ? measure_parse(p, o1, UNIT_MM) : measure_make(0.0, UNIT_MM, 1.0);
    char align = 'c';
    if (has_o2) {
        StrView al = sv_trim(o2);
        if (al.len > 0 && (al.data[0] == 'l' || al.data[0] == 'r' || al.data[0] == 'c' || al.data[0] == 's')) {
            align = al.data[0];
        }
    }
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, body, plain, sizeof(plain), &box_cs);
    Measure zero = measure_make(0.0, UNIT_MM, 1.0);
    xml_emit_box(p->gen, w, align, zero, zero, zero, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_fbox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    LengthDef *r_def = length_find_mut(p->env, (StrView){"fboxrule", 8});
    LengthDef *s_def = length_find_mut(p->env, (StrView){"fboxsep", 7});
    Measure border = r_def ? r_def->value : measure_make(0.4, UNIT_PT, 1.0);
    Measure pad    = s_def ? s_def->value : measure_make(3.0, UNIT_PT, 1.0);
    Measure zero   = measure_make(0.0, UNIT_MM, 1.0);

    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, args[0], plain, sizeof(plain), &box_cs);
    xml_emit_box(p->gen, zero, 'c', border, pad, zero, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_framebox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    double pw = 0.0, ph = 0.0;
    if (p->gen->in_picture && parse_pic_coords(p, &pw, &ph)) {
        emit_pic_box_helper(p, pw, ph, p->gen->pic_line_thick_mm, 0.0);
        p->cmd_space_before = false;
        return;
    }
    StrView o1 = {0}, o2 = {0}, body = {0};
    bool has_o1 = parse_opt_arg(p, &o1);
    bool has_o2 = has_o1 ? parse_opt_arg(p, &o2) : false;
    if (!parse_arg(p, &body)) return;

    LengthDef *r_def = length_find_mut(p->env, (StrView){"fboxrule", 8});
    LengthDef *s_def = length_find_mut(p->env, (StrView){"fboxsep", 7});
    Measure border = r_def ? r_def->value : measure_make(0.4, UNIT_PT, 1.0);
    Measure pad    = s_def ? s_def->value : measure_make(3.0, UNIT_PT, 1.0);
    Measure w      = has_o1 ? measure_parse(p, o1, UNIT_MM) : measure_make(0.0, UNIT_MM, 1.0);
    char align     = 'c';
    if (has_o2) {
        StrView al = sv_trim(o2);
        if (al.len > 0 && (al.data[0] == 'l' || al.data[0] == 'r' || al.data[0] == 'c' || al.data[0] == 's')) {
            align = al.data[0];
        }
    }
    Measure zero = measure_make(0.0, UNIT_MM, 1.0);
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, body, plain, sizeof(plain), &box_cs);
    xml_emit_box(p->gen, w, align, border, pad, zero, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_raisebox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView lift_sv = {0}, text_sv = {0}, dummy_opt = {0};
    if (!parse_arg(p, &lift_sv)) return;
    while (parse_opt_arg(p, &dummy_opt)) {}
    if (!parse_arg(p, &text_sv)) return;

    Measure raise = measure_parse(p, lift_sv, UNIT_MM);
    Measure zero  = measure_make(0.0, UNIT_MM, 1.0);
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, text_sv, plain, sizeof(plain), &box_cs);
    xml_emit_box(p->gen, zero, 'c', zero, zero, raise, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_parbox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    Measure w    = measure_parse(p, args[0], UNIT_MM);
    Measure zero = measure_make(0.0, UNIT_MM, 1.0);
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, args[1], plain, sizeof(plain), &box_cs);
    xml_emit_box(p->gen, w, 'l', zero, zero, zero, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_newsavebox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    savebox_register(p->env, name);
}

static void builtin_sbox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, body = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    if (!parse_arg(p, &body)) return;

    SaveBoxDef *sb = savebox_register(p->env, name);
    if (!sb) return;
    free(sb->content);
    StrBuf b;
    if (sb_init(&b, body.len + 16)) {
        sb_appendf(&b, "\\mbox{%.*s}", (int)body.len, body.data);
        sb->content = b.data;
    }
}

static void builtin_savebox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0}, opt1 = {0}, opt2 = {0}, body = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    bool has_o1 = parse_opt_arg(p, &opt1);
    bool has_o2 = has_o1 ? parse_opt_arg(p, &opt2) : false;
    if (!parse_arg(p, &body)) return;

    SaveBoxDef *sb = savebox_register(p->env, name);
    if (!sb) return;
    free(sb->content);
    StrBuf b;
    if (sb_init(&b, body.len + 64)) {
        if (has_o1 && has_o2) {
            sb_appendf(&b, "\\makebox[%.*s][%.*s]{%.*s}",
                       (int)opt1.len, opt1.data, (int)opt2.len, opt2.data, (int)body.len, body.data);
        } else if (has_o1) {
            sb_appendf(&b, "\\makebox[%.*s]{%.*s}",
                       (int)opt1.len, opt1.data, (int)body.len, body.data);
        } else {
            sb_appendf(&b, "\\mbox{%.*s}", (int)body.len, body.data);
        }
        sb->content = b.data;
    }
}

static void builtin_usebox(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, [[maybe_unused]] StrView args[]) {
    StrView name = {0};
    if (!parse_cmd_or_arg(p, &name)) return;
    SaveBoxDef *sb = savebox_find_mut(p->env, name);
    if (sb && sb->content) {
        execute_tex_fragment(p, (StrView){ sb->content, strlen(sb->content) }, p->cmd_space_before);
    }
    p->cmd_space_before = false;
}

static void builtin_hline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->cmd_space_before = false;
    if (!p->gen->in_tabular) return;
    if (p->gen->tab_row_open && (p->gen->para_has_content || p->gen->tab_cur_col > 0)) {
        xml_table_end_row(p->gen, measure_make(0.0, UNIT_MM, 1.0));
    }
    sb_append_cstr(&p->gen->story, "        <hline/>\n");
}

static void builtin_cline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    p->cmd_space_before = false;
    if (!p->gen->in_tabular) return;
    StrView r = sv_trim(args[0]);
    const char *dash = memchr(r.data, '-', r.len);
    int c_from = 1, c_to = 1;
    if (dash) {
        c_from = (int)sv_to_long(p, (StrView){ r.data, (size_t)(dash - r.data) });
        c_to   = (int)sv_to_long(p, (StrView){ dash + 1, (size_t)((r.data + r.len) - (dash + 1)) });
    } else {
        c_from = c_to = (int)sv_to_long(p, r);
    }
    if (c_from < 1) c_from = 1;
    if (c_to < c_from) c_to = c_from;
    sb_appendf(&p->gen->story, "        <cline from=\"%d\" to=\"%d\"/>\n", c_from, c_to);
}

static void builtin_multicolumn(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    long span_val = sv_to_long(p, args[0]);
    size_t span   = (span_val >= 1) ? (size_t)span_val : 1;
    StrView spec  = sv_trim(args[1]);
    int bl = 0, br = 0;
    char align = 'c';
    bool seen_col = false;
    for (size_t i = 0; i < spec.len; i++) {
        char c = spec.data[i];
        if (c == '|') {
            if (!seen_col) bl++;
            else br++;
        } else if (c == 'l' || c == 'c' || c == 'r') {
            align    = c;
            seen_col = true;
        }
    }
    if (p->gen->in_tabular) {
        p->gen->tab_has_mc_override = true;
        p->gen->tab_mc_span         = span;
        p->gen->tab_mc_align        = align;
        p->gen->tab_mc_bl           = bl;
        p->gen->tab_mc_br           = br;
        xml_ensure_para_open(p->gen);
    }
    execute_tex_fragment(p, args[2], false);
    p->cmd_space_before = false;
}

static void builtin_vline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_emit_rule(p->gen, measure_make(0.4, UNIT_PT, 1.0), measure_make(3.8, UNIT_MM, 1.0), measure_make(-0.8, UNIT_MM, 1.0), p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_caption(Parser *p, bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    xml_close_para(p->gen);
    bool is_tbl = (strcmp(p->gen->active_float_kind, "table") == 0);
    const char *cname = is_tbl ? "table" : "figure";
    const char *mname = is_tbl ? "\\tablename" : "\\figurename";

    StrBuf cbuf;
    if (!sb_init(&cbuf, args[0].len + 128)) return;
    if (starred) {
        sb_appendf(&cbuf, "\\par{\\centering\\small %.*s\\par}", (int)args[0].len, args[0].data);
    } else {
        counter_step(p->env, (StrView){ cname, strlen(cname) });
        format_the_counter(p->env, p->gen, (StrView){ cname, strlen(cname) }, p->env->current_ref, sizeof(p->env->current_ref));

        char plain_cap[256];
        eval_tex_to_plain(p, args[0], plain_cap, sizeof(plain_cap));
        CounterDef *pc = counter_find_mut(p->env, (StrView){"page", 4});
        int pnum = pc ? pc->value : (int)p->gen->page_count;
        char toc_anchor[64];
        toc_record_entry(p->env, is_tbl ? "lot" : "lof", cname, p->env->current_ref, plain_cap, pnum, toc_anchor, sizeof(toc_anchor));
        xml_emit_anchor(p->gen, toc_anchor);

        sb_appendf(&cbuf, "\\par{\\centering\\small\\textbf{%s~\\the%s:} %.*s\\par}",
                   mname, cname, (int)args[0].len, args[0].data);
    }
    execute_tex_fragment(p, (StrView){ cbuf.data, cbuf.len }, false);
    sb_free(&cbuf);
    p->cmd_space_before = false;
}

static void emit_footnote_body(Parser *p, const char *lbl, StrView body_tex) {
    char plain[512];
    eval_tex_to_plain(p, body_tex, plain, sizeof(plain));

    CharState fn_cs = p->gen->char_state;
    snprintf(fn_cs.font_family, sizeof(fn_cs.font_family), "%s", DEFAULT_FONT_FAMILY);
    fn_cs.weight     = WEIGHT_NORMAL;
    fn_cs.slant      = SLANT_NORMAL;
    fn_cs.size       = measure_make(8.5, UNIT_PT, 1.0);
    fn_cs.underline  = false;
    fn_cs.small_caps = false;
    fn_cs.shift      = SHIFT_NORMAL;
    snprintf(fn_cs.color, sizeof(fn_cs.color), "Black");
    size_t cs_id = xml_intern_char_style(p->gen, &fn_cs);

    StrBuf *dest = p->gen->para_open ? &p->gen->pending_footnotes : &p->gen->story;
    sb_appendf(dest, "      <footnote label=\"");
    sb_append_escaped_xml(dest, lbl, strlen(lbl));
    sb_appendf(dest, "\" style=\"cs%zu\">", cs_id + 1);
    sb_append_escaped_xml(dest, plain, strlen(plain));
    sb_append_cstr(dest, "</footnote>\n");
}

static void builtin_footnote(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, StrView args[]) {
    char lbl[32];
    if (opt_c > 0) {
        snprintf(lbl, sizeof(lbl), "%.*s", (int)opt[0].len, opt[0].data);
    } else {
        counter_step(p->env, (StrView){"footnote", 8});
        format_the_counter(p->env, p->gen, (StrView){"footnote", 8}, lbl, sizeof(lbl));
    }

    CharState saved = p->gen->char_state;
    p->gen->char_state.shift = SHIFT_SUPER;
    xml_emit_text(p->gen, (StrView){ lbl, strlen(lbl) }, false);
    p->gen->char_state = saved;

    emit_footnote_body(p, lbl, args[0]);
    p->cmd_space_before = false;
}

static void builtin_footnotemark(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, [[maybe_unused]] StrView args[]) {
    char lbl[32];
    if (opt_c > 0) {
        snprintf(lbl, sizeof(lbl), "%.*s", (int)opt[0].len, opt[0].data);
    } else {
        counter_step(p->env, (StrView){"footnote", 8});
        format_the_counter(p->env, p->gen, (StrView){"footnote", 8}, lbl, sizeof(lbl));
    }
    CharState saved = p->gen->char_state;
    p->gen->char_state.shift = SHIFT_SUPER;
    xml_emit_text(p->gen, (StrView){ lbl, strlen(lbl) }, false);
    p->gen->char_state = saved;
    p->cmd_space_before = false;
}

static void builtin_footnotetext(Parser *p, [[maybe_unused]] bool starred, StrView opt[], size_t opt_c, StrView args[]) {
    char lbl[32];
    if (opt_c > 0) {
        snprintf(lbl, sizeof(lbl), "%.*s", (int)opt[0].len, opt[0].data);
    } else {
        format_the_counter(p->env, p->gen, (StrView){"footnote", 8}, lbl, sizeof(lbl));
    }
    emit_footnote_body(p, lbl, args[0]);
    p->cmd_space_before = false;
}

static void builtin_marginpar(Parser *p, [[maybe_unused]] bool starred, [[maybe_unused]] StrView opt[], [[maybe_unused]] size_t opt_c, StrView args[]) {
    char plain[256];
    eval_tex_to_plain(p, args[0], plain, sizeof(plain));

    CharState mp_cs = p->gen->char_state;
    snprintf(mp_cs.font_family, sizeof(mp_cs.font_family), "%s", DEFAULT_SANS_FAMILY);
    mp_cs.weight     = WEIGHT_NORMAL;
    mp_cs.slant      = SLANT_ITALIC;
    mp_cs.size       = measure_make(8.0, UNIT_PT, 1.0);
    mp_cs.underline  = false;
    mp_cs.small_caps = false;
    mp_cs.shift      = SHIFT_NORMAL;
    snprintf(mp_cs.color, sizeof(mp_cs.color), "DarkSlateGray");
    size_t cs_id = xml_intern_char_style(p->gen, &mp_cs);

    StrBuf *dest = p->gen->para_open ? &p->gen->pending_footnotes : &p->gen->story;
    sb_appendf(dest, "      <marginpar style=\"cs%zu\">", cs_id + 1);
    sb_append_escaped_xml(dest, plain, strlen(plain));
    sb_append_cstr(dest, "</marginpar>\n");
    p->cmd_space_before = false;
}

static void emit_page_style_update(XmlGenerator *gen) {
    if (gen->capture_plain_buf) return;
    StrBuf *dest = gen->para_open ? &gen->pending_footnotes : &gen->story;
    sb_append_cstr(dest, "      <page-style mode=\"");
    sb_append_escaped_xml(dest, gen->page_style[0] ? gen->page_style : "plain", strlen(gen->page_style[0] ? gen->page_style : "plain"));
    sb_append_cstr(dest, "\" numbering=\"");
    sb_append_escaped_xml(dest, gen->page_numbering[0] ? gen->page_numbering : "arabic", strlen(gen->page_numbering[0] ? gen->page_numbering : "arabic"));
    sb_append_cstr(dest, "\" left-mark=\"");
    sb_append_escaped_xml(dest, gen->left_mark, strlen(gen->left_mark));
    sb_append_cstr(dest, "\" right-mark=\"");
    sb_append_escaped_xml(dest, gen->right_mark, strlen(gen->right_mark));
    sb_append_cstr(dest, "\"/>\n");
}

static void builtin_pagestyle(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrView st = sv_trim(args[0]);
    snprintf(p->gen->page_style, sizeof(p->gen->page_style), "%.*s", (int)st.len, st.data);
    emit_page_style_update(p->gen);
}

static void builtin_thispagestyle(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrView st = sv_trim(args[0]);
    snprintf(p->gen->this_page_style, sizeof(p->gen->this_page_style), "%.*s", (int)st.len, st.data);
}

static void builtin_pagenumbering(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    StrView num = sv_trim(args[0]);
    snprintf(p->gen->page_numbering, sizeof(p->gen->page_numbering), "%.*s", (int)num.len, num.data);
    CounterDef *pc = counter_find_mut(p->env, (StrView){"page", 4});
    if (pc) pc->value = 1;
    emit_page_style_update(p->gen);
}

static void builtin_markboth(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    eval_tex_to_plain(p, args[0], p->gen->left_mark, sizeof(p->gen->left_mark));
    eval_tex_to_plain(p, args[1], p->gen->right_mark, sizeof(p->gen->right_mark));
    emit_page_style_update(p->gen);
}

static void builtin_markright(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    eval_tex_to_plain(p, args[0], p->gen->right_mark, sizeof(p->gen->right_mark));
    emit_page_style_update(p->gen);
}

// --- Fase 5: Modo Matemático, Referencias Cruzadas, TOC/LOF/LOT, Bibliografía, Índice y Picture ---

static void builtin_math_open_inline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_push_group(p->gen);
    p->gen->in_math          = true;
    p->gen->char_state.slant = SLANT_ITALIC;
    if (p->gen->math_bold) p->gen->char_state.weight = WEIGHT_BOLD;
}

static void builtin_math_close_inline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->in_math      = false;
    p->cmd_space_before  = false;
    xml_pop_group(p->gen);
}

static void builtin_math_open_display(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_close_para(p->gen);
    xml_push_group(p->gen);
    p->gen->para_state.align        = ALIGN_CENTER;
    p->gen->para_state.indent_first = measure_make(0.0, UNIT_MM, 1.0);
    p->gen->para_state.space_before = measure_make(2.2, UNIT_MM, 1.0);
    p->gen->para_state.space_after  = measure_make(2.2, UNIT_MM, 1.0);
    p->gen->in_math                 = true;
    p->gen->in_display_math         = true;
    p->gen->eq_suppress_num         = true;
    p->gen->char_state.slant        = SLANT_ITALIC;
    p->cmd_space_before             = false;
}

static void builtin_math_close_display(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    xml_close_para(p->gen);
    p->gen->in_math         = false;
    p->gen->in_display_math = false;
    p->gen->eq_suppress_num = false;
    p->cmd_space_before     = false;
    xml_pop_group(p->gen);
}

static void builtin_ensuremath(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    if (p->gen->in_math) {
        execute_tex_fragment(p, args[0], p->cmd_space_before);
    } else {
        CharState next = p->gen->char_state;
        next.slant = SLANT_ITALIC;
        p->gen->in_math = true;
        parse_inline_scoped(p, args[0], next);
        p->gen->in_math = false;
    }
    p->cmd_space_before = false;
}

static void builtin_nonumber(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->eq_suppress_num = true;
    p->cmd_space_before     = false;
}

static void builtin_frac(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char num[256], den[256];
    eval_tex_to_plain(p, args[0], num, sizeof(num));
    eval_tex_to_plain(p, args[1], den, sizeof(den));
    xml_emit_mfrac(p->gen, (StrView){ num, strlen(num) }, (StrView){ den, strlen(den) },
                   true, false, &p->gen->char_state, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_binom(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char num[256], den[256];
    eval_tex_to_plain(p, args[0], num, sizeof(num));
    eval_tex_to_plain(p, args[1], den, sizeof(den));
    xml_emit_mfrac(p->gen, (StrView){ num, strlen(num) }, (StrView){ den, strlen(den) },
                   false, true, &p->gen->char_state, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_stackrel(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char top[256], bot[256];
    eval_tex_to_plain(p, args[0], top, sizeof(top));
    eval_tex_to_plain(p, args[1], bot, sizeof(bot));
    xml_emit_mfrac(p->gen, (StrView){ top, strlen(top) }, (StrView){ bot, strlen(bot) },
                   false, false, &p->gen->char_state, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_sqrt(Parser *p, [[maybe_unused]] bool s, StrView opt[], size_t opt_c, StrView args[]) {
    char idx[64] = {0}, body[256] = {0};
    if (opt_c > 0) eval_tex_to_plain(p, opt[0], idx, sizeof(idx));
    eval_tex_to_plain(p, args[0], body, sizeof(body));
    xml_emit_msqrt(p->gen, (StrView){ idx, strlen(idx) }, (StrView){ body, strlen(body) },
                   &p->gen->char_state, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void emit_math_mover_helper(Parser *p, const char *kind, StrView arg) {
    char body[256];
    eval_tex_to_plain(p, arg, body, sizeof(body));
    xml_emit_mover(p->gen, kind, (StrView){ body, strlen(body) }, &p->gen->char_state, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_overline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "line", args[0]); }
static void builtin_bar(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "line", args[0]); }
static void builtin_widehat(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "hat", args[0]); }
static void builtin_hat(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "hat", args[0]); }
static void builtin_widetilde(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "tilde", args[0]); }
static void builtin_tilde(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "tilde", args[0]); }
static void builtin_overbrace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "brace", args[0]); }
static void builtin_underbrace(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "ubrace", args[0]); }
static void builtin_vec(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "vec", args[0]); }
static void builtin_dot(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "dot", args[0]); }
static void builtin_ddot(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_mover_helper(p, "ddot", args[0]); }

static void emit_math_accent_char(Parser *p, char accent_ch, StrView arg) {
    char utf8_buf[32];
    compose_latex_accent(accent_ch, arg, utf8_buf, sizeof(utf8_buf));
    xml_emit_text(p->gen, (StrView){ utf8_buf, strlen(utf8_buf) }, p->cmd_space_before);
    p->cmd_space_before = false;
}

static void builtin_acute(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_accent_char(p, '\'', args[0]); }
static void builtin_grave(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_accent_char(p, '`', args[0]); }
static void builtin_check(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_accent_char(p, 'v', args[0]); }
static void builtin_breve(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) { emit_math_accent_char(p, 'u', args[0]); }

static void consume_and_emit_math_delim(Parser *p) {
    if (p->current.type == TOKEN_TEXT && p->current.text.len > 0) {
        char d = p->current.text.data[0];
        if (p->current.text.len == 1) {
            parser_advance(p);
        } else {
            p->current.text.data++;
            p->current.text.len--;
            p->current.col++;
        }
        if (d != '.') {
            char buf[2] = { d, '\0' };
            xml_emit_text(p->gen, (StrView){ buf, 1 }, p->cmd_space_before);
        }
    } else if (p->current.type == TOKEN_COMMAND) {
        StrView dcmd = p->current.text;
        parser_advance(p);
        const char *sym = find_latex_text_symbol(dcmd);
        if (sym) {
            xml_emit_text(p->gen, (StrView){ sym, strlen(sym) }, p->cmd_space_before);
        }
    }
    p->cmd_space_before = false;
}

static void builtin_left_right(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    consume_and_emit_math_delim(p);
}

static void builtin_bmod(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    CharState saved = p->gen->char_state;
    bool sm = p->gen->in_math;
    p->gen->in_math = false;
    p->gen->char_state.slant = SLANT_NORMAL;
    xml_emit_text(p->gen, (StrView){" mod ", 5}, false);
    p->gen->char_state = saved;
    p->gen->in_math = sm;
    p->cmd_space_before = false;
}

static void builtin_pmod(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char plain[128], buf[160];
    eval_tex_to_plain(p, args[0], plain, sizeof(plain));
    snprintf(buf, sizeof(buf), " (mod %s)", plain);
    CharState saved = p->gen->char_state;
    bool sm = p->gen->in_math;
    p->gen->in_math = false;
    p->gen->char_state.slant = SLANT_NORMAL;
    xml_emit_text(p->gen, (StrView){ buf, strlen(buf) }, false);
    p->gen->char_state = saved;
    p->gen->in_math = sm;
    p->cmd_space_before = false;
}

static void builtin_mathrm(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_FONT_FAMILY);
    next.slant  = SLANT_NORMAL;
    next.weight = WEIGHT_NORMAL;
    bool sm = p->gen->in_math;
    p->gen->in_math = false;
    parse_inline_scoped(p, args[0], next);
    p->gen->in_math = sm;
}

static void builtin_mathbf(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CharState next = p->gen->char_state;
    next.weight = WEIGHT_BOLD;
    next.slant  = SLANT_NORMAL;
    bool sm = p->gen->in_math;
    p->gen->in_math = false;
    parse_inline_scoped(p, args[0], next);
    p->gen->in_math = sm;
}

static void builtin_mathit(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CharState next = p->gen->char_state;
    next.slant = SLANT_ITALIC;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_mathsf(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_SANS_FAMILY);
    next.slant = SLANT_NORMAL;
    bool sm = p->gen->in_math;
    p->gen->in_math = false;
    parse_inline_scoped(p, args[0], next);
    p->gen->in_math = sm;
}

static void builtin_mathtt(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CharState next = p->gen->char_state;
    snprintf(next.font_family, sizeof(next.font_family), "%s", DEFAULT_MONO_FAMILY);
    next.slant = SLANT_NORMAL;
    bool sm = p->gen->in_math;
    p->gen->in_math = false;
    parse_inline_scoped(p, args[0], next);
    p->gen->in_math = sm;
}

static void builtin_mathcal(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CharState next = p->gen->char_state;
    next.slant  = SLANT_ITALIC;
    next.weight = WEIGHT_BOLD;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_boldmath(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->math_bold         = true;
    p->gen->char_state.weight = WEIGHT_BOLD;
}

static void builtin_unboldmath(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->math_bold         = false;
    p->gen->char_state.weight = WEIGHT_NORMAL;
}

// --- Hiperenlaces (\href, \url, \nolinkurl, \hyperref, \hypertarget, \hyperlink) y Referencias Cruzadas ---

static void clean_url_str(StrView raw_url, char *out, size_t out_sz) {
    raw_url = sv_trim(raw_url);
    size_t pos = 0;
    for (size_t i = 0; i < raw_url.len && pos + 1 < out_sz; i++) {
        if (raw_url.data[i] == '\\' && i + 1 < raw_url.len) {
            char nx = raw_url.data[i + 1];
            if (nx == '%' || nx == '#' || nx == '_' || nx == '&' || nx == '~') {
                out[pos++] = nx;
                i++;
                continue;
            }
        }
        out[pos++] = raw_url.data[i];
    }
    out[pos] = '\0';
}

static void parse_inline_with_href(Parser *p, const char *href, StrView fragment, CharState modified_state) {
    char saved_href[256];
    snprintf(saved_href, sizeof(saved_href), "%s", p->gen->current_href);
    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", href ? href : "");
    parse_inline_scoped(p, fragment, modified_state);
    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", saved_href);
}

static void builtin_href(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char raw_u[256], final_href[256];
    clean_url_str(args[0], raw_u, sizeof(raw_u));
    if (raw_u[0] == '#' && strncmp(raw_u, "#page=", 6) != 0 && strncmp(raw_u, "#dest-", 6) != 0) {
        make_anchor_id("#dest-", (StrView){ raw_u + 1, strlen(raw_u + 1) }, final_href, sizeof(final_href));
    } else {
        snprintf(final_href, sizeof(final_href), "%s", raw_u);
    }

    CharState next = p->gen->char_state;
    if (strcmp(next.color, "Black") == 0) {
        snprintf(next.color, sizeof(next.color), "RoyalBlue");
    }
    parse_inline_with_href(p, final_href, args[1], next);
}

static void builtin_url(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char url_buf[256];
    clean_url_str(args[0], url_buf, sizeof(url_buf));

    char saved_href[256];
    snprintf(saved_href, sizeof(saved_href), "%s", p->gen->current_href);
    CharState saved_cs = p->gen->char_state;

    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", url_buf);
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_MONO_FAMILY);
    p->gen->char_state.slant = SLANT_NORMAL;
    if (strcmp(p->gen->char_state.color, "Black") == 0) {
        snprintf(p->gen->char_state.color, sizeof(p->gen->char_state.color), "RoyalBlue");
    }

    xml_emit_text(p->gen, (StrView){ url_buf, strlen(url_buf) }, p->cmd_space_before);

    p->gen->char_state = saved_cs;
    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", saved_href);
    p->cmd_space_before = false;
}

static void builtin_nolinkurl(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char url_buf[256];
    clean_url_str(args[0], url_buf, sizeof(url_buf));
    CharState saved_cs = p->gen->char_state;
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_MONO_FAMILY);
    p->gen->char_state.slant = SLANT_NORMAL;
    xml_emit_text(p->gen, (StrView){ url_buf, strlen(url_buf) }, p->cmd_space_before);
    p->gen->char_state = saved_cs;
    p->cmd_space_before = false;
}

static void builtin_hyperref(Parser *p, [[maybe_unused]] bool s, StrView opt[], size_t opt_c, StrView args[]) {
    char href[256] = {0};
    if (opt_c > 0 && opt[0].len > 0) {
        make_anchor_id("#dest-", opt[0], href, sizeof(href));
    }
    CharState next = p->gen->char_state;
    if (strcmp(next.color, "Black") == 0) {
        snprintf(next.color, sizeof(next.color), "RoyalBlue");
    }
    parse_inline_with_href(p, href, args[0], next);
}

static void builtin_hypertarget(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char aid[128];
    make_anchor_id("dest-", args[0], aid, sizeof(aid));
    xml_emit_anchor(p->gen, aid);
    parse_inline_scoped(p, args[1], p->gen->char_state);
}

static void builtin_hyperlink(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char href[256];
    StrView t = sv_trim(args[0]);
    if (t.len >= 6 && strncmp(t.data, "pdest-", 6) == 0) {
        make_anchor_id("#", t, href, sizeof(href));
    } else {
        make_anchor_id("#dest-", t, href, sizeof(href));
    }
    CharState next = p->gen->char_state;
    parse_inline_with_href(p, href, args[1], next);
}

static void builtin_label(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CounterDef *pc = counter_find_mut(p->env, (StrView){"page", 4});
    int pnum = pc ? pc->value : (int)p->gen->page_count;
    label_register(p->env, args[0], p->env->current_ref, pnum);
    char aid[128];
    make_anchor_id("dest-", args[0], aid, sizeof(aid));
    xml_emit_anchor(p->gen, aid);
    p->cmd_space_before = false;
}

static void builtin_ref(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    const LabelDef *ld = label_find(p->env, args[0]);
    const char *val = (ld && ld->ref_str[0] != '\0') ? ld->ref_str : "??";
    char href[128], saved_href[256];
    make_anchor_id("#dest-", args[0], href, sizeof(href));
    snprintf(saved_href, sizeof(saved_href), "%s", p->gen->current_href);
    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", href);
    xml_emit_text(p->gen, (StrView){ val, strlen(val) }, p->cmd_space_before);
    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", saved_href);
    p->cmd_space_before = false;
}

static void builtin_pageref(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char target[128], href[128];
    make_anchor_id("dest-", args[0], target, sizeof(target));
    make_anchor_id("#pdest-", args[0], href, sizeof(href));
    xml_emit_page_ref(p->gen, target, href, p->cmd_space_before);
    p->cmd_space_before = false;
}

// --- Índices de Contenidos (\tableofcontents, \listoffigures, \listoftables) ---

static void emit_contents_list(Parser *p, const char *kind, const char *heading_macro) {
    xml_close_para(p->gen);
    StrBuf hbuf;
    if (sb_init(&hbuf, 128)) {
        const char *sec_cmd = (strcmp(p->gen->doc_class, "article") == 0 || strcmp(p->gen->doc_class, "slides") == 0)
                              ? "\\section*" : "\\chapter*";
        sb_appendf(&hbuf, "%s{%s}", sec_cmd, heading_macro);
        execute_tex_fragment(p, (StrView){ hbuf.data, hbuf.len }, false);
        sb_free(&hbuf);
    }

    if (p->env->is_prescan) return;

    CounterDef *tocdepth = counter_find_mut(p->env, (StrView){"tocdepth", 8});
    int max_depth = tocdepth ? tocdepth->value : 3;

    for (size_t i = 0; i < p->env->toc_count; i++) {
        const TocEntryDef *te = &p->env->toc_entries[i];
        if (strcmp(te->kind, kind) != 0) continue;

        double indent_mm = 0.0;
        bool is_bold = false;
        int entry_depth = 1;
        if (strcmp(te->level, "part") == 0 || strcmp(te->level, "chapter") == 0) {
            indent_mm = 0.0;
            is_bold   = true;
            entry_depth = 0;
        } else if (strcmp(te->level, "section") == 0) {
            indent_mm = 0.0;
            is_bold   = (strcmp(p->gen->doc_class, "book") != 0 && strcmp(p->gen->doc_class, "report") != 0);
            entry_depth = 1;
        } else if (strcmp(te->level, "subsection") == 0) {
            indent_mm = 6.0;
            entry_depth = 2;
        } else if (strcmp(te->level, "subsubsection") == 0) {
            indent_mm = 12.0;
            entry_depth = 3;
        }
        if (strcmp(kind, "toc") == 0 && entry_depth > max_depth) continue;

        StrBuf lbuf;
        if (sb_init(&lbuf, 512)) {
            sb_appendf(&lbuf, "\\par{\\parmargins{%.1fmm}{0mm}\\parspacing{0.5mm}{0.5mm}\\noindent ", indent_mm);
            if (is_bold) sb_append_cstr(&lbuf, "\\bfseries ");
            if (te->number[0] != '\0') {
                sb_appendf(&lbuf, "\\hyperlink{%s}{%s}\\hspace{2.5mm}", te->anchor_id, te->number);
            }
            sb_appendf(&lbuf, "\\hyperlink{%s}{%s}\\dotfill \\pageref{%s}\\par}",
                       te->anchor_id, te->title, te->anchor_id);
            execute_tex_fragment(p, (StrView){ lbuf.data, lbuf.len }, false);
            sb_free(&lbuf);
        }
    }
    xml_emit_vspace(p->gen, measure_make(3.0, UNIT_MM, 1.0), false, false);
    p->cmd_space_before = false;
}

static void builtin_tableofcontents(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    emit_contents_list(p, "toc", "\\contentsname");
}

static void builtin_listoffigures(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    emit_contents_list(p, "lof", "\\listfigurename");
}

static void builtin_listoftables(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    emit_contents_list(p, "lot", "\\listtablename");
}

static void builtin_addcontentsline(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char kbuf[16], lbuf[32], tbuf[256], toc_anchor[64];
    StrView k = sv_trim(args[0]);
    StrView l = sv_trim(args[1]);
    snprintf(kbuf, sizeof(kbuf), "%.*s", (int)k.len, k.data);
    snprintf(lbuf, sizeof(lbuf), "%.*s", (int)l.len, l.data);
    eval_tex_to_plain(p, args[2], tbuf, sizeof(tbuf));

    CounterDef *pc = counter_find_mut(p->env, (StrView){"page", 4});
    int pnum = pc ? pc->value : (int)p->gen->page_count;
    toc_record_entry(p->env, kbuf, lbuf, "", tbuf, pnum, toc_anchor, sizeof(toc_anchor));
    xml_emit_anchor(p->gen, toc_anchor);
    p->cmd_space_before = false;
}

// --- Bibliografía (\bibitem, \cite, \nocite) e Índice Alfabético (\index, \printindex) ---

static void builtin_bibitem(Parser *p, [[maybe_unused]] bool s, StrView opt[], size_t opt_c, StrView args[]) {
    xml_close_para(p->gen);
    p->cmd_space_before = false;

    counter_step(p->env, (StrView){"enumiv", 6});
    char lbl_raw[32];
    if (opt_c > 0) {
        eval_tex_to_plain(p, opt[0], lbl_raw, sizeof(lbl_raw));
    } else {
        CounterDef *c = counter_find_mut(p->env, (StrView){"enumiv", 6});
        snprintf(lbl_raw, sizeof(lbl_raw), "%d", c ? c->value : 1);
    }

    bibitem_register(p->env, args[0], lbl_raw);
    char aid[128];
    make_anchor_id("dest-bib:", args[0], aid, sizeof(aid));
    xml_emit_anchor(p->gen, aid);
    snprintf(p->env->current_ref, sizeof(p->env->current_ref), "%s", lbl_raw);
    snprintf(p->gen->pending_item_label, sizeof(p->gen->pending_item_label), "[%s]", lbl_raw);
    p->gen->has_pending_item_label = true;
}

static void builtin_cite(Parser *p, [[maybe_unused]] bool s, StrView opt[], size_t opt_c, StrView args[]) {
    xml_emit_text(p->gen, (StrView){"[", 1}, p->cmd_space_before);

    char saved_href[256];
    snprintf(saved_href, sizeof(saved_href), "%s", p->gen->current_href);

    const char *cur = args[0].data;
    const char *end = args[0].data + args[0].len;
    bool first = true;
    while (cur < end) {
        const char *comma = memchr(cur, ',', (size_t)(end - cur));
        size_t len = comma ? (size_t)(comma - cur) : (size_t)(end - cur);
        StrView key = sv_trim((StrView){ cur, len });
        if (key.len > 0) {
            if (!first) {
                snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", saved_href);
                xml_emit_text(p->gen, (StrView){", ", 2}, false);
            }
            const BibItemDef *bd = bibitem_find(p->env, key);
            const char *lbl = (bd && bd->label[0] != '\0') ? bd->label : "?";
            char href[128];
            make_anchor_id("#dest-bib:", key, href, sizeof(href));
            snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", href);
            xml_emit_text(p->gen, (StrView){ lbl, strlen(lbl) }, false);
            first = false;
        }
        cur = comma ? (comma + 1) : end;
    }

    snprintf(p->gen->current_href, sizeof(p->gen->current_href), "%s", saved_href);

    if (opt_c > 0 && opt[0].len > 0) {
        char note_plain[128], nbuf[144];
        eval_tex_to_plain(p, opt[0], note_plain, sizeof(note_plain));
        snprintf(nbuf, sizeof(nbuf), ", %s", note_plain);
        xml_emit_text(p->gen, (StrView){ nbuf, strlen(nbuf) }, false);
    }
    xml_emit_text(p->gen, (StrView){"]", 1}, false);
    p->cmd_space_before = false;
}

static void builtin_index(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    CounterDef *pc = counter_find_mut(p->env, (StrView){"page", 4});
    int pnum = pc ? pc->value : (int)p->gen->page_count;
    index_record_entry(p->env, args[0], pnum);
    char aid[128];
    make_anchor_id("dest-idx:", args[0], aid, sizeof(aid));
    xml_emit_anchor(p->gen, aid);
    p->cmd_space_before = false;
}

static int cmp_index_entries(const void *a, const void *b) {
    const IndexEntryDef *ia = (const IndexEntryDef *)a;
    const IndexEntryDef *ib = (const IndexEntryDef *)b;
    int c = strcmp(ia->entry, ib->entry);
    if (c != 0) return c;
    return ia->page_num - ib->page_num;
}

static void builtin_printindex(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    if (p->env->is_prescan || p->env->idx_count == 0) return;

    qsort(p->env->idx_entries, p->env->idx_count, sizeof(IndexEntryDef), cmp_index_entries);

    StrBuf ibuf;
    if (!sb_init(&ibuf, 512)) return;
    sb_append_cstr(&ibuf, "\\begin{theindex}\n");

    size_t i = 0;
    while (i < p->env->idx_count) {
        const char *raw_e = p->env->idx_entries[i].entry;
        char target_name[128];
        make_anchor_id("idx:", (StrView){ raw_e, strlen(raw_e) }, target_name, sizeof(target_name));
        const char *bang  = strchr(raw_e, '!');
        if (bang) {
            size_t main_len = (size_t)(bang - raw_e);
            sb_appendf(&ibuf, "\\par\\noindent\\hyperlink{%s}{\\textbf{%.*s} — %s, \\pageref{%s}}",
                       target_name, (int)main_len, raw_e, bang + 1, target_name);
        } else {
            sb_appendf(&ibuf, "\\par\\noindent\\hyperlink{%s}{\\textbf{%s}, \\pageref{%s}}",
                       target_name, raw_e, target_name);
        }
        size_t j = i + 1;
        while (j < p->env->idx_count && strcmp(p->env->idx_entries[j].entry, raw_e) == 0) {
            if (p->env->idx_entries[j].page_num != p->env->idx_entries[j - 1].page_num) {
                sb_appendf(&ibuf, ", %d", p->env->idx_entries[j].page_num);
            }
            j++;
        }
        sb_append_cstr(&ibuf, "\\par\n");
        i = j;
    }
    sb_append_cstr(&ibuf, "\\end{theindex}\n");
    execute_tex_fragment(p, (StrView){ ibuf.data, ibuf.len }, false);
    sb_free(&ibuf);
    p->cmd_space_before = false;
}

// --- Comandos del Entorno Picture (\put, \multiput, \line, \vector, \circle, \oval, \qbezier, \dashbox) ---

static void builtin_put(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    double px = 0.0, py = 0.0;
    parse_pic_coords(p, &px, &py);
    StrView obj = {0};
    if (!parse_arg(p, &obj)) return;

    double ul = get_unitlength_mm(p);
    double saved_x = p->gen->pic_cur_x_mm;
    double saved_y = p->gen->pic_cur_y_mm;
    p->gen->pic_cur_x_mm = saved_x + (px * ul - p->gen->pic_x0_mm);
    p->gen->pic_cur_y_mm = saved_y + (py * ul - p->gen->pic_y0_mm);
    execute_tex_fragment(p, obj, false);
    p->gen->pic_cur_x_mm = saved_x;
    p->gen->pic_cur_y_mm = saved_y;
    p->cmd_space_before = false;
}

static void builtin_multiput(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    double px = 0.0, py = 0.0, dx = 0.0, dy = 0.0;
    parse_pic_coords(p, &px, &py);
    parse_pic_coords(p, &dx, &dy);
    StrView n_sv = {0}, obj = {0};
    if (!parse_arg(p, &n_sv) || !parse_arg(p, &obj)) return;

    long count = sv_to_long(p, n_sv);
    if (count < 0) count = 0;
    if (count > 256) count = 256;

    double ul = get_unitlength_mm(p);
    double saved_x = p->gen->pic_cur_x_mm;
    double saved_y = p->gen->pic_cur_y_mm;
    for (long i = 0; i < count; i++) {
        p->gen->pic_cur_x_mm = saved_x + ((px + (double)i * dx) * ul - p->gen->pic_x0_mm);
        p->gen->pic_cur_y_mm = saved_y + ((py + (double)i * dy) * ul - p->gen->pic_y0_mm);
        execute_tex_fragment(p, obj, false);
    }
    p->gen->pic_cur_x_mm = saved_x;
    p->gen->pic_cur_y_mm = saved_y;
    p->cmd_space_before = false;
}

static void emit_pic_line_or_vec(Parser *p, bool is_arrow) {
    double dx = 1.0, dy = 0.0;
    parse_pic_coords(p, &dx, &dy);
    StrView len_sv = {0};
    if (!parse_arg(p, &len_sv)) return;
    if (p->gen->capture_plain_buf) return;

    char lbuf[64];
    size_t n = (len_sv.len < sizeof(lbuf) - 1) ? len_sv.len : (sizeof(lbuf) - 1);
    memcpy(lbuf, len_sv.data, n);
    lbuf[n] = '\0';
    double len_u = strtod(lbuf, nullptr);
    double ul = get_unitlength_mm(p);

    double x1 = p->gen->pic_cur_x_mm;
    double y1 = p->gen->pic_cur_y_mm;
    double x2 = x1, y2 = y1;
    if (fabs(dx) < 1e-6) {
        y2 = y1 + ((dy >= 0.0) ? 1.0 : -1.0) * len_u * ul;
    } else {
        double sx = (dx >= 0.0) ? 1.0 : -1.0;
        x2 = x1 + sx * len_u * ul;
        y2 = y1 + (dy / fabs(dx)) * len_u * ul;
    }

    char bx1[32], by1[32], bx2[32], by2[32], bt[32];
    measure_format(measure_make(x1, UNIT_MM, 1.0), bx1, sizeof(bx1));
    measure_format(measure_make(y1, UNIT_MM, 1.0), by1, sizeof(by1));
    measure_format(measure_make(x2, UNIT_MM, 1.0), bx2, sizeof(bx2));
    measure_format(measure_make(y2, UNIT_MM, 1.0), by2, sizeof(by2));
    measure_format(measure_make(p->gen->pic_line_thick_mm, UNIT_MM, 1.0), bt, sizeof(bt));

    sb_appendf(&p->gen->story,
               "        <pline x1=\"%s\" y1=\"%s\" x2=\"%s\" y2=\"%s\" thick=\"%s\" arrow=\"%d\"/>\n",
               bx1, by1, bx2, by2, bt, is_arrow ? 1 : 0);
    p->cmd_space_before = false;
}

static void builtin_line(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    emit_pic_line_or_vec(p, false);
}

static void builtin_vector(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    emit_pic_line_or_vec(p, true);
}

static void builtin_circle(Parser *p, bool starred, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    if (p->gen->capture_plain_buf) return;
    char dbuf[64];
    size_t n = (args[0].len < sizeof(dbuf) - 1) ? args[0].len : (sizeof(dbuf) - 1);
    memcpy(dbuf, args[0].data, n);
    dbuf[n] = '\0';
    double diam_u = strtod(dbuf, nullptr);
    double r_mm = 0.5 * diam_u * get_unitlength_mm(p);

    char bcx[32], bcy[32], br[32], bt[32];
    measure_format(measure_make(p->gen->pic_cur_x_mm, UNIT_MM, 1.0), bcx, sizeof(bcx));
    measure_format(measure_make(p->gen->pic_cur_y_mm, UNIT_MM, 1.0), bcy, sizeof(bcy));
    measure_format(measure_make(r_mm, UNIT_MM, 1.0), br, sizeof(br));
    measure_format(measure_make(p->gen->pic_line_thick_mm, UNIT_MM, 1.0), bt, sizeof(bt));

    sb_appendf(&p->gen->story,
               "        <pcircle cx=\"%s\" cy=\"%s\" r=\"%s\" thick=\"%s\" fill=\"%d\"/>\n",
               bcx, bcy, br, bt, starred ? 1 : 0);
    p->cmd_space_before = false;
}

static void builtin_oval(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    double ow = 10.0, oh = 10.0;
    parse_pic_coords(p, &ow, &oh);
    StrView part_opt = {0};
    parse_opt_arg(p, &part_opt);
    if (p->gen->capture_plain_buf) return;

    double ul = get_unitlength_mm(p);
    char bcx[32], bcy[32], bw[32], bh[32], bt[32], bpart[8] = "";
    if (part_opt.len > 0) {
        StrView tr = sv_trim(part_opt);
        snprintf(bpart, sizeof(bpart), "%.*s", (int)(tr.len < 4 ? tr.len : 4), tr.data);
    }
    measure_format(measure_make(p->gen->pic_cur_x_mm, UNIT_MM, 1.0), bcx, sizeof(bcx));
    measure_format(measure_make(p->gen->pic_cur_y_mm, UNIT_MM, 1.0), bcy, sizeof(bcy));
    measure_format(measure_make(ow * ul, UNIT_MM, 1.0), bw, sizeof(bw));
    measure_format(measure_make(oh * ul, UNIT_MM, 1.0), bh, sizeof(bh));
    measure_format(measure_make(p->gen->pic_line_thick_mm, UNIT_MM, 1.0), bt, sizeof(bt));

    sb_appendf(&p->gen->story,
               "        <poval cx=\"%s\" cy=\"%s\" width=\"%s\" height=\"%s\" thick=\"%s\" part=\"%s\"/>\n",
               bcx, bcy, bw, bh, bt, bpart);
    p->cmd_space_before = false;
}

static void builtin_qbezier(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    StrView n_opt = {0};
    parse_opt_arg(p, &n_opt);
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x3 = 0.0, y3 = 0.0;
    parse_pic_coords(p, &x1, &y1);
    parse_pic_coords(p, &x2, &y2);
    parse_pic_coords(p, &x3, &y3);
    if (p->gen->capture_plain_buf) return;

    double ul = get_unitlength_mm(p);
    char bx1[32], by1[32], bx2[32], by2[32], bx3[32], by3[32], bt[32];
    measure_format(measure_make(p->gen->pic_cur_x_mm + (x1 * ul - p->gen->pic_x0_mm), UNIT_MM, 1.0), bx1, sizeof(bx1));
    measure_format(measure_make(p->gen->pic_cur_y_mm + (y1 * ul - p->gen->pic_y0_mm), UNIT_MM, 1.0), by1, sizeof(by1));
    measure_format(measure_make(p->gen->pic_cur_x_mm + (x2 * ul - p->gen->pic_x0_mm), UNIT_MM, 1.0), bx2, sizeof(bx2));
    measure_format(measure_make(p->gen->pic_cur_y_mm + (y2 * ul - p->gen->pic_y0_mm), UNIT_MM, 1.0), by2, sizeof(by2));
    measure_format(measure_make(p->gen->pic_cur_x_mm + (x3 * ul - p->gen->pic_x0_mm), UNIT_MM, 1.0), bx3, sizeof(bx3));
    measure_format(measure_make(p->gen->pic_cur_y_mm + (y3 * ul - p->gen->pic_y0_mm), UNIT_MM, 1.0), by3, sizeof(by3));
    measure_format(measure_make(p->gen->pic_line_thick_mm, UNIT_MM, 1.0), bt, sizeof(bt));

    sb_appendf(&p->gen->story,
               "        <pbezier x1=\"%s\" y1=\"%s\" x2=\"%s\" y2=\"%s\" x3=\"%s\" y3=\"%s\" thick=\"%s\"/>\n",
               bx1, by1, bx2, by2, bx3, by3, bt);
    p->cmd_space_before = false;
}

static void builtin_dashbox(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    StrView dash_sv = {0};
    if (!parse_arg(p, &dash_sv)) return;
    char dbuf[64];
    size_t n = (dash_sv.len < sizeof(dbuf) - 1) ? dash_sv.len : (sizeof(dbuf) - 1);
    memcpy(dbuf, dash_sv.data, n);
    dbuf[n] = '\0';
    double dash_u = strtod(dbuf, nullptr);

    double pw = 0.0, ph = 0.0;
    parse_pic_coords(p, &pw, &ph);
    emit_pic_box_helper(p, pw, ph, p->gen->pic_line_thick_mm, dash_u * get_unitlength_mm(p));
    p->cmd_space_before = false;
}

static void builtin_shortstack(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    char plain[512];
    CharState box_cs = p->gen->char_state;
    eval_tex_box(p, args[0], plain, sizeof(plain), &box_cs);
    if (p->gen->in_picture) {
        xml_emit_text(p->gen, (StrView){ plain, strlen(plain) }, false);
    } else {
        Measure zero = measure_make(0.0, UNIT_MM, 1.0);
        xml_emit_box(p->gen, zero, 'c', zero, zero, zero, &box_cs, (StrView){ plain, strlen(plain) }, p->cmd_space_before);
    }
    p->cmd_space_before = false;
}

static void builtin_thinlines(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->pic_line_thick_mm = 0.4 * MM_PER_PT;
}

static void builtin_thicklines(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, [[maybe_unused]] StrView a[]) {
    p->gen->pic_line_thick_mm = 0.8 * MM_PER_PT;
}

static void builtin_linethickness(Parser *p, [[maybe_unused]] bool s, [[maybe_unused]] StrView o[], [[maybe_unused]] size_t oc, StrView args[]) {
    Measure m = measure_parse(p, args[0], UNIT_MM);
    p->gen->pic_line_thick_mm = m.mm;
}

// ============================================================================
// 10. Tabla de Comandos Nativos (Builtins)
// ============================================================================

static const BuiltinCommand BUILTIN_LIST[] = {
    // Definiciones de comandos y entornos
    { "newcommand",       0, 0, true,  builtin_newcommand },
    { "renewcommand",     0, 0, true,  builtin_renewcommand },
    { "providecommand",   0, 0, true,  builtin_providecommand },
    { "def",              0, 0, false, builtin_def },
    { "gdef",             0, 0, false, builtin_def },
    { "edef",             0, 0, false, builtin_def },
    { "xdef",             0, 0, false, builtin_def },
    { "let",              0, 0, false, builtin_let },
    { "newenvironment",   0, 0, true,  builtin_newenvironment },
    { "renewenvironment", 0, 0, true,  builtin_renewenvironment },
    { "makeatletter",     0, 0, false, builtin_makeatletter },
    { "makeatother",      0, 0, false, builtin_makeatother },
    { "relax",            0, 0, false, builtin_noop },
    { "ignorespaces",     0, 0, false, builtin_noop },
    { "protect",          0, 0, false, builtin_noop },

    // Contadores
    { "newcounter",       0, 1, false, builtin_newcounter },
    { "setcounter",       0, 2, false, builtin_setcounter },
    { "addtocounter",     0, 2, false, builtin_addtocounter },
    { "stepcounter",      0, 1, false, builtin_stepcounter },
    { "refstepcounter",   0, 1, false, builtin_refstepcounter },
    { "arabic",           0, 1, false, builtin_arabic },
    { "roman",            0, 1, false, builtin_roman },
    { "Roman",            0, 1, false, builtin_Roman },
    { "alph",             0, 1, false, builtin_alph },
    { "Alph",             0, 1, false, builtin_Alph },
    { "fnsymbol",         0, 1, false, builtin_fnsymbol },

    // Longitudes
    { "newlength",        0, 0, false, builtin_newlength },
    { "setlength",        0, 0, false, builtin_setlength },
    { "addtolength",      0, 0, false, builtin_addtolength },
    { "settowidth",       0, 0, false, builtin_settowidth },
    { "settoheight",      0, 0, false, builtin_settoheight },
    { "settodepth",       0, 0, false, builtin_settodepth },

    // Inserciones especiales dinámicas, cajas, tablas, flotantes, notas y estilo de página
    { "today",            0, 0, false, builtin_today },
    { "rule",             1, 2, false, builtin_rule },
    { "mbox",             0, 1, false, builtin_mbox },
    { "makebox",          0, 0, false, builtin_makebox },
    { "fbox",             0, 1, false, builtin_fbox },
    { "framebox",         0, 0, false, builtin_framebox },
    { "parbox",           3, 2, false, builtin_parbox },
    { "raisebox",         0, 0, false, builtin_raisebox },
    { "newsavebox",       0, 0, false, builtin_newsavebox },
    { "sbox",             0, 0, false, builtin_sbox },
    { "savebox",          0, 0, false, builtin_savebox },
    { "usebox",           0, 0, false, builtin_usebox },
    { "hline",            0, 0, false, builtin_hline },
    { "cline",            0, 1, false, builtin_cline },
    { "multicolumn",      0, 3, false, builtin_multicolumn },
    { "vline",            0, 0, false, builtin_vline },
    { "extracolsep",      0, 1, false, builtin_noop },
    { "tabularnewline",   1, 0, true,  builtin_linebreak_cmd },
    { "caption",          1, 1, true,  builtin_caption },
    { "suppressfloats",   1, 0, false, builtin_noop },
    { "footnote",         1, 1, false, builtin_footnote },
    { "footnotemark",     1, 0, false, builtin_footnotemark },
    { "footnotetext",     1, 1, false, builtin_footnotetext },
    { "marginpar",        1, 1, false, builtin_marginpar },
    { "pagestyle",        0, 1, false, builtin_pagestyle },
    { "thispagestyle",    0, 1, false, builtin_thispagestyle },
    { "pagenumbering",    0, 1, false, builtin_pagenumbering },
    { "markboth",         0, 2, false, builtin_markboth },
    { "markright",        0, 1, false, builtin_markright },

    // Fase 5: Modo matemático, referencias, TOC/LOF/LOT, bibliografía, índice y picture
    { "(",                0, 0, false, builtin_math_open_inline },
    { ")",                0, 0, false, builtin_math_close_inline },
    { "[",                0, 0, false, builtin_math_open_display },
    { "]",                0, 0, false, builtin_math_close_display },
    { "ensuremath",       0, 1, false, builtin_ensuremath },
    { "nonumber",         0, 0, false, builtin_nonumber },
    { "notag",            0, 0, false, builtin_nonumber },
    { "frac",             0, 2, false, builtin_frac },
    { "dfrac",            0, 2, false, builtin_frac },
    { "tfrac",            0, 2, false, builtin_frac },
    { "binom",            0, 2, false, builtin_binom },
    { "stackrel",         0, 2, false, builtin_stackrel },
    { "sqrt",             1, 1, false, builtin_sqrt },
    { "overline",         0, 1, false, builtin_overline },
    { "bar",              0, 1, false, builtin_bar },
    { "widehat",          0, 1, false, builtin_widehat },
    { "hat",              0, 1, false, builtin_hat },
    { "widetilde",        0, 1, false, builtin_widetilde },
    { "tilde",            0, 1, false, builtin_tilde },
    { "overbrace",        0, 1, false, builtin_overbrace },
    { "underbrace",       0, 1, false, builtin_underbrace },
    { "vec",              0, 1, false, builtin_vec },
    { "dot",              0, 1, false, builtin_dot },
    { "ddot",             0, 1, false, builtin_ddot },
    { "acute",            0, 1, false, builtin_acute },
    { "grave",            0, 1, false, builtin_grave },
    { "check",            0, 1, false, builtin_check },
    { "breve",            0, 1, false, builtin_breve },
    { "left",             0, 0, false, builtin_left_right },
    { "right",            0, 0, false, builtin_left_right },
    { "big",              0, 0, false, builtin_left_right },
    { "Big",              0, 0, false, builtin_left_right },
    { "bigg",             0, 0, false, builtin_left_right },
    { "Bigg",             0, 0, false, builtin_left_right },
    { "bigl",             0, 0, false, builtin_left_right },
    { "bigr",             0, 0, false, builtin_left_right },
    { "Bigl",             0, 0, false, builtin_left_right },
    { "Bigr",             0, 0, false, builtin_left_right },
    { "biggl",            0, 0, false, builtin_left_right },
    { "biggr",            0, 0, false, builtin_left_right },
    { "Biggl",            0, 0, false, builtin_left_right },
    { "Biggr",            0, 0, false, builtin_left_right },
    { "bmod",             0, 0, false, builtin_bmod },
    { "pmod",             0, 1, false, builtin_pmod },
    { "mathrm",           0, 1, false, builtin_mathrm },
    { "mathbf",           0, 1, false, builtin_mathbf },
    { "mathit",           0, 1, false, builtin_mathit },
    { "mathsf",           0, 1, false, builtin_mathsf },
    { "mathtt",           0, 1, false, builtin_mathtt },
    { "mathcal",          0, 1, false, builtin_mathcal },
    { "mathnormal",       0, 1, false, builtin_mathit },
    { "boldmath",         0, 0, false, builtin_boldmath },
    { "unboldmath",       0, 0, false, builtin_unboldmath },
    { "displaystyle",     0, 0, false, builtin_normalsize },
    { "textstyle",        0, 0, false, builtin_normalsize },
    { "scriptstyle",      0, 0, false, builtin_scriptsize },
    { "scriptscriptstyle",0, 0, false, builtin_tiny },
    { "label",            0, 1, false, builtin_label },
    { "ref",              0, 1, false, builtin_ref },
    { "pageref",          0, 1, false, builtin_pageref },
    { "tableofcontents",  0, 0, false, builtin_tableofcontents },
    { "listoffigures",    0, 0, false, builtin_listoffigures },
    { "listoftables",     0, 0, false, builtin_listoftables },
    { "addcontentsline",  0, 3, false, builtin_addcontentsline },
    { "addtocontents",    0, 2, false, builtin_noop },
    { "bibitem",          1, 1, false, builtin_bibitem },
    { "cite",             1, 1, false, builtin_cite },
    { "nocite",           0, 1, false, builtin_noop },
    { "bibliographystyle",0, 1, false, builtin_noop },
    { "bibliography",     0, 1, false, builtin_noop },
    { "makeindex",        0, 0, false, builtin_noop },
    { "index",            0, 1, false, builtin_index },
    { "printindex",       0, 0, false, builtin_printindex },
    { "href",             0, 2, false, builtin_href },
    { "url",              0, 1, false, builtin_url },
    { "nolinkurl",        0, 1, false, builtin_nolinkurl },
    { "hyperref",         1, 1, false, builtin_hyperref },
    { "hypertarget",      0, 2, false, builtin_hypertarget },
    { "hyperlink",        0, 2, false, builtin_hyperlink },
    { "put",              0, 0, false, builtin_put },
    { "multiput",         0, 0, false, builtin_multiput },
    { "line",             0, 0, false, builtin_line },
    { "vector",           0, 0, false, builtin_vector },
    { "circle",           0, 1, true,  builtin_circle },
    { "oval",             0, 0, false, builtin_oval },
    { "qbezier",          0, 0, false, builtin_qbezier },
    { "dashbox",          0, 0, false, builtin_dashbox },
    { "frame",            0, 1, false, builtin_fbox },
    { "shortstack",       1, 1, false, builtin_shortstack },
    { "thinlines",        0, 0, false, builtin_thinlines },
    { "thicklines",       0, 0, false, builtin_thicklines },
    { "linethickness",    0, 1, false, builtin_linethickness },

    // Clases de documento, paquetes y archivos
    { "begin",            0, 1, false, builtin_begin },
    { "end",              0, 1, false, builtin_end },
    { "documentclass",    1, 1, false, builtin_documentclass },
    { "documentstyle",    1, 1, false, builtin_documentclass },
    { "usepackage",       1, 1, false, builtin_noop },
    { "RequirePackage",   1, 1, false, builtin_noop },
    { "ProvidesClass",    1, 1, false, builtin_noop },
    { "ProvidesPackage",  1, 1, false, builtin_noop },
    { "input",            0, 1, false, builtin_input },
    { "include",          0, 1, false, builtin_include },
    { "includeonly",      0, 1, false, builtin_includeonly },

    // Metadatos, portada, seccionado y cartas
    { "title",            0, 1, false, builtin_title },
    { "author",           0, 1, false, builtin_author },
    { "date",             0, 1, false, builtin_date },
    { "thanks",           0, 1, false, builtin_thanks },
    { "and",              0, 0, false, builtin_and },
    { "maketitle",        0, 0, false, builtin_maketitle },
    { "part",             1, 1, true,  builtin_part },
    { "chapter",          1, 1, true,  builtin_chapter },
    { "section",          1, 1, true,  builtin_section },
    { "subsection",       1, 1, true,  builtin_subsection },
    { "subsubsection",    1, 1, true,  builtin_subsubsection },
    { "paragraph",        1, 1, true,  builtin_paragraph },
    { "subparagraph",     1, 1, true,  builtin_subparagraph },
    { "appendix",         0, 0, false, builtin_appendix },
    { "frontmatter",      0, 0, false, builtin_frontmatter },
    { "mainmatter",       0, 0, false, builtin_mainmatter },
    { "backmatter",       0, 0, false, builtin_backmatter },
    { "address",          0, 1, false, builtin_address },
    { "signature",        0, 1, false, builtin_signature },
    { "name",             0, 1, false, builtin_name },
    { "location",         0, 1, false, builtin_location },
    { "telephone",        0, 1, false, builtin_telephone },
    { "makelabels",       0, 0, false, builtin_noop },
    { "opening",          0, 1, false, builtin_opening },
    { "closing",          0, 1, false, builtin_closing },
    { "cc",               0, 1, false, builtin_cc },
    { "encl",             0, 1, false, builtin_encl },
    { "ps",               0, 0, false, builtin_par },

    // Geometría, página, saltos, espaciado, listas y párrafo
    { "pagesize",         0, 2, false, builtin_pagesize },
    { "orientation",      0, 1, false, builtin_orientation },
    { "margins",          0, 4, false, builtin_margins },
    { "\\",               1, 0, true,  builtin_linebreak_cmd },
    { "newline",          0, 0, false, builtin_linebreak_cmd },
    { "linebreak",        1, 0, false, builtin_linebreak_opt },
    { "nolinebreak",      1, 0, false, builtin_noop },
    { "sloppy",           0, 0, false, builtin_noop },
    { "fussy",            0, 0, false, builtin_noop },
    { "hyphenation",      0, 1, false, builtin_noop },
    { "newpage",          0, 0, false, builtin_newpage },
    { "clearpage",        0, 0, false, builtin_newpage },
    { "cleardoublepage",  0, 0, false, builtin_newpage },
    { "pagebreak",        1, 0, false, builtin_pagebreak_opt },
    { "nopagebreak",      1, 0, false, builtin_noop },
    { "enlargethispage",  0, 1, true,  builtin_enlargethispage },
    { "hspace",           0, 1, true,  builtin_hspace },
    { "hfill",            0, 0, false, builtin_hfill },
    { "hss",              0, 0, false, builtin_hfill },
    { "dotfill",          0, 0, false, builtin_dotfill },
    { "hrulefill",        0, 0, false, builtin_hrulefill },
    { "quad",             0, 0, false, builtin_quad },
    { "qquad",            0, 0, false, builtin_qquad },
    { "enspace",          0, 0, false, builtin_enspace },
    { "thinspace",        0, 0, false, builtin_thinspace },
    { "medspace",         0, 0, false, builtin_medspace },
    { "thickspace",       0, 0, false, builtin_thickspace },
    { "negthinspace",     0, 0, false, builtin_negthinspace },
    { ",",                0, 0, false, builtin_thinspace },
    { ":",                0, 0, false, builtin_medspace },
    { ";",                0, 0, false, builtin_thickspace },
    { "!",                0, 0, false, builtin_negthinspace },
    { "frenchspacing",    0, 0, false, builtin_noop },
    { "nonfrenchspacing", 0, 0, false, builtin_noop },
    { "vspace",           0, 1, true,  builtin_vspace },
    { "addvspace",        0, 1, false, builtin_vspace },
    { "vfill",            0, 0, false, builtin_vfill },
    { "smallskip",        0, 0, false, builtin_smallskip },
    { "medskip",          0, 0, false, builtin_medskip },
    { "bigskip",          0, 0, false, builtin_bigskip },
    { "item",             1, 0, false, builtin_item },
    { "usecounter",       0, 1, false, builtin_usecounter },
    { "newframe",         0, 4, false, builtin_newframe },
    { "twocolumn",        1, 0, false, builtin_twocolumn },
    { "onecolumn",        0, 0, false, builtin_onecolumn },
    { "align",            0, 1, false, builtin_align },
    { "centering",        0, 0, false, builtin_centering },
    { "raggedright",      0, 0, false, builtin_raggedright },
    { "raggedleft",       0, 0, false, builtin_raggedleft },
    { "noindent",         0, 0, false, builtin_noindent },
    { "indent",           0, 0, false, builtin_indent },
    { "linespacing",      0, 1, false, builtin_linespacing },
    { "parmargins",       0, 2, false, builtin_parmargins },
    { "parspacing",       0, 2, false, builtin_parspacing },
    { "par",              0, 0, false, builtin_par },
    { "parbreak",         0, 0, false, builtin_par },

    // Fuentes, estilos tipográficos y NFSS
    { "font",             0, 1, false, builtin_font },
    { "fontencoding",     0, 1, false, builtin_noop },
    { "fontfamily",       0, 1, false, builtin_fontfamily },
    { "fontseries",       0, 1, false, builtin_fontseries },
    { "fontshape",        0, 1, false, builtin_fontshape },
    { "fontsize",         0, 1, false, builtin_fontsize },
    { "selectfont",       0, 0, false, builtin_selectfont },
    { "usefont",          0, 4, false, builtin_usefont },
    { "linespread",       0, 1, false, builtin_linespread },
    { "fontcolor",        0, 1, false, builtin_fontcolor },
    { "color",            0, 1, false, builtin_fontcolor },
    { "rmfamily",         0, 0, false, builtin_rmfamily },
    { "sffamily",         0, 0, false, builtin_sffamily },
    { "ttfamily",         0, 0, false, builtin_ttfamily },
    { "bfseries",         0, 0, false, builtin_bfseries },
    { "mdseries",         0, 0, false, builtin_mdseries },
    { "itshape",          0, 0, false, builtin_itshape },
    { "slshape",          0, 0, false, builtin_itshape },
    { "scshape",          0, 0, false, builtin_scshape },
    { "upshape",          0, 0, false, builtin_upshape },
    { "normalfont",       0, 0, false, builtin_normalfont },
    { "em",               0, 0, false, builtin_em },
    { "rm",               0, 0, false, builtin_rmfamily },
    { "sf",               0, 0, false, builtin_sffamily },
    { "tt",               0, 0, false, builtin_ttfamily },
    { "bf",               0, 0, false, builtin_bfseries },
    { "it",               0, 0, false, builtin_itshape },
    { "sl",               0, 0, false, builtin_itshape },
    { "sc",               0, 0, false, builtin_scshape },
    { "textbf",           0, 1, false, builtin_textbf },
    { "textmd",           0, 1, false, builtin_textmd },
    { "textit",           0, 1, false, builtin_textit },
    { "textsl",           0, 1, false, builtin_textit },
    { "textsc",           0, 1, false, builtin_textsc },
    { "textup",           0, 1, false, builtin_textup },
    { "textrm",           0, 1, false, builtin_textrm },
    { "textsf",           0, 1, false, builtin_textsf },
    { "texttt",           0, 1, false, builtin_texttt },
    { "textnormal",       0, 1, false, builtin_textnormal },
    { "emph",             0, 1, false, builtin_emph },
    { "underline",        0, 1, false, builtin_underline },
    { "textsuperscript",  0, 1, false, builtin_textsuperscript },
    { "textsubscript",    0, 1, false, builtin_textsubscript },
    { "oldstylenums",     0, 1, false, builtin_oldstylenums },

    // Tamaños estándar de fuente
    { "tiny",             0, 0, false, builtin_tiny },
    { "scriptsize",       0, 0, false, builtin_scriptsize },
    { "footnotesize",     0, 0, false, builtin_footnotesize },
    { "small",            0, 0, false, builtin_small },
    { "normalsize",       0, 0, false, builtin_normalsize },
    { "large",            0, 0, false, builtin_large },
    { "Large",            0, 0, false, builtin_Large },
    { "LARGE",            0, 0, false, builtin_LARGE },
    { "huge",             0, 0, false, builtin_huge },
    { "Huge",             0, 0, false, builtin_Huge },
};
constexpr size_t BUILTIN_COUNT = sizeof(BUILTIN_LIST) / sizeof(BUILTIN_LIST[0]);

static const BuiltinCommand *find_builtin(StrView name) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (sv_equals(name, BUILTIN_LIST[i].name)) return &BUILTIN_LIST[i];
    }
    return nullptr;
}

// ============================================================================
// 11. Parser TeX (Argumentos Opcionales [...], Variantes *, Grupos y Acentos)
// ============================================================================

static void parser_advance(Parser *p) {
    p->current = lexer_next_token(&p->lexer);
}

// Comprueba y consume un modificador '*' inmediatamente después de un comando
static bool parser_consume_star(Parser *p) {
    if (p->current.type == TOKEN_TEXT && p->current.text.len >= 1 && p->current.text.data[0] == '*') {
        if (p->current.text.len == 1) {
            parser_advance(p);
        } else {
            p->current.text.data++;
            p->current.text.len--;
            p->current.col++;
        }
        return true;
    }
    return false;
}

// Comprueba y consume un argumento opcional entre corchetes [...]
static bool parse_opt_arg(Parser *p, StrView *out_opt) {
    if (p->current.type != TOKEN_TEXT || p->current.text.len == 0 || p->current.text.data[0] != '[') {
        return false;
    }

    const char *start = p->current.text.data + 1;
    size_t start_offset = (size_t)(start - p->lexer.src);
    p->lexer.cursor = start_offset;

    int bracket_depth = 1;
    int brace_depth   = 0;

    while (p->lexer.cursor < p->lexer.len && bracket_depth > 0) {
        char c = lexer_advance(&p->lexer);
        if (c == '\\' && p->lexer.cursor < p->lexer.len) {
            lexer_advance(&p->lexer);
            continue;
        }
        if (c == '{') brace_depth++;
        else if (c == '}' && brace_depth > 0) brace_depth--;
        else if (brace_depth == 0) {
            if (c == '[') bracket_depth++;
            else if (c == ']') bracket_depth--;
        }
    }

    if (bracket_depth != 0) {
        fprintf(stderr, "Error sintáctico: '[' sin cerrar en línea %zu\n", p->current.line);
        return false;
    }

    out_opt->data = start;
    out_opt->len  = (size_t)(&p->lexer.src[p->lexer.cursor - 1] - start);
    parser_advance(p);
    return true;
}

// Consume un argumento obligatorio entre llaves { ... } (o un único token TeX si no lleva llaves)
static bool parse_arg(Parser *p, StrView *out_arg) {
    if (p->current.type == TOKEN_LBRACE) {
        size_t open_line  = p->current.line;
        const char *start = p->current.text.data + 1;
        int depth = 1;

        while (p->lexer.cursor < p->lexer.len && depth > 0) {
            char c = lexer_advance(&p->lexer);
            if (c == '\\' && p->lexer.cursor < p->lexer.len) {
                char esc = lexer_peek(&p->lexer);
                if (esc == '{' || esc == '}' || esc == '\\') {
                    lexer_advance(&p->lexer);
                    continue;
                }
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
        }

        if (depth != 0) {
            fprintf(stderr, "Error sintáctico: llave sin cerrar abierta en línea %zu\n", open_line);
            return false;
        }

        out_arg->data = start;
        out_arg->len  = (size_t)(&p->lexer.src[p->lexer.cursor - 1] - start);
        parser_advance(p);
        return true;
    }

    // En TeX, si un argumento obligatorio no va entre llaves, toma el siguiente carácter o comando
    if (p->current.type == TOKEN_TEXT && p->current.text.len > 0) {
        out_arg->data = p->current.text.data;
        out_arg->len  = 1;
        if (p->current.text.len == 1) {
            parser_advance(p);
        } else {
            p->current.text.data++;
            p->current.text.len--;
            p->current.col++;
            p->current.space_before = false;
        }
        return true;
    }

    if (p->current.type == TOKEN_COMMAND) {
        out_arg->data = p->current.text.data - 1;
        out_arg->len  = p->current.text.len + 1;
        parser_advance(p);
        return true;
    }

    fprintf(stderr, "Error sintáctico: se esperaba argumento '{...}' en línea %zu, col %zu\n",
            p->current.line, p->current.col);
    return false;
}

// Consume {\cmd} o \cmd indistintamente (usado en \newcommand\foo, \setlength\parindent, \def\foo)
static bool parse_cmd_or_arg(Parser *p, StrView *out_arg) {
    if (p->current.type == TOKEN_COMMAND) {
        *out_arg = p->current.text;
        parser_advance(p);
        return true;
    }
    return parse_arg(p, out_arg);
}

static const char *find_math_function_name(StrView cmd) {
    static const struct { const char *cmd; const char *out; } MATH_FUNCS[] = {
        { "sin", "sin" }, { "cos", "cos" }, { "tan", "tan" }, { "cot", "cot" },
        { "sec", "sec" }, { "csc", "csc" }, { "arcsin", "arcsin" }, { "arccos", "arccos" },
        { "arctan", "arctan" }, { "sinh", "sinh" }, { "cosh", "cosh" }, { "tanh", "tanh" },
        { "coth", "coth" }, { "ln", "ln" }, { "log", "log" }, { "lg", "lg" },
        { "exp", "exp" }, { "lim", "lim" }, { "liminf", "lim inf" }, { "limsup", "lim sup" },
        { "sup", "sup" }, { "inf", "inf" }, { "max", "max" }, { "min", "min" },
        { "arg", "arg" }, { "det", "det" }, { "dim", "dim" }, { "gcd", "gcd" },
        { "hom", "hom" }, { "ker", "ker" }, { "Pr", "Pr" }, { "deg", "deg" }
    };
    for (size_t i = 0; i < sizeof(MATH_FUNCS) / sizeof(MATH_FUNCS[0]); i++) {
        if (sv_equals(cmd, MATH_FUNCS[i].cmd)) return MATH_FUNCS[i].out;
    }
    return nullptr;
}

static bool is_math_spaced_symbol(StrView cmd) {
    static const char *const SPACED[] = {
        "pm", "mp", "times", "div", "cdot", "ast", "star", "circ", "bullet",
        "oplus", "ominus", "otimes", "oslash", "odot", "cap", "cup", "uplus",
        "sqcap", "sqcup", "vee", "lor", "wedge", "land", "setminus",
        "leq", "le", "geq", "ge", "neq", "ne", "equiv", "approx", "cong",
        "sim", "simeq", "asymp", "doteq", "propto", "ll", "gg",
        "subset", "supset", "subseteq", "supseteq", "sqsubset", "sqsupset",
        "sqsubseteq", "sqsupseteq", "in", "notin", "ni", "owns",
        "vdash", "dashv", "models", "perp", "parallel", "mid", "bowtie",
        "prec", "succ", "preceq", "succeq",
        "leftarrow", "gets", "rightarrow", "to", "leftrightarrow",
        "Leftarrow", "Rightarrow", "Leftrightarrow", "iff", "mapsto",
        "longmapsto", "longrightarrow", "longleftarrow", "longleftrightarrow",
        "Longrightarrow", "Longleftarrow", "Longleftrightarrow"
    };
    for (size_t i = 0; i < sizeof(SPACED) / sizeof(SPACED[0]); i++) {
        if (sv_equals(cmd, SPACED[i])) return true;
    }
    return false;
}

static void parse_and_execute(Parser *p) {
    while (p->current.type != TOKEN_EOF) {
        if (p->current.type == TOKEN_TEXT) {
            xml_emit_text(p->gen, p->current.text, p->current.space_before);
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_PAR) {
            if (p->gen->inline_depth == 0) {
                xml_close_para(p->gen);
            }
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_MATH_SHIFT) {
            if (!p->gen->in_math) {
                if (p->current.space_before && p->gen->para_has_content && !p->gen->capture_plain_buf) {
                    xml_ensure_para_open(p->gen);
                    xml_activate_current_char_style(p->gen);
                    sb_append_cstr(&p->gen->story, " ");
                }
                xml_push_group(p->gen);
                p->gen->in_math          = true;
                p->gen->char_state.slant = SLANT_ITALIC;
                if (p->gen->math_bold) p->gen->char_state.weight = WEIGHT_BOLD;
            } else {
                p->gen->in_math = false;
                xml_pop_group(p->gen);
            }
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_DISPLAY_MATH) {
            if (!p->gen->in_display_math) {
                builtin_math_open_display(p, false, nullptr, 0, nullptr);
            } else {
                builtin_math_close_display(p, false, nullptr, 0, nullptr);
            }
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_SUP || p->current.type == TOKEN_SUB) {
            bool is_sup = (p->current.type == TOKEN_SUP);
            parser_advance(p);
            StrView arg = {0};
            if (parse_arg(p, &arg)) {
                CharState next = p->gen->char_state;
                next.shift = is_sup ? SHIFT_SUPER : SHIFT_SUB;
                parse_inline_scoped(p, arg, next);
            }
            continue;
        }

        // Agrupamiento léxico TeX: { ... } apila y restaura el estado tipográfico
        if (p->current.type == TOKEN_LBRACE) {
            if (p->current.space_before && p->gen->para_has_content && !p->gen->in_math) {
                xml_ensure_para_open(p->gen);
                xml_activate_current_char_style(p->gen);
                sb_append_cstr(&p->gen->story, " ");
            }
            xml_push_group(p->gen);
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_RBRACE) {
            xml_pop_group(p->gen);
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_AMP) {
            if (p->gen->in_tabular) {
                xml_table_next_cell(p->gen);
            } else if (p->gen->in_eqnarray) {
                xml_emit_hspace(p->gen, measure_make(1.5, UNIT_MM, 1.0), nullptr, false);
            }
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_COMMAND) {
            StrView cmd     = p->current.text;
            bool space_pend = p->current.space_before;

            // Caso especial léxico: \verb|...| y \verb*|...|
            if (sv_equals(cmd, "verb")) {
                bool v_star = false;
                if (lexer_peek(&p->lexer) == '*') {
                    lexer_advance(&p->lexer);
                    v_star = true;
                }
                while (p->lexer.cursor < p->lexer.len &&
                       (lexer_peek(&p->lexer) == ' ' || lexer_peek(&p->lexer) == '\t')) {
                    lexer_advance(&p->lexer);
                }
                char delim = lexer_advance(&p->lexer);
                if (delim != '\0' && delim != '\n') {
                    StrBuf vbuf;
                    if (sb_init(&vbuf, 64)) {
                        while (p->lexer.cursor < p->lexer.len &&
                               lexer_peek(&p->lexer) != delim &&
                               lexer_peek(&p->lexer) != '\n') {
                            char vc = lexer_advance(&p->lexer);
                            if (vc == ' ') {
                                sb_append_cstr(&vbuf, v_star ? "␣" : "\xC2\xA0");
                            } else {
                                sb_append_mem(&vbuf, &vc, 1);
                            }
                        }
                        if (p->lexer.cursor < p->lexer.len && lexer_peek(&p->lexer) == delim) {
                            lexer_advance(&p->lexer);
                        }
                        if (space_pend && p->gen->para_has_content) {
                            xml_ensure_para_open(p->gen);
                            xml_activate_current_char_style(p->gen);
                            sb_append_cstr(&p->gen->story, " ");
                        }
                        CharState saved_cs = p->gen->char_state;
                        snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family), "%s", DEFAULT_MONO_FAMILY);
                        p->gen->char_state.weight     = WEIGHT_NORMAL;
                        p->gen->char_state.slant      = SLANT_NORMAL;
                        p->gen->char_state.small_caps = false;
                        xml_emit_text(p->gen, (StrView){ vbuf.data, vbuf.len }, false);
                        p->gen->char_state = saved_cs;
                        sb_free(&vbuf);
                    }
                }
                parser_advance(p);
                continue;
            }

            parser_advance(p);

            // 1. ¿Es un comando de acento componible (\', \`, \^, \", \~...)?
            char accent_char = '\0';
            if (is_accent_command(cmd, &accent_char)) {
                StrView base_arg = {0};
                if (p->current.type == TOKEN_LBRACE || p->current.type == TOKEN_TEXT || p->current.type == TOKEN_COMMAND) {
                    parse_arg(p, &base_arg);
                }
                char utf8_buf[32];
                compose_latex_accent(accent_char, base_arg, utf8_buf, sizeof(utf8_buf));
                xml_emit_text(p->gen, (StrView){ utf8_buf, strlen(utf8_buf) }, space_pend);
                continue;
            }

            // 2. ¿Es una macro definida por el usuario? (Prioridad alta por si \renewcommand redefine \thesection, etc.)
            const UserMacro *macro = macro_env_find(p->env, cmd);
            if (macro) {
                p->cmd_space_before = space_pend;
                StrView args[MAX_PARAMS] = {0};
                bool ok = true;
                size_t start_idx = 0;

                if (macro->has_opt_default && macro->param_count > 0) {
                    StrView opt_val = {0};
                    if (parse_opt_arg(p, &opt_val)) {
                        args[0] = opt_val;
                    } else {
                        args[0] = (StrView){ macro->opt_default, strlen(macro->opt_default) };
                    }
                    start_idx = 1;
                }

                for (size_t i = start_idx; i < macro->param_count; i++) {
                    if (!parse_arg(p, &args[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) expand_user_macro(p, macro, args);
                p->cmd_space_before = false;
                continue;
            }

            // 3. ¿Es una función matemática estándar (\sin, \cos, \lim, \max...)?
            const char *mfunc = find_math_function_name(cmd);
            if (mfunc) {
                CharState saved_cs = p->gen->char_state;
                bool saved_math    = p->gen->in_math;
                p->gen->in_math          = false;
                p->gen->char_state.slant = SLANT_NORMAL;
                xml_emit_text(p->gen, (StrView){ mfunc, strlen(mfunc) }, space_pend);
                if (p->current.type == TOKEN_TEXT && p->current.text.len > 0 && isalpha((unsigned char)p->current.text.data[0])) {
                    xml_emit_text(p->gen, (StrView){"\xC2\xA0", 2}, false);
                }
                p->gen->char_state = saved_cs;
                p->gen->in_math    = saved_math;
                continue;
            }

            // 4. ¿Es un símbolo de texto, matemático o logo de LaTeX2e?
            const char *sym_utf8 = find_latex_text_symbol(cmd);
            if (sym_utf8) {
                if (p->gen->in_math && !p->gen->capture_plain_buf) {
                    bool spaced = is_math_spaced_symbol(cmd);
                    char sbuf[32];
                    if (spaced) {
                        snprintf(sbuf, sizeof(sbuf), " %s ", sym_utf8);
                    } else {
                        snprintf(sbuf, sizeof(sbuf), "%s", sym_utf8);
                    }
                    CharState saved_cs = p->gen->char_state;
                    bool saved_math    = p->gen->in_math;
                    p->gen->in_math          = false;
                    // Las minúsculas griegas van en cursiva matemática; operadores y mayúsculas en redonda
                    bool is_lower_greek = (cmd.len >= 2 && islower((unsigned char)cmd.data[0]) && !spaced &&
                                           (sv_equals(cmd, "alpha") || sv_equals(cmd, "beta") || sv_equals(cmd, "gamma") ||
                                            sv_equals(cmd, "delta") || sv_equals(cmd, "epsilon") || sv_equals(cmd, "varepsilon") ||
                                            sv_equals(cmd, "zeta") || sv_equals(cmd, "eta") || sv_equals(cmd, "theta") ||
                                            sv_equals(cmd, "vartheta") || sv_equals(cmd, "iota") || sv_equals(cmd, "kappa") ||
                                            sv_equals(cmd, "lambda") || sv_equals(cmd, "mu") || sv_equals(cmd, "nu") ||
                                            sv_equals(cmd, "xi") || sv_equals(cmd, "pi") || sv_equals(cmd, "varpi") ||
                                            sv_equals(cmd, "rho") || sv_equals(cmd, "varrho") || sv_equals(cmd, "sigma") ||
                                            sv_equals(cmd, "varsigma") || sv_equals(cmd, "tau") || sv_equals(cmd, "upsilon") ||
                                            sv_equals(cmd, "phi") || sv_equals(cmd, "varphi") || sv_equals(cmd, "chi") ||
                                            sv_equals(cmd, "psi") || sv_equals(cmd, "omega")));
                    p->gen->char_state.slant = is_lower_greek ? SLANT_ITALIC : SLANT_NORMAL;
                    xml_emit_text(p->gen, (StrView){ sbuf, strlen(sbuf) }, false);
                    p->gen->char_state = saved_cs;
                    p->gen->in_math    = saved_math;
                } else {
                    xml_emit_text(p->gen, (StrView){ sym_utf8, strlen(sym_utf8) }, space_pend);
                }
                continue;
            }

            // 5. ¿Es un comando nativo (Builtin)?
            const BuiltinCommand *builtin = find_builtin(cmd);
            if (builtin) {
                p->cmd_space_before = space_pend;
                bool starred = builtin->supports_star ? parser_consume_star(p) : false;

                StrView opt_args[MAX_PARAMS] = {0};
                size_t opt_count = 0;
                while (opt_count < builtin->max_opt_args && parse_opt_arg(p, &opt_args[opt_count])) {
                    opt_count++;
                }

                StrView args[MAX_PARAMS] = {0};
                bool ok = true;
                for (size_t i = 0; i < builtin->arity; i++) {
                    if (!parse_arg(p, &args[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) builtin->handler(p, starred, opt_args, opt_count, args);
                if (p->cmd_space_before && p->gen->para_has_content) {
                    p->current.space_before = true;
                }
                p->cmd_space_before = false;
                continue;
            }

            // 5. ¿Es \the<counter>?
            if (cmd.len > 3 && strncmp(cmd.data, "the", 3) == 0) {
                StrView cname = { cmd.data + 3, cmd.len - 3 };
                if (counter_find_mut(p->env, cname) != nullptr) {
                    char cbuf[64];
                    format_the_counter(p->env, p->gen, cname, cbuf, sizeof(cbuf));
                    xml_emit_text(p->gen, (StrView){ cbuf, strlen(cbuf) }, space_pend);
                    continue;
                }
            }

            // 6. Comando desconocido: emitir literalmente
            char raw_cmd[64];
            int n = snprintf(raw_cmd, sizeof(raw_cmd), "\\%.*s", (int)cmd.len, cmd.data);
            if (n > 0) {
                StrView raw_view = { .data = raw_cmd, .len = (size_t)n };
                xml_emit_text(p->gen, raw_view, space_pend);
            }
            continue;
        }

        parser_advance(p);
    }
}

static bool process_file(const char *path, MacroEnv *env, XmlGenerator *gen, size_t macro_depth) {
    if (!has_tex_extension(path)) {
        fprintf(stderr, "Error: '%s' no tiene extensión .tex\n", path);
        return false;
    }

    size_t len = 0;
    char *source = read_entire_file(path, &len);
    if (!source) {
        fprintf(stderr, "Error: no se pudo leer '%s'\n", path);
        return false;
    }

    Parser p = {
        .lexer       = lexer_init(source, len, env->allow_at),
        .env         = env,
        .gen         = gen,
        .macro_depth = macro_depth
    };

    parser_advance(&p);
    parse_and_execute(&p);

    free(source);
    return true;
}

// ============================================================================
// 12. Función Principal (main)
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <fichero_de_entrada.tex> [salida.tks]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *input_path = argv[1];
    if (!has_tex_extension(input_path)) {
        fprintf(stderr, "Error: el fichero debe tener extensión .tex (recibido: '%s')\n", input_path);
        return EXIT_FAILURE;
    }

    MacroEnv user_macros;
    macro_env_init(&user_macros);

    char tks_output_path[256];
    if (argc >= 3) {
        snprintf(tks_output_path, sizeof(tks_output_path), "%s", argv[2]);
    } else {
        replace_extension(input_path, ".tks", tks_output_path, sizeof(tks_output_path));
    }

    printf("Procesando '%s' -> Generando '%s'...\n", input_path, tks_output_path);

    // Pasada 1 (prescan en memoria): recopila \label, entradas de TOC/LOF/LOT, \bibitem e \index
    XmlGenerator prescan_gen;
    if (xml_gen_init(&prescan_gen, nullptr)) {
        user_macros.is_prescan = true;
        process_file(input_path, &user_macros, &prescan_gen, 0);
        xml_gen_finalize(&prescan_gen);
    }
    macro_env_reset_for_pass2(&user_macros);

    // Pasada 2 (emisión definitiva al archivo intermedio .tks)
    XmlGenerator gen;
    if (!xml_gen_init(&gen, tks_output_path)) {
        perror("Error inicializando generador TKS (XML intermedio)");
        macro_env_free(&user_macros);
        return EXIT_FAILURE;
    }

    if (!process_file(input_path, &user_macros, &gen, 0)) {
        fprintf(stderr, "Fallo durante el procesamiento del documento\n");
        xml_gen_finalize(&gen);
        macro_env_free(&user_macros);
        return EXIT_FAILURE;
    }

    xml_gen_finalize(&gen);
    macro_env_free(&user_macros);

    printf("Completado con éxito. Archivo generado: '%s'\n", tks_output_path);
    return EXIT_SUCCESS;
}
