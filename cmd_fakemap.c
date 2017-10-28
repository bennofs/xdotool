#include "xdo_cmd.h"
#include <X11/keysym.h>
#include <X11/extensions/XKB.h>
#include <X11/extensions/XKBstr.h>
#include <X11/extensions/XKBrules.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <locale.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static void debug_dump_xkblayout(context_t* context, char* rules, XkbRF_VarDefsRec vardefs);
static void debug_dump_current_xbklayout(context_t* context, const char* desc);
static void debug_dump_charcodes(context_t* context, charcodemap_t* charcodes, unsigned int len);
static void restore_layout(void);
static int ignore_already_grabbed(Display *dpy, XErrorEvent *xerr);

static void exit_success(int unused) {
  (void)unused;
  exit(0);
}

static char* orig_layout = NULL;
static int (*orig_error_handler)(Display*, XErrorEvent*) = NULL;

static const int PRESERVED_MODS = ControlMask;

int cmd_fakemap(context_t *context) {
  int ret = 0;
  int c;
  char *cmd = *context->argv;
  const char *window_arg = NULL;

  typedef enum {
    opt_unused, opt_help, opt_window
  } optlist_t;

  struct option longopts[] = {
    { "help", no_argument, NULL, opt_help },
    { "window", required_argument, NULL, opt_window },
    { 0, 0, 0, 0 },
  };

  static const char *usage =
    "Usage: %s [--window windowid]"
    "--window <windowid>    - specify a window to send keys to\n"
    "-h, --help             - show this help output\n"
    HELP_SEE_WINDOW_STACK;
  int option_index;

  while ((c = getopt_long_only(context->argc, context->argv, "+w:d:ch",
                               longopts, &option_index)) != -1) {
    switch (c) {
      case opt_window:
        window_arg = optarg;
        break;
      case opt_help:
        printf(usage, cmd);
        consume_args(context, context->argc);
        return EXIT_SUCCESS;
        break;
      default:
        fprintf(stderr, usage, cmd);
        return EXIT_FAILURE;
    }
  }

  consume_args(context, optind);
  setlocale(LC_CTYPE, "");

  // read the original layout
  XkbRF_VarDefsRec orig_vardefs;
  char* orig_rules_file;
  XkbRF_GetNamesProp(context->xdo->xdpy, &orig_rules_file, &orig_vardefs);

  xdotool_debug(context, "original keyboard settings:");
  debug_dump_xkblayout(context, orig_rules_file, orig_vardefs);

  // restore original layout on exit
  orig_layout = orig_vardefs.layout;
  atexit(restore_layout);
  signal(SIGTERM, exit_success);
  signal(SIGINT, exit_success);

  // change the layout (disable deadkeys)
  int child;
  if(!(child = fork())) {
    execlp("setxkbmap","setxkbmap", "-variant", "nodeadkeys", NULL);
  }
  waitpid(child, NULL, 0);
  xdotool_debug(context, "disabled deadkeys");

  // refresh the keymap
  xdo_t* old_xdo = context->xdo;
  context->xdo = xdo_new_with_opened_display(old_xdo->xdpy, old_xdo->display_name, 1);
  old_xdo->close_display_when_freed = 0;
  xdo_free(old_xdo);

  // read original charcodes
  int orig_charcodes_len = context->xdo->charcodes_len;
  charcodemap_t* orig_charcodes = malloc(sizeof(charcodemap_t) * context->xdo->charcodes_len);
  memcpy(orig_charcodes, context->xdo->charcodes, sizeof(charcodemap_t) * context->xdo->charcodes_len);

  // change the layout
  // just let setxkbmap do it, changing the layout is non-trivial
  if(!(child = fork())) {
    execlp("setxkbmap", "-layout", "us", "-variant", "basic", NULL);
  }
  waitpid(child, NULL, 0);
  if(!(child = fork())) {
    execlp(
      "xmodmap", "xmodmap",
      "-e", "remove mod1 = Alt_R",
      "-e", "keysym Alt_R = ISO_Level3_Shift",
      "-e", "add mod5 = ISO_Level3_Shift",
      NULL
    );
  }
  waitpid(child, NULL, 0);

  debug_dump_current_xbklayout(context, "faked keyboard settings:");

  // refresh the keymap
  old_xdo = context->xdo;
  context->xdo = xdo_new_with_opened_display(old_xdo->xdpy, old_xdo->display_name, 1);
  old_xdo->close_display_when_freed = 0;
  xdo_free(old_xdo);

  // check which keys we need to rebind
  charcodemap_t** map_by_char = calloc(sizeof(charcodemap_t*), 1<<16);

  if(!map_by_char) {
    fprintf(stderr, "failed to allocate maps\n");
    exit(1);
  }

  for(int i = 0; i < context->xdo->charcodes_len; ++i) {
    charcodemap_t* map = &context->xdo->charcodes[i];
    if(iscntrl(map->key)) continue;

    // there may be more than one way to produce any given key
    charcodemap_t* curmap = map_by_char[map->key];
    if(map_by_char[map->key]) {
      if(iscntrl(map->key)) continue;

      // prefer "standard" bindings (they tend to have lower keysyms)
      if(curmap->symbol < map->symbol) continue;

      // prefer bindings with less modifiers
      if(curmap->modmask < map->modmask) continue;
    }
    map_by_char[map->key] = map;
  }

  // Grab all the keys that we need to remap
  Window root = XDefaultRootWindow(context->xdo->xdpy);
  xdotool_debug(context, "Grabbing keys for window %x", root);
  orig_error_handler = XSetErrorHandler(ignore_already_grabbed);
  XSync(context->xdo->xdpy, False);
  for(int i = 0; i < orig_charcodes_len; ++i) {
    charcodemap_t chr = orig_charcodes[i];
    charcodemap_t* map = map_by_char[chr.key];
    if(!map) {
      continue;
    }
    if(map->code == chr.code && map->modmask == chr.modmask) continue;
    xdotool_debug(
      context,
      "faking key %lc code %x symbol %x (%s) group %x modmask %x",
      chr.key, chr.code, chr.symbol, XKeysymToString(chr.symbol), chr.group, chr.modmask
    );
    XGrabKey(
      context->xdo->xdpy,
      chr.code,
      chr.modmask,
      root,
      False,
      GrabModeAsync, GrabModeSync
    );
    XGrabKey(
      context->xdo->xdpy,
      chr.code,
      chr.modmask | ControlMask,
      root,
      False,
      GrabModeAsync, GrabModeSync
    );
  }
  XSync(context->xdo->xdpy, False);
  XSetErrorHandler(orig_error_handler);

  while (True) {
    // read the next key event
    XKeyEvent key;
    {
      XEvent xevent;
      xdotool_debug(context, "Waiting for next event...");
      XNextEvent(context->xdo->xdpy, &xevent);

      if(xevent.type != KeyPress && xevent.type != KeyRelease) {
        continue;
      }
      key  = xevent.xkey;
    }

    Bool pressed = key.type == KeyPress;
    xdotool_debug(
      context,
      "Got key event type %x keycode %x state %x pressed %x send_event %x",
      key.type, key.keycode, key.state, pressed, key.send_event
    );

    // thaw the keyboard to allow more events
    XAllowEvents(context->xdo->xdpy, SyncKeyboard, key.time);

    // ignore our own events
    if(key.send_event) continue;

    // translate event
    for(int i = 0; i < orig_charcodes_len; ++i) {
      charcodemap_t chr = orig_charcodes[i];
      if(chr.code != key.keycode) continue;
      int relevant_modmask = key.state &~ PRESERVED_MODS;
      if(relevant_modmask != chr.modmask) continue;

      charcodemap_t* map = map_by_char[chr.key];
      if(!map) {
        printf("unknown key %lc\n", chr.key);
        continue;
      }

      xdotool_debug(
        context,
        "translating key %lc code %x symbol %x (%s) group %x modmask %x",
        chr.key, chr.code, chr.symbol, XKeysymToString(chr.symbol), chr.group, chr.modmask
      );

      xdotool_debug(
        context,
        "-> to key %lc code %x symbol %x (%s) group %x modmask %x",
        map->key, map->code, map->symbol, XKeysymToString(map->symbol), map->group, map->modmask
      );

      Window focuswin = 0;
      xdo_get_focused_window(context->xdo, &focuswin);
      key.window = focuswin;

      XKeyEvent shift = key;
      if((key.state & ShiftMask) != (map->modmask & ShiftMask)) {
        Bool shift_down = (map->modmask & ShiftMask) ^ !pressed;
        shift.type = shift_down ? KeyPress : KeyRelease;
        int mask = shift_down ? KeyPressMask : KeyReleaseMask;
        shift.state = 0;
        shift.keycode = XKeysymToKeycode(context->xdo->xdpy, XK_Shift_L);
        xdotool_debug(context, "send shift %d", shift_down);
        XSendEvent(context->xdo->xdpy, focuswin, True, mask, (XEvent*)&shift);
      }

      if((key.state & 0x80) != (map->modmask & 0x80)) {
        Bool shift_down = (map->modmask & Mod5Mask) ^ !pressed;
        shift.type = shift_down ? KeyPress : KeyRelease;
        int mask = shift_down ? KeyPressMask : KeyReleaseMask;
        shift.state = 0;
        shift.keycode = XKeysymToKeycode(context->xdo->xdpy, XK_ISO_Level3_Shift);
        xdotool_debug(context, "send altgr %d", shift_down);
        XSendEvent(context->xdo->xdpy, focuswin, True, mask, (XEvent*)&shift);
      }

      key.keycode = map->code;
      key.state = map->modmask | (key.state & PRESERVED_MODS);
      XSendEvent(context->xdo->xdpy, focuswin, True, pressed ? KeyPressMask : KeyReleaseMask, (XEvent*)&key);
      break;
    }

  }

  return ret > 0;
}

static void debug_dump_xkblayout(context_t* context, char* rules, XkbRF_VarDefsRec vardefs) {
  xdotool_debug(
    context,
    " rules: %s\n"
    " model: %s\n"
    " layout: %s\n"
    " variant: %s\n"
    " options: %s"
    , rules, vardefs.model, vardefs.layout, vardefs.variant, vardefs.options
  );
}

static void debug_dump_current_xbklayout(context_t* context, const char* desc) {
  char* rules;
  XkbRF_VarDefsRec vardefs;
  XkbRF_GetNamesProp(context->xdo->xdpy, &rules, &vardefs);
  xdotool_debug(context, desc);
  debug_dump_xkblayout(context, rules, vardefs);
}


static void debug_dump_charcodes(context_t* context, charcodemap_t* charcodes, unsigned int len) {
  for(unsigned int i = 0; i < len; ++i) {
    charcodemap_t chr = charcodes[i];
    if(iscntrl(chr.key)) continue;
    xdotool_debug(context, "key %lc code %x symbol %x group %x modmask %x", chr.key, chr.code, chr.symbol, chr.group, chr.modmask);
  }

}

static void restore_layout(void) {
  if(!orig_layout) return;

  int child;
  if(!(child = fork())) {
    execlp("setxkbmap", "-layout", orig_layout, NULL);
  }
  waitpid(child, NULL, 0);
}


static int ignore_already_grabbed(Display *dpy, XErrorEvent *xerr) {
  (void)dpy;
  if(xerr->error_code != BadAccess) return orig_error_handler(dpy, xerr);
  fprintf(stderr, "some grab failed because key is already grabbed (ignoring)\n");
  return 1;
}
