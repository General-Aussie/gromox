// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#include <cstdio>
#include <sqlite3.h>
#include <gromox/database.h>

xstmt gx_sql_prep(sqlite3 *db, const char *query)
{
	xstmt out;
	int ret = sqlite3_prepare_v2(db, query, -1, &out.m_ptr, nullptr);
	if (ret != SQLITE_OK)
		fprintf(stderr, "sqlite3_prepare_v2 \"%s\": %s\n",
		        query, sqlite3_errstr(ret));
	return out;
}

xtransaction &xtransaction::operator=(xtransaction &&o)
{
	if (m_db != nullptr)
		sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
	m_db = o.m_db;
	o.m_db = nullptr;
	return *this;
}

xtransaction::~xtransaction()
{
	if (m_db != nullptr)
		sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
}

void xtransaction::commit()
{
	sqlite3_exec(m_db, "COMMIT TRANSACTION", nullptr, nullptr, nullptr);
	m_db = nullptr;
}

xtransaction gx_sql_begin_trans(sqlite3 *db)
{
	sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
	return xtransaction(db);
}

int gx_sql_exec(sqlite3 *db, const char *query, unsigned int flags)
{
	char *estr = nullptr;
	auto ret = sqlite3_exec(db, query, nullptr, nullptr, &estr);
	if (ret == SQLITE_OK)
		return ret;
	else if (ret == SQLITE_CONSTRAINT && (flags & SQLEXEC_SILENT_CONSTRAINT))
		;
	else
		fprintf(stderr, "sqlite3_exec \"%s\": %s\n", query,
		        estr != nullptr ? estr : sqlite3_errstr(ret));
	sqlite3_free(estr);
	return ret;
}
