# RetroArch/libretro Bare-Metal Circle MVP

This directory is the first practical slice of a Raspberry Pi 5 bare-metal
RetroArch direction: a small Circle application that acts as a static libretro
runner. It is intentionally not the full RetroArch frontend yet.

## Status

- Target: Raspberry Pi 5, AArch64, Circle.
- Output: Circle normally emits `kernel_2712.img` for Pi 5.
- Core loading: static only.
- Included test core: `libretro/builtin_pattern_core.cpp`.
- Video: Circle screen framebuffer path, RGB565-oriented.
- Audio: HDMI sound device path, signed 16-bit stereo batches.
- Input: Circle USB HID gamepad mapped to RetroPad.
- Filesystem: SD card `emmc1-1` mounted through Circle native FAT.

## Build outline with Node.js

Clone Circle next to this repository as `circle` or pass
`CIRCLEHOME=/path/to/circle`. The Node build script compiles the required
Circle libraries and this runner directly; it does not call `make`.

From the repository root:

```sh
npm run build:baremetal
```

Der NES-Prototyp mit statisch gelinktem FCEUmm-Core wird so gebaut:

```sh
npm run build:baremetal:nes
```

Oder direkt mit eigenen Parametern:

```sh
node baremetal/build.mjs --core=fceumm --rom=GAME.NES
```

The default output on Raspberry Pi 5 is:

```text
baremetal/build-node/<core>/kernel_2712.img
```

For a `kernel8.img` experiment, rename the output and set `kernel=kernel8.img`
in `config.txt`. The Circle/Pi-5-native filename remains `kernel_2712.img`.

## SD card

Minimum files on the FAT boot partition:

```text
kernel_2712.img
bcm2712-rpi-5-b.dtb
overlays/bcm2712d0.dtbo
config.txt
cmdline.txt
```

The current built-in pattern core does not require a ROM. For the NES/FCEUmm
slice, put a short-name file such as `GAME.NES` into the FAT root and build
with `npm run build:baremetal:nes` or pass `--rom=<name>`.

## Raspberry Pi Zero / Zero W im Retroflag GPi Case

Das Board-Profil `gpi` baut denselben Runner für den Pi Zero (BCM2835,
ARM1176, 32 Bit, Circle `RASPPI=1`) im Retroflag GPi Case. Standard ist das
Core-Bündel `--core=all` mit fünf Cores in einem Kernel:

| System | Core | Dateiendungen |
|---|---|---|
| NES | FCEUmm | `.nes` |
| Game Boy / Game Boy Color | Gambatte | `.gb` `.dmg` `.gbc` |
| Game Boy Advance | gpSP (ARM-Dynarec) | `.gba` `.agb` |
| SNES | Snes9x 2002 | `.sfc` `.smc` `.swc` `.fig` |
| Mega Drive, Master System, Game Gear, SG-1000, 32X | PicoDrive | `.md` `.gen` `.smd` `.bin` `.sms` `.gg` `.sg` `.32x` |

N64 braucht den AArch64-Dynarec und mehrere Kerne und ist auf dem Zero nicht
möglich. Mega-CD fehlt (braucht BIOS und CD-Images).

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
   dadurch nicht mehr.

`baremetal/libc/` verbindet newlib mit Circle: Speicher kommt aus Circles Heap
(newlib verwaltet keinen eigenen). Circles `memalign` kann nur bis zur
Cache-Zeilengröße (32 Byte) ausrichten; größere Ausrichtungen (`mmap`: 4 KB,
libretro-Dateipuffer: 64 Byte) holt die Glue-Schicht selbst und ersetzt dafür
`free`/`realloc`/`memalign`. Dateien und Verzeichnisse laufen über FatFs
(`SD:`), `stdout`/`stderr` landen im Circle-Log. Dazu kommen `<dirent.h>` und
`<sys/mman.h>`, die newlib für `arm-none-eabi` nicht hat.

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
Ordner im Speicher. Layout und Zeichnen (`kernel/tile_view.cpp`) hängen nicht
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
