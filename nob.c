// nob.c
#define NOB_IMPLEMENTATION
#include "nob.h"

#define BUILD_DIR "build/"
#define SRC_DIR "src/"
#define TKS2XML_NAME "tks2xml"
#define XML2SVG_NAME "xml2svg"
#define INPUT_FILE "input.tks"   // <- Ajustado a la raíz si está ahí, o "src/input.tks" si prefieres moverlo

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);
    Nob_Cmd cmd = {0};

    if (!nob_file_exists(BUILD_DIR)) {
        if (!nob_mkdir_if_not_exists(BUILD_DIR))
            return 1;
    }

    // 1. Compilar tks2xml
    cmd.count = 0;
    nob_cmd_append(&cmd, "cc", "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-O2");
    nob_cmd_append(&cmd, "-o", BUILD_DIR TKS2XML_NAME, SRC_DIR "main.c");
    nob_cmd_append(&cmd, "-lm");
    if (!nob_cmd_run(&cmd)) return 1;

    // 2. Compilar xml2svg
    cmd.count = 0;
    nob_cmd_append(&cmd, "cc", "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-O2");
    nob_cmd_append(&cmd, "-o", BUILD_DIR XML2SVG_NAME, SRC_DIR "xml2svg.c");
    nob_cmd_append(&cmd, "-lm");
    if (!nob_cmd_run(&cmd)) return 1;

    nob_log(NOB_INFO, "=== Compilación completada con éxito ===");

    // 3. Ejecutar fase 1: tks2xml input.tks -> input.xml (generado en la misma ruta del input)
    cmd.count = 0;
    nob_cmd_append(&cmd, BUILD_DIR TKS2XML_NAME, INPUT_FILE);
    if (!nob_cmd_run(&cmd)) return 1;

    // 4. Ejecutar fase 2: xml2svg input.xml input.svg
    cmd.count = 0;
    nob_cmd_append(&cmd, BUILD_DIR XML2SVG_NAME, "input.xml", "input.svg");
    if (!nob_cmd_run(&cmd)) return 1;

    nob_cmd_free(cmd);

    nob_log(NOB_INFO, "¡Pipeline ejecutado por completo! Fichero generado: input.svg");

    return 0;
}
