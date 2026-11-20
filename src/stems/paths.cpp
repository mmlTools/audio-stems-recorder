#include "paths.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace stems {

std::FILE *fopen_u8(const std::string &path, const char *mode)
{
#if defined(_WIN32)
	auto widen = [](const std::string &s) {
		std::wstring w;
		if (s.empty())
			return w;
		int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		if (n <= 0)
			return w;
		w.resize((size_t)n);
		MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
		return w;
	};
	std::wstring wpath = widen(path);
	std::wstring wmode = widen(mode);
	if (wpath.empty())
		return nullptr;
	std::FILE *f = nullptr;
	if (_wfopen_s(&f, wpath.c_str(), wmode.c_str()) != 0)
		return nullptr;
	return f;
#else
	return std::fopen(path.c_str(), mode);
#endif
}

int fseek64(std::FILE *f, int64_t off, int whence)
{
#if defined(_WIN32)
	return _fseeki64(f, off, whence);
#else
	return fseeko(f, (off_t)off, whence);
#endif
}

int64_t ftell64(std::FILE *f)
{
#if defined(_WIN32)
	return _ftelli64(f);
#else
	return (int64_t)ftello(f);
#endif
}

} // namespace stems
