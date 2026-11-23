// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "../include/GakuAudio.h"
#include "../include/GakuMotionClock.h"
#include "../include/GakuPaths.hpp"
#include "../include/GakuPlugin.h"
#include "../include/GakuPluginMetadata.h"
#include "../include/GakuScreenshot.h"
#include "../include/GakuWebPanel.h"

#include "CommonUtils.hpp"
#include "VideoExporterWebPanel.hpp"
#include "VideoRecorder.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	const char* const kPluginMaintainers[] = {
		"crested-ezios",
	};

	const GakuPluginLocalizedTextV1 kPluginLocalizedSummaries[] = {
		{"en", "4K lockstep video recorder and FFmpeg compositing plugin for GakuLoader."},
		{"zh-Hans", "面向 GakuLoader 的 4K 锁步视频录制与 FFmpeg 后期合成插件。"},
		{"zh-Hant", "面向 GakuLoader 的 4K 鎖步影片錄製與 FFmpeg 後期合成外掛。"},
		{"ja", "GakuLoader 向けの 4K ロックステップ録画および FFmpeg 合成プラグイン。"},
	};

	const GakuPluginDependencyV1 kPluginDependencies[] = {
		{"screenshotmanager", "ScreenshotManager", ">=0.1.0", GAKU_DEPENDENCY_REQUIRED, 0u},
		{"gakumotion", "GakuMotion", ">=0.1.1", GAKU_DEPENDENCY_OPTIONAL, 0u},
		{"gakuaudio", "GakuAudio", ">=0.1.0", GAKU_DEPENDENCY_OPTIONAL, 0u},
	};

	void ModuleAnchor() {}

	std::wstring GetModuleDirectory()
	{
		HMODULE mod = nullptr;
		if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ModuleAnchor), &mod) || !mod)
		{
			return {};
		}
		wchar_t buf[32768]{};
		const DWORD len = GetModuleFileNameW(mod, buf, static_cast<DWORD>(std::size(buf)));
		if (len == 0 || len >= std::size(buf)) return {};
		return std::filesystem::path(buf, buf + len).parent_path().wstring();
	}

	struct PluginContext
	{
		GakuHostApi host{};
		GakuLogService log{};
		GakuRuntimeService runtime{};
		GakuMainThreadService mainThread{};
		GakuAppearanceService appearance{};
		GakuFileDialogService fileDialog{};
		GakuHotkeyService hotkeys{};
		GakuPathsV1 paths{};

		std::mutex serviceCacheMutex;
		bool hasScreenshot{false};
		bool screenshotUnavailable{false};
		GakuScreenshotServiceV1 screenshot{};
		bool hasClock{false};
		bool clockUnavailable{false};
		GakuMotionClockServiceV1 clock{};
		bool hasAudio{false};
		bool audioUnavailable{false};
		GakuAudioServiceV1 audio{};
		uint8_t screenshotDiagLogged{0}; // 0=none, 1=active, 2=unavailable
		uint8_t clockDiagLogged{0};
		uint8_t audioDiagLogged{0};
		bool hasRenderFrame{false};
		GakuRenderFrameService renderFrame{};
		bool renderFrameRegistered{false};
		bool hasRenderFrameStatus{false};
		GakuRenderFrameStatusService renderFrameStatus{};

		std::mutex dialogMutex;
		uint64_t dialogTicket{0};

		std::atomic<bool> textInputActive{false};
		bool hotkeyRegistered{false};
		std::wstring uiAssetsDir;

		std::unique_ptr<VideoExporter::VideoRecorder> recorder;
		std::unique_ptr<VideoExporter::webpanel::Bridge> bridge;
	};

	void WriteLog(PluginContext* ctx, GakuLogLevel level, const std::string& msg)
	{
		if (!ctx || !ctx->log.write) return;
		ctx->log.write(ctx->log.context, level, msg.c_str());
	}

	bool QueryScreenshotLazy(PluginContext* ctx, GakuScreenshotServiceV1& out)
	{
		if (!ctx) return false;
		std::lock_guard<std::mutex> lock(ctx->serviceCacheMutex);
		if (ctx->hasScreenshot)
		{
			out = ctx->screenshot;
			return true;
		}
		if (ctx->screenshotUnavailable) return false;
		if (!ctx->host.queryService) return false;
		GakuScreenshotServiceV1 tbl{};
		const GakuStatus st = ctx->host.queryService(
			ctx->host.hostContext, GAKU_SERVICE_SCREENSHOT_CAPTURE, GAKU_SCREENSHOT_VERSION, sizeof(tbl), &tbl);
		if (st != GAKU_OK) return false;
		if (tbl.abiVersion != GAKU_SCREENSHOT_VERSION || tbl.structSize < sizeof(GakuScreenshotServiceV1) ||
			!tbl.getCapabilities || !tbl.captureFrameImmediate)
		{
			return false;
		}
		ctx->screenshot = tbl;
		ctx->hasScreenshot = true;
		out = tbl;
		return true;
	}

	bool QueryClockLazy(PluginContext* ctx, GakuMotionClockServiceV1& out)
	{
		if (!ctx) return false;
		std::lock_guard<std::mutex> lock(ctx->serviceCacheMutex);
		if (ctx->hasClock)
		{
			out = ctx->clock;
			return true;
		}
		if (ctx->clockUnavailable) return false;
		if (!ctx->host.queryService) return false;
		GakuMotionClockServiceV1 tbl{};
		const GakuStatus st = ctx->host.queryService(
			ctx->host.hostContext, GAKU_SERVICE_MOTION_CLOCK, GAKU_MOTION_CLOCK_VERSION, sizeof(tbl), &tbl);
		if (st != GAKU_OK) return false;
		if (tbl.abiVersion != GAKU_MOTION_CLOCK_VERSION || tbl.structSize < sizeof(GakuMotionClockServiceV1) ||
			!tbl.getSnapshot || !tbl.acquireRecordingLease || !tbl.releaseRecordingLease ||
			!tbl.recordingBegin || !tbl.recordingStop || !tbl.notifyFrameRendered)
		{
			return false;
		}
		ctx->clock = tbl;
		ctx->hasClock = true;
		out = tbl;
		return true;
	}

	bool QueryAudioLazy(PluginContext* ctx, GakuAudioServiceV1& out)
	{
		if (!ctx) return false;
		std::lock_guard<std::mutex> lock(ctx->serviceCacheMutex);
		if (ctx->hasAudio)
		{
			out = ctx->audio;
			return true;
		}
		if (ctx->audioUnavailable) return false;
		if (!ctx->host.queryService) return false;
		GakuAudioServiceV1 tbl{};
		const GakuStatus st = ctx->host.queryService(
			ctx->host.hostContext, GAKU_SERVICE_AUDIO_CORE, GAKU_AUDIO_VERSION, sizeof(tbl), &tbl);
		if (st != GAKU_OK) return false;
		if (tbl.abiVersion != GAKU_AUDIO_VERSION || tbl.structSize < sizeof(GakuAudioServiceV1))
		{
			return false;
		}
		ctx->audio = tbl;
		ctx->hasAudio = true;
		out = tbl;
		return true;
	}

	bool IsUnavailablePluginState(GakuPluginLoadState state) noexcept
	{
		return state == GAKU_PLUGIN_STATE_NOT_FOUND ||
			state == GAKU_PLUGIN_STATE_DISABLED ||
			state == GAKU_PLUGIN_STATE_DEPENDENCY_ERROR ||
			state == GAKU_PLUGIN_STATE_INCOMPATIBLE ||
			state == GAKU_PLUGIN_STATE_FAILED;
	}

	const char* PluginLoadStateLabel(GakuPluginLoadState state) noexcept
	{
		switch (state)
		{
		case GAKU_PLUGIN_STATE_NOT_FOUND: return "not installed";
		case GAKU_PLUGIN_STATE_DISABLED: return "disabled";
		case GAKU_PLUGIN_STATE_DEPENDENCY_ERROR: return "dependency error";
		case GAKU_PLUGIN_STATE_INCOMPATIBLE: return "incompatible";
		case GAKU_PLUGIN_STATE_QUERIED: return "queried";
		case GAKU_PLUGIN_STATE_ACTIVATED: return "activated";
		case GAKU_PLUGIN_STATE_RUNTIME_READY: return "runtime-ready";
		case GAKU_PLUGIN_STATE_FAILED: return "failed";
		default: return "unknown";
		}
	}

	void InspectPluginDependencies(PluginContext* ctx)
	{
		if (!ctx || !ctx->host.queryService) return;

		GakuPluginsServiceV1 plugins{};
		const GakuStatus st = ctx->host.queryService(
			ctx->host.hostContext, GAKU_SERVICE_PLUGINS, GAKU_PLUGINS_SERVICE_VERSION, sizeof(plugins), &plugins);
		if (st != GAKU_OK ||
			plugins.abiVersion != GAKU_PLUGINS_SERVICE_VERSION ||
			plugins.structSize < sizeof(GakuPluginsServiceV1) ||
			!plugins.findPlugin)
		{
			return;
		}

		struct DependencySpec
		{
			const char* id;
			bool required;
			const char* impactNote;
			bool PluginContext::* hasService;
			bool PluginContext::* unavailableFlag;
			uint8_t PluginContext::* diagLogged;
		};

		static constexpr DependencySpec kDeps[] = {
			{"screenshotmanager", true, "frame capture unavailable",
				&PluginContext::hasScreenshot, &PluginContext::screenshotUnavailable, &PluginContext::screenshotDiagLogged},
			{"gakumotion", false, "lockstep clock disabled (freerun mode still available)",
				&PluginContext::hasClock, &PluginContext::clockUnavailable, &PluginContext::clockDiagLogged},
			{"gakuaudio", false, "offline DSP audio baking skipped",
				&PluginContext::hasAudio, &PluginContext::audioUnavailable, &PluginContext::audioDiagLogged},
		};

		struct PendingLog
		{
			GakuLogLevel level;
			std::string message;
		};
		std::vector<PendingLog> logs;
		std::string activeSummary;

		for (const auto& dep : kDeps)
		{
			GakuPluginSummaryV1 summary{};
			if (plugins.findPlugin(dep.id, sizeof(summary), &summary) != GAKU_OK) continue;
			if (summary.abiVersion != GAKU_PLUGINS_SERVICE_VERSION ||
				summary.structSize < sizeof(GakuPluginSummaryV1))
			{
				continue;
			}

			const bool unavailable = IsUnavailablePluginState(summary.state);
			const bool active =
				summary.state == GAKU_PLUGIN_STATE_ACTIVATED ||
				summary.state == GAKU_PLUGIN_STATE_RUNTIME_READY;
			if (!unavailable && !active) continue;

			std::lock_guard<std::mutex> lock(ctx->serviceCacheMutex);
			if (unavailable)
			{
				if (!(ctx->*dep.hasService))
				{
					ctx->*dep.unavailableFlag = true;
				}
				if (ctx->*dep.diagLogged != 2u)
				{
					ctx->*dep.diagLogged = 2u;
					logs.push_back({
						dep.required ? GAKU_LOG_WARNING : GAKU_LOG_INFO,
						std::string("[VideoExporter] ") +
							(dep.required ? "Required" : "Optional") +
							" dependency " + dep.id + " is " + PluginLoadStateLabel(summary.state) +
							"; " + dep.impactNote + "."
					});
				}
			}
			else if (active && ctx->*dep.diagLogged == 0u)
			{
				ctx->*dep.diagLogged = 1u;
				if (!activeSummary.empty()) activeSummary += ", ";
				const char* ver = (summary.version[0] != '\0') ? summary.version : "unknown";
				activeSummary += std::string(dep.id) + " v" + ver + " (" + PluginLoadStateLabel(summary.state) + ")";
			}
		}

		if (!activeSummary.empty())
		{
			WriteLog(ctx, GAKU_LOG_INFO, "[VideoExporter] Dependencies via gakuloader.plugins: " + activeSummary + ".");
		}
		for (const auto& entry : logs)
		{
			WriteLog(ctx, entry.level, entry.message);
		}
	}

	void MainThreadWakeTask(void* userData)
	{
		auto* ctx = static_cast<PluginContext*>(userData);
		if (ctx && ctx->recorder)
		{
			// Process session start/stop only; frame capture runs in OnTick /
			// post-render so DrainTasks + OnTick never double-capture in one tick.
			ctx->recorder->ProcessPendingControl();
		}
	}

	void MainThreadAfterRenderTask(void* userData, int isNormalOuterFrame)
	{
		auto* ctx = static_cast<PluginContext*>(userData);
		if (ctx && ctx->recorder && isNormalOuterFrame)
		{
			ctx->recorder->OnEndOfFrameCapture();
		}
	}

	void EnsureRenderFrameRegistered(PluginContext* ctx)
	{
		if (!ctx || !ctx->host.queryService || !ctx->recorder) return;
		std::lock_guard<std::mutex> lock(ctx->serviceCacheMutex);
		if (ctx->renderFrameRegistered) return;
		if (!ctx->hasRenderFrame)
		{
			GakuRenderFrameService rf{};
			if (ctx->host.queryService(ctx->host.hostContext, GAKU_SERVICE_RENDER_FRAME,
				GAKU_SERVICE_VERSION, sizeof(rf), &rf) == GAKU_OK &&
				rf.abiVersion == GAKU_SERVICE_VERSION &&
				rf.structSize >= sizeof(GakuRenderFrameService) &&
				rf.registerObserver && rf.unregisterObserver)
			{
				ctx->renderFrame = rf;
				ctx->hasRenderFrame = true;
			}
		}
		if (!ctx->hasRenderFrameStatus)
		{
			GakuRenderFrameStatusService rfs{};
			if (ctx->host.queryService(ctx->host.hostContext, GAKU_SERVICE_RENDER_FRAME_STATUS,
				GAKU_SERVICE_VERSION, sizeof(rfs), &rfs) == GAKU_OK &&
				rfs.abiVersion == GAKU_SERVICE_VERSION &&
				rfs.structSize >= sizeof(GakuRenderFrameStatusService) &&
				rfs.getStatus)
			{
				ctx->renderFrameStatus = rfs;
				ctx->hasRenderFrameStatus = true;
			}
		}
		if (!ctx->hasRenderFrame) return;
		const GakuRenderFrameBinding binding{
			GAKU_SERVICE_VERSION,
			sizeof(GakuRenderFrameBinding),
			"videoexporter.render-frame",
			nullptr,
			&MainThreadAfterRenderTask,
			ctx
		};
		if (ctx->renderFrame.registerObserver(ctx->renderFrame.context, &binding) == GAKU_OK)
		{
			ctx->renderFrameRegistered = true;
			const bool hookUnavailable = ctx->hasRenderFrameStatus && ctx->renderFrameStatus.getStatus &&
				ctx->renderFrameStatus.getStatus() == GAKU_RENDER_FRAME_HOOK_UNAVAILABLE;
			ctx->recorder->SetDeferredCaptureMode(!hookUnavailable);
			WriteLog(ctx, GAKU_LOG_INFO, hookUnavailable
				? "[VideoExporter] gakuloader.render-frame bound, but host render hook is unavailable; using OnTick capture."
				: "[VideoExporter] Bound gakuloader.render-frame post-render capture.");
		}
	}

	bool IsRenderFrameHookUnavailable(PluginContext* ctx)
	{
		if (!ctx) return false;
		std::lock_guard<std::mutex> lock(ctx->serviceCacheMutex);
		if (!ctx->renderFrameRegistered) return true;
		if (!ctx->hasRenderFrameStatus || !ctx->renderFrameStatus.getStatus) return false;
		return ctx->renderFrameStatus.getStatus() == GAKU_RENDER_FRAME_HOOK_UNAVAILABLE;
	}

	void OnToggleRecordingHotkey(void* userData)
	{
		auto* ctx = static_cast<PluginContext*>(userData);
		if (!ctx || !ctx->recorder) return;
		std::string err;
		if (!ctx->recorder->ToggleRecording(err) && !err.empty())
		{
			WriteLog(ctx, GAKU_LOG_WARNING, "[VideoExporter] Hotkey toggle skipped: " + err);
		}
	}

	void ReleasePendingDialog(PluginContext* ctx)
	{
		if (!ctx) return;
		std::lock_guard<std::mutex> lock(ctx->dialogMutex);
		if (ctx->dialogTicket != 0 && ctx->fileDialog.release)
		{
			ctx->fileDialog.release(ctx->fileDialog.context, ctx->dialogTicket);
		}
		ctx->dialogTicket = 0;
	}

	bool BeginBrowseFfmpegDialog(PluginContext* ctx, std::string& notice)
	{
		notice.clear();
		if (!ctx || !ctx->fileDialog.beginOpen || !ctx->fileDialog.poll || !ctx->fileDialog.release)
		{
			notice = "GakuLoader 文件选择服务不可用，请手动填写 ffmpeg.exe 路径";
			return false;
		}
		std::lock_guard<std::mutex> lock(ctx->dialogMutex);
		if (ctx->dialogTicket != 0)
		{
			notice = "已有文件选择框正在等待响应";
			return false;
		}
		const GakuFileFilter filter{L"FFmpeg 可执行文件 (ffmpeg.exe)", L"ffmpeg.exe;*.exe"};
		const GakuFileDialogRequest req{
			GAKU_SERVICE_VERSION,
			sizeof(GakuFileDialogRequest),
			L"选择 ffmpeg.exe",
			1u,
			&filter
		};
		uint64_t ticket = 0;
		const GakuStatus st = ctx->fileDialog.beginOpen(ctx->fileDialog.context, &req, &ticket);
		if (st != GAKU_OK || ticket == 0)
		{
			notice = "打开文件选择框失败 (status=" + std::to_string(st) + ")";
			return false;
		}
		ctx->dialogTicket = ticket;
		return true;
	}

	int PollBrowseFfmpegDialog(PluginContext* ctx, std::string& pathUtf8, std::string& notice)
	{
		pathUtf8.clear();
		notice.clear();
		if (!ctx || !ctx->fileDialog.poll) return GAKU_FILE_DIALOG_FAILED;

		uint64_t ticket = 0;
		{
			std::lock_guard<std::mutex> lock(ctx->dialogMutex);
			ticket = ctx->dialogTicket;
		}
		if (ticket == 0) return GAKU_FILE_DIALOG_FAILED;

		GakuFileDialogResult res{};
		res.abiVersion = GAKU_SERVICE_VERSION;
		res.structSize = sizeof(res);
		const GakuStatus st = ctx->fileDialog.poll(ctx->fileDialog.context, ticket, sizeof(res), &res, 0, nullptr);
		if (st != GAKU_OK)
		{
			ReleasePendingDialog(ctx);
			notice = "轮询文件选择框失败";
			return GAKU_FILE_DIALOG_FAILED;
		}
		if (res.state == GAKU_FILE_DIALOG_PENDING)
		{
			return GAKU_FILE_DIALOG_PENDING;
		}
		if (res.state == GAKU_FILE_DIALOG_ACCEPTED && res.requiredBytes > 1)
		{
			std::string buf(res.requiredBytes, '\0');
			GakuFileDialogResult res2{};
			res2.abiVersion = GAKU_SERVICE_VERSION;
			res2.structSize = sizeof(res2);
			const GakuStatus st2 = ctx->fileDialog.poll(
				ctx->fileDialog.context, ticket, sizeof(res2), &res2,
				static_cast<uint32_t>(buf.size()), buf.data());
			ReleasePendingDialog(ctx);
			if (st2 == GAKU_OK && res2.state == GAKU_FILE_DIALOG_ACCEPTED)
			{
				const size_t nul = buf.find('\0');
				pathUtf8 = (nul == std::string::npos) ? buf : buf.substr(0, nul);
				return GAKU_FILE_DIALOG_ACCEPTED;
			}
			notice = "读取所选文件路径失败";
			return GAKU_FILE_DIALOG_FAILED;
		}
		const uint32_t finalState = res.state;
		ReleasePendingDialog(ctx);
		return static_cast<int>(finalState);
	}

	bool OpenFolderInExplorer(const std::string& folderUtf8, const std::string& fileToSelectUtf8, std::string& notice)
	{
		notice.clear();
		std::error_code ec;
		if (!fileToSelectUtf8.empty())
		{
			const std::filesystem::path filePath(VideoExporter::util::Utf8ToWide(fileToSelectUtf8));
			if (std::filesystem::is_regular_file(filePath, ec))
			{
				const std::wstring params = L"/select,\"" + filePath.wstring() + L"\"";
				const auto res = reinterpret_cast<INT_PTR>(
					ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL));
				if (res > 32) return true;
			}
		}

		const std::filesystem::path dirPath(VideoExporter::util::Utf8ToWide(folderUtf8));
		if (dirPath.empty())
		{
			notice = "输出目录路径为空";
			return false;
		}
		std::filesystem::create_directories(dirPath, ec);
		if (ec)
		{
			notice = "无法创建输出目录: " + ec.message();
			return false;
		}
		const auto res = reinterpret_cast<INT_PTR>(
			ShellExecuteW(nullptr, L"open", dirPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
		if (res <= 32)
		{
			notice = "无法在资源管理器中打开目录 (错误码 " + std::to_string(res) + ")";
			return false;
		}
		return true;
	}

	// ---- GakuWebPanelServiceV1 C ABI trampolines ----------------------------

	GakuStatus WebPanelGetAssets(void* context, GakuWebPanelAssetsV1* output)
	{
		auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context);
		if (!bridge) return GAKU_E_UNAVAILABLE;
		try { return bridge->GetAssets(output); }
		catch (...) { return GAKU_E_INTERNAL; }
	}
	void WebPanelOnShow(void* context)
	{
		if (auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context)) bridge->OnShow();
	}
	void WebPanelOnHide(void* context)
	{
		if (auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context)) bridge->OnHide();
	}
	void WebPanelOnUiTick(void* context)
	{
		if (auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context)) bridge->OnUiTick();
	}
	void WebPanelSetEventSink(void* context, GakuWebPanelEventSink sink, void* userData)
	{
		if (auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context)) bridge->SetEventSink(sink, userData);
	}
	GakuStatus WebPanelHandleMessage(void* context, const char* requestJson,
		uint32_t responseCapacity, char* responseJson, uint32_t* requiredBytes)
	{
		auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context);
		if (!bridge) return GAKU_E_UNAVAILABLE;
		return bridge->HandleMessage(requestJson, responseCapacity, responseJson, requiredBytes);
	}
	void WebPanelOnShutdown(void* context)
	{
		if (auto* bridge = static_cast<VideoExporter::webpanel::Bridge*>(context)) bridge->OnShutdown();
	}

	// ---- GakuPlugin lifecycle callbacks ------------------------------------

	GakuStatus Activate(const GakuHostApi* host, void** pluginState)
	{
		if (!host || !pluginState || host->abiVersion != GAKU_ABI_VERSION ||
			host->structSize < sizeof(GakuHostApi) || !host->queryService || !host->registerService)
		{
			return GAKU_E_INVALID_ARGUMENT;
		}

		auto ctx = std::make_unique<PluginContext>();
		ctx->host = *host;
		host->queryService(host->hostContext, GAKU_SERVICE_LOG, GAKU_SERVICE_VERSION, sizeof(ctx->log), &ctx->log);
		host->queryService(host->hostContext, GAKU_SERVICE_RUNTIME, GAKU_SERVICE_VERSION, sizeof(ctx->runtime), &ctx->runtime);
		host->queryService(host->hostContext, GAKU_SERVICE_MAIN_THREAD, GAKU_SERVICE_VERSION, sizeof(ctx->mainThread), &ctx->mainThread);
		host->queryService(host->hostContext, GAKU_SERVICE_APPEARANCE, GAKU_SERVICE_VERSION, sizeof(ctx->appearance), &ctx->appearance);
		host->queryService(host->hostContext, GAKU_SERVICE_FILE_DIALOG, GAKU_SERVICE_VERSION, sizeof(ctx->fileDialog), &ctx->fileDialog);
		host->queryService(host->hostContext, GAKU_SERVICE_HOTKEYS, GAKU_SERVICE_VERSION, sizeof(ctx->hotkeys), &ctx->hotkeys);

		std::filesystem::path configRoot;
		std::filesystem::path userDataRoot;
		if (GakuPaths::Query(host, ctx->paths) == GAKU_OK)
		{
			if (GakuPaths::GetRoot(ctx->paths, GAKU_PATH_CONFIGS, configRoot) == GAKU_OK)
			{
				ctx->paths.ensureRoot(ctx->paths.context, GAKU_PATH_CONFIGS);
			}
			GakuPaths::GetRoot(ctx->paths, GAKU_PATH_USER_DATA, userDataRoot);
		}
		if (configRoot.empty() && ctx->runtime.localRoot && ctx->runtime.localRoot[0])
		{
			configRoot = std::filesystem::path(ctx->runtime.localRoot) / L"Configs" / L"videoexporter";
		}
		std::filesystem::path defaultVideosDir;
		if (!userDataRoot.empty())
		{
			defaultVideosDir = userDataRoot / L"Videos";
		}
		else if (ctx->runtime.localRoot && ctx->runtime.localRoot[0])
		{
			defaultVideosDir = std::filesystem::path(ctx->runtime.localRoot) / L"Videos";
		}
		else
		{
			defaultVideosDir = std::filesystem::path(L"Videos");
		}

		const std::wstring modDir = GetModuleDirectory();
		if (!modDir.empty())
		{
			ctx->uiAssetsDir = (std::filesystem::path(modDir) / L"VideoExporter.UI").wstring();
		}

		PluginContext* rawCtx = ctx.get();
		VideoExporter::ServiceHostCallbacks recorderHost;
		recorderHost.queryScreenshot = [rawCtx](GakuScreenshotServiceV1& out) { return QueryScreenshotLazy(rawCtx, out); };
		recorderHost.queryClock = [rawCtx](GakuMotionClockServiceV1& out) { return QueryClockLazy(rawCtx, out); };
		recorderHost.queryAudio = [rawCtx](GakuAudioServiceV1& out) { return QueryAudioLazy(rawCtx, out); };
		recorderHost.wakeMainThread = [rawCtx]() {
			if (rawCtx->mainThread.enqueue)
			{
				rawCtx->mainThread.enqueue(rawCtx->mainThread.context, &MainThreadWakeTask, rawCtx);
			}
		};
		recorderHost.isRenderFrameHookUnavailable = [rawCtx]() { return IsRenderFrameHookUnavailable(rawCtx); };
		recorderHost.log = [rawCtx](GakuLogLevel level, const std::string& msg) { WriteLog(rawCtx, level, msg); };

		ctx->recorder = std::make_unique<VideoExporter::VideoRecorder>(std::move(recorderHost));
		const std::filesystem::path configFilePath = configRoot.empty()
			? std::filesystem::path{}
			: (configRoot / L"config.json");
		ctx->recorder->InitializePaths(configFilePath, VideoExporter::util::WideToUtf8(defaultVideosDir.wstring()));

		VideoExporter::webpanel::WebPanelHooks webHooks;
		webHooks.assetsDirs = [rawCtx]() -> std::vector<std::wstring> {
			if (rawCtx->uiAssetsDir.empty()) return {};
			return {rawCtx->uiAssetsDir};
		};
		webHooks.openFolder = [](const std::string& folder, const std::string& selectFile, std::string& notice) {
			return OpenFolderInExplorer(folder, selectFile, notice);
		};
		webHooks.beginBrowseFfmpeg = [rawCtx](std::string& notice) {
			return BeginBrowseFfmpegDialog(rawCtx, notice);
		};
		webHooks.pollBrowseFfmpeg = [rawCtx](std::string& path, std::string& notice) {
			return PollBrowseFfmpegDialog(rawCtx, path, notice);
		};
		webHooks.releaseBrowseFfmpeg = [rawCtx]() { ReleasePendingDialog(rawCtx); };
		webHooks.setTextInputActive = [rawCtx](bool active) {
			rawCtx->textInputActive.store(active, std::memory_order_release);
		};
		webHooks.getAppearance = [rawCtx](int& theme, float& uiScale, bool& ok) {
			theme = 0;
			uiScale = 1.0f;
			ok = false;
			if (!rawCtx->appearance.getAppearance) return;
			GakuUiAppearanceV1 app{};
			app.apiVersion = GAKU_SERVICE_VERSION;
			app.structSize = sizeof(app);
			if (rawCtx->appearance.getAppearance(sizeof(app), &app) == GAKU_OK)
			{
				theme = static_cast<int>(app.theme);
				uiScale = app.uiScale > 0.0f ? app.uiScale : 1.0f;
				ok = true;
			}
		};
		webHooks.pluginVersion = "0.1.0";

		ctx->bridge = std::make_unique<VideoExporter::webpanel::Bridge>(*ctx->recorder, std::move(webHooks));

		const GakuWebPanelServiceV1 webService{
			GAKU_WEB_PANEL_VERSION,
			sizeof(GakuWebPanelServiceV1),
			ctx->bridge.get(),
			&WebPanelGetAssets,
			&WebPanelOnShow,
			&WebPanelOnHide,
			&WebPanelOnUiTick,
			&WebPanelSetEventSink,
			&WebPanelHandleMessage,
			&WebPanelOnShutdown
		};
		const GakuStatus webReg = host->registerService(
			host->hostContext, "videoexporter" GAKU_WEB_PANEL_SERVICE_SUFFIX, sizeof(webService), &webService);
		if (webReg != GAKU_OK)
		{
			WriteLog(rawCtx, GAKU_LOG_WARNING,
				"[VideoExporter] Failed to register videoexporter.web-panel/v1 (status=" + std::to_string(webReg) + ")");
		}

		if (ctx->hotkeys.registerHotkey)
		{
			GakuHotkeyBinding hk{};
			hk.abiVersion = GAKU_ABI_VERSION;
			hk.structSize = sizeof(GakuHotkeyBinding);
			hk.actionId = "toggle-recording";
			hk.displayName = "VideoExporter: Start/Stop Recording";
			hk.virtualKey = VK_F12;
			hk.modifiers = 0;
			hk.callback = &OnToggleRecordingHotkey;
			hk.userData = rawCtx;
			const GakuStatus hkSt = ctx->hotkeys.registerHotkey(ctx->hotkeys.context, &hk);
			if (hkSt == GAKU_OK)
			{
				ctx->hotkeyRegistered = true;
			}
			else
			{
				WriteLog(rawCtx, GAKU_LOG_WARNING,
					"[VideoExporter] Hotkey toggle-recording registration skipped (status=" + std::to_string(hkSt) + ")");
			}
		}

		InspectPluginDependencies(rawCtx);
		WriteLog(rawCtx, GAKU_LOG_INFO, "[VideoExporter] Activated (v0.1.0).");
		*pluginState = ctx.release();
		return GAKU_OK;
	}

	void OnRuntimeReady(void* pluginState)
	{
		auto* ctx = static_cast<PluginContext*>(pluginState);
		if (!ctx) return;
		InspectPluginDependencies(ctx);
		GakuScreenshotServiceV1 ss{};
		GakuMotionClockServiceV1 clk{};
		GakuAudioServiceV1 aud{};
		QueryScreenshotLazy(ctx, ss);
		QueryClockLazy(ctx, clk);
		QueryAudioLazy(ctx, aud);
		EnsureRenderFrameRegistered(ctx);
	}

	void OnTick(void* pluginState, float /*deltaTime*/)
	{
		auto* ctx = static_cast<PluginContext*>(pluginState);
		if (!ctx || !ctx->recorder) return;
		ctx->recorder->OnTick();
	}

	void Shutdown(void* pluginState)
	{
		auto* ctx = static_cast<PluginContext*>(pluginState);
		if (!ctx) return;
		if (ctx->renderFrameRegistered && ctx->renderFrame.unregisterObserver)
		{
			ctx->renderFrame.unregisterObserver(ctx->renderFrame.context);
			ctx->renderFrameRegistered = false;
		}
		if (ctx->hotkeyRegistered && ctx->hotkeys.unregisterHotkey)
		{
			ctx->hotkeys.unregisterHotkey(ctx->hotkeys.context, "toggle-recording");
			ctx->hotkeyRegistered = false;
		}
		ReleasePendingDialog(ctx);
		if (ctx->bridge)
		{
			ctx->bridge->Invalidate();
		}
		if (ctx->recorder)
		{
			ctx->recorder->Shutdown();
		}
		delete ctx;
	}
} // namespace

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_Query(
	uint32_t requestedAbi, uint32_t outputSize, GakuPluginDescriptor* output)
{
	if (requestedAbi != GAKU_ABI_VERSION) return GAKU_E_VERSION;
	if (!output) return GAKU_E_INVALID_ARGUMENT;
	if (outputSize < sizeof(GakuPluginDescriptor)) return GAKU_E_SIZE;

	GakuPluginDescriptor desc{};
	desc.abiVersion = GAKU_ABI_VERSION;
	desc.structSize = sizeof(GakuPluginDescriptor);
	desc.id = "videoexporter";
	desc.name = "VideoExporter";
	desc.version = "0.1.0";
	desc.activate = &Activate;
	desc.onRuntimeReady = &OnRuntimeReady;
	desc.onTick = &OnTick;
	desc.onActorTick = nullptr;
	desc.onDesktopUi = nullptr;
	desc.shutdown = &Shutdown;
	*output = desc;
	return GAKU_OK;
}

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_QueryMetadata(
	uint32_t requestedAbi, uint32_t outputSize, GakuPluginMetadataV1* output)
{
	if (requestedAbi != GAKU_METADATA_ABI_VERSION) return GAKU_E_VERSION;
	if (!output) return GAKU_E_INVALID_ARGUMENT;
	if (outputSize < sizeof(GakuPluginMetadataV1)) return GAKU_E_SIZE;

	GakuPluginMetadataV1 meta{};
	meta.abiVersion = GAKU_METADATA_ABI_VERSION;
	meta.structSize = sizeof(GakuPluginMetadataV1);
	meta.originalAuthor = "VideoExporter Contributors";
	meta.maintainerCount = static_cast<uint32_t>(std::size(kPluginMaintainers));
	meta.maintainers = kPluginMaintainers;
	meta.subVersion = "";
	meta.releaseChannel = "stable";
	meta.summary = "4K lockstep video recorder and FFmpeg compositing plugin for GakuLoader.";
	meta.localizedSummaryCount = static_cast<uint32_t>(std::size(kPluginLocalizedSummaries));
	meta.localizedSummaries = kPluginLocalizedSummaries;
	meta.repositoryUrl = "https://github.com/crested-ezios/VideoExporter";
	meta.releaseUrl = "https://github.com/crested-ezios/VideoExporter/releases";
	meta.issuesUrl = "https://github.com/crested-ezios/VideoExporter/issues";
	meta.license = "MIT";
	meta.minLoaderVersion = "1.1.0";
	meta.dependencyCount = static_cast<uint32_t>(std::size(kPluginDependencies));
	meta.dependencies = kPluginDependencies;
	meta.auxiliaryDllCount = 0u;
	meta.auxiliaryDlls = nullptr;
	*output = meta;
	return GAKU_OK;
}
