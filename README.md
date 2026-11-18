# VideoExporter

GakuLoader 原生 4K 锁步视频录制与 FFmpeg 后期合成插件（`videoexporter`），通过组合 `ScreenshotManager`（`screenshotmanager.capture/v1`）、`GakuMotion`（`gakumotion.clock/v1`）、`GakuAudio`（`gakuaudio.core/v1`）与独立 `ffmpeg.exe` 管线，实现零丢帧锁步高清录像、绿幕透明通道导出与音频/滤镜后期合成。

## 核心能力

- **4 槽位背压环形缓冲区（4K 零丢帧锁步）**
  - 录制启动时按 `width × height × 4`（最高 4K `3840×2160` / `4096×2160` / 竖屏 `2160×3840`）分配 4 个帧槽位，录制结束立即释放。
  - **锁步录制模式**（完整曲目 / 起止时间区间）：通过 `acquireRecordingLease` + `recordingBegin` + `notifyFrameRendered(1)` 与 `GakuMotion` 时钟逐帧锁步推进；当环形缓冲区已满时暂停调用 `notifyFrameRendered`，确保永不丢帧。
  - **自由录制模式**：不依赖 `gakumotion.clock/v1` 租约，独立捕获每帧渲染画面。
- **FFmpeg 双阶段管线 (`FfmpegPipeline`)**
  - **Stage A（实时管道编码）**：通过 `CreateProcessW`（`CREATE_NO_WINDOW`）启动 `ffmpeg.exe` 接收 `rawvideo rgba` 标准输入流并异步读取 `stderr` 进度，支持：
    - **MP4 H.264 / HEVC**：`auto`（优先 `h264_nvenc` 或回退 `libx264`）、`h264_nvenc`、`hevc_nvenc`、`libx264`、`libx265`（支持 `yuv420p` / `yuv444p` 与 CRF `14..28`）。
    - **绿幕 Alpha 透明通道视频**：`prores_ks`（`-profile:v 4444 -pix_fmt yuva444p10le`，输出 `.mov`）与 `libvpx-vp9`（`-pix_fmt yuva420p`，输出 `.webm`）。
  - **Stage B（后期处理与音频混流）**：
    - 支持调用 `gakuaudio.core/v1::renderOfflineWav` 烘焙 `GakuAudio` 舞台空间 DSP 音效与时间轴偏移，或由 FFmpeg 过滤器（`atempo` / `adelay` / `volume` / `apad`）直接混流。
    - 支持无滤镜时无损流拷贝（`-c:v copy`）+ 音频编码（MP4 `aac 320k` / MOV `pcm_s16le` / WebM `libopus`），或应用 `sharpen` (`unsharp`)、`vivid` (`eq`)、`motion_smooth` (`tblend`)、自定义 `-vf` 滤镜与额外 FFmpeg 参数后原子重命名落地。
- **独立 WebView2 管理面板（`videoexporter.web-panel/v1`）与热键**
  - 前端工程位于 `frontend/`，构建产物部署于 `<dllDir>\VideoExporter.UI`（虚拟主机 `videoexporter.invalid`）。
  - 可选注册前景热键 `toggle-recording`（默认 `F12`）启停录制。
  - 配置文件原子写入 `<configRoot>/config.json`，默认输出目录为 `<userData or localRoot>/Videos`。

## 构建与测试

```powershell
# 一键构建原生插件 DLL、前端 VideoExporter.UI 资源并运行 C++ & 4K FFmpeg 端到端测试
powershell -NoProfile -File build.ps1 -Target All

# 仅运行离线 C++ & 真实 4K FFmpeg/ffprobe 端到端测试
powershell -NoProfile -File tests/run_tests.ps1

# 前端类型检查与 Vitest 测试
cd frontend
npm run typecheck
npm test
```

> 运行 E2E 测试需确保 `ffmpeg.exe` 与 `ffprobe.exe` 位于系统 `PATH` 中。
