# mongolwm

mongolwm is an experimental Wayland compositor forked from [MangoWM](https://github.com/DreamMaoMao/mangowc). It keeps the minimalist, tiling-first feel of its parent and adds something different: an **infinite canvas** where windows aren't boxed into your monitor, you just place them wherever you like and move the camera around.

It's built on `wlroots` and `scenefx`, so you also get rounded corners, blur, shadows and smooth animations.

> Experimental software. Things will change and things will break.

## What makes it different

### Infinite canvas (`setlayout,canvas`)

Windows live on an unbounded 2D plane. They keep their real, unscaled client resolution, and the GPU handles the scaling when you zoom, so nothing gets re-laid-out or resized behind your back.

- **Camera gliding.** Move focus with `focusdir` or `focusstack` and the view smoothly pans to center the window you landed on.
- **Cursor-centered zoom.** Zoom from 0.15x to 3.0x, with exponential easing at 120Hz, anchored to wherever your mouse is.
- **Free positioning.** Drag and resize windows anywhere on the canvas.

### Overview mode (`toggleoverview`)

A zoomable bird's-eye view of everything on your canvas. Entering and leaving the overview animates windows from and to their actual positions. You can drag windows around in it to reorganize, click one to jump to it, or right-click to close it.

### Concentric double borders

Inspired by 2bwm. There's an inner border (`border_px`) that reflects window state (focused, unfocused, urgent, scratchpad, global, overlay) and a separate outer border (`outer_border_px`) wrapped around it. It works with rounded corners (`border_radius`) and uses hardware-accelerated clipping.

### Effects (via SceneFX)

Dual-kawase blur, drop shadows, window dimming, opacity fading, and custom cubic-bezier animation curves.

### 15 built-in layouts

`canvas`, `tile`, `scroller`, `dwindle`, `monocle`, `grid`, `deck`, `center_tile`, `right_tile`, `fair`, and the vertical variants: `vertical_scroller`, `vertical_tile`, `vertical_grid`, `vertical_deck`, `vertical_fair`.

## Building

The build process is the same as MangoWM's. It uses Meson and Ninja.

You'll need the usual Wayland compositor toolchain, including `wlroots` and `scenefx`, plus `meson` and `ninja`. Package names vary by distro.

```sh
git clone https://github.com/avudn/mongolwm.git
cd mongolwm
meson setup build
ninja -C build
sudo ninja -C build install
```

If you'd rather not install system-wide, run the binary straight from `build/` instead of the last step.

## Building

The build process is the same as MangoWM's. You need the dependencies below, plus `wlroots` and `scenefx` built from source (unless your distro packages the right versions).

### Dependencies

- wayland
- wayland-protocols
- libinput
- libdrm
- libxkbcommon
- pixman
- libdisplay-info
- libliftoff
- hwdata
- seatd
- pcre2
- pango
- cjson
- xorg-xwayland
- libxcb

You'll also need `git`, `meson` and `ninja` to fetch and build everything.

### 1. Build wlroots

```bash
git clone -b 0.19.2 https://gitlab.freedesktop.org/wlroots/wlroots.git
cd wlroots
meson build -Dprefix=/usr
sudo ninja -C build install
```

### 2. Build scenefx

This is the library that provides blur, shadows and rounded corners.

```bash
git clone -b 0.4.1 https://github.com/wlrfx/scenefx.git
cd scenefx
meson build -Dprefix=/usr
sudo ninja -C build install
```

### 3. Build mongolwm

```bash
git clone https://github.com/avudn/mongolwm.git
cd mongolwm
meson build -Dprefix=/usr
sudo ninja -C build install
```

The wlroots and scenefx versions above are the ones MangoWM currently asks for. If they change upstream, mongolwm will most likely need the same bump.

## Controls

### Canvas mode

| Action | Binding |
| --- | --- |
| Pan the canvas | Middle-click + drag anywhere |
| Reset camera (1.0x, origin) | Middle-click without dragging |
| Zoom in / out | `Super` + scroll wheel (zooms at cursor) |
| Move a window | `Super` + left-click + drag (`moveresize,curmove`) |
| Resize a window | `Super` + right-click + drag (`moveresize,curresize`) |
| Focus in a direction and glide | `focusdir,left/right/up/down` |
| Cycle focus and glide | `focusstack,next` / `focusstack,prev` |

### Overview mode

| Action | Binding |
| --- | --- |
| Pan the overview | Middle-click + drag |
| Reset view / re-grid cards | Middle-click (first click resets the camera, second resets the layout) |
| Zoom in / out | Scroll wheel (zooms at cursor) |
| Move a window card | Left-click + drag on a card |
| Select a window and exit | Left-click on a card (no drag) |
| Close a window | Right-click on a card |

## Example config

Here's a starting point for `config.conf`:

```ini
# ==========================================
# Double Borders
# ==========================================
border_px=6
outer_border_px=2
border_radius=6
border_color=0x3b4261ff
focus_color=0x7aa2f7ff
outer_border_color=0x16161eff
urgent_color=0xf7768eff

# ==========================================
# Layout & Overview Keybindings
# ==========================================
bind=SUPER,c,setlayout,canvas
bind=SUPER,t,setlayout,tile
bind=SUPER,s,setlayout,scroller
bind=SUPER,d,setlayout,dwindle
bind=SUPER,Tab,toggleoverview

# ==========================================
# Directional Navigation (Camera Gliding)
# ==========================================
bind=SUPER,Left,focusdir,left
bind=SUPER,Right,focusdir,right
bind=SUPER,Up,focusdir,up
bind=SUPER,Down,focusdir,down

# ==========================================
# Window Cycling & Mouse Bindings
# ==========================================
bind=SUPER,j,focusstack,next
bind=SUPER,k,focusstack,prev

mousebind=SUPER,btn_left,moveresize,curmove
mousebind=SUPER,btn_right,moveresize,curresize
```

Colors are `0xRRGGBBAA`. The border colors above are a Tokyo Night-ish palette, so change them to whatever you like.

## Credits

mongolwm is a fork of MangoWM and stands on the work of the `wlroots` and `scenefx` projects. The double-border style comes from 2bwm.
