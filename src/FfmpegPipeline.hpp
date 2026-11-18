// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../include/GakuAudio.h"
#include "CommonUtils.hpp"
#include "Config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace VideoExporter
{
	struct EncoderAvailability
	{
		bool h264_nvenc{false};
		bool hevc_nvenc{false};
		bool libx264{false};
		bool libx265{false};
		bool prores_ks{false};
		bool libvpx_vp9{false};
	};

	struct FfmpegProbeResult
	{
		bool available{false};
		bool nvencRuntimeVerified{false};
		std::wstring ffmpegPath;
		std::wstring ffprobePath;
		std::string ffmpegPathUtf8;
		std::string ffprobePathUtf8;
		std::string version;
		EncoderAvailability encoders;
		std::string error;
	};

	struct AudioTrackInfo
	{
		bool hasAudio{false};
		std::string audioFilePath;
		std::string activeAudioEffectId;
		double audioOffsetSeconds{0.0};
		double audioSpeed{1.0};
		double playbackSpeed{1.0};
		float musicVolume{1.0f};
		double startSeconds{0.0};
		double recordedDurationSeconds{0.0};
	};

	struct CapturePipeOptions
	{
		uint32_t width{1920};
		uint32_t height{1080};
		uint32_t fpsNum{60};
		uint32_t fpsDen{1};
		std::string requestedCodec{"auto"};
		std::string pixelFormat{"yuv420p"};
		int crf{18};
		std::filesystem::path tempVideoPath;
		std::string ffmpegPathOverride;
	};

	struct PostProcessOptions
	{
		RecorderConfig config;
		std::string resolvedCodec;
		std::filesystem::path tempVideoPath;
		std::filesystem::path finalOutputPath;
		std::filesystem::path tempWavPath;
		AudioTrackInfo audio;
		const GakuAudioServiceV1* audioService{nullptr};
	};

	class FfmpegPipeline
	{
	public:
		FfmpegPipeline() = default;
		~FfmpegPipeline() { CancelAndJoin(); }

		FfmpegPipeline(const FfmpegPipeline&) = delete;
		FfmpegPipeline& operator=(const FfmpegPipeline&) = delete;

		static std::wstring QuoteWin32Arg(std::wstring_view arg)
		{
			if (arg.empty()) return L"\"\"";
			const bool needsQuotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
			if (!needsQuotes) return std::wstring(arg);

			std::wstring out;
			out.push_back(L'"');
			for (size_t i = 0; i < arg.size(); ++i)
			{
				size_t backslashes = 0;
				while (i < arg.size() && arg[i] == L'\\')
				{
					++backslashes;
					++i;
				}
				if (i == arg.size())
				{
					out.append(backslashes * 2, L'\\');
					break;
				}
				if (arg[i] == L'"')
				{
					out.append(backslashes * 2 + 1, L'\\');
					out.push_back(L'"');
				}
				else
				{
					out.append(backslashes, L'\\');
					out.push_back(arg[i]);
				}
			}
			out.push_back(L'"');
			return out;
		}

		static std::wstring BuildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args)
		{
			std::wstring cmd = QuoteWin32Arg(exe);
			for (const auto& a : args)
			{
				cmd.push_back(L' ');
				cmd += QuoteWin32Arg(a);
			}
			return cmd;
		}

		static std::vector<std::string> SplitArgsString(std::string_view input)
		{
			std::vector<std::string> tokens;
			std::string cur;
			bool inToken = false;
			bool inSingle = false;
			bool inDouble = false;

			for (size_t i = 0; i < input.size(); ++i)
			{
				const char ch = input[i];
				if (inSingle)
				{
					if (ch == '\'') inSingle = false;
					else cur.push_back(ch);
					inToken = true;
					continue;
				}
				if (inDouble)
				{
					if (ch == '"' ) inDouble = false;
					else if (ch == '\\' && i + 1 < input.size() && (input[i + 1] == '"' || input[i + 1] == '\\'))
					{
						cur.push_back(input[++i]);
					}
					else cur.push_back(ch);
					inToken = true;
					continue;
				}
				if (std::isspace(static_cast<unsigned char>(ch)))
				{
					if (inToken)
					{
						tokens.push_back(std::move(cur));
						cur.clear();
						inToken = false;
					}
					continue;
				}
				inToken = true;
				if (ch == '\'') inSingle = true;
				else if (ch == '"') inDouble = true;
				else cur.push_back(ch);
			}
			if (inToken) tokens.push_back(std::move(cur));
			return tokens;
		}

		static bool ResolveExecutables(const std::string& configuredPathUtf8,
			std::wstring& outFfmpeg, std::wstring& outFfprobe, std::string& error)
		{
			outFfmpeg.clear();
			outFfprobe.clear();
			error.clear();

			std::error_code ec;
			if (!configuredPathUtf8.empty())
			{
				std::filesystem::path p(util::Utf8ToWide(configuredPathUtf8));
				if (std::filesystem::is_directory(p, ec))
				{
					p /= L"ffmpeg.exe";
				}
				if (std::filesystem::is_regular_file(p, ec))
				{
					outFfmpeg = std::filesystem::weakly_canonical(p, ec).wstring();
					if (outFfmpeg.empty()) outFfmpeg = p.wstring();
				}
				else
				{
					error = "Configured ffmpeg path does not exist: " + configuredPathUtf8;
					return false;
				}
			}
			else
			{
				wchar_t found[32768]{};
				const DWORD len = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr);
				if (len > 0 && len < std::size(found))
				{
					outFfmpeg.assign(found, found + len);
				}
				else
				{
					error = "ffmpeg.exe was not found on PATH and no override path is configured.";
					return false;
				}
			}

			const std::filesystem::path siblingProbe = std::filesystem::path(outFfmpeg).parent_path() / L"ffprobe.exe";
			if (std::filesystem::is_regular_file(siblingProbe, ec))
			{
				outFfprobe = siblingProbe.wstring();
			}
			else
			{
				wchar_t probeBuf[32768]{};
				const DWORD pLen = SearchPathW(nullptr, L"ffprobe.exe", nullptr, static_cast<DWORD>(std::size(probeBuf)), probeBuf, nullptr);
				if (pLen > 0 && pLen < std::size(probeBuf))
				{
					outFfprobe.assign(probeBuf, probeBuf + pLen);
				}
			}
			return true;
		}

		static bool RunProcessCapture(const std::wstring& exe, const std::vector<std::wstring>& args,
			uint32_t timeoutMs, DWORD& outExitCode, std::string& outOutput, std::string& outError,
			const std::atomic<bool>* cancelFlag = nullptr)
		{
			outExitCode = 1;
			outOutput.clear();
			outError.clear();

			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;

			HANDLE readPipe = nullptr;
			HANDLE writePipe = nullptr;
			if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
			{
				outError = "CreatePipe failed (Win32=" + std::to_string(GetLastError()) + ")";
				return false;
			}
			SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			si.hStdInput = nullptr;
			si.hStdOutput = writePipe;
			si.hStdError = writePipe;

			PROCESS_INFORMATION pi{};
			std::wstring cmd = BuildCommandLine(exe, args);
			if (!CreateProcessW(
				exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
			{
				const DWORD err = GetLastError();
				CloseHandle(readPipe);
				CloseHandle(writePipe);
				outError = "CreateProcessW failed (Win32=" + std::to_string(err) + ")";
				return false;
			}

			CloseHandle(writePipe);
			CloseHandle(pi.hThread);

			std::string captured;
			std::thread reader([&]() {
				char buf[4096];
				DWORD bytesRead = 0;
				while (ReadFile(readPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0)
				{
					if (captured.size() < 2u * 1024u * 1024u)
					{
						const size_t keep = (std::min<size_t>)(bytesRead, 2u * 1024u * 1024u - captured.size());
						captured.append(buf, keep);
					}
				}
			});

			bool timedOut = false;
			bool cancelled = false;
			const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : (GetTickCount64() + timeoutMs);
			for (;;)
			{
				if (cancelFlag && cancelFlag->load(std::memory_order_acquire))
				{
					cancelled = true;
					TerminateProcess(pi.hProcess, 1);
					break;
				}
				const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
				if (wait == WAIT_OBJECT_0) break;
				if (timeoutMs != INFINITE && GetTickCount64() >= deadline)
				{
					timedOut = true;
					TerminateProcess(pi.hProcess, 1);
					break;
				}
			}

			WaitForSingleObject(pi.hProcess, 2000);
			GetExitCodeProcess(pi.hProcess, &outExitCode);
			CloseHandle(pi.hProcess);
			if (reader.joinable()) reader.join();
			CloseHandle(readPipe);

			outOutput = std::move(captured);
			if (cancelled)
			{
				outError = "Process cancelled";
				return false;
			}
			if (timedOut)
			{
				outError = "Process timed out";
				return false;
			}
			return true;
		}

		static FfmpegProbeResult Probe(const std::string& configuredPathUtf8)
		{
			FfmpegProbeResult res;
			if (!ResolveExecutables(configuredPathUtf8, res.ffmpegPath, res.ffprobePath, res.error))
			{
				return res;
			}
			res.ffmpegPathUtf8 = util::WideToUtf8(res.ffmpegPath);
			res.ffprobePathUtf8 = util::WideToUtf8(res.ffprobePath);

			DWORD code = 0;
			std::string out;
			std::string err;
			if (!RunProcessCapture(res.ffmpegPath, {L"-version"}, 10000, code, out, err) || code != 0)
			{
				res.error = err.empty() ? ("ffmpeg -version failed with code " + std::to_string(code)) : err;
				return res;
			}

			{
				std::istringstream iss(out);
				std::string line;
				if (std::getline(iss, line))
				{
					while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
					res.version = line;
				}
			}

			std::string encOut;
			if (RunProcessCapture(res.ffmpegPath, {L"-hide_banner", L"-encoders"}, 10000, code, encOut, err) && code == 0)
			{
				std::istringstream iss(encOut);
				std::string line;
				while (std::getline(iss, line))
				{
					// Format: " V....D libx264              ..."
					std::istringstream lss(line);
					std::string flags, name;
					if (!(lss >> flags >> name)) continue;
					if (flags.size() < 6 || flags[0] != 'V') continue;
					if (name == "h264_nvenc") res.encoders.h264_nvenc = true;
					else if (name == "hevc_nvenc") res.encoders.hevc_nvenc = true;
					else if (name == "libx264") res.encoders.libx264 = true;
					else if (name == "libx265") res.encoders.libx265 = true;
					else if (name == "prores_ks") res.encoders.prores_ks = true;
					else if (name == "libvpx-vp9") res.encoders.libvpx_vp9 = true;
				}
			}

			if (res.encoders.h264_nvenc)
			{
				DWORD nvCode = 1;
				std::string nvOut, nvErr;
				const bool ran = RunProcessCapture(
					res.ffmpegPath,
					{L"-hide_banner", L"-loglevel", L"error", L"-f", L"lavfi", L"-i",
					 L"color=black:s=256x256:d=0.04", L"-frames:v", L"1",
					 L"-c:v", L"h264_nvenc", L"-f", L"null", L"-"},
					8000, nvCode, nvOut, nvErr);
				res.nvencRuntimeVerified = ran && (nvCode == 0);
			}

			res.available = true;
			return res;
		}

		static std::string ResolveEffectiveCodec(const std::string& requested, const FfmpegProbeResult& probe)
		{
			if (requested != "auto" && IsValidCodec(requested)) return requested;
			if (probe.encoders.h264_nvenc && probe.nvencRuntimeVerified) return "h264_nvenc";
			if (probe.encoders.libx264) return "libx264";
			if (probe.encoders.hevc_nvenc && probe.nvencRuntimeVerified) return "hevc_nvenc";
			if (probe.encoders.libx265) return "libx265";
			return "libx264";
		}

		static std::string ResolveVideoFilter(const RecorderConfig& cfg)
		{
			std::string filter;
			if (cfg.postFilterPreset == "sharpen")
			{
				filter = "unsharp=5:5:0.75:5:5:0.0";
			}
			else if (cfg.postFilterPreset == "vivid")
			{
				filter = "eq=contrast=1.06:saturation=1.15:brightness=0.01";
			}
			else if (cfg.postFilterPreset == "motion_smooth")
			{
				filter = "tblend=all_mode=average";
			}
			else if (cfg.postFilterPreset == "custom")
			{
				filter = cfg.customVideoFilter;
			}

			if (cfg.postFilterPreset != "custom" && !cfg.customVideoFilter.empty())
			{
				if (!filter.empty()) filter += ",";
				filter += cfg.customVideoFilter;
			}
			return filter;
		}

		static void AppendVideoEncoderArgs(std::vector<std::wstring>& args, const std::string& codec,
			const std::string& pixelFormat, int crf)
		{
			const std::wstring crfStr = std::to_wstring(std::clamp(crf, kMinCrf, kMaxCrf));
			const std::wstring pixFmt = util::Utf8ToWide(IsValidPixelFormat(pixelFormat) ? pixelFormat : "yuv420p");

			if (codec == "h264_nvenc")
			{
				args.insert(args.end(), {L"-c:v", L"h264_nvenc", L"-preset", L"p5", L"-rc", L"vbr",
					L"-cq", crfStr, L"-b:v", L"0", L"-pix_fmt", pixFmt});
			}
			else if (codec == "hevc_nvenc")
			{
				args.insert(args.end(), {L"-c:v", L"hevc_nvenc", L"-preset", L"p5", L"-rc", L"vbr",
					L"-cq", crfStr, L"-b:v", L"0", L"-pix_fmt", pixFmt, L"-tag:v", L"hvc1"});
			}
			else if (codec == "libx265")
			{
				args.insert(args.end(), {L"-c:v", L"libx265", L"-preset", L"medium",
					L"-crf", crfStr, L"-pix_fmt", pixFmt, L"-tag:v", L"hvc1"});
			}
			else if (codec == "prores_ks")
			{
				args.insert(args.end(), {L"-c:v", L"prores_ks", L"-profile:v", L"4444",
					L"-pix_fmt", L"yuva444p10le"});
			}
			else if (codec == "libvpx-vp9")
			{
				args.insert(args.end(), {L"-c:v", L"libvpx-vp9", L"-crf", crfStr, L"-b:v", L"0",
					L"-pix_fmt", L"yuva420p", L"-auto-alt-ref", L"0", L"-row-mt", L"1",
					L"-deadline", L"good", L"-cpu-used", L"2"});
			}
			else
			{
				args.insert(args.end(), {L"-c:v", L"libx264", L"-preset", L"medium",
					L"-crf", crfStr, L"-pix_fmt", pixFmt});
			}
		}

		// ---- Stage A: Live Streaming Capture Pipe --------------------------

		bool StartCapturePipe(const CapturePipeOptions& opts, const FfmpegProbeResult& probe,
			std::string& outResolvedCodec, std::string& error)
		{
			CancelAndJoin();
			error.clear();
			encodedFrames_.store(0, std::memory_order_release);
			cancelRequested_.store(false, std::memory_order_release);
			{
				std::lock_guard<std::mutex> lock(stderrMutex_);
				stderrTail_.clear();
			}

			if (!probe.available || probe.ffmpegPath.empty())
			{
				error = probe.error.empty() ? "FFmpeg is not available" : probe.error;
				return false;
			}
			if (opts.width == 0 || opts.height == 0 || opts.fpsNum == 0 || opts.fpsDen == 0)
			{
				error = "Invalid capture dimensions or frame rate";
				return false;
			}

			std::error_code ec;
			if (!opts.tempVideoPath.parent_path().empty())
			{
				std::filesystem::create_directories(opts.tempVideoPath.parent_path(), ec);
				if (ec)
				{
					error = "Failed to create output directory: " + ec.message();
					return false;
				}
			}

			outResolvedCodec = ResolveEffectiveCodec(opts.requestedCodec, probe);
			const std::string ext = ContainerExtensionForCodec(outResolvedCodec);

			std::vector<std::wstring> args{
				L"-y", L"-hide_banner", L"-loglevel", L"info", L"-stats", L"-stats_period", L"0.2",
				L"-f", L"rawvideo", L"-pix_fmt", L"rgba",
				L"-s", std::to_wstring(opts.width) + L"x" + std::to_wstring(opts.height),
				L"-r", std::to_wstring(opts.fpsNum) + L"/" + std::to_wstring(opts.fpsDen),
				L"-i", L"-"
			};
			AppendVideoEncoderArgs(args, outResolvedCodec, opts.pixelFormat, opts.crf);
			if (ext == ".mp4")
			{
				args.insert(args.end(), {L"-movflags", L"+faststart"});
			}
			args.push_back(opts.tempVideoPath.wstring());

			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;

			HANDLE stdinRead = nullptr, stdinWrite = nullptr;
			HANDLE stderrRead = nullptr, stderrWrite = nullptr;
			if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 4u * 1024u * 1024u))
			{
				error = "CreatePipe(stdin) failed (Win32=" + std::to_string(GetLastError()) + ")";
				return false;
			}
			if (!CreatePipe(&stderrRead, &stderrWrite, &sa, 0))
			{
				const DWORD e = GetLastError();
				CloseHandle(stdinRead);
				CloseHandle(stdinWrite);
				error = "CreatePipe(stderr) failed (Win32=" + std::to_string(e) + ")";
				return false;
			}
			SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
			SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			si.hStdInput = stdinRead;
			si.hStdOutput = stderrWrite;
			si.hStdError = stderrWrite;

			PROCESS_INFORMATION pi{};
			std::wstring cmd = BuildCommandLine(probe.ffmpegPath, args);
			if (!CreateProcessW(
				probe.ffmpegPath.c_str(), cmd.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
			{
				const DWORD err = GetLastError();
				CloseHandle(stdinRead);
				CloseHandle(stdinWrite);
				CloseHandle(stderrRead);
				CloseHandle(stderrWrite);
				error = "Failed to launch ffmpeg.exe (Win32=" + std::to_string(err) + ")";
				return false;
			}

			CloseHandle(stdinRead);
			CloseHandle(stderrWrite);
			CloseHandle(pi.hThread);

			{
				std::lock_guard<std::mutex> pLock(processMutex_);
				process_ = pi.hProcess;
				stdinWrite_ = stdinWrite;
				stderrRead_ = stderrRead;
			}
			tempVideoPath_ = opts.tempVideoPath;
			pipeActive_.store(true, std::memory_order_release);

			stderrThread_ = std::thread(&FfmpegPipeline::ReadStderrLoop, this);
			return true;
		}

		void AbortPipe() noexcept
		{
			cancelRequested_.store(true, std::memory_order_release);
			std::lock_guard<std::mutex> pLock(processMutex_);
			if (process_)
			{
				TerminateProcess(process_, 1);
			}
		}

		bool WriteFrame(const void* rgbaData, size_t byteCount, std::string& error)
		{
			if (!pipeActive_.load(std::memory_order_acquire) || !stdinWrite_)
			{
				error = "FFmpeg capture pipe is not active";
				return false;
			}
			const auto* ptr = static_cast<const uint8_t*>(rgbaData);
			size_t writtenTotal = 0;
			while (writtenTotal < byteCount)
			{
				if (cancelRequested_.load(std::memory_order_acquire))
				{
					error = "Recording cancelled";
					return false;
				}
				const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(byteCount - writtenTotal, 4u * 1024u * 1024u));
				DWORD written = 0;
				if (!WriteFile(stdinWrite_, ptr + writtenTotal, chunk, &written, nullptr) || written == 0)
				{
					const DWORD winErr = GetLastError();
					error = "Failed writing RGBA frame to FFmpeg stdin (Win32=" + std::to_string(winErr) + ")";
					const std::string tail = LastStderrSummary();
					if (!tail.empty()) error += " | " + tail;
					return false;
				}
				writtenTotal += written;
			}
			return true;
		}

		bool FinishCapturePipe(bool abortEarly, std::string& error)
		{
			error.clear();
			HANDLE proc = nullptr;
			HANDLE stdinW = nullptr;
			HANDLE stderrR = nullptr;
			{
				std::lock_guard<std::mutex> pLock(processMutex_);
				stdinW = stdinWrite_;
				stdinWrite_ = nullptr;
				proc = process_;
			}
			if (stdinW)
			{
				CloseHandle(stdinW);
			}
			if (proc)
			{
				if (abortEarly)
				{
					if (WaitForSingleObject(proc, 1500) == WAIT_TIMEOUT)
					{
						TerminateProcess(proc, 1);
						WaitForSingleObject(proc, 2000);
					}
				}
				else
				{
					for (;;)
					{
						if (cancelRequested_.load(std::memory_order_acquire))
						{
							TerminateProcess(proc, 1);
							WaitForSingleObject(proc, 2000);
							break;
						}
						if (WaitForSingleObject(proc, 100) == WAIT_OBJECT_0) break;
					}
				}
			}

			DWORD exitCode = 1;
			{
				std::lock_guard<std::mutex> pLock(processMutex_);
				if (process_)
				{
					GetExitCodeProcess(process_, &exitCode);
					CloseHandle(process_);
					process_ = nullptr;
				}
			}
			if (stderrThread_.joinable()) stderrThread_.join();
			{
				std::lock_guard<std::mutex> pLock(processMutex_);
				stderrR = stderrRead_;
				stderrRead_ = nullptr;
			}
			if (stderrR)
			{
				CloseHandle(stderrR);
			}
			pipeActive_.store(false, std::memory_order_release);

			if (abortEarly || cancelRequested_.load(std::memory_order_acquire))
			{
				error = "Recording cancelled";
				return false;
			}
			if (exitCode != 0)
			{
				error = "FFmpeg Stage A exited with code " + std::to_string(exitCode);
				const std::string tail = LastStderrSummary();
				if (!tail.empty()) error += ": " + tail;
				return false;
			}
			std::error_code ec;
			if (!std::filesystem::is_regular_file(tempVideoPath_, ec) ||
				std::filesystem::file_size(tempVideoPath_, ec) == 0)
			{
				error = "FFmpeg Stage A produced an empty or missing video file";
				return false;
			}
			return true;
		}

		void RequestCancel() noexcept
		{
			cancelRequested_.store(true, std::memory_order_release);
		}

		uint64_t EncodedFrames() const noexcept
		{
			return encodedFrames_.load(std::memory_order_acquire);
		}

		// ---- Stage B: Post-Processing & Audio Muxing -----------------------

		static std::string BuildAudioFilterChain(double effectiveRate, double delaySec, float volume)
		{
			std::vector<std::string> filters;
			if (std::isfinite(effectiveRate) && std::fabs(effectiveRate - 1.0) > 1e-4)
			{
				double r = std::clamp(effectiveRate, 0.1, 5.0);
				while (r < 0.5)
				{
					filters.emplace_back("atempo=0.5");
					r /= 0.5;
				}
				while (r > 2.0)
				{
					filters.emplace_back("atempo=2.0");
					r /= 2.0;
				}
				char buf[48]{};
				std::snprintf(buf, sizeof(buf), "atempo=%.6f", r);
				filters.emplace_back(buf);
			}
			if (std::isfinite(delaySec) && delaySec > 0.0005)
			{
				const long long ms = std::llround(delaySec * 1000.0);
				if (ms > 0)
				{
					filters.emplace_back("adelay=delays=" + std::to_string(ms) + ":all=1");
				}
			}
			if (std::isfinite(volume) && std::fabs(volume - 1.0f) > 1e-4f)
			{
				const float clampedVol = std::clamp(volume, 0.0f, 2.0f);
				char buf[48]{};
				std::snprintf(buf, sizeof(buf), "volume=%.4f", clampedVol);
				filters.emplace_back(buf);
			}
			filters.emplace_back("apad");

			std::string out;
			for (size_t i = 0; i < filters.size(); ++i)
			{
				if (i) out.push_back(',');
				out += filters[i];
			}
			return out;
		}

		static bool RunPostProcess(const PostProcessOptions& opts, const FfmpegProbeResult& probe,
			const std::atomic<bool>* cancelFlag, std::string& error)
		{
			error.clear();
			std::error_code ec;

			auto cleanupTemps = [&]() {
				std::error_code rmEc;
				if (!opts.tempVideoPath.empty()) std::filesystem::remove(opts.tempVideoPath, rmEc);
				if (!opts.tempWavPath.empty()) std::filesystem::remove(opts.tempWavPath, rmEc);
			};

			if (!std::filesystem::is_regular_file(opts.tempVideoPath, ec))
			{
				error = "Temporary capture video is missing for post-processing";
				cleanupTemps();
				return false;
			}
			if (!opts.finalOutputPath.parent_path().empty())
			{
				std::filesystem::create_directories(opts.finalOutputPath.parent_path(), ec);
			}

			// Determine audio muxing source
			bool needAudioMux = false;
			bool audioIsBakedWav = false;
			std::filesystem::path audioSourcePath;
			double effectiveRate = 1.0;
			double directDelaySec = 0.0;

			if (opts.config.muxAudio && opts.audio.hasAudio && !opts.audio.audioFilePath.empty())
			{
				const std::filesystem::path srcPath(util::Utf8ToWide(opts.audio.audioFilePath));
				if (std::filesystem::is_regular_file(srcPath, ec))
				{
					needAudioMux = true;
					audioSourcePath = srcPath;

					effectiveRate = std::clamp(
						(std::isfinite(opts.audio.audioSpeed) && opts.audio.audioSpeed > 0.0 ? opts.audio.audioSpeed : 1.0) *
						(std::isfinite(opts.audio.playbackSpeed) && opts.audio.playbackSpeed > 0.0 ? opts.audio.playbackSpeed : 1.0),
						0.1, 5.0);
					directDelaySec = opts.audio.audioOffsetSeconds - opts.audio.startSeconds;

					if (opts.config.bakeAudioEffects && opts.audioService && opts.audioService->renderOfflineWav &&
						!opts.tempWavPath.empty())
					{
						GakuAudioOfflineRenderRequestV1 req{};
						req.abiVersion = GAKU_AUDIO_VERSION;
						req.structSize = sizeof(GakuAudioOfflineRenderRequestV1);
						req.inputPathUtf8 = opts.audio.audioFilePath.c_str();
						const std::string wavUtf8 = util::WideToUtf8(opts.tempWavPath.wstring());
						req.outputWavPathUtf8 = wavUtf8.c_str();
						req.effectPresetId = opts.audio.activeAudioEffectId.c_str();
						req.trimOffsetSeconds = opts.audio.audioOffsetSeconds;
						req.timelineDurationSeconds = (std::max)(0.1, opts.audio.startSeconds + opts.audio.recordedDurationSeconds);
						req.audioSpeed = static_cast<float>(opts.audio.audioSpeed);
						req.playbackSpeed = static_cast<float>(opts.audio.playbackSpeed);
						req.volume = opts.audio.musicVolume;
						req.targetSampleRate = 48000u;

						char audioErr[256]{};
						if (opts.audioService->renderOfflineWav(
								opts.audioService->context, &req, audioErr, sizeof(audioErr)) == GAKU_OK &&
							std::filesystem::is_regular_file(opts.tempWavPath, ec) &&
							std::filesystem::file_size(opts.tempWavPath, ec) > 44)
						{
							audioSourcePath = opts.tempWavPath;
							audioIsBakedWav = true;
						}
					}
				}
			}

			const std::string vf = ResolveVideoFilter(opts.config);
			const std::vector<std::string> extraArgs = SplitArgsString(opts.config.extraFfmpegArgs);
			const std::string ext = ContainerExtensionForCodec(opts.resolvedCodec);

			// Fast path: no audio mux, no video filter, no extra ffmpeg args -> atomic rename
			if (!needAudioMux && vf.empty() && extraArgs.empty())
			{
				if (!MoveFileExW(opts.tempVideoPath.c_str(), opts.finalOutputPath.c_str(),
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				{
					const DWORD winErr = GetLastError();
					error = "Failed to rename final output file (Win32=" + std::to_string(winErr) + ")";
					cleanupTemps();
					return false;
				}
				if (!opts.tempWavPath.empty()) std::filesystem::remove(opts.tempWavPath, ec);
				return true;
			}

			if (!probe.available || probe.ffmpegPath.empty())
			{
				error = "FFmpeg is required for post-processing";
				cleanupTemps();
				return false;
			}

			std::filesystem::path stageBTemp = opts.finalOutputPath;
			stageBTemp += L".stageb.tmp" + util::Utf8ToWide(ext);
			std::filesystem::remove(stageBTemp, ec);

			std::vector<std::wstring> args{
				L"-y", L"-hide_banner", L"-loglevel", L"info", L"-stats", L"-stats_period", L"0.2"
			};
			if (opts.resolvedCodec == "libvpx-vp9" && !vf.empty())
			{
				args.insert(args.end(), {L"-c:v", L"libvpx-vp9"});
			}
			args.insert(args.end(), {L"-i", opts.tempVideoPath.wstring()});

			if (needAudioMux)
			{
				if (audioIsBakedWav)
				{
					if (opts.audio.startSeconds > 0.0005)
					{
						char ssBuf[48]{};
						std::snprintf(ssBuf, sizeof(ssBuf), "%.6f", opts.audio.startSeconds);
						args.insert(args.end(), {L"-ss", util::Utf8ToWide(ssBuf)});
					}
				}
				else if (directDelaySec < -0.0005)
				{
					const double seekSrcSec = (-directDelaySec) * effectiveRate;
					char ssBuf[48]{};
					std::snprintf(ssBuf, sizeof(ssBuf), "%.6f", seekSrcSec);
					args.insert(args.end(), {L"-ss", util::Utf8ToWide(ssBuf)});
				}
				args.insert(args.end(), {L"-i", audioSourcePath.wstring()});
			}

			args.insert(args.end(), {L"-map", L"0:v:0"});
			if (vf.empty())
			{
				args.insert(args.end(), {L"-c:v", L"copy"});
			}
			else
			{
				args.insert(args.end(), {L"-vf", util::Utf8ToWide(vf)});
				AppendVideoEncoderArgs(args, opts.resolvedCodec, opts.config.pixelFormat, opts.config.crf);
			}

			if (needAudioMux)
			{
				args.insert(args.end(), {L"-map", L"1:a:0"});
				const std::string af = audioIsBakedWav
					? std::string("apad")
					: BuildAudioFilterChain(effectiveRate, directDelaySec, opts.audio.musicVolume);
				args.insert(args.end(), {L"-af", util::Utf8ToWide(af)});

				if (ext == ".mov")
				{
					args.insert(args.end(), {L"-c:a", L"pcm_s16le"});
				}
				else if (ext == ".webm")
				{
					args.insert(args.end(), {L"-c:a", L"libopus", L"-b:a", L"192k"});
				}
				else
				{
					args.insert(args.end(), {L"-c:a", L"aac", L"-b:a", L"320k"});
				}
				args.push_back(L"-shortest");
			}

			for (const auto& extra : extraArgs)
			{
				args.push_back(util::Utf8ToWide(extra));
			}

			if (ext == ".mp4")
			{
				args.insert(args.end(), {L"-movflags", L"+faststart"});
			}
			args.push_back(stageBTemp.wstring());

			DWORD exitCode = 1;
			std::string outText, runErr;
			const bool ran = RunProcessCapture(
				probe.ffmpegPath, args, INFINITE, exitCode, outText, runErr, cancelFlag);
			if (!ran || exitCode != 0)
			{
				error = !runErr.empty()
					? runErr
					: ("FFmpeg Stage B exited with code " + std::to_string(exitCode));
				if (!outText.empty())
				{
					// Include tail of FFmpeg stderr
					const size_t tailPos = outText.size() > 512 ? outText.size() - 512 : 0;
					error += " | " + outText.substr(tailPos);
				}
				std::filesystem::remove(stageBTemp, ec);
				cleanupTemps();
				return false;
			}

			if (!std::filesystem::is_regular_file(stageBTemp, ec) || std::filesystem::file_size(stageBTemp, ec) == 0)
			{
				error = "FFmpeg Stage B produced an empty output file";
				std::filesystem::remove(stageBTemp, ec);
				cleanupTemps();
				return false;
			}

			if (!MoveFileExW(stageBTemp.c_str(), opts.finalOutputPath.c_str(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				const DWORD winErr = GetLastError();
				error = "Failed to move post-processed file to destination (Win32=" + std::to_string(winErr) + ")";
				std::filesystem::remove(stageBTemp, ec);
				cleanupTemps();
				return false;
			}

			cleanupTemps();
			return true;
		}

	private:
		void CancelAndJoin() noexcept
		{
			cancelRequested_.store(true, std::memory_order_release);
			if (pipeActive_.load(std::memory_order_acquire) || process_ || stdinWrite_ || stderrThread_.joinable())
			{
				std::string ignored;
				FinishCapturePipe(true, ignored);
			}
		}

		void ReadStderrLoop()
		{
			if (!stderrRead_) return;
			std::string lineBuf;
			char buf[2048];
			DWORD bytesRead = 0;
			while (ReadFile(stderrRead_, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0)
			{
				for (DWORD i = 0; i < bytesRead; ++i)
				{
					const char ch = buf[i];
					if (ch == '\r' || ch == '\n')
					{
						if (!lineBuf.empty())
						{
							HandleStderrLine(lineBuf);
							lineBuf.clear();
						}
					}
					else if (lineBuf.size() < 2048)
					{
						lineBuf.push_back(ch);
					}
				}
			}
			if (!lineBuf.empty()) HandleStderrLine(lineBuf);
		}

		void HandleStderrLine(const std::string& line)
		{
			const size_t pos = line.find("frame=");
			if (pos != std::string::npos)
			{
				size_t idx = pos + 6;
				while (idx < line.size() && (line[idx] == ' ' || line[idx] == '\t')) ++idx;
				if (idx < line.size() && line[idx] >= '0' && line[idx] <= '9')
				{
					char* end = nullptr;
					const unsigned long long parsed = std::strtoull(line.c_str() + idx, &end, 10);
					if (end != line.c_str() + idx)
					{
						const uint64_t prev = encodedFrames_.load(std::memory_order_relaxed);
						if (parsed > prev)
						{
							encodedFrames_.store(parsed, std::memory_order_release);
						}
					}
				}
			}

			std::lock_guard<std::mutex> lock(stderrMutex_);
			stderrTail_.push_back(line);
			if (stderrTail_.size() > 24) stderrTail_.pop_front();
		}

		std::string LastStderrSummary() const
		{
			std::lock_guard<std::mutex> lock(stderrMutex_);
			std::string out;
			for (size_t i = stderrTail_.size() > 3 ? stderrTail_.size() - 3 : 0; i < stderrTail_.size(); ++i)
			{
				if (!out.empty()) out += " / ";
				out += stderrTail_[i];
			}
			return out;
		}

		mutable std::mutex processMutex_;
		HANDLE process_{nullptr};
		HANDLE stdinWrite_{nullptr};
		HANDLE stderrRead_{nullptr};
		std::thread stderrThread_;
		std::filesystem::path tempVideoPath_;
		std::atomic<bool> pipeActive_{false};
		std::atomic<bool> cancelRequested_{false};
		std::atomic<uint64_t> encodedFrames_{0};
		mutable std::mutex stderrMutex_;
		std::deque<std::string> stderrTail_;
	};
} // namespace VideoExporter
