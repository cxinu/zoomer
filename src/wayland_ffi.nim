## Nim FFI bindings for the Wayland C backend (wayland_backend.c)

{.compile: "wayland_backend.c".}
{.compile: "xdg-shell-protocol.c".}
{.passL: "-lwayland-client -lwayland-egl -lEGL".}

type WaylandState* = distinct pointer

proc wl_backend_init*(windowed: cint): WaylandState {.importc, cdecl.}
proc wl_backend_swap_buffers*(state: WaylandState) {.importc, cdecl.}
proc wl_backend_poll_events*(state: WaylandState): cint {.importc, cdecl.}
proc wl_backend_dispatch*(state: WaylandState): cint {.importc, cdecl.}
proc wl_state_reset_frame*(state: WaylandState) {.importc, cdecl.}
proc wl_backend_prepare_read*(state: WaylandState): cint {.importc, cdecl.}
proc wl_backend_read_events*(state: WaylandState): cint {.importc, cdecl.}
proc wl_backend_cancel_read*(state: WaylandState) {.importc, cdecl.}
proc wl_backend_get_fd*(state: WaylandState): cint {.importc, cdecl.}
proc wl_backend_roundtrip*(state: WaylandState): cint {.importc, cdecl.}
proc wl_backend_destroy*(state: WaylandState) {.importc, cdecl.}

proc wl_state_width*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_height*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_configured*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_closed*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_pointer_x*(s: WaylandState): cfloat {.importc, cdecl.}
proc wl_state_pointer_y*(s: WaylandState): cfloat {.importc, cdecl.}
proc wl_state_button_pressed*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_button_just_pressed*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_button_just_released*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_scroll_delta*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_ctrl_held*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_output_rate*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_key_event_count*(s: WaylandState): cint {.importc, cdecl.}
proc wl_state_key_event_key*(s: WaylandState, index: cint): cint {.importc, cdecl.}
proc wl_state_key_event_state*(s: WaylandState, index: cint): cint {.importc, cdecl.}
