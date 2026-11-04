#ifndef TEX_LEXER_H
#define TEX_LEXER_H

#include "tex_types.h"

// ============================================================================
// Implementación de TeX (Lexer, Catcodes, Comentarios y Ligaduras)
// ============================================================================

static inline Lexer lexer_init(const char *source, size_t len, bool allow_at) {
  return (Lexer){.src = source,
                 .cursor = 0,
                 .len = len,
                 .line = 1,
                 .col = 1,
                 .allow_at = allow_at};
}

static inline char lexer_peek(const Lexer *l) {
  return (l->cursor >= l->len) ? '\0' : l->src[l->cursor];
}

static inline char lexer_peek_ahead(const Lexer *l, size_t offset) {
  return (l->cursor + offset >= l->len) ? '\0' : l->src[l->cursor + offset];
}

static inline char lexer_advance(Lexer *l) {
  if (l->cursor >= l->len)
    return '\0';
  char c = l->src[l->cursor++];
  if (c == '\n') {
    l->line++;
    l->col = 1;
  } else {
    l->col++;
  }
  return c;
}

static inline bool is_cmd_letter(const Lexer *l, char c) {
  return isalpha((unsigned char)c) || (l->allow_at && c == '@');
}

// Comprueba si a partir del cursor actual (que apunta tras un '\n') hay una
// línea en blanco
static inline bool has_blank_line_ahead(const Lexer *l) {
  size_t pos = l->cursor;
  while (pos < l->len) {
    char c = l->src[pos];
    if (c == ' ' || c == '\t' || c == '\r') {
      pos++;
    } else if (c == '\n') {
      return true;
    } else {
      return false;
    }
  }
  return false;
}

static inline Token lexer_next_token(Lexer *l) {
  bool skipped_space = false;

  while (l->cursor < l->len) {
    char c = lexer_peek(l);

    // Comentario TeX '%': ignora hasta fin de línea inclusive Y los espacios al
    // inicio de la siguiente línea
    if (c == '%') {
      while (l->cursor < l->len && lexer_peek(l) != '\n') {
        lexer_advance(l);
      }
      if (l->cursor < l->len && lexer_peek(l) == '\n') {
        lexer_advance(l);
        while (l->cursor < l->len) {
          char sp = lexer_peek(l);
          if (sp == ' ' || sp == '\t' || sp == '\r') {
            lexer_advance(l);
          } else {
            break;
          }
        }
      }
      continue;
    }

    // Espacios horizontales
    if (c == ' ' || c == '\t' || c == '\r') {
      skipped_space = true;
      lexer_advance(l);
      continue;
    }

    // Salto de línea: simple = espacio; doble (línea en blanco) = \par
    if (c == '\n') {
      size_t line = l->line;
      size_t col = l->col;
      const char *start = &l->src[l->cursor];
      lexer_advance(l);

      if (has_blank_line_ahead(l)) {
        while (l->cursor < l->len) {
          char next = lexer_peek(l);
          if (next == ' ' || next == '\t' || next == '\r' || next == '\n') {
            lexer_advance(l);
          } else {
            break;
          }
        }
        return (Token){.type = TOKEN_PAR,
                       .text = {start, 1},
                       .line = line,
                       .col = col,
                       .space_before = false};
      }

      skipped_space = true;
      continue;
    }

    size_t line = l->line;
    size_t col = l->col;
    const char *start = &l->src[l->cursor];

    // Agrupamiento TeX '{' y '}'
    if (c == '{') {
      lexer_advance(l);
      return (Token){.type = TOKEN_LBRACE,
                     .text = {start, 1},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }
    if (c == '}') {
      lexer_advance(l);
      return (Token){.type = TOKEN_RBRACE,
                     .text = {start, 1},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }

    // Carácter activo '~' (espacio irrompible en TeX)
    if (c == '~') {
      lexer_advance(l);
      return (Token){.type = TOKEN_TEXT,
                     .text = {"\xC2\xA0", 2},
                     .line = line,
                     .col = col,
                     .space_before = false};
    }

    // Ligaduras TeX: ---, --, ``, '', ?`, !`
    if (c == '-' && lexer_peek_ahead(l, 1) == '-') {
      if (lexer_peek_ahead(l, 2) == '-') {
        lexer_advance(l);
        lexer_advance(l);
        lexer_advance(l);
        return (Token){.type = TOKEN_TEXT,
                       .text = {"\xE2\x80\x94", 3},
                       .line = line,
                       .col = col,
                       .space_before = skipped_space};
      }
      lexer_advance(l);
      lexer_advance(l);
      return (Token){.type = TOKEN_TEXT,
                     .text = {"\xE2\x80\x93", 3},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }
    if (c == '`' && lexer_peek_ahead(l, 1) == '`') {
      lexer_advance(l);
      lexer_advance(l);
      return (Token){.type = TOKEN_TEXT,
                     .text = {"\xE2\x80\x9C", 3},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }
    if (c == '\'' && lexer_peek_ahead(l, 1) == '\'') {
      lexer_advance(l);
      lexer_advance(l);
      return (Token){.type = TOKEN_TEXT,
                     .text = {"\xE2\x80\x9D", 3},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }
    if (c == '?' && lexer_peek_ahead(l, 1) == '`') {
      lexer_advance(l);
      lexer_advance(l);
      return (Token){.type = TOKEN_TEXT,
                     .text = {"\xC2\xBF", 2},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }
    if (c == '!' && lexer_peek_ahead(l, 1) == '`') {
      lexer_advance(l);
      lexer_advance(l);
      return (Token){.type = TOKEN_TEXT,
                     .text = {"\xC2\xA1", 2},
                     .line = line,
                     .col = col,
                     .space_before = skipped_space};
    }

    // Secuencias de control: \palabra o \símbolo
    if (c == '\\') {
      lexer_advance(l);
      if (l->cursor >= l->len) {
        return (Token){.type = TOKEN_COMMAND,
                       .text = {start + 1, 0},
                       .line = line,
                       .col = col,
                       .space_before = skipped_space};
      }

      char first = lexer_peek(l);
      if (is_cmd_letter(l, first)) {
        // Palabra de control (\section, \textbf, \LaTeX, etc.)
        while (l->cursor < l->len && is_cmd_letter(l, lexer_peek(l))) {
          lexer_advance(l);
        }
        StrView cmd_name = {start + 1,
                            (size_t)(&l->src[l->cursor] - (start + 1))};

        // En TeX, las palabras de control absorben los espacios horizontales
        // posteriores (o un único salto de línea si no inicia línea en blanco
        // \par)
        while (l->cursor < l->len) {
          char sp = lexer_peek(l);
          if (sp == ' ' || sp == '\t' || sp == '\r') {
            lexer_advance(l);
          } else if (sp == '\n' && !has_blank_line_ahead(l)) {
            // Comprobar si después de este '\n' hay una línea en blanco
            size_t save_cur = l->cursor;
            size_t save_ln = l->line;
            size_t save_col = l->col;
            lexer_advance(l);
            if (has_blank_line_ahead(l)) {
              // Es un \par: devolver el cursor al '\n' para que el próximo
              // token sea TOKEN_PAR
              l->cursor = save_cur;
              l->line = save_ln;
              l->col = save_col;
              break;
            }
            // Un único '\n' tras palabra de control se absorbe junto con
            // espacios de la línea siguiente
            while (l->cursor < l->len &&
                   (lexer_peek(l) == ' ' || lexer_peek(l) == '\t' ||
                    lexer_peek(l) == '\r')) {
              lexer_advance(l);
            }
            break;
          } else {
            break;
          }
        }

        return (Token){.type = TOKEN_COMMAND,
                       .text = cmd_name,
                       .line = line,
                       .col = col,
                       .space_before = skipped_space,
                       .is_control_symbol = false};
      } else {
        // Símbolo de control de 1 carácter (\\, \%, \$, \&, \#, \_, \{, \}, \',
        // \`, \^, \", \~)
        lexer_advance(l);
        return (Token){.type = TOKEN_COMMAND,
                       .text = {start + 1, 1},
                       .line = line,
                       .col = col,
                       .space_before = skipped_space,
                       .is_control_symbol = true};
      }
    }

    // Token de texto ordinario hasta encontrar delimitador o ligadura
    while (l->cursor < l->len) {
      char next = lexer_peek(l);
      if (next == '\\' || next == '{' || next == '}' || next == '%' ||
          next == '~' || next == '\n' || next == ' ' || next == '\t' ||
          next == '\r') {
        break;
      }
      if ((next == '-' && lexer_peek_ahead(l, 1) == '-') ||
          (next == '`' && lexer_peek_ahead(l, 1) == '`') ||
          (next == '\'' && lexer_peek_ahead(l, 1) == '\'') ||
          (next == '?' && lexer_peek_ahead(l, 1) == '`') ||
          (next == '!' && lexer_peek_ahead(l, 1) == '`')) {
        break;
      }
      lexer_advance(l);
    }

    return (Token){.type = TOKEN_TEXT,
                   .text = {start, (size_t)(&l->src[l->cursor] - start)},
                   .line = line,
                   .col = col,
                   .space_before = skipped_space};
  }

  return (Token){.type = TOKEN_EOF,
                 .text = {&l->src[l->cursor], 0},
                 .line = l->line,
                 .col = l->col,
                 .space_before = skipped_space};
}

#endif // TEX_LEXER_H
