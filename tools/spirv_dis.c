/* Minimal SPIR-V disassembler over Fedora's libSPIRV-Tools-shared.so (spirv-tools-libs; no spirv-dis installed).
 * Build: gcc tools/spirv_dis.c -o out/spirv-dis /usr/lib64/libSPIRV-Tools-shared.so
 * Use:   out/spirv-dis file.spv > file.spvasm   (docs/dreams/GPU_HANG.md) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef struct spv_text_t { const char* str; size_t length; } spv_text_t, *spv_text;
typedef void* spv_context; typedef void* spv_diagnostic;
spv_context spvContextCreate(int env);
int spvBinaryToText(spv_context, const uint32_t*, size_t, uint32_t, spv_text*, spv_diagnostic*);
int main(int c,char**v){ FILE*f=fopen(v[1],"rb"); fseek(f,0,2); long n=ftell(f); rewind(f);
 uint32_t*b=malloc(n); fread(b,1,n,f); spv_text t=0; spv_diagnostic d=0;
 int r=spvBinaryToText(spvContextCreate(24),b,n/4,1|8|64,&t,&d); if(r){fprintf(stderr,"err %d\n",r);return 1;}
 fwrite(t->str,1,t->length,stdout); return 0;}
