#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>

// ============================================================================
// 1. Constantes y Límites del Sistema (C23)
// ============================================================================

constexpr size_t MAX_MACROS       = 256;
constexpr size_t MAX_PARAMS       = 8;
constexpr size_t MAX_PAGES        = 500;
constexpr size_t MAX_FRAMES       = 500;
constexpr size_t MAX_MACRO_DEPTH  = 32;

constexpr size_t MAX_FONT_FAMS    = 64;
constexpr size_t MAX_PARA_STYLES  = 256;
constexpr size_t MAX_CHAR_STYLES  = 512;

constexpr double MM_PER_CM        = 10.0;
constexpr double MM_PER_M         = 1000.0;
constexpr double MM_PER_PT        = 25.4 / 72.0;
constexpr double MEASURE_EPSILON  = 1e-4;

static const char *const DEFAULT_FONT_FAMILY = "DejaVu Sans";
static const char *const DEFAULT_COLOR       = "Black";

// ============================================================================
// 2. Unidades Métricas, Orientación y Estado Tipográfico
// ============================================================================

typedef enum : unsigned char {
    UNIT_MM = 0,
    UNIT_CM,
    UNIT_M,
    UNIT_PT
} Unit;

typedef struct {
    double value;   // Valor en la unidad original elegida
    double mm;      // Valor normalizado en milímetros para comparación exacta
    Unit unit;
} Measure;

typedef enum : unsigned char {
    ORIENT_PORTRAIT = 0,
    ORIENT_LANDSCAPE
} PageOrientation;

typedef enum : unsigned char {
    ALIGN_LEFT = 0,
    ALIGN_CENTER,
    ALIGN_RIGHT,
    ALIGN_JUSTIFY
} ParaAlign;

typedef enum : unsigned char {
    WEIGHT_NORMAL = 0,
    WEIGHT_BOLD   = 1
} FontWeight;

typedef enum : unsigned char {
    SLANT_NORMAL = 0,
    SLANT_ITALIC = 1
} FontSlant;

// Máscara de variantes usadas por familia: bit 0=(N,N), 1=(B,N), 2=(N,I), 3=(B,I)
typedef struct {
    char family[64];
    unsigned char used_variants_mask;
} FontFamilyDef;

typedef struct {
    size_t family_idx;
    FontWeight weight;
    FontSlant slant;
} FontVariantRef;

typedef struct {
    ParaAlign align;
    Measure line_spacing;
    Measure space_before;
    Measure space_after;
    Measure indent_first;
    Measure indent_left;
    Measure indent_right;
} ParaState;

typedef struct {
    char font_family[64];
    FontWeight weight;
    FontSlant slant;
    Measure size;
    char color[32];
    bool underline;
} CharState;

typedef struct {
    FontVariantRef variant;
    Measure size;
    char color[32];
    bool underline;
} CharStyleDef;

// ============================================================================
// 3. Estructuras del Lexer, Macros y Contexto del Generador XML
// ============================================================================

typedef struct {
    const char *data;
    size_t len;
} StrView;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

typedef enum {
    TOKEN_EOF = 0,
    TOKEN_COMMAND,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_TEXT,
    TOKEN_NEWLINE
} TokenType;

typedef struct {
    TokenType type;
    StrView text;
    size_t line;
    size_t col;
    bool space_before;
} Token;

typedef struct {
    const char *src;
    size_t cursor;
    size_t len;
    size_t line;
    size_t col;
} Lexer;

typedef struct {
    char name[48];
    size_t param_count;
    char *template_body;
} UserMacro;

typedef struct {
    UserMacro items[MAX_MACROS];
    size_t count;
    FILE *aux_file;
} MacroEnv;

typedef struct {
    size_t page_num;
    Measure x, y, width, height;
} LayoutFrame;

typedef struct {
    FILE *file;

    // Estado de página y maquetación por defecto
    Measure page_width;
    Measure page_height;
    PageOrientation orientation;
    Measure margin_top, margin_bottom, margin_left, margin_right;
    size_t page_count;

    LayoutFrame frames[MAX_FRAMES];
    size_t frame_count;

    // Estado actual de párrafo y carácter en curso
    ParaState para_state;
    CharState char_state;

    // Registros deduplicados para el bloque <definitions>
    FontFamilyDef fonts[MAX_FONT_FAMS];
    size_t font_count;

    ParaState para_styles[MAX_PARA_STYLES];
    size_t para_style_count;

    CharStyleDef char_styles[MAX_CHAR_STYLES];
    size_t char_style_count;

    // Control de emisión del flujo <body>
    StrBuf story;
    bool para_open;
    bool para_has_content;
    int active_char_style_id; // -1 si no hay <span> abierto
    size_t inline_depth;
} XmlGenerator;

typedef struct Parser Parser;
typedef void (*BuiltinFn)(Parser *p, StrView args[]);

typedef struct {
    const char *name;
    size_t arity;
    BuiltinFn handler;
} BuiltinCommand;

struct Parser {
    Lexer lexer;
    Token current;
    MacroEnv *env;
    XmlGenerator *gen;
    bool cmd_space_before;
    size_t macro_depth;
};

// ============================================================================
// 4. Prototipos de Funciones
// ============================================================================

static bool sv_equals(StrView sv, const char *literal);
static size_t sv_to_size(StrView sv);
static bool has_tks_extension(const char *path);
static void replace_extension(const char *path, const char *new_ext, char *out_buf, size_t buf_size);
static char *read_entire_file(const char *path, size_t *out_len);

static Measure measure_make(double value, Unit unit);
static Measure measure_parse(StrView sv, Unit default_unit);
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
static FontVariantRef xml_register_font_variant(XmlGenerator *gen, const char *family, FontWeight weight, FontSlant slant);
static size_t xml_intern_para_style(XmlGenerator *gen, const ParaState *state);
static size_t xml_intern_char_style(XmlGenerator *gen, const CharState *state);
static void xml_close_active_span(XmlGenerator *gen);
static void xml_ensure_para_open(XmlGenerator *gen);
static void xml_close_para(XmlGenerator *gen);
static void xml_emit_text(XmlGenerator *gen, StrView text, bool space_before);
static void xml_emit_page_break(XmlGenerator *gen);
static void xml_gen_finalize(XmlGenerator *gen);

static Lexer lexer_init(const char *source, size_t len);
static char lexer_peek(const Lexer *l);
static char lexer_advance(Lexer *l);
static Token lexer_next_token(Lexer *l);

static void macro_env_init(MacroEnv *env, FILE *aux_file);
static void macro_env_free(MacroEnv *env);
static bool macro_env_register(MacroEnv *env, StrView name, size_t params, StrView body);
static const UserMacro *macro_env_find(const MacroEnv *env, StrView name);
static void expand_user_macro(Parser *p, const UserMacro *m, StrView args[]);

static void builtin_newcommand(Parser *p, StrView args[]);
static void builtin_include(Parser *p, StrView args[]);
static void builtin_pagesize(Parser *p, StrView args[]);
static void builtin_orientation(Parser *p, StrView args[]);
static void builtin_margins(Parser *p, StrView args[]);
static void builtin_newpage(Parser *p, StrView args[]);
static void builtin_newframe(Parser *p, StrView args[]);
static void builtin_align(Parser *p, StrView args[]);
static void builtin_linespacing(Parser *p, StrView args[]);
static void builtin_parindent(Parser *p, StrView args[]);
static void builtin_parmargins(Parser *p, StrView args[]);
static void builtin_parspacing(Parser *p, StrView args[]);
static void builtin_parbreak(Parser *p, StrView args[]);
static void builtin_font(Parser *p, StrView args[]);
static void builtin_fontsize(Parser *p, StrView args[]);
static void builtin_fontcolor(Parser *p, StrView args[]);
static void builtin_fontweight(Parser *p, StrView args[]);
static void builtin_fontslant(Parser *p, StrView args[]);
static void builtin_textbf(Parser *p, StrView args[]);
static void builtin_textit(Parser *p, StrView args[]);
static void builtin_underline(Parser *p, StrView args[]);
static const BuiltinCommand *find_builtin(StrView name);

static void parser_advance(Parser *p);
static bool parse_arg(Parser *p, StrView *out_arg);
static void parse_and_execute(Parser *p);
static void parse_inline_scoped(Parser *p, StrView fragment, CharState modified_state);
static bool process_file(const char *path, MacroEnv *env, XmlGenerator *gen, size_t macro_depth);

// ============================================================================
// 5. Tabla de Comandos Nativos (Builtins)
// ============================================================================

static const BuiltinCommand BUILTIN_LIST[] = {
    { "newcommand",   3, builtin_newcommand },
    { "include",      1, builtin_include },
    { "pagesize",     2, builtin_pagesize },
    { "orientation",  1, builtin_orientation },
    { "margins",      4, builtin_margins },
    { "newpage",      0, builtin_newpage },
    { "newframe",     4, builtin_newframe },
    { "align",        1, builtin_align },
    { "linespacing",  1, builtin_linespacing },
    { "parindent",    1, builtin_parindent },
    { "parmargins",   2, builtin_parmargins },
    { "parspacing",   2, builtin_parspacing },
    { "parbreak",     0, builtin_parbreak },
    { "font",         1, builtin_font },
    { "fontsize",     1, builtin_fontsize },
    { "fontcolor",    1, builtin_fontcolor },
    { "fontweight",   1, builtin_fontweight },
    { "fontslant",    1, builtin_fontslant },
    { "textbf",       1, builtin_textbf },
    { "textit",       1, builtin_textit },
    { "underline",    1, builtin_underline },
};
constexpr size_t BUILTIN_COUNT = sizeof(BUILTIN_LIST) / sizeof(BUILTIN_LIST[0]);

// ============================================================================
// 6. Gestión de Unidades Métricas y Utilidades
// ============================================================================

static Measure measure_make(double value, Unit unit) {
    double mm = value;
    switch (unit) {
        case UNIT_MM: mm = value;            break;
        case UNIT_CM: mm = value * MM_PER_CM; break;
        case UNIT_M:  mm = value * MM_PER_M;  break;
        case UNIT_PT: mm = value * MM_PER_PT; break;
    }
    return (Measure){ .value = value, .mm = mm, .unit = unit };
}

static Measure measure_parse(StrView sv, Unit default_unit) {
    char buf[64];
    size_t n = (sv.len < sizeof(buf) - 1) ? sv.len : (sizeof(buf) - 1);
    memcpy(buf, sv.data, n);
    buf[n] = '\0';

    char *endptr = nullptr;
    double val = strtod(buf, &endptr);
    if (endptr == buf) {
        return measure_make(0.0, default_unit);
    }

    while (*endptr != '\0' && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    Unit unit = default_unit;
    if (strcmp(endptr, "mm") == 0) {
        unit = UNIT_MM;
    } else if (strcmp(endptr, "cm") == 0) {
        unit = UNIT_CM;
    } else if (strcmp(endptr, "m") == 0) {
        unit = UNIT_M;
    } else if (strcmp(endptr, "pt") == 0) {
        unit = UNIT_PT;
    }

    return measure_make(val, unit);
}

static bool measure_equals(Measure a, Measure b) {
    return fabs(a.mm - b.mm) < MEASURE_EPSILON;
}

static void measure_format(Measure m, char *buf, size_t buf_size) {
    const char *suffix = "mm";
    int decimals = 2;

    switch (m.unit) {
        case UNIT_MM: suffix = "mm"; decimals = 2; break;
        case UNIT_CM: suffix = "cm"; decimals = 3; break;
        case UNIT_M:  suffix = "m";  decimals = 4; break;
        case UNIT_PT: suffix = "pt"; decimals = 2; break;
    }

    char num[48];
    snprintf(num, sizeof(num), "%.*f", decimals, m.value);

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

static bool sv_equals(StrView sv, const char *literal) {
    size_t lit_len = strlen(literal);
    return (sv.len == lit_len) && (strncmp(sv.data, literal, lit_len) == 0);
}

static size_t sv_to_size(StrView sv) {
    char buf[64];
    size_t n = (sv.len < sizeof(buf) - 1) ? sv.len : (sizeof(buf) - 1);
    memcpy(buf, sv.data, n);
    buf[n] = '\0';
    return (size_t)strtoul(buf, nullptr, 10);
}

static bool has_tks_extension(const char *path) {
    size_t len = strlen(path);
    const char *ext = ".tks";
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
// 7. Buffer Dinámico de Cadenas y Escape XML
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
// 8. Control de Estado, Registro de Fuentes, Estilos y Generador XML
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

    gen->file = fopen(xml_path, "w");
    if (!gen->file) return false;

    if (!sb_init(&gen->story, 8192)) {
        fclose(gen->file);
        return false;
    }

    gen->page_width    = measure_make(210.0, UNIT_MM);
    gen->page_height   = measure_make(297.0, UNIT_MM);
    gen->orientation   = ORIENT_PORTRAIT;
    gen->margin_top    = measure_make(20.0, UNIT_MM);
    gen->margin_bottom = measure_make(20.0, UNIT_MM);
    gen->margin_left   = measure_make(20.0, UNIT_MM);
    gen->margin_right  = measure_make(20.0, UNIT_MM);
    gen->page_count    = 1;

    gen->para_state = (ParaState){
        .align        = ALIGN_LEFT,
        .line_spacing = measure_make(5.3, UNIT_MM),
        .space_before = measure_make(0.0, UNIT_MM),
        .space_after  = measure_make(2.0, UNIT_MM),
        .indent_first = measure_make(0.0, UNIT_MM),
        .indent_left  = measure_make(0.0, UNIT_MM),
        .indent_right = measure_make(0.0, UNIT_MM)
    };

    snprintf(gen->char_state.font_family, sizeof(gen->char_state.font_family), "%s", DEFAULT_FONT_FAMILY);
    gen->char_state.weight    = WEIGHT_NORMAL;
    gen->char_state.slant     = SLANT_NORMAL;
    gen->char_state.size      = measure_make(11.0, UNIT_PT);
    snprintf(gen->char_state.color, sizeof(gen->char_state.color), "%s", DEFAULT_COLOR);
    gen->char_state.underline = false;

    gen->active_char_style_id = -1;
    return true;
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
            measure_equals(def->size, state->size)     &&
            strcmp(def->color, state->color) == 0) {
            return i;
        }
    }

    size_t idx = (gen->char_style_count < MAX_CHAR_STYLES) ? gen->char_style_count++ : (MAX_CHAR_STYLES - 1);
    CharStyleDef *target = &gen->char_styles[idx];
    target->variant   = vref;
    target->size      = state->size;
    target->underline = state->underline;
    snprintf(target->color, sizeof(target->color), "%s", state->color);
    return idx;
}

static void xml_close_active_span(XmlGenerator *gen) {
    if (gen->active_char_style_id >= 0) {
        sb_append_cstr(&gen->story, "</span>");
        gen->active_char_style_id = -1;
    }
}

static void xml_ensure_para_open(XmlGenerator *gen) {
    if (gen->para_open) return;

    size_t ps_id = xml_intern_para_style(gen, &gen->para_state);
    sb_appendf(&gen->story, "      <p style=\"ps%zu\">", ps_id + 1);
    gen->para_open            = true;
    gen->para_has_content     = false;
    gen->active_char_style_id = -1;
}

static void xml_close_para(XmlGenerator *gen) {
    if (!gen->para_open) return;

    xml_close_active_span(gen);
    sb_append_cstr(&gen->story, "</p>\n");
    gen->para_open        = false;
    gen->para_has_content = false;
}

static void xml_activate_current_char_style(XmlGenerator *gen) {
    size_t cs_id = xml_intern_char_style(gen, &gen->char_state);
    if (gen->active_char_style_id != (int)cs_id) {
        xml_close_active_span(gen);
        sb_appendf(&gen->story, "<span style=\"cs%zu\">", cs_id + 1);
        gen->active_char_style_id = (int)cs_id;
    }
}

static void xml_emit_text(XmlGenerator *gen, StrView text, bool space_before) {
    if (text.len == 0) return;

    xml_ensure_para_open(gen);
    xml_activate_current_char_style(gen);

    if (space_before && gen->para_has_content) {
        sb_append_cstr(&gen->story, " ");
    }

    sb_append_escaped_xml(&gen->story, text.data, text.len);
    gen->para_has_content = true;
}

static void xml_emit_page_break(XmlGenerator *gen) {
    xml_close_para(gen);
    sb_append_cstr(&gen->story, "      <break type=\"page\"/>\n");
}

static void xml_gen_finalize(XmlGenerator *gen) {
    xml_close_para(gen);

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
        fprintf(gen->file, "\" underline=\"%s\"/>\n", cs->underline ? "true" : "false");
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
}

// ============================================================================
// 9. Implementación del Lexer
// ============================================================================

static Lexer lexer_init(const char *source, size_t len) {
    return (Lexer){ .src = source, .cursor = 0, .len = len, .line = 1, .col = 1 };
}

static char lexer_peek(const Lexer *l) {
    return (l->cursor >= l->len) ? '\0' : l->src[l->cursor];
}

static char lexer_advance(Lexer *l) {
    if (l->cursor >= l->len) return '\0';
    char c = l->src[l->cursor++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    return c;
}

static Token lexer_next_token(Lexer *l) {
    bool skipped_space = false;

    while (l->cursor < l->len) {
        char c = lexer_peek(l);

        if (c == '%') {
            while (l->cursor < l->len && lexer_peek(l) != '\n') {
                lexer_advance(l);
            }
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r') {
            skipped_space = true;
            lexer_advance(l);
            continue;
        }

        size_t line = l->line;
        size_t col  = l->col;
        const char *start = &l->src[l->cursor];

        if (c == '\n') {
            lexer_advance(l);
            return (Token){ .type = TOKEN_NEWLINE, .text = { start, 1 }, .line = line, .col = col, .space_before = skipped_space };
        }
        if (c == '{') {
            lexer_advance(l);
            return (Token){ .type = TOKEN_LBRACE,  .text = { start, 1 }, .line = line, .col = col, .space_before = skipped_space };
        }
        if (c == '}') {
            lexer_advance(l);
            return (Token){ .type = TOKEN_RBRACE,  .text = { start, 1 }, .line = line, .col = col, .space_before = skipped_space };
        }

        if (c == '\\') {
            lexer_advance(l);
            while (isalpha((unsigned char)lexer_peek(l)) || lexer_peek(l) == '_') {
                lexer_advance(l);
            }
            return (Token){
                .type = TOKEN_COMMAND,
                .text = { start + 1, (size_t)(&l->src[l->cursor] - (start + 1)) },
                .line = line,
                .col  = col,
                .space_before = skipped_space
            };
        }

        while (l->cursor < l->len) {
            char next = lexer_peek(l);
            if (next == '\\' || next == '{' || next == '}' || next == '%' ||
                next == '\n' || next == ' ' || next == '\t' || next == '\r') {
                break;
            }
            lexer_advance(l);
        }

        return (Token){
            .type = TOKEN_TEXT,
            .text = { start, (size_t)(&l->src[l->cursor] - start) },
            .line = line,
            .col  = col,
            .space_before = skipped_space
        };
    }

    return (Token){
        .type = TOKEN_EOF,
        .text = { &l->src[l->cursor], 0 },
        .line = l->line,
        .col  = l->col,
        .space_before = skipped_space
    };
}

// ============================================================================
// 10. Entorno y Expansión de Macros de Usuario
// ============================================================================

static void macro_env_init(MacroEnv *env, FILE *aux_file) {
    env->count    = 0;
    env->aux_file = aux_file;
}

static void macro_env_free(MacroEnv *env) {
    for (size_t i = 0; i < env->count; i++) {
        free(env->items[i].template_body);
    }
    env->count = 0;
}

static bool macro_env_register(MacroEnv *env, StrView name, size_t params, StrView body) {
    if (env->count >= MAX_MACROS || name.len >= sizeof(env->items[0].name)) return false;

    char *body_copy = malloc(body.len + 1);
    if (!body_copy) return false;
    memcpy(body_copy, body.data, body.len);
    body_copy[body.len] = '\0';

    UserMacro *m = &env->items[env->count++];
    snprintf(m->name, sizeof(m->name), "%.*s", (int)name.len, name.data);
    m->param_count   = params;
    m->template_body = body_copy;

    if (env->aux_file) {
        fprintf(env->aux_file, "\\newcommand{%s}{%zu}{%s}\n", m->name, m->param_count, m->template_body);
        fflush(env->aux_file);
    }
    return true;
}

static const UserMacro *macro_env_find(const MacroEnv *env, StrView name) {
    for (size_t i = 0; i < env->count; i++) {
        if (sv_equals(name, env->items[i].name)) return &env->items[i];
    }
    return nullptr;
}


static void expand_user_macro(Parser *p, const UserMacro *m, StrView args[]) {
    if (p->macro_depth >= MAX_MACRO_DEPTH) {
        fprintf(stderr, "Error: recursión excesiva expandiendo macro '\\%s'\n", m->name);
        return;
    }

    StrBuf expanded;
    if (!sb_init(&expanded, 256)) return;

    const char *ptr = m->template_body;
    while (*ptr != '\0') {
        if (*ptr == '#' && isdigit((unsigned char)*(ptr + 1))) {
            size_t idx = (size_t)(*(ptr + 1) - '1');
            if (idx < m->param_count) {
                sb_append_mem(&expanded, args[idx].data, args[idx].len);
            }
            ptr += 2;
        } else {
            sb_append_mem(&expanded, ptr, 1);
            ptr++;
        }
    }

    bool saved_space = p->cmd_space_before;

    Parser sub = {
        .lexer            = lexer_init(expanded.data, expanded.len),
        .env              = p->env,
        .gen              = p->gen,
        .macro_depth      = p->macro_depth + 1,
        .cmd_space_before = false
    };

    parser_advance(&sub);

    // Si el comando invocador tenía un espacio antes, se lo inyectamos al primer token del sub-parser si es texto
    if (saved_space) {
        sub.current.space_before = true;
    }

    parse_and_execute(&sub);

    sb_free(&expanded);
}


// ============================================================================
// 11. Manejadores de Comandos Nativos y Mutadores de Estado
// ============================================================================

static void builtin_newcommand(Parser *p, StrView args[]) {
    size_t params = sv_to_size(args[1]);
    if (params > MAX_PARAMS) params = MAX_PARAMS;
    macro_env_register(p->env, args[0], params, args[2]);
}

static void builtin_include(Parser *p, StrView args[]) {
    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "%.*s", (int)args[0].len, args[0].data);

    if (!has_tks_extension(path_buf)) {
        if (strlen(path_buf) + 4 < sizeof(path_buf)) {
            strcat(path_buf, ".tks");
        } else {
            fprintf(stderr, "Error: ruta demasiado larga '%s'\n", path_buf);
            return;
        }
    }

    if (!process_file(path_buf, p->env, p->gen, p->macro_depth)) {
        fprintf(stderr, "Error: fallo al incluir '%s'\n", path_buf);
    }
}

static void builtin_pagesize(Parser *p, StrView args[]) {
    p->gen->page_width  = measure_parse(args[0], UNIT_MM);
    p->gen->page_height = measure_parse(args[1], UNIT_MM);
    p->gen->orientation = (p->gen->page_width.mm > p->gen->page_height.mm)
                          ? ORIENT_LANDSCAPE
                          : ORIENT_PORTRAIT;
}

static void builtin_orientation(Parser *p, StrView args[]) {
    if (sv_equals(args[0], "landscape") || sv_equals(args[0], "horizontal")) {
        p->gen->orientation = ORIENT_LANDSCAPE;
    } else if (sv_equals(args[0], "portrait") || sv_equals(args[0], "vertical")) {
        p->gen->orientation = ORIENT_PORTRAIT;
    } else {
        fprintf(stderr, "Aviso: orientación desconocida '%.*s'\n", (int)args[0].len, args[0].data);
        return;
    }
    sync_page_dimensions_to_orientation(p->gen);
}

static void builtin_margins(Parser *p, StrView args[]) {
    p->gen->margin_top    = measure_parse(args[0], UNIT_MM);
    p->gen->margin_bottom = measure_parse(args[1], UNIT_MM);
    p->gen->margin_left   = measure_parse(args[2], UNIT_MM);
    p->gen->margin_right  = measure_parse(args[3], UNIT_MM);
}

static void builtin_newpage(Parser *p, [[maybe_unused]] StrView args[]) {
    if (p->gen->inline_depth > 0) {
        fprintf(stderr, "Aviso (línea %zu): \\newpage ignorado en contexto en línea\n", p->current.line);
        return;
    }
    if (p->gen->page_count < MAX_PAGES) {
        p->gen->page_count++;
    }
    xml_emit_page_break(p->gen);
}

static void builtin_newframe(Parser *p, StrView args[]) {
    if (p->gen->frame_count >= MAX_FRAMES) return;

    size_t idx = p->gen->frame_count++;
    p->gen->frames[idx] = (LayoutFrame){
        .page_num = p->gen->page_count - 1,
        .x        = measure_parse(args[0], UNIT_MM),
        .y        = measure_parse(args[1], UNIT_MM),
        .width    = measure_parse(args[2], UNIT_MM),
        .height   = measure_parse(args[3], UNIT_MM)
    };
}

static void prepare_para_state_mutation(XmlGenerator *gen) {
    if (gen->para_open && gen->para_has_content && gen->inline_depth == 0) {
        xml_close_para(gen);
    }
}

static void builtin_align(Parser *p, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    if (sv_equals(args[0], "left"))         p->gen->para_state.align = ALIGN_LEFT;
    else if (sv_equals(args[0], "center"))  p->gen->para_state.align = ALIGN_CENTER;
    else if (sv_equals(args[0], "right"))   p->gen->para_state.align = ALIGN_RIGHT;
    else if (sv_equals(args[0], "justify")) p->gen->para_state.align = ALIGN_JUSTIFY;
}

static void builtin_linespacing(Parser *p, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.line_spacing = measure_parse(args[0], UNIT_MM);
}

static void builtin_parindent(Parser *p, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.indent_first = measure_parse(args[0], UNIT_MM);
}

static void builtin_parmargins(Parser *p, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.indent_left  = measure_parse(args[0], UNIT_MM);
    p->gen->para_state.indent_right = measure_parse(args[1], UNIT_MM);
}

static void builtin_parspacing(Parser *p, StrView args[]) {
    prepare_para_state_mutation(p->gen);
    p->gen->para_state.space_before = measure_parse(args[0], UNIT_MM);
    p->gen->para_state.space_after  = measure_parse(args[1], UNIT_MM);
}

static void builtin_parbreak(Parser *p, [[maybe_unused]] StrView args[]) {
    if (p->gen->inline_depth > 0) return;
    xml_close_para(p->gen);
}

static void builtin_font(Parser *p, StrView args[]) {
    snprintf(p->gen->char_state.font_family, sizeof(p->gen->char_state.font_family),
             "%.*s", (int)args[0].len, args[0].data);
}

static void builtin_fontsize(Parser *p, StrView args[]) {
    p->gen->char_state.size = measure_parse(args[0], UNIT_PT);
}

static void builtin_fontcolor(Parser *p, StrView args[]) {
    snprintf(p->gen->char_state.color, sizeof(p->gen->char_state.color),
             "%.*s", (int)args[0].len, args[0].data);
}

static void builtin_fontweight(Parser *p, StrView args[]) {
    if (sv_equals(args[0], "bold"))        p->gen->char_state.weight = WEIGHT_BOLD;
    else if (sv_equals(args[0], "normal")) p->gen->char_state.weight = WEIGHT_NORMAL;
}

static void builtin_fontslant(Parser *p, StrView args[]) {
    if (sv_equals(args[0], "italic"))      p->gen->char_state.slant = SLANT_ITALIC;
    else if (sv_equals(args[0], "normal")) p->gen->char_state.slant = SLANT_NORMAL;
}

static void parse_inline_scoped(Parser *p, StrView fragment, CharState modified_state) {
    xml_ensure_para_open(p->gen);

    if (p->cmd_space_before && p->gen->para_has_content) {
        xml_activate_current_char_style(p->gen);
        sb_append_cstr(&p->gen->story, " ");
        p->cmd_space_before = false;
    }

    CharState saved_state = p->gen->char_state;
    p->gen->char_state    = modified_state;
    p->gen->inline_depth++;

    Parser sub = {
        .lexer       = lexer_init(fragment.data, fragment.len),
        .env         = p->env,
        .gen         = p->gen,
        .macro_depth = p->macro_depth
    };
    parser_advance(&sub);

    // Si el comando en línea tenía espacio antes, se lo pasamos al primer token del fragmento
    if (p->cmd_space_before) {
        sub.current.space_before = true;
        p->cmd_space_before = false;
    }

    parse_and_execute(&sub);

    p->gen->inline_depth--;
    p->gen->char_state = saved_state;
}

static void builtin_textbf(Parser *p, StrView args[]) {
    CharState next = p->gen->char_state;
    next.weight    = WEIGHT_BOLD;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_textit(Parser *p, StrView args[]) {
    CharState next = p->gen->char_state;
    next.slant     = SLANT_ITALIC;
    parse_inline_scoped(p, args[0], next);
}

static void builtin_underline(Parser *p, StrView args[]) {
    CharState next = p->gen->char_state;
    next.underline = true;
    parse_inline_scoped(p, args[0], next);
}

static const BuiltinCommand *find_builtin(StrView name) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (sv_equals(name, BUILTIN_LIST[i].name)) return &BUILTIN_LIST[i];
    }
    return nullptr;
}

// ============================================================================
// 12. Parser y Ejecución
// ============================================================================

static void parser_advance(Parser *p) {
    p->current = lexer_next_token(&p->lexer);
}

static bool parse_arg(Parser *p, StrView *out_arg) {
    while (p->current.type == TOKEN_NEWLINE) {
        parser_advance(p);
    }

    if (p->current.type != TOKEN_LBRACE) {
        fprintf(stderr, "Error sintáctico: se esperaba '{' en línea %zu, col %zu\n",
                p->current.line, p->current.col);
        return false;
    }

    size_t open_line  = p->current.line;
    const char *start = p->current.text.data + 1;
    int depth = 1;

    while (p->lexer.cursor < p->lexer.len && depth > 0) {
        char c = lexer_advance(&p->lexer);
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

static void parse_and_execute(Parser *p) {
    while (p->current.type != TOKEN_EOF) {
        if (p->current.type == TOKEN_TEXT) {
            xml_emit_text(p->gen, p->current.text, p->current.space_before);
            parser_advance(p);
            continue;
        }

        if (p->current.type == TOKEN_NEWLINE) {
            if (p->gen->inline_depth > 0) {
                parser_advance(p);
                p->current.space_before = true;
            } else {
                xml_close_para(p->gen);
                parser_advance(p);
            }
            continue;
        }
        if (p->current.type == TOKEN_COMMAND) {
            StrView cmd = p->current.text;
            bool space_pend = p->current.space_before; // Capturar el espacio antes de avanzar
            parser_advance(p);

            const BuiltinCommand *builtin = find_builtin(cmd);
            if (builtin) {
                p->cmd_space_before = space_pend; // Pasar al contexto
                StrView args[MAX_PARAMS];
                bool ok = true;
                for (size_t i = 0; i < builtin->arity; i++) {
                    if (!parse_arg(p, &args[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) builtin->handler(p, args);
                p->cmd_space_before = false;
                continue;
            }

            const UserMacro *macro = macro_env_find(p->env, cmd);
            if (macro) {
                p->cmd_space_before = space_pend; // Pasar al contexto de la macro
                StrView args[MAX_PARAMS];
                bool ok = true;
                for (size_t i = 0; i < macro->param_count; i++) {
                    if (!parse_arg(p, &args[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) expand_user_macro(p, macro, args);
                p->cmd_space_before = false;
                continue;
            }

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
    if (!has_tks_extension(path)) {
        fprintf(stderr, "Error: '%s' no tiene extensión .tks\n", path);
        return false;
    }

    size_t len = 0;
    char *source = read_entire_file(path, &len);
    if (!source) {
        fprintf(stderr, "Error: no se pudo leer '%s'\n", path);
        return false;
    }

    Parser p = {
        .lexer       = lexer_init(source, len),
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
// 13. Función Principal (main)
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <fichero_de_entrada.tks>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *input_path = argv[1];
    if (!has_tks_extension(input_path)) {
        fprintf(stderr, "Error: el fichero debe tener extensión .tks (recibido: '%s')\n", input_path);
        return EXIT_FAILURE;
    }

    FILE *aux_file = fopen("comandos_auxiliares.tks", "w");
    if (!aux_file) {
        perror("Error creando fichero auxiliar");
        return EXIT_FAILURE;
    }

    MacroEnv user_macros;
    macro_env_init(&user_macros, aux_file);

    char xml_output_path[256];
    replace_extension(input_path, ".xml", xml_output_path, sizeof(xml_output_path));

    XmlGenerator gen;
    if (!xml_gen_init(&gen, xml_output_path)) {
        perror("Error inicializando generador XML");
        fclose(aux_file);
        macro_env_free(&user_macros);
        return EXIT_FAILURE;
    }

    printf("Procesando '%s' -> Generando '%s'...\n", input_path, xml_output_path);

    if (!process_file(input_path, &user_macros, &gen, 0)) {
        fprintf(stderr, "Fallo durante el procesamiento del documento\n");
        xml_gen_finalize(&gen);
        fclose(aux_file);
        macro_env_free(&user_macros);
        return EXIT_FAILURE;
    }

    xml_gen_finalize(&gen);
    fclose(aux_file);
    macro_env_free(&user_macros);

    printf("Completado con éxito. Archivos generados: '%s' y 'comandos_auxiliares.tks'\n", xml_output_path);
    return EXIT_SUCCESS;
}
