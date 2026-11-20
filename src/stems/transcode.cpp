#include "transcode.hpp"

#include "paths.hpp"

#include <obs-module.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace stems {
namespace fs = std::filesystem;

OutputFormat output_format_from_string(const std::string &s)
{
	if (s == "mp3")
		return OutputFormat::Mp3;
	if (s == "flac")
		return OutputFormat::Flac;
	return OutputFormat::Wav;
}

const char *output_format_to_string(OutputFormat f)
{
	switch (f) {
	case OutputFormat::Mp3:
		return "mp3";
	case OutputFormat::Flac:
		return "flac";
	default:
		return "wav";
	}
}

const char *output_format_extension(OutputFormat f)
{
	switch (f) {
	case OutputFormat::Mp3:
		return ".mp3";
	case OutputFormat::Flac:
		return ".flac";
	default:
		return ".wav";
	}
}

namespace {

#if defined(_WIN32)
std::wstring widen(const std::string &s)
{
	std::wstring w;
	if (s.empty())
		return w;
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	if (n <= 0)
		return w;
	w.resize((size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string narrow(const std::wstring &w)
{
	std::string s;
	if (w.empty())
		return s;
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return s;
	s.resize((size_t)n);
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::string get_env(const char *name)
{
	const std::wstring wname = widen(name);
	DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
	if (n == 0)
		return {};
	std::wstring buf(n, L'\0');
	n = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
	buf.resize(n);
	return narrow(buf);
}

// Quotes one argument following the CommandLineToArgvW rules.
void append_quoted(std::wstring &cmd, const std::wstring &arg)
{
	if (!cmd.empty())
		cmd.push_back(L' ');
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		cmd += arg;
		return;
	}
	cmd.push_back(L'"');
	for (size_t i = 0;; i++) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == L'\\') {
			i++;
			backslashes++;
		}
		if (i == arg.size()) {
			cmd.append(backslashes * 2, L'\\');
			break;
		}
		if (arg[i] == L'"') {
			cmd.append(backslashes * 2 + 1, L'\\');
			cmd.push_back(L'"');
		} else {
			cmd.append(backslashes, L'\\');
			cmd.push_back(arg[i]);
		}
	}
	cmd.push_back(L'"');
}

int run_process(const std::vector<std::string> &args, const std::atomic<bool> &cancel)
{
	std::wstring cmd;
	for (const auto &a : args)
		append_quoted(cmd, widen(a));
	const std::wstring exe = widen(args[0]);

	// Kill ffmpeg automatically if OBS itself goes away.
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
	}

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	const DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED;
	if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
		blog(LOG_ERROR, "[Audio Stems] failed to launch ffmpeg (error %lu)", GetLastError());
		if (job)
			CloseHandle(job);
		return -1;
	}
	if (job)
		AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	int rc = -1;
	for (;;) {
		const DWORD w = WaitForSingleObject(pi.hProcess, 100);
		if (w == WAIT_OBJECT_0) {
			DWORD code = 1;
			GetExitCodeProcess(pi.hProcess, &code);
			rc = (int)code;
			break;
		}
		if (w != WAIT_TIMEOUT || cancel.load(std::memory_order_relaxed)) {
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 2000);
			rc = -2;
			break;
		}
	}
	CloseHandle(pi.hProcess);
	if (job)
		CloseHandle(job);
	return rc;
}
#else
std::string get_env(const char *name)
{
	const char *v = std::getenv(name);
	return v ? v : "";
}

int run_process(const std::vector<std::string> &args, const std::atomic<bool> &cancel)
{
	std::vector<char *> argv;
	for (const auto &a : args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

	pid_t pid = 0;
	const int err = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	if (err != 0) {
		blog(LOG_ERROR, "[Audio Stems] failed to launch ffmpeg (errno %d)", err);
		return -1;
	}
	setpriority(PRIO_PROCESS, (id_t)pid, 10);

	for (;;) {
		int status = 0;
		const pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		if (r < 0 && errno != EINTR)
			return -1;
		if (cancel.load(std::memory_order_relaxed)) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return -2;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
#endif

bool is_file(const fs::path &p)
{
	std::error_code ec;
	return fs::is_regular_file(p, ec);
}

} // namespace

std::string find_ffmpeg(const std::string &configured)
{
#if defined(_WIN32)
	const char *exe_name = "ffmpeg.exe";
	const char sep = ';';
#else
	const char *exe_name = "ffmpeg";
	const char sep = ':';
#endif

	if (!configured.empty()) {
		fs::path p = u8path(configured);
		std::error_code ec;
		if (fs::is_directory(p, ec))
			p /= exe_name;
		if (is_file(p))
			return to_u8(p);
	}

	std::vector<std::string> dirs;
	const std::string path_env = get_env("PATH");
	size_t start = 0;
	while (start <= path_env.size()) {
		size_t end = path_env.find(sep, start);
		if (end == std::string::npos)
			end = path_env.size();
		std::string d = path_env.substr(start, end - start);
		if (d.size() >= 2 && d.front() == '"' && d.back() == '"')
			d = d.substr(1, d.size() - 2);
		if (!d.empty())
			dirs.push_back(d);
		start = end + 1;
	}

#if defined(_WIN32)
	const std::string local = get_env("LOCALAPPDATA");
	if (!local.empty())
		dirs.push_back(local + "\\Microsoft\\WinGet\\Links");
	const std::string pd = get_env("ProgramData");
	if (!pd.empty())
		dirs.push_back(pd + "\\chocolatey\\bin");
	const std::string home = get_env("USERPROFILE");
	if (!home.empty())
		dirs.push_back(home + "\\scoop\\shims");
	dirs.push_back("C:\\ffmpeg\\bin");
#else
	// GUI apps on macOS do not inherit the shell PATH.
	dirs.push_back("/opt/homebrew/bin");
	dirs.push_back("/usr/local/bin");
	dirs.push_back("/usr/bin");
#endif

	for (const auto &d : dirs) {
		const fs::path p = u8path(d) / exe_name;
		if (is_file(p))
			return to_u8(p);
	}
	return {};
}

bool export_audio(const std::string &ffmpeg, const std::string &input_wav, const std::string &output_path,
		  OutputFormat format, int mp3_bitrate_kbps, const std::atomic<bool> &cancel)
{
	if (ffmpeg.empty() || format == OutputFormat::Wav)
		return false;

	mp3_bitrate_kbps = mp3_bitrate_kbps < 64 ? 64 : (mp3_bitrate_kbps > 320 ? 320 : mp3_bitrate_kbps);

	fs::path tmp = u8path(output_path);
	tmp += ".part";
	const std::string tmp_s = to_u8(tmp);

	std::vector<std::string> args = {ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
					 "-i",   input_wav, "-vn",       "-map_metadata", "-1"};
	if (format == OutputFormat::Mp3) {
		args.insert(args.end(), {"-c:a", "libmp3lame", "-b:a", std::to_string(mp3_bitrate_kbps) + "k", "-f", "mp3"});
	} else {
		args.insert(args.end(), {"-c:a", "flac", "-compression_level", "5", "-f", "flac"});
	}
	args.push_back(tmp_s);

	const int rc = run_process(args, cancel);
	std::error_code ec;
	if (rc != 0 || !is_file(tmp)) {
		if (rc != -2)
			blog(LOG_ERROR, "[Audio Stems] ffmpeg export failed (rc=%d) for %s", rc, input_wav.c_str());
		fs::remove(tmp, ec);
		return false;
	}
	fs::rename(tmp, u8path(output_path), ec);
	if (ec) {
		blog(LOG_ERROR, "[Audio Stems] failed to finalize export %s: %s", output_path.c_str(), ec.message().c_str());
		fs::remove(tmp, ec);
		return false;
	}
	return true;
}

} // namespace stems
