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

To auto start it with Hyprland, add the following to `$HOME/.config/hypr/hyprland.lua`

```lua
hl.exec_cmd("sleep 3s; hyprctl plugin load /full/qualified/path/to/hyprselect.so")
```

## Manual starting

Or you can load it manually: 

`hyprctl plugin load /full/qualified/path/to/hyprselect.so`.

And unload it with: 

`hyprctl plugin unload /full/qualified/path/to/hyprselect.so`.

## Config Variables

If you want to customize, you can add the following to `$HOME/.config/hypr/hyprland.lua`

```lua
hl.config({
    plugin = {
        hyprselect = {
            should_round = false,

            col = {
                main   = "rgba(0085e625)",
                border = "rgba(0085e6ff)",
            },

            fade_time_ms = 65.0,

            should_blur = false,
            blur_power = 1.0,  -- range: 0.0 -> 1.0

            border_size = -1.0,  -- negative number means automatic
            rounding = 6,
            rounding_power = 2.0,
        }
    }
})
```

