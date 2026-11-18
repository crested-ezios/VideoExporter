import { act, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { App } from '../App'
import { MockVideoExporterBridgeClient, setDisableAutoMockForTest } from '../bridge'
import { videoExporterStore } from '../store'

async function renderConnectedApp() {
  await act(async () => {
    render(<App />)
  })
  await waitFor(() => {
    expect(screen.getByTestId('service-badges')).toBeInTheDocument()
  })
}

describe('VideoExporter UI & Store', () => {
  let mockClient: MockVideoExporterBridgeClient

  beforeEach(() => {
    setDisableAutoMockForTest(false)
    mockClient = new MockVideoExporterBridgeClient()
    videoExporterStore.resetForTest(mockClient)
  })

  afterEach(() => {
    mockClient.close()
    videoExporterStore.resetForTest(null)
    setDisableAutoMockForTest(false)
    vi.useRealTimers()
  })

  it('connects and renders all four service status badges and FFmpeg encoders', async () => {
    await renderConnectedApp()

    const badges = screen.getByTestId('service-badges')
    expect(badges).toHaveTextContent('ScreenshotManager: 就绪 (4096px)')
    expect(badges).toHaveTextContent('绿幕透明通道已探测')
    expect(badges).toHaveTextContent('GakuMotion 时钟: Campus Mode!! (Preview)')
    expect(badges).toHaveTextContent('GakuAudio: 已连接 (concert_hall)')
    expect(badges).toHaveTextContent('FFmpeg: n7.1.1-preview')
    expect(badges).toHaveTextContent('NVENC')
    expect(badges).toHaveTextContent('ProRes 4444')
    expect(badges).toHaveTextContent('VP9 Alpha')
  })

  it('updates FPS, custom resolution, and recording scope range inputs', async () => {
    await renderConnectedApp()

    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: '120 FPS' }))
    })
    await waitFor(() => {
      expect(videoExporterStore.getState().config.fps).toBe(120)
    })

    const widthInput = screen.getByLabelText('宽度 (px)') as HTMLInputElement
    const heightInput = screen.getByLabelText('高度 (px)') as HTMLInputElement
    await act(async () => {
      fireEvent.change(widthInput, { target: { value: '3840' } })
      fireEvent.change(heightInput, { target: { value: '2160' } })
      fireEvent.blur(heightInput)
    })
    await waitFor(() => {
      expect(videoExporterStore.getState().config.width).toBe(3840)
      expect(videoExporterStore.getState().config.height).toBe(2160)
    })

    await act(async () => {
      await videoExporterStore.updateConfig({ recordingMode: 'lockstep_range' })
    })
    expect(screen.getByLabelText('开始时间 (秒)')).toBeInTheDocument()
    expect(screen.getByLabelText('结束时间 (秒)')).toBeInTheDocument()
  })

  it('shows alpha capture controls for ProRes 4444 and VP9 and custom -vf for custom preset', async () => {
    await renderConnectedApp()

    expect(screen.queryByLabelText('透明通道采集模式')).not.toBeInTheDocument()
    await act(async () => {
      await videoExporterStore.updateConfig({ codec: 'prores_ks' })
    })
    expect(screen.getByText(/ProRes 4444 \(yuva444p10le\)/)).toBeInTheDocument()
    expect(screen.getByLabelText('透明通道采集模式')).toBeInTheDocument()

    await act(async () => {
      await videoExporterStore.updateConfig({ postFilterPreset: 'custom' })
    })
    const vfInput = screen.getByLabelText('自定义 -vf 滤镜表达式')
    expect(vfInput).toBeInTheDocument()
    await act(async () => {
      fireEvent.change(vfInput, { target: { value: 'unsharp=5:5:1.0:5:5:0.0' } })
      fireEvent.blur(vfInput)
    })
    await waitFor(() => {
      expect(videoExporterStore.getState().config.customVideoFilter).toBe('unsharp=5:5:1.0:5:5:0.0')
    })
  })

  it('starts and stops recording and updates live HUD metrics', async () => {
    await renderConnectedApp()

    const reqSpy = vi.spyOn(mockClient, 'request')
    await act(async () => {
      fireEvent.click(screen.getByTestId('toggle-recording-btn'))
    })

    await waitFor(() => {
      expect(videoExporterStore.getState().status.recording).toBe(true)
    })
    expect(reqSpy).toHaveBeenCalledWith('recording.start', {})
    expect(screen.getByTestId('phase-badge')).toHaveTextContent('录制中')

    act(() => {
      const snap = structuredClone(videoExporterStore.getState())
      mockClient.emit('state.changed', {
        config: snap.config,
        services: snap.services,
        appearance: snap.appearance,
        status: {
          ...snap.status,
          phase: 'recording',
          recording: true,
          busy: true,
          capturedFrames: 48,
          encodedFrames: 44,
          targetFrames: 120,
          fps: 59.4,
          elapsedSec: 0.8,
          progress: 0.4,
          phaseMessage: '正在锁步录制',
        },
      })
    })

    expect(screen.getByTestId('hud-captured')).toHaveTextContent('48 / 120')
    expect(screen.getByTestId('hud-encoded')).toHaveTextContent('44')
    expect(screen.getByTestId('hud-fps')).toHaveTextContent('59.4 fps')

    await act(async () => {
      fireEvent.click(screen.getByTestId('toggle-recording-btn'))
    })
    await waitFor(() => {
      expect(videoExporterStore.getState().status.phase).toBe('completed')
    })
    expect(screen.getByTestId('hud-output-path')).toHaveTextContent('Campus_Mode_1920x1080_60fps.mp4')
  })

  it('invokes ffmpeg.browse, ffmpeg.probe, folder.open and syncs host theme', async () => {
    await renderConnectedApp()
    const reqSpy = vi.spyOn(mockClient, 'request')

    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: '浏览...' }))
    })
    await waitFor(() => {
      expect(reqSpy).toHaveBeenCalledWith('ffmpeg.browse', {})
    })

    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: '探测' }))
    })
    await waitFor(() => {
      expect(reqSpy).toHaveBeenCalledWith('ffmpeg.probe', {
        path: 'C:\\Tools\\ffmpeg\\bin\\ffmpeg.exe',
      })
    })

    await act(async () => {
      fireEvent.click(screen.getByRole('button', { name: '打开文件夹' }))
    })
    await waitFor(() => {
      expect(reqSpy).toHaveBeenCalledWith(
        'folder.open',
        expect.objectContaining({ selectLastOutput: false }),
      )
    })

    act(() => {
      mockClient.setAppearance(3, 1.25)
    })
    await waitFor(() => {
      expect(document.documentElement.dataset.theme).toBe('apple-light')
      expect(document.documentElement.classList.contains('dark')).toBe(false)
      expect(document.documentElement.style.zoom).toBe('1.25')
    })
  })

  it('shows unavailable screen when no bridge is present', async () => {
    setDisableAutoMockForTest(true)
    videoExporterStore.resetForTest(null)
    await act(async () => {
      render(<App />)
    })

    await waitFor(() => {
      expect(screen.getByText('VideoExporter 原生桥接不可用')).toBeInTheDocument()
    })
  })
})
