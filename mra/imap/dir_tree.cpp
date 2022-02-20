// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstring>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/mem_file.hpp>
#include <gromox/simple_tree.hpp>
#include "dir_tree.hpp"

static void dir_tree_enum_delete(SIMPLE_TREE_NODE *pnode)
{
	DIR_NODE *pdir;

	pdir = (DIR_NODE*)pnode->pdata;
	pdir->ppool->put(pdir);
}

dir_tree::dir_tree(LIB_BUFFER *a) : ppool(a)
{
	auto ptree = this;
	simple_tree_init(&ptree->tree);
}

void dir_tree::retrieve(MEM_FILE *pfile)
{
	auto ptree = this;
	char *ptr1, *ptr2;
	char temp_path[4096 + 1];
	SIMPLE_TREE_NODE *pnode, *pnode_parent;

	auto proot = ptree->tree.get_root();
	if (NULL == proot) {
		auto pdir = ptree->ppool->get<DIR_NODE>();
		pdir->node.pdata = pdir;
		pdir->name[0] = '\0';
		pdir->b_loaded = TRUE;
		pdir->ppool = ptree->ppool;
		simple_tree_set_root(&ptree->tree, &pdir->node);
		proot = &pdir->node;
	}

	
	pfile->seek(MEM_FILE_READ_PTR, 0, MEM_FILE_SEEK_BEGIN);
	size_t len;
	while ((len = pfile->readline(temp_path, 4096)) != MEM_END_OF_FILE) {
		pnode = proot;
		if (len == 0 || temp_path[len-1] != '/') {
			temp_path[len] = '/';
			len ++;
			temp_path[len] = '\0';
		}
		ptr1 = temp_path;
		while ((ptr2 = strchr(ptr1, '/')) != NULL) {
			*ptr2 = '\0';
			pnode_parent = pnode;
			pnode = pnode->get_child();
			if (NULL != pnode) {
				do {
					auto pdir = static_cast<DIR_NODE *>(pnode->pdata);
					if (0 == strcmp(pdir->name, ptr1)) {
						break;
					}
				} while ((pnode = pnode->get_sibling()) != nullptr);
			}

			if (NULL == pnode) {
				auto pdir = ptree->ppool->get<DIR_NODE>();
				pdir->node.pdata = pdir;
				gx_strlcpy(pdir->name, ptr1, gromox::arsizeof(pdir->name));
				pdir->b_loaded = FALSE;
				pdir->ppool = ptree->ppool;
				pnode = &pdir->node;
				simple_tree_add_child(&ptree->tree, pnode_parent, pnode,
					SIMPLE_TREE_ADD_LAST);
			}
			ptr1 = ptr2 + 1;
		}

		((DIR_NODE*)(pnode->pdata))->b_loaded = TRUE;
		
	}

}

static void dir_tree_clear(DIR_TREE *ptree)
{
	auto pnode = ptree->tree.get_root();
	if (NULL != pnode) {
		simple_tree_destroy_node(&ptree->tree, pnode,
			dir_tree_enum_delete);
	}

}

DIR_NODE *dir_tree::match(const char *path)
{
	auto ptree = this;
	int len;
	DIR_NODE *pdir = nullptr;
	char *ptr1, *ptr2;
	char temp_path[4096 + 1];

	auto pnode = ptree->tree.get_root();
	if (NULL == pnode) {
		return NULL;
	}

	if ('\0' == path[0]) {
		return static_cast<DIR_NODE *>(pnode->pdata);
	}

	len = strlen(path);
	if (len >= 4096) {
		return NULL;
	}
	memcpy(temp_path, path, len);
	if ('/' != temp_path[len - 1]) {
		temp_path[len] = '/';
		len ++;
	}
	temp_path[len] = '\0';
	
	ptr1 = temp_path;
	while ((ptr2 = strchr(ptr1, '/')) != NULL) {
		*ptr2 = '\0';
		pnode = pnode->get_child();
		if (NULL == pnode) {
			return NULL;
		}
		do {
			pdir = (DIR_NODE*)pnode->pdata;
			if (0 == strcmp(pdir->name, ptr1)) {
				break;
			}
		} while ((pnode = pnode->get_sibling()) != nullptr);
		if (NULL == pnode) {
			return NULL;
		}

		ptr1 = ptr2 + 1;
	}

	return pdir;
}

DIR_NODE *dir_tree::get_child(DIR_NODE* pdir)
{
	auto pnode = pdir->node.get_child();
	if (NULL != pnode) {
		return static_cast<DIR_NODE *>(pnode->pdata);
	} else {
		return NULL;
	}
}

dir_tree::~dir_tree()
{
	auto ptree = this;
	dir_tree_clear(ptree);
	ptree->tree.clear();
	ptree->ppool = NULL;
}

