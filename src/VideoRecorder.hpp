// SPDX-License-Identifier: MIT
#pragma once

#include "../include/GakuAudio.h"
#include "../include/GakuMotionClock.h"
#include "../include/GakuPlugin.h"
#include "../include/GakuScreenshot.h"
#include "CommonUtils.hpp"
#include "Config.hpp"
#include "FfmpegPipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace VideoExporter
{
	inline constexpr size_t kRingSlots = 4;

	enum class RecorderPhase
	{
		Idle = 0,
		Starting = 1,
		Recording = 2,
		PostProcessing = 3,
		Completed = 4,
		Error = 5
	};

	inline const char* PhaseName(RecorderPhase p) noexcept
	{
		switch (p)
		{
		case RecorderPhase::Idle: return "idle";
		case RecorderPhase::Starting: return "starting";
		case RecorderPhase::Recording: return "recording";
		case RecorderPhase::PostProcessing: return "post_processing";
		case RecorderPhase::Completed: return "completed";
		case RecorderPhase::Error: return "error";
		}
		return "idle";
	}

	struct ServiceHostCallbacks
	{
		std::function<bool(GakuScreenshotServiceV1&)> queryScreenshot;
		std::function<bool(GakuMotionClockServiceV1&)> queryClock;
		std::function<bool(GakuAudioServiceV1&)> queryAudio;
		std::function<void()> wakeMainThread;
		std::function<bool()> isRenderFrameHookUnavailable;
		std::function<void(GakuLogLevel, const std::string&)> log;
	};

	struct ScreenshotStatusSnapshot
	{
		bool available{false};
		bool runtimeReady{false};
		bool cameraAvailable{false};
		bool greenScreenDetected{false};
		uint32_t maxDimension{0};
		uint32_t windowWidth{0};
		uint32_t windowHeight{0};
	};

	struct ClockStatusSnapshot
	{
		bool available{false};
		uint32_t playbackState{0};
		uint32_t runMode{0};
		double currentTimeSeconds{0.0};
		double timelineDurationSeconds{0.0};
		bool hasAudio{false};
		std::string trackTitle;
		std::string audioFilePath;
		std::string activeAudioEffectId;
	};

	struct AudioStatusSnapshot
	{
		bool available{false};
		std::string activePresetId;
	};

	struct RecorderStatusSnapshot
	{
		RecorderPhase phase{RecorderPhase::Idle};
		bool recording{false};
		bool busy{false};
		uint64_t capturedFrames{0};
		uint64_t encodedFrames{0};
		uint64_t targetFrames{0};
		double fps{0.0};
		double elapsedSec{0.0};
		double progress{0.0};
		uint64_t ringOverflowStalls{0};
		std::string phaseMessage;
		std::string lastOutputPath;
		std::string lastError;
	};

	class VideoRecorder
	{
	public:
		explicit VideoRecorder(ServiceHostCallbacks host = {})
			: host_(std::move(host))
		{
		}

		~VideoRecorder()
		{
			Shutdown();
		}

		VideoRecorder(const VideoRecorder&) = delete;
		VideoRecorder& operator=(const VideoRecorder&) = delete;

		void InitializePaths(const std::filesystem::path& configPath, const std::string& defaultOutputDir)
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			configPath_ = configPath;
			defaultOutputDir_ = defaultOutputDir.empty() ? "Videos" : defaultOutputDir;
			std::string err;
			if (!configPath_.empty() && std::filesystem::is_regular_file(configPath_))
			{
				LoadConfigFile(configPath_, config_, defaultOutputDir_, err);
			}
			else
			{
				config_ = RecorderConfig{};
				NormalizeConfig(config_, defaultOutputDir_);
			}
		}

		FfmpegProbeResult ProbeFfmpeg(const std::string* overridePath = nullptr)
		{
			std::string pathToProbe;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				pathToProbe = overridePath ? *overridePath : config_.ffmpegPathOverride;
			}
			FfmpegProbeResult res = FfmpegPipeline::Probe(pathToProbe);
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				ffmpegProbe_ = res;
				ffmpegProbedOnce_ = true;
			}
			return res;
		}

		FfmpegProbeResult GetFfmpegProbe()
		{
			bool needProbe = false;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				needProbe = !ffmpegProbedOnce_;
			}
			if (needProbe) return ProbeFfmpeg();
			std::lock_guard<std::mutex> lock(stateMutex_);
			return ffmpegProbe_;
		}

		RecorderConfig GetConfig() const
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			return config_;
		}

		bool UpdateConfig(const GakuMotion::jsonlite::Value& patch, std::string& error)
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (IsBusyLocked())
			{
				error = "Cannot modify settings while recording or post-processing is active";
				return false;
			}
			RecorderConfig next = config_;
			if (!ApplyConfigPatch(patch, next, defaultOutputDir_, error))
			{
				return false;
			}
			const bool ffmpegChanged = next.ffmpegPathOverride != config_.ffmpegPathOverride;
			config_ = std::move(next);
			if (!configPath_.empty())
			{
				std::string saveErr;
				if (!SaveConfigFile(configPath_, config_, saveErr))
				{
					Log(GAKU_LOG_WARNING, "[VideoExporter] Failed to persist config.json: " + saveErr);
				}
			}
			if (ffmpegChanged)
			{
				ffmpegProbedOnce_ = false;
			}
			return true;
		}

		bool SetFfmpegPathAndProbe(const std::string& pathUtf8, FfmpegProbeResult& outProbe, std::string& error)
		{
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (IsBusyLocked())
				{
					error = "Cannot change FFmpeg path while recording or post-processing is active";
					return false;
				}
				config_.ffmpegPathOverride = pathUtf8;
				ffmpegProbedOnce_ = false;
				if (!configPath_.empty())
				{
					std::string saveErr;
					SaveConfigFile(configPath_, config_, saveErr);
				}
			}
			outProbe = ProbeFfmpeg(&pathUtf8);
			return true;
		}

		void SetOutputPathOverrideForTest(const std::filesystem::path& path)
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			outputPathOverride_ = path;
		}

		bool IsRecording() const
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			return phase_ == RecorderPhase::Starting || phase_ == RecorderPhase::Recording;
		}

		bool IsBusy() const
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			return IsBusyLocked();
		}

		RecorderStatusSnapshot GetStatus() const
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			return BuildStatusLocked();
		}

		void QueryLiveServices(ScreenshotStatusSnapshot& ssOut, ClockStatusSnapshot& clkOut, AudioStatusSnapshot& audOut)
		{
			ssOut = {};
			clkOut = {};
			audOut = {};

			GakuScreenshotServiceV1 ss{};
			if (host_.queryScreenshot && host_.queryScreenshot(ss) && ss.getCapabilities)
			{
				ssOut.available = true;
				GakuScreenshotCapabilitiesV1 caps{};
				caps.abiVersion = GAKU_SCREENSHOT_VERSION;
				caps.structSize = sizeof(caps);
				if (ss.getCapabilities(ss.context, &caps) == GAKU_OK)
				{
					ssOut.runtimeReady = caps.runtimeReady != 0;
					ssOut.cameraAvailable = caps.cameraAvailable != 0;
					ssOut.greenScreenDetected = caps.greenScreenDetected != 0;
					ssOut.maxDimension = caps.maxDimension;
					ssOut.windowWidth = caps.windowWidth;
					ssOut.windowHeight = caps.windowHeight;
				}
			}

			GakuMotionClockServiceV1 clk{};
			if (host_.queryClock && host_.queryClock(clk) && clk.getSnapshot)
			{
				clkOut.available = true;
				GakuMotionClockSnapshotV1 snap{};
				snap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
				snap.structSize = sizeof(snap);
				if (clk.getSnapshot(clk.context, &snap) == GAKU_OK)
				{
					clkOut.playbackState = snap.playbackState;
					clkOut.runMode = snap.runMode;
					clkOut.currentTimeSeconds = snap.currentTimeSeconds;
					clkOut.timelineDurationSeconds = snap.timelineDurationSeconds;
					clkOut.hasAudio = snap.hasAudio != 0;
					clkOut.trackTitle = SafeCString(snap.trackTitle, sizeof(snap.trackTitle));
					clkOut.audioFilePath = SafeCString(snap.audioFilePath, sizeof(snap.audioFilePath));
					clkOut.activeAudioEffectId = SafeCString(snap.activeAudioEffectId, sizeof(snap.activeAudioEffectId));
				}
			}

			GakuAudioServiceV1 aud{};
			if (host_.queryAudio && host_.queryAudio(aud))
			{
				audOut.available = true;
				if (aud.getActivePreset)
				{
					char buf[64]{};
					if (aud.getActivePreset(aud.context, buf, sizeof(buf)) == GAKU_OK)
					{
						audOut.activePresetId = buf;
					}
				}
			}
		}

		bool RequestStart(std::string& error)
		{
			error.clear();
			FfmpegProbeResult probe = GetFfmpegProbe();
			if (!probe.available)
			{
				error = probe.error.empty() ? "未检测到可用的 ffmpeg.exe" : probe.error;
				return false;
			}

			ScreenshotStatusSnapshot ssSnap{};
			ClockStatusSnapshot clkSnap{};
			AudioStatusSnapshot audSnap{};
			QueryLiveServices(ssSnap, clkSnap, audSnap);

			std::lock_guard<std::mutex> lock(stateMutex_);
			if (IsBusyLocked())
			{
				error = "当前已有录制或后期合成任务正在进行";
				return false;
			}
			if (!ssSnap.available)
			{
				error = "ScreenshotManager 服务未连接";
				return false;
			}
			if (!ssSnap.runtimeReady || !ssSnap.cameraAvailable)
			{
				error = "ScreenshotManager 运行时或主相机尚未就绪";
				return false;
			}
			if (ssSnap.maxDimension > 0 &&
				(config_.width > ssSnap.maxDimension || config_.height > ssSnap.maxDimension))
			{
				error = "导出分辨率超出 ScreenshotManager 最大支持尺寸 (" + std::to_string(ssSnap.maxDimension) + ")";
				return false;
			}

			const std::string resolvedCodec = FfmpegPipeline::ResolveEffectiveCodec(config_.codec, probe);
			if (!IsEncoderPresent(probe, resolvedCodec))
			{
				error = "当前 FFmpeg 不支持所选编码器: " + resolvedCodec;
				return false;
			}

			if (config_.recordingMode != "freerun")
			{
				if (!clkSnap.available)
				{
					error = "GakuMotion 时钟服务 (gakumotion.clock/v1) 不可用，请切换到自由录制模式";
					return false;
				}
				if (clkSnap.timelineDurationSeconds <= 0.0)
				{
					error = "GakuMotion 当前未加载可录制的曲目";
					return false;
				}
				if (config_.recordingMode == "lockstep_range" &&
					(config_.rangeStartSeconds >= clkSnap.timelineDurationSeconds ||
					 config_.rangeEndSeconds <= config_.rangeStartSeconds))
				{
					error = "区间录制起止时间无效或超出当前曲目时长";
					return false;
				}
			}

			JoinFinishedPostProcessLocked();

			phase_ = RecorderPhase::Starting;
			pendingStart_ = true;
			pendingStop_ = false;
			cancelRequested_.store(false, std::memory_order_release);
			capturedFrames_.store(0, std::memory_order_release);
			targetFrames_ = 0;
			ringOverflowStalls_ = 0;
			lastError_.clear();
			phaseMessage_ = "正在准备录制会话";
			sessionConfig_ = config_;
			sessionProbe_ = probe;
			startRequestedTime_ = std::chrono::steady_clock::now();

			if (host_.wakeMainThread) host_.wakeMainThread();
			return true;
		}

		bool RequestStop(std::string& error)
		{
			error.clear();
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (phase_ == RecorderPhase::Starting)
			{
				pendingStart_ = false;
				phase_ = RecorderPhase::Idle;
				phaseMessage_ = "录制已取消";
				return true;
			}
			if (phase_ != RecorderPhase::Recording)
			{
				error = "当前未处于录制状态";
				return false;
			}
			pendingStop_ = true;
			phaseMessage_ = "正在停止录制";
			return true;
		}

		bool ToggleRecording(std::string& error)
		{
			if (IsRecording()) return RequestStop(error);
			return RequestStart(error);
		}

		// When true, OnTick() only processes start/stop lifecycle control and
		// skips frame capture; frame capture is driven by OnEndOfFrameCapture()
		// after MotionActorTick and Unity rendering have completed.
		void SetDeferredCaptureMode(bool enabled) noexcept
		{
			deferredCaptureMode_.store(enabled, std::memory_order_release);
		}

		bool IsDeferredCaptureMode() const noexcept
		{
			return deferredCaptureMode_.load(std::memory_order_acquire);
		}

		// Processes pending start/stop requests and writer errors on the Unity
		// main thread without capturing a frame. Safe for MainThreadWakeTask.
		void ProcessPendingControl()
		{
			if (writerError_.load(std::memory_order_acquire))
			{
				std::string wErr;
				{
					std::lock_guard<std::mutex> rl(ringMutex_);
					wErr = writerErrorMsg_;
				}
				FinishRecordingCapture(true, true, wErr.empty() ? "FFmpeg 写入线程异常" : wErr);
				return;
			}

			bool shouldStart = false;
			bool shouldStop = false;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (pendingStart_ && phase_ == RecorderPhase::Starting)
				{
					shouldStart = true;
					pendingStart_ = false;
				}
				else if (phase_ == RecorderPhase::Recording && pendingStop_)
				{
					shouldStop = true;
					pendingStop_ = false;
				}
			}

			if (shouldStart)
			{
				StartSessionSync();
				return;
			}
			if (shouldStop)
			{
				FinishRecordingCapture(false, false, "");
			}
		}

		// Must be called on the Unity main thread once per frame.
		void OnTick()
		{
			bool wasRecording = false;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				wasRecording = (phase_ == RecorderPhase::Recording);
			}
			ProcessPendingControl();

			bool isRecordingNow = false;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				isRecordingNow = (phase_ == RecorderPhase::Recording);
			}

			if (deferredCaptureMode_.load(std::memory_order_acquire))
			{
				if (isRecordingNow)
				{
					if (host_.isRenderFrameHookUnavailable && host_.isRenderFrameHookUnavailable())
					{
						deferredCaptureMode_.store(false, std::memory_order_release);
						if (!deferredFallbackLogged_)
						{
							deferredFallbackLogged_ = true;
							Log(GAKU_LOG_WARNING,
								"[VideoExporter] Host render-frame hook unavailable; driving capture from OnTick.");
						}
					}
					else if (wasRecording && ++ticksWithoutEndOfFrame_ >= kMaxTicksWithoutEndOfFrame)
					{
						deferredCaptureMode_.store(false, std::memory_order_release);
						if (!deferredFallbackLogged_)
						{
							deferredFallbackLogged_ = true;
							Log(GAKU_LOG_WARNING,
								"[VideoExporter] Post-render observer silent; falling back to OnTick capture drive.");
						}
					}
				}
				if (deferredCaptureMode_.load(std::memory_order_acquire))
				{
					if (usePresentPath_)
					{
						PollPendingPresentCapture(false);
					}
					return;
				}
			}

			if (usePresentPath_)
			{
				if (!PollPendingPresentCapture(false))
				{
					return;
				}
				if (usePresentPath_)
				{
					TryArmPresentCapture();
					return;
				}
			}
			TickActiveRecording();
		}

		// Must be called on the Unity main thread after actor pose/face updates
		// and frame rendering have completed for the current frame.
		void OnEndOfFrameCapture()
		{
			ticksWithoutEndOfFrame_ = 0;
			if (writerError_.load(std::memory_order_acquire))
			{
				std::string wErr;
				{
					std::lock_guard<std::mutex> rl(ringMutex_);
					wErr = writerErrorMsg_;
				}
				FinishRecordingCapture(true, true, wErr.empty() ? "FFmpeg 写入线程异常" : wErr);
				return;
			}

			bool shouldStop = false;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (phase_ != RecorderPhase::Recording)
				{
					return;
				}
				if (pendingStop_)
				{
					shouldStop = true;
					pendingStop_ = false;
				}
			}
			if (shouldStop)
			{
				FinishRecordingCapture(false, false, "");
				return;
			}
			if (usePresentPath_)
			{
				if (!PollPendingPresentCapture(true))
				{
					return;
				}
				if (usePresentPath_)
				{
					TryArmPresentCapture();
					return;
				}
			}
			TickActiveRecording();
		}

		void Shutdown()
		{
			cancelRequested_.store(true, std::memory_order_release);
			ffmpeg_.AbortPipe();

			bool needFinishCapture = false;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				pendingStart_ = false;
				if (phase_ == RecorderPhase::Starting)
				{
					phase_ = RecorderPhase::Idle;
				}
				else if (phase_ == RecorderPhase::Recording)
				{
					needFinishCapture = true;
				}
			}
			if (needFinishCapture)
			{
				FinishRecordingCapture(true, true, "Plugin shutdown");
			}

			{
				std::lock_guard<std::mutex> ringLock(ringMutex_);
				writerStopRequested_ = true;
			}
			ringNotEmptyCv_.notify_all();
			ringNotFullCv_.notify_all();

			std::thread postThread;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (postProcessThread_.joinable()) postThread = std::move(postProcessThread_);
			}
			if (postThread.joinable()) postThread.join();

			if (writerThread_.joinable()) writerThread_.join();
			FreeRingSlots();
		}

	private:
		struct FrameSlot
		{
			std::vector<uint8_t> data;
			uint32_t width{0};
			uint32_t height{0};
			uint32_t rowPitch{0};
		};

		static bool IsEncoderPresent(const FfmpegProbeResult& probe, const std::string& codec) noexcept
		{
			if (codec == "h264_nvenc") return probe.encoders.h264_nvenc;
			if (codec == "hevc_nvenc") return probe.encoders.hevc_nvenc;
			if (codec == "libx264") return probe.encoders.libx264;
			if (codec == "libx265") return probe.encoders.libx265;
			if (codec == "prores_ks") return probe.encoders.prores_ks;
			if (codec == "libvpx-vp9") return probe.encoders.libvpx_vp9;
			return false;
		}

		static std::string SafeCString(const char* buf, size_t maxLen)
		{
			if (!buf || maxLen == 0) return {};
			const void* nul = std::memchr(buf, '\0', maxLen);
			const size_t len = nul ? static_cast<const char*>(nul) - buf : maxLen;
			return std::string(buf, len);
		}

		bool IsBusyLocked() const noexcept
		{
			return phase_ == RecorderPhase::Starting ||
				phase_ == RecorderPhase::Recording ||
				phase_ == RecorderPhase::PostProcessing;
		}

		void JoinFinishedPostProcessLocked()
		{
			if (postProcessThread_.joinable())
			{
				postProcessThread_.join();
			}
		}

		RecorderStatusSnapshot BuildStatusLocked() const
		{
			RecorderStatusSnapshot s;
			s.phase = phase_;
			s.recording = (phase_ == RecorderPhase::Starting || phase_ == RecorderPhase::Recording);
			s.busy = IsBusyLocked();
			s.capturedFrames = capturedFrames_.load(std::memory_order_acquire);
			s.encodedFrames = (phase_ == RecorderPhase::Recording || phase_ == RecorderPhase::PostProcessing)
				? (std::max)(ffmpeg_.EncodedFrames(), writtenFrames_.load(std::memory_order_acquire))
				: lastEncodedFrames_;
			s.targetFrames = targetFrames_;
			s.ringOverflowStalls = ringOverflowStalls_;
			s.phaseMessage = phaseMessage_;
			s.lastOutputPath = lastOutputPath_;
			s.lastError = lastError_;

			if (phase_ == RecorderPhase::Recording || phase_ == RecorderPhase::PostProcessing)
			{
				const auto endTime = (phase_ == RecorderPhase::Recording)
					? std::chrono::steady_clock::now()
					: captureEndTime_;
				s.elapsedSec = std::chrono::duration<double>(endTime - captureStartTime_).count();
				if (s.elapsedSec > 0.05)
				{
					s.fps = static_cast<double>(s.capturedFrames) / s.elapsedSec;
				}
			}
			else
			{
				s.elapsedSec = lastElapsedSec_;
				s.fps = lastFps_;
			}

			if (s.targetFrames > 0)
			{
				const double raw = static_cast<double>(s.capturedFrames) / static_cast<double>(s.targetFrames);
				s.progress = std::clamp(raw, 0.0, 1.0);
			}
			else
			{
				s.progress = 0.0;
			}
			return s;
		}

		void Log(GakuLogLevel level, const std::string& msg) const
		{
			if (host_.log) host_.log(level, msg);
		}

		bool AllocateRingSlots(size_t frameBytes)
		{
			std::lock_guard<std::mutex> lock(ringMutex_);
			ringReadIdx_ = 0;
			ringWriteIdx_ = 0;
			ringCount_ = 0;
			writerStopRequested_ = false;
			writerError_.store(false, std::memory_order_release);
			writerErrorMsg_.clear();
			try
			{
				for (auto& slot : ringSlots_)
				{
					slot.data.assign(frameBytes, 0);
					slot.width = 0;
					slot.height = 0;
					slot.rowPitch = 0;
				}
				return true;
			}
			catch (...)
			{
				for (auto& slot : ringSlots_)
				{
					std::vector<uint8_t>{}.swap(slot.data);
				}
				return false;
			}
		}

		void FreeRingSlots() noexcept
		{
			std::lock_guard<std::mutex> lock(ringMutex_);
			ringReadIdx_ = 0;
			ringWriteIdx_ = 0;
			ringCount_ = 0;
			for (auto& slot : ringSlots_)
			{
				std::vector<uint8_t>{}.swap(slot.data);
				slot.width = 0;
				slot.height = 0;
				slot.rowPitch = 0;
			}
		}

		void StartSessionSync()
		{
			RecorderConfig cfg;
			FfmpegProbeResult probe;
			std::filesystem::path explicitOutPath;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				JoinFinishedPostProcessLocked();
				cfg = sessionConfig_;
				probe = sessionProbe_;
				explicitOutPath = outputPathOverride_;
				outputPathOverride_.clear();
			}

			auto failStart = [&](const std::string& reason) {
				std::lock_guard<std::mutex> lock(stateMutex_);
				phase_ = RecorderPhase::Error;
				lastError_ = reason;
				phaseMessage_ = "启动录制失败";
				Log(GAKU_LOG_ERROR, "[VideoExporter] " + reason);
			};

			GakuScreenshotServiceV1 ss{};
			if (!host_.queryScreenshot || !host_.queryScreenshot(ss) || !ss.captureFrameImmediate || !ss.getCapabilities)
			{
				failStart("ScreenshotManager 服务不可用");
				return;
			}
			GakuScreenshotCapabilitiesV1 ssCaps{};
			ssCaps.abiVersion = GAKU_SCREENSHOT_VERSION;
			ssCaps.structSize = sizeof(ssCaps);
			if (ss.getCapabilities(ss.context, &ssCaps) != GAKU_OK ||
				!ssCaps.runtimeReady || !ssCaps.cameraAvailable)
			{
				failStart("ScreenshotManager 运行时或相机尚未就绪");
				return;
			}

			const bool isLockstep = (cfg.recordingMode != "freerun");
			GakuMotionClockServiceV1 clk{};
			GakuMotionClockSnapshotV1 preSnap{};
			preSnap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
			preSnap.structSize = sizeof(preSnap);

			double startSeconds = 0.0;
			uint64_t computedTargetFrames = 0;
			if (isLockstep)
			{
				if (!host_.queryClock || !host_.queryClock(clk) ||
					!clk.getSnapshot || !clk.acquireRecordingLease || !clk.releaseRecordingLease ||
					!clk.recordingBegin || !clk.recordingStop || !clk.notifyFrameRendered)
				{
					failStart("GakuMotion 时钟服务不可用");
					return;
				}
				if (clk.getSnapshot(clk.context, &preSnap) != GAKU_OK || preSnap.timelineDurationSeconds <= 0.0)
				{
					failStart("GakuMotion 当前未加载曲目");
					return;
				}

				if (cfg.recordingMode == "lockstep_range")
				{
					startSeconds = (std::max)(0.0, cfg.rangeStartSeconds);
					const double endSeconds = (std::min)(cfg.rangeEndSeconds, preSnap.timelineDurationSeconds);
					if (endSeconds <= startSeconds)
					{
						failStart("区间录制起止时间无效");
						return;
					}
					const double dur = endSeconds - startSeconds;
					computedTargetFrames = static_cast<uint64_t>(
						(std::max<int64_t>)(1, std::llround(dur * static_cast<double>(cfg.fps))));
				}
				else
				{
					startSeconds = 0.0;
					computedTargetFrames = static_cast<uint64_t>(
						(std::max<int64_t>)(1, std::llround(preSnap.timelineDurationSeconds * static_cast<double>(cfg.fps))));
				}
			}

			const size_t frameBytes = static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height) * 4u;
			if (!AllocateRingSlots(frameBytes))
			{
				failStart("无法分配 4 槽位帧缓冲区 (" + std::to_string(frameBytes / (1024 * 1024)) + " MB/槽)");
				return;
			}

			const std::string resolvedCodec = FfmpegPipeline::ResolveEffectiveCodec(cfg.codec, probe);
			const std::string ext = ContainerExtensionForCodec(resolvedCodec);
			std::filesystem::path finalPath = explicitOutPath;
			if (finalPath.empty())
			{
				const std::filesystem::path outDir(util::Utf8ToWide(cfg.outputDir));
				const std::string rawTitle = isLockstep
					? SafeCString(preSnap.trackTitle, sizeof(preSnap.trackTitle))
					: std::string("FreeRun");
				const std::string baseTitle = util::SanitizeFilenameUtf8(rawTitle, "GakuVideo");
				const std::string stamp = util::LocalTimestampCompact();
				const std::string fileName = baseTitle + "_" + stamp + "_" +
					std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + "_" +
					std::to_string(cfg.fps) + "fps" + ext;
				finalPath = outDir / util::Utf8ToWide(fileName);
			}

			std::filesystem::path tempVideoPath = finalPath;
			tempVideoPath += L".stagea.tmp" + util::Utf8ToWide(ext);
			std::filesystem::path tempWavPath = finalPath;
			tempWavPath += L".audio.tmp.wav";

			CapturePipeOptions pipeOpts;
			pipeOpts.width = cfg.width;
			pipeOpts.height = cfg.height;
			pipeOpts.fpsNum = cfg.fps;
			pipeOpts.fpsDen = 1;
			pipeOpts.requestedCodec = cfg.codec;
			pipeOpts.pixelFormat = cfg.pixelFormat;
			pipeOpts.crf = cfg.crf;
			pipeOpts.tempVideoPath = tempVideoPath;
			pipeOpts.ffmpegPathOverride = cfg.ffmpegPathOverride;

			std::string actualCodec;
			std::string pipeErr;
			if (!ffmpeg_.StartCapturePipe(pipeOpts, probe, actualCodec, pipeErr))
			{
				FreeRingSlots();
				failStart(pipeErr);
				return;
			}

			uint64_t leaseId = 0;
			bool leaseHeld = false;
			uint64_t lastSerial = 0;
			bool sawPlaying = false;
			AudioTrackInfo audioInfo{};
			audioInfo.startSeconds = startSeconds;

			if (isLockstep)
			{
				const GakuStatus leaseStatus = clk.acquireRecordingLease(
					clk.context, "VideoExporter", cfg.fps, 1u, &leaseId);
				if (leaseStatus != GAKU_OK || leaseId == 0)
				{
					std::string ignored;
					ffmpeg_.FinishCapturePipe(true, ignored);
					std::error_code ec;
					std::filesystem::remove(tempVideoPath, ec);
					FreeRingSlots();
					failStart("无法获取 GakuMotion 录制锁 (status=" + std::to_string(leaseStatus) + ")");
					return;
				}
				leaseHeld = true;

				const GakuStatus beginStatus = clk.recordingBegin(clk.context, leaseId, startSeconds);
				if (beginStatus != GAKU_OK)
				{
					clk.releaseRecordingLease(clk.context, leaseId);
					std::string ignored;
					ffmpeg_.FinishCapturePipe(true, ignored);
					std::error_code ec;
					std::filesystem::remove(tempVideoPath, ec);
					FreeRingSlots();
					failStart("GakuMotion recordingBegin 失败 (status=" + std::to_string(beginStatus) + ")");
					return;
				}

				GakuMotionClockSnapshotV1 postSnap{};
				postSnap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
				postSnap.structSize = sizeof(postSnap);
				if (clk.getSnapshot(clk.context, &postSnap) == GAKU_OK)
				{
					if (postSnap.evaluatedFrameSerial != preSnap.evaluatedFrameSerial)
					{
						lastSerial = preSnap.evaluatedFrameSerial;
					}
					else if (postSnap.waitingForRenderComplete != 0 && postSnap.evaluatedFrameSerial != 0)
					{
						lastSerial = postSnap.evaluatedFrameSerial - 1;
					}
					else
					{
						lastSerial = postSnap.evaluatedFrameSerial;
					}
					if (postSnap.playbackState != 0) sawPlaying = true;
					ExtractAudioTrackInfo(postSnap, audioInfo);
				}
				else
				{
					lastSerial = preSnap.evaluatedFrameSerial;
					ExtractAudioTrackInfo(preSnap, audioInfo);
				}
			}

			const bool canUsePresentPath =
				HasPresentCaptureApi(ss) &&
				ResolveScreenshotAlphaMode(cfg) == GAKU_SCREENSHOT_ALPHA_OPAQUE &&
				!IsAlphaCodec(actualCodec) &&
				ssCaps.windowWidth == cfg.width &&
				ssCaps.windowHeight == cfg.height;

			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				screenshot_ = ss;
				clock_ = clk;
				activeLockstep_ = isLockstep;
				leaseId_ = leaseId;
				leaseHeld_ = leaseHeld;
				lastCapturedSerial_ = lastSerial;
				hasCapturedUnityFrame_ = false;
				lastCapturedUnityFrame_ = 0;
				sameUnityFrameCaptures_ = 0;
				usePresentPath_ = canUsePresentPath;
				awaitingPresentToken_ = 0;
				awaitingPresentPoseSerial_ = 0;
				nextPresentToken_ = 1;
				sawClockPlaying_ = sawPlaying;
				ticksWithoutEndOfFrame_ = 0;
				deferredFallbackLogged_ = false;
				notifyErrorLogged_ = false;
				targetFrames_ = computedTargetFrames;
				capturedFrames_.store(0, std::memory_order_release);
				writtenFrames_.store(0, std::memory_order_release);
				lastEncodedFrames_ = 0;
				lastElapsedSec_ = 0.0;
				lastFps_ = 0.0;
				captureStartTime_ = std::chrono::steady_clock::now();
				resolvedCodec_ = actualCodec;
				tempVideoPath_ = tempVideoPath;
				tempWavPath_ = tempWavPath;
				finalOutputPath_ = finalPath;
				audioInfo_ = audioInfo;
				phase_ = RecorderPhase::Recording;
				phaseMessage_ = isLockstep ? "正在锁步录制" : "正在自由录制";
				writerThread_ = std::thread(&VideoRecorder::WriterLoop, this);
			}

			Log(GAKU_LOG_INFO, "[VideoExporter] Started recording: " + util::WideToUtf8(finalPath.wstring()) +
				" (" + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + " @ " +
				std::to_string(cfg.fps) + "fps, codec=" + actualCodec +
				", capture=" + (canUsePresentPath ? "swapchain-present" : "offscreen-camera") + ")");
		}

		static bool HasPresentCaptureApi(const GakuScreenshotServiceV1& ss) noexcept
		{
			return ss.structSize >= offsetof(GakuScreenshotServiceV1, cancelPresentCapture) + sizeof(ss.cancelPresentCapture) &&
				ss.armPresentCapture && ss.consumePresentCapture && ss.cancelPresentCapture;
		}

		static void ExtractAudioTrackInfo(const GakuMotionClockSnapshotV1& snap, AudioTrackInfo& out)
		{
			out.hasAudio = snap.hasAudio != 0;
			out.audioFilePath = SafeCString(snap.audioFilePath, sizeof(snap.audioFilePath));
			out.activeAudioEffectId = SafeCString(snap.activeAudioEffectId, sizeof(snap.activeAudioEffectId));
			out.audioOffsetSeconds = std::isfinite(snap.audioOffsetSeconds) ? snap.audioOffsetSeconds : 0.0;
			out.audioSpeed = (std::isfinite(snap.audioSpeed) && snap.audioSpeed > 0.0f) ? snap.audioSpeed : 1.0;
			out.playbackSpeed = (std::isfinite(snap.playbackSpeed) && snap.playbackSpeed > 0.0f) ? snap.playbackSpeed : 1.0;
			out.musicVolume = std::isfinite(snap.musicVolume) ? snap.musicVolume : 1.0f;
		}

		bool EvaluateLockstepGate(const GakuMotionClockServiceV1& clk, uint64_t leaseId,
			bool isLockstep, uint64_t lastSerial, bool& sawPlaying, uint64_t targetFrames,
			std::chrono::steady_clock::time_point captureStart, GakuMotionClockSnapshotV1& outSnap)
		{
			outSnap = {};
			outSnap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
			outSnap.structSize = sizeof(outSnap);
			if (!isLockstep) return true;

			const uint64_t curCaptured = capturedFrames_.load(std::memory_order_acquire);
			if (targetFrames > 0 && curCaptured >= targetFrames)
			{
				FinishRecordingCapture(false, false, "");
				return false;
			}
			if (clk.getSnapshot(clk.context, &outSnap) != GAKU_OK)
			{
				FinishRecordingCapture(true, true, "无法读取 GakuMotion 时钟快照");
				return false;
			}
			if (outSnap.leaseId != leaseId)
			{
				FinishRecordingCapture(true, true, "GakuMotion 录制锁已丢失");
				return false;
			}
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (audioInfo_.audioFilePath.empty() && outSnap.audioFilePath[0] != '\0')
				{
					ExtractAudioTrackInfo(outSnap, audioInfo_);
				}
			}
			if (outSnap.playbackState != 0)
			{
				sawPlaying = true;
				std::lock_guard<std::mutex> lock(stateMutex_);
				sawClockPlaying_ = true;
			}
			else if (sawPlaying)
			{
				FinishRecordingCapture(false, false, "");
				return false;
			}
			if (outSnap.evaluatedFrameSerial == lastSerial)
			{
				if (capturedFrames_.load(std::memory_order_acquire) == 0 &&
					std::chrono::steady_clock::now() - captureStart > std::chrono::seconds(8))
				{
					FinishRecordingCapture(true, true, "等待 GakuMotion 首帧姿态或进入录制播放状态超时");
				}
				return false;
			}
			return true;
		}

		bool AcquireFreeRingSlot(bool isLockstep, size_t& outWriteIndex)
		{
			std::unique_lock<std::mutex> ringLock(ringMutex_);
			if (ringCount_ == kRingSlots)
			{
				if (isLockstep)
				{
					ringLock.unlock();
					std::lock_guard<std::mutex> stLock(stateMutex_);
					++ringOverflowStalls_;
					return false;
				}
				ringNotFullCv_.wait_for(ringLock, std::chrono::milliseconds(50), [&]() {
					return ringCount_ < kRingSlots || writerStopRequested_ || writerError_.load(std::memory_order_acquire);
				});
				if (ringCount_ == kRingSlots || writerStopRequested_)
				{
					ringLock.unlock();
					std::lock_guard<std::mutex> stLock(stateMutex_);
					++ringOverflowStalls_;
					return false;
				}
			}
			outWriteIndex = ringWriteIdx_;
			return true;
		}

		bool CommitCapturedFrame(size_t writeIndex, const RecorderConfig& cfg,
			const GakuScreenshotFrameInfoV1& info, bool isLockstep,
			const GakuMotionClockServiceV1& clk, uint64_t leaseId, uint64_t targetFrames,
			uint64_t serialForCommit)
		{
			const uint32_t expectedRowBytes = cfg.width * 4u;
			FrameSlot& slot = ringSlots_[writeIndex];
			if (info.width != cfg.width || info.height != cfg.height || info.rowPitch < expectedRowBytes ||
				static_cast<size_t>(info.rowPitch) * static_cast<size_t>(info.height) > slot.data.size())
			{
				FinishRecordingCapture(true, true, "ScreenshotManager 返回了不匹配的帧尺寸或步幅");
				return false;
			}
			slot.width = info.width;
			slot.height = info.height;
			slot.rowPitch = info.rowPitch;

			{
				std::lock_guard<std::mutex> ringLock(ringMutex_);
				ringWriteIdx_ = (ringWriteIdx_ + 1) % kRingSlots;
				++ringCount_;
			}
			ringNotEmptyCv_.notify_one();

			const uint64_t newCount = capturedFrames_.fetch_add(1, std::memory_order_acq_rel) + 1;
			if (info.unityFrame != 0)
			{
				bool warnSameFrame = false;
				{
					std::lock_guard<std::mutex> lock(stateMutex_);
					if (hasCapturedUnityFrame_ && info.unityFrame == lastCapturedUnityFrame_)
					{
						++sameUnityFrameCaptures_;
						warnSameFrame = (sameUnityFrameCaptures_ == 1);
					}
					lastCapturedUnityFrame_ = info.unityFrame;
					hasCapturedUnityFrame_ = true;
				}
				if (warnSameFrame)
				{
					Log(GAKU_LOG_WARNING,
						"[VideoExporter] Duplicate unityFrame observed during capture (frame=" +
						std::to_string(info.unityFrame) + ")");
				}
			}
			if (isLockstep)
			{
				{
					std::lock_guard<std::mutex> lock(stateMutex_);
					lastCapturedSerial_ = serialForCommit;
				}
				const GakuStatus notifySt = clk.notifyFrameRendered(clk.context, leaseId, serialForCommit, 1u);
				if (notifySt != GAKU_OK && !notifyErrorLogged_)
				{
					notifyErrorLogged_ = true;
					Log(GAKU_LOG_WARNING,
						"[VideoExporter] notifyFrameRendered returned status=" + std::to_string(notifySt) +
						" (serial=" + std::to_string(serialForCommit) + ")");
				}
				if (targetFrames > 0 && newCount >= targetFrames)
				{
					FinishRecordingCapture(false, false, "");
				}
			}
			return true;
		}

		// Returns true when caller may proceed (no pending frame still blocking this frame).
		bool PollPendingPresentCapture(bool fromAfterRender)
		{
			RecorderConfig cfg;
			GakuScreenshotServiceV1 ss{};
			GakuMotionClockServiceV1 clk{};
			bool isLockstep = false;
			uint64_t leaseId = 0;
			uint64_t targetFrames = 0;
			uint64_t token = 0;
			uint64_t poseSerial = 0;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (phase_ != RecorderPhase::Recording || !usePresentPath_) return true;
				if (awaitingPresentToken_ == 0) return true;
				cfg = sessionConfig_;
				ss = screenshot_;
				clk = clock_;
				isLockstep = activeLockstep_;
				leaseId = leaseId_;
				targetFrames = targetFrames_;
				token = awaitingPresentToken_;
				poseSerial = awaitingPresentPoseSerial_;
			}

			size_t writeIndex = 0;
			if (!AcquireFreeRingSlot(isLockstep, writeIndex)) return false;

			FrameSlot& slot = ringSlots_[writeIndex];
			GakuScreenshotFrameInfoV1 info{};
			info.abiVersion = GAKU_SCREENSHOT_VERSION;
			info.structSize = sizeof(info);
			uint64_t gotToken = 0;
			char errBuf[256]{};
			const GakuStatus st = ss.consumePresentCapture(
				ss.context, &gotToken, slot.data.data(), slot.data.size(), &info, errBuf, sizeof(errBuf));
			if (st != GAKU_OK)
			{
				{
					std::lock_guard<std::mutex> lock(stateMutex_);
					usePresentPath_ = false;
					awaitingPresentToken_ = 0;
				}
				Log(GAKU_LOG_WARNING,
					std::string("[VideoExporter] Swapchain Present capture unavailable (") +
					(errBuf[0] ? errBuf : std::to_string(st)) + "); falling back to offscreen camera render.");
				return true;
			}
			if (gotToken == 0)
			{
				return false; // Still awaiting Present; do not arm next frame yet.
			}

			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				awaitingPresentToken_ = 0;
			}

			uint64_t commitSerial = poseSerial;
			if (isLockstep && clk.getSnapshot)
			{
				GakuMotionClockSnapshotV1 curSnap{};
				curSnap.abiVersion = GAKU_MOTION_CLOCK_VERSION;
				curSnap.structSize = sizeof(curSnap);
				if (clk.getSnapshot(clk.context, &curSnap) == GAKU_OK && curSnap.evaluatedFrameSerial != 0)
					commitSerial = curSnap.evaluatedFrameSerial;
			}
			CommitCapturedFrame(writeIndex, cfg, info, isLockstep, clk, leaseId, targetFrames, commitSerial);
			// If consumed in OnEndOfFrameCapture, wait for the next MotionActorTick + render before arming again.
			return !fromAfterRender;
		}

		void TryArmPresentCapture()
		{
			RecorderConfig cfg;
			GakuScreenshotServiceV1 ss{};
			GakuMotionClockServiceV1 clk{};
			bool isLockstep = false;
			uint64_t leaseId = 0;
			uint64_t targetFrames = 0;
			uint64_t lastSerial = 0;
			bool sawPlaying = false;
			std::chrono::steady_clock::time_point captureStart{};
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (phase_ != RecorderPhase::Recording || !usePresentPath_ || awaitingPresentToken_ != 0)
					return;
				cfg = sessionConfig_;
				ss = screenshot_;
				clk = clock_;
				isLockstep = activeLockstep_;
				leaseId = leaseId_;
				targetFrames = targetFrames_;
				lastSerial = lastCapturedSerial_;
				sawPlaying = sawClockPlaying_;
				captureStart = captureStartTime_;
			}

			GakuMotionClockSnapshotV1 snap{};
			if (!EvaluateLockstepGate(clk, leaseId, isLockstep, lastSerial, sawPlaying, targetFrames, captureStart, snap))
				return;

			size_t unusedWriteIdx = 0;
			if (!AcquireFreeRingSlot(isLockstep, unusedWriteIdx)) return;

			uint64_t nextToken = 0;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				nextToken = nextPresentToken_++;
			}
			const GakuStatus armSt = ss.armPresentCapture(
				ss.context, nextToken, cfg.width, cfg.height, GAKU_PIXEL_RGBA8_SRGB_TOPDOWN);
			if (armSt == GAKU_OK)
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				awaitingPresentToken_ = nextToken;
				awaitingPresentPoseSerial_ = snap.evaluatedFrameSerial;
				return;
			}

			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				usePresentPath_ = false;
				awaitingPresentToken_ = 0;
			}
			Log(GAKU_LOG_WARNING,
				"[VideoExporter] armPresentCapture unavailable (status=" + std::to_string(armSt) +
				"); falling back to offscreen camera render.");
			TickActiveRecording();
		}

		void TickActiveRecording()
		{
			if (writerError_.load(std::memory_order_acquire))
			{
				std::string wErr;
				{
					std::lock_guard<std::mutex> rl(ringMutex_);
					wErr = writerErrorMsg_;
				}
				FinishRecordingCapture(true, true, wErr.empty() ? "FFmpeg 写入线程异常" : wErr);
				return;
			}

			RecorderConfig cfg;
			GakuScreenshotServiceV1 ss{};
			GakuMotionClockServiceV1 clk{};
			bool isLockstep = false;
			uint64_t leaseId = 0;
			uint64_t targetFrames = 0;
			uint64_t lastSerial = 0;
			bool sawPlaying = false;
			std::chrono::steady_clock::time_point captureStart{};
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (phase_ != RecorderPhase::Recording) return;
				cfg = sessionConfig_;
				ss = screenshot_;
				clk = clock_;
				isLockstep = activeLockstep_;
				leaseId = leaseId_;
				targetFrames = targetFrames_;
				lastSerial = lastCapturedSerial_;
				sawPlaying = sawClockPlaying_;
				captureStart = captureStartTime_;
			}

			GakuMotionClockSnapshotV1 snap{};
			if (!EvaluateLockstepGate(clk, leaseId, isLockstep, lastSerial, sawPlaying, targetFrames, captureStart, snap))
			{
				return;
			}

			size_t writeIndex = 0;
			if (!AcquireFreeRingSlot(isLockstep, writeIndex))
			{
				return;
			}

			FrameSlot& slot = ringSlots_[writeIndex];
			GakuScreenshotFrameRequestV1 req{};
			req.abiVersion = GAKU_SCREENSHOT_VERSION;
			req.structSize = sizeof(req);
			req.width = cfg.width;
			req.height = cfg.height;
			req.pixelFormat = GAKU_PIXEL_RGBA8_SRGB_TOPDOWN;
			req.alphaMode = ResolveScreenshotAlphaMode(cfg);

			GakuScreenshotFrameInfoV1 info{};
			info.abiVersion = GAKU_SCREENSHOT_VERSION;
			info.structSize = sizeof(info);
			char errBuf[256]{};

			const GakuStatus capStatus = ss.captureFrameImmediate(
				ss.context, &req, slot.data.data(), slot.data.size(), &info, errBuf, sizeof(errBuf));
			if (capStatus != GAKU_OK)
			{
				const std::string detail = errBuf[0] ? std::string(errBuf)
					: ("captureFrameImmediate failed (status=" + std::to_string(capStatus) + ")");
				FinishRecordingCapture(true, true, detail);
				return;
			}
			CommitCapturedFrame(writeIndex, cfg, info, isLockstep, clk, leaseId, targetFrames, snap.evaluatedFrameSerial);
		}

		void WriterLoop()
		{
			std::vector<uint8_t> compactScratch;
			for (;;)
			{
				size_t idx = 0;
				{
					std::unique_lock<std::mutex> ringLock(ringMutex_);
					ringNotEmptyCv_.wait(ringLock, [&]() {
						return ringCount_ > 0 || writerStopRequested_;
					});
					if (ringCount_ == 0 && writerStopRequested_) break;
					idx = ringReadIdx_;
				}

				const FrameSlot& slot = ringSlots_[idx];
				const uint32_t tightRow = slot.width * 4u;
				const size_t tightTotal = static_cast<size_t>(tightRow) * static_cast<size_t>(slot.height);
				const uint8_t* writePtr = slot.data.data();

				if (slot.rowPitch != tightRow)
				{
					compactScratch.resize(tightTotal);
					for (uint32_t y = 0; y < slot.height; ++y)
					{
						std::memcpy(
							compactScratch.data() + static_cast<size_t>(y) * tightRow,
							slot.data.data() + static_cast<size_t>(y) * slot.rowPitch,
							tightRow);
					}
					writePtr = compactScratch.data();
				}

				std::string writeErr;
				const bool ok = ffmpeg_.WriteFrame(writePtr, tightTotal, writeErr);

				{
					std::lock_guard<std::mutex> ringLock(ringMutex_);
					ringReadIdx_ = (ringReadIdx_ + 1) % kRingSlots;
					--ringCount_;
					if (!ok)
					{
						writerErrorMsg_ = writeErr;
						writerError_.store(true, std::memory_order_release);
					}
				}
				ringNotFullCv_.notify_all();

				if (!ok) break;
				writtenFrames_.fetch_add(1, std::memory_order_acq_rel);
			}
		}

		void FinishRecordingCapture(bool abortEarly, bool isError, const std::string& errorMessage)
		{
			GakuMotionClockServiceV1 clk{};
			bool releaseLease = false;
			uint64_t leaseId = 0;

			RecorderConfig cfg;
			FfmpegProbeResult probe;
			std::string resolvedCodec;
			std::filesystem::path tempVideoPath;
			std::filesystem::path tempWavPath;
			std::filesystem::path finalOutputPath;
			AudioTrackInfo audioInfo;
			uint64_t captured = 0;
			double elapsedSec = 0.0;
			std::thread writerToJoin;

			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				if (phase_ != RecorderPhase::Recording) return;

				if (HasPresentCaptureApi(screenshot_) && awaitingPresentToken_ != 0)
				{
					screenshot_.cancelPresentCapture(screenshot_.context);
				}
				usePresentPath_ = false;
				awaitingPresentToken_ = 0;
				awaitingPresentPoseSerial_ = 0;

				if (leaseHeld_)
				{
					clk = clock_;
					leaseId = leaseId_;
					releaseLease = true;
					leaseHeld_ = false;
					leaseId_ = 0;
				}

				captureEndTime_ = std::chrono::steady_clock::now();
				elapsedSec = (std::max)(0.001, std::chrono::duration<double>(captureEndTime_ - captureStartTime_).count());
				captured = capturedFrames_.load(std::memory_order_acquire);
				lastElapsedSec_ = elapsedSec;
				lastFps_ = captured > 0 ? static_cast<double>(captured) / elapsedSec : 0.0;

				cfg = sessionConfig_;
				probe = sessionProbe_;
				resolvedCodec = resolvedCodec_;
				tempVideoPath = tempVideoPath_;
				tempWavPath = tempWavPath_;
				finalOutputPath = finalOutputPath_;
				audioInfo = audioInfo_;
				writerToJoin = std::move(writerThread_);

				if (abortEarly && isError)
				{
					lastError_ = errorMessage;
				}
				phase_ = RecorderPhase::PostProcessing;
				phaseMessage_ = abortEarly ? "正在清理录制资源" : "正在进行 FFmpeg 后期合成与音频混流";
			}

			// Always release GakuMotion clock lease promptly on the Unity main thread.
			if (releaseLease)
			{
				if (clk.recordingStop) clk.recordingStop(clk.context, leaseId);
				if (clk.releaseRecordingLease) clk.releaseRecordingLease(clk.context, leaseId);
			}

			// Query optional GakuAudio service for Stage B DSP baking.
			GakuAudioServiceV1 audioService{};
			bool hasAudioService = false;
			if (!abortEarly && host_.queryAudio && host_.queryAudio(audioService))
			{
				hasAudioService = true;
			}

			if (abortEarly)
			{
				ffmpeg_.AbortPipe();
			}

			std::lock_guard<std::mutex> lock(stateMutex_);
			JoinFinishedPostProcessLocked();
			postProcessThread_ = std::thread([this,
				writer = std::move(writerToJoin),
				abortEarly,
				isError,
				errorMessage,
				cfg,
				probe,
				resolvedCodec,
				tempVideoPath,
				tempWavPath,
				finalOutputPath,
				audioInfo,
				captured,
				hasAudioService,
				audioService]() mutable {
				{
					std::lock_guard<std::mutex> rl(ringMutex_);
					writerStopRequested_ = true;
				}
				ringNotEmptyCv_.notify_all();
				ringNotFullCv_.notify_all();
				if (writer.joinable()) writer.join();
				FreeRingSlots();

				std::string stageAErr;
				const bool stageAOk = ffmpeg_.FinishCapturePipe(abortEarly, stageAErr);

				if (abortEarly || !stageAOk || captured == 0)
				{
					std::error_code ec;
					std::filesystem::remove(tempVideoPath, ec);
					std::filesystem::remove(tempWavPath, ec);

					std::lock_guard<std::mutex> stLock(stateMutex_);
					lastEncodedFrames_ = (std::max)(ffmpeg_.EncodedFrames(), writtenFrames_.load(std::memory_order_acquire));
					if (isError || (!abortEarly && !stageAOk))
					{
						phase_ = RecorderPhase::Error;
						lastError_ = !errorMessage.empty() ? errorMessage : stageAErr;
						phaseMessage_ = "录制失败";
					}
					else if (!abortEarly && captured == 0)
					{
						phase_ = RecorderPhase::Idle;
						phaseMessage_ = "未捕获到有效帧";
					}
					else
					{
						phase_ = RecorderPhase::Idle;
						phaseMessage_ = "录制已中止";
					}
					return;
				}

				PostProcessOptions postOpts;
				postOpts.config = cfg;
				postOpts.resolvedCodec = resolvedCodec;
				postOpts.tempVideoPath = tempVideoPath;
				postOpts.finalOutputPath = finalOutputPath;
				postOpts.tempWavPath = tempWavPath;
				postOpts.audio = audioInfo;
				postOpts.audio.recordedDurationSeconds = static_cast<double>(captured) / static_cast<double>(cfg.fps);
				postOpts.audioService = hasAudioService ? &audioService : nullptr;

				std::string postErr;
				const bool postOk = FfmpegPipeline::RunPostProcess(postOpts, probe, &cancelRequested_, postErr);

				std::lock_guard<std::mutex> stLock(stateMutex_);
				lastEncodedFrames_ = (std::max)(ffmpeg_.EncodedFrames(), writtenFrames_.load(std::memory_order_acquire));
				if (postOk)
				{
					phase_ = RecorderPhase::Completed;
					lastOutputPath_ = util::WideToUtf8(finalOutputPath.wstring());
					lastError_.clear();
					phaseMessage_ = "导出完成";
					Log(GAKU_LOG_INFO, "[VideoExporter] Exported video: " + lastOutputPath_);
				}
				else
				{
					phase_ = RecorderPhase::Error;
					lastError_ = postErr;
					phaseMessage_ = "后期处理失败";
					Log(GAKU_LOG_ERROR, "[VideoExporter] Post-process failed: " + postErr);
				}
			});
		}

		ServiceHostCallbacks host_;

		mutable std::mutex stateMutex_;
		std::filesystem::path configPath_;
		std::string defaultOutputDir_{"Videos"};
		RecorderConfig config_;
		FfmpegProbeResult ffmpegProbe_;
		bool ffmpegProbedOnce_{false};
		std::filesystem::path outputPathOverride_;

		RecorderPhase phase_{RecorderPhase::Idle};
		bool pendingStart_{false};
		bool pendingStop_{false};
		std::atomic<bool> cancelRequested_{false};
		std::string phaseMessage_;
		std::string lastOutputPath_;
		std::string lastError_;

		RecorderConfig sessionConfig_;
		FfmpegProbeResult sessionProbe_;
		GakuScreenshotServiceV1 screenshot_{};
		GakuMotionClockServiceV1 clock_{};
		static constexpr uint64_t kMaxTicksWithoutEndOfFrame = 4;
		std::atomic<bool> deferredCaptureMode_{false};
		uint64_t ticksWithoutEndOfFrame_{0};
		bool deferredFallbackLogged_{false};
		bool notifyErrorLogged_{false};
		bool activeLockstep_{false};
		uint64_t leaseId_{0};
		bool leaseHeld_{false};
		uint64_t lastCapturedSerial_{0};
		bool hasCapturedUnityFrame_{false};
		uint64_t lastCapturedUnityFrame_{0};
		uint64_t sameUnityFrameCaptures_{0};
		std::atomic<bool> usePresentPath_{false};
		uint64_t awaitingPresentToken_{0};
		uint64_t awaitingPresentPoseSerial_{0};
		uint64_t nextPresentToken_{1};
		bool sawClockPlaying_{false};
		uint64_t targetFrames_{0};
		uint64_t ringOverflowStalls_{0};
		std::atomic<uint64_t> capturedFrames_{0};
		std::atomic<uint64_t> writtenFrames_{0};
		uint64_t lastEncodedFrames_{0};
		double lastElapsedSec_{0.0};
		double lastFps_{0.0};
		std::chrono::steady_clock::time_point startRequestedTime_{};
		std::chrono::steady_clock::time_point captureStartTime_{};
		std::chrono::steady_clock::time_point captureEndTime_{};
		std::string resolvedCodec_;
		std::filesystem::path tempVideoPath_;
		std::filesystem::path tempWavPath_;
		std::filesystem::path finalOutputPath_;
		AudioTrackInfo audioInfo_;

		FfmpegPipeline ffmpeg_;
		std::thread writerThread_;
		std::thread postProcessThread_;

		std::mutex ringMutex_;
		std::condition_variable ringNotEmptyCv_;
		std::condition_variable ringNotFullCv_;
		std::array<FrameSlot, kRingSlots> ringSlots_;
		size_t ringReadIdx_{0};
		size_t ringWriteIdx_{0};
		size_t ringCount_{0};
		bool writerStopRequested_{false};
		std::atomic<bool> writerError_{false};
		std::string writerErrorMsg_;
	};
} // namespace VideoExporter
