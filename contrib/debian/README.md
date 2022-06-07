
Debian
====================
This directory contains files used to package aviand/avian-qt
for Debian-based Linux systems. If you compile aviand/avian-qt yourself, there are some useful files here.

## avian: URI support ##


avian-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install avian-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your avian-qt binary to `/usr/bin`
and the `../../share/pixmaps/raven128.png` to `/usr/share/pixmaps`

avian-qt.protocol (KDE)

