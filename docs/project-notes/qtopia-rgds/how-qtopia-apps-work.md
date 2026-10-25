# How Qtopia apps work

This guide uses the built-in **Clock** app (`src/applications/clock/` in the QtMoko source) as the worked example. Every Qt Extended 4.4 app is built the same way, and so were the apps on your 4.3.2 disc.

## 1. What an app is made of

```
clock/
  main.cpp          3 lines: hands the main window to Qtopia
  clockmain.cpp/.h  the main window, plus the services it offers
  clock.cpp ...     the screens (clock, alarm, stopwatch tabs)
  *.ui              layouts drawn in Qt Designer
  clock.desktop     launcher entry: name, icon, category
  qbuild.pro        build + install rules
  services/         which services this app answers
  help/  pics/  sounds/
```

An app is an ordinary **Qt 4 widgets program**. Qtopia adds four things: a special startup, a launcher entry, a way for other apps to send it messages, and phone-style UI conventions.

## 2. Startup: there is no normal `main()`

Clock's whole `main.cpp`:

```cpp
#include "clockmain.h"
#include <qtopiaapplication.h>

QTOPIA_ADD_APPLICATION(QTOPIA_TARGET, ClockMain)
QTOPIA_MAIN
```

The app never writes its own `main()`. It only names its main window class (`ClockMain`), and the macros generate the rest.

The reason is that the same source can be built three ways. This is chosen in `qbuild.pro` with `CONFIG+=quicklaunch singleexec`:

- **Normal program:** `bin/clock` is a regular executable.
- **Quicklaunch plugin:** the app is built as a library. The always-running spare `quicklauncher` process loads it and becomes Clock, so Qt and the Qtopia libraries are already loaded and launch is near-instant. This is the default on devices.
- **Single-exec:** every app is linked into one big binary, which saves memory and load time on very small devices.

Either way, the app gets a `QtopiaApplication` instead of a plain `QApplication`. It connects to the `qpe` server, loads the theme, fonts and translations, and listens for messages addressed to the app.

## 3. The launcher entry: `.desktop` files

```ini
[Desktop Entry]
Exec=clock
Icon=clock/Clock
Type=Application
Name[]=Clock
Categories=MainApplications
```

The server builds the launcher grid from these files in `/opt/qtmoko/apps/<category>/`. `Exec` names the app, `Icon` points into `pics/`, and `Categories` decides which folder it lands in (Applications, Games, Settings...). The `[Translation]` block tells Qtopia where the translated name lives. Adding an app to the menu means dropping in a `.desktop` file, with no code change.

## 4. Talking to other apps: services and messages

Apps never call each other directly. They send **requests** through the server over QCop (Qtopia's message bus), and Qtopia starts the target app if it isn't running.

**Offering a service.** Clock announces that it handles `Clock` and `Alarm`:

```cpp
class ClockService : public QtopiaAbstractService {
    ClockService(ClockMain *parent)
        : QtopiaAbstractService("Clock", parent) { publishAll(); }
public slots:
    void showClock();     // any app can now ask for this
};
```

**Asking for a service.** Clock's "Set Time..." menu item doesn't contain a time editor. It asks whichever app provides the `Time` service:

```cpp
QtopiaServiceRequest req("Time", "editTime()");
req.send();
```

The files in `services/<Name>/` and `src/server/main/services/<Name>.service` tell the server which app answers which service. That lets a device swap in a different calendar or dialer without changing the apps that use them.

**Direct messages.** An app also gets raw messages addressed to it through the `appMessage` signal. The system alarm server wakes Clock this way when an alarm is due, even if Clock wasn't running:

```cpp
if (msg == "alarm(QDateTime,int)") { ... alarm->triggerAlarm(when, t); }
```

## 5. Phone-style UI rules

Qtopia was designed for phones with few or no buttons, so apps follow a few conventions instead of desktop menus and toolbars:

- **Soft keys / context bar.** The bar along the bottom of the screen belongs to whichever app is focused. `QSoftMenuBar::menuFor(this)` gives the app's options menu. `QSoftMenuBar::setLabel(widget, Qt::Key_Back, ..., tr("Dismiss"))` relabels a key for the current widget, which is how the alarm screen turns "Back" into "Dismiss".
- **Full screen, one window at a time.** `QtopiaApplication::showMainWidget()` brings the app up full-screen. Dialogs go through `QtopiaApplication::execDialog()`, which sizes them for the device's screen.
- **Works with keys or touch.** The same app runs on keypad phones and touch screens. Focus moves with the d-pad, Select activates, Back goes back. This matters for the RG DS pad.
- **Power hints.** An app can stop the screen from dimming while it needs it, as the stopwatch does: `QtopiaApplication::setPowerConstraint(DisableLightOff)`.

## 6. Data apps don't manage themselves

Qtopia provides shared libraries so apps don't each invent storage:

- **Documents** (pictures, music, notes): `QContent` / `QDocumentSelector`, backed by the one SQLite content database. A new photo shows up in every app's file picker straight away.
- **Contacts, calendar, to-do**: the PIM library (`QContactModel`, `QAppointmentModel`, ...), shared by the address book, datebook and any app that wants them.
- **Settings**: `QSettings("Trolltech", "Clock")`, written to `~/Settings/Trolltech/Clock.conf`.
- **Alarms and timers** that must fire even when the app is closed: the system alarm server, as Clock uses.
- **Sound and video**: requests to `mediaserver` instead of opening the sound card directly.

## 7. Lifetime

- The user opens an app from the launcher, and the server starts it (via the quicklauncher) or brings it to the front if it's already running.
- Leaving an app usually keeps it running in the background. It appears in the task manager and comes back instantly.
- The server can close background apps when memory runs low, so apps save state as they go instead of only on exit.
- An app with no window can still be started just to answer a service request or a message, such as the alarm in section 4.

## 8. Building and installing

`qbuild.pro` lists the sources and **install rules** (`desktop`, `pics`, `help`, `services`, ...). `qbuild image` copies everything into the right place under `/opt/qtmoko`.

- **Built into the image:** add the app's folder to the device profile's `projects.pri`. This is how we added `scribble`, `qterminal` and `screenshot` for the RG DS.
- **Installed later:** build a `.qpk` package and install it with the on-device package manager.

The `target [ hint=sxe domain=trusted ]` lines are for SXE, Qtopia's app sandbox. Built-in apps are "trusted". Downloaded packages can be limited to the services and files they declare.

## 9. Writing a new app for the RG DS

The smallest useful app is four files: `main.cpp` with the two macros, a main window class, a `.desktop` file and a `qbuild.pro`. `examples/application` in the QtMoko source is exactly that template. Existing Qt 4 widget code ports by:

1. swapping `main()` for the macros;
2. moving menus into `QSoftMenuBar`;
3. making sure everything works with the d-pad as well as touch.

For the dual screens, `examples/dualdisplaybasic` shows how an app draws on the secondary display, which on the RG DS is the bottom panel.
