#include <EGL/egl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "wlr-layer-shell-protocol.h"
#include "xdg-shell-protocol.h"

#define MAX_KEY_EVENTS 16

/* ── Wayland state exposed to Nim ── */

typedef struct {
  /* Wayland core */
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct wl_output *output;
  struct wl_surface *surface;

  /* xdg-shell */
  struct xdg_wm_base *wm_base;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;

  /* layer-shell (used for seamless fullscreen overlay) */
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwlr_layer_surface_v1 *layer_surface;

  /* EGL */
  struct wl_egl_window *egl_window;
  EGLDisplay egl_display;
  EGLContext egl_context;
  EGLSurface egl_surface;
  EGLConfig egl_config;

  /* state */
  int width;
  int height;
  int configured;
  int closed;
  int windowed;

  /* input state – updated by callbacks, read by Nim */
  float pointer_x;
  float pointer_y;
  int button_pressed; /* left button currently held */
  int button_just_pressed;
  int button_just_released;
  int scroll_delta; /* +1 up, -1 down per frame */
  int ctrl_held;

  /* key event queue for this frame */
  struct {
    int key;
    int state; /* 1=press 0=release */
  } key_events[MAX_KEY_EVENTS];
  int key_event_count;
  int key_read_index; /* cursor for Nim to iterate */

  /* output info */
  int output_rate; /* refresh rate in mHz */
} WaylandState;

/* ── Forward declarations for listeners ── */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version);
static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name);

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* xdg_wm_base */
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial) {
  xdg_wm_base_pong(wm_base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping =
                                                                 wm_base_ping};

/* xdg_surface */
static void xdg_surface_configure_handler(void *data,
                                          struct xdg_surface *xdg_surface,
                                          uint32_t serial) {
  WaylandState *state = (WaylandState *)data;
  xdg_surface_ack_configure(xdg_surface, serial);
  state->configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener_obj = {
    .configure = xdg_surface_configure_handler,
};

/* xdg_toplevel */
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states) {
  WaylandState *state = (WaylandState *)data;
  if (width > 0 && height > 0) {
    state->width = width;
    state->height = height;
    if (state->egl_window) {
      wl_egl_window_resize(state->egl_window, width, height, 0, 0);
    }
  }
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
  WaylandState *state = (WaylandState *)data;
  state->closed = 1;
}
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                      int32_t width, int32_t height) {
  /* no-op */
}
static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                     struct wl_array *capabilities) {
  /* no-op */
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

/* pointer */
static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx,
                          wl_fixed_t sy) {
  WaylandState *state = (WaylandState *)data;
  state->pointer_x = wl_fixed_to_double(sx);
  state->pointer_y = wl_fixed_to_double(sy);
}
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *surface) {}
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
  WaylandState *state = (WaylandState *)data;
  state->pointer_x = wl_fixed_to_double(sx);
  state->pointer_y = wl_fixed_to_double(sy);
}
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t btn_state) {
  WaylandState *state = (WaylandState *)data;
  if (button == BTN_LEFT) {
    if (btn_state == WL_POINTER_BUTTON_STATE_PRESSED) {
      state->button_pressed = 1;
      state->button_just_pressed = 1;
    } else {
      state->button_pressed = 0;
      state->button_just_released = 1;
    }
  }
}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
  WaylandState *state = (WaylandState *)data;
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    if (wl_fixed_to_double(value) < 0)
      state->scroll_delta += 1; /* scroll up */
    else
      state->scroll_delta -= 1; /* scroll down */
  }
}
static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p,
                                uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                              uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
                                  uint32_t axis, int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* keyboard */
static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                            int fd, uint32_t size) {}
static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface) {}
static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state_val) {
  WaylandState *state = (WaylandState *)data;
  if (state->key_event_count < MAX_KEY_EVENTS) {
    state->key_events[state->key_event_count].key = key;
    state->key_events[state->key_event_count].state =
        (state_val == WL_KEYBOARD_KEY_STATE_PRESSED) ? 1 : 0;
    state->key_event_count++;
  }
}
static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
  WaylandState *state = (WaylandState *)data;
  /* bit 2 = Control */
  state->ctrl_held = (mods_depressed & 4) ? 1 : 0;
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                 int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* seat */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  WaylandState *state = (WaylandState *)data;
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !state->pointer) {
    state->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(state->pointer, &pointer_listener, state);
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !state->keyboard) {
    state->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
  }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* output – for refresh rate */
static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {}
static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
  WaylandState *state = (WaylandState *)data;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    state->output_rate = refresh; /* in mHz */
    if (state->width == 0) {
      state->width = width;
      state->height = height;
    }
  }
}
static void output_done(void *data, struct wl_output *output) {}
static void output_scale(void *data, struct wl_output *output, int32_t factor) {
}
static void output_name(void *data, struct wl_output *output,
                        const char *name) {}
static void output_description(void *data, struct wl_output *output,
                               const char *desc) {}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

/* registry */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version) {
  WaylandState *state = (WaylandState *)data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    state->compositor =
        wl_registry_bind(reg, name, &wl_compositor_interface, 4);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    state->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(state->wm_base, &wm_base_listener, state);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    state->layer_shell =
        wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    state->seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
    wl_seat_add_listener(state->seat, &seat_listener, state);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    if (!state->output) {
      state->output = wl_registry_bind(reg, name, &wl_output_interface, 4);
      wl_output_add_listener(state->output, &output_listener, state);
    }
  }
}
static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {}

/* layer-shell callbacks */
static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
  WaylandState *state = (WaylandState *)data;
  state->width = width;
  state->height = height;
  state->configured = 1;
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  if (state->egl_window) {
    wl_egl_window_resize(state->egl_window, width, height, 0, 0);
  }
}
static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
  WaylandState *state = (WaylandState *)data;
  state->closed = 1;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ── Public API for Nim ── */

WaylandState *wl_backend_init(int windowed) {
  WaylandState *state = calloc(1, sizeof(WaylandState));
  if (!state)
    return NULL;

  state->display = wl_display_connect(NULL);
  if (!state->display) {
    fprintf(stderr, "Failed to connect to Wayland display\n");
    free(state);
    return NULL;
  }

  state->registry = wl_display_get_registry(state->display);
  wl_registry_add_listener(state->registry, &registry_listener, state);
  wl_display_roundtrip(state->display); /* get globals */
  wl_display_roundtrip(state->display); /* get seat capabilities, output info */

  if (!state->compositor || !state->wm_base) {
    fprintf(
        stderr,
        "Missing required Wayland interfaces (compositor or xdg_wm_base)\n");
    wl_display_disconnect(state->display);
    free(state);
    return NULL;
  }

  state->windowed = windowed;

  /* Create surface */
  state->surface = wl_compositor_create_surface(state->compositor);

  if (!windowed && state->layer_shell) {
    /* Layer-shell overlay: no window management, instant fullscreen */
    state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state->layer_shell, state->surface, state->output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "boomer");
    zwlr_layer_surface_v1_set_anchor(state->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(state->layer_surface,
                                       &layer_surface_listener, state);
  } else {
    /* xdg-shell: for windowed mode or fallback */
    state->xdg_surface =
        xdg_wm_base_get_xdg_surface(state->wm_base, state->surface);
    xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener_obj,
                             state);
    state->xdg_toplevel = xdg_surface_get_toplevel(state->xdg_surface);
    xdg_toplevel_add_listener(state->xdg_toplevel, &toplevel_listener, state);
    xdg_toplevel_set_title(state->xdg_toplevel, "boomer");
    xdg_toplevel_set_app_id(state->xdg_toplevel, "boomer");
    if (!windowed) {
      xdg_toplevel_set_fullscreen(state->xdg_toplevel, state->output);
    }
  }

  /* Commit with no buffer attached — triggers configure event.
   * The surface only becomes visible on the first eglSwapBuffers. */
  wl_surface_commit(state->surface);
  wl_display_roundtrip(state->display); /* get configure */

  /* Use screen size as default if configure didn't provide dimensions */
  if (state->width == 0)
    state->width = 1920;
  if (state->height == 0)
    state->height = 1080;

  /* EGL setup */
  state->egl_display = eglGetDisplay((EGLNativeDisplayType)state->display);
  if (state->egl_display == EGL_NO_DISPLAY) {
    fprintf(stderr, "Failed to get EGL display\n");
    return NULL;
  }

  EGLint major, minor;
  if (!eglInitialize(state->egl_display, &major, &minor)) {
    fprintf(stderr, "Failed to initialize EGL\n");
    return NULL;
  }

  if (!eglBindAPI(EGL_OPENGL_API)) {
    fprintf(stderr, "Failed to bind OpenGL API\n");
    return NULL;
  }

  EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                             EGL_WINDOW_BIT,
                             EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_BIT,
                             EGL_RED_SIZE,
                             8,
                             EGL_GREEN_SIZE,
                             8,
                             EGL_BLUE_SIZE,
                             8,
                             EGL_ALPHA_SIZE,
                             8,
                             EGL_DEPTH_SIZE,
                             24,
                             EGL_NONE};

  EGLint num_configs;
  eglChooseConfig(state->egl_display, config_attribs, &state->egl_config, 1,
                  &num_configs);
  if (num_configs == 0) {
    fprintf(stderr, "Failed to choose EGL config\n");
    return NULL;
  }

  EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                              3,
                              EGL_CONTEXT_MINOR_VERSION,
                              3,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK,
                              EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                              EGL_NONE};

  state->egl_context = eglCreateContext(state->egl_display, state->egl_config,
                                        EGL_NO_CONTEXT, context_attribs);
  if (state->egl_context == EGL_NO_CONTEXT) {
    /* Fall back to compat profile */
    EGLint fallback_attribs[] = {EGL_NONE};
    state->egl_context = eglCreateContext(state->egl_display, state->egl_config,
                                          EGL_NO_CONTEXT, fallback_attribs);
    if (state->egl_context == EGL_NO_CONTEXT) {
      fprintf(stderr, "Failed to create EGL context\n");
      return NULL;
    }
  }

  state->egl_window =
      wl_egl_window_create(state->surface, state->width, state->height);
  if (!state->egl_window) {
    fprintf(stderr, "Failed to create EGL window\n");
    return NULL;
  }

  state->egl_surface =
      eglCreateWindowSurface(state->egl_display, state->egl_config,
                             (EGLNativeWindowType)state->egl_window, NULL);
  if (state->egl_surface == EGL_NO_SURFACE) {
    fprintf(stderr, "Failed to create EGL surface\n");
    return NULL;
  }

  eglMakeCurrent(state->egl_display, state->egl_surface, state->egl_surface,
                 state->egl_context);

  /* Enable vsync – frame pacing via compositor */
  eglSwapInterval(state->egl_display, 1);

  /* Init key event queue */
  state->key_event_count = 0;
  state->key_read_index = 0;

  printf("Screen rate: %d\n",
         state->output_rate > 0 ? state->output_rate / 1000 : 60);

  return state;
}

void wl_backend_swap_buffers(WaylandState *state) {
  eglSwapBuffers(state->egl_display, state->egl_surface);
}

int wl_backend_poll_events(WaylandState *state) {
  /* Flush outgoing requests to compositor */
  if (wl_display_flush(state->display) == -1) {
    return -1;
  }

  /* Try to read new events from the socket */
  if (wl_display_prepare_read(state->display) == 0) {
    struct pollfd pfd = {.fd = wl_display_get_fd(state->display),
                         .events = POLLIN};
    if (poll(&pfd, 1, 0) > 0) {
      wl_display_read_events(state->display);
    } else {
      wl_display_cancel_read(state->display);
    }
  }

  /* Dispatch all queued events */
  return wl_display_dispatch_pending(state->display);
}

void wl_state_reset_frame(WaylandState *state) {
  state->button_just_pressed = 0;
  state->button_just_released = 0;
  state->scroll_delta = 0;
  state->key_event_count = 0;
  state->key_read_index = 0;
}

/* Legacy compat – keep for any code that still calls these */
int wl_backend_dispatch(WaylandState *state) {
  return wl_display_dispatch_pending(state->display);
}

int wl_backend_prepare_read(WaylandState *state) {
  return wl_display_prepare_read(state->display);
}

int wl_backend_read_events(WaylandState *state) {
  return wl_display_read_events(state->display);
}

void wl_backend_cancel_read(WaylandState *state) {
  wl_display_cancel_read(state->display);
}

int wl_backend_get_fd(WaylandState *state) {
  return wl_display_get_fd(state->display);
}

int wl_backend_roundtrip(WaylandState *state) {
  return wl_display_roundtrip(state->display);
}

void wl_backend_destroy(WaylandState *state) {
  if (!state)
    return;

  if (state->egl_surface != EGL_NO_SURFACE)
    eglDestroySurface(state->egl_display, state->egl_surface);
  if (state->egl_window)
    wl_egl_window_destroy(state->egl_window);
  if (state->egl_context != EGL_NO_CONTEXT)
    eglDestroyContext(state->egl_display, state->egl_context);
  if (state->egl_display != EGL_NO_DISPLAY)
    eglTerminate(state->egl_display);

  if (state->layer_surface)
    zwlr_layer_surface_v1_destroy(state->layer_surface);
  if (state->layer_shell)
    zwlr_layer_shell_v1_destroy(state->layer_shell);
  if (state->xdg_toplevel)
    xdg_toplevel_destroy(state->xdg_toplevel);
  if (state->xdg_surface)
    xdg_surface_destroy(state->xdg_surface);
  if (state->surface)
    wl_surface_destroy(state->surface);
  if (state->pointer)
    wl_pointer_destroy(state->pointer);
  if (state->keyboard)
    wl_keyboard_destroy(state->keyboard);
  if (state->seat)
    wl_seat_destroy(state->seat);
  if (state->wm_base)
    xdg_wm_base_destroy(state->wm_base);
  if (state->output)
    wl_output_destroy(state->output);
  if (state->compositor)
    wl_compositor_destroy(state->compositor);
  if (state->registry)
    wl_registry_destroy(state->registry);
  if (state->display)
    wl_display_disconnect(state->display);

  free(state);
}

/* Getters for Nim */
int wl_state_width(WaylandState *s) { return s->width; }
int wl_state_height(WaylandState *s) { return s->height; }
int wl_state_configured(WaylandState *s) { return s->configured; }
int wl_state_closed(WaylandState *s) { return s->closed; }
float wl_state_pointer_x(WaylandState *s) { return s->pointer_x; }
float wl_state_pointer_y(WaylandState *s) { return s->pointer_y; }
int wl_state_button_pressed(WaylandState *s) { return s->button_pressed; }
int wl_state_button_just_pressed(WaylandState *s) {
  return s->button_just_pressed;
}
int wl_state_button_just_released(WaylandState *s) {
  return s->button_just_released;
}
int wl_state_scroll_delta(WaylandState *s) { return s->scroll_delta; }
int wl_state_ctrl_held(WaylandState *s) { return s->ctrl_held; }
int wl_state_output_rate(WaylandState *s) {
  return s->output_rate > 0 ? s->output_rate / 1000 : 60;
}

/* Key event queue iteration for Nim */
int wl_state_key_event_count(WaylandState *s) { return s->key_event_count; }
int wl_state_key_event_key(WaylandState *s, int index) {
  if (index >= 0 && index < s->key_event_count)
    return s->key_events[index].key;
  return -1;
}
int wl_state_key_event_state(WaylandState *s, int index) {
  if (index >= 0 && index < s->key_event_count)
    return s->key_events[index].state;
  return 0;
}
