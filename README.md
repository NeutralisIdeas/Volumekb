# VolumeKB

**VolumeKB** is a simple and lightweight utility designed for convenient volume control using a keyboard scroll wheel on Linux devices.

## Features
* Directly reads volume wheel and media key events.
* Provides a smooth, non-intrusive on-screen display (OSD) for volume changes and mute status.
* Bypasses desktop environment hotkey conflicts.

## Installation Guide

Follow these steps to get `volumekb` up and running:

### 1. Install Dependencies
You need to install the required dependencies using your system's package manager.

For Arch Linux (using `pacman`):

sudo pacman -S gcc make libx11 pipewire wireplumber

If you are using a different distribution, replace pacman -S with your respective package manager, e.g., apt install or dnf install).

### 2. Grant User Permissions

Before running the utility, you must add your current user to the input group to allow it to read hardware events natively:
Bash

sudo usermod -aG input $USER

Important: You must log out and log back in (or reboot) for this group change to take effect!

### 3. Build and Install

Compile the source code and install it system-wide by running the utility with root privileges:
Bash

make
sudo make install

(Note: If you run the executable with sudo, it can automatically set up the system-wide configuration).
### 4. Run the Daemon

Once installed and permissions are applied, simply run the utility:
Bash

volumekb &
