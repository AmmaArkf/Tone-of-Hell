# Third-party code

These files are copied from their projects unchanged. Do not edit them: update them by replacing the files with a newer release and recording the new version and checksum here.

| Library | Version | Files | License | Source | Used for |
| --- | --- | --- | --- | --- | --- |
| miniaudio | 0.11.26 | `miniaudio/miniaudio.h` | Public domain or MIT-0 (see the end of the file) | https://github.com/mackron/miniaudio | Audio output and file decoding |
| doctest | 2.5.3 | `doctest/doctest.h` | MIT (see the top of the file) | https://github.com/doctest/doctest | Unit tests only; not part of the program |
| Dear ImGui | 1.93.0 | `imgui/` (core files and the SDL3 and SDL_Renderer3 backends) | MIT (`imgui/LICENSE.txt`) | https://github.com/ocornut/imgui | The ImGui user interface |
| miniz | 3.1.2 | `miniz/miniz.c`, `miniz/miniz.h` | MIT (`miniz/LICENSE`) | https://github.com/richgel999/miniz | Reading and writing project files (zip archives) |

## Checksums (SHA-256)

| File | SHA-256 |
| --- | --- |
| `miniaudio/miniaudio.h` | `6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e` |
| `doctest/doctest.h` | `cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421` |
| `imgui/imgui.cpp` | `9542082a1eaeffcab7b3d8069e707839ce1714d167005e9f273ac9f656b415ed` |
| `imgui/imgui.h` | `314462301948fbd5673f5ead6f9a53acf54767cbee57e186e69f8f8b022c7094` |
| `imgui/imgui_draw.cpp` | `fff3bb04ca11e32041144e83a37b89ec92f4d5c62d681eb7ef29ee6cd7663e84` |
| `imgui/imgui_internal.h` | `61bc50172d907cc931159cdce48fbff1db616cda33251c9c3469e316056939e6` |
| `imgui/imgui_tables.cpp` | `36635c1cf2ad1b7c88cdced456cceba3a1d02266d78a63d77e9cce92f4d48d61` |
| `imgui/imgui_widgets.cpp` | `d2d388f711688f6a5ce1ae3b30168c1bf4f954ead40235b0b2b9667452850c93` |
| `imgui/imconfig.h` | `fa91f606da783669ffd485f3f9e0cd0d92cddeac459efce4c2b9327cce7a3233` |
| `imgui/imstb_rectpack.h` | `889b396795202d1457560a797a7242e96f6f132d4b88ca2d69be58bf05e1771f` |
| `imgui/imstb_textedit.h` | `a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741` |
| `imgui/imstb_truetype.h` | `c51a0f7e7ea760f2366bd3752635ec58e21fccfec4a832501639990ba6ce0528` |
| `imgui/backends/imgui_impl_sdl3.cpp` | `306060dffa276b574f0541194e8e079c1685c29c1095602a3972de0bb23171e7` |
| `imgui/backends/imgui_impl_sdl3.h` | `fa184f17d59d31e986df2e827ef964d7b3c28bcba7789643a4aaaed344cb868e` |
| `imgui/backends/imgui_impl_sdlrenderer3.cpp` | `039528b1cac2b6a8e272b4f6f9d2f9301ee531e07c49249677558f62d3e009f0` |
| `imgui/backends/imgui_impl_sdlrenderer3.h` | `09b1a375f061c97cf0df6d5db9b297d165f2875f8a912e34989118a8a32ce9ec` |
| `miniz/miniz.c` | `e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30` |
| `miniz/miniz.h` | `b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f` |
| `miniz/LICENSE` | `0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3` |

miniz comes from the release archive `miniz-3.1.2.zip` (SHA-256 `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`).

The checksums were taken on Windows right after downloading. A Git checkout may convert line endings, which changes them.

## Downloaded while building

| Library | Version | License | Source | Used for |
| --- | --- | --- | --- | --- |
| SDL | 3.4.20 | zlib | https://github.com/libsdl-org/SDL | Window, input and file dialogs of the ImGui user interface |

| LAME | 4.0 | LGPL 2 or later | https://lame.sourceforge.io | MP3 export; built as a separate shared library (`mp3lame`) |

SDL is too large to keep in the repository. CMake downloads the official release archive and checks its SHA-256 (`f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f`); see `apps/toh_imgui/CMakeLists.txt`. Configure with `-DTOH_BUILD_GUI_IMGUI=OFF` to build without it.

LAME is licensed under the LGPL, so it is deliberately kept apart from our code: CMake downloads the official release archive `lame-4.0.tar.gz` and checks its SHA-256 (`3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb`), then builds only its encoder library as a separate DLL or shared object (see `cmake/Lame.cmake` and `cmake/lame/config.h`). The programs load it at start-up, and anyone may replace it with another build of LAME. When the program is distributed, the LAME license (`COPYING` in its archive) must be included next to the library.
