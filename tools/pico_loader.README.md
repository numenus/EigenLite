# Pico Loader

`pico_loader.py` is a standalone Windows-friendly firmware loader for an Eigenharp Pico in pre-load mode.

It targets these USB IDs:

- pre-load: `2139:0001`
- fallback pre-load: `04b4:6473`
- post-load success check: `2139:0101` or `beca:0101`

## Windows use

Requirements:

- Python 3
- `pyusb`
- a libusb-compatible driver bound with Zadig to the pre-load device

Install `pyusb`:

```powershell
py -m pip install pyusb
```

Run from the `EigenLite/tools` directory:

```powershell
py .\pico_loader.py --list
py .\pico_loader.py
```

If the upload succeeds, the device should disconnect and come back with product ID `0101`.

If Windows then shows a new unknown device, run Zadig again on that new `0101` device and bind the same libusb-style driver to it.

## Firmware source

By default the script looks for:

- `EigenLite/eigenapi/resources/firmware/ihx/pico.ihx`
- then `EigenD/resources/pico.ihx`

You can override this with:

```powershell
py .\pico_loader.py --firmware C:\path\to\pico.ihx
```
