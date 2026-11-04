#ifndef TEX_TYPES_H
#define TEX_TYPES_H

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// 1. Constantes y Límites del Sistema
// ============================================================================

constexpr size_t MAX_MACROS = 512;
constexpr size_t MAX_USER_ENVS = 128;
constexpr size_t MAX_PARAMS = 9;
constexpr size_t MAX_PAGES = 500;
constexpr size_t MAX_FRAMES = 500;
constexpr size_t MAX_MACRO_DEPTH = 48;
constexpr size_t MAX_GROUP_DEPTH = 64;
constexpr size_t MAX_ENV_DEPTH = 32;

constexpr size_t MAX_FONT_FAMS = 64;
constexpr size_t MAX_PARA_STYLES = 512;
constexpr size_t MAX_CHAR_STYLES = 1024;

constexpr size_t MAX_COUNTERS = 128;
constexpr size_t MAX_LENGTHS = 128;
constexpr size_t MAX_SAVEBOXES = 64;
constexpr size_t MAX_TAB_COLS = 32;

constexpr double MM_PER_CM = 10.0;
constexpr double MM_PER_M = 1000.0;
constexpr double MM_PER_IN = 25.4;
constexpr double MM_PER_BP = 25.4 / 72.0;
constexpr double MM_PER_PT = 25.4 / 72.27;
constexpr double MM_PER_PC = 12.0 * (25.4 / 72.27);
constexpr double MM_PER_DD = (1238.0 / 1157.0) * (25.4 / 72.27);
constexpr double MM_PER_CC = 12.0 * ((1238.0 / 1157.0) * (25.4 / 72.27));
constexpr double MM_PER_SP = (25.4 / 72.27) / 65536.0;
constexpr double MEASURE_EPSILON = 1e-4;

static const char *const DEFAULT_FONT_FAMILY = "Liberation Serif";
static const char *const DEFAULT_SANS_FAMILY = "DejaVu Sans";
static const char *const DEFAULT_MONO_FAMILY = "Liberation Mono";
static const char *const DEFAULT_COLOR = "Black";

// ============================================================================
// 2. Unidades Métricas, Orientación y Estado Tipográfico
// ============================================================================

typedef enum : unsigned char {
  UNIT_MM = 0,
  UNIT_CM,
  UNIT_M,
  UNIT_PT,
  UNIT_BP,
  UNIT_IN,
  UNIT_PC,
  UNIT_DD,
  UNIT_CC,
  UNIT_SP,
  UNIT_EM,
  UNIT_EX
} Unit;

typedef struct {
  double value; // Valor en la unidad original elegida
  double mm;    // Valor normalizado en milímetros para comparación exacta
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

typedef enum : unsigned char { WEIGHT_NORMAL = 0, WEIGHT_BOLD = 1 } FontWeight;

typedef enum : unsigned char { SLANT_NORMAL = 0, SLANT_ITALIC = 1 } FontSlant;

typedef enum : unsigned char {
  SHIFT_NORMAL = 0,
  SHIFT_SUPER = 1,
  SHIFT_SUB = 2
} BaselineShift;

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
  bool small_caps;
  BaselineShift shift;
} CharState;

typedef struct {
  FontVariantRef variant;
  Measure size;
  char color[32];
  bool underline;
  bool small_caps;
  BaselineShift shift;
} CharStyleDef;

// ============================================================================
// 3. Estructuras del Lexer, Macros, Contadores, Longitudes y Generador XML
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
  TOKEN_AMP,
  TOKEN_TEXT,
  TOKEN_PAR
} TokenType;

typedef struct {
  TokenType type;
  StrView text;
  size_t line;
  size_t col;
  bool space_before;
  bool is_control_symbol;
} Token;

typedef struct {
  const char *src;
  size_t cursor;
  size_t len;
  size_t line;
  size_t col;
  bool allow_at;
} Lexer;

typedef struct {
  char name[48];
  size_t param_count;
  bool has_opt_default;
  char *opt_default;
  char *template_body;
} UserMacro;

typedef struct {
  char name[48];
  size_t param_count;
  bool has_opt_default;
  char *opt_default;
  char *begin_body;
  char *end_body;
} UserEnv;

typedef struct {
  char name[48];
  int value;
  char resets[8][48];
  size_t reset_count;
} CounterDef;

typedef struct {
  char name[48];
  Measure value;
} LengthDef;

typedef struct {
  char name[48];
  char *content;
} SaveBoxDef;

typedef struct {
  char align;       // 'l', 'c', 'r', 'p'
  int border_left;  // 0, 1, 2
  int border_right; // 0, 1, 2
  double width_mm;  // >0 para p{width}
} TabColSpec;

typedef struct {
  UserMacro items[MAX_MACROS];
  size_t count;

  UserEnv envs[MAX_USER_ENVS];
  size_t env_count;

  CounterDef counters[MAX_COUNTERS];
  size_t counter_count;

  LengthDef lengths[MAX_LENGTHS];
  size_t length_count;

  SaveBoxDef saveboxes[MAX_SAVEBOXES];
  size_t savebox_count;

  bool allow_at;
  char current_ref[64];
  char include_only[512];
} MacroEnv;

typedef struct {
  size_t page_num;
  Measure x, y, width, height;
} LayoutFrame;

typedef struct {
  ParaState para_state;
  CharState char_state;
} GroupFrame;

typedef struct {
  char name[48];
  GroupFrame saved_state;
  char *end_expansion;
} ActiveEnvFrame;

typedef enum : unsigned char {
  LIST_NONE = 0,
  LIST_ITEMIZE,
  LIST_ENUMERATE,
  LIST_DESCRIPTION,
  LIST_CUSTOM
} ListEnvKind;

typedef struct {
  ListEnvKind kind;
  size_t level; // 1..4
  char default_label[128];
  char counter_name[48];
} ListLevelState;

typedef struct {
  FILE *file;

  // Clase de documento, opciones y metadatos
  char doc_class[32];
  double base_font_pt;
  double linespread_factor;
  bool twocolumn;
  bool twoside;
  bool in_appendix;
  int matter_mode; // 0=mainmatter, 1=frontmatter, 2=backmatter

  char title[256];
  char author[256];
  char date[128];
  char thanks[256];

  // Metadatos de clase letter
  char sender_name[128];
  char sender_address[256];
  char sender_signature[128];
  char sender_location[128];
  char sender_telephone[64];

  // Estado NFSS pendiente para \selectfont
  Measure pending_nfss_baselineskip;
  bool has_pending_nfss_baselineskip;

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
  bool next_para_noindent;

  // Pila de grupos léxicos TeX { ... } y entornos \begin{...}...\end{...}
  GroupFrame group_stack[MAX_GROUP_DEPTH];
  size_t group_depth;

  ActiveEnvFrame env_stack[MAX_ENV_DEPTH];
  size_t env_depth;

  // Estado de listas anidadas (itemize, enumerate, description, list)
  ListLevelState list_stack[8];
  size_t list_depth;
  size_t itemize_depth;
  size_t enumerate_depth;
  char pending_item_label[128];
  bool has_pending_item_label;

  // Estado de tablas (tabular / tabular*)
  bool in_tabular;
  TabColSpec tab_cols[MAX_TAB_COLS];
  size_t tab_col_count;
  size_t tab_cur_col;
  bool tab_row_open;
  bool tab_cell_open;
  size_t tab_cur_span;
  CharState tab_base_char_state;
  bool tab_has_mc_override;
  size_t tab_mc_span;
  char tab_mc_align;
  int tab_mc_bl;
  int tab_mc_br;

  // Estado de flotantes, notas al pie y estilo de página
  char active_float_kind[16];
  StrBuf pending_footnotes;
  char page_style[32];
  char this_page_style[32];
  char page_numbering[16];
  char left_mark[128];
  char right_mark[128];

  // Registros deduplicados para el bloque <definitions>
  FontFamilyDef fonts[MAX_FONT_FAMS];
  size_t font_count;

  ParaState para_styles[MAX_PARA_STYLES];
  size_t para_style_count;

  CharStyleDef char_styles[MAX_CHAR_STYLES];
  size_t char_style_count;

  // Control de emisión del flujo <body>
  StrBuf story;
  StrBuf *capture_plain_buf;
  CharState captured_char_state;
  bool has_captured_char_state;
  bool para_open;
  bool para_has_content;
  int active_char_style_id; // -1 si no hay <span> abierto
  size_t inline_depth;
} XmlGenerator;

typedef struct Parser Parser;
typedef void (*BuiltinFn)(Parser *p, bool starred, StrView opt_args[],
                          size_t opt_count, StrView args[]);

typedef struct {
  const char *name;
  size_t max_opt_args;
  size_t arity;
  bool supports_star;
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

#endif // TEX_TYPES_H
