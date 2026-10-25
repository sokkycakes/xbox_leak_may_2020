# How Qtopia works

This covers Qt Extended 4.4.3, the version now running in QEMU. Your 4.3.2 works the same way. Some of the server's internal pieces were rearranged between the two versions, but the overall design didn't change. Paths below are inside the image, under `/opt/qtmoko`.

## The layers, bottom to top

```
 apps (addressbook, clock, qtmail, games...)   one process each
 ─────────────────────────────────────────────
 Qtopia libraries (libqtopia, libqtopiabase,   shared by every app
   pim, mail, media, ...)
 ─────────────────────────────────────────────
 qpe: the Qtopia server                        one process, always running
 ─────────────────────────────────────────────
 Qt for Embedded Linux (QWS, once called       draws windows, routes input
   "Qtopia Core")
 ─────────────────────────────────────────────
 Linux: KMS/DRM, evdev input, ALSA
```

Qtopia isn't an operating system. It's a phone-style desktop and set of apps on top of plain Linux. There is no X server: Qt itself acts as the window system.

## 1. Qt for Embedded Linux (QWS): the window system

With X11 or Wayland, a separate display server owns the screen. In Qtopia, one Qt program plays that role itself, which is why `qpe` starts with `-qws` ("be the QWS server").

- **The server owns the hardware.** It loads one **screen driver** (ours is the `kms` plugin), one or more **mouse/touch handlers** and **keyboard handlers**. On our image these are set by `QWS_DISPLAY`, `QWS_MOUSE_PROTO` and `QWS_KEYBOARD` in `/etc/init.d/S90qtextended`.
- **Every other app is a QWS client.** It connects to the server over a Unix socket in `/tmp/qtembedded-0/`.
- **Clients draw their own windows** into shared-memory buffers.
- **The server composites.** It knows where every window sits and which parts are visible. It copies the visible parts into the screen buffer, and our `kms` driver pushes the changed rectangles to the panel.
- **Input goes through the server.** It reads touch and keys, works out which window is under the finger or has focus, and sends the event to that client.
- **Two panels** use the `Multi:` display spec. Qt treats them as one 640x960 desktop made of two sub-screens. Each touch device is tied to its panel with `screen=0` / `screen=1`.

## 2. qpe: the Qtopia server

`qpe` is the program you actually see. It is the QWS server from step 1, plus everything that makes the device feel like a phone:

- the home screen, the app launcher grid and the top status bar (battery, signal, clock)
- the bottom "soft key" bar (the context menu, the keyboard toggle and Back in the screenshots)
- the task manager, the first-use wizard (language, time), the lock screen and shutdown
- volume, power and screen-saver handling
- the secondary display: Qtopia was built with flip phones in mind, which had a small outer screen, and that clock screen is what now runs on the bottom panel

Inside, the server is a set of **tasks**, small modules registered with `QTOPIA_TASK(...)` (about 90 in `src/server/`). Each device profile chooses which tasks to build. That is how we dropped the Neo-specific modem and audio pieces and kept the rest.

The look comes from **themes**: XML plus images in `etc/themes/` (ours is `finxi`). You can restyle the home screen, status bar and soft keys without touching code.

## 3. How processes talk to each other

Qtopia splits work across many processes, so it has two built-in ways for them to talk.

- **QCop** is a message bus that runs over the QWS connection. A process sends a message such as `execute(QString)` on a channel, and anyone listening on that channel gets it. On top of QCop sit **services**: named jobs like `Launcher` or `TaskManager` that any app can request without knowing which program provides them. The RG DS button setup uses exactly this: in `etc/defaultbuttons.conf` a button press says `PressedActionService=TaskManager`, `PressedActionMessage=multitask()`.
- **The value space** is a shared tree of live values, held in shared memory and readable from any process. Examples are `/Hardware/...`, battery level and network state. The UI can show or hide things based on it. The launcher menu config has entries like `{@/Telephony/AvailableServiceCount}`, meaning "only show this icon if that value is set".

## 4. Apps and starting them fast

- **Each app is a `.desktop` file plus a program or plugin.** The `.desktop` files in `apps/` give the name, icon and category. The launcher grid is built from them.
- **The quicklauncher keeps launches fast.** `qpe` keeps one spare `quicklauncher` process already running, with Qt and the Qtopia libraries loaded and initialized. To open an app, the server tells that spare process to load the app as a plugin and become it, then starts a new spare. On 2008 phone CPUs this cut seconds off every launch.
- **Some background servers start on demand:** `mediaserver` (sound and video playback), `messageserver` (email and SMS) and the PIM data store (contacts, calendar, to-do lists).

## 5. Documents, settings and storage

- **Content database.** Qtopia keeps one SQLite database (`qtopia_db.sqlite`) indexing documents, pictures and music. That's how "Documents" and file pickers work without scanning the disk each time. `content_installer` fills in the built-in entries when the image is built.
- **Settings** are INI files.
  - Defaults live in `etc/default/Trolltech/*.conf`, and the device's button map is in `defaultbuttons.conf`.
  - Your changes are written under `$HOME/Settings/Trolltech/`.
  - Wiping that folder resets the device and brings back the first-use wizard.
- **Packages and security.** The package manager installs `.qpk` packages. SXE ("Safe Execution Environment") can sandbox downloaded apps by limiting which QCop messages and files they may use.

## 6. How it starts on our image

1. Linux boots, Buildroot's init runs `/etc/init.d/S90qtextended`.
2. That script sets the display, touch and key settings, then starts `qpe -qws`. If `qpe` exits it restarts it, and the log is `/tmp/qtextended/qpe.log`.
3. `qpe` opens both panels through `kms`, opens the touch devices, and starts its tasks.
4. On first boot the first-use wizard runs (welcome, then time zone). After that, the home screen appears on top and the clock appears on the bottom panel.
5. Tapping an app makes the spare quicklauncher become that app, which then draws as a QWS client.

## 7. How it's built

Qt Extended has its own build tool, **qbuild**, plus a **device profile**: our `devices/rgds/` folder.

- The profile says which compiler to use (the Buildroot aarch64 toolchain), screen size, modules (PIM, messaging, games...) and device-specific plugins (our `kms` screen driver).
- `configure -device rgds` sets things up, `qbuild` compiles Qt plus Qtopia, and `qbuild image` produces the `/opt/qtmoko` tree that goes into the Buildroot image.

## What this means for the RG DS

- **Pad buttons** will be keyboard handlers plus entries in `defaultbuttons.conf`, mapping each button to a key or a service, such as Home or the task switcher.
- **The bottom panel** can stay the secondary display or become a second full touch area. That is a server/theme choice, not a driver change.
- **New apps** are regular Qt 4 widget programs plus a `.desktop` file, built with the same toolchain.
