# TODO

- [x] SD card mounted and self-tested
- [x] Save photos to the SD card (tap anywhere = shutter)
- [ ] Check saved JPEG colours on a PC (RGB565 byte order into the JPEG engine unverified)
- [ ] On-screen controls beyond tap-to-shoot (shutter button, exposure, review last shot)
- [ ] Full-resolution still capture (sensor mode switch from the 1920x1520 preview mode)
- [ ] Colour-temperature-indexed CCM: AWB zone model with `export_ct`, using the six libcamera CCMs
- [ ] Confirm whether ISP AWB stats are sampled before or after the WB gains, then tighten the AWB box
- [ ] 12-bit mode
- [ ] Canon EF/EF-S lenses via the Pinefeat CEF168 AF adapter (I2C 0x0d); power rails not yet read from its docs
