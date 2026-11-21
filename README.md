# SpektrafilmWinApp

**English** · [简体中文](README.zh-CN.md)

SpektrafilmWinApp is a digital darkroom for Windows. Its spectral model follows light through film and photographic paper to simulate the development and printing of RAW, TIFF, JPEG and PNG photographs.

This is an independently developed and maintained fork of [SpektraLab](https://github.com/JamesQiu2005/SpektraLab). The Windows editor and its releases live in this repository.

![A night waterfront developed in the Windows editor with Kodak Portra 400](screenshots/windows-darkroom-en.jpg)

*Night waterfront from `7IV05987.ARW`, photographed by wuye9036. Developed with Kodak Portra 400 on Kodak Professional Supra Endura, film exposure −2 EV and Print EDR enabled. The photograph and its rendition are licensed under [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/).*

## Features

- **Redesigned highlights, shadows and contrast.** On the finished print, highlight and shadow adjustments use local brightness to preserve detail within bright and dark regions. The contrast curve holds black, middle grey and paper white in place, keeping fine tonal differences visible near both ends of the scale.
- **Fully GPU-accelerated rendering and preview.** Vulkan runs the spectral film and paper simulation. The Windows preview shares the rendered image directly with Direct3D 11, keeping print grading, histogram analysis and display colour conversion on the GPU.
- **Highlight and shadow adjustments before development.** Bright and dark regions of the scene can each be adjusted by up to two stops before film exposure. This makes room for bright lamps or brings out texture in a shaded foreground while preserving detail within each region.
- **Four interface languages.** English, Simplified Chinese, Japanese and Spanish cover the editing controls, settings and in-app explanations.

The editor supports colour negatives, slide film and black-and-white stocks, with grain and halation scaled to the film format. RAW white-balance previews and lens correction, film edges, date imprints and half-frame pairs round out the darkroom tools. Print EDR brings out additional highlight detail on calibrated papers. The ProPhoto RGB workflow includes per-monitor colour management, soft proofing and batch export.

## Windows release

**v1.0** starts the independent SpektrafilmWinApp release series. The x64 installer and portable ZIP require 64-bit Windows and a Vulkan-capable graphics driver. [Releases](https://github.com/wuye9036/SpektraLab/releases) · [Windows build guide](windows_UI/README.md#build-msvc-2022)

## Credits and licences

The application and engine are [GPL-3.0-or-later](license/LICENSE). Hanze Qiu and the SpektraLab contributors developed the upstream application and native engine. Andrea Volpato’s [spektrafilm](https://github.com/andreavolpato/spektrafilm) supplies the film and print model, the 28 colour profiles and the print LUTs. Those profiles and LUTs retain their [CC BY-SA 4.0 licence](license/SPEKTRAFILM_LICENSE.txt).

Hanze Qiu’s four black-and-white film profiles and Multigrade IV RC paper are documented in the [profile notice](engine/resources_product/NOTICE.md). Notices for Qt, LibRaw, fonts and other bundled components are available in the app’s About window and in the [Windows third-party notices](windows_UI/THIRD_PARTY.md).
