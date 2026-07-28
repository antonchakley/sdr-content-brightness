
### 🔆 Lightweight Windows tray app to control SDR content brightness in HDR mode 



Modern Windows versions have good support for SDR content in HDR mode, with color clamping and the ability to tune SDR content brightness, so many people with HDR monitors prefer not to switch between HDR and SDR modes constantly and instead use HDR mode for everything.

However, in HDR mode monitors usually lock brightness controls and the Windows SDR content brightness slider is buried deep in the settings app, which makes it annoying to access.




I believe brightness control should be as easily accessible as audio volume control, because room lighting can change dramatically, so you want the screen brightness to be adjusted as well. This will keep your eyes comfortable,  not blinded by excessive brightness at nighttime, nor strained from trying to read on a low brightness screen in a sunny room.


There are already a few brightness control apps on GitHub, so why another one?

* This uses an (undocumented) Windows SDR contrent brightness boost function, so it won't mess with your real monitor brightness. It affects only SDR content brightness in HDR mode — the same brightness you'd set in Windows settings, so your HDR games and movies will still be using full monitor brightness as they should.

* It is made intentionally simple, just a brightness icon and slider in the Windows 11 style, with no UI clutter, no popups, or anything else that could distract you

* It is very lightweight, written in plain C++ and uses only a few KBs of your precious memory

* It supports Windows themes and looks good with both dark and bright Windows themes, and it uses Windows accent color from theme so it feels  in place with your OS.

* It supports DPI scaling, so it should look clean on high-DPI monitors (which modern HDR monitors usually are).
<p align="center">


<img src="https://github.com/antonchakley/sdr-content-brightness/blob/main/screenshot.png" width=50%  >

</p>

It has an autorun feature in the tray icon right-click context menu, if you want the app to start automatically. If you move the executable file to another location on your disk, just uncheck the autorun menu item and check it again, so the path in the registry gets updated.
