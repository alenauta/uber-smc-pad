# Optional: copy to the Pico as main.py for standalone use (start button on
# config.PIN_BUTTON). This is the RECOMMENDED way: Pico powered from a separate
# 5 V supply, with no serial traffic on the pad's USB hub (see ../README.md).
# Without a button, leave this file off the Pico and call usbkey.run() from the
# REPL (e.g. `mpremote exec "import usbkey; usbkey.run()"`).
import config
import usbkey

if config.PIN_BUTTON is not None:
    usbkey.button_loop()
