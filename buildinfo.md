# Linux

---

## Debian (Ubuntu, Mint, ...)

```
sudo apt install libgtk-3-dev
sudo apt install mesa-common-dev
sudo apt install libpulse-dev
sudo apt install libudev-dev
sudo apt install libxcb-xinput-dev && apt install libxcb-xfixes0
sudo apt install libmp3lame-dev
sudo apt install libfreetype-dev
```


## ARCH (Manjaro, Endeavour, ...)

---
```
sudo pacman -Sy gcc
sudo pacman -Sy make
sudo pacman -Sy gtk3
sudo pacman -Sy pkgconf
```

## RPM (Fedora, SUSE, ...)

---
```
sudo dnf install gcc
sudo dnf install freetype-devel
sudo dnf install pulseaudio-libs-devel
sudo dnf install gtk3-devel
sudo dnf install libgudev-devel
```

## Gentoo

---
```
sudo emerge --ask dev-vcs/git
sudo emerge --ask cmake
sudo emerge --ask media-sound/pulseaudio
```

## Build and install

---
```
git clone https://github.com/piciji/denise.git denise
cd denise
cmake -B builds/release
cmake --build builds/release --parallel
sudo cmake --build builds/release --target install
```

## Updates

---
```
cd denise
git pull
cmake --build builds/release --parallel
sudo cmake --build builds/release --target install
```

## Uninstall

---
```
sudo cmake --build builds/release --target uninstall
```

## Optional: build and install denise locally without sudo

---
```
cmake --build builds/release --target clean
cmake -B builds/release -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build builds/release --parallel
cmake --build builds/release --target install
```
