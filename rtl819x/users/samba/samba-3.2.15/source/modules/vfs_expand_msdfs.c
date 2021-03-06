/* 
 * Expand msdfs targets based on client IP
 *
 * Copyright (C) Volker Lendecke, 2004
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_VFS

extern userdom_struct current_user_info;

/**********************************************************
  Under mapfile we expect a table of the following format:

  IP-Prefix whitespace expansion

  For example:
  192.168.234 local.samba.org
  192.168     remote.samba.org
              default.samba.org

  This is to redirect a DFS client to a host close to it.
***********************************************************/

static char *read_target_host(TALLOC_CTX *ctx, const char *mapfile)
{
	XFILE *f;
	char buf[1024];
	char *space = buf;
	bool found = false;

	f = x_fopen(mapfile, O_RDONLY, 0);

	if (f == NULL) {
		DEBUG(0,("can't open IP map %s. Error %s\n",
			 mapfile, strerror(errno) ));
		return NULL;
	}

	DEBUG(10, ("Scanning mapfile [%s]\n", mapfile));

	while (x_fgets(buf, sizeof(buf), f) != NULL) {
		char addr[INET6_ADDRSTRLEN];

		if ((strlen(buf) > 0) && (buf[strlen(buf)-1] == '\n'))
			buf[strlen(buf)-1] = '\0';

		DEBUG(10, ("Scanning line [%s]\n", buf));

		space = strchr_m(buf, ' ');

		if (space == NULL) {
			DEBUG(0, ("Ignoring invalid line %s\n", buf));
			continue;
		}

		*space = '\0';

		if (strncmp(client_addr(get_client_fd(),addr,sizeof(addr)),
				buf, strlen(buf)) == 0) {
			found = true;
			break;
		}
	}

	x_fclose(f);

	if (!found) {
		return NULL;
	}

	space += 1;

	while (isspace(*space))
		space += 1;

	return talloc_strdup(ctx, space);
}

/**********************************************************

  Expand the msdfs target host using read_target_host
  explained above. The syntax used in the msdfs link is

  msdfs:@table-filename@/share

  Everything between and including the two @-signs is
  replaced by the substitution string found in the table
  described above.

***********************************************************/

static char *expand_msdfs_target(TALLOC_CTX *ctx,
				connection_struct *conn,
				char *target)
{
	char *mapfilename = NULL;
	char *filename_start = strchr_m(target, '@');
	char *filename_end = NULL;
	int filename_len = 0;
	char *targethost = NULL;
	char *new_target = NULL;

	if (filename_start == NULL) {
		DEBUG(10, ("No filename start in %s\n", target));
		return NULL;
	}

	filename_end = strchr_m(filename_start+1, '@');

	if (filename_end == NULL) {
		DEBUG(10, ("No filename end in %s\n", target));
		return NULL;
	}

	filename_len = PTR_DIFF(filename_end, filename_start+1);
	mapfilename = talloc_strdup(ctx, filename_start+1);
	if (!mapfilename) {
		return NULL;
	}
	mapfilename[filename_len] = '\0';

	DEBUG(10, ("Expanding from table [%s]\n", mapfilename));

	if ((targethost = read_target_host(ctx, mapfilename)) == NULL) {
		DEBUG(1, ("Could not expand target host from file %s\n",
			  mapfilename));
		return NULL;
	}

	targethost = talloc_sub_advanced(ctx,
				lp_servicename(SNUM(conn)),
				conn->user,
				conn->connectpath,
				conn->gid,
				get_current_username(),
				current_user_info.domain,
				targethost);

	DEBUG(10, ("Expanded targethost to %s\n", targethost));

	/* Replace the part between '@...@' */
	*filename_start = '\0';
	new_target = talloc_asprintf(ctx,
				"%s%s%s",
				target,
				targethost,
				filename_end+1);
	if (!new_target) {
		return NULL;
	}

	DEBUG(10, ("New DFS target: %s\n", new_target));
	return new_target;
}

static int expand_msdfs_readlink(struct vfs_handle_struct *handle,
				 const char *path, char *buf, size_t bufsiz)
{
	TALLOC_CTX *ctx = talloc_tos();
	int result;
	char *target = TALLOC_ARRAY(ctx, char, PATH_MAX+1);

	if (!target) {
		errno = ENOMEM;
		return -1;
	}
	result = SMB_VFS_NEXT_READLINK(handle, path, target,
				       PATH_MAX);

	if (result < 0)
		return result;

	target[result] = '\0';

	if ((strncmp(target, "msdfs:", strlen("msdfs:")) == 0) &&
	    (strchr_m(target, '@') != NULL)) {
		target = expand_msdfs_target(ctx, handle->conn, target);
		if (!target) {
			errno = ENOENT;
			return -1;
		}
	}

	safe_strcpy(buf, target, bufsiz-1);
	return strlen(buf);
}

/* VFS operations structure */

static vfs_op_tuple expand_msdfs_ops[] = {
	{SMB_VFS_OP(expand_msdfs_readlink), SMB_VFS_OP_READLINK,
	 SMB_VFS_LAYER_TRANSPARENT},
	{SMB_VFS_OP(NULL), SMB_VFS_OP_NOOP, SMB_VFS_LAYER_NOOP}
};

NTSTATUS vfs_expand_msdfs_init(void);
NTSTATUS vfs_expand_msdfs_init(void)
{
	return smb_register_vfs(SMB_VFS_INTERFACE_VERSION, "expand_msdfs",
				expand_msdfs_ops);
}
