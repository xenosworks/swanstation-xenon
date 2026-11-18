# SwanStation - PlayStation 1, aka. PSX Emulator

## Opis
Port Xbox 360 udostępnia okładki gier w istniejącym pickerze: lokalny cache JPG/PNG,
opcjonalne HTTPS z PSX Covers i jednoznaczny fallback Libretro. Brak grafiki lub
sieci pozostawia dotychczasowe CD. Dokumentacja poniżej opisuje pozostałe funkcje rdzenia.

## Wymagania
- Do XEX: otwarty xsdk, toolchain PowerPC, imgui-xenon, glslang i CMake.
- Do HTTPS: curl-xenon i mbedtls-xenon, zestaw CA i poprawny zegar konsoli.
- Do testów okładek na komputerze: G++ z C++17, Python 3 i publiczne nagłówki zależności.

## Instalacja
Zbuduj aplikację według [instrukcji okładek Xbox 360](docs/COVER_ART.md).
Zachowaj oryginalną instalację i testuj w osobnym katalogu. Gry i własny BIOS
dodaj lub wskaż samodzielnie; proponowana zmiana ich nie zawiera.

## Uruchomienie
Przenieś XEX wraz z `cacert.pem`, `cover_fallback.tsv` i `cover_fallback.source.json`,
następnie uruchom go z dashboardu. START w pickerze ponawia brakujące okładki.
Testy na komputerze: `bash tests/test_cover_art.sh` i `bash tests/xenon/test_cover_download.sh`.

## Konfiguracja
W płaskim `settings.txt`: `cover_enabled`, `cover_auto_download`,
`cover_download_on_startup`, `cover_fallback_source` (`1`/`0`). Opcjonalne `games_root`
wskazuje katalog gier, a `bios_directory` w `host.txt` katalog odczytu BIOS-u.
Domyślne katalogi gier i zapisów pozostają bez zmian. Więcej w `docs/COVER_ART.md`.

## Znane problemy i rozwiązania
- Brak CA, nieprawidłowy zegar i błąd TLS wyłączają pobranie, bez niebezpiecznych obejść.
- Pierwsza nieznana gra i skok poza bufor mogą wymagać odczytu obrazu płyty i grafiki.
- Potwierdzono start, listę i pobranie 230 okładek na Xbox 360; ostatnia poprawka
  bufora RAM czeka na ocenę płynności na konsoli. Rozgrywka i zapisy wymagają dalszych testów.

## Historia zmian
- Bieżący commit `docs(readme)`: instrukcja konfiguracji okładek dla Xbox 360.
- `d2ab127d`: lokalne okładki, bezpieczne HTTPS, bufor podglądów i testy hosta/CI.
- `7c2a4b29`: wersja 0.1.2.0 w nagłówku XEX.
- `e7a45609`: wersja 0.1.1.0 w nagłówku XEX.
- `7d8e032c`: analogi i wibracje DualShock oraz karty pamięci w plikach.
- `c28584e7`: ustawienie kontrolera przy starcie gry.
- `0c009861`: respektowanie trybu analogowego po resecie.
- `c1f7c021`: obsługa plików RetroArch przez libretro-common.
- `2cb70cce`: wersja 0.1.0.0 w nagłówku XEX.
- `f935e9e2`: rozłożenie kompilacji sceny na klatki.

## Dokumentacja rdzenia

[Features](#features) | [System Requirements](#system-requirements) | [Disclaimers](#disclaimers)

SwanStation is a hard fork and open-source Libretro core implementation of DuckStation, which is an emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability. The goal is to be as accurate as possible while maintaining performance suitable for low-end devices. "Hack" options are discouraged, the default configuration should support all playable games with only some of the enhancements having compatibility issues.

A "BIOS" ROM image can be used, but is not required. You can use an image from any hardware version or region, although mismatching game regions and BIOS regions may have compatibility issues. A ROM image is not provided with the emulator for legal reasons, you should dump this from your own console using Caetla or other means. If no ROM image is provided, [OpenBIOS](https://pcsx-redux.consoledev.net/openbios/) will be used instead.

## Features

SwanStation features include:

 - CPU Recompiler/JIT (x86-64, armv7/AArch32 and AArch64)
 - Hardware (D3D11, OpenGL, Vulkan) and software rendering
 - Upscaling, texture filtering, and true colour (24-bit) in hardware renderers
 - PGXP for geometry precision, texture correction, and depth buffer emulation
 - Adaptive downsampling filter
 - Post processing shader chains
 - "Fast boot" for skipping BIOS splash/intro
 - Save state support
 - Supports bin/cue images, raw bin/img files, MAME CHD, single-track ECM, MDS/MDF, and unencrypted PBP formats.
 - Direct booting of homebrew executables
 - Direct loading of Portable Sound Format (psf) files
 - Digital and analog controllers for input (rumble is forwarded to host)
 - Namco GunCon lightgun support (simulated with mouse)
 - NeGcon support
 - Emulated CPU overclocking
 - Multitap controllers (up to 8 devices)
 - RetroAchievements
 - Automatic loading/applying of PPF patches

## System Requirements
 - A CPU faster than a potato. But it needs to be x86_64, AArch32/armv7, or AArch64/ARMv8, otherwise you won't get a recompiler and it'll be slow.
 - For the hardware renderers, a GPU capable of OpenGL 3.1/OpenGL ES 3.0/Direct3D 11 Feature Level 10.0 (or Vulkan 1.0) and above. So, basically anything made in 2013 or later.

### Region detection and BIOS images
By default, SwanStation will emulate the region check present in the CD-ROM controller of the console. This means that when the region of the console does not match the disc, it will refuse to boot, giving a "Please insert PlayStation CD-ROM" message. SwanStation supports automatic detection disc regions, and if you set the console region to auto-detect as well, this should never be a problem.

The region checking can be disabled in the console options tab. This is the only way to play unlicensed games or homebrew which does not supply a correct region string on the disc, aside from using fastboot which skips the check entirely.

Mismatching the disc and console regions with the check disabled is supported, but may break games if they are patching the BIOS and expecting specific content.

### LibCrypt protection and SBI files

A number of PAL region games use LibCrypt protection, requiring additional CD subchannel information to run properly. libcrypt not functioning usually manifests as hanging or crashing, but can sometimes affect gameplay too, depending on how the game implemented it.

For these games, make sure that the CD image and its corresponding SBI (.sbi) file have the same name and are placed in the same directory. SwanStation will automatically load the SBI file when it is found next to the CD image.

For example, if your disc image was named `Spyro3.cue`, you would place the SBI file in the same directory, and name it `Spyro3.sbi`.

## Tests
 - Passes amidog's CPU and GTE tests in both interpreter and recompiler modes, partial passing of CPX tests

## Disclaimers

"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.
