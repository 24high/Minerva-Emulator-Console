# Minerva Console: der Bare-Metal-Kernel

Minerva Console ist ein Circle-Kernel, der libretro-Cores statisch gelinkt
ohne Betriebssystem ausführt (Spieleliste mit Kacheln, Spielstände, Start in
etwa 5 Sekunden). Es gibt ihn für drei Geräte:

| Gerät | Board-Profil | Build | Kernel | SD-Karte | Image |
|---|---|---|---|---|---|
| Retroflag GPi Case, Raspberry Pi Zero / Zero W | `gpi` | `npm run build:gpi` | `kernel.img` (32 Bit, 15 Cores) | `output-gpi/` | `dist/minerva-gpi-zero.img` |
| Retroflag GPi Case 2, Compute Module 4 | `gpi2` | `npm run build:gpi2` | `kernel8-rpi4.img` (64 Bit, 16 Cores mit N64) | `output-gpi2/` | `dist/minerva-gpi2-cm4.img` |
| Raspberry Pi 5 am Fernseher | `pi5` | `npm run build:rpi5` | `kernel_2712.img` (64 Bit, 16 Cores mit N64) | `output-rpi5/` | `dist/minerva-rpi5.img` |

Alle drei bauen das Core-Bündel `--core=all`. `build.mjs` übersetzt Circle,
die Cores und den Runner direkt, ohne `make` (Circle als `circle/` neben
`baremetal/` oder per `CIRCLEHOME=...`). Das Image ist 256 MB groß,
`SD_IMAGE_MB=4096` macht es 4 GB. Das GPi Case ist auf dem Gerät getestet,
GPi Case 2 und Pi 5 bisher nur emuliert (siehe unten).

### Ältere Einzel-Builds für den Pi 5

Aus der Anfangszeit gibt es für das Board `pi5` noch Kernel mit einem Core:
`--core=pattern` (Testbild, `npm run build:baremetal`), `--core=fceumm`
(NES, `npm run build:baremetal:nes`, ROM per `--rom=<Name>`),
`--core=n64` und `--core=multi` (NES + N64). Sie landen unter
`baremetal/build-node/<core>/kernel_2712.img`; auf die SD-Karte gehören dazu
`bcm2712-rpi-5-b.dtb`, `overlays/bcm2712d0.dtbo` und die Dateien aus
`baremetal/rpi5/`.

## Raspberry Pi 5 und GPi Case 2 (Compute Module 4)

`--core=all` gibt es auch für 64 Bit: `npm run build:rpi5` (Board `pi5`,
Cortex-A76, HDMI 1080p, USB-Pad) und `npm run build:gpi2` (Board `gpi2`,
CM4/BCM2711, Cortex-A72). Beide enthalten die 15 Cores des GPi-Builds plus
N64 (Mupen64Plus-Next mit ARM64-Dynarec, RDP angrylion auf mehreren Kernen).
SD-Karte: `output-rpi5/` bzw. `output-gpi2/`, Images
`dist/minerva-rpi5.img` und `dist/minerva-gpi2-cm4.img`
(`SD_IMAGE_MB=4096` für 4 GB). Toolchain: `aarch64-none-elf` 15.2.rel1 in
`./toolchain`.

GPi Case 2 (Werte aus RetroFlags `GPiCase2-Script`, `baremetal/gpi2/config.txt`):

| Teil | Linux | hier |
|---|---|---|
| Bildschirm 640×480 | `dtoverlay=dpi24`, `dpi_output_format=0x00016` | DPI-Zeilen in `config.txt`, Pins 0-17/20-25 ALT2 wie beim GPi Case (`circle_gpi.cpp`) |
| Ton | USB-Soundkarte im Gehäuse (`snd_usb_audio`) | `CUSBSoundBaseDevice` mit 48 kHz, `CircleAudio` rechnet die Core-Raten linear um |
| Controller, Soundkarte | `dtoverlay=dwc2,dr_mode=host` | interner xHCI des BCM2711 (`USE_XHCI_INTERNAL`, `otg_mode=1`) |
| Ein/Aus | `SafeShutdown_gpi2.py`: GPIO26 Schalter, GPIO27 Halten | wie beim GPi Case |
| Dock (HDMI) | GPIO18 high, Skript tauscht `config.txt` und startet neu | noch nicht unterstützt |

Der Kernel ist für den CM4 Lite (Boot von der SD-Karte); `armstub8-rpi4.bin`
baut `build.mjs` aus Circles `boot/armstub/armstub8.S`.

Was 64 Bit anders braucht:

- Snes9x 2002 ohne `ARM_ASM` (32-Bit-Inline-Assembler) mit dem portablen
  Renderer (`ppu_.c`, `gfx.c`, `tile.c`); dessen `memset32`/`memset16`
  füllten nur Bytes, `patches/snes9x2002` schreibt ganze Wörter.
- gpSP mit dem ARM64-Dynarec. Sein Code-Cache muss in BL-Reichweite
  (±128 MB) des Programmcodes liegen; `mmap(PROT_EXEC)` gibt dafür auf AArch64
  einen 12-MB-Block aus `.bss` direkt hinter dem Kernelcode
  (`libc/newlib_glue.cpp`).
- Ausführbare Seiten: Circle mappt alles hinter `.text` als PXN; dafür löscht
  `ra_libc_make_executable` (`libc/circle_bridge.cpp`) das PXN-Bit in den
  64-KB-Seiten (gpSP-Cache, N64-Dynarec in `.bss` über `mprotect`).
- C++-Exceptions (fake-08s Lua): `crtbegin.o`/`crtend.o` registrieren
  `.eh_frame` (wie Circles `Rules.mk`), und `DW.ref.__gxx_personality_v0`
  bleibt bei der Core-Isolation global, sonst landete der Unwinder bei einer
  verworfenen Kopie.
- N64: `asm_defines_gas.h` erzeugt der Build aus `asm_defines.c` wie das
  Makefile des Cores. Nach einem N64-Spiel führt Start+Select per Neustart ins
  Menü (der Core ist nicht für einen zweiten Start im selben Lauf gemacht).

Tests: `node baremetal/tests/arm-smoke.mjs --board=pi5` bzw. `--board=gpi2`
mit allen Modi (`--fatfs`, `--sequence`, `--saves`, `--input`) unter
`qemu-aarch64` (Cortex-A76 bzw. A72). Dafür gibt es eine eigene
Programmstart-/Systemaufruf-Schicht (`tests/aarch64_linux_crt.c`,
`tests/linux_syscall.h`). Ohne `--fatfs` läuft gpSP dort interpretiert (das
`mmap` von Linux liegt außer Reichweite), mit `--fatfs` über die echte
libc-Schicht mit Dynarec. N64 lässt sich ohne freies Testmodul mit Bootcode
nicht im Test ausführen. Auf echter Hardware sind Pi 5 und GPi Case 2 noch
nicht getestet.

## Raspberry Pi Zero / Zero W im Retroflag GPi Case

Das Board-Profil `gpi` baut denselben Runner für den Pi Zero (BCM2835,
ARM1176, 32 Bit, Circle `RASPPI=1`) im Retroflag GPi Case. Standard ist das
Core-Bündel `--core=all` mit 15 Cores in einem Kernel:

| System | Core | Dateiendungen |
|---|---|---|
| NES | FCEUmm | `.nes` |
| Game Boy / Game Boy Color | Gambatte | `.gb` `.dmg` `.gbc` |
| Game Boy Advance | gpSP (ARM-Dynarec) | `.gba` `.agb` |
| SNES | Snes9x 2002 | `.sfc` `.smc` `.swc` `.fig` |
| Mega Drive, Master System, Game Gear, SG-1000, 32X | PicoDrive | `.md` `.gen` `.smd` `.bin` `.sms` `.gg` `.sg` `.32x` |
| Atari 2600 | Stella 2014 | `.a26` |
| Atari Lynx | Handy (HLE-BIOS, keine BIOS-Datei nötig) | `.lnx` |
| PC Engine / TurboGrafx-16 (HuCards) | Beetle PCE Fast | `.pce` |
| WonderSwan / Color | Beetle WonderSwan | `.ws` `.wsc` |
| ZX Spectrum | Fuse | `.tzx` `.tap` `.z80` `.sna` `.szx` |
| ZX81 | EightyOne | `.p` `.t81` |
| Amstrad CPC | Caprice32 | `.dsk` `.cdt` |
| Commodore 64 | Frodo | `.d64` `.t64` `.x64` `.p00` |
| PICO-8 | fake-08 | `.p8` `.p8.png` |
| Arcade | MAME 2000 (0.37b5) | `.zip` |

Die Endungen überschneiden sich nicht (`.tzx` ist Spectrum, `.zip` ist MAME).
MAME legt den Münzeinwurf auf Select. Die Heimcomputer am GPi:

| System | Belegung |
|---|---|
| ZX Spectrum | Steuerkreuz = Joystick (Kempston und Cursor-Tasten 5 bis 8 zugleich), A, X, Y = Feuer (auch Taste 0), B = hoch, Select = Bildschirmtastatur |
| ZX81 | Steuerkreuz = Tasten 5 bis 8, A, B, X, Y = Taste 0, Select = Bildschirmtastatur |
| Amstrad CPC | Steuerkreuz = Joystick, A = Feuer, B = zweiter Feuerknopf, Y = Leertaste, Select + Start (kurz) = Bildschirmtastatur, Select + B tippt `CAT`, Select + A tippt `RUN"DISC` |
| C64 | Steuerkreuz und A = Joystick, Y = Bildschirmtastatur, Select wechselt zwischen Joystick und Maus |

Viele Spectrum- und CPC-Spiele nehmen die Tastatur, bis man im Spielmenü den
Joystick wählt; dafür ist die Bildschirmtastatur da. Beim CPC hat sie einen
Zeiger (Steuerkreuz bewegt, A drückt die Taste, Select + Start schließt);
*The Living Daylights* will zum Beispiel `f3` auf dem Ziffernblock. Mit dem
abgeschnittenen Rand (`cap32_scr_crop`) zeichnet Caprice32 die Tastatur
gestaucht, sonst fehlten Esc und der halbe Ziffernblock
(`patches/libretro-cap32`). A ist Bit 4 des Joystick-Ports, der Feuerknopf
gewöhnlicher Ein-Knopf-Joysticks (Pin 6).

Fuse bekommt den Pad als Cursor-Joystick (Port 0) und als Kempston-Joystick
(Port 1) zugleich (`padPorts` im Runner, `patches/fuse-libretro`).

ZX81-Programme ohne Autostart startet der Core nach dem Laden selbst
(`patches/81-libretro`); der echte ZX81 bleibt dann mit `0/0` auf leerem
Bildschirm stehen und wartet auf `RUN`.

N64 braucht den AArch64-Dynarec und mehrere Kerne und ist auf dem Zero nicht
möglich. Mega-CD fehlt (braucht BIOS und CD-Images), ebenso PC-Engine-CD.
PSP und N-Gage übersteigen den Pi Zero bei weitem. DOS fehlt: DOSBox Pure
braucht echte Threads, die DOSBox-SVN-Portierung SDL und mehrere externe
Bibliotheken (MT-32, FluidSynth, Audio-Decoder), und am GPi fehlt die
Tastatur.

```sh
npm run build:gpi        # alle Cores
npm run build:gpi:nes    # nur FCEUmm, wie bisher
```

Voraussetzungen (alle per `.gitignore` ausgenommen):

- Circle in `./circle`: `git clone --depth 1 https://github.com/rsta2/circle.git circle`
- Die Cores in `baremetal/cores/`:

  ```sh
  git clone --depth 1 https://github.com/libretro/libretro-fceumm.git baremetal/cores/libretro-fceumm
  git clone --depth 1 https://github.com/libretro/gambatte-libretro.git baremetal/cores/gambatte-libretro
  git clone --depth 1 https://github.com/libretro/snes9x2002.git baremetal/cores/snes9x2002
  git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/libretro/picodrive.git baremetal/cores/picodrive
  git clone --depth 1 https://github.com/libretro/gpsp.git baremetal/cores/gpsp
  git clone --depth 1 https://github.com/libretro/stella2014-libretro.git baremetal/cores/stella2014-libretro
  git clone --depth 1 https://github.com/libretro/libretro-handy.git baremetal/cores/libretro-handy
  git clone --depth 1 https://github.com/libretro/beetle-pce-fast-libretro.git baremetal/cores/beetle-pce-fast-libretro
  git clone --depth 1 https://github.com/libretro/beetle-wswan-libretro.git baremetal/cores/beetle-wswan-libretro
  git clone --depth 1 https://github.com/libretro/fuse-libretro.git baremetal/cores/fuse-libretro
  git clone --depth 1 https://github.com/libretro/81-libretro.git baremetal/cores/81-libretro
  git clone --depth 1 https://github.com/libretro/libretro-cap32.git baremetal/cores/libretro-cap32
  git clone --depth 1 https://github.com/libretro/frodo-libretro.git baremetal/cores/frodo-libretro
  git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/jtothebell/fake-08.git baremetal/cores/fake-08
  git clone --depth 1 https://github.com/libretro/mame2000-libretro.git baremetal/cores/mame2000-libretro
  ```

- Arm GNU Toolchain 15.2.rel1 (`arm-none-eabi`, von developer.arm.com) entpackt
  in `./toolchain`, oder `TOOLCHAIN=/pfad/zur/toolchain`
- Für das SD-Image unter Linux: `dosfstools` (mkfs.vfat) und `mtools` (mcopy)

`build.mjs` baut nur neu, was sich geändert hat. Dazu zählen auch die Header:
Der Compiler schreibt mit `-MMD` je Objekt eine `.d`-Datei, und ändert sich
ein Header, werden alle Dateien neu übersetzt, die ihn einbinden. Ohne das
behielt `kernel.cpp` nach einer Änderung an `libretro_runner.h` die alte
Größe von `LibretroRunner`, und der Kernel überschrieb beim Spielstart eigenen
Speicher.

Der Build lädt die Pi-Firmware (`bootcode.bin`, `start.elf`, `fixup.dat`) in
der Revision, die Circle in `circle/boot/Makefile` festlegt, und erzeugt:

```text
baremetal/build-node/gpi-all/kernel.img      Kernel
output-gpi/                                  Dateien für eine FAT32-SD-Karte
dist/minerva-gpi-zero.img                    flashbares Image (256 MiB, SD_IMAGE_MB=…)
```

ROMs kommen ins Wurzelverzeichnis oder in beliebige Unterordner der
FAT-Partition; der ROM-Browser zeigt zu jeder Datei das System an. Das GPi-Profil nutzt Circles FatFs-Addon und unterstützt daher
Unterordner und lange Dateinamen. Das Pi-5-Profil verwendet weiterhin Circles
nativen FAT-Treiber, dessen `DirectoryFindFirst` nicht in Upstream-Circle
enthalten ist.

### Wie mehrere Cores in einen Kernel passen

Die Cores sind für ein gehostetes System geschrieben und bringen eigene Kopien
von libretro-common, zlib und Hilfsfunktionen mit. `build.mjs` behandelt jeden
Core deshalb getrennt:

1. Kompilieren gegen newlib, die C-Bibliothek der Toolchain (Gambatte zusätzlich
   gegen libstdc++), mit den Flags aus dem jeweiligen libretro-Makefile.
2. Vorlinken aller Objekte zu einem Objekt (`ld -r`).
3. Mit `objcopy` alle Symbole lokal machen außer der libretro-API, die zu
   `<core>_retro_*` umbenannt wird. Doppelte Hilfsfunktionen der Cores kollidieren
   dadurch nicht mehr. Global bleiben außerdem die schwachen Symbole der
   C++-Standardbibliothek (Template-Instanzen in `std`/`__gnu_cxx`): Sie sind in
   allen Cores gleich, der Linker behält von jeder COMDAT-Gruppe nur eine Kopie
   (eine lokalisierte könnte er verwerfen, während der Core sie noch braucht),
   und libstdc++ von GCC 15 braucht selbst `basic_string::_M_construct<true>`.

Die zehn später hinzugekommenen Cores (Stella bis MAME) stehen nicht als
Dateiliste in `build.mjs`: `makefileBundleCore` liest Quellen, Defines und
Include-Pfade per `make -n platform=unix` aus dem Makefile des Cores (Ergebnis
im Build-Verzeichnis zwischengespeichert). Dateien, die das Makefile erst
erzeugt (`config.h`, `version.c`, ROM-Header per `xxd`), lässt es mit den
eigenen Regeln des Cores anlegen. Korrekturen an einem Core liegen als Patch in
`baremetal/patches/<Core-Verzeichnis>/` und werden vor dem Kompilieren
angewendet, falls noch nicht geschehen (Frodo: `mainThread` nach
`retro_deinit` zurücksetzen, sonst startet kein zweites C64-Spiel). fake-08
übersetzt seine `.c`-Dateien wie upstream als C++ und bekommt `int32_t = int`
(auf `arm-none-eabi` ist es `long`, z8lua erwartet `int`).

Mit allen Cores ist der Kernel rund 21 MB groß und belegt samt `.bss` gut 42 MB;
das Bündel hat dafür `KERNEL_MAX_SIZE` 64 MB.

`baremetal/libc/` verbindet newlib mit Circle: Speicher kommt aus Circles Heap
(newlib verwaltet keinen eigenen). Circles `memalign` kann nur bis zur
Cache-Zeilengröße (32 Byte) ausrichten; größere Ausrichtungen (`mmap`: 4 KB,
libretro-Dateipuffer: 64 Byte) holt die Glue-Schicht selbst und ersetzt dafür
`free`/`realloc`/`memalign`. Dateien und Verzeichnisse laufen über FatFs
(`SD:`), `stdout`/`stderr` landen im Circle-Log. Dazu kommen `<dirent.h>`
(mit `scandir`/`alphasort`) und `<sys/mman.h>`, die newlib für
`arm-none-eabi` nicht hat, sowie `chdir`/`getcwd`/`realpath`: Relative Pfade
beziehen sich auf das Arbeitsverzeichnis, `.` und `..` werden aufgelöst.

Circles Heap verwendet freigegebene Blöcke oberhalb seiner größten Bucket-Größe
(standardmäßig 512 KB) nie wieder. Weil Spiele beendet und andere gestartet
werden können und die Cores große Blöcke anfordern (gpSP: ROM in 1-MB-Blöcken,
10,5 MB Übersetzungs-Cache), setzt das GPi-Profil `HEAP_BLOCK_BUCKET_SIZES`
mit Stufen bis 32 MB. Das Log nennt beim Spielstart den freien Heap.

Für den Zero gewählte Konfigurationen:

- Snes9x 2002 mit `ARM_ASM` (ARM-optimierter Renderer wie beim 3DS-Port),
  CPU und SPC700 in C.
- PicoDrive mit den C-CPU-Kernen FAME (68000) und CZ80 (Z80), ohne DRCs und ohne
  ARM-Assembler. Die schnelleren ARM-Kerne (Cyclone, DrZ80) brauchen generierte
  Dateien und sind noch nicht eingebunden. Falls Mega-Drive-Spiele auf dem Zero
  ruckeln, ist das der nächste Hebel. 32X läuft ohne SH2-DRC nur interpretiert und
  damit langsam.

- gpSP wie unter Linux auf dem Pi 1 (`platform=rpi1`): ARM-Dynarec, der seinen
  Übersetzungs-Cache per `mmap(PROT_EXEC)` anfordert. Ohne eigenes BIOS nutzt
  gpSP das eingebaute Open-Source-BIOS; ein originales `gba_bios.bin` im
  Wurzelverzeichnis der SD-Karte wird automatisch bevorzugt.

#### Spielstände

Spielstände im Batterie-RAM des Moduls funktionieren bei allen Systemen. Der
Runner (`LoadSaves`, `CheckSaves`, `FlushSaves` in `libretro/libretro_runner.cpp`)
nutzt dafür die libretro-Schnittstelle `retro_get_memory_data/size`:

- Die Datei liegt neben dem ROM, wie bei RetroArch mit „Spielstände im
  Inhaltsverzeichnis“: `Pokemon.gba` → `Pokemon.srm`. Game-Boy-Module mit Uhr
  (MBC3, z. B. Pokémon Gold) bekommen zusätzlich `Pokemon.rtc`. Spielstände aus
  RetroArch können so übernommen werden.
- Geladen wird direkt nach dem Start des Spiels, vor dem ersten Bild.
- Gespeichert wird, sobald das Spiel seinen Speicher geändert und dann eine
  Sekunde lang nicht mehr verändert hat (also nach dem Speichern im Spiel),
  spätestens alle 30 Sekunden bei ständig wechselndem Inhalt, außerdem beim
  Beenden mit Start+Select und beim Ausschalten.
- Mit FatFs schreibt `CircleFs::WriteWholeFile` erst `<Datei>.tmp` und benennt
  sie dann um. Ein Stromausfall mitten im Schreiben hinterlässt so keinen
  halben Spielstand; liegt nur die `.tmp`-Datei vor, wird sie geladen.

Weil der Pi keine batteriegepufferte Uhr hat, die Uhren von Spielen (Game-Boy-
Module mit Uhr, GBA-Spiele wie Pokémon Rubin) aber auf der Systemzeit beruhen,
speichert der Kernel die Zeit in `minerva.time` (beim Ausschalten, beim
Beenden eines Spiels und alle 5 Minuten) und setzt sie beim Booten wieder.
Die Zeit läuft so über Neustarts weiter, nur nicht, während das Gerät aus ist.

#### Dynarec auf Bare Metal

Circle markiert auf dem Pi Zero nur den Kernel-Code als ausführbar, Daten und
Heap sind „execute never“. `mmap`/`mprotect` mit `PROT_EXEC` löschen deshalb das
XN-Bit der betroffenen 1-MB-Sektionen in Circles Seitentabelle
(`ra_libc_make_executable` in `libc/circle_bridge.cpp`). Außerdem ist libgccs
`__clear_cache` für `arm-none-eabi` eine leere Funktion. Schlimmer: GCC weiß
das und lässt Aufrufe von `__clear_cache()` beim Kompilieren ganz weg. gpSP
synchronisierte dadurch nie die Caches und führte auf der echten Hardware
veralteten Code aus (schwarzer Bildschirm), während es unter QEMU, das keine
Caches nachbildet, lief. gpSP wird deshalb mit `-include
libc/include/ra_clear_cache.h` gebaut, das die Aufrufe auf
`ra_libc_clear_cache()` umleitet. Die Funktion arbeitet wie Linux auf dem
ARM1176 (`v6_coherent_user_range`): Datencache für den erzeugten Code
zurückschreiben, dann den ganzen Instruktionscache invalidieren, mit der
Umgehung für Erratum 411920. Nach dem Laden eines Spiels synchronisiert der
Runner zusätzlich einmal alle Caches, weil gpSP beim Laden Hilfscode (BIOS-
Division) erzeugt, den es selbst nicht synchronisiert.

Circle sichert die VFP-Register bei Interrupts nur ab `RASPPI=2`. GCC darf
sie aber in jeder Funktion verwenden, auch in Interrupt-Handlern; das
GPi-Profil setzt deshalb `SAVE_VFP_REGS_ON_IRQ` und `SAVE_VFP_REGS_ON_FIQ`.

Cores mit `need_fullpath` (gpSP) liest der Runner nicht selbst ein, sie öffnen
die Datei über die FatFs-Schicht. Content-Info-Overrides der Cores (PicoDrive)
werden dabei berücksichtigt.

### Bildformat, Farben und Core-Optionen

Konsolen (NES, SNES, Mega Drive, Master System, SG-1000, 32X) werden im
Seitenverhältnis 4:3 ausgegeben, wie auf einem Fernseher, und füllen das
320×240-Display. Handhelds behalten das Format, das ihr Core meldet: Game Boy
10:9, GBA 3:2, Game Gear 4:3. Festgelegt ist das in `libretro/libretro_runner.cpp`
(`displayAspect` je Core, `handheld` je System).
Game Boy-Spiele erscheinen in der grünen Palette des Original-Game-Boys
(Gambatte: `gambatte_gb_colorization = "internal"`, Palette `GB - DMG`).

Core-Optionen lassen sich in `minerva.cfg` im Wurzelverzeichnis der SD-Karte
setzen, im RetroArch-Format `key = "value"`. Sie gelten ab dem nächsten
Spielstart und haben Vorrang vor den eingebauten Vorgaben. Die mitgelieferte
Datei enthält kommentierte Beispiele, etwa eine andere GB-Palette oder
`gpsp_drc = "disabled"` (GBA ohne Recompiler).

### Diagnose: Log-Dateien

Der Kernel schreibt sein Log 15 Sekunden nach Spielstart und beim Ausschalten
auf die SD-Karte, bei schweren Fehlern sofort: eine Datei je System
(`minerva-GBA.log`, `minerva-GB.log`, …), vor dem Spielstart `minerva.log`. Darin stehen
Core-Start, Video- und Audioformat und, für den GBA, ob der Übersetzungs-Cache
ausführbar gemacht werden konnte (`mmap: … executable … ok`). Alle fünf
Sekunden kommt eine Zeile wie

```text
speed 59.7 fps (core 59.7), drawn 298, lit 87%
```

dazu: tatsächlich emulierte Bilder pro Sekunde, Sollwert des Cores, an den
Bildschirm übergebene Bilder und der Anteil nicht schwarzer Pixel im letzten
Bild. Daran lässt sich ein zu langsamer Core von einem unterscheiden, der zwar
läuft, aber nichts zeichnet.

### Tests ohne Hardware

`baremetal/tests/` lässt die Cores so laufen, wie sie für den Kernel gebaut
sind, auf einem emulierten ARM1176 (`qemu-arm -cpu arm1176`):

```sh
node baremetal/tests/make-test-roms.mjs                 # selbst erzeugte Test-ROMs
node baremetal/tests/arm-smoke.mjs                      # Cores + Runner, Linux-Dateizugriff
node baremetal/tests/arm-smoke.mjs --fatfs              # mit libc-Schicht und FatFs-Image
node baremetal/tests/arm-smoke.mjs --fatfs --options=minerva.cfg test.gba
node baremetal/tests/arm-smoke.mjs --fatfs --sequence   # Spielwechsel, siehe unten
node baremetal/tests/arm-smoke.mjs --fatfs --saves      # Spielstände, siehe unten
node baremetal/tests/arm-smoke.mjs --input              # Tastenbelegung, siehe unten
node baremetal/tests/button-mapper-test.mjs             # Tastenlogik Bild für Bild (Host)
node baremetal/tests/tile-preview.mjs                   # ROM-Browser als PNG (Host)
```

`--input` lässt `input-snes.sfc` und `input-gba.gba` mit gedrückten Tasten
laufen (`SMOKE_BUTTONS`, eine Tastenfolge je Bild). Beide ROMs zeigen die
Tasten, die die emulierte Konsole sieht, als Hintergrundfarbe; so wird die
Belegung vom Gamepad bis in das Joypad-Register von SNES und GBA geprüft.

`--saves` lässt die `save-*`-ROMs (je System eine, mit Batterie-RAM) zweimal
als getrennte Prozesse laufen, wie über einen Neustart hinweg. Jede zählt beim
Start das erste Byte ihres Speichers hoch; nach dem zweiten Lauf muss genau ein
Byte um eins gestiegen sein. Mit `--fatfs` laufen die Spielstände dabei über
die echte `circle_fs.cpp` in das FAT-Image.

`--sequence` lässt zusätzlich alle ROMs zweimal hintereinander im selben
Prozess mit demselben Runner laufen (`Init`, Bilder, `Shutdown`), wie beim
Beenden mit Start+Select und dem Start des nächsten Spiels. Jedes Ergebnis muss
dem eigenen Einzellauf exakt gleichen, sonst hat ein Core beim Neustart Zustand
behalten.

Für die übrigen Systeme erzeugt `make-test-roms.mjs` kleine Programme oder
Abbilder, deren Bild sich prüfen lässt: Atari 2600 gelber Hintergrund, Lynx
(Homebrew-Datei, Video-Timer wie das Boot-ROM) rot, PC Engine rot, ZX Spectrum
roter Rahmen, PICO-8 `cls(8)` rot; ZX81 (`.p` ohne Programm), Amstrad und C64
(leere, formatierte Disketten) zeigen ihr BASIC. `invaders.zip` enthält ein
ROM-Set aus Nullen mit den Dateinamen von Space Invaders: MAME warnt wegen der
Prüfsummen und läuft dann. MAME sucht sein ZIP über eine Verzeichnisliste und
läuft deshalb nur im FatFs-Modus des Tests.

Die GBA-Test-ROM läuft wie ein Spiel in Thumb-Code, wartet per BIOS-Aufruf auf
den VBlank-Interrupt und schreibt pro Bild einen Zähler als Hintergrundfarbe.
Nach 300 Bildern muss sie `#584800` zeigen. `syncs=` zählt die Cache-Syncs
eines Cores; bei gpSP muss der Wert größer als 0 sein, sonst läuft der Dynarec
nur unter QEMU. QEMU bildet weder Caches noch die
Seitentabelle des Kernels nach; was davon abhängt, zeigt erst das Log auf
dem Gerät.

### Was die GPi-Linux-Skripte hier ersetzt

Unter Linux braucht das Case Overlays und Skripte. Ohne Betriebssystem
übernehmen das `baremetal/gpi/config.txt` (Firmware-`gpio=`-Zeilen greifen
schon vor dem Kernelstart) und `platform/circle/circle_gpi.cpp`:

| Funktion | Linux | Bare Metal |
|---|---|---|
| Bildschirm (DPI, 240×320 hochkant, gedreht auf 320×240) | `dtoverlay=dpi24-gpi` | DPI-Timings in `config.txt`, `gpio=0-17,20-25=a2`, nochmals in `CircleGpiCase::Initialize` |
| Ton | `pwm-audio`-Overlay | `gpio=18-19=a5`, Circle-PWM-Audio auf GPIO18/19 |
| Soft-Power-Off | `SafeShutdown_gpi.py` | GPIO27 hält den Power-Latch, GPIO26 (Ein/Aus-Schalter) wird im ROM-Browser und im Spiel abgefragt |
| Controller | `xpad`-Treiber (meldet sich als Xbox-360-Pad `045e:028e`) | Circles Xbox-360-Treiber mit GPi-Anpassung (siehe unten) und GPi-Belegung in `circle_input.cpp` |

Für das Soft-Power-Off muss der Schalter „SAFE SHUTDOWN“ des Cases auf ON
stehen. Schaltet man aus, stoppt der Kernel den Ton und gibt den Latch frei,
das Case trennt dann die Stromversorgung. Die serielle Konsole entfällt, weil
GPIO14/15 im Case DPI-Datenleitungen sind.

Der ROM-Browser zeigt jedes Spiel als quadratische Kachel mit dem Namen
darunter (auf dem GPi 3×2 Kacheln je Seite, die Fußzeile nennt den vollen
Namen). Liegt neben dem Spiel ein Bild `<ROM-Name>.png` (z. B.
`Pokemon - FireRed Version (USA).png`, auch `<Dateiname>.png` wie
`Pokemon - FireRed Version (USA).gba.png`), wird es als Kachelbild genommen,
eingepasst ins Quadrat; sonst das Bild des Systems. Die System-Bilder stammen
von wowroms.com (`baremetal/assets/systems/<system>.png`, 152×152) und werden
beim Build in den Kernel eingebettet (`gen/system_images.c`). Eigene PNGs
dekodiert `stb_image` (`baremetal/third_party/stb`), bis 2048×2048 Pixel.
Die Bilder laden nach und nach, die sichtbaren zuerst, und bleiben für den
Ordner im Speicher, jeweils an den Pfad des Spiels gebunden. Gelistet werden
nur Spiele und Ordner, bis zu 512 je Ordner (`CircleFs::ListDirectory` mit
Filter); Bilder, Spielstände und Logs belegen keine Plätze, sonst rutschten
die Spiele nach jedem Speichern (neue FAT-Reihenfolge) auf andere Positionen. Layout und Zeichnen (`kernel/tile_view.cpp`) hängen nicht
von Circle ab; `node baremetal/tests/tile-preview.mjs` rendert dieselben
Ansichten auf dem PC nach `build-node/host-tests/previews/`.

Bedienung: Steuerkreuz links/rechts wechselt die Kachel, hoch/runter die
Reihe, A startet bzw. öffnet einen Ordner, B geht zurück. Das Steuerkreuz
funktioniert im Hat-Modus (Standard) und im Achsen-Modus des Cases.

Im Spiel beendet **Start + Select (½ Sekunde gemeinsam halten)** das Spiel und
kehrt in den ROM-Browser zurück, an die Stelle des gestarteten Spiels. Der
Kernel gibt dabei Ton, Core und Spiel frei (`LibretroRunner::Shutdown`,
`CKernel::StopGame`), speichert geänderte Spielstände und schreibt das Log.

Der GPi-Controller gibt sich als Xbox-360-Pad aus, quittiert aber das
Player-LED-Kommando nicht, das Circles Treiber beim Einrichten ohne Timeout
sendet. Die USB-Initialisierung blieb dadurch bei „usb init...“ hängen (vgl.
rsta2/circle#345). Das GPi-Profil ersetzt deshalb
`circle/lib/usb/usbgamepadxbox360.cpp` durch
`platform/circle/overrides/usbgamepadxbox360.cpp`: LED- und Rumble-Kommandos
laufen mit 100 ms Timeout, ein Fehlschlag wird nur protokolliert.

Obwohl er sich als Xbox-Pad ausgibt, meldet der Controller die Tasten nach
ihrer Beschriftung (GPi-A als Xbox-A usw.), nicht nach Position im Xbox-Layout.
Der Block `RA_BAREMETAL_GPI_CASE` in `platform/circle/circle_input.cpp` legt
deshalb jede Taste auf die RetroPad-Taste gleichen Namens; damit stimmen die
Beschriftungen mit den Tasten von SNES, NES, Game Boy usw. überein (auf dem
Gerät geprüft: A startet im ROM-Browser ein Spiel). Andere bekannte Pads
werden wie in RetroArch nach Position belegt.

Das GPi hat nur A, B, X und Y. Systeme mit mehr Tasten bekommen eine Belegung
(`libretro/button_mapper.h`, je System in der Systemtabelle von
`libretro/libretro_runner.cpp`): die Schultertasten L und R liegen immer auf Y
und X, direkt beim GBA (4 Tasten), zusammen mit Select bei Systemen mit mehr
als 4 Tasten.

| System | Tasten | Belegung |
|---|---|---|
| NES, Game Boy, Master System, Game Gear | 2 | A, B wie beschriftet |
| GBA | A, B, L, R | A, B; Y = L, X = R |
| SNES | A, B, X, Y, L, R | A, B, X, Y wie beschriftet; Select+Y = L, Select+X = R |
| Mega Drive, 32X (6-Tasten-Pad) | A, B, C, X, Y, Z | Y = A, B = B, A = C, X = Y; Select+Y = X, Select+X = Z |

Bei SNES und Mega Drive hält die Belegung Select zurück, damit eine
Kombination nicht auch Select (Mega Drive: Mode) im Spiel drückt: kurz
angetippt erreicht Select das Spiel nach dem Loslassen (6 Bilder lang), länger
als ¼ Sekunde ohne Kombination gehalten wird es durchgereicht.

Für das Mega Drive ist das 6-Tasten-Pad voreingestellt (`picodrive_input1`).
Die wenigen alten Spiele, die damit nicht zurechtkommen, laufen mit
`picodrive_input1 = "3 button pad"` in `minerva.cfg`.

## Bootbild

Statt der Kernel-Textausgabe zeigt der Kernel beim Booten
`baremetal/assets/splash.png`, mindestens 1,5 Sekunden lang, bis der
ROM-Browser erscheint. `build.mjs` wandelt das PNG in ein RGB565-Array um
(`baremetal/splash.mjs`, ohne Zusatzpakete), verkleinert es bei Bedarf auf die
Displaygröße des Boards und bettet es in den Kernel ein. Am Gerät wird es
zentriert und seitenverhältnistreu skaliert (GPi: 320×240, also 1:1).

```sh
node baremetal/build.mjs --board=gpi --splash=/pfad/zum/bild.png   # anderes Bild
node baremetal/build.mjs --board=gpi --splash=0                    # Textausgabe wie früher
```

Die Bootmeldungen und das Circle-Log landen solange in einem Puffer (Anfang
und die letzten ~8 KB). Schlägt etwas fehl (SD-Karte, ROM-Auswahl, Core-Start,
Kernel-Panic), wird das Bild durch das Log ersetzt, damit Fehler sichtbar
bleiben; dasselbe Log landet auf der SD-Karte (siehe „Diagnose“).
