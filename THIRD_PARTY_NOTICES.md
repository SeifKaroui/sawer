# Third-party notices

## SDL

Sawer statically links SDL 3.4.10.

Copyright (C) 1997-2026 Sam Lantinga and contributors.
SDL is distributed under the zlib License.

- Source: https://github.com/libsdl-org/SDL/releases/tag/release-3.4.10
- License: https://github.com/libsdl-org/SDL/blob/release-3.4.10/LICENSE.txt

## SDL_ttf

Sawer statically links SDL_ttf 3.2.2.

Copyright (C) 1997-2026 Sam Lantinga and contributors.
SDL_ttf is distributed under the zlib License.

- Source: https://github.com/libsdl-org/SDL_ttf/releases/tag/release-3.2.2
- License: https://github.com/libsdl-org/SDL_ttf/blob/release-3.2.2/LICENSE.txt

## FreeType

Sawer statically links the SDL-maintained FreeType 2.13.2 branch used by
SDL_ttf. FreeType is distributed under the FreeType License or GPLv2; Sawer
uses it under the FreeType License.

- Source: https://github.com/libsdl-org/freetype/tree/VER-2-13-2-SDL
- License: https://github.com/libsdl-org/freetype/blob/VER-2-13-2-SDL/docs/FTL.TXT

## JSON for Modern C++

Sawer uses nlohmann/json 3.12.0 for its board and preference JSON.

Copyright (c) 2013-2025 Niels Lohmann.
JSON for Modern C++ is distributed under the MIT License.

- Source: https://github.com/nlohmann/json/releases/tag/v3.12.0
- License: https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT

## Source Sans 3

Sawer embeds the Regular, Medium, and Semibold weights of Source Sans 3
version 3.052. Source Sans 3 was created by Adobe and is distributed under
the SIL Open Font License 1.1.

- Source: https://github.com/adobe-fonts/source-sans/tree/3.052R
- License: https://github.com/adobe-fonts/source-sans/blob/3.052R/LICENSE.md

## Lucide Icons

Sawer's native icon geometry follows the Lucide 24-pixel design grid and
adapts Lucide icon forms for the custom triangle renderer.

Copyright (c) 2026 Lucide Icons and Contributors.
Lucide is distributed under the ISC License.

- Source: https://github.com/lucide-icons/lucide/tree/1.26.0
- License: https://lucide.dev/license

## Catch2

Sawer's test executables use Catch2 3.15.0. Catch2 is not linked into the
distributed Sawer application.

Copyright (c) 2017 Two Blue Cubes Ltd.
Catch2 is distributed under the Boost Software License 1.0.

- Source: https://github.com/catchorg/Catch2/releases/tag/v3.15.0
- License: https://github.com/catchorg/Catch2/blob/v3.15.0/LICENSE.txt

## stb_image and stb_image_write

Sawer vendors the single-header image decoder and PNG writer from stb commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20` for bounded clipboard decoding
and canonical PNG encoding.

stb is dual-licensed under the MIT License and the public domain.

- Source: https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20
- License: `subprojects/stb/LICENSE`
