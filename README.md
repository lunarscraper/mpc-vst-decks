# mpc-vst-decks

Arbeitstitel **Decks** · VST2-Instrument für die Akai Force (MPC OS, armhf) · UID `Dcks` · Vorlage: `mpc-vst-rattler`

Schlanker DJ-Player mit zwei Decks und Crossfader in **einem** Plugin, angelehnt an [Mixxx](https://github.com/mixxxdj/mixxx). Mixxx selbst ist eine Qt-Desktop-Anwendung und wird nicht portiert; übernommen sind Konzepte (zwei Decks, Sync, Quantisierung, Beatgrid). Es steckt kein Mixxx-Code im Plugin, daher MIT-Lizenz.

## Festgelegte Eckpunkte

| Thema | Entscheidung |
|---|---|
| Tempo | **Ab 0.2.0: das zuletzt gestartete Deck ist Master** und spielt in seinem eigenen Tempo, das andere folgt (`CLOCK DECKS`, Voreinstellung). Wahlweise wie in 0.1.0 die Force-Clock als Master (`CLOCK FORCE`) |
| Pitch | **Vinyl-Modus** – Tonhöhe ändert sich mit dem Tempo, kein Time-Stretching (minimale CPU-Last) |
| Vorhören | **kein Cue-Ausgang** – nur Master-Summe (Stereo-Out) |
| Plugin-Typ | Instrument, 0 In / 2 Out, belegt nur einen der 8 Plugin-Plätze |
| Bedienung | Force-Pads, Q-Links (nur Regler, keine Tasten) und große Touch-Flächen |
| Samplerate | Force 44,1 kHz; Tracks mit anderer Rate (z. B. 48 kHz) werden über die Abspielrate ausgeglichen (gleiches Ergebnis wie Umrechnen beim Laden, spart Zeit und RAM) |

## Update 0.4.0 – Stil-Crates und AUTO

Für den Einsatz als Notlösung, Pausenfüller und für kurze private Sessions, mit Tracks, die in Ordnern nach Stilen vorsortiert sind.

**Stil-Crates (BROWSER):** Jeder Ordner direkt unter einem Musikordner (Quelle) ist ein Stil. Oben im BROWSER wählst du mit den Pfeilen oder dem STYLE-Regler (Q-Link 1) zwischen `ALL` und den Stilen, z. B. `STYLE 3/7: Goa (54 TRACKS)`; die Liste zeigt dann nur diesen Stil, Unterordner darin eingeschlossen. Gleich benannte Stilordner auf verschiedenen Laufwerken landen in einem Crate. Tracks, die direkt im Musikordner liegen, bilden `(TOP FOLDER)`. Wähle als Quelle also den Hauptordner, in dem die Stilordner liegen. Nach RESCAN oder einem Laufwerkswechsel bleibt der gewählte Stil erhalten.

**AUTO (BROWSER, Pad 48):** Spielt das gewählte Crate selbstständig, **in fester Reihenfolge** (wie in der Liste), beginnend beim gewählten Track; nach dem letzten geht es mit dem ersten weiter.
- Läuft nichts, lädt AUTO den gewählten Track auf A und startet ihn; der Crossfader geht ganz auf A.
- Das freie Deck bekommt sofort den nächsten Track. Dessen Anzeige zählt herunter: `AUTO NEXT in 12 BARS`.
- **AUTO MIX** (SETUP: 4, 8, 16, 32 Takte; Voreinstellung 8) Takte vor Trackende startet das nächste Deck auf dem Takt, der Crossfader fährt in dieser Zeit hinüber, danach stoppt das alte Deck und bekommt den übernächsten Track. Das Tempo geht über GLIDE zum neuen Track (bei `CLOCK DECKS`).
- Gestartet wird am ersten Kick des Tracks (Cue nach dem Laden); ein Track ohne Tempo wird wie 120 BPM gezählt.
- Eingreifen geht jederzeit: einen anderen Track aufs wartende Deck laden (der kommt dann als Nächstes), selbst mischen (spielen beide Decks, hält sich AUTO heraus), Lautstärken, Pitch. **Stoppst du von Hand alles, schaltet AUTO ab** (`AUTO OFF (STOPPED BY HAND)`) und startet nichts neu.
- AUTO wird nicht mit dem Projekt gespeichert – ein geladenes Projekt fängt nie von selbst an zu spielen.
- Ein Track, der sich nicht lesen lässt, wird übersprungen; nach fünf Fehlschlägen in Folge schaltet AUTO ab.

Grenze: Decks läuft im Programm der Force. Stürzt die Force als Ganzes ab, ist auch Decks weg; für diesen Fall braucht es einen unabhängigen Zuspieler.

## Update 0.3.2 – Anzeigen laufen mit

Auf der Force blieben Zeit und Positionsbalken eines laufenden Decks stehen. MPC liest den Text von Anzeige-Feldern nur nach einem `UpdateDisplay` neu – es fragt ihn nicht von selbst ab. Gelöst wie im gemeinsamen Wrapper (und in reMixXXL, das ihn nutzt): Alle 100 ms prüft Decks, ob sich etwas Sichtbares geändert hat (Sekunde, Schlag, Zustand, Tempo, Statuszeile, Listen, Ordner), und fordert dann ein `UpdateDisplay` an – höchstens 10 pro Sekunde. Die Lock-Abweichung in ms allein löst keine Aktualisierung aus (sie würde sonst ständig neu zeichnen); sie läuft mit jedem Schlag mit.

## Update 0.3.1 – Stand von mpc-vst-plugins übernommen

Decks nutzt wie Rattler eine **handgeschriebene VST2-Hülle**, nicht den gemeinsamen Wrapper `vst2_wrap.c` – Wrapper-Fixes kommen also nicht von selbst an. Abgeglichen mit `open-mpc/mpc-vst-plugins` (Stand `8e266ce`, 2026-11-04) und übernommen, was für die Hülle gilt:
- **Doppelter Tastendruck** (Fix 2026-10-30): MPC liefert einen Tipp auf eine gezeichnete Taste als zwei Drücke 30–140 ms hintereinander. Ein zweiter Druck innerhalb von 250 ms wird ignoriert – für alle Tasten und für Antippen von Track- und Ordnerzeilen. Ohne das startete PLAY und stoppte sofort wieder, OPEN ging zwei Ordner tief.
- **UpdateDisplay** (Fixes 2026-10-21): kein Aufruf mehr direkt nach einem Tastendruck, höchstens einer pro 50 ms. MPC baut bei jedem Aufruf den Bildschirm neu auf, das konnte Q-Links beim Drehen blockieren.
- **Popups** (Fix 2026-10-07): Öffnen einer Auswahlliste schließt die anderen.
- Workflow zeigt auf `open-mpc/mpc-vst-plugins` (Umzug von `sd88me`), `params.lock` (neu im Generator: hält die Parameter-Reihenfolge fest) liegt bei.

Nicht übernommen, weil für Decks ohne Wirkung: NRPN/CC-Steuerung des Wrappers, Effekt-Eingang bei In-place-Puffern (Decks ist ein Instrument), Gen2-Kernzuordnung, das Halten von Parameter 0 beim STOP (Parameter 0 ist VOL A; MPC setzt ihn kurz ans Ende und sofort zurück, das wirkt erst im nächsten Block und damit gar nicht).

## Update 0.3.0 – Bedienung am Gerät

Nach dem Test auf der Force:
- **Ordner per Regler:** Der FOLDER-Regler wählt jetzt einen Ordner aus der ganzen Liste (markierte Zeile, die Liste blättert seitenweise mit), **OPEN** öffnet ihn. Antippen einer Zeile öffnet weiterhin direkt. Gleiches Prinzip wie TRACK + LOAD A/B im Browser.
- **Regler bewegen sich:** Alle Bedienelemente melden sich jetzt beim Host als automatisierbar – vorher nur die Mixer-Einstellungen, vermutlich der Grund, warum FOLDER (und TRACK) auf den Q-Links nicht reagierten. Die Regler-Anzeige zeigt `3 / 63` und wird nach jedem Einlesen aktualisiert (vorher blieb `-` stehen).
- **SETUP lesbar:** Keine kleine eingebrannte Pixelschrift mehr. CLOCK, GLIDE und QUANTIZE sind Auswahlfelder, Erklärung und Pad-Belegung stehen in Textzeilen in der Gerätschrift.

## Update 0.2.0 – eigene Clock

Erfahrung aus dem ersten Test auf der Force: Mit der Force-Clock lief Decks nur bei laufendem Transport, das Tempo der Force galt für alle Tracks, und die Force stoppt nach 999 Takten. Deshalb gibt es jetzt eine eigene Clock im Plugin.

**`CLOCK DECKS` (Voreinstellung)** – unabhängig vom Force-Transport:
- Startet ein Deck, während das andere steht, startet es sofort und gibt **sein eigenes Tempo** vor (`LEAD`). PITCH verändert dieses Tempo wie ein Pitch-Fader.
- Startet das zweite Deck, während das erste läuft, wartet es auf dessen nächsten Takt (QUANTIZE BAR) und übernimmt dann die Führung: **sein Tempo gilt**, das bisherige Deck folgt in Tempo und Phase (`SYNC`, nur mit SYNC an).
- **GLIDE** (OFF, 1, 2, 4, 8, 16 Takte; Voreinstellung 4) führt das Tempo vom alten zum neuen Track, statt zu springen: Beide Decks laufen zuerst im bisherigen Tempo und erreichen nach der eingestellten Zeit das Tempo des neuen Tracks. Bei OFF springt das auslaufende Deck sofort ins neue Tempo.
- Stoppt das führende Deck (oder endet sein Track), übernimmt das andere – ebenfalls über GLIDE zu seinem eigenen Tempo.
- Ein Deck mit SYNC aus läuft unabhängig mit seinem PITCH.

**`CLOCK FORCE`** – wie in 0.1.0: beide Decks folgen der Song-Position der Force. Neu: Steht die Song-Position still, gilt der Transport als gestoppt, auch wenn die Force ihn als laufend meldet – die Decks laufen dann frei im letzten Tempo weiter, statt hängen zu bleiben.

Umschalten auf der Seite SETUP (CLOCK, GLIDE, QUANTIZE, MASTER; auch auf den Q-Links). Die Anzeige je Deck zeigt `LEAD`, `SYNC`, `PLAY` (frei), `FREE` (Force-Clock, Force steht), `WAIT BAR`/`WAIT BEAT` oder `STOP`.

Die übrigen Force-Spuren laufen bei `CLOCK DECKS` nicht automatisch mit den Decks mit. Ein möglicher nächster Schritt: Decks sendet MIDI-Clock über einen eigenen MIDI-Port (wie Acid), und die Force folgt ihr als externer Clock – ob die Force einen Plugin-Port als Clock-Quelle annimmt, muss am Gerät getestet werden.

## Stand Phase 1

Umgesetzt in drei Teilen plus Hülle:

| Datei | Inhalt |
|---|---|
| `vst/decks_engine.h` | ein Deck: Wiedergabe aus dem RAM, Vinyl-Modus, Kopplung an die Force-Clock |
| `vst/decks_analysis.h` | Tempo- und Beatgrid-Erkennung |
| `vst/decks_library.h` | Laufwerke, Musikordner, Track-Liste, Index, Hintergrund-Thread, Dekodierung |
| `vst/decks_vst.cpp` | Plugin-Hülle: Parameter, Skin-Texte, Pads, Mixer, Projekt-Chunk |
| `vst/third_party/` | dr_wav, dr_flac, dr_mp3 (gemeinfrei / MIT-0), unverändert |

### Bedienseiten

| Seite | Inhalt | Q-Links |
|---|---|---|
| DECKS | links Deck A, rechts Deck B: Titel, Tempo/Zeit, Positionsbalken mit Takt.Schlag und Lock-Abweichung; PLAY, CUE, SYNC je Deck; MASTER und X-FADE in der Mitte | 1 PITCH A · 2 NUDGE A · 3 VOL A · 4 X-FADE · 5 VOL B · 6 NUDGE B · 7 PITCH B · 8 MASTER (gespiegelt wie ein DJ-Mixer) |
| BROWSER | Stil-Wahl (Pfeile), Track-Liste mit 8 großen Zeilen (Tempo, Name, `[A]`/`[B]` für geladene Tracks), Antippen wählt aus; LOAD A, LOAD B, AUTO, RESCAN; Statuszeile | STYLE, TRACK (blättert durch die ganze Liste) |
| SOURCE | Laufwerk (Pfeile), aktueller Ordner, 6 Unterordner (Antippen öffnet), OPEN (gewählter Ordner), UP, USE FOLDER, REMOVE; Quellen-Übersicht, Status | DRIVE, FOLDER (wählt den Ordner) |
| SETUP | CLOCK (DECKS/FORCE), GLIDE, QUANTIZE (OFF/BEAT/BAR), AUTO MIX, MASTER, Pad-Belegung | CLOCK, GLIDE, QUANTIZE, AUTO MIX, MASTER |

Außerhalb des Seiten-Folgemodus liegen die DECKS-Regler auf den Q-Links.

**Keine Wellenform:** MPC-Skins können keine Live-Bilder zeichnen (nur Parametertexte, statische Bilder, Tasten – siehe `docs/NOTES.md` in `mpc-vst-plugins`). Phase 1 zeigt daher Text-Anzeigen: Positionsbalken `[|||||.....]`, `BAR 12.3` und die Lock-Abweichung in ms. Eine echte Wellenform ginge über eine Browser-Ansicht im WLAN der Force (wie der CC-Map-Hub) – Kandidat für eine spätere Phase.

### Laufwerke und Musikordner

- Erkannt wird jedes eingehängte Dateisystem unter `/media/…`, `/sdcard`, `/mnt/…` und `/run/media/…` (aus `/proc/mounts`, Systemdateisysteme ausgelassen, ein doppelt eingehängtes Gerät nur einmal). Auf der Force: interner Speicher (`INTERNAL`), `SDCARD`, eine eingebaute SSD und gesteckte SD-Karten oder USB-Sticks (unter ihrem Namen).
- Auf der Seite SOURCE: Laufwerk mit den Pfeilen wählen, Ordner antippen zum Öffnen, UP eine Ebene zurück (nie über das Laufwerk hinaus), **USE FOLDER** macht den angezeigten Ordner zur Quelle dieses Laufwerks, REMOVE nimmt sie wieder heraus.
- Pro Laufwerk eine Quelle, mehrere Laufwerke gleichzeitig (z. B. SSD-Sammlung + SD-Karte mit dem Set des Abends).
- **Automatisches Einlesen:** beim Start, alle 2 s Prüfung auf neue/entfernte Laufwerke (dann neu einlesen) und per RESCAN. Rekursiv, WAV, AIFF, FLAC, MP3; versteckte Dateien und macOS-`._`-Dateien werden übergangen, Verknüpfungen nicht verfolgt. Höchstens 20 000 Tracks.
- Fehlt ein Laufwerk, bleiben seine Tracks in der Liste und sind als `OFFLINE` markiert (nicht ladbar), bis es wieder steckt.
- Zwei Dateien neben dem Plugin-Ordner (wie die Rattler-Presets): `decks_sources.txt` (Quellen) und `decks_index.txt` (Track-Liste mit Größe, Zeitstempel, Tempo, erstem Kick). Ein Track wird nur einmal analysiert; ändert sich die Datei, wird neu analysiert.
- Umlaute in Datei- und Ordnernamen werden in der Anzeige ausgeschrieben (`ä` → `ae`), andere Sonderzeichen als `?`.

### Laden und Analyse

- Laden nur auf ein **gestopptes** Deck (Live-Schutz: ein laufendes Deck wird nie ersetzt, die Statuszeile sagt `DECK A IS PLAYING - STOP IT FIRST`).
- Dekodiert wird im Hintergrund-Thread (niedrige Priorität), das andere Deck spielt ungestört weiter. Der Track liegt komplett als 16-Bit-Stereo im RAM (≈ 75 MB für 7 Minuten), höchstens 20 Minuten.
- **Tempo und Beatgrid** beim ersten Laden (Statuszeile `ANALYSING`): Einsatzkurve aus tiefem Band (Kick) und hohem Band (Hats), grobes Tempo per Autokorrelation (85–180 BPM, ein halbes Tempo wird verdoppelt), feines Tempo durch Falten der Einsatzkurve über die Schlaglänge (auf Tausendstel BPM), danach sample-genaues Raster über die Kick-Einsätze (Gerade durch bis zu 256 Kicks). Ein Tempo knapp an einer ganzen Zahl wird auf diese gerundet. Von den vier 16teln gilt das mit dem lautesten Pegel als Schlag – damit rastet das Raster bei rollender Psytrance-Bassline nicht auf eine Bassnote ein.
- Der **Cue-Punkt** nach dem Laden ist der **erste Kick** des Tracks (= Schlag 1 des Rasters). Mit QUANTIZE BAR liegen damit die Takte von Track und Force übereinander.
- Ohne erkennbaren Beat (Ambient, Flächen) zeigt das Deck `NO TEMPO`; es spielt dann frei mit PITCH, SYNC ist wirkungslos.

### Synchronisation zur Force (`CLOCK FORCE`)

- Abspielrate = Force-BPM ÷ Track-BPM (Vinyl: 128-BPM-Track bei 140 BPM → +9,4 %, gut 1,5 Halbtöne).
- **Phasenkopplung:** Beim Start merkt sich das Deck, wie sein Schlagzähler zur Song-Position der Force steht, und hält diesen Bezug **modulo einem Takt**. Dadurch bleibt ein Deck beim Loopen oder Springen der Force (ganze Takte) einfach weiterlaufen, auf demselben Schlag im Takt. Kleine Abweichungen werden über die Rate mit höchstens ±3 % nachgezogen, größere als ¼ Schlag (Transport neu gestartet, SYNC eingeschaltet) springen mit 6 ms Überblendung.
- **QUANTIZE:** OFF = PLAY startet sofort (nur Tempo-Sync) · BEAT = Start auf dem nächsten Schlag · BAR = Start auf dem nächsten Takt (Voreinstellung).
- **NUDGE** verschiebt die Phase live um bis zu ±½ Schlag. Das gehört eigentlich zur Grid-Korrektur aus Phase 2, ist aber für den Live-Einsatz von Anfang an nötig: Sollte ein Raster einmal um ein 16tel danebenliegen, holt NUDGE es zurück.
- Force-Transport gestoppt: Ein laufendes Deck läuft im zuletzt gültigen Tempo weiter (`FREE`). Startet der Transport, rastet es auf den nächsten Schlag ein (QUANTIZE OFF: Phase bleibt, wie sie ist).
- SYNC aus: Abspielen mit PITCH ±8 % wie am Plattenspieler.

### PLAY, CUE, Mixer

- **PLAY** startet (quantisiert) bzw. pausiert. `WAIT BAR`/`WAIT BEAT` zeigt das Warten auf den Takt.
- **CUE** bei laufendem Deck: zurück zum Cue-Punkt und Stopp. Bei gestopptem Deck: Cue-Punkt hierher setzen (auf den nächsten Schlag).
- Lautstärke je Deck, **Crossfader** (Mitte: beide voll; zur Seite blendet nur die Gegenseite aus), MASTER. Die Summe läuft durch einen weichen Begrenzer über −3 dBFS (Decke 0,98).

### Pads (Noten auf der Plugin-Spur)

| Note | Deck A | Note | Deck B | Note | Liste |
|---|---|---|---|---|---|
| 36 | PLAY | 40 | PLAY | 44 | Track hoch |
| 37 | CUE | 41 | CUE | 45 | Track runter |
| 38 | SYNC an/aus | 42 | SYNC an/aus | 46 | Seite hoch |
| 39 | LOAD (gewählter Track) | 43 | LOAD | 47 | Seite runter |
|  |  |  |  | 48 | AUTO an/aus |

Die Pads müssen chromatisch ab C1 (36) senden, z. B. im Pad-Modus „Notes“ ohne Skala oder per Drum-Pad-Belegung.

### Projekt

Mit dem Projekt gespeichert werden Lautstärken, Crossfader, MASTER, SYNC, PITCH, NUDGE, QUANTIZE, CLOCK, GLIDE und die Tracks auf beiden Decks. Beim Laden des Projekts werden die Tracks wieder geladen (gestoppt, am ersten Kick).

### Offline-Test (`vst/test.sh`)

Der Host-Test schreibt eigene Test-Tracks in einen Temp-Ordner mit zwei „Laufwerken“ (`DECKS_ROOTS`) und prüft Laufwerksliste, Ordner-Browser, USE FOLDER, Einlesen mit Unterordnern und Sortierung, Umlaute, die Analyse (128 BPM mit Intro, 140 BPM mono bei 48 kHz, Fläche ohne Beat), PLAY auf den Takt, Phasenkopplung (alle Kicks auf den Force-Schlägen: schlechtester Wert 0,8 ms), NUDGE, Loop-Sprung der Force, SYNC aus mit PITCH, freien Lauf bei gestopptem Transport, CUE, Laden auf laufendes Deck verweigert, Pads, Projekt-Chunk, Laufwerk weg und wieder da, Zufalls-Stresstest, keine NaN/Denormals. Ab 0.2.0 zusätzlich: eigene Clock (A führt allein bei 128 BPM ohne jede Force-Clock, B startet auf A's Takt und führt bei 140, A folgt auf denselben Schlag, B stoppt und A gleitet zurück auf 128) und eine stehende Song-Position bei „laufend“ gemeldetem Transport. Mit ASan/UBSan. CPU (x86, ein Kern): beide Decks synchron 0,13 %.

Zusätzlich lokal geprüft: FLAC, MP3 (320 kbit/s) und AIFF werden gelesen und gleich analysiert; ein 7-Minuten-MP3 braucht auf dem PC 0,6 s für Dekodieren und Analyse – auf der Force grob geschätzt 5–10 s.

### Bauen

GitHub → Actions → „VST release (draft)“ → Run workflow (Version `0.1.0`, `dry_run` an für einen reinen Build). Der Workflow baut `decks.so` für armhf, erzeugt den Skin, lässt `vst/test.sh` laufen und legt Skin-Vorschauen als Artefakte ab.

## Fahrplan

### Phase 1 – Grundgerüst ✔
- Zwei Decks, Crossfader, Lautstärke je Deck, MASTER
- Laufwerkserkennung, frei wählbare Ordner, automatisches Einlesen, Browser
- Laden WAV/AIFF/FLAC/MP3 im Hintergrund
- BPM-/Beatgrid-Analyse, Force-Clock-Sync mit Phasenkopplung, quantisierter Start, NUDGE
- Skin mit Text-Anzeigen (statt Wellenform), Pad-Steuerung für Play/Cue/Sync/Laden

### Phase 2 – Mischen
- 3-Band-EQ mit Kill und Ein-Knopf-Filter (HP/LP) je Deck
- 4 Hot-Cues je Deck, quantisierte Loops (1/2 bis 16 Beats), Loop-Roll
- Grid-Korrektur (×2, ÷2, Verschieben – dauerhaft im Index gespeichert)
- CC-Steuerung mit einstellbarem Kanal

### Phase 3 – Komfort
- Auto-Übergang: Crossfade über 4/8/16/32 Takte inkl. EQ-Bass-Tausch
- Einfache Send-Effekte (Echo im Takt, kurzer Hall) je Deck
- PC-Skript zur Vorab-Analyse großer Sammlungen (schreibt den Index direkt aufs Laufwerk)
- Optional STEMS-Mitschnitt der Master-Summe (wie Percolator)
- Optional Wellenform im Browser (WLAN der Force)

## Offene Tests auf der Force

1. Welche Laufwerke erscheinen (Namen, Einhängepunkte) – interne SSD, gesteckte SD-Karte, USB-Stick; Lesezugriff auf alle.
2. Erkennt das Plugin das Einstecken/Entfernen einer SD-Karte im laufenden Betrieb?
3. RAM-Spielraum bei zwei geladenen Tracks neben den übrigen Plugins.
4. Lade- und Analysezeit eines 7-Minuten-MP3 auf der Force-CPU.
5. Kommen die Force-Pads auf der Plugin-Spur als Noten 36–47 an?
8. `CLOCK DECKS`: läuft Decks bei gestoppter Force, und klingt GLIDE beim Übergang gut (welche Länge passt)?
6. Passen Raster und Sync bei echten Tracks (Goa/Psy, Techno)? Liegt ein Raster daneben: Track und Abweichung notieren.
7. Werden die Text-Anzeigen (Titel, Position, Track-Zeilen) in voller Länge dargestellt?

## Referenzen

- [Mixxx](https://github.com/mixxxdj/mixxx) – Vorbild für Bedienkonzept, Sync und Quantisierung
- [mackron/dr_libs](https://github.com/mackron/dr_libs) – Dekoder
- `sd88me/mpc-vst-plugins` – Build-Werkzeuge, Skin-Generator, Workflow
