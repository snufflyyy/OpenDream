// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceIme (src/runtime_ime.c): sceImeOpen text entry through a fake game window (the
 * bbgpu_text_input_* calls), the no-window and DREAMS_IME_TEXT paths, and USB keyboard events
 * from SDL key events. Built by tests/run-dreams-tests.sh (SDL3, dummy video driver). */
#define _GNU_SOURCE
#include "../src/runtime_ime.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,nid)) return (uintptr_t)table[i].function;
    return 0;
}
/* The fake window. */
static int window=1, begun, state;
static char window_text[8192], begun_with[8192];
int bbgpu_text_input_begin(const char *initial, const char *prompt) {
    (void)prompt;
    if (!window) return 0;
    ++begun; size_t n=strnlen(initial,sizeof(begun_with)-1); memcpy(begun_with,initial,n); begun_with[n]=0; memcpy(window_text,begun_with,n+1); state=0;
    return 1;
}
int bbgpu_text_input_poll(char *out, uint64_t size) { size_t n=strnlen(window_text,size-1); memcpy(out,window_text,n); out[n]=0; return state; }

static ImeEvent got[64];
static int got_count;
static void *got_arg;
static ABI void handler(void *arg, const ImeEvent *e) { got_arg=arg; assert(got_count<64); got[got_count++]=*e; }
#define F(type,name) ((type)runtime_ime_resolve(name))
static int32_t (ABI *update)(ImeHandler);
static int pump(void) { got_count=0; assert(update(handler)==0); return got_count; }
static int u16eq(const uint16_t *a, const char *ascii) { for (;*ascii;++a,++ascii) if (*a!=(uint8_t)*ascii) return 0; return *a==0; }

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    setenv("SDL_VIDEODRIVER","dummy",1);
    unsetenv("DREAMS_IME_TEXT");
    update=F(int32_t (ABI *)(ImeHandler),"sceImeUpdate");
    void (ABI *param_init)(ImeParam *)=F(void (ABI *)(ImeParam *),"sceImeParamInit");
    int32_t (ABI *open)(const ImeParam *, const void *)=F(int32_t (ABI *)(const ImeParam *, const void *),"sceImeOpen");
    int32_t (ABI *close)(void)=F(int32_t (ABI *)(void),"sceImeClose");
    int32_t (ABI *set_text)(const uint16_t *, uint32_t)=F(int32_t (ABI *)(const uint16_t *, uint32_t),"sceImeSetText");
    int32_t (ABI *kb_open)(int32_t, const ImeKeyboardParam *)=F(int32_t (ABI *)(int32_t, const ImeKeyboardParam *),"sceImeKeyboardOpen");
    int32_t (ABI *kb_info)(uint32_t, ImeKeyboardInfo *)=F(int32_t (ABI *)(uint32_t, ImeKeyboardInfo *),"sceImeKeyboardGetInfo");
    int32_t (ABI *position)(ImePositionAndForm *)=F(int32_t (ABI *)(ImePositionAndForm *),"sceImeGetPanelPositionAndForm");
    assert(update && param_init && open && close && set_text && kb_open && kb_info && position);
    assert(update(handler)==IME_ERR_NOT_OPENED);

    /* sceImeOpen: Open event first, then typing in the window. */
    static uint16_t text[33], work[0x2800];
    text[0]='O'; text[1]='l'; text[2]='d';
    ImeParam p;
    param_init(&p);
    assert(p.user_id==-1 && open(&p,NULL)==IME_ERR_INVALID_USER_ID);
    p.user_id=1; p.max_text_length=32; p.input_text=text; p.work=work; p.arg=(void *)0x77; p.handler=handler;
    p.posx=100; p.posy=200;
    assert(open(&p,NULL)==0 && open(&p,NULL)==IME_ERR_BUSY);
    assert(pump()==1 && got[0].id==EV_OPEN && got[0].param.rect.x==100.0f && got[0].param.rect.width==0x319 && got_arg==(void *)0x77);
    ImePositionAndForm form;
    assert(position(&form)==0 && form.type==4 && form.posy==200.0f);
    assert(pump()==0 && begun==1 && !strcmp(begun_with,"Old"));
    snprintf(window_text,sizeof(window_text),"Old Dream");
    assert(pump()==1 && got[0].id==EV_UPDATE_TEXT);
    assert(got[0].param.text.str==work && u16eq(work,"Old Dream") && u16eq(text,"Old Dream"));
    assert(got[0].param.text.area[0].index==3 && got[0].param.text.area[0].length==6 && got[0].param.text.caret_index==9);
    snprintf(window_text,sizeof(window_text),"Old Drea");  /* backspace */
    assert(pump()==1 && got[0].param.text.area[0].index==8 && got[0].param.text.area[0].length==-1);
    assert(pump()==0); /* nothing changed */
    /* The game replaces the text: the window edits the new one. */
    const uint16_t fresh[]={'N','e','w',0};
    assert(set_text(fresh,3)==0 && begun==2 && !strcmp(begun_with,"New"));
    snprintf(window_text,sizeof(window_text),"New!"); state=1;
    assert(pump()==2 && got[0].id==EV_UPDATE_TEXT && got[1].id==EV_PRESS_ENTER && u16eq(text,"New!"));
    assert(pump()==0);
    assert(close()==0 && close()==IME_ERR_NOT_OPENED);

    /* Escape: PressClose, the game's text unchanged. */
    assert(open(&p,NULL)==0 && pump()==1 && pump()==0);
    snprintf(window_text,sizeof(window_text),"discarded"); state=2;
    assert(pump()==1 && got[0].id==EV_PRESS_CLOSE && u16eq(text,"New!"));
    close();

    /* No window (--cpu-only): the initial text is confirmed at once; DREAMS_IME_TEXT overrides. */
    window=0;
    assert(open(&p,NULL)==0 && pump()==1);
    assert(pump()==1 && got[0].id==EV_PRESS_ENTER && u16eq(text,"New!"));
    close();
    setenv("DREAMS_IME_TEXT","Caf\xc3\xa9 \xf0\x9f\x8e\xae",1); /* "Cafe" with an acute e, then a game controller emoji */
    assert(open(&p,NULL)==0 && pump()==1);
    assert(pump()==2 && got[0].id==EV_UPDATE_TEXT && got[1].id==EV_PRESS_ENTER);
    assert(text[3]==0xE9 && text[5]==0xD83C && text[6]==0xDFAE && text[7]==0);
    assert(got[0].param.text.area[0].index==0 && got[0].param.text.caret_index==7);
    close();
    unsetenv("DREAMS_IME_TEXT");

    /* USB keyboard: SDL key events (as the window thread delivers them) become keycode events. */
    assert(SDL_Init(SDL_INIT_VIDEO));
    ImeKeyboardParam kp={.option=1,.arg=(void *)0x55,.handler=handler};
    assert(kb_open(1,&kp)==0 && kb_open(1,&kp)==IME_ERR_BUSY);
    assert(pump()==1 && got[0].id==EV_KB_CONNECTION && got[0].param.resources.resource_id[0]==1 && got_arg==(void *)0x55);
    ImeKeyboardInfo info;
    assert(kb_info(1,&info)==0 && info.status==1 && info.type==4);
    SDL_Event e={0};
    e.type=SDL_EVENT_KEY_DOWN; e.key.scancode=SDL_SCANCODE_A; e.key.mod=SDL_KMOD_LSHIFT;
    key_watch(NULL,&e);
    e.key.repeat=true; key_watch(NULL,&e);
    e.type=SDL_EVENT_KEY_UP; e.key.repeat=false; e.key.mod=0; key_watch(NULL,&e);
    e.type=SDL_EVENT_KEY_DOWN; e.key.scancode=SDL_SCANCODE_F1; key_watch(NULL,&e);
    assert(pump()==4);
    assert(got[0].id==EV_KB_DOWN && got[0].param.keycode.keycode==4 && got[0].param.keycode.character=='A');
    assert(got[0].param.keycode.status==(KS_KEYCODE_VALID|KS_CHARACTER_VALID|KS_L_SHIFT) && got[0].param.keycode.user_id==1);
    assert(got[1].id==EV_KB_REPEAT && (got[1].param.keycode.status & 0x80));
    assert(got[2].id==EV_KB_UP && got[2].param.keycode.character=='a');
    assert(got[3].id==EV_KB_DOWN && got[3].param.keycode.keycode==SDL_SCANCODE_F1 && got[3].param.keycode.character==0 &&
           got[3].param.keycode.status==KS_KEYCODE_VALID);
    /* While sceImeOpen's window text entry runs, the keys are the IME's. */
    window=1; state=0;
    assert(open(&p,NULL)==0 && pump()==1 && pump()==0);
    e.type=SDL_EVENT_KEY_DOWN; e.key.scancode=SDL_SCANCODE_B; key_watch(NULL,&e);
    assert(pump()==0);
    close();
    puts("ime-test passed");
    return 0;
}
