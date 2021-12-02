#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <gromox/mapi_types.hpp>
#include <gromox/double_list.hpp>
#include <sys/types.h>
#define FTSTREAM_PRODUCER_POINT_LENGTH			1024
#define FTSTREAM_PRODUCER_BUFFER_LENGTH			4*1024*1024
#define STRING_OPTION_NONE						0x00
#define STRING_OPTION_UNICODE					0x01
#define STRING_OPTION_CPID						0x02
#define STRING_OPTION_FORCE_UNICODE				0x08

struct ATTACHMENT_CONTENT;
struct FOLDER_CHANGES;
struct logon_object;
struct MESSAGE_CONTENT;
struct MSGCHG_PARTIAL;
struct PROGRESS_INFORMATION;
struct PROGRESS_MESSAGE;

struct fxstream_producer {
	protected:
	fxstream_producer();
	NOMOVE(fxstream_producer);

	public:
	~fxstream_producer();
	static std::unique_ptr<fxstream_producer> create(logon_object *, uint8_t string_option);
	inline int total_length() const { return offset; }
	BOOL read_buffer(void *buf, uint16_t *len, BOOL *last);
	BOOL write_uint32(uint32_t);
	BOOL write_proplist(const TPROPVAL_ARRAY *);
	BOOL write_attachmentcontent(BOOL delprop, const ATTACHMENT_CONTENT *);
	BOOL write_messagecontent(BOOL delprop, const MESSAGE_CONTENT *);
	BOOL write_message(const MESSAGE_CONTENT *);
	BOOL write_progresstotal(const PROGRESS_INFORMATION *);
	BOOL write_progresspermessage(const PROGRESS_MESSAGE *);
	BOOL write_messagechangefull(const TPROPVAL_ARRAY *chgheader, MESSAGE_CONTENT *);
	BOOL write_messagechangepartial(const TPROPVAL_ARRAY *chgheader, const MSGCHG_PARTIAL *msg);
	BOOL write_deletions(const TPROPVAL_ARRAY *);
	BOOL write_readstatechanges(const TPROPVAL_ARRAY *);
	BOOL write_state(const TPROPVAL_ARRAY *);
	BOOL write_hierarchysync(const FOLDER_CHANGES *fldchgs, const TPROPVAL_ARRAY *del, const TPROPVAL_ARRAY *state);

	int type = 0, fd = -1;
	uint32_t offset = 0;
	std::string path;
	uint8_t buffer[FTSTREAM_PRODUCER_BUFFER_LENGTH]{};
	uint32_t buffer_offset = 0, read_offset = 0;
	uint8_t string_option = 0;
	logon_object *plogon = nullptr; /* plogon is a protected member */
	DOUBLE_LIST bp_list{};
	BOOL b_read = false;
};
using FTSTREAM_PRODUCER = fxstream_producer;
using ftstream_producer = fxstream_producer;
