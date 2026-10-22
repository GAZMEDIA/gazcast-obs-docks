# GazCast docks for OBS Studio

A small, free OBS Studio plugin that adds two docks to OBS showing pages from the
**GazCast** desktop app:

| Dock            | Dock id           | Page                                  |
| --------------- | ----------------- | ------------------------------------- |
| GazCast         | `gazcast-list`    | `http://127.0.0.1:5510/dock`          |
| GazCast Control | `gazcast-control` | `http://127.0.0.1:5510/dock/control`  |

It also adds a third dock, **GazCast Clean Record**, that records a second,
"clean" copy of your show without the overlays (see below). This dock is plain
OBS and works without the app.

It also adds two entries to OBS's **Tools** menu:

- **GazCast: open app**: launches GazCast through its `gazcast://open` link.
- **GazCast: reload docks**: re-reads the config and reloads both docks.

The plugin is only a shell. It shows what the app serves, and every feature
(including licensing) lives in the GazCast app. Without the app, the docks just
show a "start the app" page.

Requires OBS Studio 30 or later (64-bit). Windows x64 is the supported target;
the template's macOS and Linux build paths are kept and should also work.

## Install (Windows)

1. Download the plugin: https://overlays.gazgoc.com.ng/download/obs (or the zip
   from this repo's Releases).
2. Close OBS.
3. Unzip it and copy the `gazcast-docks` folder to
   `C:\ProgramData\obs-studio\plugins\`.
4. Start OBS and turn the docks on from **Docks → GazCast** and
   **Docks → GazCast Control**. OBS remembers where you put them.

You also need the GazCast desktop app (free 14-day trial):
https://gazgoc.com.ng/gazcast/

To uninstall, delete `C:\ProgramData\obs-studio\plugins\gazcast-docks`.

Without the plugin, you can add the same pages yourself under
**Docks → Custom Browser Docks**: `http://127.0.0.1:5510/dock` and
`http://127.0.0.1:5510/dock/control`.

## Clean recording (no overlays)

The **GazCast Clean Record** dock records the same program output OBS sends to
the stream, with the same sound, but leaves out the sources you tick. Your
stream and your normal recording still show everything.

1. Turn the dock on: **Docks → GazCast Clean Record**.
2. In its list, tick the sources to leave out. GazCast overlays are ticked for
   you. Ticking a source adds a filter called **Hide in GazCast clean
   recording** to it, so you can also add or remove it from the source's
   **Filters** window. It is saved with your scene collection.
3. Press **Start Recording** in OBS as usual. With **Start and stop together
   with OBS's Start Recording button** ticked (the default), the clean copy
   starts and stops with it and is saved next to your recording as
   `<recording name> (clean).mkv` (or `.mp4` if you record MP4). You can also
   use the dock's own **Start clean recording** button; that file is called
   `GazCast clean <date> <time>`.

The clean copy uses the same video encoder and settings as your normal
recording (or the first hardware encoder it finds, then x264), so it costs
about as much as a second recording. MP4 files are written fragmented so they
survive a crash.

Known limit: OBS draws a few things once per frame and reuses them. An overlay
still shows in the clean copy if it sits inside a scene item that is cropped,
has a scale filter or a blending mode set, and during the second or so a scene
transition runs. The stream is never affected.

## Building

Push to this repo (or run the workflow by hand) and GitHub Actions builds the
Windows plugin with the obs-plugintemplate scripts in `.github/scripts`; each
build is saved as a release.

## Reporting problems

Open an issue: https://github.com/GAZMEDIA/gazcast-obs-docks/issues

## How the docks find the app

- The app serves its pages on `http://127.0.0.1:<port>`. The port is `5510` by
  default.
- To use another port, edit
  `%APPDATA%\obs-studio\plugin_config\gazcast-docks\config.json` (the plugin
  creates it on first run):

  ```json
  { "port": 5510 }
  ```

  Then use **Tools → GazCast: reload docks**, or restart OBS.
- Each dock first opens the bundled `data/waiting.html?to=<page url>`. That page
  checks `http://127.0.0.1:<port>/healthz` right away and then every 2 seconds.
  When the app answers, it opens the real page. While it waits, it shows
  "GazCast isn't running" and a **Start GazCast** button (`gazcast://open`),
  which OBS passes to Windows to launch the app.
- If the app stops while a dock is open, the plugin catches obs-browser's error
  page and goes back to the waiting page, which reconnects by itself.
- The docks use their own persistent browser profile (cookie store `gazcast`,
  under `%APPDATA%\obs-studio\plugin_config\obs-browser\`), so signing in to the
  app inside the dock is remembered across OBS restarts. The app's sign-in cookie
  needs an `Expires`/`Max-Age`, because OBS's browser does not keep session-only
  cookies.
- Links that open a new window (`target="_blank"`, `window.open`) open in the
  user's normal web browser.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). OBS Studio is GPL-licensed, so its
plugins must be GPL-compatible. This plugin is free software. It only loads web
pages from the separate GazCast application over HTTP on localhost, and it
contains none of the app's code. `src/obs-browser/browser-panel.hpp` is an
unmodified copy of obs-browser's public panel header (GPL-2.0-or-later, OBS
Project).

## Build

This folder is based on the official
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) and keeps
its layout (`buildspec.json`, `CMakePresets.json`, `cmake/`, `.github/scripts/`).
The template downloads the pinned OBS sources, obs-deps and Qt6 listed in
`buildspec.json` into `.deps/`.

**CI:** `.github/workflows/build-obs-plugin.yml` (at the repo root) builds on
`windows-2022` whenever `obs-plugin/**` changes, or when run by hand. It uploads
the artifact `gazcast-docks-windows`.

**Windows, locally** (Visual Studio 2022 with the Windows 10/11 SDK, CMake 3.28+):

```powershell
cd obs-plugin
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
# -> release/RelWithDebInfo/gazcast-docks/{bin/64bit,data}
```

**Linux** (Ubuntu, using the distro's OBS dev packages):

```sh
sudo apt install cmake ninja-build libobs-dev qt6-base-dev qt6-base-private-dev
cd obs-plugin
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

**macOS:** `cmake --preset macos && cmake --build --preset macos` (Xcode 16).

Browser docks need OBS's obs-browser plugin, which official OBS builds include.
OBS does not support browser docks under Wayland, so on Wayland the docks show a
notice instead.
