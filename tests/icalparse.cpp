#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <libHX/io.h>
#include <gromox/fileio.h>
#include <gromox/ical.hpp>
#include <gromox/mapi_types.hpp>
#include <gromox/oxcical.hpp>
#include <gromox/scope.hpp>

using namespace gromox;

static BOOL get_propids(const PROPNAME_ARRAY *pn, PROPID_ARRAY *id)
{
	id->count = pn->count;
	id->ppropid = new uint16_t[id->count]{1};
	return TRUE;
}

static BOOL un_to_eid(const char *username, const char *dispname, BINARY *bv,
    enum display_type *)
{
	bv->pc = strdup(username);
	bv->cb = strlen(username);
	return TRUE;
}

int main(int argc, const char **argv)
{
	std::unique_ptr<char[], stdlib_delete> data;
	if (argc >= 2)
		data.reset(HX_slurp_file(argv[1], nullptr));
	else
		data.reset(HX_slurp_fd(STDIN_FILENO, nullptr));
	ICAL ical;
	if (ical.init() < 0) {
		printf("BAD ical_init\n");
		return EXIT_FAILURE;
	}
	if (!ical.retrieve(data.get()))
		printf("BAD retrieve\n");
	auto msg = oxcical_import("UTC", &ical, malloc, get_propids, un_to_eid);
	if (msg == nullptr)
		printf("BAD import\n");
	return 0;
}
