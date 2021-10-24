#pragma once
#include <gromox/lib_buffer.hpp>
#include <gromox/double_list.hpp>
#define STREAM_BLOCK_SIZE    0x10000
#define STREAM_ALLOC_SIZE    (STREAM_BLOCK_SIZE + sizeof(DOUBLE_LIST_NODE))

/**
 * %STREAM_LINE_FAIL: mail envelope lines overflow the first buffer of stream
 */
enum {
    STREAM_LINE_ERROR = -2, 
    STREAM_LINE_FAIL,
    STREAM_LINE_UNAVAILABLE = 0,
    STREAM_LINE_AVAILABLE,
};

enum {
    STREAM_EOM_ERROR = -1,
    STREAM_EOM_NONE = 0,
    STREAM_EOM_NET,
    STREAM_EOM_DIRTY,
};

enum {
	STREAM_COPY_ERROR = -3,
    STREAM_COPY_PART,
    STREAM_COPY_TERM,
    STREAM_COPY_OK,
    STREAM_COPY_END
};

enum {
    STREAM_DUMP_FAIL = -1,
    STREAM_DUMP_OK
};

enum {
	STREAM_WRITE_FAIL = -1,
	STREAM_WRITE_OK
};

struct STREAM {
	STREAM(LIB_BUFFER *);
	STREAM(STREAM &&) = delete;
	STREAM &operator=(STREAM &&);
	~STREAM();

	int has_newline() const { return line_result; }
	unsigned int readline(char **);
	void try_mark_line();
	void try_mark_eom();
	int has_eom();
	size_t get_total_length() const { return wr_total_pos; }
	int copyline(char *buf, unsigned int *size);
	unsigned int peek_buffer(char *, unsigned int) const;
	int dump(int fd);

	DOUBLE_LIST_NODE *pnode_rd = nullptr, *pnode_wr = nullptr;
	int line_result = 0, eom_result = 0;
	size_t rd_block_pos = 0, wr_block_pos = 0;
	size_t rd_total_pos = 0, wr_total_pos = 0;
	size_t last_eom_parse = 0;
	size_t block_line_parse = 0, block_line_pos = 0;
	LIB_BUFFER *allocator = nullptr;
	DOUBLE_LIST list{};

	protected:
	STREAM(const STREAM &);
	void operator=(const STREAM &) = delete;
	void xcopy(const STREAM &);
	bool is_clone = false;
	friend void stream_split_eom(STREAM *, STREAM *);
};

void stream_clear(STREAM *pstream);
void stream_split_eom(STREAM *pstream, STREAM *pstream_second);
extern void *stream_getbuffer_for_writing(STREAM *pstream, unsigned int *psize);
unsigned int stream_forward_writing_ptr(STREAM *pstream, unsigned int offset);
unsigned int stream_forward_reading_ptr(STREAM *pstream, unsigned int offset);
unsigned int stream_backward_writing_ptr(STREAM *pstream, unsigned int offset);
unsigned int stream_backward_reading_ptr(STREAM *pstream, unsigned int offset);
extern void *stream_getbuffer_for_reading(STREAM *pstream, unsigned int *psize);
void stream_reset_reading(STREAM *pstream);
extern int stream_write(STREAM *pstream, const void *pbuff, size_t size);
