#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {

    if (argc < 2) {
        fprintf(stderr, "Uso: %s <fichero_de_entrada.txt>\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("Archivo a procesar recibido: %s\n", argv[1]);

    return EXIT_SUCCESS;
}
