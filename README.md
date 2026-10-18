# Drumbox

**Drum-Synth** · Acht synthetische Drum-Stimmen für Akai Force / MPC, nachgebaut nach
[nb_tonic](https://github.com/sonocircuit/nb_tonic) (sonocircuit) bzw. dessen Vorlage
[supertonic](https://github.com/schollz/supercollider-microtonic) (Zack Scholl), beide inspiriert von Sonic Charge
Microtonic. Lädt **Microtonic-Presets (`.mtpreset`, z. B. aus dem Patternarium)**, **Microtonic-Drum-Sounds
(`.mtdrum`)** und **nb_tonic-Kits (`.tkit`)**, hat einen **Preset-Browser mit Vorhören** (Pattern A des Presets),
einen **Favoritenordner** und **32 eigene Speicherplätze**. Kein Sequencer: gespielt wird über Pads, Clips oder
MIDI-Spuren.
· Lizenz: MIT ([`LICENSE`](LICENSE))

<img src="screenshots/page_0.png" width="760" alt="Drumbox, MASTER">
<img src="screenshots/page_2.png" width="760" alt="Drumbox, Stimme 1">

**Keine Emulation.** Microtonics DSP ist nicht öffentlich. Drumbox ist eine C-Umsetzung von nb_tonics
SuperCollider-SynthDef (`lib/nb_tonic.sc`), Zeile für Zeile. Scholl hat seine Fassung nach Gehör gegen Microtonic
abgeglichen („very close“). Ein Microtonic-Preset klingt in Drumbox also ähnlich, aber nicht identisch.

## Auf der Force

- Im Plugin-Browser: **[DRUM] Drumbox** von **lunarscraper** (Drum Machine)
- Installiert wird ein Ordner: `/sdcard/Synths/lunarscraper - VST - [DRUM] Drumbox/` mit `drumbox.so`, Skin und
  dem Datenordner `drumbox/` (eigene Kits und die Speicherplätze; bleibt bei Updates erhalten)
- 217 Parameter auf 10 Seiten (MASTER, SOUNDS, V1–V8), jede Stimmen-Seite mit drei Q-Link-Bänken (OSC, NOISE, OUT)
- Kits im PRESET-Menü der Force und auf der MASTER-Seite unter KIT

## Spielen

- **Noten 36–43 (C1–G1)** spielen die Stimmen 1–8, auf jedem MIDI-Kanal. Das sind die ersten acht Drum-Pads
  der Force. Mit **V1 NOTE** (MASTER-Seite) lässt sich verschieben: C0, C1, **G#1 (Pads 9–16)**, C2, C3.
- **Velocity** wirkt über die drei VEL-Regler je Stimme (Oszillator, Rauschen, Pitch-Modulation; 0–200 %).
- Jeder Schlag ist ein One-Shot: Note-Offs werden ignoriert.
- Ein neuer Schlag einer Stimme blendet ihren vorigen in 50 ms aus (wie nb_tonic).
- **CHOKE 1–3**: Stimmen in derselben Gruppe würgen sich gegenseitig ab (z. B. offene und geschlossene Hi-Hat).
  OFF = nur die eigene Stimme.
- CC 120 / 123 stoppen alles. CC 20–35 bewegen die Q-Links der MASTER-Seite, NRPN erreicht jeden Parameter
  (wie in allen Plugins mit diesem Framework).

## Presets durchhören (Browser)

Für viele Presets (z. B. 600 aus dem Patternarium): Dateien in einen Unterordner von `drumbox/` legen (siehe
unten), Drumbox einfügen, dann auf der **MASTER**-Seite:

1. Mit den **Pfeilen** am KIT-Feld (oder im PRESET-Menü) zum nächsten Preset. Mit **PREVIEW ON** spielt Drumbox
   dabei sofort das **Pattern A** des Presets zweimal ab: im Tempo der Force, Akzente lauter, die Mutes des Presets
   beachtet. Presets ohne Pattern (nb_tonic-Kits, Speicherplätze ohne Pattern) spielen ihre acht Stimmen
   nacheinander. **PLAY** spielt es noch einmal.
2. Gefällt es: **FAVORIT**. Die Originaldatei (mit allen Patterns) wird nach `drumbox/Favoriten/` kopiert und
   erscheint sofort als „FAV <Name>“ am Ende der Kit-Liste, nach dem nächsten Einfügen alphabetisch einsortiert.
   Sonst einfach weiter zum nächsten.

Das Vorhören läuft nur, solange die Force **nicht spielt**, und endet, sobald du ein Pad spielst, ein anderes Kit
wählst oder PLAY auf der Force drückst. Es ist nur zum Anhören gedacht, kein Sequencer für die Spur.

**Mit den Pads blättern (PAD BROWSE ON, MASTER-Seite):** Die Pads 9–15 bedienen dann den Browser. Das geht nur,
solange die Force nicht spielt, damit ein Clip mit diesen Noten nie das Kit wechselt:

| Pad (bei V1 NOTE = C1) | Note | Funktion |
|---|---|---|
| 9 / 10 | G#1 / A1 | Kit zurück / vor (mit Vorhören) |
| 11 | A#1 | FAVORIT |
| 12 | B1 | Pattern noch einmal (PLAY) |
| 13 / 14 | C2 / C#2 | Sound zurück / vor (SOUNDS-Seite, mit Anspielen) |
| 15 | D2 | TAKE (Sound in die gewählte Stimme) |

Ein Q-Link blättert KIT nicht einzeln (ein Ruck wären 16 Kits), deshalb liegen KIT und SOUND auf keinem Q-Link.

## Einzelne Sounds (.mtdrum, Seite SOUNDS)

`.mtdrum`-Dateien (ein Microtonic-Drum-Sound, `MicrotonicDrumPatchV3`) legst du wie die Presets in `drumbox/` oder
einen Unterordner, z. B. `drumbox/Sounds/`. Der Name kommt aus dem Dateinamen. Auf der Seite **SOUNDS**:

1. Mit den Pfeilen durch die Sounds blättern. Jeder Schritt spielt den Sound auf einer eigenen Vorhör-Stimme an,
   das Kit bleibt dabei unverändert. **PLAY** spielt ihn noch einmal.
2. Unter **TO VOICE** die Zielstimme wählen (V1–V8), **TAKE** übernimmt den Sound in diese Stimme. Ihre
   CHOKE-Gruppe bleibt, alles andere wird ersetzt.
3. Das so zusammengestellte Kit mit **SAVE** in einen Speicherplatz sichern.

## Die Stimme

Pro Stimme, in der Reihenfolge des Signals:

| Teil | Regler | Was passiert |
|---|---|---|
| **Oszillator** | WAVE (SINE/TRI/SAW), FREQ, ATTACK, DECAY, VEL | Hüllkurve exponentiell; FREQ + 5 Hz (Scholls Abgleich) |
| **Pitch-Mod** | DECAY / SINE / NOISE, AMOUNT (±96 Halbtöne), RATE, VEL | DECAY: RATE ist die Abklingzeit (ms, „INF“ = keine), SINE / NOISE: Hz. NOISE mit DECAY ≥ 100 ms ersetzt den Oszillator durch tiefpassgefiltertes Rauschen (wie nb_tonic) |
| **Rauschen** | FILTER (LP/BP/HP), FREQ, Q, STEREO, ENVELOPE (EXP/LIN/CLAP), ATTACK, DECAY, VEL | bei hohem Q blendet das Filter in einen Sinus auf seiner Frequenz über; CLAP: Klatsch-Salven während ATTACK |
| **Ausgang** | MIX, DIST, EQ FREQ, EQ GAIN, LEVEL, PAN, CHOKE | MIX 100 = nur Oszillator, 0 = nur Rauschen (Microtonic-Konvention). EQ GAIN in Microtonics Skala (±40 dB; der EQ wirkt mit der Hälfte, wie bei Scholl) |

**MASTER:** VOLUME, LPF CUTOFF und LPF RESO (Tiefpass auf die Summe, wie nb_tonics globales Filter).

## Kits

Die Liste (PRESET-Menü und KIT) hat immer diese Reihenfolge, damit eine Nummer immer dasselbe Kit meint:

1. **Eingebaut (9):** Init (nb_tonics default), nb ARCHER, nb DISTEC, nb KROUMN, nb LODEKA und vier
   Patternarium-Presets: PA Unmouth Sphing, PA Isober Bewee, PA Apolid Pseudosteo, PA Perlike Epithel
2. **U01–U32:** deine Speicherplätze (`drumbox/USER/01.mtpreset` … `32.mtpreset`)
3. **Alle `.mtpreset`- und `.tkit`-Dateien** im Datenordner `drumbox/` und eine Ordnerebene darunter, alphabetisch
   nach Ordner und Datei; die in `Favoriten/` als „FAV <Name>“. Bis zu 2048 Kits.

Die Namen kommen aus dem Dateinamen („Unmouth_Sphing_139.mtpreset“ wird zu „Unmouth Sphing“). Gelesen wird eine
Datei erst, wenn du sie anwählst. So bleibt das Einfügen auch mit 600 Presets schnell (offline gemessen: 641 Kits in
10 ms gelistet, ein Preset in 0,5 ms gelesen; auf der SD-Karte der Force etwas langsamer).

### Speichern (32 Plätze)

MASTER-Seite, Feld **USER SLOTS**: mit den Pfeilen (oder dem Q-Link) den Platz wählen, **SAVE** tippen. Gespeichert
werden alle acht Stimmen so, wie sie gerade eingestellt sind, als **Microtonic-Preset** (`USER/NN.mtpreset`),
dazu das Pattern A des geladenen Kits (für das Vorhören). Der
Platz heißt danach „U05 <Kitname>“ und erscheint sofort in der Kit-Liste. Ein Projekt speichert seine Einstellungen
ohnehin selbst. Die Plätze sind für Kits, die du in anderen Projekten wiederfinden willst.

### Eigene Presets aus dem Patternarium laden

1. Im Patternarium ein Pattern mit dem Download-Knopf als `.mtpreset` speichern. Mit aktivem Favoriten-Filter lädt
   der Knopf alle Favoriten einer Generation als ZIP. Das ZIP am PC entpacken.
2. Die `.mtpreset`-Dateien in den Datenordner kopieren. Vom PC aus (Eingabeaufforderung oder PowerShell, im Ordner
   mit den Dateien; `<force-ip>` durch die IP-Adresse der Force ersetzen):
   ```
   ssh root@<force-ip> "mkdir -p '/sdcard/Synths/lunarscraper - VST - [DRUM] Drumbox/drumbox/Patternarium'"
   scp *.mtpreset "root@<force-ip>:/sdcard/Synths/lunarscraper - VST - [DRUM] Drumbox/drumbox/Patternarium/"
   ```
   Ein Unterordner (hier `Patternarium`) hält Ordnung, eine Ebene tief wird gelesen.
3. Drumbox von der Spur entfernen und neu einfügen: Die Kits werden beim Einfügen gelesen.

Gelesen werden die acht Klänge und, nur fürs Vorhören, Pattern A mit Tempo, StepRate und Mutes. Die Patterns B–L,
Swing und Fills nicht. Der Tempo-Zusatz im Namen („(139)“) wird abgeschnitten. nb_tonic-Kits (`.tkit`) gehen genauso.

Rechte: Patternarium-Patterns sind laut Sonic Charge gemeinfrei (public domain). Die vier eingebauten stammen von
dort, nb_tonics Kits stehen unter MIT (`kits/nb_tonic/LICENSE`).

## Unterschiede zu Microtonic

- Kein Sequencer (die Force sequenziert); Pattern A nur zum Vorhören, ohne Swing und Fills
- Keine getrennten Ausgänge A/B (MPC spielt nur das erste Stereo-Paar eines Plugins)
- Velocity nach nb_tonics Modell, nicht Microtonics
- Klang nah dran, nicht identisch (siehe oben)

## Installieren

Aus einem Release: `DRUM-Drumbox-<version>-mpc-armv7.zip` auf die Force kopieren, entpacken und im entpackten Ordner
als root `sh install.sh` ausführen. Der Installer stoppt die MPC-Software, sichert `MPC.settings`, installiert den
Plugin-Ordner und startet neu. `INSTALL.md` im ZIP beschreibt das ausführlich.

## Bauen (GitHub Actions)

1. Repo zu GitHub hochladen (z. B. `lunarscraper/mpc-vst-drumbox`).
2. **Actions → „Release (draft)“ → Run workflow**, Version eintragen (z. B. `0.1.0`).
3. Der Workflow baut mit den Werkzeugen aus
   [saustin2010/mpc-vst-plugins](https://github.com/saustin2010/mpc-vst-plugins) (Commit `eaef502`, derselbe wie
   bei Percolator) die ARM-Bibliothek und den Skin, führt den Host-Test aus und legt einen Release-Entwurf mit dem ZIP an.
4. ZIP auf der Force installieren und testen, danach den Entwurf veröffentlichen.

Lokal (mit Docker): `python3 mpc/gen.py`, dann `bash "$MPC_VST/tools/build_port.sh" vst.json` und
`bash "$MPC_VST/tools/test_port.sh" vst.json`.

## Wie es gemacht ist

| Datei | Inhalt |
|---|---|
| `src/voice.c` | die Stimme (nb_tonics SynthDef in C), die Regler-Kurven |
| `src/kits.c` | Kit-Liste (träges Laden), `.mtpreset`-, `.mtdrum`- und `.tkit`-Leser, Umrechnung der Einheiten, SAVE, FAVORIT |
| `src/drumbox.c` | Parameter, MIDI, Kit-Wechsel, Vorhören (Pattern A), PAD BROWSE, Master (VOLUME, LPF), Zustand, Anzeigen |
| `mpc/art.py` | zeichnet die großen Touch-Knöpfe (`images/`; von Hand ausführen, braucht Pillow) |
| `src/dsp.h` | Biquads wie SuperColliders BLowPass/BBandPass/BHiPass/BPeakEQ, softclip, Sinus-Tabelle |
| `mpc/gen.py` | schreibt `params.json`, `src/params_def.h`, `layout.conf` und `src/builtin_kits.h` aus einer Liste |
| `mpc/render.c` | Offline-Renderer: jedes Kit, jede Stimme einzeln und ein Groove als WAV, mit Pegeln |
| `kits/` | die Quellen der eingebauten Kits |
| `drumbox.css` | das Aussehen des Skins (HTML-Renderer des Frameworks) |

Die Parameter-Reihenfolge ist der VST-Index, über den Projekte und Q-Links gespeichert werden: neue Parameter nur
hinten anhängen (`mpc/gen.py`).

## Status (2026-10-18)

Offline geprüft, noch **nicht auf der Force**:

- Host-Test des Frameworks (`tools/test_port.sh`, x86, ASan/UBSan): **PASSED** (60 Prüfungen: zwei Instanzen,
  Parameter, Q-Links, Kit-Liste im PRESET-Menü, Noten → Audio, Zustand speichern/laden, zwei Threads)
- Skin gebaut ohne Warnungen (Überlappungen, Ränder, Q-Links), Vorschau in `screenshots/`
- Zufallstest: 7,7 Mio. Samples mit zufälligen Extremwerten aller Regler, keine ungültigen Zahlen, keine
  Speicherfehler (ASan/UBSan)
- Import: Werte eines Patternarium-Presets 1:1 gegen die Datei geprüft; SAVE → neu laden für alle 9 eingebauten
  Kits gleich bis auf 0,0005 Regler-Einheiten; Projektzustand identisch wiederhergestellt
- Pegel: Grooves der eingebauten Kits −2,6 bis −10,9 dBFS Spitze
- Rechenzeit, schlimmster Fall (16 lange Stereo-Klänge gleichzeitig, Säge, Distortion, LPF): rund 4 % eines
  x86-Kerns (2,1 GHz). Auf der Force mit `tools/bench.sh` nachmessen.
- Browser (0.2.0): Ordner mit 600 Presets, 3 `.mtdrum` und Favoriten. Pattern A eines Presets gegen die Datei
  geprüft (Trigger, Akzente, Tempo, Mutes). Das Vorhören spielt 32 Schritte im Host-Tempo, startet nicht und stoppt,
  wenn die Force spielt, und stoppt bei einem Pad. FAVORIT kopiert die Datei, ein zweites Mal meldet es „schon
  Favorit“. PAD BROWSE blättert Kit und Sound, TAKE übernimmt in die Stimme, der Bildschirm wird benachrichtigt.
- `.mtdrum` mit drei echten Dateien geprüft (`MicrotonicDrumPatchV3`, Zeilen `Key: Wert Einheit`, mit und ohne
  Anführungszeichen): alle Werte kommen wie in der Datei an, ModRate je nach Einheit als ms oder Hz, Mix als
  „Osc / Noise“. Der Leser versteht beide Schreibweisen (`Key = Wert` und `Key: Wert Einheit`), auch in `.mtpreset`.
