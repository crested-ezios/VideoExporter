# Third-Party Notices

This repository includes or interacts with the following third-party software and interfaces:

- **GakuLoader Plugin SDK** (MIT License) — `include/GakuPlugin.h`, `include/GakuPaths.h`, `include/GakuPaths.hpp`, `include/GakuWebPanel.h`
- **Service Contracts** (MIT License) — `include/GakuScreenshot.h`, `include/GakuMotionClock.h`, `include/GakuMotionStageState.h`, `include/GakuAudio.h`
- **FFmpeg** (external user-supplied CLI binary invoked out-of-process via `CreateProcessW`; not bundled or linked into `VideoExporter.dll`)
- **Frontend UI Libraries (`frontend/`)** — React, Vite, Vitest, Tailwind CSS, Radix UI primitives, Lucide Icons, and Remix Icon (see `frontend/package.json` and `frontend/package-lock.json` for exact package versions and licenses).
