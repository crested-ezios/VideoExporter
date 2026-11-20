// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../include/GakuAudio.h"
#include "../include/GakuMotionClock.h"
#include "../include/GakuPaths.h"
#include "../include/GakuPlugin.h"
#include "../include/GakuScreenshot.h"
#include "../include/GakuWebPanel.h"

#include "../src/CommonUtils.hpp"
#include "../src/Config.hpp"
#include "../src/FfmpegPipeline.hpp"
#include "../src/GkJsonLite.hpp"
#include "../src/VideoExporterWebPanel.hpp"
#include "../src/VideoRecorder.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	int g_failures = 0;

	void Check(bool cond, const char* expr, int line)
	{
		if (!cond)
		{
			std::cerr << "[FAIL] line " << line << ": " << expr << std::endl;
			++g_failures;
		}
	}

#define EXPECT(cond) Check(static_cast<bool>(cond), #cond, __LINE__)

	struct TempDirGuard
	{
		std::filesystem::path dir;
		explicit TempDirGuard(const std::wstring& prefix)
		{
			wchar_t tmp[MAX_PATH]{};
			GetTempPathW(MAX_PATH, tmp);
			dir = std::filesystem::path(tmp) / (prefix + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
			std::filesystem::create_directories(dir);
		}
		~TempDirGuard()
		{
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}
	};

	struct MockScreenshotState
	{
		uint32_t runtimeReady{1};
		uint32_t cameraAvailable{1};
		uint32_t greenScreenDetected{1};
		uint32_t maxDimension{4096};
		uint32_t lastAlphaMode{999};
		std::atomic<uint64_t> captureCount{0};
		bool alphaPattern{false};
		bool failNextCapture{false};

		GakuScreenshotServiceV1 Service()
		{
			GakuScreenshotServiceV1 s{};
			s.abiVersion = GAKU_SCREENSHOT_VERSION;
			s.structSize = sizeof(s);
			s.context = this;
			s.getCapabilities = [](void* ctx, GakuScreenshotCapabilitiesV1* out) -> GakuStatus {
				if (!ctx || !out) return GAKU_E_INVALID_ARGUMENT;
				const auto* st = static_cast<const MockScreenshotState*>(ctx);
				out->abiVersion = GAKU_SCREENSHOT_VERSION;
				out->structSize = sizeof(*out);
				out->runtimeReady = st->runtimeReady;
				out->cameraAvailable = st->cameraAvailable;
				out->greenScreenDetected = st->greenScreenDetected;
				out->maxDimension = st->maxDimension;
				out->windowWidth = 1920;
				out->windowHeight = 1080;
				return GAKU_OK;
			};
			s.captureFrameImmediate = [](void* ctx, const GakuScreenshotFrameRequestV1* req,
				void* outPixels, size_t capacity, GakuScreenshotFrameInfoV1* outInfo,
				char* errBuf, uint32_t errCap) -> GakuStatus {
				auto* st = static_cast<MockScreenshotState*>(ctx);
				if (!st || !req || !outPixels || !outInfo) return GAKU_E_INVALID_ARGUMENT;
				if (st->failNextCapture)
				{
					if (errBuf && errCap > 0) std::snprintf(errBuf, errCap, "Simulated capture error");
					return GAKU_E_INTERNAL;
				}
				const size_t rowPitch = static_cast<size_t>(req->width) * 4u;
				const size_t total = rowPitch * static_cast<size_t>(req->height);
				if (capacity < total) return GAKU_E_SIZE;

				st->lastAlphaMode = req->alphaMode;
				const uint64_t frameIdx = st->captureCount.fetch_add(1, std::memory_order_acq_rel);
				auto* px = static_cast<uint8_t*>(outPixels);

				if (st->alphaPattern)
				{
					// Transparent background (alpha=0) + moving opaque foreground block (alpha=255)
					std::memset(px, 0, total);
					const uint32_t xStart = static_cast<uint32_t>((160u + frameIdx * 80u) % (req->width - 400u));
					const uint32_t yStart = 200u;
					const uint32_t boxW = 320u;
					const uint32_t boxH = 320u;
					for (uint32_t y = yStart; y < yStart + boxH && y < req->height; ++y)
					{
						uint8_t* row = px + static_cast<size_t>(y) * rowPitch;
						for (uint32_t x = xStart; x < xStart + boxW && x < req->width; ++x)
						{
							row[x * 4 + 0] = 240;
							row[x * 4 + 1] = 80;
							row[x * 4 + 2] = 40;
							row[x * 4 + 3] = 255;
						}
					}
				}
				else
				{
					// Fill dark gradient background + fast moving stripe (avoid slow per-pixel math across 8.3M pixels)
					std::vector<uint8_t> templateRow(rowPitch, 0);
					for (uint32_t x = 0; x < req->width; ++x)
					{
						templateRow[x * 4 + 0] = static_cast<uint8_t>((x * 255u) / req->width);
						templateRow[x * 4 + 1] = 36;
						templateRow[x * 4 + 2] = 96;
						templateRow[x * 4 + 3] = 255;
					}
					for (uint32_t y = 0; y < req->height; ++y)
					{
						std::memcpy(px + static_cast<size_t>(y) * rowPitch, templateRow.data(), rowPitch);
					}
					const uint32_t barX = static_cast<uint32_t>((frameIdx * 180u) % (req->width - 240u));
					for (uint32_t y = 120; y < req->height - 120; ++y)
					{
						uint8_t* row = px + static_cast<size_t>(y) * rowPitch;
						for (uint32_t x = barX; x < barX + 240u; ++x)
						{
							row[x * 4 + 0] = 40;
							row[x * 4 + 1] = 235;
							row[x * 4 + 2] = 220;
							row[x * 4 + 3] = 255;
						}
					}
				}

				outInfo->abiVersion = GAKU_SCREENSHOT_VERSION;
				outInfo->structSize = sizeof(*outInfo);
				outInfo->width = req->width;
				outInfo->height = req->height;
				outInfo->rowPitch = static_cast<uint32_t>(rowPitch);
				outInfo->hasAlpha = st->alphaPattern ? 1u : 0u;
				outInfo->unityFrame = frameIdx + 1;
				return GAKU_OK;
			};
			return s;
		}
	};

	struct MockClockState
	{
		uint64_t nextLeaseId{42};
		uint64_t activeLeaseId{0};
		uint32_t playbackState{0};
		uint32_t runMode{GAKUMOTION_RUN_REALTIME};
		uint32_t fpsNum{60};
		uint32_t fpsDen{1};
		uint64_t evaluatedFrameSerial{0};
		int64_t fixedFrameIndex{0};
		double currentTimeSeconds{0.0};
		double timelineDurationSeconds{0.2};
		uint32_t hasAudio{0};
		float audioOffsetSeconds{0.0f};
		float audioSpeed{1.0f};
		float playbackSpeed{1.0f};
		float musicVolume{1.0f};
		std::string trackTitle{"TestTrack"};
		std::string audioFilePath;
		std::string activeAudioEffectId{"concert_hall"};

		std::vector<std::string> callLog;
		uint64_t notifyCount{0};
		double recordedBeginSeconds{-1.0};
		bool stallSerialOnce{false};

		GakuMotionClockServiceV1 Service()
		{
			GakuMotionClockServiceV1 c{};
			c.abiVersion = GAKU_MOTION_CLOCK_VERSION;
			c.structSize = sizeof(c);
			c.context = this;
			c.getSnapshot = [](void* ctx, GakuMotionClockSnapshotV1* out) -> GakuStatus {
				if (!ctx || !out) return GAKU_E_INVALID_ARGUMENT;
				const auto* st = static_cast<const MockClockState*>(ctx);
				out->abiVersion = GAKU_MOTION_CLOCK_VERSION;
				out->structSize = sizeof(*out);
				out->runMode = st->runMode;
				out->configuredSource = GAKUMOTION_CLOCK_FIXED_STEP;
				out->effectiveSource = GAKUMOTION_CLOCK_FIXED_STEP;
				out->playbackState = st->playbackState;
				out->fpsNumerator = st->fpsNum;
				out->fpsDenominator = st->fpsDen;
				out->waitingForRenderComplete = (st->activeLeaseId != 0 && st->playbackState == 1) ? 1u : 0u;
				out->hasAudio = st->hasAudio;
				out->currentTimeSeconds = st->currentTimeSeconds;
				out->timelineDurationSeconds = st->timelineDurationSeconds;
				out->lastStepDeltaSeconds = static_cast<double>(st->fpsDen) / static_cast<double>(st->fpsNum);
				out->fixedFrameIndex = st->fixedFrameIndex;
				out->leaseId = st->activeLeaseId;
				out->evaluatedFrameSerial = st->evaluatedFrameSerial;
				out->audioOffsetSeconds = st->audioOffsetSeconds;
				out->audioSpeed = st->audioSpeed;
				out->playbackSpeed = st->playbackSpeed;
				out->musicVolume = st->musicVolume;
				std::snprintf(out->leaseOwner, sizeof(out->leaseOwner), "VideoExporter");
				std::snprintf(out->trackTitle, sizeof(out->trackTitle), "%s", st->trackTitle.c_str());
				std::snprintf(out->audioFilePath, sizeof(out->audioFilePath), "%s", st->audioFilePath.c_str());
				std::snprintf(out->activeAudioEffectId, sizeof(out->activeAudioEffectId), "%s", st->activeAudioEffectId.c_str());
				return GAKU_OK;
			};
			c.acquireRecordingLease = [](void* ctx, const char* owner, uint32_t fpsN, uint32_t fpsD, uint64_t* outLease) -> GakuStatus {
				if (!ctx || !owner || !outLease || fpsN == 0 || fpsD == 0) return GAKU_E_INVALID_ARGUMENT;
				auto* st = static_cast<MockClockState*>(ctx);
				if (st->activeLeaseId != 0) return GAKU_E_CONFLICT;
				st->activeLeaseId = st->nextLeaseId++;
				st->fpsNum = fpsN;
				st->fpsDen = fpsD;
				st->runMode = GAKUMOTION_RUN_RECORDING;
				*outLease = st->activeLeaseId;
				st->callLog.emplace_back("acquireRecordingLease");
				return GAKU_OK;
			};
			c.releaseRecordingLease = [](void* ctx, uint64_t leaseId) -> GakuStatus {
				if (!ctx) return GAKU_E_INVALID_ARGUMENT;
				auto* st = static_cast<MockClockState*>(ctx);
				if (st->activeLeaseId != leaseId) return GAKU_E_INVALID_ARGUMENT;
				st->activeLeaseId = 0;
				st->runMode = GAKUMOTION_RUN_REALTIME;
				st->callLog.emplace_back("releaseRecordingLease");
				return GAKU_OK;
			};
			c.recordingBegin = [](void* ctx, uint64_t leaseId, double startSeconds) -> GakuStatus {
				if (!ctx) return GAKU_E_INVALID_ARGUMENT;
				auto* st = static_cast<MockClockState*>(ctx);
				if (st->activeLeaseId != leaseId) return GAKU_E_INVALID_ARGUMENT;
				st->recordedBeginSeconds = startSeconds;
				st->currentTimeSeconds = startSeconds;
				st->fixedFrameIndex = 0;
				st->playbackState = 1;
				++st->evaluatedFrameSerial;
				st->callLog.emplace_back("recordingBegin");
				return GAKU_OK;
			};
			c.recordingStop = [](void* ctx, uint64_t leaseId) -> GakuStatus {
				if (!ctx) return GAKU_E_INVALID_ARGUMENT;
				auto* st = static_cast<MockClockState*>(ctx);
				if (st->activeLeaseId != leaseId) return GAKU_E_INVALID_ARGUMENT;
				st->playbackState = 0;
				st->callLog.emplace_back("recordingStop");
				return GAKU_OK;
			};
			c.notifyFrameRendered = [](void* ctx, uint64_t leaseId, uint64_t serial, uint32_t advanceFrames) -> GakuStatus {
				if (!ctx) return GAKU_E_INVALID_ARGUMENT;
				auto* st = static_cast<MockClockState*>(ctx);
				if (st->activeLeaseId != leaseId || st->evaluatedFrameSerial != serial) return GAKU_E_INVALID_ARGUMENT;
				++st->notifyCount;
				st->fixedFrameIndex += advanceFrames;
				st->currentTimeSeconds += static_cast<double>(advanceFrames) * st->fpsDen / static_cast<double>(st->fpsNum);
				++st->evaluatedFrameSerial;
				st->callLog.emplace_back("notifyFrameRendered");
				return GAKU_OK;
			};
			return c;
		}
	};

	void WriteSyntheticWav(const std::filesystem::path& wavPath, double seconds, uint32_t sampleRate = 48000)
	{
		const uint32_t totalFrames = static_cast<uint32_t>(std::llround(seconds * sampleRate));
		const uint16_t channels = 2;
		const uint16_t bps = 16;
		const uint16_t blockAlign = channels * (bps / 8);
		const uint32_t dataBytes = totalFrames * blockAlign;
		const uint32_t riffSize = 36u + dataBytes;

		std::ofstream out(wavPath, std::ios::binary | std::ios::trunc);
		auto writeU16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
		auto writeU32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
		out.write("RIFF", 4);
		writeU32(riffSize);
		out.write("WAVEfmt ", 8);
		writeU32(16u);
		writeU16(1u);
		writeU16(channels);
		writeU32(sampleRate);
		writeU32(sampleRate * blockAlign);
		writeU16(blockAlign);
		writeU16(bps);
		out.write("data", 4);
		writeU32(dataBytes);

		constexpr double kPi = 3.14159265358979323846;
		for (uint32_t i = 0; i < totalFrames; ++i)
		{
			const float sample = 0.4f * std::sin(static_cast<float>(2.0 * kPi * 440.0 * i / sampleRate));
			const int16_t q = static_cast<int16_t>(std::lrintf(sample * 32767.0f));
			writeU16(static_cast<uint16_t>(q));
			writeU16(static_cast<uint16_t>(q));
		}
		out.flush();
	}

	struct MockAudioState
	{
		bool calledRenderOffline{false};
		std::string lastPresetId;
		std::string activePresetId{"concert_hall"};

		GakuAudioServiceV1 Service()
		{
			GakuAudioServiceV1 a{};
			a.abiVersion = GAKU_AUDIO_VERSION;
			a.structSize = sizeof(a);
			a.context = this;
			a.getActivePreset = [](void* ctx, char* outId, uint32_t cap) -> GakuStatus {
				if (!ctx || !outId || cap == 0) return GAKU_E_INVALID_ARGUMENT;
				const auto* st = static_cast<const MockAudioState*>(ctx);
				std::snprintf(outId, cap, "%s", st->activePresetId.c_str());
				return GAKU_OK;
			};
			a.renderOfflineWav = [](void* ctx, const GakuAudioOfflineRenderRequestV1* req,
				char* errBuf, uint32_t errCap) -> GakuStatus {
				(void)errBuf;
				(void)errCap;
				if (!ctx || !req || !req->outputWavPathUtf8) return GAKU_E_INVALID_ARGUMENT;
				auto* st = static_cast<MockAudioState*>(ctx);
				st->calledRenderOffline = true;
				st->lastPresetId = req->effectPresetId ? req->effectPresetId : "";
				const std::filesystem::path outPath(VideoExporter::util::Utf8ToWide(req->outputWavPathUtf8));
				WriteSyntheticWav(outPath, req->timelineDurationSeconds > 0.0 ? req->timelineDurationSeconds : 0.2,
					req->targetSampleRate > 0 ? req->targetSampleRate : 48000u);
				return GAKU_OK;
			};
			return a;
		}
	};

	bool WaitForRecorderFinish(VideoExporter::VideoRecorder& rec, int maxMs = 60000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const auto st = rec.GetStatus();
			if (st.phase == VideoExporter::RecorderPhase::Completed ||
				st.phase == VideoExporter::RecorderPhase::Error ||
				st.phase == VideoExporter::RecorderPhase::Idle)
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	bool RunFfprobeJson(const std::wstring& ffprobePath, const std::filesystem::path& filePath, GakuMotion::jsonlite::Value& outJson)
	{
		DWORD exitCode = 1;
		std::string output, err;
		const bool ran = VideoExporter::FfmpegPipeline::RunProcessCapture(
			ffprobePath,
			{L"-v", L"error", L"-show_streams", L"-show_format", L"-of", L"json", filePath.wstring()},
			15000, exitCode, output, err);
		if (!ran || exitCode != 0)
		{
			std::cerr << "[ffprobe error] " << err << "\n" << output << std::endl;
			return false;
		}
		std::string parseErr;
		return GakuMotion::jsonlite::ParseText(output, outJson, parseErr) && outJson.IsObject();
	}

	void TestConfigNormalizationAndPersistence()
	{
		TempDirGuard tmp(L"ve_cfg");
		const std::filesystem::path cfgPath = tmp.dir / L"config.json";
		const std::string defVideos = VideoExporter::util::WideToUtf8((tmp.dir / L"Videos").wstring());

		VideoExporter::RecorderConfig cfg;
		VideoExporter::NormalizeConfig(cfg, defVideos);
		EXPECT(cfg.width == 1920 && cfg.height == 1080);
		EXPECT(cfg.fps == 60);
		EXPECT(cfg.crf == 18);
		EXPECT(cfg.outputDir == defVideos);

		// Portrait 4K (2160x3840) must remain intact
		cfg.width = 2160;
		cfg.height = 3840;
		VideoExporter::NormalizeConfig(cfg, defVideos);
		EXPECT(cfg.width == 2160 && cfg.height == 3840);

		// Oversized 4096x4096 must clamp proportionally within 4096x2160 pixel budget
		cfg.width = 4096;
		cfg.height = 4096;
		VideoExporter::NormalizeConfig(cfg, defVideos);
		EXPECT(static_cast<uint64_t>(cfg.width) * cfg.height <= VideoExporter::kMaxVideoPixels);

		// Save and load roundtrip
		cfg.width = 3840;
		cfg.height = 2160;
		cfg.fps = 30;
		cfg.codec = "prores_ks";
		cfg.postFilterPreset = "vivid";
		cfg.crf = 15;
		std::string err;
		EXPECT(VideoExporter::SaveConfigFile(cfgPath, cfg, err));
		EXPECT(!std::filesystem::exists(cfgPath.wstring() + L".tmp"));

		VideoExporter::RecorderConfig loaded;
		EXPECT(VideoExporter::LoadConfigFile(cfgPath, loaded, defVideos, err));
		EXPECT(loaded.width == 3840 && loaded.height == 2160);
		EXPECT(loaded.fps == 30);
		EXPECT(loaded.codec == "prores_ks");
		EXPECT(loaded.postFilterPreset == "vivid");
		EXPECT(loaded.crf == 15);
	}

	void TestFfmpegProbe()
	{
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available);
		EXPECT(!probe.ffmpegPath.empty());
		EXPECT(!probe.ffprobePath.empty());
		EXPECT(!probe.version.empty());
		EXPECT(probe.encoders.libx264);
		EXPECT(probe.encoders.prores_ks);
		EXPECT(probe.encoders.libvpx_vp9);

		const auto badProbe = VideoExporter::FfmpegPipeline::Probe("C:\\NonExistent\\ffmpeg_missing_12345.exe");
		EXPECT(!badProbe.available);
		EXPECT(!badProbe.error.empty());
	}

	void TestReal4kLockstepMp4WithAudioAndFilter()
	{
		TempDirGuard tmp(L"ve_4k_mp4");
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available && !probe.ffprobePath.empty());

		const std::filesystem::path srcWav = tmp.dir / L"source_bgm.wav";
		WriteSyntheticWav(srcWav, 0.5, 48000);

		MockScreenshotState mockSs;
		MockClockState mockClk;
		mockClk.timelineDurationSeconds = 0.2; // 30fps * 0.2s = 6 frames
		mockClk.hasAudio = 1;
		mockClk.audioFilePath = VideoExporter::util::WideToUtf8(srcWav.wstring());
		mockClk.activeAudioEffectId = "concert_hall";
		mockClk.trackTitle = "4K_Test_Track";
		MockAudioState mockAud;

		VideoExporter::ServiceHostCallbacks hostCb;
		hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
		hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };
		hostCb.queryAudio = [&](GakuAudioServiceV1& out) { out = mockAud.Service(); return true; };

		VideoExporter::VideoRecorder rec(hostCb);
		const std::string outDir = VideoExporter::util::WideToUtf8(tmp.dir.wstring());
		rec.InitializePaths(tmp.dir / L"config.json", outDir);

		GakuMotion::jsonlite::Value patch;
		std::string err;
		EXPECT(GakuMotion::jsonlite::ParseText(
			R"({"width":3840,"height":2160,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","crf":22,"muxAudio":true,"bakeAudioEffects":true,"postFilterPreset":"sharpen"})",
			patch, err));
		EXPECT(rec.UpdateConfig(patch, err));

		const std::filesystem::path outMp4 = tmp.dir / L"export_4k.mp4";
		rec.SetOutputPathOverrideForTest(outMp4);
		EXPECT(rec.RequestStart(err));

		// Pump OnTick until capture phase finishes
		for (int i = 0; i < 64; ++i)
		{
			rec.OnTick();
			const auto st = rec.GetStatus();
			if (st.phase != VideoExporter::RecorderPhase::Starting &&
				st.phase != VideoExporter::RecorderPhase::Recording)
			{
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		EXPECT(WaitForRecorderFinish(rec, 90000));
		const auto finalSt = rec.GetStatus();
		if (finalSt.phase != VideoExporter::RecorderPhase::Completed)
		{
			std::cerr << "[4K MP4 ERROR] " << finalSt.lastError << std::endl;
		}
		EXPECT(finalSt.phase == VideoExporter::RecorderPhase::Completed);
		EXPECT(finalSt.capturedFrames == 6);
		EXPECT(mockSs.captureCount.load() == 6);
		EXPECT(mockSs.lastAlphaMode == GAKU_SCREENSHOT_ALPHA_OPAQUE);
		EXPECT(mockClk.notifyCount == 6);
		EXPECT(mockClk.activeLeaseId == 0);
		EXPECT(mockAud.calledRenderOffline);
		EXPECT(mockAud.lastPresetId == "concert_hall");
		EXPECT(std::filesystem::is_regular_file(outMp4));

		// Verify exact lockstep call lifecycle order
		EXPECT(!mockClk.callLog.empty() && mockClk.callLog[0] == "acquireRecordingLease");
		EXPECT(mockClk.callLog.size() >= 10);
		EXPECT(mockClk.callLog[1] == "recordingBegin");
		EXPECT(mockClk.callLog[mockClk.callLog.size() - 2] == "recordingStop");
		EXPECT(mockClk.callLog[mockClk.callLog.size() - 1] == "releaseRecordingLease");

		// Probe the real exported 4K MP4 with ffprobe!
		GakuMotion::jsonlite::Value ffJson;
		EXPECT(RunFfprobeJson(probe.ffprobePath, outMp4, ffJson));
		const auto* streams = ffJson.Find("streams");
		EXPECT(streams && streams->IsArray() && streams->Size() == 2);
		if (streams && streams->Size() >= 2)
		{
			const auto* v = streams->At(0);
			const auto* a = streams->At(1);
			EXPECT(v->StringPath("codec_name", "") == "h264");
			EXPECT(v->IntPath("width", 0) == 3840);
			EXPECT(v->IntPath("height", 0) == 2160);
			EXPECT(v->StringPath("nb_frames", "") == "6");
			EXPECT(a->StringPath("codec_name", "") == "aac");
			EXPECT(a->StringPath("sample_rate", "") == "48000");
		}
	}

	void TestReal4kAlphaProResMov()
	{
		TempDirGuard tmp(L"ve_4k_mov");
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available && !probe.ffprobePath.empty());

		MockScreenshotState mockSs;
		mockSs.alphaPattern = true;
		MockClockState mockClk;
		mockClk.timelineDurationSeconds = 1.0;

		VideoExporter::ServiceHostCallbacks hostCb;
		hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
		hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };

		VideoExporter::VideoRecorder rec(hostCb);
		rec.InitializePaths(tmp.dir / L"config.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));

		GakuMotion::jsonlite::Value patch;
		std::string err;
		EXPECT(GakuMotion::jsonlite::ParseText(
			R"({"width":3840,"height":2160,"fps":30,"recordingMode":"lockstep_range","rangeStartSeconds":0.1,"rangeEndSeconds":0.233333,"codec":"prores_ks","muxAudio":false,"postFilterPreset":"none"})",
			patch, err));
		EXPECT(rec.UpdateConfig(patch, err));

		const std::filesystem::path outMov = tmp.dir / L"export_4k_alpha.mov";
		rec.SetOutputPathOverrideForTest(outMov);
		EXPECT(rec.RequestStart(err));

		for (int i = 0; i < 64; ++i)
		{
			rec.OnTick();
			const auto st = rec.GetStatus();
			if (st.phase != VideoExporter::RecorderPhase::Starting &&
				st.phase != VideoExporter::RecorderPhase::Recording)
			{
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		EXPECT(WaitForRecorderFinish(rec, 90000));
		const auto finalSt = rec.GetStatus();
		if (finalSt.phase != VideoExporter::RecorderPhase::Completed)
		{
			std::cerr << "[4K MOV ERROR] " << finalSt.lastError << std::endl;
		}
		EXPECT(finalSt.phase == VideoExporter::RecorderPhase::Completed);
		EXPECT(finalSt.capturedFrames == 4);
		EXPECT(std::fabs(mockClk.recordedBeginSeconds - 0.1) < 1e-4);
		EXPECT(mockSs.lastAlphaMode == GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN);
		EXPECT(mockClk.activeLeaseId == 0);
		EXPECT(std::filesystem::is_regular_file(outMov));

		GakuMotion::jsonlite::Value ffJson;
		EXPECT(RunFfprobeJson(probe.ffprobePath, outMov, ffJson));
		const auto* streams = ffJson.Find("streams");
		EXPECT(streams && streams->IsArray() && streams->Size() == 1);
		if (streams && streams->Size() >= 1)
		{
			const auto* v = streams->At(0);
			EXPECT(v->StringPath("codec_name", "") == "prores");
			EXPECT(v->StringPath("profile", "") == "4444");
			EXPECT(v->IntPath("width", 0) == 3840);
			EXPECT(v->IntPath("height", 0) == 2160);
			EXPECT(v->StringPath("nb_frames", "") == "4");
			const std::string pixFmt = v->StringPath("pix_fmt", "");
			// FFmpeg prores_ks encodes -pix_fmt yuva444p10le; FFmpeg 7.x prores decoder reports yuva444p12le (or yuva444p10le).
			EXPECT(pixFmt == "yuva444p10le" || pixFmt == "yuva444p12le");
		}

		// Decode frame 0 back to raw RGBA and verify alpha=0 background vs alpha=255 foreground!
		const std::filesystem::path rawRgba = tmp.dir / L"frame0.rgba";
		DWORD decCode = 1;
		std::string decOut, decErr;
		EXPECT(VideoExporter::FfmpegPipeline::RunProcessCapture(
			probe.ffmpegPath,
			{L"-y", L"-hide_banner", L"-loglevel", L"error", L"-i", outMov.wstring(),
			 L"-frames:v", L"1", L"-f", L"rawvideo", L"-pix_fmt", L"rgba", rawRgba.wstring()},
			30000, decCode, decOut, decErr) && decCode == 0);
		if (std::filesystem::is_regular_file(rawRgba))
		{
			std::ifstream in(rawRgba, std::ios::binary);
			std::vector<uint8_t> frame(3840ull * 2160ull * 4ull);
			in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
			EXPECT(in.gcount() == static_cast<std::streamsize>(frame.size()));
			// Background pixel (10, 10) must be transparent (alpha == 0)
			const size_t bgOff = (10ull * 3840ull + 10ull) * 4ull;
			EXPECT(frame[bgOff + 3] == 0);
			// Foreground pixel (300, 300) must be opaque (alpha == 255)
			const size_t fgOff = (300ull * 3840ull + 300ull) * 4ull;
			EXPECT(frame[fgOff + 3] >= 250);
		}
	}

	void TestLockstepSerialHoldAndFreeRun()
	{
		TempDirGuard tmp(L"ve_freerun");
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available && !probe.ffprobePath.empty());

		// 1. Test Lockstep stall when evaluatedFrameSerial does not advance
		{
			MockScreenshotState mockSs;
			MockClockState mockClk;
			mockClk.timelineDurationSeconds = 1.0;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.InitializePaths(tmp.dir / L"cfg1.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"stall_test.mp4");
			EXPECT(rec.RequestStart(err));

			// First tick starts + captures serial 1 -> mock increments serial to 2
			rec.OnTick();
			EXPECT(rec.GetStatus().capturedFrames == 1);

			// Freeze serial at 2, wait: reset serial back to 1 (same as lastCapturedSerial)
			const uint64_t savedSerial = mockClk.evaluatedFrameSerial;
			mockClk.evaluatedFrameSerial = 1;
			rec.OnTick();
			rec.OnTick();
			rec.OnTick();
			EXPECT(rec.GetStatus().capturedFrames == 1); // Must NOT capture duplicate frame!

			mockClk.evaluatedFrameSerial = savedSerial;
			EXPECT(rec.RequestStop(err));
			rec.OnTick();
			EXPECT(WaitForRecorderFinish(rec, 30000));
			EXPECT(rec.GetStatus().phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(mockClk.activeLeaseId == 0);
		}

		// 2. Test Free-Run mode without GakuMotion clock service
		{
			MockScreenshotState mockSs;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [](GakuMotionClockServiceV1&) { return false; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.InitializePaths(tmp.dir / L"cfg2.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":640,"height":360,"fps":30,"recordingMode":"freerun","codec":"libx264","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));

			const std::filesystem::path freeOut = tmp.dir / L"freerun.mp4";
			rec.SetOutputPathOverrideForTest(freeOut);
			EXPECT(rec.RequestStart(err));

			for (int i = 0; i < 5; ++i)
			{
				rec.OnTick();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			EXPECT(rec.GetStatus().capturedFrames == 5);
			EXPECT(rec.RequestStop(err));
			rec.OnTick();
			EXPECT(WaitForRecorderFinish(rec, 30000));
			EXPECT(rec.GetStatus().phase == VideoExporter::RecorderPhase::Completed);

			GakuMotion::jsonlite::Value ffJson;
			EXPECT(RunFfprobeJson(probe.ffprobePath, freeOut, ffJson));
			const auto* streams = ffJson.Find("streams");
			EXPECT(streams && streams->Size() == 1);
			if (streams && streams->Size() >= 1)
			{
				EXPECT(streams->At(0)->IntPath("width", 0) == 640);
				EXPECT(streams->At(0)->IntPath("height", 0) == 360);
				EXPECT(streams->At(0)->StringPath("nb_frames", "") == "5");
			}
		}
	}

	void TestVp9AlphaAndRingBackpressure()
	{
		TempDirGuard tmp(L"ve_vp9_bp");
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available && !probe.ffprobePath.empty() && probe.encoders.libvpx_vp9);

		MockScreenshotState mockSs;
		mockSs.alphaPattern = true;
		MockClockState mockClk;
		mockClk.timelineDurationSeconds = 0.2; // 30fps * 0.2s = 6 frames

		VideoExporter::ServiceHostCallbacks hostCb;
		hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
		hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };

		VideoExporter::VideoRecorder rec(hostCb);
		rec.InitializePaths(tmp.dir / L"config.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));

		GakuMotion::jsonlite::Value patch;
		std::string err;
		EXPECT(GakuMotion::jsonlite::ParseText(
			R"({"width":1920,"height":1080,"fps":30,"recordingMode":"lockstep_full","codec":"libvpx-vp9","crf":24,"muxAudio":false,"postFilterPreset":"none"})",
			patch, err));
		EXPECT(rec.UpdateConfig(patch, err));

		const std::filesystem::path outWebm = tmp.dir / L"export_alpha.webm";
		rec.SetOutputPathOverrideForTest(outWebm);
		EXPECT(rec.RequestStart(err));

		// Pump ticks without initial delay to exercise 4-slot ring buffer backpressure on VP9 encode
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const uint64_t prevCaptured = rec.GetStatus().capturedFrames;
			rec.OnTick();
			const auto st = rec.GetStatus();
			if (st.phase != VideoExporter::RecorderPhase::Starting &&
				st.phase != VideoExporter::RecorderPhase::Recording)
			{
				break;
			}
			// If ring buffer held (same captured count), yield briefly for WriterThread to drain
			if (st.capturedFrames == prevCaptured)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}

		EXPECT(WaitForRecorderFinish(rec, 60000));
		const auto finalSt = rec.GetStatus();
		EXPECT(finalSt.phase == VideoExporter::RecorderPhase::Completed);
		EXPECT(finalSt.capturedFrames == 6);
		EXPECT(mockClk.notifyCount == 6);
		EXPECT(mockClk.activeLeaseId == 0);
		EXPECT(std::filesystem::is_regular_file(outWebm));

		GakuMotion::jsonlite::Value ffJson;
		EXPECT(RunFfprobeJson(probe.ffprobePath, outWebm, ffJson));
		const auto* streams = ffJson.Find("streams");
		EXPECT(streams && streams->Size() == 1);
		if (streams && streams->Size() >= 1)
		{
			const auto* v = streams->At(0);
			EXPECT(v->StringPath("codec_name", "") == "vp9");
			EXPECT(v->IntPath("width", 0) == 1920);
			EXPECT(v->IntPath("height", 0) == 1080);
			EXPECT(v->StringPath("tags.alpha_mode", "") == "1");
		}

		// Decode frame 0 using -c:v libvpx-vp9 to verify alpha plane transparency roundtrip!
		const std::filesystem::path rawRgba = tmp.dir / L"vp9_frame0.rgba";
		DWORD decCode = 1;
		std::string decOut, decErr;
		EXPECT(VideoExporter::FfmpegPipeline::RunProcessCapture(
			probe.ffmpegPath,
			{L"-y", L"-hide_banner", L"-loglevel", L"error", L"-c:v", L"libvpx-vp9",
			 L"-i", outWebm.wstring(), L"-frames:v", L"1", L"-f", L"rawvideo", L"-pix_fmt", L"rgba", rawRgba.wstring()},
			30000, decCode, decOut, decErr) && decCode == 0);
		if (std::filesystem::is_regular_file(rawRgba))
		{
			std::ifstream in(rawRgba, std::ios::binary);
			std::vector<uint8_t> frame(1920ull * 1080ull * 4ull);
			in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
			EXPECT(in.gcount() == static_cast<std::streamsize>(frame.size()));
			const size_t bgOff = (10ull * 1920ull + 10ull) * 4ull;
			EXPECT(frame[bgOff + 3] == 0);
			const size_t fgOff = (300ull * 1920ull + 300ull) * 4ull;
			EXPECT(frame[fgOff + 3] >= 250);
		}
	}

	void TestWebPanelBridgeContract()
	{
		TempDirGuard tmp(L"ve_web");
		MockScreenshotState mockSs;
		MockClockState mockClk;
		MockAudioState mockAud;
		VideoExporter::ServiceHostCallbacks hostCb;
		hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
		hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };
		hostCb.queryAudio = [&](GakuAudioServiceV1& out) { out = mockAud.Service(); return true; };

		VideoExporter::VideoRecorder rec(hostCb);
		rec.InitializePaths(tmp.dir / L"config.json", VideoExporter::util::WideToUtf8((tmp.dir / L"Videos").wstring()));

		std::vector<std::string> events;
		bool textInputActive = false;
		bool folderOpened = false;
		std::string openedFolder;
		const std::filesystem::path uiDir = tmp.dir / L"VideoExporter.UI";

		VideoExporter::webpanel::WebPanelHooks hooks;
		hooks.assetsDirs = [&]() -> std::vector<std::wstring> { return {uiDir.wstring()}; };
		hooks.openFolder = [&](const std::string& folder, const std::string&, std::string&) {
			folderOpened = true;
			openedFolder = folder;
			return true;
		};
		hooks.beginBrowseFfmpeg = [](std::string&) { return true; };
		hooks.pollBrowseFfmpeg = [](std::string& path, std::string&) {
			// Return current PATH ffmpeg
			const auto p = VideoExporter::FfmpegPipeline::Probe("");
			path = p.ffmpegPathUtf8;
			return GAKU_FILE_DIALOG_ACCEPTED;
		};
		hooks.setTextInputActive = [&](bool active) { textInputActive = active; };
		hooks.getAppearance = [](int& theme, float& scale, bool& ok) {
			theme = 1;
			scale = 1.25f;
			ok = true;
		};

		VideoExporter::webpanel::Bridge bridge(rec, hooks);

		// Before index.html exists, GetAssets must return GAKU_E_UNAVAILABLE
		GakuWebPanelAssetsV1 assets{};
		EXPECT(bridge.GetAssets(&assets) == GAKU_E_UNAVAILABLE);

		std::filesystem::create_directories(uiDir);
		{
			std::ofstream f(uiDir / L"index.html");
			f << "<!doctype html><html></html>";
		}
		EXPECT(bridge.GetAssets(&assets) == GAKU_OK);
		EXPECT(std::wstring(assets.virtualHost) == L"videoexporter.invalid");
		EXPECT(std::wstring(assets.entryPath) == L"index.html");

		bridge.SetEventSink([](void* userData, const char* json) {
			static_cast<std::vector<std::string>*>(userData)->emplace_back(json);
		}, &events);
		bridge.OnShow();
		bridge.OnUiTick();
		EXPECT(events.size() >= 2); // appearance.changed + state.changed

		// Size negotiation test
		uint32_t reqBytes = 0;
		char tiny[8]{};
		const std::string helloReq = R"({"v":1,"id":1,"type":"request","method":"bridge.hello","params":{}})";
		EXPECT(bridge.HandleMessage(helloReq.c_str(), sizeof(tiny), tiny, &reqBytes) == GAKU_E_SIZE);
		EXPECT(reqBytes > sizeof(tiny));

		std::string buf(reqBytes, '\0');
		EXPECT(bridge.HandleMessage(helloReq.c_str(), reqBytes, buf.data(), &reqBytes) == GAKU_OK);
		GakuMotion::jsonlite::Value resp;
		std::string err;
		EXPECT(GakuMotion::jsonlite::ParseText(buf.c_str(), resp, err));
		EXPECT(resp.BoolPath("ok", false));
		EXPECT(resp.StringPath("result.pluginId", "") == "videoexporter");

		// state.get
		const std::string stateResp = bridge.HandleMessageForTest(R"({"v":1,"id":2,"type":"request","method":"state.get","params":{}})");
		EXPECT(GakuMotion::jsonlite::ParseText(stateResp, resp, err));
		EXPECT(resp.BoolPath("ok", false));
		EXPECT(resp.IntPath("result.config.width", 0) == 1920);
		EXPECT(resp.BoolPath("result.services.screenshot.available", false));
		EXPECT(resp.BoolPath("result.services.clock.available", false));
		EXPECT(resp.BoolPath("result.services.audio.available", false));
		EXPECT(resp.BoolPath("result.services.ffmpeg.available", false));
		EXPECT(resp.IntPath("result.appearance.theme", -1) == 1);

		// config.set
		const std::string cfgResp = bridge.HandleMessageForTest(
			R"({"v":1,"id":3,"type":"request","method":"config.set","params":{"width":3840,"height":2160,"codec":"libvpx-vp9","crf":24}})");
		EXPECT(GakuMotion::jsonlite::ParseText(cfgResp, resp, err));
		EXPECT(resp.BoolPath("ok", false));
		EXPECT(resp.IntPath("result.config.width", 0) == 3840);
		EXPECT(resp.StringPath("result.config.codec", "") == "libvpx-vp9");

		// Invalid config.set returns structured error
		const std::string badCfgResp = bridge.HandleMessageForTest(
			R"({"v":1,"id":4,"type":"request","method":"config.set","params":{"width":9999}})");
		EXPECT(GakuMotion::jsonlite::ParseText(badCfgResp, resp, err));
		EXPECT(!resp.BoolPath("ok", true));
		EXPECT(resp.StringPath("error.code", "") == "invalid_params");

		// folder.open & ffmpeg.browse & ffmpeg.probe & shell.setTextInputActive
		const std::string openResp = bridge.HandleMessageForTest(R"({"v":1,"id":5,"type":"request","method":"folder.open","params":{}})");
		EXPECT(GakuMotion::jsonlite::ParseText(openResp, resp, err) && resp.BoolPath("ok", false));
		EXPECT(folderOpened);

		const std::string browseResp = bridge.HandleMessageForTest(R"({"v":1,"id":6,"type":"request","method":"ffmpeg.browse","params":{}})");
		EXPECT(GakuMotion::jsonlite::ParseText(browseResp, resp, err) && resp.BoolPath("ok", false));
		EXPECT(resp.StringPath("result.state", "") == "accepted");

		const std::string probeResp = bridge.HandleMessageForTest(R"({"v":1,"id":7,"type":"request","method":"ffmpeg.probe","params":{}})");
		EXPECT(GakuMotion::jsonlite::ParseText(probeResp, resp, err) && resp.BoolPath("ok", false));
		EXPECT(resp.BoolPath("result.ffmpeg.available", false));

		bridge.HandleMessageForTest(R"({"v":1,"id":8,"type":"request","method":"shell.setTextInputActive","params":{"active":true}})");
		EXPECT(textInputActive);

		bridge.OnShutdown();
		EXPECT(!events.empty() && events.back().find("\"bridge.closed\"") != std::string::npos);
	}

	void TestPluginDllAbiAndLazyServices()
	{
		// Verify compiled DLL exports and ABI lifecycle if build/native/VideoExporter.dll exists.
		std::error_code ec;
		const std::filesystem::path dllPath = std::filesystem::current_path() / L"build" / L"native" / L"VideoExporter.dll";
		if (!std::filesystem::is_regular_file(dllPath, ec))
		{
			std::cout << "[INFO] Skipping DLL ABI load test (VideoExporter.dll not at " << VideoExporter::util::WideToUtf8(dllPath.wstring()) << ")\n";
			return;
		}
		HMODULE mod = LoadLibraryW(dllPath.c_str());
		EXPECT(mod != nullptr);
		if (!mod) return;

		auto queryFn = reinterpret_cast<FnGakuPlugin_Query>(GetProcAddress(mod, "GakuPlugin_Query"));
		EXPECT(queryFn != nullptr);
		GakuPluginDescriptor desc{};
		EXPECT(queryFn(GAKU_ABI_VERSION, sizeof(desc), &desc) == GAKU_OK);
		EXPECT(std::string(desc.id) == "videoexporter");
		EXPECT(std::string(desc.name) == "VideoExporter");
		EXPECT(std::string(desc.version) == "0.1.0");
		EXPECT(desc.activate != nullptr && desc.onTick != nullptr && desc.shutdown != nullptr);

		// Simulate GakuLoader host registering services lazily AFTER Activate
		struct HostState
		{
			std::unordered_map<std::string, std::vector<uint8_t>> services;
			std::string registeredHotkeyId;
			uint32_t registeredHotkeyVk{0};
		} hostState;

		GakuHostApi host{};
		host.abiVersion = GAKU_ABI_VERSION;
		host.structSize = sizeof(host);
		host.hostContext = &hostState;
		host.registerService = [](void* ctx, const char* name, uint32_t tableSize, const void* table) -> GakuStatus {
			auto* hs = static_cast<HostState*>(ctx);
			const auto* bytes = static_cast<const uint8_t*>(table);
			hs->services[name] = std::vector<uint8_t>(bytes, bytes + tableSize);
			return GAKU_OK;
		};
		host.queryService = [](void* ctx, const char* name, uint32_t reqVer, uint32_t outSize, void* out) -> GakuStatus {
			auto* hs = static_cast<HostState*>(ctx);
			if (std::string(name) == GAKU_SERVICE_HOTKEYS && outSize >= sizeof(GakuHotkeyService))
			{
				auto* hk = static_cast<GakuHotkeyService*>(out);
				hk->abiVersion = reqVer;
				hk->structSize = sizeof(GakuHotkeyService);
				hk->context = hs;
				hk->registerHotkey = [](void* c, const GakuHotkeyBinding* b) -> GakuStatus {
					auto* h = static_cast<HostState*>(c);
					h->registeredHotkeyId = b->actionId;
					h->registeredHotkeyVk = b->virtualKey;
					return GAKU_OK;
				};
				hk->unregisterHotkey = [](void*, const char*) -> GakuStatus { return GAKU_OK; };
				return GAKU_OK;
			}
			const auto it = hs->services.find(name);
			if (it == hs->services.end()) return GAKU_E_UNAVAILABLE;
			if (outSize < it->second.size()) return GAKU_E_SIZE;
			std::memcpy(out, it->second.data(), it->second.size());
			return GAKU_OK;
		};

		void* pluginState = nullptr;
		EXPECT(desc.activate(&host, &pluginState) == GAKU_OK);
		EXPECT(pluginState != nullptr);
		EXPECT(hostState.services.count("videoexporter.web-panel/v1") == 1);
		EXPECT(hostState.registeredHotkeyId == "toggle-recording");
		EXPECT(hostState.registeredHotkeyVk == VK_F12);

		// Register ScreenshotManager AFTER Activate and verify lazy query picks it up!
		MockScreenshotState mockSs;
		GakuScreenshotServiceV1 ssTbl = mockSs.Service();
		host.registerService(&hostState, GAKU_SERVICE_SCREENSHOT_CAPTURE, sizeof(ssTbl), &ssTbl);
		if (desc.onRuntimeReady) desc.onRuntimeReady(pluginState);

		GakuWebPanelServiceV1 wp{};
		std::memcpy(&wp, hostState.services["videoexporter.web-panel/v1"].data(), sizeof(wp));
		char respBuf[16384]{};
		uint32_t reqBytes = 0;
		const std::string stReq = R"({"v":1,"id":10,"type":"request","method":"state.get","params":{}})";
		EXPECT(wp.handleMessage(wp.context, stReq.c_str(), sizeof(respBuf), respBuf, &reqBytes) == GAKU_OK);
		GakuMotion::jsonlite::Value stJson;
		std::string err;
		EXPECT(GakuMotion::jsonlite::ParseText(respBuf, stJson, err));
		EXPECT(stJson.BoolPath("result.services.screenshot.available", false));

		desc.shutdown(pluginState);
		FreeLibrary(mod);
	}

	void TestWakeControlDeduplicationAndDeferredEndOfFrameCapture()
	{
		TempDirGuard tmp(L"ve_deferred");
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available && !probe.ffprobePath.empty());

		// 1. ProcessPendingControl() + OnTick() in same TickFrame (freerun) must capture only 1 frame.
		{
			MockScreenshotState mockSs;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [](GakuMotionClockServiceV1&) { return false; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.InitializePaths(tmp.dir / L"cfg_wake.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"freerun","codec":"libx264","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"wake_dedup.mp4");
			EXPECT(rec.RequestStart(err));

			// Simulate DrainTasks(MainThreadWakeTask -> ProcessPendingControl) followed immediately by OnTick.
			rec.ProcessPendingControl();
			EXPECT(rec.IsRecording());
			EXPECT(rec.GetStatus().capturedFrames == 0);
			rec.OnTick();
			EXPECT(rec.GetStatus().capturedFrames == 1);

			EXPECT(rec.RequestStop(err));
			rec.ProcessPendingControl();
			EXPECT(WaitForRecorderFinish(rec, 30000));
			EXPECT(rec.GetStatus().phase == VideoExporter::RecorderPhase::Completed);
		}

		// 2. Deferred End-of-Frame Capture matches real GakuMotion contract:
		//    - recordingBegin does NOT bump evaluatedFrameSerial;
		//    - OnTick() never captures or steps clock before MotionActorTick;
		//    - MotionActorTick evaluates pose and increments evaluatedFrameSerial;
		//    - OnEndOfFrameCapture() captures that exact frame and calls notifyFrameRendered.
		{
			MockScreenshotState mockSs;
			struct RealLikeClockState
			{
				uint64_t leaseId{0};
				uint32_t playbackState{0};
				uint64_t evaluatedSerial{100};
				int64_t frameIndex{0};
				double timeSeconds{0.0};
				double durationSeconds{0.1}; // 30fps * 0.1s = 3 frames
				std::vector<uint64_t> capturedSerials;
				uint64_t notifyCount{0};

				GakuMotionClockServiceV1 Service()
				{
					GakuMotionClockServiceV1 c{};
					c.abiVersion = GAKU_MOTION_CLOCK_VERSION;
					c.structSize = sizeof(c);
					c.context = this;
					c.getSnapshot = [](void* ctx, GakuMotionClockSnapshotV1* out) -> GakuStatus {
						auto* st = static_cast<RealLikeClockState*>(ctx);
						out->abiVersion = GAKU_MOTION_CLOCK_VERSION;
						out->structSize = sizeof(*out);
						out->runMode = st->leaseId ? GAKUMOTION_RUN_RECORDING : GAKUMOTION_RUN_REALTIME;
						out->configuredSource = GAKUMOTION_CLOCK_EXTERNAL;
						out->effectiveSource = GAKUMOTION_CLOCK_EXTERNAL;
						out->playbackState = st->playbackState;
						out->fpsNumerator = 30;
						out->fpsDenominator = 1;
						out->waitingForRenderComplete = 0; // Real GakuMotion external lease keeps this 0
						out->currentTimeSeconds = st->timeSeconds;
						out->timelineDurationSeconds = st->durationSeconds;
						out->fixedFrameIndex = st->frameIndex;
						out->leaseId = st->leaseId;
						out->evaluatedFrameSerial = st->evaluatedSerial;
						std::snprintf(out->trackTitle, sizeof(out->trackTitle), "RealLike");
						return GAKU_OK;
					};
					c.acquireRecordingLease = [](void* ctx, const char*, uint32_t, uint32_t, uint64_t* outLease) -> GakuStatus {
						auto* st = static_cast<RealLikeClockState*>(ctx);
						st->leaseId = 77;
						*outLease = 77;
						return GAKU_OK;
					};
					c.releaseRecordingLease = [](void* ctx, uint64_t leaseId) -> GakuStatus {
						auto* st = static_cast<RealLikeClockState*>(ctx);
						if (st->leaseId != leaseId) return GAKU_E_CONFLICT;
						st->leaseId = 0;
						return GAKU_OK;
					};
					c.recordingBegin = [](void* ctx, uint64_t leaseId, double startSec) -> GakuStatus {
						auto* st = static_cast<RealLikeClockState*>(ctx);
						if (st->leaseId != leaseId) return GAKU_E_CONFLICT;
						st->timeSeconds = startSec;
						st->frameIndex = 0;
						st->playbackState = 1;
						// Note: real GakuMotion::RecordingBegin does NOT bump evaluatedFrameSerial!
						return GAKU_OK;
					};
					c.recordingStop = [](void* ctx, uint64_t leaseId) -> GakuStatus {
						auto* st = static_cast<RealLikeClockState*>(ctx);
						if (st->leaseId != leaseId) return GAKU_E_CONFLICT;
						st->playbackState = 0;
						return GAKU_OK;
					};
					c.notifyFrameRendered = [](void* ctx, uint64_t leaseId, uint64_t serial, uint32_t advanceFrames) -> GakuStatus {
						auto* st = static_cast<RealLikeClockState*>(ctx);
						if (st->leaseId != leaseId || st->evaluatedSerial != serial) return GAKU_E_CONFLICT;
						st->capturedSerials.push_back(serial);
						++st->notifyCount;
						st->frameIndex += advanceFrames;
						st->timeSeconds += static_cast<double>(advanceFrames) / 30.0;
						return GAKU_OK;
					};
					return c;
				}
			} mockClk;

			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.SetDeferredCaptureMode(true);
			rec.InitializePaths(tmp.dir / L"cfg_def.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"deferred_eof.mp4");
			EXPECT(rec.RequestStart(err));

			// Premature EndOfFrame before OnTick started session must do nothing
			rec.OnEndOfFrameCapture();
			EXPECT(mockSs.captureCount.load() == 0);

			// Frame 0: OnTick starts session (does NOT capture), MotionActorTick evaluates frame 0, then OnEndOfFrameCapture captures.
			rec.OnTick();
			EXPECT(rec.IsRecording());
			EXPECT(mockSs.captureCount.load() == 0);
			EXPECT(mockClk.notifyCount == 0);

			for (uint64_t f = 0; f < 3; ++f)
			{
				// Extra OnTick (e.g., WakeFramePump aliasing) must NEVER capture in deferred mode!
				rec.OnTick();
				EXPECT(mockSs.captureCount.load() == f);

				// MotionActorTick evaluates frame f
				++mockClk.evaluatedSerial;

				// Extra OnEndOfFrameCapture without new serial after capture must skip
				rec.OnEndOfFrameCapture();
				EXPECT(mockSs.captureCount.load() == f + 1);
				EXPECT(mockClk.notifyCount == f + 1);

				if (f + 1 < 3)
				{
					rec.OnEndOfFrameCapture();
					EXPECT(mockSs.captureCount.load() == f + 1);
				}
			}

			EXPECT(WaitForRecorderFinish(rec, 30000));
			const auto st = rec.GetStatus();
			EXPECT(st.phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(st.capturedFrames == 3);
			EXPECT(mockClk.leaseId == 0);
			EXPECT(mockClk.capturedSerials.size() == 3);
			EXPECT(mockClk.capturedSerials[0] == 101);
			EXPECT(mockClk.capturedSerials[1] == 102);
			EXPECT(mockClk.capturedSerials[2] == 103);
		}
	}

	void TestSwapchainPresentPathAndFallback()
	{
		std::cout << "  [Test] Swapchain Present Path & Fallback...\n";
		TempDirGuard tmp(L"ve_present_test");
		const auto probe = VideoExporter::FfmpegPipeline::Probe("");
		EXPECT(probe.available && !probe.ffprobePath.empty());

		struct PresentMockScreenshot
		{
			uint64_t armedToken{0};
			uint64_t readyToken{0};
			uint64_t immediateCalls{0};
			uint64_t presentConsumeCalls{0};
			bool forcePresentUnavailableOnSecond{false};
			uint64_t frameCounter{10};
			uint8_t fillSeed{32};

			GakuScreenshotServiceV1 Service()
			{
				GakuScreenshotServiceV1 s{};
				s.abiVersion = GAKU_SCREENSHOT_VERSION;
				s.structSize = sizeof(s);
				s.context = this;
				s.getCapabilities = [](void*, GakuScreenshotCapabilitiesV1* out) -> GakuStatus {
					out->abiVersion = GAKU_SCREENSHOT_VERSION;
					out->structSize = sizeof(*out);
					out->runtimeReady = 1;
					out->cameraAvailable = 1;
					out->maxDimension = 4096;
					out->windowWidth = 320;
					out->windowHeight = 180;
					return GAKU_OK;
				};
				s.captureFrameImmediate = [](void* ctx, const GakuScreenshotFrameRequestV1* req,
					void* outPixels, size_t capacity, GakuScreenshotFrameInfoV1* outInfo, char*, uint32_t) -> GakuStatus {
					auto* st = static_cast<PresentMockScreenshot*>(ctx);
					++st->immediateCalls;
					const size_t bytes = static_cast<size_t>(req->width) * req->height * 4u;
					if (capacity < bytes) return GAKU_E_SIZE;
					std::memset(outPixels, st->fillSeed++, bytes);
					outInfo->width = req->width;
					outInfo->height = req->height;
					outInfo->rowPitch = req->width * 4u;
					outInfo->hasAlpha = 0;
					outInfo->unityFrame = ++st->frameCounter;
					return GAKU_OK;
				};
				s.armPresentCapture = [](void* ctx, uint64_t token, uint32_t, uint32_t, uint32_t) -> GakuStatus {
					auto* st = static_cast<PresentMockScreenshot*>(ctx);
					st->armedToken = token;
					return GAKU_OK;
				};
				s.consumePresentCapture = [](void* ctx, uint64_t* outToken, void* outPixels, size_t capacity,
					GakuScreenshotFrameInfoV1* outInfo, char* errBuf, uint32_t errCap) -> GakuStatus {
					auto* st = static_cast<PresentMockScreenshot*>(ctx);
					if (st->forcePresentUnavailableOnSecond && st->presentConsumeCalls >= 1)
					{
						if (errBuf && errCap > 0) std::snprintf(errBuf, errCap, "Forced swapchain size mismatch");
						return GAKU_E_UNAVAILABLE;
					}
					if (st->readyToken == 0)
					{
						*outToken = 0;
						return GAKU_OK;
					}
					const size_t bytes = 320u * 180u * 4u;
					if (capacity < bytes) return GAKU_E_SIZE;
					std::memset(outPixels, st->fillSeed++, bytes);
					outInfo->width = 320;
					outInfo->height = 180;
					outInfo->rowPitch = 320u * 4u;
					outInfo->hasAlpha = 0;
					outInfo->unityFrame = ++st->frameCounter;
					*outToken = st->readyToken;
					st->readyToken = 0;
					++st->presentConsumeCalls;
					return GAKU_OK;
				};
				s.cancelPresentCapture = [](void* ctx) -> GakuStatus {
					auto* st = static_cast<PresentMockScreenshot*>(ctx);
					st->armedToken = 0;
					st->readyToken = 0;
					return GAKU_OK;
				};
				return s;
			}

			void SimulatePresent()
			{
				if (armedToken != 0)
				{
					readyToken = armedToken;
					armedToken = 0;
				}
			}
		};

		struct PresentMockClock
		{
			uint64_t leaseId{0};
			uint32_t playbackState{0};
			uint64_t evaluatedSerial{500};
			uint64_t notifyCount{0};

			GakuMotionClockServiceV1 Service()
			{
				GakuMotionClockServiceV1 c{};
				c.abiVersion = GAKU_MOTION_CLOCK_VERSION;
				c.structSize = sizeof(c);
				c.context = this;
				c.getSnapshot = [](void* ctx, GakuMotionClockSnapshotV1* out) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					out->abiVersion = GAKU_MOTION_CLOCK_VERSION;
					out->structSize = sizeof(*out);
					out->runMode = st->leaseId ? GAKUMOTION_RUN_RECORDING : GAKUMOTION_RUN_REALTIME;
					out->playbackState = st->playbackState;
					out->fpsNumerator = 30;
					out->fpsDenominator = 1;
					out->timelineDurationSeconds = 0.1; // 3 frames
					out->leaseId = st->leaseId;
					out->evaluatedFrameSerial = st->evaluatedSerial;
					std::snprintf(out->trackTitle, sizeof(out->trackTitle), "PresentTrack");
					return GAKU_OK;
				};
				c.acquireRecordingLease = [](void* ctx, const char*, uint32_t, uint32_t, uint64_t* outLease) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					st->leaseId = 91;
					*outLease = 91;
					return GAKU_OK;
				};
				c.releaseRecordingLease = [](void* ctx, uint64_t lease) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease) return GAKU_E_CONFLICT;
					st->leaseId = 0;
					return GAKU_OK;
				};
				c.recordingBegin = [](void* ctx, uint64_t lease, double) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease) return GAKU_E_CONFLICT;
					st->playbackState = 1;
					return GAKU_OK;
				};
				c.recordingStop = [](void* ctx, uint64_t lease) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease) return GAKU_E_CONFLICT;
					st->playbackState = 0;
					return GAKU_OK;
				};
				c.notifyFrameRendered = [](void* ctx, uint64_t lease, uint64_t, uint32_t) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease) return GAKU_E_CONFLICT;
					++st->notifyCount;
					return GAKU_OK;
				};
				return c;
			}
		};

		// A. Pure Present capture path: 0 captureFrameImmediate calls, 3 Present frames.
		{
			PresentMockScreenshot mockSs;
			PresentMockClock mockClk;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.SetDeferredCaptureMode(true);
			rec.InitializePaths(tmp.dir / L"cfg_pres.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","alphaMode":"opaque","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"present_pure.mp4");
			EXPECT(rec.RequestStart(err));

			rec.OnTick(); // Start session
			EXPECT(rec.IsRecording());

			for (int f = 0; f < 3; ++f)
			{
				++mockClk.evaluatedSerial; // MotionActorTick
				rec.OnEndOfFrameCapture(); // Arm Present
				EXPECT(mockSs.armedToken != 0);
				mockSs.SimulatePresent(); // Swapchain Present copies backbuffer
				rec.OnTick();             // Next frame OnTick consumes Present & steps clock
			}

			EXPECT(WaitForRecorderFinish(rec, 30000));
			const auto st = rec.GetStatus();
			EXPECT(st.phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(st.capturedFrames == 3);
			EXPECT(mockSs.presentConsumeCalls == 3);
			EXPECT(mockSs.immediateCalls == 0);
			EXPECT(mockClk.notifyCount == 3);
		}

		// B. Mid-session swapchain failure seamlessly falls back to offscreen Camera.Render without aborting.
		{
			PresentMockScreenshot mockSs;
			mockSs.forcePresentUnavailableOnSecond = true;
			PresentMockClock mockClk;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = mockClk.Service(); return true; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.SetDeferredCaptureMode(true);
			rec.InitializePaths(tmp.dir / L"cfg_fb.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","alphaMode":"opaque","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"present_fallback.mp4");
			EXPECT(rec.RequestStart(err));

			rec.OnTick();
			++mockClk.evaluatedSerial;
			rec.OnEndOfFrameCapture(); // Arm frame 0
			mockSs.SimulatePresent();
			rec.OnTick(); // Consumes frame 0 via Present

			++mockClk.evaluatedSerial;
			rec.OnEndOfFrameCapture(); // Arms frame 1
			mockSs.SimulatePresent();
			rec.OnTick(); // Consume sees GAKU_E_UNAVAILABLE -> falls back cleanly without step
			// Because fallback didn't consume frame 1, evaluatedSerial didn't need to advance yet:
			rec.OnEndOfFrameCapture(); // Captures frame 1 immediately via captureFrameImmediate!
			EXPECT(mockSs.immediateCalls == 1);

			++mockClk.evaluatedSerial;
			rec.OnEndOfFrameCapture(); // Captures frame 2 via captureFrameImmediate!
			EXPECT(mockSs.immediateCalls == 2);

			EXPECT(WaitForRecorderFinish(rec, 30000));
			const auto st = rec.GetStatus();
			EXPECT(st.phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(st.capturedFrames == 3);
			EXPECT(mockSs.presentConsumeCalls == 1);
			EXPECT(mockSs.immediateCalls == 2);
		}

		// C. When deferredCaptureMode is set but render-frame hook is unavailable (e.g. GakuVRify
		//    hook conflict with all fallbacks failing), OnTick drives swapchain-present AND
		//    refreshes evaluatedFrameSerial so strict GakuMotion NotifyFrameRendered never conflicts.
		{
			PresentMockScreenshot mockSs;
			PresentMockClock mockClk;
			auto strictClockService = [&]() {
				GakuMotionClockServiceV1 c = mockClk.Service();
				c.notifyFrameRendered = [](void* ctx, uint64_t lease, uint64_t serial, uint32_t) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease || st->evaluatedSerial != serial) return GAKU_E_CONFLICT;
					++st->notifyCount;
					return GAKU_OK;
				};
				return c;
			};

			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) { out = strictClockService(); return true; };
			hostCb.isRenderFrameHookUnavailable = []() { return true; };

			VideoExporter::VideoRecorder rec(hostCb);
			rec.SetDeferredCaptureMode(true);
			rec.InitializePaths(tmp.dir / L"cfg_unavail.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","alphaMode":"opaque","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"present_hook_unavail.mp4");
			EXPECT(rec.RequestStart(err));

			// Frame 0: OnTick starts session + drops deferred mode; MotionActorTick(0) bumps serial.
			rec.OnTick();
			EXPECT(rec.IsRecording());
			EXPECT(!rec.IsDeferredCaptureMode());
			++mockClk.evaluatedSerial;

			for (int f = 0; f < 3; ++f)
			{
				if (f > 0) ++mockClk.evaluatedSerial; // MotionActorTick evaluated next clock time
				rec.OnTick(); // Arms Present on OnTick
				EXPECT(mockSs.armedToken != 0);
				++mockClk.evaluatedSerial; // MotionActorTick runs AFTER OnTick in same Unity frame
				mockSs.SimulatePresent();  // Present completes at end of frame
				rec.OnTick();              // Consumes Present & calls notifyFrameRendered with latest serial
			}

			EXPECT(WaitForRecorderFinish(rec, 30000));
			const auto st = rec.GetStatus();
			EXPECT(st.phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(st.capturedFrames == 3);
			EXPECT(mockSs.presentConsumeCalls == 3);
			EXPECT(mockSs.immediateCalls == 0);
			EXPECT(mockClk.notifyCount == 3);
		}

		// D. Even if render-frame hook claimed active, silent OnEndOfFrameCapture falls back cleanly
		//    after 4 ticks without hanging at 0 frames (swapchain-present).
		{
			PresentMockScreenshot mockSs;
			PresentMockClock mockClk;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) {
				out = mockClk.Service();
				out.notifyFrameRendered = [](void* ctx, uint64_t lease, uint64_t serial, uint32_t) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease || st->evaluatedSerial != serial) return GAKU_E_CONFLICT;
					++st->notifyCount;
					return GAKU_OK;
				};
				return true;
			};

			VideoExporter::VideoRecorder rec(hostCb);
			rec.SetDeferredCaptureMode(true);
			rec.InitializePaths(tmp.dir / L"cfg_silent_pres.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":320,"height":180,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","alphaMode":"opaque","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"present_silent_fallback.mp4");
			EXPECT(rec.RequestStart(err));

			rec.OnTick(); // Start session
			++mockClk.evaluatedSerial;
			for (int t = 0; t < 4; ++t)
			{
				rec.OnTick(); // Watchdog trips on tick 4 and arms Present!
			}
			EXPECT(!rec.IsDeferredCaptureMode());
			EXPECT(mockSs.armedToken != 0);
			++mockClk.evaluatedSerial;
			mockSs.SimulatePresent();
			rec.OnTick(); // Consumes frame 0

			for (int f = 1; f < 3; ++f)
			{
				++mockClk.evaluatedSerial;
				rec.OnTick();
				EXPECT(mockSs.armedToken != 0);
				++mockClk.evaluatedSerial;
				mockSs.SimulatePresent();
				rec.OnTick();
			}

			EXPECT(WaitForRecorderFinish(rec, 30000));
			const auto st = rec.GetStatus();
			EXPECT(st.phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(st.capturedFrames == 3);
			EXPECT(mockSs.presentConsumeCalls == 3);
			EXPECT(mockSs.immediateCalls == 0);
			EXPECT(mockClk.notifyCount == 3);
		}

		// E. Silent OnEndOfFrameCapture with offscreen Camera.Render() (e.g. 4K / Green Screen Alpha)
		//    also recovers cleanly via OnTick without hanging at 0 frames.
		{
			PresentMockScreenshot mockSs;
			PresentMockClock mockClk;
			VideoExporter::ServiceHostCallbacks hostCb;
			hostCb.queryScreenshot = [&](GakuScreenshotServiceV1& out) { out = mockSs.Service(); return true; };
			hostCb.queryClock = [&](GakuMotionClockServiceV1& out) {
				out = mockClk.Service();
				out.notifyFrameRendered = [](void* ctx, uint64_t lease, uint64_t serial, uint32_t) -> GakuStatus {
					auto* st = static_cast<PresentMockClock*>(ctx);
					if (st->leaseId != lease || st->evaluatedSerial != serial) return GAKU_E_CONFLICT;
					++st->notifyCount;
					return GAKU_OK;
				};
				return true;
			};

			VideoExporter::VideoRecorder rec(hostCb);
			rec.SetDeferredCaptureMode(true);
			rec.InitializePaths(tmp.dir / L"cfg_silent_off.json", VideoExporter::util::WideToUtf8(tmp.dir.wstring()));
			GakuMotion::jsonlite::Value patch;
			std::string err;
			// 640x360 != window 320x180 -> offscreen Camera.Render path
			EXPECT(GakuMotion::jsonlite::ParseText(
				R"({"width":640,"height":360,"fps":30,"recordingMode":"lockstep_full","codec":"libx264","alphaMode":"force_transparent","muxAudio":false})",
				patch, err));
			EXPECT(rec.UpdateConfig(patch, err));
			rec.SetOutputPathOverrideForTest(tmp.dir / L"offscreen_silent_fallback.mp4");
			EXPECT(rec.RequestStart(err));

			rec.OnTick(); // Start session
			++mockClk.evaluatedSerial;
			for (int t = 0; t < 4; ++t)
			{
				rec.OnTick(); // Watchdog trips on tick 4 and captures frame 0 immediately
			}
			EXPECT(!rec.IsDeferredCaptureMode());
			EXPECT(mockSs.immediateCalls == 1);
			for (int f = 1; f < 3; ++f)
			{
				++mockClk.evaluatedSerial;
				rec.OnTick();
			}

			EXPECT(WaitForRecorderFinish(rec, 30000));
			const auto st = rec.GetStatus();
			EXPECT(st.phase == VideoExporter::RecorderPhase::Completed);
			EXPECT(st.capturedFrames == 3);
			EXPECT(mockSs.immediateCalls == 3);
			EXPECT(mockSs.presentConsumeCalls == 0);
			EXPECT(mockClk.notifyCount == 3);
		}
	}
} // namespace

int main()
{
	std::cout << "Running VideoExporter unit and real 4K FFmpeg E2E tests...\n";
	TestConfigNormalizationAndPersistence();
	TestFfmpegProbe();
	TestReal4kLockstepMp4WithAudioAndFilter();
	TestReal4kAlphaProResMov();
	TestVp9AlphaAndRingBackpressure();
	TestLockstepSerialHoldAndFreeRun();
	TestWebPanelBridgeContract();
	TestPluginDllAbiAndLazyServices();
	TestWakeControlDeduplicationAndDeferredEndOfFrameCapture();
	TestSwapchainPresentPathAndFallback();

	if (g_failures == 0)
	{
		std::cout << "All VideoExporter C++ & 4K FFmpeg E2E tests PASSED.\n";
		return 0;
	}
	std::cerr << g_failures << " test check(s) FAILED.\n";
	return 1;
}
