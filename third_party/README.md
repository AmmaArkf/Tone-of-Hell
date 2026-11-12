# Third-party code

These files are copied from their projects unchanged. Do not edit them: update them by replacing the files with a newer release and recording the new version and checksum here.

| Library | Version | Files | License | Source | Used for |
| --- | --- | --- | --- | --- | --- |
| miniaudio | 0.11.26 | `miniaudio/miniaudio.h` | Public domain or MIT-0 (see the end of the file) | https://github.com/mackron/miniaudio | Audio output and file decoding |
| doctest | 2.5.3 | `doctest/doctest.h` | MIT (see the top of the file) | https://github.com/doctest/doctest | Unit tests only; not part of the program |
| Dear ImGui | 1.93.0 | `imgui/` (core files and the SDL3 and SDL_Renderer3 backends) | MIT (`imgui/LICENSE.txt`) | https://github.com/ocornut/imgui | The ImGui user interface |

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

The checksums were taken on Windows right after downloading. A Git checkout may convert line endings, which changes them.

## Downloaded while building

| Library | Version | License | Source | Used for |
| --- | --- | --- | --- | --- |
| SDL | 3.4.20 | zlib | https://github.com/libsdl-org/SDL | Window, input and file dialogs of the ImGui user interface |

SDL is too large to keep in the repository. CMake downloads the official release archive and checks its SHA-256 (`f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f`); see `apps/toh_imgui/CMakeLists.txt`. Configure with `-DTOH_BUILD_GUI_IMGUI=OFF` to build without it.
