# Boomer

![](./demo.gif)

This is a fork of [tsoding/boomer](https://github.com/tsoding/boomer) for Zoomers (**Wayland users**).
It is a zooming tool for Linux designed for presentations, streams, and high-performance screen magnification.

## Installation

### Arch Linux (AUR)

```console
$ yay -S boomer-wayland-git

```

### Manual Build

#### 1. Install Dependencies

**Arch Linux:**

```console
$ sudo pacman -S wayland wayland-protocols mesa grim nim nimble

```

**Debian/Ubuntu:**

```console
$ sudo apt install libwayland-dev libwayland-egl1-mesa libegl1-mesa-dev grim nim

```

_Note: `grim` is required at runtime for screen capture on Wayland._

#### 2. Compile

To build for Wayland:

```console
$ nimble build -d:release -d:wayland

```

To build for X11:

```console
$ nimble build -d:release

```

> NOTE: to build this for x11 requires `libx11` `libxext` `libxrandr`.

## Usage

```console
$ ./boomer --help

```

## Configuration

Configuration file is located at `$HOME/.config/boomer/config` and has roughly the following format:

```
<param-1> = <value-1>
<param-2> = <value-2>
# comment
<param-3> = <value-3>
```

You can generate a new config at `$HOME/.config/boomer/config` with `$ boomer --new-config`.

Supported parameters:

| Name           | Description                                        |
| -------------- | -------------------------------------------------- |
| min_scale      | The smallest it can get when zooming out           |
| scroll_speed   | How quickly you can zoom in/out by scrolling       |
| drag_friction  | How quickly the movement slows down after dragging |
| scale_friction | How quickly the zoom slows down after scrolling    |

## Experimental Features Compilation Flags

Experimental or unstable features can be enabled by passing the following flags to `nimble build` command:

| Flag         | Description                                                                                                                    |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------ |
| `-d:wayland` | Build with native Wayland support instead of X11. Requires `grim` for screenshots.                                             |
| `-d:live`    | Live image update. See issue [#26].                                                                                            |
| `-d:mitshm`  | Enables faster Live image update using MIT-SHM X11 extension. Should be used along with `-d:live` to have an effect            |
| `-d:select`  | Application lets the user to click on te window to "track" and it will track that specific window instead of the whole screen. |

## Credits & Support

- Original Creator: [tsoding](https://github.com/rexim)
