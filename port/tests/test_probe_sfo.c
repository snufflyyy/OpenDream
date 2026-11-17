// SPDX-FileCopyrightText: Copyright 2026 OpenDream contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise the real static loader parser without linking/starting the runtime.
 * Compile with -ffunction-sections -fdata-sections and -Wl,--gc-sections. */
/* Renaming main removes C's implicit-return exemption in the unused loader entry. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main unused_probe_main
#include "../src/probe.c"
#undef main
#pragma GCC diagnostic pop
#include <assert.h>

static unsigned char fixture[128];
static char path[]="/tmp/dreams-sfo-test.XXXXXX";
static void put16(size_t at, uint16_t v) { memcpy(fixture+at,&v,sizeof(v)); }
static void put32(size_t at, uint32_t v) { memcpy(fixture+at,&v,sizeof(v)); }
static void reset(void) {
    memset(fixture,0,sizeof(fixture));
    memcpy(fixture,"\0PSF",4);
    put32(8,48); put32(12,80); put32(16,1);
    put16(20,0); put16(22,0x404); put32(24,4); put32(28,4); put32(32,0);
    memcpy(fixture+48,"TITLE_ID",9); put32(80,42);
}
static int parse(size_t size, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(fixture,1,size,f)==size); assert(fclose(f)==0);
    return sfo_value(path,"TITLE_ID",text,text_size,number);
}
int main(void) {
    int fd=mkstemp(path); assert(fd>=0); close(fd);
    uint32_t number=0;
    reset(); assert(parse(sizeof(fixture),NULL,0,&number)==1 && number==42);
    reset(); put16(22,0x204); put32(24,10); memcpy(fixture+80,"CUSA08010",10);
    char text[6]; assert(parse(sizeof(fixture),text,sizeof(text),NULL)==1);
    assert(!strcmp(text,"CUSA0"));
    reset(); assert(parse(19,NULL,0,&number)==0);
    reset(); put32(16,UINT32_MAX); assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); put32(8,UINT32_MAX); assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); put16(20,UINT16_MAX); assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); memset(fixture+48,'T',sizeof(fixture)-48);
    assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); put32(12,UINT32_MAX-15); put32(32,48);
    assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); put32(24,UINT32_MAX); assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); put32(32,UINT32_MAX); assert(parse(sizeof(fixture),NULL,0,&number)==0);
    reset(); put32(24,3); assert(parse(sizeof(fixture),NULL,0,&number)==0);
    unlink(path);
    puts("PASS loader SFO: valid values, truncated tables, keys, and overflowing offsets");
    return 0;
}
