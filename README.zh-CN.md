# SpektrafilmWinApp

[English](README.md) · **简体中文**

SpektrafilmWinApp 是一款 Windows 数字暗房，支持 RAW、TIFF、JPEG 和 PNG 照片。它用光谱模型模拟光线通过胶片和相纸的过程，重现不同胶片与相纸组合的成像效果。

本项目从 [SpektraLab](https://github.com/JamesQiu2005/SpektraLab) 分支而来，现已独立开发和维护。Windows 编辑器的开发和版本发布都在本仓库进行。

![在 Windows 编辑器中以 Kodak Portra 400 显影的码头夜景](screenshots/windows-darkroom-zh.jpg)

*码头夜景，原片 `7IV05987.ARW`，摄影：wuye9036。截图使用 Kodak Portra 400 胶片和 Kodak Professional Supra Endura 相纸，胶片曝光 −2 EV，已开启相纸 EDR。照片及此显影版本采用 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) 授权。*

## 主要特性

- **全新设计的高光、阴影与对比度算法。** 调整成片的高光和阴影时，算法根据局部亮度判断调整幅度，保留亮部和暗部的纹理。对比度曲线固定黑点、中灰和纸白，增强反差时，两端的细微层次依然可辨。
- **从显影到预览的全 GPU 加速管线。** Vulkan 在 GPU 上完成胶片和相纸的光谱模拟，并将渲染结果直接共享给 Direct3D 11。成片调色、直方图分析和显示色彩转换都在 GPU 上完成。
- **冲印前可独立调节高光与阴影。** 在模拟胶片曝光之前，场景中的亮部和暗部可以分别提亮或压暗最多两档。例如，夜景可以压低灯光亮度，也可以提亮背光的前景，同时保留各区域内部的纹理。
- **多语言界面。** 编辑控件、设置和软件内说明均支持简体中文、英语、日语和西班牙语。

编辑器支持彩色负片、反转片和黑白胶片，颗粒与光晕随画幅大小变化。暗房工具包括 RAW 白平衡预览、镜头校正、片边、日期后背和半格拼片。已标定的相纸可通过放印 EDR 呈现更多高光层次。ProPhoto RGB 工作流程支持按显示器进行色彩管理、软打样和批量导出。

## Windows 版本

SpektrafilmWinApp 从 **v1.0** 开始独立编号，提供 x64 安装程序和便携 ZIP，需要 64 位 Windows 和支持 Vulkan 的显卡驱动。[版本发布](https://github.com/wuye9036/SpektraLab/releases) · [Windows 构建指南](windows_UI/README.md#build-msvc-2022)

## 致谢与许可

应用和引擎采用 [GPL-3.0-or-later](license/LICENSE)。上游应用和原生引擎由 Hanze Qiu 与 SpektraLab 贡献者开发。胶片与放印模型、28 份彩色配置文件以及放印 LUT 来自 Andrea Volpato 的 [spektrafilm](https://github.com/andreavolpato/spektrafilm)。这些彩色配置文件与 LUT 保留 [CC BY-SA 4.0 许可](license/SPEKTRAFILM_LICENSE.txt)。

Hanze Qiu 制作的四款黑白胶片和 Multigrade IV RC 相纸配置文件另有[数据来源说明](engine/resources_product/NOTICE.md)。Qt、LibRaw、字体等随附组件的声明可在应用的「关于」窗口或 [Windows 第三方许可清单](windows_UI/THIRD_PARTY.md) 中查看。
