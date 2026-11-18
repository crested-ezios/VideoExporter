// SPDX-License-Identifier: MIT
#pragma once

#include "../include/GakuPlugin.h"
#include "../include/GakuWebPanel.h"
#include "CommonUtils.hpp"
#include "Config.hpp"
#include "FfmpegPipeline.hpp"
#include "GkJsonLite.hpp"
#include "VideoRecorder.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace VideoExporter::webpanel
{
	struct WebPanelHooks
	{
		std::function<std::vector<std::wstring>()> assetsDirs;
		std::function<bool(const std::string& folderUtf8, const std::string& fileToSelectUtf8, std::string& notice)> openFolder;
		std::function<bool(std::string& notice)> beginBrowseFfmpeg;
		std::function<int(std::string& pathUtf8, std::string& notice)> pollBrowseFfmpeg;
		std::function<void()> releaseBrowseFfmpeg;
		std::function<void(bool active)> setTextInputActive;
		std::function<void(int& theme, float& uiScale, bool& ok)> getAppearance;
		std::string pluginVersion{"0.1.0"};
	};

	inline constexpr const char* kSupportedMethods[] = {
		"bridge.hello",
		"bridge.ping",
		"state.get",
		"recording.start",
		"recording.stop",
		"config.set",
		"folder.open",
		"ffmpeg.browse",
		"ffmpeg.probe",
		"dialog.poll",
		"appearance.get",
		"shell.setTextInputActive",
	};

	inline constexpr const char* kSupportedEvents[] = {
		"state.changed",
		"appearance.changed",
		"ffmpeg.browsed",
		"bridge.closed",
	};

	class Bridge
	{
	public:
		Bridge(VideoRecorder& recorder, WebPanelHooks hooks)
			: recorder_(recorder), hooks_(std::move(hooks))
		{
			sessionId_ = NextSessionId();
		}

		uint64_t SessionId() const noexcept { return sessionId_; }
		bool Closed() const noexcept { return closed_.load(std::memory_order_acquire); }

		GakuStatus GetAssets(GakuWebPanelAssetsV1* output)
		{
			if (!output) return GAKU_E_INVALID_ARGUMENT;
			if (closed_.load(std::memory_order_acquire)) return GAKU_E_SHUTTING_DOWN;
			if (!hooks_.assetsDirs) return GAKU_E_UNAVAILABLE;

			for (const std::wstring& dir : hooks_.assetsDirs())
			{
				if (dir.empty()) continue;
				std::error_code ec;
				const std::filesystem::path entry = std::filesystem::path(dir) / L"index.html";
				if (!std::filesystem::is_regular_file(entry, ec) || ec) continue;
				assetsDirCache_ = dir;
				output->abiVersion = GAKU_WEB_PANEL_VERSION;
				output->structSize = sizeof(GakuWebPanelAssetsV1);
				output->assetsDir = assetsDirCache_.c_str();
				output->entryPath = L"index.html";
				output->virtualHost = L"videoexporter.invalid";
				return GAKU_OK;
			}
			return GAKU_E_UNAVAILABLE;
		}

		void OnShow()
		{
			visible_ = true;
			baselinePending_ = true;
		}

		void OnHide()
		{
			visible_ = false;
			if (hooks_.setTextInputActive) hooks_.setTextInputActive(false);
		}

		void SetEventSink(GakuWebPanelEventSink sink, void* userData)
		{
			std::lock_guard<std::mutex> lock(sinkMutex_);
			sink_ = sink;
			sinkUserData_ = userData;
			if (sink) baselinePending_ = true;
		}

		GakuStatus HandleMessage(const char* requestJson, uint32_t responseCapacity,
			char* responseJson, uint32_t* requiredBytes)
		{
			if (!requestJson) return GAKU_E_INVALID_ARGUMENT;
			std::string response;
			try
			{
				response = DispatchRequest(requestJson);
			}
			catch (...)
			{
				response = MakeError(0, "internal_error", "Unhandled VideoExporter bridge exception");
			}
			const uint64_t needed = static_cast<uint64_t>(response.size()) + 1ull;
			if (needed > 0xFFFFFFFFull) return GAKU_E_LIMIT;
			if (requiredBytes) *requiredBytes = static_cast<uint32_t>(needed);
			if (!responseJson || responseCapacity < needed) return GAKU_E_SIZE;
			std::memcpy(responseJson, response.c_str(), static_cast<size_t>(needed));
			return GAKU_OK;
		}

		void OnUiTick()
		{
			if (closed_.load(std::memory_order_acquire)) return;
			try
			{
				Tick();
			}
			catch (...)
			{
			}
		}

		void OnShutdown()
		{
			if (closed_.load(std::memory_order_acquire)) return;
			Emit("bridge.closed", "{\"reason\":\"service-shutdown\"}");
			if (browsePending_ && hooks_.releaseBrowseFfmpeg) hooks_.releaseBrowseFfmpeg();
			browsePending_ = false;
			visible_ = false;
			if (hooks_.setTextInputActive) hooks_.setTextInputActive(false);
			{
				std::lock_guard<std::mutex> lock(sinkMutex_);
				sink_ = nullptr;
				sinkUserData_ = nullptr;
				sessionId_ = NextSessionId();
				seq_ = 0;
			}
			baselinePending_ = true;
			lastStateJson_.clear();
			lastAppearanceJson_.clear();
			lastPhase_ = RecorderPhase::Idle;
		}

		void Invalidate()
		{
			closed_.store(true, std::memory_order_release);
			browsePending_ = false;
			if (hooks_.releaseBrowseFfmpeg) hooks_.releaseBrowseFfmpeg();
			std::lock_guard<std::mutex> lock(sinkMutex_);
			sink_ = nullptr;
			sinkUserData_ = nullptr;
		}

		std::string HandleMessageForTest(const std::string& request)
		{
			return DispatchRequest(request.c_str());
		}

		static std::string FfmpegProbeToJson(const FfmpegProbeResult& p)
		{
			std::string out = "{";
			out += "\"available\":" + util::JBool(p.available);
			out += ",\"nvencRuntimeVerified\":" + util::JBool(p.nvencRuntimeVerified);
			out += ",\"ffmpegPath\":" + util::JString(p.ffmpegPathUtf8);
			out += ",\"ffprobePath\":" + util::JString(p.ffprobePathUtf8);
			out += ",\"version\":" + util::JString(p.version);
			out += ",\"error\":" + util::JString(p.error);
			out += ",\"encoders\":{";
			out += "\"h264_nvenc\":" + util::JBool(p.encoders.h264_nvenc);
			out += ",\"hevc_nvenc\":" + util::JBool(p.encoders.hevc_nvenc);
			out += ",\"libx264\":" + util::JBool(p.encoders.libx264);
			out += ",\"libx265\":" + util::JBool(p.encoders.libx265);
			out += ",\"prores_ks\":" + util::JBool(p.encoders.prores_ks);
			out += ",\"libvpx-vp9\":" + util::JBool(p.encoders.libvpx_vp9);
			out += "}}";
			return out;
		}

	private:
		struct CmdResult
		{
			bool ok{true};
			std::string body{"{}"};
			std::string code;
			std::string message;

			static CmdResult Ok(std::string result = "{}")
			{
				CmdResult r;
				r.body = std::move(result);
				return r;
			}
			static CmdResult Err(std::string code_, std::string message_)
			{
				CmdResult r;
				r.ok = false;
				r.code = std::move(code_);
				r.message = std::move(message_);
				return r;
			}
		};

		static uint64_t NextSessionId()
		{
			static std::atomic<uint64_t> counter{0};
			return ++counter;
		}

		static std::string MakeOk(uint64_t id, const std::string& result)
		{
			return "{\"v\":1,\"type\":\"response\",\"id\":" + util::JUInt(id) +
				",\"ok\":true,\"result\":" + result + "}";
		}

		static std::string MakeError(uint64_t id, const std::string& code, const std::string& message)
		{
			return "{\"v\":1,\"type\":\"response\",\"id\":" + util::JUInt(id) +
				",\"ok\":false,\"error\":{\"code\":" + util::JString(code) +
				",\"message\":" + util::JString(message) + "}}";
		}

		void Emit(const std::string& name, const std::string& payloadJson)
		{
			std::lock_guard<std::mutex> lock(sinkMutex_);
			if (!sink_ || closed_.load(std::memory_order_acquire)) return;
			const std::string json = "{\"v\":1,\"type\":\"event\",\"name\":" + util::JString(name) +
				",\"payload\":" + payloadJson + ",\"seq\":" + util::JUInt(++seq_) +
				",\"sid\":" + util::JUInt(sessionId_) + "}";
			sink_(sinkUserData_, json.c_str());
		}

		std::string DispatchRequest(const char* requestJson)
		{
			GakuMotion::jsonlite::Value root;
			std::string parseErr;
			if (!GakuMotion::jsonlite::ParseText(requestJson, root, parseErr) || !root.IsObject())
			{
				return MakeError(0, "invalid_json", "Malformed request JSON");
			}
			const auto* version = root.Find("v");
			const auto* type = root.Find("type");
			const auto* idVal = root.Find("id");
			const auto* methodVal = root.Find("method");

			uint64_t id = 0;
			if (idVal && idVal->IsNumber() && idVal->numberValue >= 0.0)
			{
				id = static_cast<uint64_t>(idVal->numberValue);
			}
			if (!version || !version->IsNumber() || version->numberValue != 1.0 ||
				!type || !type->IsString() || type->stringValue != "request" ||
				!idVal || !idVal->IsNumber() ||
				!methodVal || !methodVal->IsString() || methodVal->stringValue.empty())
			{
				return MakeError(id, "invalid_request", "Request must carry v=1, type=request, numeric id, and method");
			}
			if (closed_.load(std::memory_order_acquire))
			{
				return MakeError(id, "shutting_down", "VideoExporter bridge is closed");
			}
			const auto* params = root.Find("params");
			if (params && !params->IsObject() && !params->IsNull())
			{
				return MakeError(id, "invalid_params", "params must be an object");
			}
			static const GakuMotion::jsonlite::Value emptyParams{[] {
				GakuMotion::jsonlite::Value v;
				v.type = GakuMotion::jsonlite::Type::Object;
				return v;
			}()};

			CmdResult res;
			try
			{
				res = DispatchMethod(methodVal->stringValue, params && params->IsObject() ? *params : emptyParams);
			}
			catch (...)
			{
				res = CmdResult::Err("internal_error", "Unhandled method exception");
			}
			if (res.ok) return MakeOk(id, res.body);
			return MakeError(id, res.code, res.message);
		}

		CmdResult DispatchMethod(const std::string& method, const GakuMotion::jsonlite::Value& params)
		{
			if (method == "bridge.hello")
			{
				return CmdResult::Ok(BuildHelloJson());
			}
			if (method == "bridge.ping")
			{
				return CmdResult::Ok("{\"pong\":true}");
			}
			if (method == "state.get")
			{
				return CmdResult::Ok(BuildFullStateJson());
			}
			if (method == "appearance.get")
			{
				return CmdResult::Ok(BuildAppearanceJson());
			}
			if (method == "shell.setTextInputActive")
			{
				const auto* activeVal = params.Find("active");
				if (!activeVal || !activeVal->IsBool())
				{
					return CmdResult::Err("invalid_params", "active must be a boolean");
				}
				if (hooks_.setTextInputActive) hooks_.setTextInputActive(activeVal->boolValue);
				return CmdResult::Ok("{\"active\":" + util::JBool(activeVal->boolValue) + "}");
			}
			if (method == "config.set")
			{
				const auto* cfgObj = params.Find("config");
				const GakuMotion::jsonlite::Value& patch = (cfgObj && cfgObj->IsObject()) ? *cfgObj : params;
				std::string err;
				if (!recorder_.UpdateConfig(patch, err))
				{
					return CmdResult::Err("invalid_params", err);
				}
				forceStateEvent_ = true;
				return CmdResult::Ok("{\"config\":" + ConfigToJson(recorder_.GetConfig()) + "}");
			}
			if (method == "recording.start")
			{
				if (const auto* cfgObj = params.Find("config"); cfgObj && cfgObj->IsObject())
				{
					std::string cfgErr;
					if (!recorder_.UpdateConfig(*cfgObj, cfgErr))
					{
						return CmdResult::Err("invalid_params", cfgErr);
					}
				}
				std::string err;
				if (!recorder_.RequestStart(err))
				{
					return CmdResult::Err("not_ready", err);
				}
				forceStateEvent_ = true;
				const auto status = recorder_.GetStatus();
				return CmdResult::Ok("{\"started\":true,\"status\":" + StatusToJson(status) + "}");
			}
			if (method == "recording.stop")
			{
				std::string err;
				if (!recorder_.RequestStop(err))
				{
					return CmdResult::Err("not_ready", err);
				}
				forceStateEvent_ = true;
				const auto status = recorder_.GetStatus();
				return CmdResult::Ok("{\"stopping\":true,\"status\":" + StatusToJson(status) + "}");
			}
			if (method == "folder.open")
			{
				const auto cfg = recorder_.GetConfig();
				const auto status = recorder_.GetStatus();
				std::string folder = cfg.outputDir;
				std::string fileToSelect;
				if (const auto* p = params.Find("path"); p && p->IsString() && !p->stringValue.empty())
				{
					folder = p->stringValue;
				}
				if (const auto* sel = params.Find("selectLastOutput"); sel && sel->IsBool() && sel->boolValue)
				{
					fileToSelect = status.lastOutputPath;
				}
				if (!hooks_.openFolder)
				{
					return CmdResult::Err("not_implemented", "Host folder open callback is unavailable");
				}
				std::string notice;
				if (!hooks_.openFolder(folder, fileToSelect, notice))
				{
					return CmdResult::Err("failed", notice.empty() ? "无法打开导出目录" : notice);
				}
				return CmdResult::Ok("{\"opened\":true,\"path\":" + util::JString(folder) + "}");
			}
			if (method == "ffmpeg.probe")
			{
				FfmpegProbeResult probe;
				if (const auto* p = params.Find("path"); p && p->IsString())
				{
					std::string err;
					if (!recorder_.SetFfmpegPathAndProbe(p->stringValue, probe, err))
					{
						return CmdResult::Err("busy", err);
					}
				}
				else
				{
					probe = recorder_.ProbeFfmpeg();
				}
				forceStateEvent_ = true;
				return CmdResult::Ok("{\"ffmpeg\":" + FfmpegProbeToJson(probe) + "}");
			}
			if (method == "ffmpeg.browse")
			{
				if (!hooks_.beginBrowseFfmpeg || !hooks_.pollBrowseFfmpeg)
				{
					return CmdResult::Err("not_implemented", "宿主文件选择服务不可用，请手动输入 ffmpeg.exe 路径");
				}
				if (browsePending_)
				{
					return CmdResult::Err("busy", "已有文件选择框正在等待响应");
				}
				std::string notice;
				if (!hooks_.beginBrowseFfmpeg(notice))
				{
					return CmdResult::Err("failed", notice.empty() ? "无法打开文件选择框" : notice);
				}
				browsePending_ = true;
				++browseDialogId_;
				browseState_ = "pending";
				browsePath_.clear();
				browseNotice_.clear();
				PollBrowseDialog();
				std::string body = "{\"dialogId\":" + util::JUInt(browseDialogId_) +
					",\"state\":" + util::JString(browseState_) +
					",\"path\":" + util::JString(browsePath_) +
					",\"notice\":" + util::JString(browseNotice_) + "}";
				return CmdResult::Ok(body);
			}
			if (method == "dialog.poll")
			{
				const auto* did = params.Find("dialogId");
				if (!did || !did->IsNumber() || static_cast<uint64_t>(did->numberValue) != browseDialogId_)
				{
					return CmdResult::Err("invalid_params", "dialogId does not match the active browse request");
				}
				PollBrowseDialog();
				return CmdResult::Ok("{\"state\":" + util::JString(browseState_) +
					",\"path\":" + util::JString(browsePath_) +
					",\"notice\":" + util::JString(browseNotice_) + "}");
			}
			return CmdResult::Err("unknown_method", "Unknown method: " + method);
		}

		void PollBrowseDialog()
		{
			if (!browsePending_ || !hooks_.pollBrowseFfmpeg) return;
			std::string path, notice;
			const int st = hooks_.pollBrowseFfmpeg(path, notice);
			if (st == GAKU_FILE_DIALOG_PENDING) return;

			browsePending_ = false;
			browseNotice_ = notice;
			if (st == GAKU_FILE_DIALOG_ACCEPTED && !path.empty())
			{
				browseState_ = "accepted";
				browsePath_ = path;
				FfmpegProbeResult probe;
				std::string err;
				recorder_.SetFfmpegPathAndProbe(path, probe, err);
				forceStateEvent_ = true;
			}
			else if (st == GAKU_FILE_DIALOG_CANCELLED)
			{
				browseState_ = "cancelled";
				browsePath_.clear();
			}
			else
			{
				browseState_ = "failed";
				browsePath_.clear();
			}

			Emit("ffmpeg.browsed", "{\"dialogId\":" + util::JUInt(browseDialogId_) +
				",\"state\":" + util::JString(browseState_) +
				",\"path\":" + util::JString(browsePath_) +
				",\"notice\":" + util::JString(browseNotice_) + "}");
		}

		void Tick()
		{
			PollBrowseDialog();
			if (!visible_ && !baselinePending_) return;

			const std::string appearance = BuildAppearanceJson();
			const RecorderStatusSnapshot status = recorder_.GetStatus();
			const std::string stateJson = BuildFullStateJsonWithStatus(status);

			const bool baseline = baselinePending_;
			baselinePending_ = false;

			if (baseline || appearance != lastAppearanceJson_)
			{
				lastAppearanceJson_ = appearance;
				Emit("appearance.changed", appearance);
			}

			const auto now = std::chrono::steady_clock::now();
			const bool active = status.busy;
			const bool phaseChanged = (status.phase != lastPhase_);
			const bool throttled = active && !baseline && !forceStateEvent_ && !phaseChanged &&
				(now - lastStateEmit_ < std::chrono::milliseconds(100));

			if (!throttled && (baseline || forceStateEvent_ || phaseChanged || stateJson != lastStateJson_))
			{
				lastStateJson_ = stateJson;
				lastPhase_ = status.phase;
				lastStateEmit_ = now;
				forceStateEvent_ = false;
				Emit("state.changed", stateJson);
			}
		}

		std::string BuildHelloJson() const
		{
			std::string caps = "[";
			for (size_t i = 0; i < std::size(kSupportedMethods); ++i)
			{
				if (i) caps.push_back(',');
				caps += util::JString(kSupportedMethods[i]);
			}
			caps += "]";

			std::string evs = "[";
			for (size_t i = 0; i < std::size(kSupportedEvents); ++i)
			{
				if (i) evs.push_back(',');
				evs += util::JString(kSupportedEvents[i]);
			}
			evs += "]";

			return "{\"protocol\":1,\"sessionId\":" + util::JUInt(sessionId_) +
				",\"pluginId\":\"videoexporter\",\"pluginVersion\":" + util::JString(hooks_.pluginVersion) +
				",\"capabilities\":" + caps + ",\"events\":" + evs + "}";
		}

		std::string BuildAppearanceJson() const
		{
			int theme = 0;
			float uiScale = 1.0f;
			bool ok = false;
			if (hooks_.getAppearance) hooks_.getAppearance(theme, uiScale, ok);
			return "{\"theme\":" + util::JNum(theme) + ",\"uiScale\":" + util::JNum(uiScale) + "}";
		}

		static std::string StatusToJson(const RecorderStatusSnapshot& s)
		{
			std::string out = "{";
			out += "\"phase\":" + util::JString(PhaseName(s.phase));
			out += ",\"recording\":" + util::JBool(s.recording);
			out += ",\"busy\":" + util::JBool(s.busy);
			out += ",\"capturedFrames\":" + util::JUInt(s.capturedFrames);
			out += ",\"encodedFrames\":" + util::JUInt(s.encodedFrames);
			out += ",\"targetFrames\":" + util::JUInt(s.targetFrames);
			out += ",\"fps\":" + util::JNum(s.fps);
			out += ",\"elapsedSec\":" + util::JNum(s.elapsedSec);
			out += ",\"progress\":" + util::JNum(s.progress);
			out += ",\"ringOverflowStalls\":" + util::JUInt(s.ringOverflowStalls);
			out += ",\"phaseMessage\":" + util::JString(s.phaseMessage);
			out += ",\"lastOutputPath\":" + util::JString(s.lastOutputPath);
			out += ",\"lastError\":" + util::JString(s.lastError);
			out += "}";
			return out;
		}

		std::string BuildFullStateJson()
		{
			return BuildFullStateJsonWithStatus(recorder_.GetStatus());
		}

		std::string BuildFullStateJsonWithStatus(const RecorderStatusSnapshot& status)
		{
			const RecorderConfig cfg = recorder_.GetConfig();
			const FfmpegProbeResult ff = recorder_.GetFfmpegProbe();
			ScreenshotStatusSnapshot ss{};
			ClockStatusSnapshot clk{};
			AudioStatusSnapshot aud{};
			recorder_.QueryLiveServices(ss, clk, aud);

			std::string out = "{";
			out += "\"config\":" + ConfigToJson(cfg);
			out += ",\"status\":" + StatusToJson(status);
			out += ",\"services\":{";
			out += "\"screenshot\":{";
			out += "\"available\":" + util::JBool(ss.available);
			out += ",\"runtimeReady\":" + util::JBool(ss.runtimeReady);
			out += ",\"cameraAvailable\":" + util::JBool(ss.cameraAvailable);
			out += ",\"greenScreenDetected\":" + util::JBool(ss.greenScreenDetected);
			out += ",\"maxDimension\":" + util::JUInt(ss.maxDimension);
			out += ",\"windowWidth\":" + util::JUInt(ss.windowWidth);
			out += ",\"windowHeight\":" + util::JUInt(ss.windowHeight);
			out += "}";
			out += ",\"clock\":{";
			out += "\"available\":" + util::JBool(clk.available);
			out += ",\"playbackState\":" + util::JUInt(clk.playbackState);
			out += ",\"runMode\":" + util::JUInt(clk.runMode);
			out += ",\"currentTimeSeconds\":" + util::JNum(clk.currentTimeSeconds);
			out += ",\"timelineDurationSeconds\":" + util::JNum(clk.timelineDurationSeconds);
			out += ",\"hasAudio\":" + util::JBool(clk.hasAudio);
			out += ",\"trackTitle\":" + util::JString(clk.trackTitle);
			out += ",\"audioFilePath\":" + util::JString(clk.audioFilePath);
			out += ",\"activeAudioEffectId\":" + util::JString(clk.activeAudioEffectId);
			out += "}";
			out += ",\"audio\":{";
			out += "\"available\":" + util::JBool(aud.available);
			out += ",\"activePresetId\":" + util::JString(aud.activePresetId);
			out += "}";
			out += ",\"ffmpeg\":" + FfmpegProbeToJson(ff);
			out += "}";
			out += ",\"appearance\":" + BuildAppearanceJson();
			out += "}";
			return out;
		}

		VideoRecorder& recorder_;
		WebPanelHooks hooks_;
		uint64_t sessionId_{0};
		uint64_t seq_{0};
		std::atomic<bool> closed_{false};
		bool visible_{false};
		bool baselinePending_{true};
		bool forceStateEvent_{false};
		mutable std::mutex sinkMutex_;
		GakuWebPanelEventSink sink_{nullptr};
		void* sinkUserData_{nullptr};
		std::wstring assetsDirCache_;

		bool browsePending_{false};
		uint64_t browseDialogId_{0};
		std::string browseState_{"idle"};
		std::string browsePath_;
		std::string browseNotice_;

		RecorderPhase lastPhase_{RecorderPhase::Idle};
		std::chrono::steady_clock::time_point lastStateEmit_{};
		std::string lastStateJson_;
		std::string lastAppearanceJson_;
	};
} // namespace VideoExporter::webpanel
