// nob.c
#define NOB_IMPLEMENTATION
#include "nob.h"
#include <string.h>

#define BUILD_DIR "build/"
#define SRC_DIR "src/"
#define TKS2XML_NAME "tks2xml"
#define XML2SVG_NAME "xml2svg"
#define DEFAULT_INPUT_FILE "input.tex"

static void replace_ext(const char *path, const char *new_ext, char *out, size_t out_sz)
{
    strncpy(out, path, out_sz - 1);
    out[out_sz - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    strncat(out, new_ext, out_sz - strlen(out) - 1);
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);
    Nob_Cmd cmd = {0};

    const char *input_tex = (argc >= 2) ? argv[1] : DEFAULT_INPUT_FILE;
    char inter_tks[256];
    char output_svg[256];
    replace_ext(input_tex, ".tks", inter_tks, sizeof(inter_tks));
    replace_ext(input_tex, ".svg", output_svg, sizeof(output_svg));

    if (!nob_file_exists(BUILD_DIR)) {
        if (!nob_mkdir_if_not_exists(BUILD_DIR))
            return 1;
    }

    // 1. Compilar generador de formato intermedio (.tex -> .tks)
    cmd.count = 0;
    nob_cmd_append(&cmd, "cc", "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-O2");
    nob_cmd_append(&cmd, "-o", BUILD_DIR TKS2XML_NAME, SRC_DIR "main.c");
    nob_cmd_append(&cmd, "-lm");
    if (!nob_cmd_run(&cmd)) return 1;

    // 2. Compilar renderizador tipográfico (.tks -> .svg)
    cmd.count = 0;
    nob_cmd_append(&cmd, "cc", "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-O2");
    nob_cmd_append(&cmd, "-o", BUILD_DIR XML2SVG_NAME, SRC_DIR "xml2svg.c");
    nob_cmd_append(&cmd, "-lm");
    if (!nob_cmd_run(&cmd)) return 1;

    nob_log(NOB_INFO, "=== Compilación completada con éxito ===");

    // 3. Ejecutar fase 1: .tex -> .tks (XML intermedio)
    cmd.count = 0;
    nob_cmd_append(&cmd, BUILD_DIR TKS2XML_NAME, input_tex, inter_tks);
    if (!nob_cmd_run(&cmd)) return 1;

    // 4. Ejecutar fase 2: .tks -> .svg
    cmd.count = 0;
    nob_cmd_append(&cmd, BUILD_DIR XML2SVG_NAME, inter_tks, output_svg);
    if (!nob_cmd_run(&cmd)) return 1;

    nob_cmd_free(cmd);

    nob_log(NOB_INFO, "¡Pipeline ejecutado por completo! Fichero generado: %s", output_svg);

    return 0;
}
