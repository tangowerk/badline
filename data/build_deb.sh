#!/bin/bash

prefix=usr/local

rm -rf badlinepackage
mkdir badlinepackage
mkdir badlinepackage/DEBIAN
mkdir -p badlinepackage/$prefix/
mkdir badlinepackage/$prefix/bin
mkdir badlinepackage/$prefix/share
mkdir badlinepackage/$prefix/share/badline
mkdir badlinepackage/$prefix/share/badline/images
mkdir badlinepackage/$prefix/share/badline/fonts
mkdir badlinepackage/$prefix/share/badline/data
mkdir badlinepackage/$prefix/share/badline/sounds
mkdir badlinepackage/$prefix/share/badline/presets
mkdir badlinepackage/$prefix/share/applications
mkdir badlinepackage/$prefix/share/appdata
mkdir badlinepackage/$prefix/share/icons
mkdir -p badlinepackage/$prefix/share/icons/hicolor/256x256/apps
mkdir -p badlinepackage/$prefix/share/icons/hicolor/48x48/apps
mkdir -p badlinepackage/$prefix/share/icons/hicolor/32x32/apps
mkdir badlinepackage/$prefix/share/mime
mkdir badlinepackage/$prefix/share/mime/packages

install -D -m 644 control badlinepackage/DEBIAN
install -D -m 644 fonts/* badlinepackage/$prefix/share/badline/fonts
cp -r sounds/* badlinepackage/$prefix/share/badline/sounds/
cp -r presets/* badlinepackage/$prefix/share/badline/presets/
install -D -m 644 data/* badlinepackage/$prefix/share/badline/data
install -D -m 644 img/badline_48.png badlinepackage/$prefix/share/icons/badline.png
install -D -m 644 img/badline_48.png badlinepackage/$prefix/share/icons/hicolor/48x48/apps/badline.png
install -D -m 644 img/badline_32.png badlinepackage/$prefix/share/icons/hicolor/32x32/apps/badline.png
install -D -m 644 img/badline.png badlinepackage/$prefix/share/icons/hicolor/256x256/apps/badline.png
install -D -m 644 images/* badlinepackage/$prefix/share/badline/images
install -D -m 644 badline.desktop badlinepackage/$prefix/share/applications
install -D -m 644 badline.appdata.xml badlinepackage/$prefix/share/appdata
install -D -m 644 application-x-badline.xml badlinepackage/$prefix/share/mime/packages
install -D -m 755 ../builds/release/program/badline badlinepackage/$prefix/bin
chmod +x badlinepackage/$prefix/bin/badline

INSTALLED_SIZE=$(du -ks badlinepackage | cut -f 1)
VERSION=$(cat ../program/program.h | grep '^#define VERSION' ../program/program.h | sed 's/.*"\(.*\)".*/\1/')

sed -i 's/_INSTALLED_SIZE/'$INSTALLED_SIZE'/;s/_VERSION/'$VERSION'/' badlinepackage/DEBIAN/control
