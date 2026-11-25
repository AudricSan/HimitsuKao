# HimitsuKao (秘密顔)

An OBS Studio video filter for IRL streamers that **blurs every face that is not in your list of known people**, live, before the picture reaches your stream.

You manage the list from your phone during the live: tap a face that is on screen right now to mark that person as known, swipe them away to blur them again.

> Status: early development (v0.1.0). Not ready for use yet.

## Planned features

- Face detection (YuNet) and face recognition (SFace) running on the CPU through OpenCV, with no GPU or CUDA required.
- Face tracking, so a recognised person stays visible while they move.
- Mask styles: pixelate, gaussian blur or solid colour, with adjustable strength.
- A small web page served by the plugin for your phone (meant to be reached over a private network such as Tailscale, protected by an access code):
  - thumbnails of the faces on screen, tap one to add that person;
  - list of known people, remove one in a tap;
  - "this live only" people, forgotten when OBS closes;
  - emergency "blur everything" and "blurring off" switches.

## Phone page and security

The management page is also shown **inside OBS** as the *HimitsuKao* dock (when OBS's browser panel support is available), and can be reached from your phone.

The **phone page is off by default**. While it is off, the server listens on `127.0.0.1` only, so only the OBS dock on this PC can reach it. Tick *Enable the phone page* in the filter properties to listen on the network (the server rebinds immediately, no restart needed); untick it to go back to loopback only.

- **Trusted networks only.** The server answers only clients on loopback, private networks (10/8, 172.16/12, 192.168/16), Tailscale/CGNAT (100.64/10) and IPv6 unique-local addresses. Every other address gets a 403, and requests whose `Host` is not an IP address or `localhost` are refused (DNS rebinding protection). Tailscale (or a LAN you trust) is the recommended way to reach it from your phone.
- **Plain HTTP, no TLS.** The access code travels in clear text on that network. Do not use the page on an untrusted Wi-Fi.
- **Never expose it to the internet:** no router port forwarding, no Tailscale Funnel, no public reverse proxy.
- **Access code.** A random 10-character code is generated on first start and shown (hidden by default, with a *Show code* button) in the filter properties, together with the address to type on the phone. The one-tap address `http://<ip>:<port>/#code=<code>` stores the code in the phone browser and removes it from the URL. Regenerate it from the same properties page if it leaks. Wrong codes are rate limited per address (5 tries, then 60 s) and globally (more than 30 failures in 10 minutes block every remote client for 10 minutes).
- **Recovery without the phone.** The masking mode (normal, blur all faces, blur the whole image, off) can also be changed from the filter properties in OBS.
- **Debug boxes.** The per-filter option *Show detection boxes (debug)* draws a box around every tracked face (red = unknown and masked, green = known, orange = being added), even with the blur off, to help tuning. The boxes are part of the video: leave the option off while streaming.
- **Files.** The settings (`server.json`: port, access code, style) and the known people (`known.json`, embeddings and thumbnails, "this live only" people excluded) live in OBS's per-user plugin config folder; on Linux and macOS they are created readable by you only.

## Building

HimitsuKao is based on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

Windows (Visual Studio 2022, CMake 3.28 or later):

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Licence

GPL-2.0-or-later, see [LICENSE](LICENSE).
