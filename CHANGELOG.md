# Changelog

All notable changes to this project are listed here, newest first. The version numbers follow the rules in [docs/VERSIONING.md](docs/VERSIONING.md).

## 0.2.15

- Two plugins against noise come with the program (FX > Add an effect):
  - Noise Gate: turns the sound down while it is quieter than the threshold, so the pauses are silent (threshold, attack, release, depth).
  - Denoiser: takes a steady noise (hiss, hum, a fan) out of the sound. Switch "Learn noise" on while only the noise plays, then off: it has learned the level of the noise at every frequency and turns each frequency down by how much noise it holds (reduction, sensitivity, smoothing). The learned noise is saved with the project. Its delay of 16 ms is compensated.

## 0.2.14

- VST3 plugins: effects and instruments in the VST3 format work like CLAP ones (FX button, instrument plugins, their own windows on Windows, sliders otherwise, saved state, delay compensation). The program finds them in the usual VST3 folders (Common Files\VST3 and the user's Programs\Common\VST3 on Windows, ~/.vst3 and /usr/lib/vst3 on Linux); menus mark them [VST3].
- The VST3 interfaces (MIT license since VST 3.8) are downloaded while configuring, like the CLAP headers.

## 0.2.13

- Delay compensation: plugins that delay the sound (they say so through the CLAP latency extension, for example look-ahead limiters and linear-phase equalizers) no longer put their track late. Such a track reads its regions that much ahead, so all tracks stay together and nothing else gets later. When a plugin changes its latency, its track is compensated anew.

## 0.2.12

- Instrument plugins: a CLAP synthesizer can play the note regions of a track. Choose it with "Instrument plugins" in the menu of a note region, or add a track with "New instrument plugin track" (right-click the track area). Its window opens at once; the FX menu shows it above the effects.
- The notes reach the plugin where the playhead passes them, to the frame. A seek, a loop or playing backwards ends the notes that were sounding, and so does stopping.
- Keys played live (MIDI keyboard, piano keys) go to the instrument plugin of the track as well, also while playback is stopped.
- The instrument plugin and its state are saved inside the project.
- A fourth example plugin: Simple Synth, a small synthesizer (attack, release, brightness, volume).

## 0.2.11

- Plugins (stage 7 begins): every track has an FX button with a chain of CLAP effects. The sound passes them from top to bottom before the fader; they run also after the regions end, so echoes ring out. Effects can be switched off, moved up and down and removed; each change is one undo step.
- The program looks for CLAP plugins in "plugins" next to itself and in the usual CLAP folders (and CLAP_PATH); any .clap file can also be loaded by hand.
- "Open" shows the plugin's own window, or sliders for its parameters when it has none.
- Effects are saved inside the project with their state (the settings of the plugin).
- Three example plugins come with the program, written for it under the MIT license: Echo, Tremolo and Gain (ToneOfHell-Examples.clap).
- Instrument plugins (that play notes) come next.

## 0.2.10

- Sampler: any sound file (WAV, MP3, FLAC) can be the instrument of a track. Every note plays the sound higher or lower, like a tape. Add a track with "New sampler track (any sound)..." (right-click the track area), or choose Sampler in the menu of a note region.
- The note of the sound is found by itself from its spectrum: the loudest frequency of its loudest part, or the fundamental below it when that frequency is an overtone. The Sampler menu shows the spectrum (quiet blue, loud red), the note found, and lets you set the note and fine tuning yourself.
- Sampler sounds are saved inside the project, and they play live notes from keyboards too.
- Fixed: SoundFont instruments with a long release (strings, pads, choirs) rang on after the key was released as if the sustain pedal were held. The release now falls by 100 dB over its time, as the SoundFont 2 standard says, instead of in a straight line.

## 0.2.9

- Note (MIDI) regions can be made longer: drag either edge of the region further out than its notes went, or use + bar and - bar in the piano roll. The notes stay where they are.
- Scrolling by itself: while a part of the tracks is selected, or notes are dragged or selected with a box in the piano roll, the view moves on when the mouse comes near or beyond its edge, faster the further out.

## 0.2.8

- Piano roll: dragging on an empty place draws a box that selects the notes it touches; with Ctrl they are added to the selection. The selected notes then move together.

## 0.2.7

- Piano roll: a single click moves and selects notes (on an empty place it clears the selection); a double-click adds a note, and dragging right after it sets its length.

## 0.2.6

- Regions have a name strip on top: drag it to move the region, as before also onto another track.
- Dragging inside regions selects a part of the track, or of several tracks when the mouse goes up or down. It sticks to the grid, region edges and the playhead nearby (Alt: free).
- Piano roll: a strip with bar numbers on top places the playhead (click or drag). A note sounds while it is clicked or dragged, again at every new pitch; the keys on the left sound when clicked. The mouse wheel only zooms (around the mouse); the scrollbars move.
- Cut (Ctrl+X), Copy (Ctrl+C), Paste at the playhead (Ctrl+V), Keep only the selection (Ctrl+E) and Delete for the selected part, in the Edit menu and the region menu. Without a selected part, Cut and Copy take the selected region. Each is one undo step.

## 0.2.5

- MIDI keyboards: every connected MIDI keyboard plays notes right away (Windows MIDI inputs, Linux raw MIDI devices). Edit > Settings > General lists them.
- Piano keys on the toolbar (Ctrl+K): the letter keys play notes, A W S E D F T G Y H U J K for C to C, Z and X change the octave. Single-letter shortcuts pause meanwhile.
- Notes sound with the instrument of the armed instrument track, else a selected one, else the first one; they sound also while playback is stopped.
- Recording notes: arm an instrument track (R) and press Record. What you play becomes a new note region when you stop.

## 0.2.4

- MIDI files: File > Import a MIDI file... (or drop a .mid onto the window). Every channel of every track becomes an instrument track with its General MIDI sound (channel 10 plays the drums), and the project takes the tempo and time signature of the file; tempo changes inside the file are followed.
- File > Export audio > MIDI file (the notes) writes the notes of all note regions, one track per track, at the tempo of the project. The simple synthesizers become the closest General MIDI sounds.

## 0.2.3

- Recorded instruments: instrument tracks can now be played by real recorded sounds from a SoundFont (.sf2). The program comes with FluidR3 GM by Frank Wen (MIT license): the 128 General MIDI instruments (pianos, organs, guitars, strings, brass, synthesizers ...) and a drum kit. It is downloaded while building and lies in "sounds" next to the program.
- New instrument track and the Instrument menu of a note region list the sounds by family, plus Drums and the simple synthesizers of 0.2.1. Projects and exports keep the chosen sound.
- Edit > Settings > General > Instrument sounds chooses another SoundFont.

## 0.2.2

- A piano roll for note regions: right-click a note region > Open the piano roll, or select it and press P. It shows a keyboard from C-1 to G9 and the grid of the project.
  - Click: add a note (it gets the length used last); drag right away to make it longer.
  - Drag a note to move it in time and pitch, drag its right end to change its length; the selected notes move together. Ctrl+click selects several, Ctrl+A all.
  - Right-click a note or press Delete to remove it. Alt while dragging ignores the grid. Ctrl+wheel zooms.
  - Notes outside the region are shaded: they are kept but not heard. The playhead is shown, and every change can be undone.

## 0.2.1

The first step of level 1 (MIDI and instruments):

- Instrument tracks: right-click the track area > New instrument track, then Keys, Organ, Pad or Pluck, four simple built-in synthesizers of our own. Such a track holds a note region (purple, with a bar for every note).
- Double-click in a note region adds a note there (one beat long, on the grid, the height chooses the pitch from C3 to C6); double-clicking a note removes it. A proper piano roll comes next.
- The instrument of a track can be changed from the right-click menu of its region. Note regions can be moved, trimmed, cut, joined, undone, exported and saved in projects like audio regions; they keep normal speed.

## 0.2.0

Level 0 (the core) is complete: every stage from the audio engine to projects and undo has been checked on a real computer. New since 0.1.0:

- **Undo and redo** (Ctrl+Z, Ctrl+Y): every edit of tracks and regions, a whole drag as one step, up to 200 steps.
- **Autosave** with recovery after a crash: unsaved changes go to a recovery file every few minutes, and the next start offers to restore them.
- **Playback speed** from 25 % to 400 % (right-click Play, Ctrl+Up, Ctrl+Down, Ctrl+0), and **a speed of its own for every region**, like a tape.
- **Curved fades** by default, with a choice of curve, straight line or S-curve per region.
- **Endless metronome**: with the metronome on, playback goes on past the end of the project.
- **Track pictures** are saved inside the project, and a PNG or BMP dropped onto a track becomes its picture.

The detailed list follows below (0.1.1 to 0.1.7).

## 0.1.7

- Autosave: every 3 minutes (1 to 30, Edit > Settings > General > Autosave), unsaved changes go to a recovery file next to the settings. Your project file changes only when you save. If the program closes unexpectedly, the next start offers to restore the project; Ctrl+S then saves it to its own file again. Closing normally or saving removes the recovery file.

## 0.1.6

- Every region has a speed of its own (right-click a region > Region speed, 25 % to 400 %), also parts of one recording. Like a tape, the region gets shorter or longer and the pitch follows; its name shows the speed. Cutting, trimming, joining, projects and undo keep it. The playback speed of the whole project stays separate.

## 0.1.5

- Fades are curved by default: the level rises slowly, then faster (and a fade-out falls fast, then slowly), which sounds even to the ear. The right-click menu of a region chooses the shape: curve, straight line or S-curve. The waveform shows the curve, and projects keep the shape.

## 0.1.4

- Playback speed from 25 % to 400 %: right-click the Play button, or Playback > Speed in the right-click menus of the tracks and regions, or Transport > Speed. Ctrl+Up and Ctrl+Down change it by 5 %, Ctrl+0 returns to normal. Like a tape, the pitch changes with the speed. A button next to Play shows a speed that is not normal; a click on it returns to normal. Recording and export always run at the normal speed.

## 0.1.3

- Pictures of tracks that come from files are saved inside the project, so they survive moving or deleting the file and opening the project on another computer.
- A PNG or BMP file dropped onto a track becomes its picture (before, it was taken for an audio file and failed to load). Other picture formats get a clear message.
- When a picture cannot be shown, the reason appears at the bottom of the window.

## 0.1.2

- Undo (Ctrl+Z) and redo (Ctrl+Y), also in the Edit menu, which names the step ("Undo: Remove a track"). Every edit of tracks and regions is a step, a whole drag is one step, and up to 200 steps are kept. Mute, solo, the selection, the loop and the metronome are not steps. Opening a project or starting a new one clears the history.

## 0.1.1

- With the metronome on, playback goes on past the end of the project until you stop it (Edit > Settings > General > Metronome; on by default). A loop still loops.

## 0.1.0

The first pre-release. Everything since 0.0.1 in short: playback, recording, a multitrack mixer with regions, fades, splitting, joining and moving between tracks, a metronome and grid, reverse playback, WAV and MP3 export, projects, and an interface in English and Russian.

New in this version:

- A project is one file: "*.tohp" is a zip archive with the description of the project and all its audio, compressed. Every project gets a folder of its own (".../Song/Song.tohp").
- Closing the program, starting a new project and opening one ask whether to save unsaved changes. The window title shows a star while there are any.
- Temporary files are removed as soon as nothing uses them (for example a removed recording), and the rest when the program closes.
- A log file for every session (Edit > Settings > General > Logs, on by default; Help > Open the logs folder). It records the version, the audio device, the errors you saw, late disk reads, lost recording input and crashes. The 20 newest logs are kept.
- Scissors next to the magnet (key C): while they are on, a click on a region cuts it where the mouse is, also at a nearby edge or the playhead when the magnet is on.

## 0.0.18

- Projects can be saved and opened: File > New project (Ctrl+N), Open a project (Ctrl+Shift+O), Save the project (Ctrl+S) and Save the project as (Ctrl+Shift+S). The first Ctrl+S asks where to save.
- A project is a text file ("*.tohp") with a folder next to it ("<name> audio") that holds its audio. Saving again writes only new audio and removes audio that is no longer used.
- The dialogs start in "Documents/Tone of Hell"; Edit > Settings > General > Projects chooses another folder.
- The window title shows the name of the project.

## 0.0.17

- The window can open maximized or in full screen: Edit > Settings > General > Window.
- F11 (and View > Full screen) switches full screen on and off.

## 0.0.16

- New option "The Reverse button starts playback" (Edit > Settings > General). Switched off, the button only changes the direction, so reverse can be turned off while stopped without starting playback.
- New option "Switch reverse off at the start of the timeline" (on by default): when backwards playback reaches the start and stops, reverse turns itself off.

## 0.0.15

- Regions can be dragged onto other tracks: drag a region up or down over another lane. It keeps its place in time (and still sticks to edges with the magnet).

## 0.0.14

- Audio is played from disk instead of being held in memory. Imported files, recordings and joined regions are written to a cache folder and read in pages of 1 MB: at most six pages per file are in memory, loaded by a separate thread ahead of the playhead (backwards too, and at the loop start). Exports read the disk directly.
- Imported files no longer have a 30-minute limit.
- Until projects can be saved, the cache folder lies in the system's folder for temporary files ("Tone of Hell/session-..."). It is removed when the program closes; folders left behind after a crash are removed a week later.

## 0.0.13

- Regions no longer jump to the grid: they follow the mouse smoothly. The grid is only drawn.
- Sticking to edges is steadier: an edge that sticks holds on until the mouse is clearly away, the moving playhead no longer attracts during playback, and the reach is smaller.
- A magnet button on the toolbar (and View > Magnet) switches sticking on and off; the choice is remembered. Alt still switches it off while dragging.

## 0.0.12

- Tracks can be selected: click a track name or the empty part of its header (Ctrl or Shift adds more; Escape clears). A selected track has a blue header.
- New option "Play only the selected tracks" (Edit > Settings > General, and the Transport menu): when it is on and some tracks are selected, only those are heard, in playback and in exports. With nothing selected every track plays. Tracks that are not heard are drawn faded.

## 0.0.11

- Regions can be glued into one: "Join with the next region" (J) and "Join all regions of the track", in the Edit menu and in the right-click menu of a region. Overlaps are mixed, gaps become silence, and the outer fades stay adjustable.
- Dragged regions and edges stick to the edges of other regions (on every track), to the playhead and to the start of the timeline; a yellow line shows where. Edges come before the grid, and Alt switches both off.

## 0.0.10

- Tracks can be renamed: double-click the name of a track.
- Every track has a picture on the left of its controls. Click it to choose one of ten built-in pictures (voice, guitar, bass, drums, keys, synth, strings, brass, sample, other) or your own PNG or BMP image.

## 0.0.9

- Empty tracks for recording: right-click empty space in the track area (below the tracks, an empty part of a lane or of a track header) and choose "New track" or "New track for recording", which also arms it. The same is in the Edit menu, Ctrl+T adds a track, and an empty project shows a button for it.
- An empty track says how to record onto it.

## 0.0.8

- While recording, the take shows its waveform as it grows, so you can see what is being recorded. Parts that reach full scale are drawn red, and the take is marked "Input overloaded".
- An input level meter appears next to the Record button while the input device is open.

## 0.0.7

- A double-click on a slider returns it to its normal value: track volume and master volume to 100%, pan to the centre, input sensitivity to 100%, metronome click volume to 50%.

## 0.0.6

- Export to MP3: File > Export audio now also offers MP3 at 320 kbps, 192 kbps or variable bit rate (best quality). The files carry a header with their exact length.
- MP3 encoding uses LAME 4.0, built as a separate library (LGPL) that is downloaded while the build is configured. The project's own code stays under the MIT license.
- CMake 3.21 or newer is now needed.

## 0.0.5

- A settings window (Edit > Settings) with three tabs:
  - General: the interface language, English or Russian.
  - Audio: choice of the output and input devices, output volume, input sensitivity and an input level meter.
  - Shortcuts: every keyboard shortcut can be changed or removed, and all can be reset to the defaults. Menus show the current shortcuts.
- Settings are remembered between runs in a settings file in the user's application data folder.
- New default shortcuts: L for loop, K for the metronome, F to show the whole project.

## 0.0.4

- The version number now changes with every change, and this changelog records what each version brought.

## 0.0.3 and earlier

Development before the changelog was started:

- **Recording and export (stage 4).** Recording from the input device onto a track with a latency correction, input monitoring, and export of the mix to WAV (16-bit, 24-bit, 32-bit float).
- **Menus and navigation.** A menu bar, a status bar with project, audio and build figures, a timeline scrollbar, mouse-wheel zoom, automatic scrolling while dragging, and an option to switch the grid off.
- **Timeline and regions (stage 3).** Regions that can be moved, trimmed, cut and faded, snapping to bars and beats, zoom and scroll, a ruler with bars and time.
- **The ImGui interface.** A window with tracks, waveforms, transport and mixer controls; reverse playback; a metronome with tempo, time signature and tap tempo.
- **Tracks and mixer (stage 2).** Up to 64 tracks with fader, pan, mute and solo, 8 buses, a master fader and level meters.
- **Audio engine and player (stage 1).** A real-time safe engine with play, pause, stop, seek and loop, audio output, decoding of WAV, FLAC and MP3 files, and a command-line test player.
