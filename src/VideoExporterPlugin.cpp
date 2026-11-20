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
#include "../include/GakuScreenshot.h"
#include "../include/GakuWebPanel.h"

#include "CommonUtils.hpp"
#include "VideoExporterWebPanel.hpp"
#include "VideoRecorder.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
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
		GakuScreenshotServiceV1 screenshot{};
		bool hasClock{false};
		GakuMotionClockServiceV1 clock{};
		bool hasAudio{false};
		GakuAudioServiceV1 audio{};
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

		WriteLog(rawCtx, GAKU_LOG_INFO, "[VideoExporter] Activated (v0.1.0).");
		*pluginState = ctx.release();
		return GAKU_OK;
	}

	void OnRuntimeReady(void* pluginState)
	{
		auto* ctx = static_cast<PluginContext*>(pluginState);
		if (!ctx) return;
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
