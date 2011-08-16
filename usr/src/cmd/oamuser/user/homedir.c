/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved  	*/


#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <userdefs.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/mnttab.h>
#include <libzfs.h>
#include <sys/mntent.h>
#include <sys/mount.h>
#include <libgen.h>
#include <limits.h>
#include "messages.h"

#define 	SBUFSZ	(2 * PATH_MAX + 1)
#define 	EXPORTDIR	"/export"
#define		AUTOFS_HOME	"/etc/auto_home"
#define		AUTOFS_LINESZ	4096

extern int rm_files();
extern int edit_autofs_home();
static int rm_homedir();
static char *get_mnt_special();

static char cmdbuf[ SBUFSZ ];	/* buffer for system call */
static char dhome[ PATH_MAX + 1 ]; /* buffer for dirname */
static char bhome[ PATH_MAX + 1 ]; /* buffer for basename */
static char pdir[ PATH_MAX + 1 ]; /* parent directory */
static libzfs_handle_t *g_zfs = NULL;

/*
	Create a home directory and populate with files from skeleton
	directory.
*/
int
create_home(char *homedir, char *skeldir, uid_t uid, gid_t gid, int newfs)
		/* home directory to create */
		/* skel directory to copy if indicated */
		/* uid of new user */
		/* group id of new user */
		/* allow filesystem creation */
{
	struct stat stbuf;
	char *dname, *bname;
	char *dataset;
	FILE *fp;

	if (g_zfs == NULL)
		g_zfs = libzfs_init();

	(void) strcpy(dhome, homedir);
	(void) strcpy(bhome, homedir);
	dname = dirname(dhome);
	bname = basename(bhome);

	(void) strcpy(pdir, dname);
	if ((stat(pdir, &stbuf) != 0) || !S_ISDIR(stbuf.st_mode)) {
		errmsg(M_OOPS, "access the parent directory", strerror(errno));
		return (EX_HOMEDIR);
	}

	if (strcmp(stbuf.st_fstype, MNTTYPE_AUTOFS) == 0) {
		(void) strcpy(pdir, EXPORTDIR);
		(void) strlcat(pdir, dname, PATH_MAX + 1);
		(void) snprintf(homedir, PATH_MAX + 1, "%s/%s", pdir, bname);
		if (stat(pdir, &stbuf) == 0)
			(void) edit_autofs_home(bname, bname, pdir);
	}

	if ((strcmp(stbuf.st_fstype, MNTTYPE_ZFS) == 0) &&
	    (g_zfs != NULL) && newfs &&
	    ((dataset = get_mnt_special(pdir, stbuf.st_fstype)) != NULL)) {
		char nm[ZFS_MAXNAMELEN];
		zfs_handle_t *zhp;

	    	(void) snprintf(nm, ZFS_MAXNAMELEN, "%s/%s", dataset, bname);

		if ((zfs_create(g_zfs, nm, ZFS_TYPE_FILESYSTEM, NULL) != 0) ||
	    	    ((zhp = zfs_open(g_zfs, nm, ZFS_TYPE_FILESYSTEM)) ==
		    NULL)) {
			errmsg(M_OOPS, "create the home directory",
			    libzfs_error_description(g_zfs));
			return (EX_HOMEDIR);
		}

		if (zfs_mount(zhp, NULL, 0) != 0) {
			errmsg(M_OOPS, "mount the home directory",
			    libzfs_error_description(g_zfs));
			(void) zfs_destroy(zhp, B_FALSE);
			return (EX_HOMEDIR);
		}

		zfs_close(zhp);

		if (chmod(homedir, S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH) != 0) {
			errmsg(M_OOPS, "change permissions of home directory",
			    strerror(errno));
			return (EX_HOMEDIR);
		}
	} else {
		if (mkdir(homedir, S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH) != 0) {
			errmsg(M_OOPS, "create the home directory",
			    strerror(errno));
			return (EX_HOMEDIR);
		}
	}

	if( chown(homedir, uid, gid) != 0 ) {
		errmsg(M_OOPS, "change ownership of home directory", 
		    strerror(errno));
		return( EX_HOMEDIR );
	}

	if(skeldir) {
		/* copy the skel_dir into the home directory */
		(void) sprintf( cmdbuf, "cd %s && find . -print | cpio -pd %s",
			skeldir, homedir);

		if( system( cmdbuf ) != 0 ) {
			errmsg(M_OOPS, "copy skeleton directory into home "
			    "directory", strerror(errno));
			(void) rm_homedir( homedir );
			return( EX_HOMEDIR );
		}

		/* make sure contents in the home dirctory have correct owner */
		(void) sprintf( cmdbuf,"cd %s && find . -exec chown %ld {} \\;",
			homedir, uid );
		if( system( cmdbuf ) != 0) {
			errmsg(M_OOPS, "change owner of files home directory",
			    strerror(errno));

			(void) rm_homedir( homedir );
			return( EX_HOMEDIR );
		}

		/* and group....... */
		(void) sprintf( cmdbuf, "cd %s && find . -exec chgrp %ld {} \\;",
			homedir, gid );
		if( system( cmdbuf ) != 0) {
			errmsg(M_OOPS, "change group of files home directory",
			    strerror(errno));
			(void) rm_homedir( homedir );
			return( EX_HOMEDIR );
		}
	}
	return( EX_SUCCESS );
}

/* Remove a home directory structure */
int
rm_homedir(char *dir)
{
	struct stat stbuf;
	char *nm;

	if ((stat(dir, &stbuf) != 0) || !S_ISDIR(stbuf.st_mode))
		return 0;

	if (g_zfs == NULL)
		g_zfs = libzfs_init();

	if ((strcmp(stbuf.st_fstype, MNTTYPE_ZFS) == 0) && 
	    (g_zfs != NULL) &&
	    ((nm = get_mnt_special(dir, stbuf.st_fstype)) != NULL)) {
		zfs_handle_t *zhp;

	    	if ((zhp = zfs_open(g_zfs, nm, ZFS_TYPE_FILESYSTEM)) != NULL) {
			if ((zfs_unmount(zhp, NULL, 0) == 0) &&
			    (zfs_destroy(zhp, B_FALSE) == 0)) {
				zfs_close(zhp);
				return 0;
			}

			(void) zfs_mount(zhp, NULL, 0);
			zfs_close(zhp);
		}
	}

	(void) sprintf(cmdbuf, "rm -rf %s", dir);

	return (system(cmdbuf));
}

int
rm_files(char *homedir, char *user)
{
	struct stat stbuf;
	char *dname, *bname;
	int autofs = 0;

	(void) strcpy(dhome, homedir);
	(void) strcpy(bhome, homedir);
	dname = dirname(dhome);
	bname = basename(bhome);

	(void) strcpy(pdir, dname);
	if ((stat(pdir, &stbuf) != 0) || !S_ISDIR(stbuf.st_mode))
		return (EX_SUCCESS);

	if (strcmp(stbuf.st_fstype, MNTTYPE_AUTOFS) == 0)
		autofs = 1;

	if (autofs) {
		(void) umount2(homedir, 0);
		(void) strcpy(pdir, EXPORTDIR);
		(void) strlcat(pdir, dname, PATH_MAX + 1);
		(void) snprintf(homedir, PATH_MAX + 1, "%s/%s", pdir, bname);
	}

        if (rm_homedir(homedir) != 0) {
                errmsg(M_RMFILES);
                return (EX_HOMEDIR);
        }

	if (autofs)
		(void) edit_autofs_home(bname, (char *)NULL, (char *) NULL);

        return (EX_SUCCESS);
}

/* Get the name of a mounted filesytem */
char *
get_mnt_special(char *mountp, char *fstype)
{
	struct mnttab entry, search;
	char *special = NULL;
	FILE *fp;

	search.mnt_special = search.mnt_mntopts = search.mnt_time = NULL;
	search.mnt_mountp = mountp;
	search.mnt_fstype = fstype;

	if ((fp = fopen(MNTTAB, "r")) != NULL) {
		if (getmntany(fp, &entry, &search) == 0)
			special = entry.mnt_special;

		(void) fclose(fp);
	}

	return special;
}

/* Modify autofs map for home directories */
int
edit_autofs_home(char *dir, char *new_dir, char *new_pdir)
{
	char t_name[] = "/etc/htmp.XXXXXX";
	char *first;
	FILE *e_fp, *t_fp;
	struct stat stbuf;
	char workbuf[AUTOFS_LINESZ];
	char linebuf[AUTOFS_LINESZ];
	int fd, modified = 0;

	if ((e_fp = fopen(AUTOFS_HOME, "r")) == NULL)
		return (EX_UPDATE);

	if (fstat(fileno(e_fp), &stbuf) != 0) {
		(void) fclose(e_fp);
		return (EX_UPDATE);
	}

	/* Make sure it's not an executable map */
	if (stbuf.st_mode & S_IXUSR) {
		(void) fclose(e_fp);
		return (EX_SUCCESS);
	}

	if ((fd = mkstemp(t_name)) == -1) {
		(void) fclose(e_fp);
		return (EX_UPDATE);
	}

	if ((t_fp = fdopen(fd, "w")) == NULL) {
		(void) close(fd);
		(void) unlink(t_name);
		(void) fclose(e_fp);
		return (EX_UPDATE);
	}
	
	/*
	 * Get ownership and permissions corrrect
	 */

	if (fchmod(fd, stbuf.st_mode) != 0 ||
	    fchown(fd, stbuf.st_uid, stbuf.st_gid) != 0) {
		(void) fclose(t_fp);
		(void) fclose(e_fp);
		(void) unlink(t_name);
		return (EX_UPDATE);
	}

	/* Make TMP file look like we want auto_home to look */
	while (fgets(linebuf, AUTOFS_LINESZ, e_fp) != NULL) {
		if (dir) {
			(void) strcpy(workbuf, linebuf);
			first = strtok(workbuf, " 	\\\"");
			if ((first != NULL) && (strcmp(first, dir) == 0)) {
				modified++;
				if (new_dir != NULL) {
		   	 		(void) snprintf(linebuf, AUTOFS_LINESZ,
					    "%s	localhost:%s/&\n",
					    new_dir, new_pdir);
				} else {
					continue;
				}
			}
		}
		if (fputs(linebuf, t_fp) == EOF) {
			(void) fclose(t_fp);
			(void) fclose(e_fp);
			(void) unlink(t_name);
			return (EX_UPDATE);
		}
	}

	if (!modified && (new_dir != NULL)) {
		modified++;
		(void) snprintf(linebuf, AUTOFS_LINESZ, "%s	localhost:%s/&\n",
		    new_dir, new_pdir);
		if (fputs(linebuf, t_fp) == EOF) {
			(void) fclose(t_fp);
			(void) fclose(e_fp);
			(void) unlink(t_name);
			return (EX_UPDATE);
		}
	}

	(void) fclose(e_fp);
	(void) fclose(t_fp);

	/* Now, update auto_home, if it was modified */
	if (modified && rename(t_name, AUTOFS_HOME) < 0) {
		(void) unlink(t_name);
		return (EX_UPDATE);
	}

	(void) unlink(t_name);
	return (EX_SUCCESS);
}
