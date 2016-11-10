# mseedindex - Synchronize Mini-SEED with database

For usage information see the [mseedindex manual](doc/mseedindex.md)
in the 'doc' directory.

## Building and Installing

In most environments a simple 'make' will build the program.  The build
system is designed for GNU make, which make be avilable as 'gmake'.

The CC and CFLAGS environment variables can be used to configure
the build parameters.

For example, if Postgres is installed in non-system locations:
CFLAGS='-I/Library/PostgreSQL/9.5/include -L/Library/PostgreSQL/9.5/lib/'

To build without PostgreSQL support set the variable WITHOUTPOSTGRESQL.
This can be done in a single command with make like:
$ WITHOUTPOSTGRESQL=1 make

For further installation simply copy the resulting binary and man page
(in the 'doc' directory) to appropriate system directories.

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

