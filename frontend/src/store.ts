import { useSyncExternalStore } from 'react'
import { getVideoExporterBridge, setVideoExporterBridge } from './bridge'
import {
  DEFAULT_CONFIG,
  DEFAULT_SERVICES,
  DEFAULT_STATUS,
  type AppearanceInfo,
  type RecorderConfig,
  type RecorderStatus,
  type ServiceStatuses,
  type VideoExporterBridgeClient,
  type VideoExporterStateSnapshot,
} from './protocol'

export interface StoreState {
  connected: boolean
  connecting: boolean
  bridgeKind: 'webview2' | 'mock' | null
  connectionError: string | null
  actionNotice: string | null
  actionError: string | null
  config: RecorderConfig
  status: RecorderStatus
  services: ServiceStatuses
  appearance: AppearanceInfo
}

function initialStoreState(): StoreState {
  return {
    connected: false,
    connecting: false,
    bridgeKind: null,
    connectionError: null,
    actionNotice: null,
    actionError: null,
    config: { ...DEFAULT_CONFIG },
    status: { ...DEFAULT_STATUS },
    services: structuredClone(DEFAULT_SERVICES),
    appearance: { theme: 0, uiScale: 1 },
  }
}

class VideoExporterStore {
  private state: StoreState = initialStoreState()
  private listeners = new Set<() => void>()
  private unsubscribes: Array<() => void> = []
  private activeClient: VideoExporterBridgeClient | null = null
  private bootCounter = 0

  getState = (): StoreState => this.state

  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener)
    return () => {
      this.listeners.delete(listener)
    }
  }

  private setState(updater: (prev: StoreState) => StoreState): void {
    this.state = updater(this.state)
    for (const listener of this.listeners) {
      listener()
    }
  }

  private applySnapshot(snap: VideoExporterStateSnapshot): void {
    this.setState((prev) => ({
      ...prev,
      config: snap.config,
      status: snap.status,
      services: snap.services,
      appearance: snap.appearance ?? prev.appearance,
    }))
  }

  async connect(): Promise<void> {
    const bootId = ++this.bootCounter
    this.cleanupSubscriptions()
    const client = getVideoExporterBridge()
    if (!client) {
      this.setState((prev) => ({
        ...prev,
        connected: false,
        connecting: false,
        bridgeKind: null,
        connectionError: 'WebView2 原生桥接不可用，请在 GakuLoader 面板内打开或使用预览模式。',
      }))
      return
    }

    this.activeClient = client
    this.setState((prev) => ({
      ...prev,
      connecting: true,
      connectionError: null,
      bridgeKind: client.kind,
    }))

    try {
      await client.request('bridge.hello', {})
      if (bootId !== this.bootCounter) return

      this.unsubscribes.push(
        client.on('state.changed', (snap) => {
          if (bootId !== this.bootCounter) return
          this.applySnapshot(snap)
        }),
        client.on('appearance.changed', (appearance) => {
          if (bootId !== this.bootCounter) return
          this.setState((prev) => ({ ...prev, appearance }))
        }),
        client.on('ffmpeg.browsed', (res) => {
          if (bootId !== this.bootCounter) return
          if (res.state === 'accepted' && res.path) {
            this.setState((prev) => ({
              ...prev,
              actionNotice: `已选择 FFmpeg: ${res.path}`,
              actionError: null,
            }))
            void this.refreshState()
          } else if (res.state === 'failed') {
            this.setState((prev) => ({
              ...prev,
              actionError: res.notice || '选择 FFmpeg 文件失败',
            }))
          }
        }),
        client.on('bridge.closed', () => {
          if (bootId !== this.bootCounter) return
          this.setState((prev) => ({
            ...prev,
            connected: false,
            connectionError: '原生桥接会话已关闭',
          }))
        }),
      )

      const snapshot = await client.request('state.get', {})
      if (bootId !== this.bootCounter) return

      this.setState((prev) => ({
        ...prev,
        connected: true,
        connecting: false,
        connectionError: null,
        config: snapshot.config,
        status: snapshot.status,
        services: snapshot.services,
        appearance: snapshot.appearance ?? prev.appearance,
      }))
    } catch (error) {
      if (bootId !== this.bootCounter) return
      this.setState((prev) => ({
        ...prev,
        connected: false,
        connecting: false,
        connectionError: error instanceof Error ? error.message : '连接 VideoExporter 桥接失败',
      }))
    }
  }

  disconnect(): void {
    ++this.bootCounter
    this.cleanupSubscriptions()
    this.activeClient = null
  }

  resetForTest(client: VideoExporterBridgeClient | null = null): void {
    this.disconnect()
    setVideoExporterBridge(client)
    this.state = initialStoreState()
    for (const listener of this.listeners) {
      listener()
    }
  }

  async refreshState(): Promise<void> {
    if (!this.activeClient) return
    try {
      const snapshot = await this.activeClient.request('state.get', {})
      this.applySnapshot(snapshot)
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : '刷新状态失败',
      }))
    }
  }

  async updateConfig(patch: Partial<RecorderConfig>): Promise<boolean> {
    if (!this.activeClient) return false
    try {
      const res = await this.activeClient.request('config.set', patch)
      this.setState((prev) => ({
        ...prev,
        config: res.config,
        actionError: null,
      }))
      return true
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : '更新配置失败',
      }))
      return false
    }
  }

  async startRecording(): Promise<boolean> {
    if (!this.activeClient) return false
    try {
      const res = await this.activeClient.request('recording.start', {})
      this.setState((prev) => ({
        ...prev,
        status: res.status,
        actionError: null,
        actionNotice: null,
      }))
      return true
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : '无法开始录制',
      }))
      return false
    }
  }

  async stopRecording(): Promise<boolean> {
    if (!this.activeClient) return false
    try {
      const res = await this.activeClient.request('recording.stop', {})
      this.setState((prev) => ({
        ...prev,
        status: res.status,
        actionError: null,
      }))
      return true
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : '无法停止录制',
      }))
      return false
    }
  }

  async openFolder(selectLastOutput = false): Promise<boolean> {
    if (!this.activeClient) return false
    try {
      const res = await this.activeClient.request('folder.open', {
        path: this.state.config.outputDir,
        selectLastOutput,
      })
      this.setState((prev) => ({
        ...prev,
        actionNotice: `已打开目录: ${res.path}`,
        actionError: null,
      }))
      return true
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : '打开目录失败',
      }))
      return false
    }
  }

  async browseFfmpeg(): Promise<void> {
    if (!this.activeClient) return
    try {
      const res = await this.activeClient.request('ffmpeg.browse', {})
      if (res.state === 'accepted' && res.path) {
        this.setState((prev) => ({
          ...prev,
          actionNotice: `已选择 FFmpeg: ${res.path}`,
          actionError: null,
        }))
        await this.refreshState()
      } else if (res.state === 'failed') {
        this.setState((prev) => ({
          ...prev,
          actionError: res.notice || '选择 FFmpeg 文件失败',
        }))
      }
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : '浏览 FFmpeg 失败',
      }))
    }
  }

  async probeFfmpeg(path?: string): Promise<void> {
    if (!this.activeClient) return
    try {
      const res = await this.activeClient.request(
        'ffmpeg.probe',
        path !== undefined ? { path } : {},
      )
      this.setState((prev) => ({
        ...prev,
        services: { ...prev.services, ffmpeg: res.ffmpeg },
        actionNotice: res.ffmpeg.available
          ? `FFmpeg 探测成功: ${res.ffmpeg.version || res.ffmpeg.ffmpegPath}`
          : null,
        actionError: res.ffmpeg.available ? null : res.ffmpeg.error || 'FFmpeg 探测失败',
      }))
    } catch (error) {
      this.setState((prev) => ({
        ...prev,
        actionError: error instanceof Error ? error.message : 'FFmpeg 探测失败',
      }))
    }
  }

  clearMessages(): void {
    this.setState((prev) => ({ ...prev, actionNotice: null, actionError: null }))
  }

  private cleanupSubscriptions(): void {
    for (const unsub of this.unsubscribes) {
      unsub()
    }
    this.unsubscribes = []
  }
}

export const videoExporterStore = new VideoExporterStore()

export function useVideoExporterStore<T>(selector: (state: StoreState) => T): T {
  return useSyncExternalStore(
    videoExporterStore.subscribe,
    () => selector(videoExporterStore.getState()),
  )
}
