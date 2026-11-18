// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace VideoExporter::util
{
	inline std::wstring Utf8ToWide(std::string_view input)
	{
		if (input.empty()) return {};
		const int count = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
		if (count <= 0)
		{
			const int fallback = MultiByteToWideChar(
				CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
			if (fallback <= 0) return {};
			std::wstring wide(static_cast<size_t>(fallback), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), wide.data(), fallback);
			return wide;
		}
		std::wstring wide(static_cast<size_t>(count), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), wide.data(), count);
		return wide;
	}

	inline std::string WideToUtf8(std::wstring_view input)
	{
		if (input.empty()) return {};
		const int count = WideCharToMultiByte(
			CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
		if (count <= 0) return {};
		std::string utf8(static_cast<size_t>(count), '\0');
		WideCharToMultiByte(
			CP_UTF8, 0, input.data(), static_cast<int>(input.size()), utf8.data(), count, nullptr, nullptr);
		return utf8;
	}

	inline void AppendEscapedJson(std::string& out, std::string_view text)
	{
		out.push_back('"');
		for (const unsigned char ch : text)
		{
			switch (ch)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20)
				{
					char buf[8]{};
					std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
					out += buf;
				}
				else
				{
					out.push_back(static_cast<char>(ch));
				}
			}
		}
		out.push_back('"');
	}

	inline std::string JString(std::string_view text)
	{
		std::string out;
		out.reserve(text.size() + 4);
		AppendEscapedJson(out, text);
		return out;
	}

	inline std::string JNum(double value)
	{
		if (!std::isfinite(value)) return "0";
		char buf[64]{};
		if (value == std::floor(value) && std::fabs(value) < 9.0e15)
		{
			std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
		}
		else
		{
			std::snprintf(buf, sizeof(buf), "%.17g", value);
		}
		return buf;
	}

	inline std::string JUInt(uint64_t value)
	{
		char buf[32]{};
		std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
		return buf;
	}

	inline std::string JBool(bool value)
	{
		return value ? "true" : "false";
	}

	inline bool WriteFileAtomicUtf8(const std::filesystem::path& path, const std::string& content, std::string& error)
	{
		if (path.empty())
		{
			error = "Target path is empty";
			return false;
		}
		std::error_code ec;
		if (!path.parent_path().empty())
		{
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec)
			{
				error = "Failed to create directory: " + ec.message();
				return false;
			}
		}

		std::filesystem::path tmpPath = path;
		tmpPath += L".tmp";
		DeleteFileW(tmpPath.c_str());

		HANDLE handle = CreateFileW(
			tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			error = "Failed to open temporary file (Win32=" + std::to_string(GetLastError()) + ")";
			return false;
		}

		size_t offset = 0;
		bool writeOk = true;
		while (offset < content.size())
		{
			const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(content.size() - offset, 1u << 20));
			DWORD written = 0;
			if (!WriteFile(handle, content.data() + offset, chunk, &written, nullptr) || written != chunk)
			{
				writeOk = false;
				break;
			}
			offset += written;
		}
		if (writeOk && !FlushFileBuffers(handle))
		{
			writeOk = false;
		}
		const DWORD lastErr = GetLastError();
		CloseHandle(handle);

		if (!writeOk)
		{
			DeleteFileW(tmpPath.c_str());
			error = "Failed to write temporary file (Win32=" + std::to_string(lastErr) + ")";
			return false;
		}

		if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD moveErr = GetLastError();
			DeleteFileW(tmpPath.c_str());
			error = "Failed to replace target file atomically (Win32=" + std::to_string(moveErr) + ")";
			return false;
		}
		return true;
	}

	inline std::string ReadFileUtf8(const std::filesystem::path& path, bool& ok)
	{
		ok = false;
		HANDLE handle = CreateFileW(
			path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return {};

		LARGE_INTEGER size{};
		if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024)
		{
			CloseHandle(handle);
			return {};
		}

		std::string out(static_cast<size_t>(size.QuadPart), '\0');
		size_t offset = 0;
		while (offset < out.size())
		{
			const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(out.size() - offset, 1u << 20));
			DWORD read = 0;
			if (!ReadFile(handle, out.data() + offset, chunk, &read, nullptr) || read == 0)
			{
				CloseHandle(handle);
				return {};
			}
			offset += read;
		}
		CloseHandle(handle);
		ok = true;
		return out;
	}

	inline std::string SanitizeFilenameUtf8(std::string_view raw, std::string_view fallback = "GakuVideo")
	{
		std::string out;
		out.reserve(raw.size());
		for (const unsigned char ch : raw)
		{
			if (ch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
				ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
			{
				out.push_back('_');
			}
			else
			{
				out.push_back(static_cast<char>(ch));
			}
		}
		while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
		while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
		if (out.empty()) return std::string(fallback);
		// Truncate safely on UTF-8 boundary to avoid overly long paths.
		if (out.size() > 72)
		{
			size_t len = 72;
			while (len > 0 && (static_cast<unsigned char>(out[len]) & 0xC0) == 0x80) --len;
			if (len > 0) out.resize(len);
		}
		return out;
	}

	inline std::string LocalTimestampCompact()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char buf[32]{};
		std::snprintf(buf, sizeof(buf), "%04u%02u%02u_%02u%02u%02u",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return buf;
	}
} // namespace VideoExporter::util
