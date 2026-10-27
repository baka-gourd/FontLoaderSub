# FontLoaderSub

OpenType font loader for subtitles (ASS/SSA), inspired by [CryptWizard's FontLoader](https://bitbucket.org/cryptw/fontloader).

Instead of font files, drag-and-drop ASS/SSA subtitles and it will load corresponding font files in its directory.

## Usage

1. Move `FontLoaderSub.exe` to the root of font directory;
1. Drag-and-drop subtitles `*.ass` (or folders) onto `FontLoaderSub.exe`.

Use `FontLoaderSub.exe -old` to force the Win32 interface. The switch can appear
before or after subtitle paths, for example `FontLoaderSub.exe -old "episode.ass"`.

## UI

On Windows 10 1809 or later with a compatible Windows App Runtime 1.8 installed,
the single executable uses WinUI 3. Other devices automatically use Win32.

The Win32 interface retains its original behavior:

* Line 1: requested fonts from subtitles;
* Line 2: stats for font collection;
* `OK`: minimize;
* `Retry`: rebuild font cache;
* `Close`, Esc key, Alt+F4: unload fonts and exit.

## Note

* In order to work with huge font collections, font cache `fc-subs.db` will be built for fast lookup.
* Only accept ASS/SSA files under 64MB, encoded in Unicode with BOM.
* Windows 7 (or later) required.

## Build

Example (with pixi):

```bash
pixi run build
```

Meson builds the optional WinUI code directly into the executable. It prepares
pinned Windows App SDK / C++/WinRT build packages from the local NuGet cache or
downloads them from NuGet; the remaining dependencies continue to use vcpkg.
Use `-Dwinui=false` with Meson to build only the Win32 interface.
