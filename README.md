# hypr-autoclicker

Native Linux/Wayland autoclicker for Hyprland. Creates a virtual mouse device
through `/dev/uinput`, so it works without X11-only tools like `xdotool`.

## Features

- Left, right, or middle mouse button clicking
- Configurable interval and hold duration per click
- Optional click-count and time limits
- Stops cleanly on `Ctrl-C` or `SIGTERM`

## Requirements

- Linux kernel with `uinput` support
- Hyprland or any Wayland compositor that accepts kernel input devices
- GCC and Make
- Write access to `/dev/uinput` (see [Permissions](#permissions))

## Build

```sh
make
```

This produces `./autoclicker`. Warnings (`-Wall -Wextra -Wpedantic`) are
enabled by default in the Makefile.

## Install

```sh
sudo make install
```

Installs the binary to `/usr/local/bin/autoclicker`.

## Permissions

The program needs write access to `/dev/uinput`.

The simplest option is running it with `sudo`:

```sh
sudo ./autoclicker
```

For non-root usage, add your user to the `input` group and create a udev rule:

```sh
sudo usermod -aG input "$USER"
sudo tee /etc/udev/rules.d/99-uinput.rules >/dev/null <<'EOF'
KERNEL=="uinput", GROUP="input", MODE="0660"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Log out and back in after changing groups.

## Usage

```sh
sudo ./autoclicker [OPTIONS]
```

| Option | Description | Default |
|--------|-------------|---------|
| `-i <ms>` | Interval between clicks in milliseconds | `100` |
| `-b <button>` | Button to click: `left`, `right`, or `middle` | `left` |
| `-d <ms>` | Hold duration per click in milliseconds (must be ≤ interval) | `10` |
| `-c <count>` | Stop after N clicks; `0` = infinite | `0` |
| `-t <seconds>` | Stop after T seconds; `0` = infinite | `0` |
| `-h` | Show help | |

Examples:

```sh
# Click every 50 ms
sudo ./autoclicker -i 50

# 25 right-clicks, 200 ms apart
sudo ./autoclicker -b right -i 200 -c 25

# Middle-click for 10 seconds, 20 ms hold per click
sudo ./autoclicker -b middle -i 100 -d 20 -t 10
```

Stop an infinite run with `Ctrl-C`.

## Safety Notes

This program sends real mouse button events through a virtual input device.
Test with a small click limit (e.g. `-c 10`) before running indefinitely.

Some games, websites, or applications prohibit automated input. Use accordingly.

## Development

Run a stricter compile check:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -o /tmp/autoclicker-check autoclicker.c
```
