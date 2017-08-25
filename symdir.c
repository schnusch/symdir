/*
Copyright 2017 Schnusch

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHUNKSIZE 4096
#define MAX(a, b)  ((a) ^ (((a) ^ (b)) & -((a) < (b))))

static int verbosity = 0;
static const char *argv0 = NULL;
#define LOG(lvl, fp, fmt, ...) (void)(verbosity >= lvl \
		? fprintf(fp, "%s: " fmt "%.*s\n", argv0, __VA_ARGS__) : 0)
#define DEBUG(...)  LOG(2, stdout, __VA_ARGS__, 0, "")
#define INFO(...)   LOG(1, stdout, __VA_ARGS__, 0, "")
#define WARN(...)   LOG(0, stderr, __VA_ARGS__, 0, "")
#define ERROR(...)  LOG(0, stderr, __VA_ARGS__, 0, "")

struct asd {
	const char *coll;
	struct {
		char  *buf;
		size_t off;
		size_t len;
		size_t buflen;
	} path;
	struct {
		char  *buf;
		size_t buflen;
	} link;
};
#define PATHFMT "%s%s%s%s%s"
#define DIRPATH(s, name) \
		(s)->path.buf, \
		"/",           \
		"",            \
		"",            \
		(name) ? (name) : ""
#define COLLPATH(s, name) \
		(s)->coll                     ? (s)->coll                     : "", \
		(s)->coll                     ? "/"                           : "", \
		(s)->path.len > (s)->path.off ? (s)->path.buf + (s)->path.off : "", \
		(s)->path.len > (s)->path.off ? "/"                           : "", \
		(name)                        ? (name)                        :     \
				(s)->coll || (s)->path.len > (s)->path.off ? "" : "."

#define INVALID_SYMLINK_ERROR(stuff, name) \
		(WARN("invalid symlink "PATHFMT": %s", COLLPATH(stuff, name), (stuff)->link.buf), FLAG_WARN)
#define DIR_CONFLICT_ERROR(stuff, name, stdir, stcoll) \
		(ERROR(PATHFMT" is a %s but "PATHFMT" is a %s%s%s",          \
				DIRPATH((stuff), name),  filetype((stdir).st_mode),  \
				COLLPATH((stuff), name), filetype((stcoll).st_mode), \
				S_ISLNK((stcoll).st_mode) ? " to "            : "",  \
				S_ISLNK((stcoll).st_mode) ? (stuff)->link.buf : ""), FLAG_ERROR | FLAG_NONEMPTY)

#define SKIP_NONLINK_MSG(stuff, name) \
		(DEBUG("skipped "PATHFMT, COLLPATH(stuff, name)), FLAG_NONEMPTY)
#define KEEP_LINK_MSG(stuff, name) \
		(DEBUG("kept    "PATHFMT, COLLPATH(stuff, name)), FLAG_NONEMPTY)

enum {
	FLAG_ERROR    = 1,
	FLAG_WARN     = 2,
	FLAG_NONEMPTY = 4
};

typedef int (*command_func)(int, DIR *, int, DIR *, struct asd *, int);

static void normalize_path(char *dst, const char *src)
{
	int abs = 0;
	if(*src == '/')
	{
		abs = 1;
		*dst++ = *src++;
		if(src[0] == '/' && src[1] != '/')
			*dst++ = *src++;
	}
	char *start = dst;
	char *down  = dst;
	while(1)
	{
		// ignore additional /
		while(*src == '/')
			src++;

		if(!*src)
			break;

		if(src[0] == '.')
		{
			if(src[1] == '/' || !src[1])
			{
				// ignore .
				src++;
				continue;
			}
			else if(src[1] == '.' && (src[2] == '/' || !src[2]))
			{
				if(dst == down)
				{
					if(!abs)
					{
						// append ..
						*dst++ = '.';
						*dst++ = '.';
						*dst++ = '/';
						down = dst;
					}
				}
				else
				{
					// pop last name
					char *dst2 = memrchr(down, '/', dst - 1 - down);
					dst = dst2 ? dst2 + 1 : down;
				}
				src += 2;
				continue;
			}
		}
		// append name
		char *end = strchr(src, '/');
		if(end)
		{
			memmove(dst, src, end + 1 - src);
			dst += end + 1 - src, src = end + 1;
		}
		else
		{
			while(*src)
				*dst++ = *src++;
			break;
		}
	}
	if(dst == start && !abs)
	{
		*dst++ = '.';
		*dst   = '\0';
	}
	else if(dst > start && dst[-1] == '/')
		dst[-1] = '\0';
	else
		dst[0] = '\0';
}

static int is_normalized_path(const char *path)
{
	if(*path == '/')
		path++;
	if(*path == '/')
		path++;
	do
	{
		if(*path == '/')
			return 0;
		if(*path == '.')
		{
			path++;
			if(*path == '/' || !*path)
				return 0;
			if(*path == '.')
			{
				path++;
				if(*path == '/' || !*path)
					return 0;
			}
		}
		path = strchr(path, '/');
		if(path)
			path++;
	}
	while(path);
	return 1;
}

static int opendirat(DIR **d, int dirfd, const char *path)
{
	int fd = openat(dirfd, path, O_DIRECTORY | (d ? O_RDONLY : O_PATH));
	if(fd < 0)
		return -1;
	int tmpfd;
	if(d && ((tmpfd = dup(fd)) < 0 || !(*d = fdopendir(tmpfd))))
	{
		int errbak = errno;
		if(tmpfd >= 0)
			close(tmpfd);
		close(fd);
		errno = errbak;
		return -1;
	}
	return fd;
}

static int growing_getcwd(struct asd *stuff)
{
	while(1)
	{
		void *tmp;
		if(!stuff->path.buf)
			goto resize;
		else if(!getcwd(stuff->path.buf, stuff->path.buflen))
		{
			if(errno != ERANGE)
				return -1;
		resize:
			tmp = realloc(stuff->path.buf, stuff->path.buflen + CHUNKSIZE);
			if(!tmp)
				return -1;
			stuff->path.buf = tmp, stuff->path.buflen += CHUNKSIZE;
		}
		else
			return 0;
	}
}

static int prepare_dir_path(struct asd *stuff, const char *dir)
{
	if(*dir != '/')
	{
		// make path absolute
		if(growing_getcwd(stuff) < 0)
			return -1;
	}
	size_t cwdlen = stuff->path.buf ? strlen(stuff->path.buf) : 0;
	stuff->path.len = cwdlen + strlen(dir) + 2;
	if(stuff->path.len > stuff->path.buflen)
	{
		stuff->path.buflen = (stuff->path.len + CHUNKSIZE - 1) & ~(CHUNKSIZE - 1);
		void *tmp = realloc(stuff->path.buf, stuff->path.buflen);
		if(!tmp)
			return -1;
		stuff->path.buf = tmp;
	}
	if(cwdlen > 0)
		stuff->path.buf[cwdlen++] = '/';
	strcpy(stuff->path.buf + cwdlen, dir);

	normalize_path(stuff->path.buf, stuff->path.buf);

	stuff->path.len = strlen(stuff->path.buf);
	stuff->path.off = stuff->path.len + 1;
	stuff->path.buf[stuff->path.off] = '\0';

	return 0;
}

static const char *filetype(mode_t mode)
{
	switch(mode & S_IFMT)
	{
	case S_IFREG:
		return "file";
	case S_IFDIR:
		return "directory";
	case S_IFLNK:
		return "symlink";
	case S_IFSOCK:
		return "socket";
	case S_IFIFO:
		return "named pipe";
	case S_IFCHR:
		return "character device";
	case S_IFBLK:
		return "block device";
	default:
		return "unknown file";
	}
}

static int path_append(struct asd *stuff, const char *name)
{
	size_t len = stuff->path.len + strlen(name) + 2;
	if(!stuff->path.buf || len > stuff->path.buflen)
	{
		len = (len + CHUNKSIZE - 1) & ~(CHUNKSIZE - 1);
		char *tmp = realloc(stuff->path.buf, len);
		if(!tmp)
			return -1;
		stuff->path.buf    = tmp;
		stuff->path.buflen = len;
	}
	stuff->path.buf[stuff->path.len++] = '/';
	stuff->path.len = stpcpy(stuff->path.buf + stuff->path.len, name) - stuff->path.buf;
	return 0;
}

static void path_remove(struct asd *stuff, size_t len)
{
	stuff->path.len = len;
	stuff->path.buf[len] = '\0';
}

static int path_eq_link(struct asd *stuff, const char *name)
{
	return strncmp(stuff->link.buf, stuff->path.buf, stuff->path.len) == 0
			&& stuff->link.buf[stuff->path.len] == '/'
			&& strcmp(stuff->link.buf + stuff->path.len + 1, name) == 0;
}

static int path_valid_link(struct asd *stuff, const char *name)
{
	return is_normalized_path(stuff->link.buf) && 1;
}

static int growing_readlinkat(int dirfd, const char *name, struct asd *stuff)
{
	ssize_t len;
	while(!stuff->link.buf || (len = readlinkat(dirfd, name, stuff->link.buf,
			stuff->link.buflen)) == (ssize_t)stuff->link.buflen)
	{
		void *tmp = realloc(stuff->link.buf, stuff->link.buflen + CHUNKSIZE);
		if(!tmp)
			return -1;
		stuff->link.buf = tmp, stuff->link.buflen += CHUNKSIZE;
	}
	if(len < 0)
		return -1;
	stuff->link.buf[len] = '\0';
	return 0;
}

static int cmd_add    (int, DIR *, int, DIR *, struct asd *, int);
static int cmd_rm     (int, DIR *, int, DIR *, struct asd *, int);
static int cmd_refresh(int, DIR *, int, DIR *, struct asd *, int);

static int go_deeper(command_func cmd, int fdsrc, int fdsym, const char *name, struct asd *stuff, int depth, const char *namesym)
{
	size_t off = stuff->path.len;
	if(!namesym && path_append(stuff, name) < 0)
	{
		ERROR("cannot access "PATHFMT": %s", DIRPATH(stuff, name), strerror(errno));
		return FLAG_ERROR | FLAG_NONEMPTY;
	}

	int  flags = 0;
	DIR *dsrc  = NULL;
	DIR *dsym  = NULL;

	if(fdsrc != -1)
	{
		fdsrc = opendirat(cmd == cmd_rm ? NULL : &dsrc, fdsrc, name);
		if(fdsrc < 0)
		{
			if(errno == ENOENT)
				goto skip;
			ERROR("cannot open %s: %s", stuff->path.buf, strerror(errno));
			fdsym = -1;
			goto error;
		}
	}

	if(namesym)
		name = namesym;
	fdsym = opendirat(cmd == cmd_add ? NULL : &dsym, fdsym, name);
	if(fdsym < 0)
	{
		if(errno != ENOENT)
			flags |= FLAG_NONEMPTY;
		if(namesym)
			ERROR("cannot open %s: %s", name, strerror(errno));
		else
			ERROR("cannot open "PATHFMT": %s", COLLPATH(stuff, name), strerror(errno));
		goto error;
	}

	flags = cmd(fdsrc, dsrc, fdsym, dsym, stuff, depth);
	if(0)
	{
	error:
		flags |= FLAG_ERROR;
	}

	if(fdsrc >= 0)
	{
		close(fdsrc);
		closedir(dsrc);
	}
	if(fdsym >= 0)
	{
		close(fdsym);
		closedir(dsym);
	}

skip:
	path_remove(stuff, off);

	return flags;
}

static int create_symlink(int fdsym, const char *name, struct asd *stuff)
{
	size_t off = stuff->path.len;
	int error = path_append(stuff, name) < 0
			|| symlinkat(stuff->path.buf, fdsym, name) < 0;
	path_remove(stuff, off);
	if(error)
	{
		ERROR("cannot create symlink "PATHFMT": %s", COLLPATH(stuff, name), strerror(errno));
		return FLAG_ERROR;
	}
	INFO("created symlink "PATHFMT, COLLPATH(stuff, name));
	return FLAG_NONEMPTY;
}

static int remove_dir(int fdsym, const char *name, struct asd *stuff)
{
	int flags = go_deeper(cmd_rm, -1, fdsym, name, stuff, -1, NULL);
	if(flags & FLAG_NONEMPTY)
		(void)KEEP_LINK_MSG(stuff, name);
	else if(unlinkat(fdsym, name, AT_REMOVEDIR) < 0)
	{
		if(errno != ENOENT)
		{
			ERROR("cannot unlink "PATHFMT": %s", COLLPATH(stuff, name), strerror(errno));
			flags |= FLAG_NONEMPTY;
		}
	}
	else
		INFO("removed "PATHFMT, COLLPATH(stuff, name));
	return flags;
}

static int cmd_add(int fdsrc, DIR *dsrc, int fdsym, DIR *dsym, struct asd *stuff, int depth)
{
	(void)dsym;
	int flags = 0;
	const struct dirent *ent;
	while(errno = 0, (ent = readdir(dsrc)))
	{
		const char *name = ent->d_name;
		if(name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
			continue;

		struct stat stdir;
		if(fstatat(fdsrc, name, &stdir, AT_SYMLINK_NOFOLLOW) < 0)
		{
			if(errno == ENOENT)
				continue;
			ERROR("cannot access "PATHFMT": %s", DIRPATH(stuff, name), strerror(errno));
			flags |= FLAG_ERROR;
			continue;
		}

		if(growing_readlinkat(fdsym, name, stuff) < 0)
		{
			int exists;
			struct stat stcoll;
			if(errno != EINVAL || fstatat(fdsym, name, &stcoll, AT_SYMLINK_NOFOLLOW) < 0)
			{
				if(errno != ENOENT)
				{
					ERROR("cannot access "PATHFMT": %s", COLLPATH(stuff, name),
							strerror(errno));
					flags |= FLAG_ERROR;
					continue;
				}
				exists = 0;
			}
			else if(S_ISDIR(stdir.st_mode) != S_ISDIR(stcoll.st_mode))
			{
				flags |= DIR_CONFLICT_ERROR(stuff, name, stdir, stcoll);
				continue;
			}
			else
				exists = 1;

			if(S_ISDIR(stdir.st_mode))
			{
				if(depth == 0)
					continue;
				if(!exists)
				{
					if(mkdirat(fdsym, name, 0777) < 0)
					{
						ERROR("cannot create directory "PATHFMT": %s",
								COLLPATH(stuff, name), strerror(errno));
						flags |= FLAG_ERROR;
						continue;
					}
					INFO("created directory "PATHFMT, COLLPATH(stuff, name));
				}
				flags |= go_deeper(cmd_add, fdsrc, fdsym, name, stuff, MAX(depth - 1, -1), NULL);
			}
			else if(exists)
			{
				// conflicting file exists
				ERROR(PATHFMT" is a %s", COLLPATH(stuff, name), filetype(stcoll.st_mode));
			}
			else
				flags |= create_symlink(fdsym, name, stuff);
		}
		else if(path_eq_link(stuff, name))
		{
			// symlink to the same file
			DEBUG(PATHFMT" already exists", COLLPATH(stuff, name));
		}
		else if(path_valid_link(stuff, name))
		{
			// symlink to another file
			WARN(PATHFMT" already links to %s", COLLPATH(stuff, name), stuff->link.buf);
			flags |= FLAG_WARN;
		}
		else
			flags |= INVALID_SYMLINK_ERROR(stuff, name);
	}
	if(errno)
	{
		ERROR("cannot read "PATHFMT": %s", DIRPATH(stuff, NULL), strerror(errno));
		flags |= FLAG_ERROR;
	}
	return flags;
}

static int cmd_rm(int fdsrc, DIR *dsrc, int fdsym, DIR *dsym, struct asd *stuff, int depth)
{
	(void)fdsrc, (void)dsrc, (void)depth;
	int flags = 0;
	const struct dirent *ent;
	while(errno = 0, (ent = readdir(dsym)))
	{
		const char *name = ent->d_name;
		if(name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
			continue;

		const char *erraction;
		if(growing_readlinkat(fdsym, name, stuff) < 0)
		{
			struct stat stcoll;
			if(errno != EINVAL || fstatat(fdsym, name, &stcoll, AT_SYMLINK_NOFOLLOW) < 0)
			{
				if(errno == ENOENT)
					continue;
				erraction = "access";
				goto error;
			}

			if(S_ISDIR(stcoll.st_mode))
			{
				flags |= remove_dir(fdsym, name, stuff);
				continue;
			}
			else
				(void)SKIP_NONLINK_MSG(stuff, name);
		}
		else if(path_eq_link(stuff, name))
		{
			if(unlinkat(fdsym, name, 0) < 0)
			{
				if(errno == ENOENT)
					continue;
				erraction = "unlink";
			error:
				ERROR("cannot %s "PATHFMT": %s", erraction,
						COLLPATH(stuff, name), strerror(errno));
				flags |= FLAG_ERROR | FLAG_NONEMPTY;
			}
			else
				INFO("removed "PATHFMT, COLLPATH(stuff, name));
			continue;
		}
		else if(path_valid_link(stuff, name))
			(void)KEEP_LINK_MSG(stuff, name);
		else
			flags |= INVALID_SYMLINK_ERROR(stuff, name);
		flags |= FLAG_NONEMPTY;
	}
	if(errno)
	{
		ERROR("cannot read "PATHFMT": %s", COLLPATH(stuff, NULL), strerror(errno));
		flags |= FLAG_ERROR | FLAG_NONEMPTY;
	}
	return flags;
}

static int cmd_refresh(int fdsrc, DIR *dsrc, int fdsym, DIR *dsym, struct asd *stuff, int depth)
{
	int flags = 0;
	const struct dirent *ent;

	// create links and directories that do not yet exist
	while(errno = 0, (ent = readdir(dsrc)))
	{
		const char *name = ent->d_name;
		if(name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
			continue;

		struct stat stcoll, stdir;
		if(fstatat(fdsym, name, &stcoll, AT_SYMLINK_NOFOLLOW) >= 0)
		{
			// will be dealt with when iterating over dsym
			continue;
		}
		else if(errno != ENOENT)
		{
			ERROR("cannot access "PATHFMT": %s", COLLPATH(stuff, name), strerror(errno));
			flags |= FLAG_ERROR | FLAG_NONEMPTY;
			continue;
		}

		if(fstatat(fdsrc, name, &stdir, AT_SYMLINK_NOFOLLOW) < 0)
		{
			if(errno != ENOENT)
			{
				ERROR("cannot access "PATHFMT": %s", DIRPATH(stuff, name), strerror(errno));
				flags |= FLAG_ERROR;
			}
			continue;
		}

		if(S_ISDIR(stdir.st_mode))
		{
			if(depth == 0)
				continue;
			if(mkdirat(fdsym, name, 0777) < 0)
			{
				ERROR("cannot create directory "PATHFMT": %s",
						COLLPATH(stuff, name), strerror(errno));
				flags |= FLAG_ERROR;
				continue;
			}
			INFO("created directory "PATHFMT, COLLPATH(stuff, name));
			flags |= go_deeper(cmd_add, fdsrc, fdsym, name, stuff, MAX(depth - 1, -1), NULL) | FLAG_NONEMPTY;
		}
		else
			flags |= create_symlink(fdsym, name, stuff);
	}
	if(errno)
	{
		ERROR("cannot read "PATHFMT": %s", DIRPATH(stuff, NULL), strerror(errno));
		flags |= FLAG_ERROR;
	}

	// clean up existing links and directories
	while(errno = 0, (ent = readdir(dsym)))
	{
		const char *name = ent->d_name;
		if(name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
			continue;

		int exists;
		struct stat stdir;
		if(fstatat(fdsrc, name, &stdir, AT_SYMLINK_NOFOLLOW) < 0)
		{
			if(errno == ENOENT)
				exists = 0;
			else
			{
				ERROR("cannot access "PATHFMT": %s", DIRPATH(stuff, name), strerror(errno));
				flags |= FLAG_ERROR;
				continue;
			}
		}
		else
			exists = 1;

		if(growing_readlinkat(fdsym, name, stuff) < 0)
		{
			struct stat stcoll;
			if(errno != EINVAL || fstatat(fdsym, name, &stcoll, AT_SYMLINK_NOFOLLOW) < 0)
			{
				if(errno != ENOENT)
				{
					ERROR("cannot access "PATHFMT": %s", COLLPATH(stuff, name), strerror(errno));
					flags |= FLAG_ERROR;
				}
			}
			else if(!exists)
			{
				if(S_ISDIR(stcoll.st_mode))
					flags |= remove_dir(fdsym, name, stuff);
				else
					flags |= SKIP_NONLINK_MSG(stuff, name);
			}
			else if(S_ISDIR(stdir.st_mode) != S_ISDIR(stcoll.st_mode))
				flags |= DIR_CONFLICT_ERROR(stuff, name, stdir, stcoll);
			else if(S_ISDIR(stcoll.st_mode))
				flags |= go_deeper(cmd_refresh, fdsrc, fdsym, name, stuff, -1, NULL);
			else
			{
				// conflicting file exists
				ERROR(PATHFMT" is a %s", COLLPATH(stuff, name), filetype(stcoll.st_mode));
				flags |= FLAG_NONEMPTY;
			}
		}
		else if(path_eq_link(stuff, name))
		{
			// symlink to the same file
			if(exists)
				flags |= KEEP_LINK_MSG(stuff, name);
			else if(unlinkat(fdsym, name, 0) < 0)
			{
				if(errno == ENOENT)
					continue;
				ERROR("cannot unlink "PATHFMT": %s", COLLPATH(stuff, name), strerror(errno));
				flags |= FLAG_ERROR | FLAG_NONEMPTY;
			}
			else
				INFO("removed "PATHFMT, COLLPATH(stuff, name));
		}
		else if(!path_valid_link(stuff, name))
			flags |= INVALID_SYMLINK_ERROR(stuff, name);
	}
	if(errno)
	{
		ERROR("cannot read "PATHFMT": %s", COLLPATH(stuff, NULL), strerror(errno));
		flags |= FLAG_ERROR | FLAG_NONEMPTY;
	}

	return flags;
}

int main(int argc, char **argv)
{
	static const struct option globalopts[] = {
		{"collection", required_argument, NULL, 'c'},
		{"help",       no_argument,       NULL, 'h'},
		{"verbose",    no_argument,       NULL, 'v'},
		{NULL, 0, NULL, 0}
	};
	static const char globaloptstr[] = "hv";

	static const struct option addopts[] = {
		{"collection", required_argument, NULL, 'c'},
		{"depth",      no_argument,       NULL, 'd'},
		{"help",       no_argument,       NULL, 'h'},
		{"verbose",    no_argument,       NULL, 'v'},
		{NULL, 0, NULL, 0}
	};
	static const char addoptstr[] = "d:hv";

	argv0 = argv[0];
	command_func cmd;
	const char  *cmdstr;
	const char  *coll = NULL;
	int          opt;
	int          depth = -1;

	int resetenv = !getenv("POSIXLY_CORRECT");
	if(resetenv && setenv("POSIXLY_CORRECT", "", 0) < 0)
	{
		ERROR("%s", strerror(errno));
		return 1;
	}

	while((opt = getopt_long(argc, argv, globaloptstr, globalopts, NULL)) != -1)
	{
		switch(opt)
		{
		case 'h':
			printf("usage: %s [-h | --help] [-v | --verbose]... [--collection=<path>]\n"
					"              <command> [<option>]... <dir>\n"
					"Manage a directory full of symlinks. command must be one of add, refresh, and remove.\n"
					"\n"
					"Mandatory arguments to long optionas are mandatory for short options too.\n"
					"      --collection=<path>    s\n"
					"  -v, --verbose              increase verbosity\n"
					"  -h, --help                 display this help and exit\n",
					argv0);
			return 0;
		case 'c':
			coll = optarg;
			break;
		case 'v':
			verbosity++;
			continue;
		default:
			return 2;
		}

		// TODO guess last arg and remove POSIXLY_CORRECT environment shenanigans
	}

	if(resetenv)
		unsetenv("POSIXLY_CORRECT");

	const struct option *cmdopts;
	const char *cmdoptstr;
	if(optind == argc)
	{
		ERROR("no command given");
		return 2;
	}
	else if(strcmp(argv[optind], "refresh") == 0)
	{
		cmd     = cmd_refresh, cmdstr    = "refresh";
		cmdopts = addopts,     cmdoptstr = addoptstr;
	}
	else if(strcmp(argv[optind], "add") == 0)
	{
		cmd     = cmd_add, cmdstr    = "add";
		cmdopts = addopts, cmdoptstr = addoptstr;
	}
	else if(strcmp(argv[optind], "rm") == 0 || strcmp(argv[optind], "remove") == 0)
	{
		cmd     = cmd_rm,     cmdstr    = "remove";
		cmdopts = globalopts, cmdoptstr = globaloptstr;
	}
	else
	{
		ERROR("unknown command: %s", argv[optind]);
		return 2;
	}
	optind++;

	unsigned long ldepth;
	char *end;
	while((opt = getopt_long(argc, argv, cmdoptstr, cmdopts, NULL)) != -1)
		switch(opt)
		{
		case 'h':
			printf("usage: %s %s [-h | --help] [-v | --verbose]... [--collection=<path>]\n"
					"              <command> [<option>]... <dir>\n"
					"%s\n"
					"\n"
					"Mandatory arguments to long optionas are mandatory for short options too.\n"
					"      --collection=<path>    TODO description\n"
					"%s"
					"  -v, --verbose              increase verbosity\n"
					"  -h, --help                 display this help and exit\n",
					argv0, cmdstr,
					"TODO description",
					cmdopts == addopts ? "  -d, --depth=<depth>        set recursion depth, unlimited by default\n" : "");
			return 0;
		case 'c':
			coll = optarg;
			break;
		case 'd':
			ldepth = strtoul(optarg, &end, 0);
			if(ldepth > INT_MAX || *end)
			{
				ERROR("cannot parse depth %s: %s", optarg, strerror(*end ? EINVAL : ERANGE));
				return 2;
			}
			depth = ldepth;
			break;
		case 'v':
			verbosity++;
			break;
		default:
			return 2;
		}

	if(optind + 1 != argc)
	{
		ERROR("%s", optind == argc ? "no directory given" : "unexpected trailing arguments");
		return 2;
	}

	int error = 0;
	struct asd stuff = {
		.coll = coll
	};

	if(prepare_dir_path(&stuff, argv[optind]) < 0)
	{
		ERROR("%s", strerror(errno));
		goto error;
	}

	INFO("%s %s %s %s", cmdstr, stuff.path.buf,
			cmd == cmd_refresh ? "in" :
			cmd == cmd_add     ? "to" :
			"from",
			coll ? coll : ".");

	if(go_deeper(cmd, cmd == cmd_rm ? -1 : AT_FDCWD, AT_FDCWD,
			stuff.path.buf, &stuff, depth, coll ? coll : ".") & (FLAG_ERROR | FLAG_WARN))
	{
	error:
		error = 1;
	}

	free(stuff.path.buf);
	free(stuff.link.buf);

	return error;
}
