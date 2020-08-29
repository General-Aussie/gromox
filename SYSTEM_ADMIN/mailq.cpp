/* SPDX-License: AGPL-3.0-or-later */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <gromox/paths.h>

namespace {

struct io_deleter {
	void operator()(DIR *d) { closedir(d); }
	void operator()(FILE *f) { fclose(f); }
};

}

int main()
{
	bool tty = isatty(STDOUT_FILENO);
	auto c_dark = tty ? "\e[0;1;30m" : "";
	auto c_ptr = tty ? "\e[0;36m" : "";
	auto c_reset = tty ? "\e[0m" : "";

	std::string msg_dir = PKGSTATEQUEUEDIR "/mess";
	std::unique_ptr<DIR, io_deleter> dh(opendir(msg_dir.c_str()));
	if (dh == nullptr) {
		fprintf(stderr, "Could not open %s: %s\n", msg_dir.c_str(), strerror(errno));
		return EXIT_FAILURE;
	}
	const struct dirent *de;
	printf("#%-5s  %-19s  %9s  %9s  Sender Recipient\n", "Qid", "date", "msg_size", "Fid");
	while ((de = readdir(dh.get())) != nullptr) {
		std::string filename = msg_dir + "/" + de->d_name;
		struct stat sb;
		char timebuf[64];

		if (stat(filename.c_str(), &sb) < 0 || !S_ISREG(sb.st_mode))
			continue;
		strftime(timebuf, sizeof(timebuf), "%FT%T", localtime(&sb.st_mtime));
		printf("%-6s  %-19s", de->d_name, timebuf);
		int fd = open(filename.c_str(), O_RDONLY);
		if (fd < 0) {
			printf("\n");
			fprintf(stderr, "%s: %s\n", filename.c_str(), strerror(errno));
			continue;
		}
		size_t clump_size = sizeof(size_t) + 3 * sizeof(int) + 2;
		if (static_cast<size_t>(sb.st_size) < clump_size) {
			printf("\n");
			continue;
		}
		auto fcontent = reinterpret_cast<char *>(mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
		close(fd);
		if (fcontent == reinterpret_cast<void *>(MAP_FAILED)) {
			printf("\n");
			continue;
		}
		size_t mail_len = 0;
		unsigned int flush_id = 0, bound_type = 0, is_spam = 0;
		memcpy(&mail_len, fcontent, sizeof(mail_len));
		if (static_cast<size_t>(sb.st_size) < clump_size + mail_len) {
			printf("\n");
			munmap(fcontent, sb.st_size);
			continue;
		}
		const char *ptr = fcontent + sizeof(size_t) + mail_len;
		memcpy(&flush_id, ptr, sizeof(flush_id));
		ptr += sizeof(flush_id);
		memcpy(&bound_type, ptr, sizeof(bound_type));
		ptr += sizeof(bound_type);
		memcpy(&is_spam, ptr, sizeof(is_spam));
		ptr += sizeof(is_spam);
		const char *from = ptr;
		ptr += strlen(from) + 1;
		const char *rcpt = ptr;
		printf("  %9zu  %9u  %s<%s%s%s> %s► %s<%s%s%s>%s\n",
			mail_len, flush_id,
			c_dark, c_reset, from, c_dark, c_ptr, c_dark, c_reset, rcpt, c_dark, c_reset);
		munmap(fcontent, sb.st_size);
	}
}
