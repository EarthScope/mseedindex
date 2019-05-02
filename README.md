# mseedindex - Synchronize miniSEED with database

This program reads miniSEED files, creates an index of the available
data and stores this information into a database.  The index includes
details such as identifers, time ranges, file names, location within files,
and additional details.  The database can be either
[PostgreSQL](https://www.postgres.org) or 
[SQLite](https://www.sqlite.org/).

## Documentation

The [Wiki](https://github.com/iris-edu/mseedindex/wiki) provides an
overview and documentation of the database schema.

For program usage see the [mseedindex manual](doc/mseedindex.md)
in the 'doc' directory.

## Download release versions

The [releases](https://github.com/iris-edu/mseedindex/releases) area
contains release versions.

## Building and Installing

In most environments a simple 'make' will build the program.  The build
system is designed for GNU make, which make be avilable as 'gmake'.

The CC, CFLAGS and LDFLAGS environment variables can be used to configure
the build parameters.

For example, if Postgres is installed in non-system locations:
* CFLAGS='-I/Library/PostgreSQL/9.5/include'
* LDFLAGS='-L/Library/PostgreSQL/9.5/lib/'

To build without PostgreSQL support set the variable WITHOUTPOSTGRESQL.
This can be done in a single command with make like:
$ WITHOUTPOSTGRESQL=1 make

For further installation simply copy the resulting binary and man page
(in the 'doc' directory) to appropriate system directories.

In the Win32 environment the Makefile.win can be used with the nmake
build tool included with Visual Studio.  PostgreSQL support is turned
off by default in the Windows build procedure.

## Licensing 

Copyright (C) 2016 Chad Trabant, IRIS Data Management Center

This library is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as
published by the Free Software Foundation; either version 3 of the
License, or (at your option) any later version.

This library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License (GNU-LGPL) for more details.

You should have received a copy of the GNU Lesser General Public
License along with this software.
If not, see <https://www.gnu.org/licenses/>.

