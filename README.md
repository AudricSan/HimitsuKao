# HimitsuKao (秘密顔)

An OBS Studio video filter for IRL streamers that **blurs every face that is not in your list of known people**, live, before the picture reaches your stream.

You manage the list from your phone during the live: tap a face that is on screen right now to mark that person as known, swipe them away to blur them again.

> Status: early development (v0.1.0). Not ready for use yet.

## Planned features

- Face detection (YuNet) and face recognition (SFace) running on the CPU through OpenCV, with no GPU or CUDA required.
- Face tracking, so a recognised person stays visible while they move.
- Mask styles: pixelate, gaussian blur or solid colour, with adjustable strength.
- A small web page served by the plugin for your phone (meant to be reached over a private network such as Tailscale, protected by a PIN):
  - thumbnails of the faces on screen, tap one to add that person;
  - list of known people, remove one in a tap;
  - "this live only" people, forgotten when OBS closes;
  - emergency "blur everything" and "blurring off" switches.

## Building

HimitsuKao is based on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

Windows (Visual Studio 2022, CMake 3.28 or later):

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Licence

GPL-2.0-or-later, see [LICENSE](LICENSE).
