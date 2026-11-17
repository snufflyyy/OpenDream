// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise atomic save copies through guest file operations on a private mount. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_memory_note_write(uintptr_t a,uint64_t n) { (void)a; (void)n; }
void runtime_memory_note_cpu_write(uintptr_t a,uint64_t n) { (void)a; (void)n; }
void runtime_memory_prepare_cpu_write(uintptr_t a,uint64_t n) { (void)a; (void)n; }
uintptr_t runtime_lookup(const RuntimeExport *t,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(t[i].name,name)) return (uintptr_t)t[i].function;
    return 0;
}
static void contents(const char *path,const char *expected) {
    char b[32]={0}; FILE *f=fopen(path,"rb"); assert(f);
    size_t n=fread(b,1,sizeof(b),f); assert(!ferror(f) && !fclose(f));
    assert(n==strlen(expected) && !memcmp(b,expected,n));
}
static void no_temps(const char *root) {
    DIR *d=opendir(root); assert(d);
    struct dirent *e; while ((e=readdir(d))) assert(!temp_name(e->d_name));
    assert(!closedir(d));
}
int main(int argc,char **argv) {
    assert(argc==2);
    char root[]="/tmp/bbport-save-commit-XXXXXX"; assert(mkdtemp(root));
    char path[512]; snprintf(path,sizeof(path),"%s/save",root);
    assert(!runtime_file_mount("/savedata0",root));
    const char *guest="/savedata0/save";
    if (strcmp(argv[1],"create")) {
        FILE *f=fopen(path,"wb"); assert(f); assert(fwrite("original",1,8,f)==8); assert(!fclose(f));
    }
    if (!strcmp(argv[1],"create")) {
        int fd=(int)do_open(guest,0x202,0644); assert(fd>=3);
        assert(access(path,F_OK)<0 && errno==ENOENT);
        assert(!do_close(fd)); contents(path,"");
    } else if (!strcmp(argv[1],"truncate")) {
        int fd=(int)do_open(guest,0x402,0644); assert(fd>=3); contents(path,"original");
        assert(!do_close(fd)); contents(path,"");
    } else if (!strcmp(argv[1],"clean")) {
        struct stat before,after; assert(!stat(path,&before));
        int fd=(int)do_open(guest,2,0); assert(fd>=3); assert(!do_close(fd));
        assert(!stat(path,&after) && before.st_ino==after.st_ino); contents(path,"original");
        fd=(int)do_open(guest,0x202,0644); assert(fd>=3); assert(!do_close(fd));
        assert(!stat(path,&after) && before.st_ino==after.st_ino); contents(path,"original");
    } else if (!strcmp(argv[1],"write")) {
        int fd=(int)do_open(guest,0x402,0); assert(fd>=3);
        assert(do_write(fd,"new",3)==3); contents(path,"original");
        assert(!do_close(fd)); contents(path,"new");
    } else if (!strcmp(argv[1],"exclusive")) {
        assert(do_open(guest,0xa02,0644)==-EEXIST); contents(path,"original");
        assert(do_open("/savedata0/absent",2,0)==-ENOENT);
    } else if (!strcmp(argv[1],"exhaustion")) {
        for (int i=3;i<MAX_FILES;++i) files[i].used=1;
        assert(do_open(guest,0x402,0)==-EMFILE); contents(path,"original");
        assert(do_open("/savedata0/new",0x202,0644)==-EMFILE);
        char newpath[512]; snprintf(newpath,sizeof(newpath),"%s/new",root);
        assert(access(newpath,F_OK)<0 && errno==ENOENT);
        for (int i=3;i<MAX_FILES;++i) files[i].used=0;
    } else assert(0);
    no_temps(root); assert(!unlink(path)); assert(!rmdir(root));
    printf("save commit %s PASS\n",argv[1]);
}
