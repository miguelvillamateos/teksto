#ifndef TEX_SYMBOLS_H
#define TEX_SYMBOLS_H

#include "tex_types.h"

// ============================================================================
// Catálogo de Símbolos Especiales, Acentos y Logos de LaTeX2e (Cap. 23)
// ============================================================================

typedef struct {
    const char *cmd;
    const char *utf8;
} SymbolEntry;

static const SymbolEntry LATEX_TEXT_SYMBOLS[] = {
    // Caracteres reservados escapados (Cap. 23.1)
    { "#",                   "#" },
    { "$",                   "$" },
    { "%",                   "%" },
    { "&",                   "&" },
    { "_",                   "_" },
    { "{",                   "{" },
    { "}",                   "}" },
    { " ",                   " " },
    { "\t",                  " " },
    { "\n",                  " " },
    { ",",                   "\xE2\x80\x89" }, // thin space U+2009
    { ":",                   "\xE2\x81\x9F" }, // medium space U+205F
    { ";",                   "\xE2\x80\x84" }, // thick space U+2004
    { "!",                   "" },             // negative thin space
    { "/",                   "" },             // italic correction
    { "-",                   "" },             // discretionary hyphen

    // Letras adicionales europeas y ligaduras (Cap. 23.2 / 23.3)
    { "aa",                  "å" },
    { "AA",                  "Å" },
    { "ae",                  "æ" },
    { "AE",                  "Æ" },
    { "oe",                  "œ" },
    { "OE",                  "Œ" },
    { "o",                   "ø" },
    { "O",                   "Ø" },
    { "l",                   "ł" },
    { "L",                   "Ł" },
    { "ss",                  "ß" },
    { "SS",                  "ẞ" },
    { "i",                   "ı" },
    { "j",                   "ȷ" },
    { "ij",                  "ĳ" },
    { "IJ",                  "Ĳ" },
    { "dh",                  "ð" },
    { "DH",                  "Ð" },
    { "dj",                  "đ" },
    { "DJ",                  "Đ" },
    { "ng",                  "ŋ" },
    { "NG",                  "Ŋ" },
    { "th",                  "þ" },
    { "TH",                  "Þ" },

    // Símbolos tipográficos y de texto (Cap. 23.4)
    { "dag",                 "†" },
    { "ddag",                "‡" },
    { "textdagger",          "†" },
    { "textdaggerdbl",       "‡" },
    { "S",                   "§" },
    { "textsection",         "§" },
    { "P",                   "¶" },
    { "textparagraph",       "¶" },
    { "copyright",           "©" },
    { "textcopyright",       "©" },
    { "pounds",              "£" },
    { "textsterling",        "£" },
    { "euro",                "€" },
    { "texteuro",            "€" },
    { "dots",                "…" },
    { "ldots",               "…" },
    { "textellipsis",        "…" },
    { "textbullet",          "•" },
    { "textperiodcentered",  "·" },
    { "textbackslash",       "\\" },
    { "textbar",             "|" },
    { "textbardbl",          "‖" },
    { "textless",            "<" },
    { "textgreater",         ">" },
    { "textasciitilde",      "~" },
    { "textasciicircum",     "^" },
    { "textregistered",      "®" },
    { "texttrademark",       "™" },
    { "textordfeminine",     "ª" },
    { "textordmasculine",    "º" },
    { "textquestiondown",    "¿" },
    { "textexclamdown",      "¡" },
    { "textquotedblleft",    "“" },
    { "textquotedblright",   "”" },
    { "textquoteleft",       "‘" },
    { "textquoteright",      "’" },
    { "lq",                  "‘" },
    { "rq",                  "’" },
    { "guillemotleft",       "«" },
    { "guillemotright",      "»" },
    { "guilsinglleft",       "‹" },
    { "guilsinglright",      "›" },
    { "quotedblbase",        "„" },
    { "quotesinglbase",      "‚" },
    { "textvisiblespace",    "␣" },
    { "textemdash",          "—" },
    { "textendash",          "–" },
    { "textasteriskcentered","∗" },
    { "textdegree",          "°" },
    { "textcent",            "¢" },
    { "textyen",             "¥" },
    { "textbrokenbar",       "¦" },
    { "textonequarter",      "¼" },
    { "textonehalf",         "½" },
    { "textthreequarters",   "¾" },
    { "texttimes",           "×" },
    { "textdiv",             "÷" },
    { "textpm",              "±" },
    { "quad",                "\xE2\x80\x83" },         // em-quad U+2003
    { "qquad",               "\xE2\x80\x83\xE2\x80\x83" },
    { "enspace",             "\xE2\x80\x82" },         // en-space U+2002
    { "thinspace",           "\xE2\x80\x89" },         // thin space U+2009
    { "negthinspace",        "" },
    { "space",               " " },

    // Logos (Cap. 23.6)
    { "TeX",                 "TeX" },
    { "LaTeX",               "LaTeX" },
    { "LaTeXe",              "LaTeX2ε" },
};
constexpr size_t LATEX_TEXT_SYMBOL_COUNT = sizeof(LATEX_TEXT_SYMBOLS) / sizeof(LATEX_TEXT_SYMBOLS[0]);

typedef struct {
    char accent;
    char base;
    const char *utf8;
} AccentPair;

static const AccentPair PRECOMPOSED_ACCENTS[] = {
    // Agudo \'
    { '\'', 'a', "á" }, { '\'', 'e', "é" }, { '\'', 'i', "í" }, { '\'', 'o', "ó" }, { '\'', 'u', "ú" }, { '\'', 'y', "ý" },
    { '\'', 'A', "Á" }, { '\'', 'E', "É" }, { '\'', 'I', "Í" }, { '\'', 'O', "Ó" }, { '\'', 'U', "Ú" }, { '\'', 'Y', "Ý" },
    { '\'', 'c', "ć" }, { '\'', 'C', "Ć" }, { '\'', 'n', "ń" }, { '\'', 'N', "Ń" }, { '\'', 's', "ś" }, { '\'', 'S', "Ś" },
    { '\'', 'z', "ź" }, { '\'', 'Z', "Ź" }, { '\'', 'l', "ĺ" }, { '\'', 'L', "Ĺ" }, { '\'', 'r', "ŕ" }, { '\'', 'R', "Ŕ" },

    // Grave \`
    { '`',  'a', "à" }, { '`',  'e', "è" }, { '`',  'i', "ì" }, { '`',  'o', "ò" }, { '`',  'u', "ù" },
    { '`',  'A', "À" }, { '`',  'E', "È" }, { '`',  'I', "Ì" }, { '`',  'O', "Ò" }, { '`',  'U', "Ù" },

    // Circunflejo \^
    { '^',  'a', "â" }, { '^',  'e', "ê" }, { '^',  'i', "î" }, { '^',  'o', "ô" }, { '^',  'u', "û" }, { '^',  'y', "ŷ" },
    { '^',  'A', "Â" }, { '^',  'E', "Ê" }, { '^',  'I', "Î" }, { '^',  'O', "Ô" }, { '^',  'U', "Û" }, { '^',  'Y', "Ŷ" },
    { '^',  'c', "ĉ" }, { '^',  'C', "Ĉ" }, { '^',  'g', "ĝ" }, { '^',  'G', "Ĝ" }, { '^',  'h', "ĥ" }, { '^',  'H', "Ĥ" },
    { '^',  'j', "ĵ" }, { '^',  'J', "Ĵ" }, { '^',  's', "ŝ" }, { '^',  'S', "Ŝ" }, { '^',  'w', "ŵ" }, { '^',  'W', "Ŵ" },

    // Diéresis \"
    { '"',  'a', "ä" }, { '"',  'e', "ë" }, { '"',  'i', "ï" }, { '"',  'o', "ö" }, { '"',  'u', "ü" }, { '"',  'y', "ÿ" },
    { '"',  'A', "Ä" }, { '"',  'E', "Ë" }, { '"',  'I', "Ï" }, { '"',  'O', "Ö" }, { '"',  'U', "Ü" }, { '"',  'Y', "Ÿ" },

    // Virgulilla \~
    { '~',  'a', "ã" }, { '~',  'n', "ñ" }, { '~',  'o', "õ" }, { '~',  'i', "ĩ" }, { '~',  'u', "ũ" },
    { '~',  'A', "Ã" }, { '~',  'N', "Ñ" }, { '~',  'O', "Õ" }, { '~',  'I', "Ĩ" }, { '~',  'U', "Ũ" },

    // Macrón \=
    { '=',  'a', "ā" }, { '=',  'e', "ē" }, { '=',  'i', "ī" }, { '=',  'o', "ō" }, { '=',  'u', "ū" },
    { '=',  'A', "Ā" }, { '=',  'E', "Ē" }, { '=',  'I', "Ī" }, { '=',  'O', "Ō" }, { '=',  'U', "Ū" },

    // Punto superior \.
    { '.',  'c', "ċ" }, { '.',  'C', "Ċ" }, { '.',  'e', "ė" }, { '.',  'E', "Ė" }, { '.',  'g', "ġ" }, { '.',  'G', "Ġ" },
    { '.',  'I', "İ" }, { '.',  'z', "ż" }, { '.',  'Z', "Ż" },

    // Breve \u
    { 'u',  'a', "ă" }, { 'u',  'A', "Ă" }, { 'u',  'e', "ĕ" }, { 'u',  'E', "Ĕ" }, { 'u',  'g', "ğ" }, { 'u',  'G', "Ğ" },
    { 'u',  'i', "ĭ" }, { 'u',  'I', "Ĭ" }, { 'u',  'o', "ŏ" }, { 'u',  'O', "Ŏ" }, { 'u',  'u', "ŭ" }, { 'u',  'U', "Ŭ" },

    // Carón / háček \v
    { 'v',  'c', "č" }, { 'v',  'C', "Č" }, { 'v',  'd', "ď" }, { 'v',  'D', "Ď" }, { 'v',  'e', "ě" }, { 'v',  'E', "Ě" },
    { 'v',  'n', "ň" }, { 'v',  'N', "Ň" }, { 'v',  'r', "ř" }, { 'v',  'R', "Ř" }, { 'v',  's', "š" }, { 'v',  'S', "Š" },
    { 'v',  't', "ť" }, { 'v',  'T', "Ť" }, { 'v',  'z', "ž" }, { 'v',  'Z', "Ž" },

    // Doble agudo \H
    { 'H',  'o', "ő" }, { 'H',  'O', "Ő" }, { 'H',  'u', "ű" }, { 'H',  'U', "Ű" },

    // Cedilla \c
    { 'c',  'c', "ç" }, { 'c',  'C', "Ç" }, { 'c',  's', "ş" }, { 'c',  'S', "Ş" }, { 'c',  't', "ţ" }, { 'c',  'T', "Ţ" },
    { 'c',  'g', "ģ" }, { 'c',  'G', "Ģ" }, { 'c',  'k', "ķ" }, { 'c',  'K', "Ķ" }, { 'c',  'l', "ļ" }, { 'c',  'L', "Ļ" },
    { 'c',  'n', "ņ" }, { 'c',  'N', "Ņ" }, { 'c',  'r', "ŗ" }, { 'c',  'R', "Ŗ" },

    // Ogonek \k
    { 'k',  'a', "ą" }, { 'k',  'A', "Ą" }, { 'k',  'e', "ę" }, { 'k',  'E', "Ę" }, { 'k',  'i', "į" }, { 'k',  'I', "Į" },
    { 'k',  'u', "ų" }, { 'k',  'U', "Ų" },

    // Anillo \r
    { 'r',  'a', "å" }, { 'r',  'A', "Å" }, { 'r',  'u', "ů" }, { 'r',  'U', "Ů" },
};
constexpr size_t PRECOMPOSED_ACCENT_COUNT = sizeof(PRECOMPOSED_ACCENTS) / sizeof(PRECOMPOSED_ACCENTS[0]);

static inline bool is_accent_command(StrView cmd, char *out_accent_char) {
    if (cmd.len != 1) return false;
    char c = cmd.data[0];
    switch (c) {
        case '\'': case '`': case '^': case '"': case '~':
        case '=':  case '.': case 'u': case 'v': case 'H':
        case 'c':  case 'k': case 'r': case 'b': case 'd':
        case 't':
            *out_accent_char = c;
            return true;
        default:
            return false;
    }
}

static inline const char *combining_mark_for_accent(char accent) {
    switch (accent) {
        case '`':  return "\xCC\x80"; // U+0300
        case '\'': return "\xCC\x81"; // U+0301
        case '^':  return "\xCC\x82"; // U+0302
        case '~':  return "\xCC\x83"; // U+0303
        case '=':  return "\xCC\x84"; // U+0304
        case 'u':  return "\xCC\x86"; // U+0306
        case '.':  return "\xCC\x87"; // U+0307
        case '"':  return "\xCC\x88"; // U+0308
        case 'r':  return "\xCC\x8A"; // U+030A
        case 'H':  return "\xCC\x8B"; // U+030B
        case 'v':  return "\xCC\x8C"; // U+030C
        case 'd':  return "\xCC\xA3"; // U+0323
        case 'c':  return "\xCC\xA7"; // U+0327
        case 'k':  return "\xCC\xA8"; // U+0328
        case 'b':  return "\xCC\xB1"; // U+0331
        case 't':  return "\xCD\xA1"; // U+0361
        default:   return "";
    }
}

static inline void compose_latex_accent(char accent, StrView arg, char *out_buf, size_t buf_size) {
    // Limpiar espacios y traducir \i -> i, \j -> j
    while (arg.len > 0 && isspace((unsigned char)arg.data[0])) {
        arg.data++;
        arg.len--;
    }
    while (arg.len > 0 && isspace((unsigned char)arg.data[arg.len - 1])) {
        arg.len--;
    }

    char base = '\0';
    if (arg.len == 1) {
        base = arg.data[0];
    } else if (arg.len == 2 && arg.data[0] == '\\' && (arg.data[1] == 'i' || arg.data[1] == 'j')) {
        base = arg.data[1];
    }

    if (base != '\0') {
        for (size_t i = 0; i < PRECOMPOSED_ACCENT_COUNT; i++) {
            if (PRECOMPOSED_ACCENTS[i].accent == accent && PRECOMPOSED_ACCENTS[i].base == base) {
                snprintf(out_buf, buf_size, "%s", PRECOMPOSED_ACCENTS[i].utf8);
                return;
            }
        }
        snprintf(out_buf, buf_size, "%c%s", base, combining_mark_for_accent(accent));
        return;
    }

    if (arg.len == 0) {
        // Acento aislado (\~{}, \^{})
        if (accent == '~')      snprintf(out_buf, buf_size, "~");
        else if (accent == '^') snprintf(out_buf, buf_size, "^");
        else                    snprintf(out_buf, buf_size, "%s", combining_mark_for_accent(accent));
        return;
    }

    snprintf(out_buf, buf_size, "%.*s%s", (int)arg.len, arg.data, combining_mark_for_accent(accent));
}

static inline const char *find_latex_text_symbol(StrView name) {
    for (size_t i = 0; i < LATEX_TEXT_SYMBOL_COUNT; i++) {
        size_t len = strlen(LATEX_TEXT_SYMBOLS[i].cmd);
        if (name.len == len && strncmp(name.data, LATEX_TEXT_SYMBOLS[i].cmd, len) == 0) {
            return LATEX_TEXT_SYMBOLS[i].utf8;
        }
    }
    return nullptr;
}

#endif // TEX_SYMBOLS_H

