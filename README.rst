symdir
######

###################################
manage a directory full of symlinks
###################################

:Author:         Schnusch
:Date:           2017-08-26
:Copyright:      MIT License
:Manual section: 1

SYNOPSIS
========

| **symdir** [-h | --help] [-v | --verbose]... [--collection=<path>] <command> [<option>]... <dir>

DESCRIPTION
===========

**symdir** mirrors the directory structures of it source directories and creates
symlinks pointing to the files in the source directories.

It can be used to access a collection of related files spanning multiple disks
in a single place.

GLOBAL OPTIONS
==============

**-h, --help**
	show usage information and help, then exit

**-v, --verbose**
	increase verbosity, if given at least once all files/directories that are
	created/removed are shown, if given more than once all files that
	were processed but left untouched are shown

**--collection=<path>**
	use collection *<path>* instead of *.*

COMMANDS
========

*command*
	**add**
		For every file in *dir* that does not yet exist in the collection create
		a symlink pointing to it. For every directory in *dir* if the recursion
		limit is not yet reached create that directory in the collection and
		repeat the process for this directory.

		*option*
			all `global options`_ are also accepted

			**-d, --depth=<depth>**
				set recursion depth limit, default unlimited

	**remove**, **rm**
		Remove all symlinks pointing to files in *dir* and empty directories
		from the collection.

		*option*
			all `global options`_ are accepted

	**refresh**
		Perform **add** for *dir* and remove all symlinks pointing to files in
		*dir* that no longer exist and empty directories from the collection.

		*option*
			all `global options`_ are also accepted

			**-d, --depth=<depth>**
				set recursion depth limit for **add**, default unlimited

BUILD
=====

| **make** `[CFLAGS=<cflags>]` `[LDFLAGS=<ldflags>]` `[RST2MAN=<rst2man>]` \
		`[INSTALL=<install>]` `[PREFIX=<prefix>]` `[DESTDIR=<destdir>]` <target>

*target*
	**all**

	**install**

	**build**

	**doc**
