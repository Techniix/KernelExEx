# kexhda.sys — Intel High Definition Audio (WDM, PortCls)

Our own driver from the Intel HD Audio specification 1.0a, for Windows 98 SE / ME:
Intel ICH6 to 8 Series HD Audio controllers with any HD Audio codec.

- Codec commands through CORB/RIRB (polled), Immediate Command as fallback.
- Walks the codec's widget graph: every analog speaker, headphone and line-out pin is
  routed to a DAC; all play one 48 kHz 16-bit stereo stream (KMixer converts the rest).
- Master volume and mute on the DACs' (else the pins') output amps; EAPD on.
- Headphones plugged in silence the speaker (pin sense, polled every 0.5 s).
- Codecs with only digital pins (HDMI) are skipped.

Target: Dell Vostro 1440 (Intel 5 Series 8086:3B56, IDT 92HD87B1/3: speaker pin 0x0D,
headphone pin 0x0B, DACs 0x13/0x14).

Tested in QEMU (intel-hda + hda-output): installs, starts, plays a WAV file.
Not yet tested on hardware. Diagnostics: HKLM\Software\KernelEx\Diag, value "kexhda".
Not yet: recording, S/PDIF, HDMI audio.
