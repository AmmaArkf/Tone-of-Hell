# Tone of Hell

**EN:** English version below · **RU:** [Русская версия ниже](#ru-русская-версия)

An open digital audio workstation (DAW) written in C++: recording, a multitrack mixer, MIDI and hosting of third-party plugins. The source code is open to everyone, and the project is developed together with its community.

> **Status: version 0.3 (early).** The working minimum (level 1) is complete: besides recording, arranging, mixing and exporting you can play and edit notes, use CLAP and VST3 plugins, automate volume, pan and plugin parameters, change the tempo inside a song and use the built-in effects. Expect rough edges and keep copies of projects that matter. See [ROADMAP.md](ROADMAP.md) for the plan and [CHANGELOG.md](CHANGELOG.md) for every change.

## Download

Ready-made builds for Windows and Linux are on the [Releases](https://github.com/AmmaArkf/Tone-of-Hell/releases) page (open "Assets" under a release).

- **Windows:** unpack the zip and start `Tone of Hell.exe`. The program is not signed yet, so Windows SmartScreen may warn the first time ("More info" > "Run anyway").
- **Linux:** unpack the archive and start `./tone-of-hell`.

## What it can do now

- **Tracks and mixer:** volume, pan, mute, solo and level meters; names and pictures for tracks; play all tracks or only the selected ones.
- **Regions on a timeline:** move (also to other tracks), trim, cut (with the scissors or at the playhead), join, fades with a curve of your choice, a magnet that snaps edges to each other, a speed of their own for every region.
- **Recording** from any input with input gain, monitoring, latency correction and a live waveform; a take goes straight to disk.
- **Playback:** loop, reverse, a metronome with any time signature, a playback speed from 25 % to 400 %; audio is streamed from disk in 1 MB pages, so long files do not fill the memory.
- **Notes and instruments:** instrument tracks with a piano roll, 128 recorded General MIDI instruments and drums (the FluidR3 SoundFont), simple built-in synthesizers, a sampler that turns any sound file into an instrument and finds its note by itself, MIDI keyboards and the computer keyboard as a piano, recording of what you play, import and export of MIDI files.
- **Plugins:** CLAP and VST3 effects and instruments on every track, with their own windows, saved state and delay compensation.
- **Built-in effects:** Equalizer, Compressor, Limiter, Reverb, Echo, Noise Gate, Denoiser, Tremolo and Gain, and a simple synthesizer.
- **Automation and tempo:** lines on the tracks for volume, pan and any plugin parameter; tempo changes inside a song that the grid and the metronome follow.
- **Editing:** select a part of one or more tracks, cut, copy, paste, keep only the selection.
- **Import** of WAV, FLAC and MP3; **export** to WAV (16-bit, 24-bit, 32-bit float) and MP3 (CBR or VBR).
- **Projects** in one file (`.tohp`, a zip archive with all the audio and pictures), undo and redo, a question about unsaved changes, autosave with recovery after a crash.
- **Interface** in English and Russian, shortcuts that you can change, full screen, a log file for every session to help find errors.

## Goals

- A free program for recording, editing and mixing music.
- Fully open source code and open formats: MIDI, WAV, CLAP, VST3.
- Windows and Linux from the start. The code is written to be portable.
- Hosting of third-party plugins (CLAP first, then VST3).
- Extensibility: built-in effects and instruments, automation and scripting.

## What this project is not

This is an original program, not a copy of anyone else's DAW. We do not use or accept:

- code, user interface, icons, sounds, loops or presets taken from other programs;
- code or data obtained by disassembling, decompiling or reverse engineering closed programs or their file formats;
- material whose license does not allow redistribution in an open source project.

Ideas and well-known principles of audio software are fine to use; protected code and material are not. Details are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

The status of every stage is tracked in [ROADMAP.md](ROADMAP.md). In short:

| Level | What it includes | Status |
| --- | --- | --- |
| 0. Core | Audio engine, transport, tracks, regions, mixer, recording, import and export, projects, undo/redo | Done (version 0.2) |
| 1. Working minimum | MIDI and piano roll, CLAP/VST3 plugins, automation, tempo map, basic effects and instruments | Done (version 0.3) |
| 2. Advanced | Comping, time-stretch, groups, sidechain, controllers, score editor, clip grid | Next |
| 3. Distinctive | ML features, sound library, surround sound, mastering assistant | Idea |

## Building

See [docs/BUILDING.md](docs/BUILDING.md). In short, you need:

- a compiler with C++17 support or newer (MSVC, GCC, Clang);
- CMake 3.21 or newer;
- Git, and an internet connection for the first configure (it downloads SDL and LAME and checks their checksums).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

## How to help

You can help without writing any code. Pick what suits you:

- **Found a bug?** Open an issue with the "Report a bug" template in the Issues tab. Attach the log of the session (Help > Open the logs folder in the program).
- **Have an idea, or want to automate a chore?** Use the "Suggest an idea or an automation" template.
- **Want to propose a plugin, an effect, an instrument or support for a format?** Use the "Suggest a plugin or an integration" template.
- **Have a question, or want to discuss something?** Open the Discussions tab.
- **Want to write code?** Read [CONTRIBUTING.md](CONTRIBUTING.md) and look for issues labeled `good first issue`.

Before opening an issue, search for a similar one: it may already be under discussion. If you find one, add a 👍 and your own details.

## Security

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md), not in public issues.

## Community rules

Taking part in the project means agreeing to the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

The project is released under the MIT license, see [LICENSE](LICENSE). Third-party code and its licenses are listed in [third_party/README.md](third_party/README.md).

---

## RU: Русская версия

**RU:** русский перевод описания проекта. **EN:** [English version above](#tone-of-hell). Остальные документы проекта (CONTRIBUTING, ROADMAP и другие) пока доступны только на английском.

Открытая цифровая звуковая рабочая станция (DAW) на C++: запись, многодорожечный микшер, MIDI и подключение сторонних плагинов. Исходный код открыт для всех, а проект развивается вместе с сообществом.

> **Статус: версия 0.3 (ранняя).** Рабочий минимум (уровень 1) готов: кроме записи, аранжировки, сведения и экспорта можно играть и править ноты, подключать плагины CLAP и VST3, автоматизировать громкость, панораму и параметры плагинов, менять темп внутри песни и пользоваться встроенными эффектами. Возможны шероховатости, поэтому храните копии важных проектов. План в [ROADMAP.md](ROADMAP.md), все изменения в [CHANGELOG.md](CHANGELOG.md).

### Скачать

Готовые сборки для Windows и Linux лежат на странице [Releases](https://github.com/AmmaArkf/Tone-of-Hell/releases) (раздел «Assets» под релизом).

- **Windows:** распакуйте zip и запустите `Tone of Hell.exe`. Программа пока не подписана, поэтому SmartScreen при первом запуске может предупредить («Подробнее» > «Выполнить в любом случае»).
- **Linux:** распакуйте архив и запустите `./tone-of-hell`.

### Что уже умеет

- **Дорожки и микшер:** громкость, панорама, mute, solo и индикаторы уровня; названия и картинки дорожек; воспроизведение всех дорожек или только выделенных.
- **Регионы на таймлайне:** перемещение (и на другие дорожки), обрезка, разрезание (ножницами или по линии воспроизведения), склейка, фейды с выбором формы кривой, магнит для прилипания краёв, своя скорость у каждого региона.
- **Запись** с любого входа: чувствительность входа, мониторинг, коррекция задержки, волна прямо во время записи; дубль сразу пишется на диск.
- **Воспроизведение:** цикл, реверс, метроном с любым размером, скорость от 25 % до 400 %; звук читается с диска кусками по 1 МБ, поэтому длинные файлы не занимают память.
- **Ноты и инструменты:** дорожки с инструментами и пиано-ролл, 128 записанных инструментов General MIDI и ударные (SoundFont FluidR3), простые встроенные синтезаторы, сэмплер, который делает инструмент из любого звукового файла и сам находит его ноту, MIDI-клавиатуры и клавиатура компьютера как пианино, запись сыгранного, импорт и экспорт MIDI-файлов.
- **Плагины:** эффекты и инструменты CLAP и VST3 на каждой дорожке, с собственными окнами, сохранением настроек и компенсацией задержки.
- **Встроенные эффекты:** эквалайзер, компрессор, лимитер, ревербератор, эхо, шумовой гейт, шумоподавитель, тремоло и усиление, а также простой синтезатор.
- **Автоматизация и темп:** линии на дорожках для громкости, панорамы и любого параметра плагина; смены темпа внутри песни, за которыми следуют сетка и метроном.
- **Правка:** выделение части одной или нескольких дорожек, вырезать, копировать, вставить, оставить только выделенное.
- **Импорт** WAV, FLAC и MP3; **экспорт** в WAV (16 бит, 24 бита, 32 бита float) и MP3 (постоянный или переменный битрейт).
- **Проекты** в одном файле (`.tohp`, zip-архив со всем звуком и картинками), отмена и повтор действий, вопрос о несохранённых изменениях, автосохранение с восстановлением после сбоя.
- **Интерфейс** на русском и английском, настраиваемые горячие клавиши, полноэкранный режим, лог каждого сеанса для поиска ошибок.

### Цели

- Бесплатная программа для записи, редактирования и сведения музыки.
- Полностью открытый исходный код и открытые форматы: MIDI, WAV, CLAP, VST3.
- Windows и Linux с самого начала. Код пишется переносимым.
- Подключение сторонних плагинов (сначала CLAP, затем VST3).
- Расширяемость: встроенные эффекты и инструменты, автоматизация и скрипты.

### Чем этот проект не является

Это оригинальная программа, а не копия чьей-то DAW. Мы не используем и не принимаем:

- код, интерфейс, значки, звуки, лупы и пресеты, взятые из других программ;
- код или данные, полученные дизассемблированием, декомпиляцией или обратной разработкой закрытых программ или их форматов файлов;
- материалы, лицензия которых не позволяет распространять их в проекте с открытым кодом.

Идеи и общеизвестные принципы звуковых программ использовать можно, защищённый код и материалы нельзя. Подробности в [CONTRIBUTING.md](CONTRIBUTING.md).

### Возможности

Состояние каждого этапа отслеживается в [ROADMAP.md](ROADMAP.md). Коротко:

| Уровень | Что входит | Статус |
| --- | --- | --- |
| 0. Ядро | Аудиодвижок, транспорт, дорожки, регионы, микшер, запись, импорт и экспорт, проекты, отмена и повтор | Готово (версия 0.2) |
| 1. Рабочий минимум | MIDI и пиано-ролл, плагины CLAP/VST3, автоматизация, карта темпа, базовые эффекты и инструменты | Готово (версия 0.3) |
| 2. Расширенный | Компинг, растяжение времени, группы, сайдчейн, контроллеры, нотный редактор, сетка клипов | Следующий |
| 3. Особенный | Функции на основе машинного обучения, библиотека звуков, объёмный звук, помощник по мастерингу | Идея |

### Сборка

Подробно в [docs/BUILDING.md](docs/BUILDING.md) (на английском). Коротко, нужны:

- компилятор с поддержкой C++17 или новее (MSVC, GCC, Clang);
- CMake 3.21 или новее;
- Git и интернет при первой настройке (скачиваются SDL и LAME с проверкой контрольных сумм).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

### Как помочь

Помочь можно и без написания кода. Выберите, что вам подходит (названия шаблонов остаются английскими, как в самих формах):

- **Нашли ошибку?** Откройте issue по шаблону "Report a bug" на вкладке Issues. Приложите лог сеанса (в программе: Справка > Открыть папку с логами).
- **Есть идея или хотите автоматизировать рутину?** Используйте шаблон "Suggest an idea or an automation".
- **Хотите предложить плагин, эффект, инструмент или поддержку формата?** Используйте шаблон "Suggest a plugin or an integration".
- **Есть вопрос или хотите что-то обсудить?** Откройте вкладку Discussions.
- **Хотите писать код?** Прочитайте [CONTRIBUTING.md](CONTRIBUTING.md) и ищите issues с меткой `good first issue`.

Прежде чем создавать issue, поищите похожий: возможно, он уже обсуждается. Если нашли, поставьте 👍 и добавьте свои подробности.

### Безопасность

Об уязвимостях сообщайте приватно, как описано в [SECURITY.md](SECURITY.md), а не в публичных issues.

### Правила сообщества

Участие в проекте означает согласие с [Кодексом поведения](CODE_OF_CONDUCT.md).

### Лицензия

Проект выпущен под лицензией MIT, см. [LICENSE](LICENSE). Сторонний код и его лицензии перечислены в [third_party/README.md](third_party/README.md).
