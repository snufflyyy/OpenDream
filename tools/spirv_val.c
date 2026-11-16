/* Minimal SPIR-V validator over Fedora's libSPIRV-Tools-shared.so (no spirv-val installed).
 * Build: gcc tools/spirv_val.c -o out/spirv-val /usr/lib64/libSPIRV-Tools-shared.so
 * Use:   out/spirv-val [--env N] file.spv...   prints "<file>: OK" or the first error; exit 1 if any
 *        failed. Default env 25 = SPV_ENV_VULKAN_1_3 (docs/dreams/GPU_HANG.md, loop guard checks). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
typedef struct { size_t line, column, index; } spv_position_t;
typedef struct { spv_position_t position; char* error; bool isTextSource; } spv_diagnostic_t, *spv_diagnostic;
typedef void* spv_context;
spv_context spvContextCreate(int env);
int spvValidateBinary(spv_context, const uint32_t*, size_t, spv_diagnostic*);
void spvDiagnosticDestroy(spv_diagnostic);
int main(int argc, char** argv) {
    int env = 25, first = 1, failed = 0;
    if (argc > 2 && !strcmp(argv[1], "--env")) { env = atoi(argv[2]); first = 3; }
    spv_context ctx = spvContextCreate(env);
    for (int i = first; i < argc; ++i) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { printf("%s: cannot open\n", argv[i]); failed = 1; continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
        uint32_t* words = malloc(n);
        if (fread(words, 1, n, f) != (size_t)n) { printf("%s: short read\n", argv[i]); failed = 1; }
        fclose(f);
        spv_diagnostic diag = NULL;
        int r = spvValidateBinary(ctx, words, n / 4, &diag);
        if (r) { printf("%s: FAIL %d: %s\n", argv[i], r, diag && diag->error ? diag->error : "?"); failed = 1; }
        else printf("%s: OK\n", argv[i]);
        if (diag) spvDiagnosticDestroy(diag);
        free(words);
    }
    return failed;
}
