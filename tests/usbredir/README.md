# Simulated USB devices for the QEMU test VM

`padsim.c` is a wired Xbox 360 controller (the real one's descriptors, all
four interfaces) that QEMU's `usb-redir` device plugs into the Windows 98
VM, so the controller path (New Hardware wizard, `kexpad.sys`, the HID game
controller, the Game Controllers test page) is tested without hardware.
It uses libusbredirparser (LGPL) and is a test tool only; nothing of it is
shipped.

    gcc -O1 -o padsim padsim.c -lusbredirparser -lm
    ./padsim listen:/tmp/pad.sock 2dc8:310a "8BitDo Ultimate 2C" <pad.ctl &
    # in QEMU's monitor (hot plug = the pad being plugged in):
    chardev-add socket,id=pad,path=/tmp/pad.sock
    device_add usb-redir,chardev=pad,id=pad0

By itself it presses A every other second and turns the left stick round;
lines written to its standard input take over: `a 1`, `a 0`, `x -30000`,
`y 12000`, `auto`, `quit`. Quitting unplugs it. The log shows every request
Windows makes and every packet the driver sends (LED, rumble).

Plug it in after Windows has started: a `usb-redir` device present from boot
is not needed, and hot plugging is what a user does.
