#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace stems {

// All strings inside the plugin are UTF-8. On Windows std::filesystem::path and
// fopen() interpret narrow strings in the ANSI code page, which silently breaks
// user names / folders / source names containing non-ASCII characters. These
// helpers keep every conversion explicit.

inline std::filesystem::path u8path(const std::string &s)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
	return std::filesystem::u8path(s);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

inline std::string to_u8(const std::filesystem::path &p)
{
	auto s = p.u8string();
	return std::string(s.begin(), s.end());
}

std::FILE *fopen_u8(const std::string &path, const char *mode);

int fseek64(std::FILE *f, int64_t off, int whence);
int64_t ftell64(std::FILE *f);

} // namespace stems
