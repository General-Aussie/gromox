// SPDX-License-Identifier: AGPL-3.0-or-later, OR GPL-2.0-or-later WITH linking exception
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#define _GNU_SOURCE 1 /* unistd.h:environ */
#include <list>
#include <memory>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <libHX/ctype_helper.h>
#include <libHX/io.h>
#include <libHX/string.h>
#include <gromox/fileio.h>
#include <gromox/paths.h>
#include <gromox/scope.hpp>
#include <gromox/tie.hpp>
#include <gromox/util.hpp>

class hxmc_deleter {
	public:
	void operator()(hxmc_t *s) { HXmc_free(s); }
};

using namespace gromox;

char **read_file_by_line(const char *file)
{
	std::unique_ptr<FILE, file_deleter> fp(fopen(file, "r"));
	if (fp == nullptr)
		return nullptr;

	hxmc_t *line = nullptr;
	try {
		std::list<std::unique_ptr<char[]>> dq;
		while (HX_getl(&line, fp.get()) != nullptr) {
			HX_chomp(line);
			decltype(dq)::value_type s(strdup(line));
			if (s == nullptr)
				return nullptr;
			dq.push_back(std::move(s));
		}
		HXmc_free(line);
		line = nullptr;
		auto ret = std::make_unique<char *[]>(dq.size() + 1);
		size_t i = 0;
		for (auto &e : dq)
			ret[i++] = e.release();
		return ret.release();
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
		return nullptr;
	} catch (...) {
		HXmc_free(line);
		throw;
	}
}

int gx_vsnprintf1(char *buf, size_t sz, const char *file, unsigned int line,
    const char *fmt, va_list args)
{
	auto ret = vsnprintf(buf, sz, fmt, args);
	if (ret < 0) {
		*buf = '\0';
		return ret;
	} else if (static_cast<size_t>(ret) >= sz) {
		fprintf(stderr, "gx_vsnprintf: truncation at %s:%u (%d bytes into buffer of %zu)\n",
		        file, line, ret, sz);
		return strlen(buf);
	}
	return ret;
}

int gx_snprintf1(char *buf, size_t sz, const char *file, unsigned int line,
    const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	auto ret = gx_vsnprintf1(buf, sz, file, line, fmt, args);
	va_end(args);
	if (ret < 0) {
		*buf = '\0';
		return ret;
	} else if (static_cast<size_t>(ret) >= sz) {
		fprintf(stderr, "gx_snprintf: truncation at %s:%u (%d bytes into buffer of %zu)\n",
		        file, line, ret, sz);
		return strlen(buf);
	}
	return ret;
}

namespace {

struct popen_fdset {
	int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, null = -1;

	~popen_fdset()
	{
		if (in[0] != -1) close(in[0]);
		if (in[1] != -1) close(in[1]);
		if (out[0] != -1) close(out[0]);
		if (out[1] != -1) close(out[1]);
		if (err[0] != -1) close(err[0]);
		if (err[1] != -1) close(err[1]);
		if (null != -1) close(null);
	}
};

}

namespace gromox {

pid_t popenfd(const char *const *argv, int *fdinp, int *fdoutp,
    int *fderrp, const char *const *env)
{
	if (argv == nullptr || argv[0] == nullptr)
		return -EINVAL;

	popen_fdset fd;
	if (fdinp == nullptr || fdoutp == nullptr || fderrp == nullptr) {
		fd.null = open("/dev/null", O_RDWR);
		if (fd.null < 0)
			return -errno;
	}
	posix_spawn_file_actions_t fa{};
	auto ret = posix_spawn_file_actions_init(&fa);
	if (ret != 0)
		return -ret;
	auto cl2 = make_scope_exit([&]() { posix_spawn_file_actions_destroy(&fa); });

	/* Close child-unused ends of the pipes; move child-used ends to fd 0-2. */
	if (fdinp != nullptr) {
		if (pipe(fd.in) < 0)
			return -errno;
		ret = posix_spawn_file_actions_addclose(&fa, fd.in[1]);
		if (ret != 0)
			return -ret;
		ret = posix_spawn_file_actions_adddup2(&fa, fd.in[0], STDIN_FILENO);
		if (ret != 0)
			return -ret;
	} else {
		ret = posix_spawn_file_actions_adddup2(&fa, fd.null, STDIN_FILENO);
		if (ret != 0)
			return -ret;
	}

	if (fdoutp != nullptr) {
		if (pipe(fd.out) < 0)
			return -errno;
		ret = posix_spawn_file_actions_addclose(&fa, fd.out[0]);
		if (ret != 0)
			return -ret;
		ret = posix_spawn_file_actions_adddup2(&fa, fd.out[1], STDOUT_FILENO);
		if (ret != 0)
			return -ret;
	} else {
		ret = posix_spawn_file_actions_adddup2(&fa, fd.null, STDOUT_FILENO);
		if (ret != 0)
			return -ret;
	}

	if (fderrp == nullptr) {
		ret = posix_spawn_file_actions_adddup2(&fa, fd.null, STDERR_FILENO);
		if (ret != 0)
			return -ret;
	} else if (fderrp == fdoutp) {
		ret = posix_spawn_file_actions_adddup2(&fa, fd.out[1], STDERR_FILENO);
		if (ret != 0)
			return -ret;
	} else {
		if (fderrp != nullptr && fderrp != fdoutp && pipe(fd.err) < 0)
			return -errno;
		ret = posix_spawn_file_actions_addclose(&fa, fd.err[0]);
		if (ret != 0)
			return -ret;
		ret = posix_spawn_file_actions_adddup2(&fa, fd.err[1], STDERR_FILENO);
		if (ret != 0)
			return -ret;
	}

	/* Close all pipe ends that were not already fd 0-2. */
	if (fd.in[0] != -1 && fd.in[0] != STDIN_FILENO &&
	    (ret = posix_spawn_file_actions_addclose(&fa, fd.in[0])) != 0)
		return -ret;
	if (fderrp != fdoutp) {
		if (fd.out[1] != -1 && fd.out[1] != STDOUT_FILENO &&
		    (ret = posix_spawn_file_actions_addclose(&fa, fd.out[1])) != 0)
			return -ret;
		if (fd.err[1] != -1 && fd.err[1] != STDERR_FILENO &&
		    (ret = posix_spawn_file_actions_addclose(&fa, fd.err[1])) != 0)
			return -ret;
	} else {
		if (fd.out[1] != -1 && fd.out[1] != STDOUT_FILENO &&
		    fd.out[1] != STDERR_FILENO &&
		    (ret = posix_spawn_file_actions_addclose(&fa, fd.out[1])) != 0)
			return -ret;
	}
	if (fd.null != -1 && fd.null != STDIN_FILENO &&
	    fd.null != STDOUT_FILENO && fd.null != STDERR_FILENO &&
	    (ret = posix_spawn_file_actions_addclose(&fa, fd.null)) != 0)
		return -ret;

	pid_t pid = -1;
	ret = posix_spawnp(&pid, argv[0], &fa, nullptr, const_cast<char **>(argv), const_cast<char **>(env));
	if (ret != 0)
		return -ret;
	if (fdinp != nullptr) {
		*fdinp = fd.in[1];
		fd.in[1] = -1;
	}
	if (fdoutp != nullptr) {
		*fdoutp = fd.out[0];
		fd.out[0] = -1;
	}
	if (fderrp != nullptr && fderrp != fdoutp) {
		*fderrp = fd.err[0];
		fd.err[0] = -1;
	}
	return pid;
}

ssize_t feed_w3m(const void *inbuf, size_t len, std::string &outbuf) try
{
	std::string filename;
	auto tmpdir = getenv("TMPDIR");
	filename = tmpdir == nullptr ? "/tmp" : tmpdir;
	auto pos = filename.length();
	filename += "/XXXXXXXXXXXX.html";
	randstring_k(&filename[pos+1], 12, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
	filename[pos+13] = '.';

	struct xclose { void operator()(FILE *f) { fclose(f); } };
	std::unique_ptr<FILE, xclose> fp(fopen(filename.c_str(), "w"));
	if (fp == nullptr || fwrite(inbuf, len, 1, fp.get()) != 1)
		return -1;
	auto cl1 = make_scope_exit([&]() { unlink(filename.c_str()); });
	fp.reset();
	int fout = -1;
	auto cl2 = make_scope_exit([&]() { if (fout != -1) close(fout); });
	const char *const argv[] = {"w3m", "-dump", filename.c_str(), nullptr};
	auto pid = popenfd(argv, nullptr, &fout, nullptr, const_cast<const char *const *>(environ));
	if (pid < 0)
		return -1;
	int status = 0;
	auto cl3 = make_scope_exit([&]() { waitpid(pid, &status, 0); });
	outbuf = std::string();
	size_t ret;
	char fbuf[4096];
	while ((ret = read(fout, fbuf, GX_ARRAY_SIZE(fbuf))) > 0)
		outbuf.append(fbuf, ret);
	return WIFEXITED(status) ? outbuf.size() : -1;
} catch (...) {
	return -1;
}

/*
 * Trim "<foo>" from string, and make two C strings from it,
 * each with a trailing \0, and each being preprended with
 * Pascal-style length byte which incidentally also counts the \0.
 * \r\n is appended.
 * "hi <who>, give" -> 4 h i space \0 9 , space g i v e \r \n \0
 */
std::string resource_parse_stcode_line(const char *src)
{
	std::string out;
	uint8_t srclen = strlen(src);
	out.reserve(srclen + 6);
	auto ptr = strchr(src, '<');
	if (ptr == nullptr || ptr == src) {
		uint8_t sub = srclen + 3;
		out.append(reinterpret_cast<char *>(&sub), 1);
		out.append(src, srclen);
		out.append("\r\n", 3);
		return out;
	}
	uint8_t seg = ptr - src + 1;
	out.append(reinterpret_cast<char *>(&seg), 1);
	out.append(src, seg - 1);
	out += '\0';
	ptr = strchr(src, '>');
	if (ptr == nullptr)
		return "\006OMG\r\n";
	++ptr;
	seg = strlen(ptr) + 3;
	out.append(reinterpret_cast<char *>(&seg), 1);
	out.append(ptr);
	out.append("\r\n", 2);
	return out;
}

static constexpr struct {
	const char *suffix = nullptr;
	unsigned int len = 0, mult = 0;
} time_suffix[] = {
	{"seconds", 7, 1},
	{"second", 6, 1},
	{"sec", 3, 1},
	{"s", 1, 1},
	{"minutes", 7, 60},
	{"minute", 6, 60},
	{"min", 3, 60},
	{"m", 1, 60},
	{"hours", 5, 3600},
	{"hour", 4, 3600},
	{"h", 1, 3600},
	{"days", 4, 86400},
	{"day", 3, 86400},
	{"d", 1, 86400},
};

long atoitvl(const char *s)
{
	long result = 0;
	do {
		while (HX_isspace(*s))
			++s;
		if (*s == '\0')
			break;
		unsigned int mult = 0;
		char *end;
		auto v = strtoul(s, &end, 10);
		if (s == end)
			return -1;
		s = end;
		while (HX_isspace(*s))
			++s;
		for (const auto &e : time_suffix) {
			if (strncmp(s, e.suffix, e.len) == 0) {
				mult = e.mult;
				s += e.len;
				break;
			}
		}
		if (mult == 0)
			return -1;
		result += v * mult;
	} while (true);
	return result;
}

bool parse_bool(const char *s)
{
	if (s == nullptr)
		return false;
	char *end = nullptr;
	if (strtoul(s, &end, 0) == 0 && *end == '\0')
		return false;
	if (strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0 ||
	    strcasecmp(s, "false") == 0)
		return false;
	return true;
}

std::string bin2hex(const void *vin, size_t len)
{
	std::string buffer;
	if (vin == nullptr)
		return buffer;
	static constexpr char digits[] = "0123456789abcdef";
	auto input = static_cast<const char *>(vin);
	buffer.resize(len * 2);
	for (size_t j = 0; len-- > 0; j += 2) {
		buffer[j]   = digits[(*input >> 4) & 0x0F];
		buffer[j+1] = digits[*input & 0x0F];
		++input;
	}
	return buffer;
}

std::string hex2bin(const char *input)
{
	auto max = strlen(input) / 2; /* ignore last nibble if needed */
	std::string buf;
	buf.resize(max);
	for (size_t n = 0; n < max; ++n) {
		unsigned char c = HX_tolower(input[2*n]);
		unsigned char d = HX_tolower(input[2*n+1]);
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'A' && c <= 'F')
			c -= 'A' + 10;
		else
			c = 0;
		if (d >= '0' && d <= '9')
			d -= '0';
		else if (d >= 'A' && d <= 'F')
			d -= 'A' + 10;
		else
			d = 0;
		buf[n] = (c << 4) | d;
	}
	return buf;
}

void rfc1123_dstring(char *buf, size_t z, time_t ts)
{
	if (ts == 0)
		ts = time(nullptr);
	struct tm tm;
	gmtime_r(&ts, &tm);
	strftime(buf, z, "%a, %d %b %Y %T GMT", &tm);
}

void startup_banner(const char *prog)
{
	fprintf(stderr, "\n%s %s (pid %ld uid %ld)\n\n", prog, PACKAGE_VERSION,
	        static_cast<long>(getpid()), static_cast<long>(getuid()));
}

/**
 * Upon setuid, tasks are restricted in their dumping (cf. linux/kernel/cred.c
 * in commit_creds, calling set_dumpable). To restore the dump flag, one could
 * use prctl, but re-executing the process has the benefit that the application
 * completely re-runs as unprivileged user from the start and can catch e.g.
 * file access errors that would occur before gx_reexec, and we can be sure
 * that privileged informationed does not escape into a dump.
 */
int gx_reexec(const char *const *argv)
{
	auto s = getenv("GX_REEXEC_DONE");
	if (s != nullptr || argv == nullptr) {
		chdir("/");
		unsetenv("GX_REEXEC_DONE");
		return 0;
	}
	setenv("GX_REEXEC_DONE", "1", true);

	hxmc_t *resolved = nullptr;
	auto ret = HX_readlink(&resolved, "/proc/self/exe");
	if (ret < 0) {
		fprintf(stderr, "reexec: readlink: %s", strerror(-ret));
		return ret;
	}
	fprintf(stderr, "Reexecing %s\n", resolved);
	execv(resolved, const_cast<char **>(argv));
	int saved_errno = errno;
	perror("execv");
	HXmc_free(resolved);
	return -saved_errno;
}

}
