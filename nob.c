// nob.c
#define NOB_IMPLEMENTATION
#include "nob.h"

#define BUILD_DIR "build/"
#define SRC_DIR "src/"
#define APP_NAME "teksto"
#define INPUT_FILE "input.txt"


int main(int argc, char **argv)
{
  NOB_GO_REBUILD_URSELF(argc, argv);
  Nob_Cmd cmd = {0};

  if (!nob_file_exists(BUILD_DIR)) {
    if (!nob_mkdir_if_not_exists(BUILD_DIR))
      return 1;
  }

  nob_cmd_append(&cmd, "cc", "-Wall", "-Wextra", "-o", BUILD_DIR APP_NAME, SRC_DIR"main.c");

  if (!nob_cmd_run(&cmd)) return 1;

  nob_cmd_append(&cmd,BUILD_DIR APP_NAME, INPUT_FILE);

  if (!nob_cmd_run(&cmd)) return 1;

  return 0;
}
