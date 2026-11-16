// SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceIme: the in-game IME (sceImeOpen, used by Dreams to name creations and for search/text
 * fields) and the USB keyboard (sceImeKeyboardOpen, opened by Dreams at boot). Both deliver their
 * events through the handler the game passes to sceImeUpdate, on the game's thread, as on the PS4.
 * Reference: ref/shadps4 (branch dreams) src/core/libraries/ime/{ime.cpp, ime_ui.cpp,
 * ime_common.h}; structures and error codes from there.
 *
 * Text entry: there is no on-screen keyboard. sceImeOpen starts SDL text input in the game window
 * (bbgpu_text_input_begin: the window thread owns SDL events; vcore/shim/window.cpp shows the text
 * in the title bar, Enter confirms, Escape cancels). Each sceImeUpdate polls it: a changed text is
 * written to the game's inputTextBuffer and work buffer and announced as UpdateText (edit area =
 * the changed span, as shadPS4's ime_ui.cpp computes it), Enter becomes PressEnter, Escape
 * PressClose. Without a window (--cpu-only) or with DREAMS_IME_TEXT=<utf-8> set, that text (else
 * the initial text) is entered and confirmed at the first update, so the game never waits.
 *
 * USB keyboard: while sceImeKeyboardOpen is open and the window exists, an SDL event watch
 * (SDL_AddEventWatch: runs on the window thread, like runtime_pad's key latch) queues every key
 * press/release; sceImeUpdate delivers them in order as KeyboardKeycodeDown/Up (keycode = USB HID
 * usage = SDL scancode, character = the key's UTF-16 character with Shift/Caps applied, modifier and
 * LED bits in status) and SDL's key repeats as KeyboardKeycodeRepeat when the game asked for
 * repeats (option Repeat/RepeatEachKey). A KeyboardConnection event goes first. Not while sceImeOpen's text entry is active (the keys are
 * the IME's then). DREAMS_IME_KEYBOARD=0 turns the keyboard events off (shadPS4 sends none).
 * Note: bbport's pad bindings also read the keyboard (WASD etc.); the game sees both. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define IME_ERR_BUSY ((int32_t)0x80BC0001)
#define IME_ERR_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_ERR_INVALID_TEXT ((int32_t)0x80BC0006)
#define IME_ERR_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_ERR_INVALID_TYPE ((int32_t)0x80BC0011)
#define IME_ERR_INVALID_MAX_TEXT_LENGTH ((int32_t)0x80BC0016)
#define IME_ERR_INVALID_INPUT_TEXT_BUFFER ((int32_t)0x80BC0017)
#define IME_ERR_INVALID_WORK ((int32_t)0x80BC0020)
#define IME_ERR_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_ERR_INVALID_PARAM ((int32_t)0x80BC0030)
#define IME_ERR_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_ERR_INVALID_RESERVED ((int32_t)0x80BC0032)
#define IME_MAX_TEXT 2048

enum { EV_OPEN=0, EV_UPDATE_TEXT=1, EV_UPDATE_CARET=2, EV_PRESS_CLOSE=4, EV_PRESS_ENTER=5, EV_ABORT=6,
       EV_KB_OPEN=256, EV_KB_DOWN=257, EV_KB_UP=258, EV_KB_REPEAT=259, EV_KB_CONNECTION=260, EV_KB_DISCONNECTION=261 };
enum { KS_KEYCODE_VALID=1, KS_CHARACTER_VALID=2, KS_L_CTRL=0x100, KS_L_SHIFT=0x200, KS_L_ALT=0x400, KS_L_GUI=0x800,
       KS_R_CTRL=0x1000, KS_R_SHIFT=0x2000, KS_R_ALT=0x4000, KS_R_GUI=0x8000, KS_NUM=0x10000, KS_CAPS=0x20000, KS_SCROLL=0x40000 };

typedef struct { float x, y; uint32_t width, height; } ImeRect;
typedef struct { uint32_t mode, index; int32_t length; } ImeTextArea;
typedef struct { uint16_t *str; uint32_t caret_index, area_num; ImeTextArea area[4]; } ImeEditText;
typedef struct { uint16_t keycode, character; uint32_t status, type; int32_t user_id; uint32_t resource_id; uint64_t timestamp; } ImeKeycode;
typedef struct { int32_t user_id; uint32_t resource_id[5]; } ImeResourceIds;
typedef union { ImeRect rect; ImeEditText text; uint32_t caret_move; ImeKeycode keycode; ImeResourceIds resources; int8_t reserved[64]; } ImeEventParam;
typedef struct { uint32_t id; ImeEventParam param; } ImeEvent;
typedef void (ABI *ImeHandler)(void *arg, const ImeEvent *event);
typedef struct {
    int32_t user_id; uint32_t type; uint64_t supported_languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_text_length; uint16_t *input_text; float posx, posy;
    uint32_t horizontal_alignment, vertical_alignment; void *work, *arg; ImeHandler handler; int8_t reserved[8];
} ImeParam;
typedef struct { uint32_t option; int8_t reserved1[4]; void *arg; ImeHandler handler; int8_t reserved2[8]; } ImeKeyboardParam;
typedef struct { int32_t user_id; uint32_t device, type, repeat_delay, repeat_rate, status; int8_t reserved[12]; } ImeKeyboardInfo;
typedef struct { uint32_t type; float posx, posy; uint32_t horizontal_alignment, vertical_alignment, width, height; } ImePositionAndForm;
typedef struct { float x, y; uint32_t height, index; } ImeCaret;
_Static_assert(sizeof(ImeEvent)==72 && sizeof(ImeEditText)==64 && sizeof(ImeKeycode)==32, "OrbisImeEvent layout");
_Static_assert(sizeof(ImeParam)==96 && __builtin_offsetof(ImeParam,input_text)==40 && __builtin_offsetof(ImeParam,handler)==80, "OrbisImeParam layout");
_Static_assert(sizeof(ImeKeyboardParam)==32 && sizeof(ImeKeyboardInfo)==36, "OrbisImeKeyboardParam/Info layout");

/* ---- UTF-8 <-> UTF-16 ---- */
static size_t utf16_len(const uint16_t *s, size_t max) { size_t n=0; while (s && n<max && s[n]) ++n; return n; }
static size_t utf16_to_utf8(const uint16_t *in, size_t length, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0;i<length && in[i];++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<length && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        char b[4]; int k;
        if (c<0x80) { b[0]=(char)c; k=1; }
        else if (c<0x800) { b[0]=(char)(0xC0|c>>6); b[1]=(char)(0x80|(c&63)); k=2; }
        else if (c<0x10000) { b[0]=(char)(0xE0|c>>12); b[1]=(char)(0x80|((c>>6)&63)); b[2]=(char)(0x80|(c&63)); k=3; }
        else { b[0]=(char)(0xF0|c>>18); b[1]=(char)(0x80|((c>>12)&63)); b[2]=(char)(0x80|((c>>6)&63)); b[3]=(char)(0x80|(c&63)); k=4; }
        if (n+(size_t)k+1>size) break;
        memcpy(out+n,b,(size_t)k); n+=(size_t)k;
    }
    if (size) out[n]=0;
    return n;
}
/* At most `max` UTF-16 units (a pair is never split); returns the units written, terminated. */
static size_t utf8_to_utf16(const char *in, uint16_t *out, size_t max) {
    size_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else { if (n+1>max) break; out[n++]=(uint16_t)c; }
    }
    out[n]=0;
    return n;
}

/* ---- State ---- */
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    int open;
    ImeParam param;
    uint32_t max;                 /* min(maxTextLength, 2048) */
    uint16_t text[IME_MAX_TEXT+1]; /* what the game was told last */
    size_t length;
    uint32_t caret;
    int input_begun, input_window, finished, open_pending, text_dirty;
} ime;
/* Key events from the window thread (SDL event watch), delivered at the next sceImeUpdate. */
#define KEY_QUEUE 256
typedef struct { uint32_t id; uint16_t scancode; uint16_t mods; uint64_t tick; } KeyRecord;
static struct {
    int open, connected_sent, watching;
    ImeKeyboardParam param;
    KeyRecord queue[KEY_QUEUE];
    int head, count;
} keyboard;
static int typing_now(void) { return ime.open && ime.input_window && !ime.finished; }
/* runtime_pad: the keyboard is the text box's now (its key bindings pause). */
int runtime_ime_typing(void) {
    pthread_mutex_lock(&lock);
    const int typing=typing_now();
    pthread_mutex_unlock(&lock);
    return typing;
}
static uint64_t rtc_tick(void);
static bool key_watch(void *userdata, SDL_Event *e) {
    (void)userdata;
    if (e->type!=SDL_EVENT_KEY_DOWN && e->type!=SDL_EVENT_KEY_UP) return true;
    pthread_mutex_lock(&lock);
    const int repeat=keyboard.param.option & 3; /* Repeat | RepeatEachKey */
    if (keyboard.open && !typing_now() && (int)e->key.scancode>0 && (int)e->key.scancode<SDL_SCANCODE_COUNT &&
        (!e->key.repeat || repeat) && keyboard.count<KEY_QUEUE) {
        KeyRecord *r=&keyboard.queue[(keyboard.head+keyboard.count++)%KEY_QUEUE];
        r->id=e->type==SDL_EVENT_KEY_UP ? EV_KB_UP : e->key.repeat ? EV_KB_REPEAT : EV_KB_DOWN;
        r->scancode=(uint16_t)e->key.scancode; r->mods=(uint16_t)e->key.mod; r->tick=rtc_tick();
    }
    pthread_mutex_unlock(&lock);
    return true;
}
/* bbgpu_text_input_end: ends the window's text input when the game closes the IME first. The
 * window does not have it yet (vcore/shim/window.cpp); weak until it does. */
extern void bbgpu_text_input_end(void) __attribute__((weak));

static uint64_t rtc_tick(void) { /* sceRtc ticks: microseconds since 0001-01-01 */
    struct timeval tv; gettimeofday(&tv,NULL);
    return (uint64_t)tv.tv_sec*1000000ull+(uint64_t)tv.tv_usec+62135596800ull*1000000ull;
}
static int env_on(const char *name, int fallback) { const char *v=getenv(name); return v && *v ? v[0]!='0' : fallback; }

/* The text as the game sees it now (work buffer = event str, inputTextBuffer), then UpdateText
 * with the changed span (ref ime_ui.cpp InputTextCallback). Called with lock held; the event is
 * returned for delivery outside it. */
static int set_text(const uint16_t *next, size_t length, ImeEvent *event) {
    if (length>ime.max) length=ime.max;
    size_t prefix=0;
    while (prefix<ime.length && prefix<length && ime.text[prefix]==next[prefix]) ++prefix;
    size_t old_tail=ime.length, new_tail=length;
    while (old_tail>prefix && new_tail>prefix && ime.text[old_tail-1]==next[new_tail-1]) { --old_tail; --new_tail; }
    if (length==ime.length && prefix==length) return 0;
    memmove(ime.text,next,length*2); ime.text[length]=0; ime.length=length; ime.caret=(uint32_t)length;
    uint16_t *work=ime.param.work;
    memcpy(work,ime.text,(length+1)*2);
    if (ime.param.input_text) memcpy(ime.param.input_text,ime.text,(length+1)*2);
    memset(event,0,sizeof(*event));
    event->id=EV_UPDATE_TEXT;
    event->param.text.str=work;
    event->param.text.caret_index=(uint32_t)length;
    event->param.text.area_num=1;
    event->param.text.area[0].mode=1; /* Edit */
    event->param.text.area[0].index=(uint32_t)prefix;
    event->param.text.area[0].length=(int32_t)(new_tail-prefix)-(int32_t)(old_tail-prefix);
    return 1;
}
static void text_payload(ImeEvent *event, uint32_t id) {
    memset(event,0,sizeof(*event));
    event->id=id;
    event->param.text.str=ime.param.work;
    event->param.text.caret_index=ime.caret;
    event->param.text.area_num=1;
    event->param.text.area[0].mode=1;
    event->param.text.area[0].index=ime.caret;
}

/* ---- sceIme* ---- */
static ABI void ime_param_init(ImeParam *param) { if (param) { memset(param,0,sizeof(*param)); param->user_id=-1; } }
static ABI int32_t ime_open(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param) return IME_ERR_INVALID_ADDRESS;
    if (param->user_id==-1 || param->user_id==0xff) return IME_ERR_INVALID_USER_ID;
    if (param->type>4) return IME_ERR_INVALID_TYPE;
    if (!param->max_text_length || param->max_text_length>IME_MAX_TEXT) return IME_ERR_INVALID_MAX_TEXT_LENGTH;
    if (!param->input_text) return IME_ERR_INVALID_INPUT_TEXT_BUFFER;
    if (!param->work || ((uintptr_t)param->work & 3)) return IME_ERR_INVALID_WORK;
    if (!param->handler) return IME_ERR_INVALID_HANDLER;
    for (int i=0;i<8;++i) if (param->reserved[i]) return IME_ERR_INVALID_RESERVED;
    pthread_mutex_lock(&lock);
    if (ime.open) { pthread_mutex_unlock(&lock); return IME_ERR_BUSY; }
    memset(&ime,0,sizeof(ime));
    ime.param=*param; ime.open=1; ime.open_pending=1;
    ime.max=param->max_text_length;
    memset(param->work,0,0x5000); /* ref sceImeOpen: the work area is the library's */
    ime.length=utf16_len(param->input_text,ime.max);
    memcpy(ime.text,param->input_text,ime.length*2); ime.text[ime.length]=0;
    ime.caret=(uint32_t)ime.length;
    pthread_mutex_unlock(&lock);
    char initial[IME_MAX_TEXT*3+1];
    utf16_to_utf8(param->input_text,ime.max,initial,sizeof(initial));
    printf("Runtime: Ime opened (type %u, up to %u characters, text \"%s\")\n",param->type,param->max_text_length,initial);
    return 0;
}
static ABI int32_t ime_close(void) {
    pthread_mutex_lock(&lock);
    const int was=ime.open, window=ime.input_window && !ime.finished;
    ime.open=0;
    pthread_mutex_unlock(&lock);
    if (!was) return IME_ERR_NOT_OPENED;
    if (window && bbgpu_text_input_end) bbgpu_text_input_end();
    puts("Runtime: Ime closed");
    return 0;
}
static ABI int32_t ime_set_text(const uint16_t *text, uint32_t length) {
    pthread_mutex_lock(&lock);
    if (!ime.open) { pthread_mutex_unlock(&lock); return IME_ERR_NOT_OPENED; }
    if (!text) { pthread_mutex_unlock(&lock); return IME_ERR_INVALID_ADDRESS; }
    const int multiline=ime.param.option & 1;
    for (uint32_t i=0;i<length && text[i];++i)
        if ((!multiline && (text[i]=='\n' || text[i]=='\r')) || (text[i]&0xF800)==0xD800) { pthread_mutex_unlock(&lock); return IME_ERR_INVALID_TEXT; }
    size_t n=utf16_len(text,length);
    if (n>ime.max) n=ime.max;
    memcpy(ime.text,text,n*2); ime.text[n]=0; ime.length=n; ime.caret=(uint32_t)n;
    const int restart=ime.input_window && !ime.finished;
    char utf8[IME_MAX_TEXT*3+1];
    utf16_to_utf8(ime.text,n,utf8,sizeof(utf8));
    pthread_mutex_unlock(&lock);
    if (restart) bbgpu_text_input_begin(utf8,"Text"); /* the window edits the game's text now */
    return 0;
}
static ABI int32_t ime_set_caret(const ImeCaret *caret) {
    pthread_mutex_lock(&lock);
    int32_t r=!ime.open ? IME_ERR_NOT_OPENED : !caret ? IME_ERR_INVALID_ADDRESS : caret->index>ime.length ? IME_ERR_INVALID_PARAM : 0;
    if (!r) ime.caret=caret->index; /* the window always types at the end */
    pthread_mutex_unlock(&lock);
    return r;
}
/* ref ime.cpp sceImeGetPanelSize */
static void panel_size(const ImeParam *p, uint32_t *w, uint32_t *h) {
    if (p->type==4) { *w=0x172; *h=0x192; } else { *w=0x319; *h=0x198; }
    if (p->option & 0x400) { *w<<=1; *h<<=1; } /* USE_OVER_2K_COORDINATES */
}
static ABI int32_t ime_panel_position(ImePositionAndForm *form) {
    if (!form) return 0; /* as shadPS4 */
    pthread_mutex_lock(&lock);
    memset(form,0,sizeof(*form));
    if (ime.open) {
        form->type=4; /* Edit: no on-screen keyboard panel */
        form->posx=ime.param.posx; form->posy=ime.param.posy;
        form->horizontal_alignment=ime.param.horizontal_alignment; form->vertical_alignment=ime.param.vertical_alignment;
        panel_size(&ime.param,&form->width,&form->height);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* The IME's events since the last update, delivered to `handler` after the lock is released. */
static int ime_collect(ImeEvent *events, int max) {
    int n=0;
    if (!ime.open) return 0;
    if (ime.open_pending && n<max) {
        ime.open_pending=0;
        ImeEvent *e=&events[n++]; memset(e,0,sizeof(*e));
        e->id=EV_OPEN; e->param.rect.x=ime.param.posx; e->param.rect.y=ime.param.posy;
        panel_size(&ime.param,&e->param.rect.width,&e->param.rect.height);
        return n; /* the game sees Open alone first */
    }
    if (ime.finished) return 0;
    char text[IME_MAX_TEXT*3+1];
    int state=0;
    if (!ime.input_begun) {
        ime.input_begun=1;
        const char *preset=getenv("DREAMS_IME_TEXT");
        utf16_to_utf8(ime.text,ime.length,text,sizeof(text));
        if (!preset || !*preset) {
            pthread_mutex_unlock(&lock); /* the window takes its own lock */
            ime.input_window=bbgpu_text_input_begin(text,"Name");
            pthread_mutex_lock(&lock);
            if (!ime.open) return 0;
        }
        if (preset && *preset) { snprintf(text,sizeof(text),"%s",preset); state=1; }
        else if (!ime.input_window) state=1; /* no window: the initial text, confirmed */
        else printf("Runtime: Ime: type in the game window (title bar shows the text), Enter = OK, Esc = cancel\n");
        if (state) printf("Runtime: Ime: no text entry window: entering \"%s\"\n",text);
    } else if (ime.input_window) {
        pthread_mutex_unlock(&lock);
        state=bbgpu_text_input_poll(text,sizeof(text));
        pthread_mutex_lock(&lock);
        if (!ime.open) return 0;
    } else return 0;
    if (state!=2) { /* cancelled: the game's text stays as it was */
        uint16_t next[IME_MAX_TEXT+1];
        const size_t length=utf8_to_utf16(text,next,ime.max);
        if (n<max && set_text(next,length,&events[n])) ++n;
    }
    if (state && n<max) {
        ime.finished=1;
        text_payload(&events[n++],state==1 ? EV_PRESS_ENTER : EV_PRESS_CLOSE);
        utf16_to_utf8(ime.text,ime.length,text,sizeof(text));
        printf("Runtime: Ime %s: \"%s\"\n",state==1 ? "confirmed" : "cancelled",text);
    }
    return n;
}

static uint16_t key_character(SDL_Scancode sc, SDL_Keymod mods) {
    const SDL_Keycode k=SDL_GetKeyFromScancode(sc,mods,false);
    if (k & SDLK_SCANCODE_MASK) return 0;
    if (k>=0x20 && k!=0x7f && k<0xD800) return (uint16_t)k;
    return 0;
}
static uint32_t key_status(SDL_Keymod mods) {
    uint32_t s=KS_KEYCODE_VALID;
    if (mods & SDL_KMOD_LCTRL) s|=KS_L_CTRL;
    if (mods & SDL_KMOD_LSHIFT) s|=KS_L_SHIFT;
    if (mods & SDL_KMOD_LALT) s|=KS_L_ALT;
    if (mods & SDL_KMOD_LGUI) s|=KS_L_GUI;
    if (mods & SDL_KMOD_RCTRL) s|=KS_R_CTRL;
    if (mods & SDL_KMOD_RSHIFT) s|=KS_R_SHIFT;
    if (mods & SDL_KMOD_RALT) s|=KS_R_ALT;
    if (mods & SDL_KMOD_RGUI) s|=KS_R_GUI;
    if (mods & SDL_KMOD_NUM) s|=KS_NUM;
    if (mods & SDL_KMOD_CAPS) s|=KS_CAPS;
    if (mods & SDL_KMOD_SCROLL) s|=KS_SCROLL;
    return s;
}
static void key_event(ImeEvent *e, const KeyRecord *r) {
    memset(e,0,sizeof(*e));
    e->id=r->id;
    ImeKeycode *k=&e->param.keycode;
    k->keycode=r->scancode; /* SDL scancodes are USB HID usages, as PS4 keycodes */
    k->character=key_character((SDL_Scancode)r->scancode,(SDL_Keymod)r->mods);
    k->status=key_status((SDL_Keymod)r->mods) | (k->character ? KS_CHARACTER_VALID : 0) | (r->id==EV_KB_REPEAT ? 0x80 : 0);
    k->type=4; /* ENGLISH_US */
    k->user_id=1; k->resource_id=1;
    k->timestamp=r->tick;
}
/* Called with lock held. The watch is installed once the window's SDL video exists (unlocked:
 * SDL takes its own event lock, which the window thread holds while calling key_watch). */
static int keyboard_collect(ImeEvent *events, int max) {
    if (!keyboard.open || !env_on("DREAMS_IME_KEYBOARD",1)) return 0;
    if (!keyboard.watching) {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
        keyboard.watching=1;
        pthread_mutex_unlock(&lock);
        const int ok=SDL_AddEventWatch(key_watch,NULL);
        pthread_mutex_lock(&lock);
        if (!ok) { printf("Runtime: Ime keyboard: no SDL event watch (%s)\n",SDL_GetError()); return 0; }
    }
    int n=0;
    if (!keyboard.connected_sent && n<max) {
        keyboard.connected_sent=1;
        ImeEvent *e=&events[n++]; memset(e,0,sizeof(*e));
        e->id=EV_KB_CONNECTION; e->param.resources.user_id=1; e->param.resources.resource_id[0]=1;
        puts("Runtime: Ime keyboard: the host keyboard is connected (USB keyboard events on)");
    }
    while (keyboard.count && n<max) {
        key_event(&events[n++],&keyboard.queue[keyboard.head]);
        keyboard.head=(keyboard.head+1)%KEY_QUEUE; --keyboard.count;
    }
    return n;
}

static ABI int32_t ime_update(ImeHandler handler) {
    ImeEvent events[64];
    pthread_mutex_lock(&lock);
    if (!ime.open && !keyboard.open) { pthread_mutex_unlock(&lock); return IME_ERR_NOT_OPENED; }
    int n=ime_collect(events,32);
    n+=keyboard_collect(events+n,64-n);
    pthread_mutex_unlock(&lock);
    if (!handler) return n ? IME_ERR_INVALID_HANDLER : 0;
    for (int i=0;i<n;++i) handler(events[i].id>=EV_KB_OPEN ? keyboard.param.arg : ime.param.arg,&events[i]);
    return 0;
}

static ABI int32_t ime_keyboard_open(int32_t user, const ImeKeyboardParam *param) {
    if (!param) return IME_ERR_INVALID_ADDRESS;
    if (!param->handler) return IME_ERR_INVALID_HANDLER;
    if (user==-1) return IME_ERR_INVALID_USER_ID;
    for (int i=0;i<4;++i) if (param->reserved1[i]) return IME_ERR_INVALID_RESERVED;
    for (int i=0;i<8;++i) if (param->reserved2[i]) return IME_ERR_INVALID_RESERVED;
    pthread_mutex_lock(&lock);
    if (keyboard.open) { pthread_mutex_unlock(&lock); return IME_ERR_BUSY; }
    memset(&keyboard,0,sizeof(keyboard));
    keyboard.open=1; keyboard.param=*param;
    pthread_mutex_unlock(&lock);
    printf("Runtime: Ime keyboard opened (option %#x)\n",param->option);
    return 0;
}
static ABI int32_t ime_keyboard_close(int32_t user) {
    if (user==-1) return IME_ERR_INVALID_USER_ID;
    pthread_mutex_lock(&lock);
    const int was=keyboard.open; keyboard.open=0;
    pthread_mutex_unlock(&lock);
    return was ? 0 : IME_ERR_NOT_OPENED;
}
static ABI int32_t ime_keyboard_info(uint32_t resource, ImeKeyboardInfo *info) {
    if (!info) return IME_ERR_INVALID_ADDRESS;
    memset(info,0,sizeof(*info));
    pthread_mutex_lock(&lock);
    const int connected=keyboard.open && keyboard.connected_sent && resource==1;
    pthread_mutex_unlock(&lock);
    info->user_id=1; info->device=0; info->type=4; info->repeat_delay=1; info->repeat_rate=1;
    info->status=connected ? 1 : 0;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceImeParamInit",ime_param_init}, {"sceImeOpen",ime_open}, {"sceImeClose",ime_close},
    {"sceImeUpdate",ime_update}, {"sceImeSetText",ime_set_text}, {"sceImeSetCaret",ime_set_caret},
    {"sceImeGetPanelPositionAndForm",ime_panel_position},
    {"sceImeKeyboardOpen",ime_keyboard_open}, {"sceImeKeyboardClose",ime_keyboard_close},
    {"sceImeKeyboardGetInfo",ime_keyboard_info},
};
uintptr_t runtime_ime_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
