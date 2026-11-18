// SPDX-License-Identifier: MIT
#pragma once

#include "../include/GakuScreenshot.h"
#include "CommonUtils.hpp"
#include "GkJsonLite.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace VideoExporter
{
	inline constexpr uint32_t kMinVideoDim = 64u;
	inline constexpr uint32_t kMaxVideoDim = 4096u;
	inline constexpr uint64_t kMaxVideoPixels = 4096ULL * 2160ULL;
	inline constexpr uint32_t kMinFps = 1u;
	inline constexpr uint32_t kMaxFps = 240u;
	inline constexpr int kMinCrf = 14;
	inline constexpr int kMaxCrf = 28;

	struct RecorderConfig
	{
		uint32_t width{1920};
		uint32_t height{1080};
		uint32_t fps{60};
		std::string recordingMode{"lockstep_full"}; // lockstep_full | lockstep_range | freerun
		double rangeStartSeconds{0.0};
		double rangeEndSeconds{10.0};
		std::string codec{"auto"}; // auto | h264_nvenc | hevc_nvenc | libx264 | libx265 | prores_ks | libvpx-vp9
		std::string pixelFormat{"yuv420p"}; // yuv420p | yuv444p (for opaque mp4)
		std::string alphaMode{"auto"}; // auto | greenscreen | force_transparent | opaque
		int crf{18};
		bool muxAudio{true};
		bool bakeAudioEffects{true};
		std::string postFilterPreset{"none"}; // none | sharpen | vivid | motion_smooth | custom
		std::string customVideoFilter;
		std::string extraFfmpegArgs;
		std::string ffmpegPathOverride;
		std::string outputDir;
	};

	inline bool IsValidRecordingMode(std::string_view mode) noexcept
	{
		return mode == "lockstep_full" || mode == "lockstep_range" || mode == "freerun";
	}

	inline bool IsValidCodec(std::string_view codec) noexcept
	{
		return codec == "auto" || codec == "h264_nvenc" || codec == "hevc_nvenc" ||
			codec == "libx264" || codec == "libx265" || codec == "prores_ks" || codec == "libvpx-vp9";
	}

	inline bool IsAlphaCodec(std::string_view codec) noexcept
	{
		return codec == "prores_ks" || codec == "libvpx-vp9";
	}

	inline bool IsValidPixelFormat(std::string_view fmt) noexcept
	{
		return fmt == "yuv420p" || fmt == "yuv444p";
	}

	inline bool IsValidAlphaMode(std::string_view mode) noexcept
	{
		return mode == "auto" || mode == "greenscreen" || mode == "force_transparent" || mode == "opaque";
	}

	inline bool IsValidPostFilterPreset(std::string_view preset) noexcept
	{
		return preset == "none" || preset == "sharpen" || preset == "vivid" ||
			preset == "motion_smooth" || preset == "custom";
	}

	inline std::string ContainerExtensionForCodec(std::string_view codec)
	{
		if (codec == "prores_ks") return ".mov";
		if (codec == "libvpx-vp9") return ".webm";
		return ".mp4";
	}

	inline uint32_t ResolveScreenshotAlphaMode(const RecorderConfig& cfg) noexcept
	{
		if (cfg.alphaMode == "opaque" || !IsAlphaCodec(cfg.codec))
		{
			return GAKU_SCREENSHOT_ALPHA_OPAQUE;
		}
		if (cfg.alphaMode == "force_transparent")
		{
			return GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT;
		}
		return GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN;
	}

	inline uint32_t ClampEvenDimension(uint32_t value) noexcept
	{
		uint32_t clamped = std::clamp(value, kMinVideoDim, kMaxVideoDim);
		clamped &= ~1u;
		return (std::max)(clamped, kMinVideoDim);
	}

	inline void NormalizeConfig(RecorderConfig& cfg, const std::string& defaultOutputDir)
	{
		cfg.width = ClampEvenDimension(cfg.width);
		cfg.height = ClampEvenDimension(cfg.height);
		while (static_cast<uint64_t>(cfg.width) * static_cast<uint64_t>(cfg.height) > kMaxVideoPixels)
		{
			const double scale = std::sqrt(
				static_cast<double>(kMaxVideoPixels) /
				(static_cast<double>(cfg.width) * static_cast<double>(cfg.height)));
			const uint32_t nextW = ClampEvenDimension(static_cast<uint32_t>(std::floor(cfg.width * scale)));
			const uint32_t nextH = ClampEvenDimension(static_cast<uint32_t>(std::floor(cfg.height * scale)));
			if (nextW == cfg.width && nextH == cfg.height)
			{
				if (cfg.width > cfg.height) cfg.width -= 2u;
				else cfg.height -= 2u;
			}
			else
			{
				cfg.width = nextW;
				cfg.height = nextH;
			}
		}

		cfg.fps = std::clamp(cfg.fps, kMinFps, kMaxFps);
		if (!IsValidRecordingMode(cfg.recordingMode)) cfg.recordingMode = "lockstep_full";
		if (!std::isfinite(cfg.rangeStartSeconds) || cfg.rangeStartSeconds < 0.0) cfg.rangeStartSeconds = 0.0;
		if (!std::isfinite(cfg.rangeEndSeconds) || cfg.rangeEndSeconds <= cfg.rangeStartSeconds)
		{
			cfg.rangeEndSeconds = cfg.rangeStartSeconds + 10.0;
		}
		if (!IsValidCodec(cfg.codec)) cfg.codec = "auto";
		if (!IsValidPixelFormat(cfg.pixelFormat)) cfg.pixelFormat = "yuv420p";
		if (!IsValidAlphaMode(cfg.alphaMode)) cfg.alphaMode = "auto";
		cfg.crf = std::clamp(cfg.crf, kMinCrf, kMaxCrf);
		if (!IsValidPostFilterPreset(cfg.postFilterPreset)) cfg.postFilterPreset = "none";
		if (cfg.outputDir.empty()) cfg.outputDir = defaultOutputDir;
	}

	inline std::string ConfigToJson(const RecorderConfig& cfg)
	{
		std::string out = "{";
		out += "\"version\":1";
		out += ",\"width\":" + util::JUInt(cfg.width);
		out += ",\"height\":" + util::JUInt(cfg.height);
		out += ",\"fps\":" + util::JUInt(cfg.fps);
		out += ",\"recordingMode\":" + util::JString(cfg.recordingMode);
		out += ",\"rangeStartSeconds\":" + util::JNum(cfg.rangeStartSeconds);
		out += ",\"rangeEndSeconds\":" + util::JNum(cfg.rangeEndSeconds);
		out += ",\"codec\":" + util::JString(cfg.codec);
		out += ",\"pixelFormat\":" + util::JString(cfg.pixelFormat);
		out += ",\"alphaMode\":" + util::JString(cfg.alphaMode);
		out += ",\"crf\":" + util::JNum(cfg.crf);
		out += ",\"muxAudio\":" + util::JBool(cfg.muxAudio);
		out += ",\"bakeAudioEffects\":" + util::JBool(cfg.bakeAudioEffects);
		out += ",\"postFilterPreset\":" + util::JString(cfg.postFilterPreset);
		out += ",\"customVideoFilter\":" + util::JString(cfg.customVideoFilter);
		out += ",\"extraFfmpegArgs\":" + util::JString(cfg.extraFfmpegArgs);
		out += ",\"ffmpegPathOverride\":" + util::JString(cfg.ffmpegPathOverride);
		out += ",\"outputDir\":" + util::JString(cfg.outputDir);
		out += "}";
		return out;
	}

	inline bool ApplyConfigPatch(const GakuMotion::jsonlite::Value& patch, RecorderConfig& target,
		const std::string& defaultOutputDir, std::string& error)
	{
		if (!patch.IsObject())
		{
			error = "Config payload must be a JSON object";
			return false;
		}
		RecorderConfig next = target;

		auto readInt = [&](const char* key, int64_t& out, bool& present) -> bool {
			present = false;
			const auto* v = patch.Find(key);
			if (!v) return true;
			if (!v->IsNumber() || !std::isfinite(v->numberValue) || v->numberValue != std::floor(v->numberValue))
			{
				error = std::string(key) + " must be an integer";
				return false;
			}
			out = static_cast<int64_t>(v->numberValue);
			present = true;
			return true;
		};
		auto readNum = [&](const char* key, double& out, bool& present) -> bool {
			present = false;
			const auto* v = patch.Find(key);
			if (!v) return true;
			if (!v->IsNumber() || !std::isfinite(v->numberValue))
			{
				error = std::string(key) + " must be a finite number";
				return false;
			}
			out = v->numberValue;
			present = true;
			return true;
		};
		auto readBool = [&](const char* key, bool& out, bool& present) -> bool {
			present = false;
			const auto* v = patch.Find(key);
			if (!v) return true;
			if (!v->IsBool())
			{
				error = std::string(key) + " must be a boolean";
				return false;
			}
			out = v->boolValue;
			present = true;
			return true;
		};
		auto readStr = [&](const char* key, std::string& out, bool& present) -> bool {
			present = false;
			const auto* v = patch.Find(key);
			if (!v) return true;
			if (!v->IsString())
			{
				error = std::string(key) + " must be a string";
				return false;
			}
			out = v->stringValue;
			present = true;
			return true;
		};

		int64_t iv = 0;
		double dv = 0.0;
		bool bv = false;
		std::string sv;
		bool has = false;

		if (!readInt("width", iv, has)) return false;
		if (has)
		{
			if (iv < static_cast<int64_t>(kMinVideoDim) || iv > static_cast<int64_t>(kMaxVideoDim) || (iv & 1) != 0)
			{
				error = "width must be an even integer in [64, 4096]";
				return false;
			}
			next.width = static_cast<uint32_t>(iv);
		}
		if (!readInt("height", iv, has)) return false;
		if (has)
		{
			if (iv < static_cast<int64_t>(kMinVideoDim) || iv > static_cast<int64_t>(kMaxVideoDim) || (iv & 1) != 0)
			{
				error = "height must be an even integer in [64, 4096]";
				return false;
			}
			next.height = static_cast<uint32_t>(iv);
		}
		if (static_cast<uint64_t>(next.width) * static_cast<uint64_t>(next.height) > kMaxVideoPixels)
		{
			error = "Total resolution exceeds 4K pixel budget (4096x2160)";
			return false;
		}
		if (!readInt("fps", iv, has)) return false;
		if (has)
		{
			if (iv < static_cast<int64_t>(kMinFps) || iv > static_cast<int64_t>(kMaxFps))
			{
				error = "fps must be in [1, 240]";
				return false;
			}
			next.fps = static_cast<uint32_t>(iv);
		}
		if (!readStr("recordingMode", sv, has)) return false;
		if (has)
		{
			if (!IsValidRecordingMode(sv))
			{
				error = "Unsupported recordingMode: " + sv;
				return false;
			}
			next.recordingMode = std::move(sv);
		}
		if (!readNum("rangeStartSeconds", dv, has)) return false;
		if (has)
		{
			if (dv < 0.0)
			{
				error = "rangeStartSeconds must be >= 0";
				return false;
			}
			next.rangeStartSeconds = dv;
		}
		if (!readNum("rangeEndSeconds", dv, has)) return false;
		if (has)
		{
			if (dv <= 0.0)
			{
				error = "rangeEndSeconds must be > 0";
				return false;
			}
			next.rangeEndSeconds = dv;
		}
		if (next.recordingMode == "lockstep_range" && next.rangeEndSeconds <= next.rangeStartSeconds)
		{
			error = "rangeEndSeconds must be greater than rangeStartSeconds";
			return false;
		}
		if (!readStr("codec", sv, has)) return false;
		if (has)
		{
			if (!IsValidCodec(sv))
			{
				error = "Unsupported codec: " + sv;
				return false;
			}
			next.codec = std::move(sv);
		}
		if (!readStr("pixelFormat", sv, has)) return false;
		if (has)
		{
			if (!IsValidPixelFormat(sv))
			{
				error = "Unsupported pixelFormat: " + sv;
				return false;
			}
			next.pixelFormat = std::move(sv);
		}
		if (!readStr("alphaMode", sv, has)) return false;
		if (has)
		{
			if (!IsValidAlphaMode(sv))
			{
				error = "Unsupported alphaMode: " + sv;
				return false;
			}
			next.alphaMode = std::move(sv);
		}
		if (!readInt("crf", iv, has)) return false;
		if (has)
		{
			if (iv < kMinCrf || iv > kMaxCrf)
			{
				error = "crf must be in [14, 28]";
				return false;
			}
			next.crf = static_cast<int>(iv);
		}
		if (!readBool("muxAudio", bv, has)) return false;
		if (has) next.muxAudio = bv;
		if (!readBool("bakeAudioEffects", bv, has)) return false;
		if (has) next.bakeAudioEffects = bv;
		if (!readStr("postFilterPreset", sv, has)) return false;
		if (has)
		{
			if (!IsValidPostFilterPreset(sv))
			{
				error = "Unsupported postFilterPreset: " + sv;
				return false;
			}
			next.postFilterPreset = std::move(sv);
		}
		if (!readStr("customVideoFilter", sv, has)) return false;
		if (has) next.customVideoFilter = std::move(sv);
		if (!readStr("extraFfmpegArgs", sv, has)) return false;
		if (has) next.extraFfmpegArgs = std::move(sv);
		if (!readStr("ffmpegPathOverride", sv, has)) return false;
		if (has) next.ffmpegPathOverride = std::move(sv);
		if (!readStr("outputDir", sv, has)) return false;
		if (has)
		{
			next.outputDir = sv.empty() ? defaultOutputDir : sv;
		}

		NormalizeConfig(next, defaultOutputDir);
		target = std::move(next);
		return true;
	}

	inline bool LoadConfigFile(const std::filesystem::path& path, RecorderConfig& cfg,
		const std::string& defaultOutputDir, std::string& error)
	{
		cfg = RecorderConfig{};
		NormalizeConfig(cfg, defaultOutputDir);
		if (path.empty()) return true;

		bool readOk = false;
		const std::string text = util::ReadFileUtf8(path, readOk);
		if (!readOk || text.empty()) return false;

		GakuMotion::jsonlite::Value root;
		if (!GakuMotion::jsonlite::ParseText(text, root, error) || !root.IsObject())
		{
			return false;
		}

		if (const auto* v = root.Find("width"); v && v->IsNumber()) cfg.width = static_cast<uint32_t>(v->AsInt(static_cast<int>(cfg.width)));
		if (const auto* v = root.Find("height"); v && v->IsNumber()) cfg.height = static_cast<uint32_t>(v->AsInt(static_cast<int>(cfg.height)));
		if (const auto* v = root.Find("fps"); v && v->IsNumber()) cfg.fps = static_cast<uint32_t>(v->AsInt(static_cast<int>(cfg.fps)));
		if (const auto* v = root.Find("recordingMode"); v && v->IsString()) cfg.recordingMode = v->stringValue;
		if (const auto* v = root.Find("rangeStartSeconds"); v && v->IsNumber()) cfg.rangeStartSeconds = v->AsDouble(cfg.rangeStartSeconds);
		if (const auto* v = root.Find("rangeEndSeconds"); v && v->IsNumber()) cfg.rangeEndSeconds = v->AsDouble(cfg.rangeEndSeconds);
		if (const auto* v = root.Find("codec"); v && v->IsString()) cfg.codec = v->stringValue;
		if (const auto* v = root.Find("pixelFormat"); v && v->IsString()) cfg.pixelFormat = v->stringValue;
		if (const auto* v = root.Find("alphaMode"); v && v->IsString()) cfg.alphaMode = v->stringValue;
		if (const auto* v = root.Find("crf"); v && v->IsNumber()) cfg.crf = v->AsInt(cfg.crf);
		if (const auto* v = root.Find("muxAudio"); v) cfg.muxAudio = v->AsBool(cfg.muxAudio);
		if (const auto* v = root.Find("bakeAudioEffects"); v) cfg.bakeAudioEffects = v->AsBool(cfg.bakeAudioEffects);
		if (const auto* v = root.Find("postFilterPreset"); v && v->IsString()) cfg.postFilterPreset = v->stringValue;
		if (const auto* v = root.Find("customVideoFilter"); v && v->IsString()) cfg.customVideoFilter = v->stringValue;
		if (const auto* v = root.Find("extraFfmpegArgs"); v && v->IsString()) cfg.extraFfmpegArgs = v->stringValue;
		if (const auto* v = root.Find("ffmpegPathOverride"); v && v->IsString()) cfg.ffmpegPathOverride = v->stringValue;
		if (const auto* v = root.Find("outputDir"); v && v->IsString()) cfg.outputDir = v->stringValue;

		NormalizeConfig(cfg, defaultOutputDir);
		return true;
	}

	inline bool SaveConfigFile(const std::filesystem::path& path, const RecorderConfig& cfg, std::string& error)
	{
		if (path.empty())
		{
			error = "Config path is not configured";
			return false;
		}
		return util::WriteFileAtomicUtf8(path, ConfigToJson(cfg) + "\n", error);
	}
} // namespace VideoExporter
