#!/bin/sh

#
# Windows
#
inkscape -z -e /tmp/image-0.png icon_128.svg
inkscape -z -e /tmp/image-1.png icon_1024.svg
convert '/tmp/image-%d.png[0-1]' -background transparent	\
    \( -clone 0 -resize 16 -colors 256 -compress none \)	\
    \( -clone 0 -resize 20 -colors 256 -compress none \)	\
    \( -clone 0 -resize 24 -colors 256 -compress none \)	\
    \( -clone 0 -resize 32 -colors 256 -compress none \)	\
    \( -clone 0 -resize 40 -colors 256 -compress none \)	\
    \( -clone 1 -resize 48 -colors 256 -compress none \)	\
    \( -clone 1 -resize 96 -colors 256 -compress none \)	\
    \( -clone 1 -resize 128 -colors 256 -compress none \)	\
    \( -clone 0 -resize 16 -compress none \)	\
    \( -clone 0 -resize 20 -compress none \)	\
    \( -clone 0 -resize 24 -compress none \)	\
    \( -clone 0 -resize 32 -compress none \)	\
    \( -clone 0 -resize 40 -compress none \)	\
    \( -clone 1 -resize 48 -compress none \)	\
    \( -clone 1 -resize 64 -compress none \)	\
    \( -clone 1 -resize 96 -compress none \)	\
    \( -clone 1 -resize 128 -compress none \)	\
    \( -clone 1 -resize 256 -compress Zip \)	\
    -delete 1 -delete 0	\
    -alpha remove ../icons/windows-icons/js8call.ico
rm /tmp/image-0.png /tmp/image-1.png
identify -format '%f %p/%n %m %C/%Q %r %G %A %z\n' ../icons/windows-icons/js8call.ico
#
#inkscape -z -e /dev/stdout -w 150 -h 57 -b white installer_logo.svg | tail -n +4 |	\
#    convert png:- -resize 150x57 +matte BMP3:../icons/windows-icons/installer_logo.bmp
convert png:installer_logo.svg.png -resize 150x57 +matte BMP3:../icons/windows-icons/installer_logo.bmp
identify -format '%f %p/%n %m %C/%Q %r %G %A %z\n' ../icons/windows-icons/installer_logo.bmp

#
# Mac
#
mkdir -p ../icons/Darwin/JS8Call.iconset
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_16x16.png -w 16 -h 16 icon_128.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_16x16@2x.png -w 32 -h 32 icon_128.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_32x32.png -w 32 -h 32 icon_128.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_32x32@2x.png -w 64 -h 64 icon_128.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_128x128.png -w 128 -h 128 icon_1024.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_128x128@2x.png -w 256 -h 256 icon_1024.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_256x256.png -w 256 -h 256 icon_1024.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_256x256@2x.png -w 512 -h 512 icon_1024.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_512x512.png -w 512 -h 512 icon_1024.svg
inkscape -z -e ../icons/Darwin/JS8Call.iconset/icon_512x512@2x.png -w 1024 -h 1024 icon_1024.svg
identify -format '%f %p/%n %m %C/%Q %r %G %A %z\n' ../icons/Darwin/JS8Call.iconset/*
# generic globe iconset for utilities
mkdir -p ../icons/Darwin/wsjt.iconset
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_16x16.png -w 16 -h 16 icon_128.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_16x16@2x.png -w 32 -h 32 icon_128.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_32x32.png -w 32 -h 32 icon_128.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_32x32@2x.png -w 64 -h 64 icon_128.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_128x128.png -w 128 -h 128 icon_1024.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_128x128@2x.png -w 256 -h 256 icon_1024.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_256x256.png -w 256 -h 256 icon_1024.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_256x256@2x.png -w 512 -h 512 icon_1024.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_512x512.png -w 512 -h 512 icon_1024.svg
inkscape -z -e ../icons/Darwin/wsjt.iconset/icon_512x512@2x.png -w 1024 -h 1024 icon_1024.svg
identify -format '%f %p/%n %m %C/%Q %r %G %A %z\n' ../icons/Darwin/wsjt.iconset/*

#
# KDE & Gnome
#
inkscape -z -e ../icons/Unix/js8call_icon.png -w 128 -h 128 icon_1024.svg
identify -format '%f %p/%n %m %C/%Q %r %G %A %z\n' ../icons/Unix/js8call_icon.png
