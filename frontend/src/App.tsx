import * as React from 'react'
import {
  CheckCircle2,
  Film,
  FolderOpen,
  Gauge,
  HardDriveDownload,
  Loader2,
  Music2,
  PlugZap,
  RefreshCw,
  Search,
  SlidersHorizontal,
  Sparkles,
  Square,
  Video,
  Wand2,
} from 'lucide-react'
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Progress } from '@/components/ui/progress'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { Separator } from '@/components/ui/separator'
import { Slider } from '@/components/ui/slider'
import { Switch } from '@/components/ui/switch'
import { getVideoExporterBridge } from './bridge'
import {
  CODEC_OPTIONS,
  FPS_PRESETS,
  POST_FILTER_OPTIONS,
  RESOLUTION_PRESETS,
  isAlphaCodec,
  type AlphaMode,
  type PixelFormat,
  type PostFilterPreset,
  type RecorderPhase,
  type RecordingMode,
  type VideoCodec,
} from './protocol'
import { useVideoExporterStore, videoExporterStore } from './store'

const THEME_NAMES = ['dark', 'sapphire', 'paper', 'apple-light', 'apple-dark'] as const
const DARK_THEMES = new Set([0, 1, 4])

function isEditableTarget(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false
  if (target.tagName === 'TEXTAREA' || target.tagName === 'SELECT') return true
  if (target.tagName === 'INPUT') {
    const t = (target as HTMLInputElement).type
    return !['button', 'checkbox', 'radio', 'range', 'submit', 'reset', 'file', 'color'].includes(t)
  }
  return target.isContentEditable
}

function phaseLabel(phase: RecorderPhase): string {
  switch (phase) {
    case 'idle':
      return '待机就绪'
    case 'starting':
      return '正在启动'
    case 'recording':
      return '录制中'
    case 'post_processing':
      return 'FFmpeg 后期合成中'
    case 'completed':
      return '导出完成'
    case 'error':
      return '出现错误'
  }
}

function phaseBadgeVariant(phase: RecorderPhase): 'default' | 'success' | 'warning' | 'destructive' | 'muted' {
  switch (phase) {
    case 'recording':
      return 'destructive'
    case 'starting':
    case 'post_processing':
      return 'warning'
    case 'completed':
      return 'success'
    case 'error':
      return 'destructive'
    default:
      return 'muted'
  }
}

function formatDuration(sec: number): string {
  if (!Number.isFinite(sec) || sec <= 0) return '0.0s'
  if (sec < 60) return `${sec.toFixed(1)}s`
  const m = Math.floor(sec / 60)
  const s = (sec % 60).toFixed(1).padStart(4, '0')
  return `${m}:${s}`
}

export function App() {
  const connected = useVideoExporterStore((s) => s.connected)
  const connecting = useVideoExporterStore((s) => s.connecting)
  const connectionError = useVideoExporterStore((s) => s.connectionError)
  const bridgeKind = useVideoExporterStore((s) => s.bridgeKind)
  const actionNotice = useVideoExporterStore((s) => s.actionNotice)
  const actionError = useVideoExporterStore((s) => s.actionError)
  const config = useVideoExporterStore((s) => s.config)
  const status = useVideoExporterStore((s) => s.status)
  const services = useVideoExporterStore((s) => s.services)
  const appearance = useVideoExporterStore((s) => s.appearance)

  const [customWidthText, setCustomWidthText] = React.useState(String(config.width))
  const [customHeightText, setCustomHeightText] = React.useState(String(config.height))
  const [rangeStartText, setRangeStartText] = React.useState(String(config.rangeStartSeconds))
  const [rangeEndText, setRangeEndText] = React.useState(String(config.rangeEndSeconds))
  const [customVfText, setCustomVfText] = React.useState(config.customVideoFilter)
  const [extraArgsText, setExtraArgsText] = React.useState(config.extraFfmpegArgs)
  const [ffmpegPathText, setFfmpegPathText] = React.useState(config.ffmpegPathOverride)
  const [outputDirText, setOutputDirText] = React.useState(config.outputDir)

  React.useEffect(() => {
    void videoExporterStore.connect()
    return () => {
      videoExporterStore.disconnect()
    }
  }, [])

  React.useEffect(() => {
    const theme = appearance?.theme ?? 0
    const root = document.documentElement
    root.dataset.theme = THEME_NAMES[theme] ?? 'dark'
    root.classList.toggle('dark', DARK_THEMES.has(theme))
    root.style.zoom = String(appearance?.uiScale ?? 1)
  }, [appearance])

  React.useEffect(() => {
    if (!connected) return
    const client = getVideoExporterBridge()
    if (!client) return
    let active = false
    const report = (focused: boolean) => {
      if (focused === active) return
      active = focused
      void client.request('shell.setTextInputActive', { active: focused }).catch(() => {})
    }
    const onFocusIn = (e: FocusEvent) => report(isEditableTarget(e.target))
    const onFocusOut = (e: FocusEvent) => report(isEditableTarget(e.relatedTarget))
    const onBlur = () => report(false)
    document.addEventListener('focusin', onFocusIn)
    document.addEventListener('focusout', onFocusOut)
    window.addEventListener('blur', onBlur)
    return () => {
      document.removeEventListener('focusin', onFocusIn)
      document.removeEventListener('focusout', onFocusOut)
      window.removeEventListener('blur', onBlur)
      report(false)
    }
  }, [connected])

  React.useEffect(() => {
    if (bridgeKind !== 'mock') return
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'F12' && !e.ctrlKey && !e.altKey && !e.shiftKey) {
        e.preventDefault()
        if (status.recording) void videoExporterStore.stopRecording()
        else if (!status.busy) void videoExporterStore.startRecording()
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [bridgeKind, status.recording, status.busy])

  React.useEffect(() => {
    setCustomWidthText(String(config.width))
    setCustomHeightText(String(config.height))
    setRangeStartText(String(config.rangeStartSeconds))
    setRangeEndText(String(config.rangeEndSeconds))
    setCustomVfText(config.customVideoFilter)
    setExtraArgsText(config.extraFfmpegArgs)
    setFfmpegPathText(config.ffmpegPathOverride)
    setOutputDirText(config.outputDir)
  }, [config])

  const busy = status.busy
  const alphaSelected = isAlphaCodec(config.codec)
  const matchedResolutionPreset = React.useMemo(() => {
    const found = RESOLUTION_PRESETS.find((p) => p.width === config.width && p.height === config.height)
    return found ? found.id : 'custom'
  }, [config.width, config.height])

  const commitCustomResolution = () => {
    const w = Number.parseInt(customWidthText, 10)
    const h = Number.parseInt(customHeightText, 10)
    if (!Number.isFinite(w) || !Number.isFinite(h)) return
    const evenW = Math.max(64, Math.min(4096, w & ~1))
    const evenH = Math.max(64, Math.min(4096, h & ~1))
    void videoExporterStore.updateConfig({ width: evenW, height: evenH })
  }

  const commitRange = () => {
    const s = Number.parseFloat(rangeStartText)
    const e = Number.parseFloat(rangeEndText)
    if (!Number.isFinite(s) || !Number.isFinite(e) || s < 0 || e <= s) return
    void videoExporterStore.updateConfig({ rangeStartSeconds: s, rangeEndSeconds: e })
  }

  const canStartRecording =
    connected &&
    !busy &&
    services.ffmpeg.available &&
    services.screenshot.available &&
    services.screenshot.runtimeReady &&
    services.screenshot.cameraAvailable &&
    (config.recordingMode === 'freerun' ||
      (services.clock.available && services.clock.timelineDurationSeconds > 0))

  if (!connected) {
    return (
      <div className="flex h-full min-h-full w-full flex-col items-center justify-center gap-4 bg-background p-6 text-muted-foreground">
        {connectionError ? (
          <div className="flex max-w-md flex-col items-center gap-3 rounded-xl border bg-card/90 p-6 text-center shadow-sm">
            <div className="flex size-11 items-center justify-center rounded-full bg-destructive/15 text-destructive">
              <PlugZap className="size-5" />
            </div>
            <div className="space-y-1">
              <p className="text-gaku-md font-medium text-foreground">VideoExporter 原生桥接不可用</p>
              <p className="text-gaku-xs leading-relaxed text-muted-foreground">{connectionError}</p>
            </div>
            <Button type="button" variant="outline" size="sm" onClick={() => void videoExporterStore.connect()}>
              <RefreshCw className="size-4" />
              重新连接
            </Button>
          </div>
        ) : (
          <div className="flex flex-col items-center gap-2.5">
            <Loader2 className="size-7 animate-spin text-primary" />
            <p className="text-gaku-base">{connecting ? '正在连接 VideoExporter…' : '正在初始化…'}</p>
          </div>
        )}
      </div>
    )
  }

  const progressPct =
    status.targetFrames > 0
      ? Math.min(100, Math.round(status.progress * 100))
      : status.phase === 'completed'
        ? 100
        : 0

  return (
    <div className="flex h-full min-h-full w-full flex-col overflow-hidden bg-background text-foreground">
      {/* Top Header & Service Status Badges */}
      <header className="w-full shrink-0 border-b bg-card/80 px-5 py-3.5 backdrop-blur-sm">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div className="flex items-center gap-3">
            <div className="flex size-10 items-center justify-center rounded-lg bg-primary/15 text-primary">
              <Film className="size-5" />
            </div>
            <div>
              <div className="flex flex-wrap items-center gap-2">
                <h1 className="text-gaku-title font-semibold leading-tight tracking-tight">VideoExporter</h1>
                <span className="rounded bg-muted px-2 py-0.5 font-mono text-gaku-micro text-muted-foreground">
                  v0.1.0
                </span>
                <Badge variant={phaseBadgeVariant(status.phase)} data-testid="phase-badge">
                  {phaseLabel(status.phase)}
                </Badge>
              </div>
              <p className="mt-1 text-gaku-xs text-muted-foreground">
                4K 锁步视频录制与 FFmpeg 后期合成管线
              </p>
            </div>
          </div>

          <div className="flex flex-wrap items-center gap-2">
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={busy}
              onClick={() => void videoExporterStore.refreshState()}
            >
              <RefreshCw className="size-4" />
              刷新状态
            </Button>
            {status.recording ? (
              <Button
                type="button"
                variant="destructive"
                data-testid="toggle-recording-btn"
                onClick={() => void videoExporterStore.stopRecording()}
              >
                <Square className="size-4" />
                停止录制 (F12)
              </Button>
            ) : (
              <Button
                type="button"
                variant="default"
                disabled={!canStartRecording}
                data-testid="toggle-recording-btn"
                onClick={() => void videoExporterStore.startRecording()}
              >
                <Video className="size-4" />
                开始录制 (F12)
              </Button>
            )}
          </div>
        </div>

        {/* Service Status Badges Row */}
        <div className="mt-3 flex flex-wrap items-center gap-2" data-testid="service-badges">
          <Badge
            variant={
              services.screenshot.available &&
              services.screenshot.runtimeReady &&
              services.screenshot.cameraAvailable
                ? 'success'
                : services.screenshot.available
                  ? 'warning'
                  : 'destructive'
            }
          >
            ScreenshotManager:{' '}
            {services.screenshot.available
              ? services.screenshot.runtimeReady && services.screenshot.cameraAvailable
                ? `就绪 (${services.screenshot.maxDimension}px)`
                : '等待相机'
              : '未连接'}
          </Badge>

          {services.screenshot.greenScreenDetected && (
            <Badge variant="default">绿幕透明通道已探测</Badge>
          )}

          <Badge variant={services.clock.available ? 'success' : 'warning'}>
            GakuMotion 时钟:{' '}
            {services.clock.available
              ? services.clock.timelineDurationSeconds > 0
                ? `${services.clock.trackTitle || '已加载'} (${formatDuration(services.clock.timelineDurationSeconds)})`
                : '未加载曲目'
              : '未连接'}
          </Badge>

          <Badge variant={services.audio.available ? 'success' : 'muted'}>
            GakuAudio:{' '}
            {services.audio.available
              ? `已连接 (${services.audio.activePresetId || 'off'})`
              : '未连接'}
          </Badge>

          <Badge variant={services.ffmpeg.available ? 'success' : 'destructive'} title={services.ffmpeg.version}>
            FFmpeg:{' '}
            {services.ffmpeg.available
              ? services.ffmpeg.version.replace(/^ffmpeg version\s+/i, '').slice(0, 32)
              : '未找到'}
          </Badge>

          {services.ffmpeg.available && (
            <div className="flex flex-wrap items-center gap-1.5 text-gaku-micro text-muted-foreground">
              {services.ffmpeg.encoders.h264_nvenc && (
                <Badge variant={services.ffmpeg.nvencRuntimeVerified ? 'default' : 'outline'} className="text-gaku-micro">
                  NVENC
                </Badge>
              )}
              {services.ffmpeg.encoders.libx264 && (
                <Badge variant="outline" className="text-gaku-micro">
                  x264
                </Badge>
              )}
              {services.ffmpeg.encoders.libx265 && (
                <Badge variant="outline" className="text-gaku-micro">
                  x265
                </Badge>
              )}
              {services.ffmpeg.encoders.prores_ks && (
                <Badge variant="outline" className="text-gaku-micro">
                  ProRes 4444
                </Badge>
              )}
              {services.ffmpeg.encoders['libvpx-vp9'] && (
                <Badge variant="outline" className="text-gaku-micro">
                  VP9 Alpha
                </Badge>
              )}
            </div>
          )}
        </div>
      </header>

      {/* Scrollable Main Workspace */}
      <main className="min-h-0 flex-1 space-y-4 overflow-y-auto px-5 py-4">
        {(actionError || status.lastError) && (
          <Alert variant="destructive" data-testid="error-alert">
            <AlertTitle>导出或操作提示</AlertTitle>
            <AlertDescription className="break-words">{actionError || status.lastError}</AlertDescription>
          </Alert>
        )}
        {actionNotice && !actionError && (
          <Alert variant="info">
            <CheckCircle2 className="size-4" />
            <AlertDescription className="break-words">{actionNotice}</AlertDescription>
          </Alert>
        )}

        {/* Live Progress HUD */}
        <Card data-testid="progress-hud">
          <CardHeader className="pb-2.5">
            <div className="flex flex-wrap items-center justify-between gap-2">
              <CardTitle className="flex items-center gap-2 text-gaku-md">
                <Gauge className="size-4 text-primary" />
                实时录制与编码进度 HUD
              </CardTitle>
              <span className="text-gaku-xs text-muted-foreground">{status.phaseMessage || phaseLabel(status.phase)}</span>
            </div>
          </CardHeader>
          <CardContent className="space-y-3">
            <Progress value={progressPct} />
            <div className="grid grid-cols-2 gap-2.5 sm:grid-cols-5">
              <div className="rounded-md border bg-surface/60 px-3 py-2">
                <div className="text-gaku-xs text-muted-foreground">已捕获帧数</div>
                <div className="font-mono text-gaku-base font-semibold" data-testid="hud-captured">
                  {status.capturedFrames}
                  {status.targetFrames > 0 ? ` / ${status.targetFrames}` : ''}
                </div>
              </div>
              <div className="rounded-md border bg-surface/60 px-3 py-2">
                <div className="text-gaku-xs text-muted-foreground">已编码帧数</div>
                <div className="font-mono text-gaku-base font-semibold" data-testid="hud-encoded">
                  {status.encodedFrames}
                </div>
              </div>
              <div className="rounded-md border bg-surface/60 px-3 py-2">
                <div className="text-gaku-xs text-muted-foreground">吞吐帧率</div>
                <div className="font-mono text-gaku-base font-semibold" data-testid="hud-fps">
                  {status.fps.toFixed(1)} fps
                </div>
              </div>
              <div className="rounded-md border bg-surface/60 px-3 py-2">
                <div className="text-gaku-xs text-muted-foreground">已用耗时</div>
                <div className="font-mono text-gaku-base font-semibold" data-testid="hud-elapsed">
                  {formatDuration(status.elapsedSec)}
                </div>
              </div>
              <div className="rounded-md border bg-surface/60 px-3 py-2">
                <div className="text-gaku-xs text-muted-foreground">当前阶段</div>
                <div className="truncate text-gaku-base font-semibold" data-testid="hud-phase">
                  {phaseLabel(status.phase)}
                </div>
              </div>
            </div>

            {status.lastOutputPath && (
              <div className="flex flex-wrap items-center justify-between gap-2.5 rounded-md border bg-muted/40 px-3 py-2.5 text-gaku-xs">
                <div className="min-w-0 flex-1 break-all font-mono text-foreground" title={status.lastOutputPath} data-testid="hud-output-path">
                  最近导出: {status.lastOutputPath}
                </div>
                <Button
                  type="button"
                  variant="outline"
                  size="sm"
                  className="shrink-0"
                  onClick={() => void videoExporterStore.openFolder(true)}
                >
                  <FolderOpen className="size-4" />
                  定位文件
                </Button>
              </div>
            )}
          </CardContent>
        </Card>

        {/* Two-column settings grid */}
        <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
          {/* Resolution, FPS & Recording Scope */}
          <Card>
            <CardHeader className="pb-3">
              <CardTitle className="flex items-center gap-2 text-gaku-md">
                <SlidersHorizontal className="size-4 text-primary" />
                画面规格与录制范围
              </CardTitle>
              <CardDescription>最高支持 4K (3840×2160 / 竖屏 2160×3840) 与零丢帧锁步时钟</CardDescription>
            </CardHeader>
            <CardContent className="space-y-3.5">
              <div className="space-y-1.5">
                <Label htmlFor="resolution-preset">分辨率预设</Label>
                <Select
                  value={matchedResolutionPreset}
                  disabled={busy}
                  onValueChange={(val) => {
                    const preset = RESOLUTION_PRESETS.find((p) => p.id === val)
                    if (preset) {
                      void videoExporterStore.updateConfig({ width: preset.width, height: preset.height })
                    }
                  }}
                >
                  <SelectTrigger id="resolution-preset" aria-label="分辨率预设">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {RESOLUTION_PRESETS.map((p) => (
                      <SelectItem key={p.id} value={p.id}>
                        {p.label}
                      </SelectItem>
                    ))}
                    <SelectItem value="custom">自定义分辨率 ({config.width}×{config.height})</SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="grid grid-cols-2 gap-2.5">
                <div className="space-y-1.5">
                  <Label htmlFor="custom-width">宽度 (px)</Label>
                  <Input
                    id="custom-width"
                    type="number"
                    min={64}
                    max={4096}
                    step={2}
                    disabled={busy}
                    className="font-mono"
                    value={customWidthText}
                    onChange={(e) => setCustomWidthText(e.target.value)}
                    onBlur={commitCustomResolution}
                    onKeyDown={(e) => e.key === 'Enter' && commitCustomResolution()}
                  />
                </div>
                <div className="space-y-1.5">
                  <Label htmlFor="custom-height">高度 (px)</Label>
                  <Input
                    id="custom-height"
                    type="number"
                    min={64}
                    max={4096}
                    step={2}
                    disabled={busy}
                    className="font-mono"
                    value={customHeightText}
                    onChange={(e) => setCustomHeightText(e.target.value)}
                    onBlur={commitCustomResolution}
                    onKeyDown={(e) => e.key === 'Enter' && commitCustomResolution()}
                  />
                </div>
              </div>

              <div className="space-y-1.5">
                <Label>输出帧率 (FPS)</Label>
                <div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
                  {FPS_PRESETS.map((fps) => (
                    <Button
                      key={fps}
                      type="button"
                      size="sm"
                      disabled={busy}
                      variant={config.fps === fps ? 'default' : 'outline'}
                      onClick={() => void videoExporterStore.updateConfig({ fps })}
                    >
                      {fps} FPS
                    </Button>
                  ))}
                </div>
              </div>

              <Separator />

              <div className="space-y-1.5">
                <Label htmlFor="recording-mode">录制范围与同步模式</Label>
                <Select
                  value={config.recordingMode}
                  disabled={busy}
                  onValueChange={(val) =>
                    void videoExporterStore.updateConfig({ recordingMode: val as RecordingMode })
                  }
                >
                  <SelectTrigger id="recording-mode" aria-label="录制范围与同步模式">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="lockstep_full">完整曲目锁步录制 (GakuMotion 供录制)</SelectItem>
                    <SelectItem value="lockstep_range">区间录制 (开始-结束秒)</SelectItem>
                    <SelectItem value="freerun">自由录制 (无需曲目时钟)</SelectItem>
                  </SelectContent>
                </Select>
              </div>

              {config.recordingMode === 'lockstep_range' && (
                <div className="grid grid-cols-2 gap-2.5 rounded-md border bg-muted/30 p-3">
                  <div className="space-y-1.5">
                    <Label htmlFor="range-start">开始时间 (秒)</Label>
                    <Input
                      id="range-start"
                      type="number"
                      min={0}
                      step={0.1}
                      disabled={busy}
                      className="font-mono"
                      value={rangeStartText}
                      onChange={(e) => setRangeStartText(e.target.value)}
                      onBlur={commitRange}
                      onKeyDown={(e) => e.key === 'Enter' && commitRange()}
                    />
                  </div>
                  <div className="space-y-1.5">
                    <Label htmlFor="range-end">结束时间 (秒)</Label>
                    <Input
                      id="range-end"
                      type="number"
                      min={0.1}
                      step={0.1}
                      disabled={busy}
                      className="font-mono"
                      value={rangeEndText}
                      onChange={(e) => setRangeEndText(e.target.value)}
                      onBlur={commitRange}
                      onKeyDown={(e) => e.key === 'Enter' && commitRange()}
                    />
                  </div>
                </div>
              )}
            </CardContent>
          </Card>

          {/* Video Codec, Alpha & Quality */}
          <Card>
            <CardHeader className="pb-3">
              <CardTitle className="flex items-center gap-2 text-gaku-md">
                <Sparkles className="size-4 text-primary" />
                视频编码与绿幕 Alpha 透明
              </CardTitle>
              <CardDescription>支持 MP4 H.264/HEVC 与 ProRes 4444 / VP9 透明通道导出</CardDescription>
            </CardHeader>
            <CardContent className="space-y-3.5">
              <div className="space-y-1.5">
                <Label htmlFor="video-codec">输出格式与编码器</Label>
                <Select
                  value={config.codec}
                  disabled={busy}
                  onValueChange={(val) =>
                    void videoExporterStore.updateConfig({ codec: val as VideoCodec })
                  }
                >
                  <SelectTrigger id="video-codec" aria-label="输出格式与编码器">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {CODEC_OPTIONS.map((opt) => (
                      <SelectItem key={opt.value} value={opt.value}>
                        {opt.label}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>

              {alphaSelected ? (
                <div className="space-y-2 rounded-md border border-primary/30 bg-primary/5 p-3">
                  <Label htmlFor="alpha-mode">透明通道采集模式</Label>
                  <Select
                    value={config.alphaMode}
                    disabled={busy}
                    onValueChange={(val) =>
                      void videoExporterStore.updateConfig({ alphaMode: val as AlphaMode })
                    }
                  >
                    <SelectTrigger id="alpha-mode" aria-label="透明通道采集模式">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="auto">自动绿幕透明 (推荐)</SelectItem>
                      <SelectItem value="force_transparent">强制透明通道采集</SelectItem>
                      <SelectItem value="opaque">关闭透明 (仅不透明像素)</SelectItem>
                    </SelectContent>
                  </Select>
                  <p className="text-gaku-xs leading-relaxed text-muted-foreground">
                    {config.codec === 'prores_ks'
                      ? '将使用 ProRes 4444 (yuva444p10le) 输出 .mov 透明视频，适合剪辑软件直接叠加。'
                      : '将使用 VP9 (yuva420p) 输出 .webm 透明视频，体积轻量。'}
                  </p>
                </div>
              ) : (
                <div className="space-y-1.5">
                  <Label htmlFor="pixel-format">色度采样格式</Label>
                  <Select
                    value={config.pixelFormat}
                    disabled={busy}
                    onValueChange={(val) =>
                      void videoExporterStore.updateConfig({ pixelFormat: val as PixelFormat })
                    }
                  >
                    <SelectTrigger id="pixel-format" aria-label="色度采样格式">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="yuv420p">yuv420p (通用兼容，播放器支持最好)</SelectItem>
                      <SelectItem value="yuv444p">yuv444p (全色度高保真)</SelectItem>
                    </SelectContent>
                  </Select>
                </div>
              )}

              <div className="space-y-2">
                <div className="flex items-center justify-between">
                  <Label>画质系数 (CRF / CQ)</Label>
                  <span className="font-mono text-gaku-sm text-muted-foreground">
                    {config.crf} ({config.crf <= 16 ? '极高画质' : config.crf <= 20 ? '推荐均衡' : '高压缩'})
                  </span>
                </div>
                <div className="flex h-gaku-control items-center">
                  <Slider
                    aria-label="画质系数 CRF"
                    min={14}
                    max={28}
                    step={1}
                    disabled={busy}
                    value={[config.crf]}
                    onValueCommit={(vals) => {
                      if (vals[0] !== undefined) {
                        void videoExporterStore.updateConfig({ crf: vals[0] })
                      }
                    }}
                  />
                </div>
                <div className="flex justify-between text-gaku-xs text-muted-foreground">
                  <span>14 (无损感)</span>
                  <span>21</span>
                  <span>28 (轻量)</span>
                </div>
              </div>
            </CardContent>
          </Card>
        </div>

        {/* FFmpeg Post-Processing, Audio & Paths */}
        <Card>
          <CardHeader className="pb-3">
            <CardTitle className="flex items-center gap-2 text-gaku-md">
              <Wand2 className="size-4 text-primary" />
              FFmpeg 后期处理、音频混流与目录设置
            </CardTitle>
            <CardDescription>Stage B 自动混入 GakuAudio 舞台音效并执行可选视频滤镜</CardDescription>
          </CardHeader>
          <CardContent className="space-y-3.5">
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <div className="flex items-center justify-between gap-3 rounded-md border bg-surface/50 px-3.5 py-2.5">
                <div className="space-y-0.5">
                  <div className="flex items-center gap-1.5 text-gaku-base font-medium">
                    <Music2 className="size-4 text-primary" />
                    混流曲目音频
                  </div>
                  <p className="text-gaku-xs leading-relaxed text-muted-foreground">自动从 GakuMotion 时钟读取音频轨与延迟偏移</p>
                </div>
                <Switch
                  aria-label="混流曲目音频"
                  checked={config.muxAudio}
                  disabled={busy}
                  onCheckedChange={(checked) => void videoExporterStore.updateConfig({ muxAudio: checked })}
                />
              </div>

              <div className="flex items-center justify-between gap-3 rounded-md border bg-surface/50 px-3.5 py-2.5">
                <div className="space-y-0.5">
                  <div className="text-gaku-base font-medium">烘焙 GakuAudio 舞台音效</div>
                  <p className="text-gaku-xs leading-relaxed text-muted-foreground">导出 48kHz WAV 离线混响/空间 DSP 预设</p>
                </div>
                <Switch
                  aria-label="烘焙 GakuAudio 舞台音效"
                  checked={config.bakeAudioEffects}
                  disabled={busy || !config.muxAudio}
                  onCheckedChange={(checked) =>
                    void videoExporterStore.updateConfig({ bakeAudioEffects: checked })
                  }
                />
              </div>
            </div>

            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <div className="space-y-1.5">
                <Label htmlFor="post-filter-preset">后期视频滤镜预设</Label>
                <Select
                  value={config.postFilterPreset}
                  disabled={busy}
                  onValueChange={(val) =>
                    void videoExporterStore.updateConfig({ postFilterPreset: val as PostFilterPreset })
                  }
                >
                  <SelectTrigger id="post-filter-preset" aria-label="后期视频滤镜预设">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {POST_FILTER_OPTIONS.map((opt) => (
                      <SelectItem key={opt.value} value={opt.value}>
                        {opt.label}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-1.5">
                <Label htmlFor="extra-ffmpeg-args">额外 FFmpeg 参数 (Stage B)</Label>
                <Input
                  id="extra-ffmpeg-args"
                  placeholder="例如: -metadata comment=Gakumas"
                  disabled={busy}
                  className="font-mono"
                  value={extraArgsText}
                  onChange={(e) => setExtraArgsText(e.target.value)}
                  onBlur={() =>
                    extraArgsText !== config.extraFfmpegArgs &&
                    void videoExporterStore.updateConfig({ extraFfmpegArgs: extraArgsText })
                  }
                  onKeyDown={(e) =>
                    e.key === 'Enter' &&
                    void videoExporterStore.updateConfig({ extraFfmpegArgs: extraArgsText })
                  }
                />
              </div>
            </div>

            {config.postFilterPreset === 'custom' && (
              <div className="space-y-1.5">
                <Label htmlFor="custom-vf">自定义 -vf 滤镜表达式</Label>
                <Input
                  id="custom-vf"
                  placeholder="例如: unsharp=5:5:0.8:5:5:0.0,eq=saturation=1.1"
                  disabled={busy}
                  className="font-mono"
                  value={customVfText}
                  onChange={(e) => setCustomVfText(e.target.value)}
                  onBlur={() =>
                    customVfText !== config.customVideoFilter &&
                    void videoExporterStore.updateConfig({ customVideoFilter: customVfText })
                  }
                  onKeyDown={(e) =>
                    e.key === 'Enter' &&
                    void videoExporterStore.updateConfig({ customVideoFilter: customVfText })
                  }
                />
              </div>
            )}

            <Separator />

            <div className="grid grid-cols-1 gap-3.5 sm:grid-cols-2">
              <div className="space-y-1.5">
                <Label htmlFor="ffmpeg-path">FFmpeg 路径覆盖（留空则使用 PATH）</Label>
                <div className="flex flex-wrap items-center gap-2">
                  <Input
                    id="ffmpeg-path"
                    placeholder="自动探测 PATH 中的 ffmpeg.exe"
                    disabled={busy}
                    className="min-w-[180px] flex-1 font-mono"
                    value={ffmpegPathText}
                    onChange={(e) => setFfmpegPathText(e.target.value)}
                    onBlur={() =>
                      ffmpegPathText !== config.ffmpegPathOverride &&
                      void videoExporterStore.probeFfmpeg(ffmpegPathText)
                    }
                    onKeyDown={(e) =>
                      e.key === 'Enter' && void videoExporterStore.probeFfmpeg(ffmpegPathText)
                    }
                  />
                  <Button
                    type="button"
                    variant="outline"
                    size="sm"
                    className="shrink-0"
                    disabled={busy}
                    onClick={() => void videoExporterStore.browseFfmpeg()}
                  >
                    浏览...
                  </Button>
                  <Button
                    type="button"
                    variant="outline"
                    size="sm"
                    className="shrink-0"
                    disabled={busy}
                    onClick={() => void videoExporterStore.probeFfmpeg(ffmpegPathText)}
                  >
                    <Search className="size-4" />
                    探测
                  </Button>
                </div>
              </div>

              <div className="space-y-1.5">
                <Label htmlFor="output-dir">视频输出目录</Label>
                <div className="flex flex-wrap items-center gap-2">
                  <Input
                    id="output-dir"
                    disabled={busy}
                    className="min-w-[180px] flex-1 font-mono"
                    value={outputDirText}
                    onChange={(e) => setOutputDirText(e.target.value)}
                    onBlur={() =>
                      outputDirText !== config.outputDir &&
                      void videoExporterStore.updateConfig({ outputDir: outputDirText })
                    }
                    onKeyDown={(e) =>
                      e.key === 'Enter' &&
                      void videoExporterStore.updateConfig({ outputDir: outputDirText })
                    }
                  />
                  <Button
                    type="button"
                    variant="outline"
                    size="sm"
                    className="shrink-0"
                    onClick={() => void videoExporterStore.openFolder(false)}
                  >
                    <HardDriveDownload className="size-4" />
                    打开文件夹
                  </Button>
                </div>
              </div>
            </div>
          </CardContent>
        </Card>
      </main>
    </div>
  )
}

export default App
