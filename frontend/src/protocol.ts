import type { RequestOptions } from './bridge/protocol'

export { BridgeError, PROTOCOL_VERSION, type RequestOptions } from './bridge/protocol'

export type RecordingMode = 'lockstep_full' | 'lockstep_range' | 'freerun'
export type VideoCodec =
  | 'auto'
  | 'h264_nvenc'
  | 'hevc_nvenc'
  | 'libx264'
  | 'libx265'
  | 'prores_ks'
  | 'libvpx-vp9'
export type PixelFormat = 'yuv420p' | 'yuv444p'
export type AlphaMode = 'auto' | 'greenscreen' | 'force_transparent' | 'opaque'
export type PostFilterPreset = 'none' | 'sharpen' | 'vivid' | 'motion_smooth' | 'custom'
export type RecorderPhase = 'idle' | 'starting' | 'recording' | 'post_processing' | 'completed' | 'error'

export interface RecorderConfig {
  version?: number
  width: number
  height: number
  fps: number
  recordingMode: RecordingMode
  rangeStartSeconds: number
  rangeEndSeconds: number
  codec: VideoCodec
  pixelFormat: PixelFormat
  alphaMode: AlphaMode
  crf: number
  muxAudio: boolean
  bakeAudioEffects: boolean
  postFilterPreset: PostFilterPreset
  customVideoFilter: string
  extraFfmpegArgs: string
  ffmpegPathOverride: string
  outputDir: string
}

export interface RecorderStatus {
  phase: RecorderPhase
  recording: boolean
  busy: boolean
  capturedFrames: number
  encodedFrames: number
  targetFrames: number
  fps: number
  elapsedSec: number
  progress: number
  ringOverflowStalls: number
  phaseMessage: string
  lastOutputPath: string
  lastError: string
}

export interface ScreenshotServiceStatus {
  available: boolean
  runtimeReady: boolean
  cameraAvailable: boolean
  greenScreenDetected: boolean
  maxDimension: number
  windowWidth: number
  windowHeight: number
}

export interface ClockServiceStatus {
  available: boolean
  playbackState: number
  runMode: number
  currentTimeSeconds: number
  timelineDurationSeconds: number
  hasAudio: boolean
  trackTitle: string
  audioFilePath: string
  activeAudioEffectId: string
}

export interface AudioServiceStatus {
  available: boolean
  activePresetId: string
}

export interface FfmpegEncoders {
  h264_nvenc: boolean
  hevc_nvenc: boolean
  libx264: boolean
  libx265: boolean
  prores_ks: boolean
  'libvpx-vp9': boolean
}

export interface FfmpegServiceStatus {
  available: boolean
  nvencRuntimeVerified: boolean
  ffmpegPath: string
  ffprobePath: string
  version: string
  error: string
  encoders: FfmpegEncoders
}

export interface ServiceStatuses {
  screenshot: ScreenshotServiceStatus
  clock: ClockServiceStatus
  audio: AudioServiceStatus
  ffmpeg: FfmpegServiceStatus
}

export interface AppearanceInfo {
  theme: number
  uiScale: number
}

export interface VideoExporterStateSnapshot {
  config: RecorderConfig
  status: RecorderStatus
  services: ServiceStatuses
  appearance: AppearanceInfo
}

export interface BridgeHelloResult {
  protocol: number
  sessionId: number
  pluginId: string
  pluginVersion: string
  capabilities: string[]
  events: string[]
}

export interface BrowseResult {
  dialogId: number
  state: 'pending' | 'accepted' | 'cancelled' | 'failed' | 'idle'
  path: string
  notice: string
}

export type VideoExporterMethods = {
  'bridge.hello': { params: Record<string, never>; result: BridgeHelloResult }
  'bridge.ping': { params: Record<string, never>; result: { pong: boolean } }
  'state.get': { params: Record<string, never>; result: VideoExporterStateSnapshot }
  'recording.start': {
    params: { config?: Partial<RecorderConfig> }
    result: { started: boolean; status: RecorderStatus }
  }
  'recording.stop': {
    params: Record<string, never>
    result: { stopping: boolean; status: RecorderStatus }
  }
  'config.set': {
    params: Partial<RecorderConfig> | { config: Partial<RecorderConfig> }
    result: { config: RecorderConfig }
  }
  'folder.open': {
    params: { path?: string; selectLastOutput?: boolean }
    result: { opened: boolean; path: string }
  }
  'ffmpeg.browse': {
    params: Record<string, never>
    result: BrowseResult
  }
  'ffmpeg.probe': {
    params: { path?: string }
    result: { ffmpeg: FfmpegServiceStatus }
  }
  'dialog.poll': {
    params: { dialogId: number }
    result: Omit<BrowseResult, 'dialogId'>
  }
  'appearance.get': {
    params: Record<string, never>
    result: AppearanceInfo
  }
  'shell.setTextInputActive': {
    params: { active: boolean }
    result: { active: boolean }
  }
}

export type VideoExporterEvents = {
  'state.changed': VideoExporterStateSnapshot
  'appearance.changed': AppearanceInfo
  'ffmpeg.browsed': BrowseResult
  'bridge.closed': { reason?: string }
}

export type VideoExporterMethodName = keyof VideoExporterMethods & string
export type VideoExporterEventName = keyof VideoExporterEvents & string

export interface VideoExporterBridgeClient {
  readonly kind: 'webview2' | 'mock'
  request<M extends VideoExporterMethodName>(
    method: M,
    params?: VideoExporterMethods[M]['params'],
    options?: RequestOptions,
  ): Promise<VideoExporterMethods[M]['result']>
  on<E extends VideoExporterEventName>(
    event: E,
    handler: (payload: VideoExporterEvents[E]) => void,
  ): () => void
  close(): void
}

export const RESOLUTION_PRESETS = [
  { id: '1280x720', label: '1280×720 (HD)', width: 1280, height: 720 },
  { id: '1920x1080', label: '1920×1080 (FHD)', width: 1920, height: 1080 },
  { id: '2560x1440', label: '2560×1440 (2K QHD)', width: 2560, height: 1440 },
  { id: '3840x2160', label: '3840×2160 (4K UHD)', width: 3840, height: 2160 },
  { id: '1080x1920', label: '1080×1920 (竖屏 FHD)', width: 1080, height: 1920 },
  { id: '2160x3840', label: '2160×3840 (竖屏 4K)', width: 2160, height: 3840 },
] as const

export const FPS_PRESETS = [24, 30, 60, 120] as const

export const CODEC_OPTIONS: ReadonlyArray<{
  value: VideoCodec
  label: string
  alpha: boolean
  container: string
}> = [
  { value: 'auto', label: 'MP4 H.264 (自动优选 NVENC)', alpha: false, container: 'MP4' },
  { value: 'h264_nvenc', label: 'MP4 H.264 (NVIDIA NVENC)', alpha: false, container: 'MP4' },
  { value: 'libx264', label: 'MP4 H.264 (CPU libx264)', alpha: false, container: 'MP4' },
  { value: 'hevc_nvenc', label: 'MP4 HEVC (NVIDIA NVENC)', alpha: false, container: 'MP4' },
  { value: 'libx265', label: 'MP4 HEVC (CPU libx265)', alpha: false, container: 'MP4' },
  { value: 'prores_ks', label: 'MOV ProRes 4444 (带Alpha透明)', alpha: true, container: 'MOV' },
  { value: 'libvpx-vp9', label: 'WebM VP9 (带Alpha透明)', alpha: true, container: 'WebM' },
]

export const POST_FILTER_OPTIONS: ReadonlyArray<{
  value: PostFilterPreset
  label: string
  expr: string
}> = [
  { value: 'none', label: '无滤镜（无损混流 Copy）', expr: '' },
  { value: 'sharpen', label: '细节锐化 (unsharp)', expr: 'unsharp=5:5:0.75:5:5:0.0' },
  { value: 'vivid', label: '色彩增强 (vivid eq)', expr: 'eq=contrast=1.06:saturation=1.15:brightness=0.01' },
  { value: 'motion_smooth', label: '运动平滑 (tblend)', expr: 'tblend=all_mode=average' },
  { value: 'custom', label: '自定义 -vf 滤镜', expr: '' },
]

export function isAlphaCodec(codec: VideoCodec): boolean {
  return codec === 'prores_ks' || codec === 'libvpx-vp9'
}

export const DEFAULT_CONFIG: RecorderConfig = {
  width: 1920,
  height: 1080,
  fps: 60,
  recordingMode: 'lockstep_full',
  rangeStartSeconds: 0,
  rangeEndSeconds: 10,
  codec: 'auto',
  pixelFormat: 'yuv420p',
  alphaMode: 'auto',
  crf: 18,
  muxAudio: true,
  bakeAudioEffects: true,
  postFilterPreset: 'none',
  customVideoFilter: '',
  extraFfmpegArgs: '',
  ffmpegPathOverride: '',
  outputDir: 'C:\\GakuLoader\\UserData\\videoexporter\\Videos',
}

export const DEFAULT_STATUS: RecorderStatus = {
  phase: 'idle',
  recording: false,
  busy: false,
  capturedFrames: 0,
  encodedFrames: 0,
  targetFrames: 0,
  fps: 0,
  elapsedSec: 0,
  progress: 0,
  ringOverflowStalls: 0,
  phaseMessage: '准备就绪',
  lastOutputPath: '',
  lastError: '',
}

export const DEFAULT_SERVICES: ServiceStatuses = {
  screenshot: {
    available: false,
    runtimeReady: false,
    cameraAvailable: false,
    greenScreenDetected: false,
    maxDimension: 4096,
    windowWidth: 1920,
    windowHeight: 1080,
  },
  clock: {
    available: false,
    playbackState: 0,
    runMode: 0,
    currentTimeSeconds: 0,
    timelineDurationSeconds: 0,
    hasAudio: false,
    trackTitle: '',
    audioFilePath: '',
    activeAudioEffectId: 'off',
  },
  audio: {
    available: false,
    activePresetId: 'off',
  },
  ffmpeg: {
    available: false,
    nvencRuntimeVerified: false,
    ffmpegPath: '',
    ffprobePath: '',
    version: '',
    error: '',
    encoders: {
      h264_nvenc: false,
      hevc_nvenc: false,
      libx264: false,
      libx265: false,
      prores_ks: false,
      'libvpx-vp9': false,
    },
  },
}
