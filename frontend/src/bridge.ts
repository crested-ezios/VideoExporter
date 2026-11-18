import { WebView2BridgeCore, isWebView2Available } from './bridge/core'
import {
  BridgeError,
  DEFAULT_CONFIG,
  DEFAULT_SERVICES,
  DEFAULT_STATUS,
  type BrowseResult,
  type RecorderConfig,
  type VideoExporterBridgeClient,
  type VideoExporterEventName,
  type VideoExporterEvents,
  type VideoExporterMethodName,
  type VideoExporterMethods,
  type VideoExporterStateSnapshot,
} from './protocol'

export { isWebView2Available }

export class VideoExporterWebView2Client
  extends WebView2BridgeCore<VideoExporterMethods, VideoExporterEvents>
  implements VideoExporterBridgeClient
{
  readonly kind = 'webview2' as const
}

export class MockVideoExporterBridgeClient implements VideoExporterBridgeClient {
  readonly kind = 'mock' as const
  private state: VideoExporterStateSnapshot
  private listeners = new Map<VideoExporterEventName, Set<(payload: never) => void>>()
  private timer: ReturnType<typeof setInterval> | null = null

  constructor(initial?: Partial<VideoExporterStateSnapshot>) {
    this.state = {
      config: { ...DEFAULT_CONFIG, ...initial?.config },
      status: { ...DEFAULT_STATUS, ...initial?.status },
      services: {
        screenshot: {
          ...DEFAULT_SERVICES.screenshot,
          available: true,
          runtimeReady: true,
          cameraAvailable: true,
          greenScreenDetected: true,
          ...initial?.services?.screenshot,
        },
        clock: {
          ...DEFAULT_SERVICES.clock,
          available: true,
          timelineDurationSeconds: 132.5,
          hasAudio: true,
          trackTitle: 'Campus Mode!! (Preview)',
          audioFilePath: 'D:\\Audio\\campus_mode.wav',
          activeAudioEffectId: 'concert_hall',
          ...initial?.services?.clock,
        },
        audio: {
          ...DEFAULT_SERVICES.audio,
          available: true,
          activePresetId: 'concert_hall',
          ...initial?.services?.audio,
        },
        ffmpeg: {
          ...DEFAULT_SERVICES.ffmpeg,
          available: true,
          nvencRuntimeVerified: true,
          ffmpegPath: 'C:\\Tools\\ffmpeg\\bin\\ffmpeg.exe',
          ffprobePath: 'C:\\Tools\\ffmpeg\\bin\\ffprobe.exe',
          version: 'ffmpeg version n7.1.1-preview',
          encoders: {
            h264_nvenc: true,
            hevc_nvenc: true,
            libx264: true,
            libx265: true,
            prores_ks: true,
            'libvpx-vp9': true,
          },
          ...initial?.services?.ffmpeg,
        },
      },
      appearance: { theme: 0, uiScale: 1, ...initial?.appearance },
    }
  }

  async request<M extends VideoExporterMethodName>(
    method: M,
    params?: VideoExporterMethods[M]['params'],
  ): Promise<VideoExporterMethods[M]['result']> {
    const p = (params ?? {}) as Record<string, unknown>
    switch (method) {
      case 'bridge.hello':
        return {
          protocol: 1,
          sessionId: 1,
          pluginId: 'videoexporter',
          pluginVersion: '0.1.0',
          capabilities: [
            'bridge.hello',
            'bridge.ping',
            'state.get',
            'recording.start',
            'recording.stop',
            'config.set',
            'folder.open',
            'ffmpeg.browse',
            'ffmpeg.probe',
            'appearance.get',
            'shell.setTextInputActive',
          ],
          events: ['state.changed', 'appearance.changed', 'ffmpeg.browsed', 'bridge.closed'],
        } as VideoExporterMethods[M]['result']
      case 'bridge.ping':
        return { pong: true } as VideoExporterMethods[M]['result']
      case 'state.get':
        return structuredClone(this.state) as VideoExporterMethods[M]['result']
      case 'appearance.get':
        return { ...this.state.appearance } as VideoExporterMethods[M]['result']
      case 'shell.setTextInputActive':
        return { active: Boolean(p.active) } as VideoExporterMethods[M]['result']
      case 'config.set': {
        if (this.state.status.busy) {
          throw new BridgeError('invalid_params', 'Cannot modify settings while recording is active')
        }
        const patch = (
          p.config && typeof p.config === 'object' ? p.config : p
        ) as Partial<RecorderConfig>
        this.state = {
          ...this.state,
          config: { ...this.state.config, ...patch },
        }
        this.emit('state.changed', structuredClone(this.state))
        return { config: structuredClone(this.state.config) } as VideoExporterMethods[M]['result']
      }
      case 'recording.start': {
        if (this.state.status.busy) {
          throw new BridgeError('not_ready', 'Already recording')
        }
        const total =
          this.state.config.recordingMode === 'freerun'
            ? 0
            : Math.max(
                1,
                Math.round(
                  (this.state.config.recordingMode === 'lockstep_range'
                    ? Math.max(0.1, this.state.config.rangeEndSeconds - this.state.config.rangeStartSeconds)
                    : Math.max(1, this.state.services.clock.timelineDurationSeconds)) *
                    this.state.config.fps,
                ),
              )
        this.state = {
          ...this.state,
          status: {
            ...DEFAULT_STATUS,
            phase: 'recording',
            recording: true,
            busy: true,
            targetFrames: total,
            phaseMessage:
              this.state.config.recordingMode === 'freerun' ? '正在自由录制' : '正在锁步录制',
          },
        }
        this.startMockTimer()
        this.emit('state.changed', structuredClone(this.state))
        return {
          started: true,
          status: structuredClone(this.state.status),
        } as VideoExporterMethods[M]['result']
      }
      case 'recording.stop': {
        if (!this.state.status.recording) {
          throw new BridgeError('not_ready', 'Not recording')
        }
        this.finishMockRecording()
        return {
          stopping: true,
          status: structuredClone(this.state.status),
        } as VideoExporterMethods[M]['result']
      }
      case 'folder.open':
        return {
          opened: true,
          path: (p.path as string) || this.state.config.outputDir,
        } as VideoExporterMethods[M]['result']
      case 'ffmpeg.probe': {
        const path = typeof p.path === 'string' ? p.path : this.state.config.ffmpegPathOverride
        if (typeof p.path === 'string') {
          this.state.config.ffmpegPathOverride = path
        }
        this.emit('state.changed', structuredClone(this.state))
        return { ffmpeg: structuredClone(this.state.services.ffmpeg) } as VideoExporterMethods[M]['result']
      }
      case 'ffmpeg.browse': {
        const res: BrowseResult = {
          dialogId: 1,
          state: 'accepted',
          path: this.state.services.ffmpeg.ffmpegPath || 'C:\\Tools\\ffmpeg\\bin\\ffmpeg.exe',
          notice: '',
        }
        this.state.config.ffmpegPathOverride = res.path
        this.emit('ffmpeg.browsed', res)
        this.emit('state.changed', structuredClone(this.state))
        return res as VideoExporterMethods[M]['result']
      }
      case 'dialog.poll':
        return { state: 'accepted', path: '', notice: '' } as VideoExporterMethods[M]['result']
    }
    throw new BridgeError('unknown_method', `Unknown method: ${method}`)
  }

  on<E extends VideoExporterEventName>(
    event: E,
    handler: (payload: VideoExporterEvents[E]) => void,
  ): () => void {
    let set = this.listeners.get(event)
    if (!set) {
      set = new Set()
      this.listeners.set(event, set)
    }
    set.add(handler as (payload: never) => void)
    return () => {
      set?.delete(handler as (payload: never) => void)
    }
  }

  emit<E extends VideoExporterEventName>(event: E, payload: VideoExporterEvents[E]): void {
    const set = this.listeners.get(event)
    if (set) {
      for (const h of set) (h as (arg: VideoExporterEvents[E]) => void)(payload)
    }
  }

  setAppearance(theme: number, uiScale: number): void {
    this.state.appearance = { theme, uiScale }
    this.emit('appearance.changed', { theme, uiScale })
  }

  close(): void {
    if (this.timer) {
      clearInterval(this.timer)
      this.timer = null
    }
    this.listeners.clear()
  }

  private startMockTimer(): void {
    if (this.timer) clearInterval(this.timer)
    this.timer = setInterval(() => {
      if (!this.state.status.recording) return
      const nextCaptured = this.state.status.capturedFrames + 12
      const nextEncoded = Math.max(0, nextCaptured - 2)
      const target = this.state.status.targetFrames
      if (target > 0 && nextCaptured >= target) {
        this.state.status.capturedFrames = target
        this.state.status.encodedFrames = target
        this.state.status.progress = 1
        this.finishMockRecording()
        return
      }
      this.state.status = {
        ...this.state.status,
        capturedFrames: nextCaptured,
        encodedFrames: nextEncoded,
        elapsedSec: Number((this.state.status.elapsedSec + 0.1).toFixed(1)),
        fps: 42.5,
        progress: target > 0 ? Math.min(1, nextCaptured / target) : 0,
      }
      this.emit('state.changed', structuredClone(this.state))
    }, 100)
  }

  private finishMockRecording(): void {
    if (this.timer) {
      clearInterval(this.timer)
      this.timer = null
    }
    const ext =
      this.state.config.codec === 'prores_ks'
        ? '.mov'
        : this.state.config.codec === 'libvpx-vp9'
          ? '.webm'
          : '.mp4'
    const outPath = `${this.state.config.outputDir}\\Campus_Mode_${this.state.config.width}x${this.state.config.height}_${this.state.config.fps}fps${ext}`
    this.state.status = {
      ...this.state.status,
      phase: 'completed',
      recording: false,
      busy: false,
      encodedFrames: this.state.status.capturedFrames,
      progress: 1,
      phaseMessage: '导出完成',
      lastOutputPath: outPath,
    }
    this.emit('state.changed', structuredClone(this.state))
  }
}

let activeClient: VideoExporterBridgeClient | null = null
let autoMockDisabled = false

export function getVideoExporterBridge(): VideoExporterBridgeClient | null {
  if (!activeClient) {
    if (isWebView2Available()) activeClient = new VideoExporterWebView2Client()
    else if (!autoMockDisabled && (import.meta.env.DEV || import.meta.env.MODE === 'preview')) {
      activeClient = new MockVideoExporterBridgeClient()
    } else return null
  }
  return activeClient
}

export function setVideoExporterBridge(next: VideoExporterBridgeClient | null): void {
  activeClient = next
}

export function setDisableAutoMockForTest(disabled: boolean): void {
  autoMockDisabled = disabled
}
