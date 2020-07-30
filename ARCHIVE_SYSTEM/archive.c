#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <errno.h>
#include <string.h>
#include <libHX/option.h>
#include <gromox/defs.h>
#include <gromox/socket.h>
#include "mail.h"
#include "mail_func.h"
#include "util.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netdb.h>
#define MAX_DIGLEN				256*1024

#define SOCKET_TIMEOUT			60


static int g_cidb_port;
static char g_cidb_host[16];
static char g_area_path[128];
static MIME_POOL *g_mime_pool;
static char *opt_config_file = NULL;
static unsigned int opt_show_version;

static struct HXoption g_options_table[] = {
	{.sh = 'c', .type = HXTYPE_STRING, .ptr = &opt_config_file, .help = "Config file to read", .htyp = "FILE"},
	{.ln = "version", .type = HXTYPE_NONE, .ptr = &opt_show_version, .help = "Output version information and exit"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static void insert_directory(const char *dir_path);

static int64_t insert_cidb(MAIL *pmail, char *path);

static int connect_cidb(const char *ip_addr, int port);

int main(int argc, const char **argv)
{
	char *ptoken;
	struct stat node_stat;
	
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	if (opt_show_version) {
		printf("version: %s\n", PROJECT_VERSION);
		return 0;
	}

	if (4 != argc) {
		printf("%s src-dir dst-path cidb-host:port\n", argv[0]);
		return 1;
	}

    
	if (0 != stat(argv[1], &node_stat)) {
		printf("fail to find %s\n", argv[1]);
		return 2;
	}
	
	if (0 == S_ISDIR(node_stat.st_mode)) {
		printf("%s is not a directory\n", argv[1]);
		return 2;
	}
	
	if (0 != stat(argv[2], &node_stat)) {
		printf("fail to find %s\n", argv[2]);
		return 2;
	}
	
	if (0 == S_ISDIR(node_stat.st_mode)) {
		printf("%s is not a directory\n", argv[2]);
		return 2;
	}
	
	strncpy(g_area_path, argv[2], 128);
	
	
	if (NULL == extract_ip(argv[3], g_cidb_host)) {
		printf("cannot find ip address in %s\n", argv[3]);
		return 3;
	}
	
	ptoken = strchr(argv[3], ':');
	if (NULL == ptoken) {
		g_cidb_port = 5556;
	} else {
		g_cidb_port = atoi(ptoken + 1);
		if (g_cidb_port <= 0) {
			printf("port error in %s\n", argv[3]);
			return 3;
		}
	}
	
	g_mime_pool = mime_pool_init(1024, 32, FALSE);
	if (NULL == g_mime_pool) {
		printf("Failed to init MIME pool\n");
		return 4;
	}
	
	insert_directory(argv[1]);
	
	mime_pool_free(g_mime_pool);
	
	return 0;

}

static void insert_directory(const char *dir_path)
{
	DIR *dirp;
	MAIL imail;
	char *pbuff;
	size_t offset;
	int fd, tmp_len;
	int64_t mail_id;
	char temp_path[256];
	char dest_path[128];
	struct stat node_stat;
	struct dirent *direntp;
	
	dirp = opendir(dir_path);
	if (NULL == dirp) {
		return;
	}
	
	while (direntp = readdir(dirp)) {
		if (0 == strcmp(direntp->d_name, ".") ||
			0 == strcmp(direntp->d_name, "..")) {
			continue;	
		}
		snprintf(temp_path, 256, "%s/%s", dir_path, direntp->d_name);
		if (0 != stat(temp_path, &node_stat)) {
			continue;
		}
		
		if (S_ISDIR(node_stat.st_mode)) {
			insert_directory(temp_path);
			continue;
		}
		
		if (0 == S_ISREG(node_stat.st_mode)) {
			continue;
		}
		
		pbuff = malloc(node_stat.st_size);
		if (NULL == pbuff) {
			continue;
		}
		
		fd = open(temp_path, O_RDONLY);
		if (-1 == fd) {
			printf("Failed to open %s: %s\n", temp_path, strerror(errno));
			free(pbuff);
			continue;
		}

		if (node_stat.st_size != read(fd, pbuff, node_stat.st_size)) {
			printf("Failed to read %s: %s\n", temp_path, strerror(errno));
			free(pbuff);
			close(fd);
			continue;
		}

		close(fd);
		
		mail_init(&imail, g_mime_pool);
			
		if (FALSE == mail_retrieve(&imail, pbuff, node_stat.st_size)) {
			mail_free(&imail);
			free(pbuff);
			continue;
		}
		
		
		mail_id = insert_cidb(&imail, dest_path);
		if (mail_id > 0) {
			printf("%s is inserted into archive database\n", direntp->d_name);
			snprintf(temp_path, 256, "%s/%s/%lld", g_area_path, dest_path, mail_id);
			fd = open(temp_path, O_CREAT|O_WRONLY|O_TRUNC, 0666);
			if (-1 != fd) {
				mail_to_file(&imail, fd);
				close(fd);
			}
		} else {
			printf("fail to insert %s into archive database\n", direntp->d_name);
		}
		mail_free(&imail);
		free(pbuff);
		
	}
	
}


static int64_t insert_cidb(MAIL *pmail, char *path)
{
	int i, j;
	int sockd;
	int length;
	int offset;
	MIME *pmime;
	char *ptoken;
	int read_len;
	int field_len;
	int64_t mail_id;
	size_t encode_len;
	char *ptoken_prev;
	EMAIL_ADDR email_addr;
	char temp_address[1024];
	char field_buff[64*1024];
	char envelop_buff[64*1024];
	char temp_buff[2*MAX_DIGLEN];
	
	
	pmime = mail_get_head(pmail);
	if (NULL == pmime) {
		return -1;
	}
	
	if (FALSE == mime_get_field(pmime, "From", field_buff,
		sizeof(field_buff))) {
		offset = sprintf(envelop_buff, "none@none");
	} else {
		parse_email_addr(&email_addr, field_buff);
		if (0 != strlen(email_addr.local_part) &&
			0 != strlen(email_addr.domain)) {
			offset = snprintf(envelop_buff, sizeof(envelop_buff), "%s@%s",
						email_addr.local_part, email_addr.domain);
		} else {
			offset = sprintf(envelop_buff, "none@none");
		}
	}
	
	offset ++;
	
	for (j=0; j<3; j++) {
		switch (j) {
		case 0:
			if (FALSE == mime_get_field(pmime, "To",
				field_buff, sizeof(field_buff))) {
				continue;
			}
			break;
		case 1:
			if (FALSE == mime_get_field(pmime, "Cc",
				field_buff, sizeof(field_buff))) {
				continue;
			}
			break;
		case 2:
			if (FALSE == mime_get_field(pmime, "Bcc",
				field_buff, sizeof(field_buff))) {
				continue;
			}
			break;
		}
		field_len = strlen(field_buff);
		ptoken_prev = field_buff;
		for (i=0; i<=field_len; i++) {
			if (',' == field_buff[i] || ';' == field_buff[i] ||
				'\0' == field_buff[i]) {
				ptoken = field_buff + i;
				memcpy(temp_address, ptoken_prev, ptoken - ptoken_prev);
				temp_address[ptoken - ptoken_prev] = '\0';
				parse_email_addr(&email_addr, temp_address);
				if (0 != strlen(email_addr.local_part) &&
					0 != strlen(email_addr.domain)) {
					offset += snprintf(envelop_buff + offset,
								sizeof(envelop_buff) - offset, "%s@%s",
								email_addr.local_part, email_addr.domain);
					if (offset >= sizeof(envelop_buff) - 1) {
						return FALSE;
					}
					offset ++;
				}
				ptoken_prev = ptoken + 1;
			}
		}
	}
	
	envelop_buff[offset] = '\0';
	offset ++;
	
	length = sprintf(temp_buff, "A-INST ");
	encode64(envelop_buff, offset, temp_buff + length,
		sizeof(temp_buff) - length, &encode_len);
	length += encode_len;

	temp_buff[length] = ' ';
	length ++;
	
	length += sprintf(temp_buff + length, "{\"file\":\"\",");

	if (1 != mail_get_digest(pmail, &encode_len, temp_buff + length,
		sizeof(temp_buff) - length - 2)) {
		return -1;
	}

	length += strlen(temp_buff + length);
	memcpy(temp_buff + length, "}\r\n", 3);
	length += 3;
	
	sockd = connect_cidb(g_cidb_host, g_cidb_port);
	if (-1 == sockd) {
		return -1;
	}
	
	if (length != write(sockd, temp_buff, length)) {
		close(sockd);
		return -1;
	}
	
	read_len = read(sockd, temp_buff, 1024);
	close(sockd);
	if (read_len <= 0) {
        return -1;
	}
	
	if (0 != strncmp(temp_buff + read_len - 2, "\r\n", 2)) {
		return -1;
	}
	temp_buff[read_len - 2] = '\0';
	
	if (0 != strncasecmp(temp_buff, "TRUE ", 5)) {
		close(sockd);
		return -1;
	}
	
	
	ptoken = strchr(temp_buff + 5, ' ');
	if (NULL == ptoken) {
		return -1;
	}
	
	*ptoken = '\0';
	mail_id = atoll(temp_buff + 5);
	
	strncpy(path, ptoken + 1, 128);
	
	return mail_id;
}

static int connect_cidb(const char *ip_addr, int port)
{
    int offset;
    int read_len;
	fd_set myset;
	struct timeval tv;
    char temp_buff[1024];

	int sockd = gx_inet_connect(ip_addr, port, 0);
	if (sockd < 0)
		return -1;
	tv.tv_usec = 0;
	tv.tv_sec = SOCKET_TIMEOUT;
	FD_ZERO(&myset);
	FD_SET(sockd, &myset);
	if (select(sockd + 1, &myset, NULL, NULL, &tv) <= 0) {
		close(sockd);
		return -1;
	}
	read_len = read(sockd, temp_buff, 1024);
	if (read_len <= 0) {
        close(sockd);
        return -1;
	}
	temp_buff[read_len] = '\0';
	if (0 != strcasecmp(temp_buff, "OK\r\n")) {
		close(sockd);
		return -1;
	}
	return sockd;
}

