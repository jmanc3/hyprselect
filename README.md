# HyprSelect

HyprSelect adds a desktop selection box to Hyprland, that does absolutely nothing, but is absolutely essential.

![Image of desktop selection on empty desktop](https://github.com/user-attachments/assets/309bacff-a539-466f-83fa-b92b71dd8567)

## Installation

```bash
git clone https://github.com/jmanc3/hyprselect
cd hyprselect
make
```

That should create `hyprselect.so`.

To auto start it with Hyprland, add the following to `$HOME/.config/hypr/hyprland.conf`

```bash
exec-once = hyprctl plugin load /full/qualified/path/to/hyprselect.so
```

## Manual starting

Or you can load it manually: 

`hyprctl plugin load /full/qualified/path/to/hyprselect.so`.

And unload it with: 

`hyprctl plugin unload /full/qualified/path/to/hyprselect.so`.

## Config Variables

If you want to customize, you can add the following to `$HOME/.config/hypr/hyprland.conf`

```ini
plugin:hyprselect:should_round = false
plugin:hyprselect:col.main = rgba(0085e625)
plugin:hyprselect:col.border = rgba(0085e6ff)

plugin:hyprselect:fade_time_ms = 65.0

plugin:hyprselect:border_size = -1.0  # negative number means automatic
plugin:hyprselect:rounding = 6
plugin:hyprselect:rounding_power = 2.0
```

