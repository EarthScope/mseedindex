# mseedindex - Generate miniSEED summary and synchronize database

This program reads miniSEED files, creates an index of the available
data and stores this information into a database.  Alternatively, the index
information can be produced in JSON format. The index includes details
such as identifers, time ranges, file names, location within files,
and additional details.  The database can be either
[PostgreSQL](https://www.postgres.org) or
[SQLite](https://www.sqlite.org/).

## Documentation

The [Wiki](https://github.com/EarthScope/mseedindex/wiki) provides an
overview and documentation of the database schema.

For program usage see the [mseedindex manual](doc/mseedindex.md)
in the 'doc' directory.

## Download release versions

The [releases](https://github.com/EarthScope/mseedindex/releases) area
contains release versions.

## Building and Installing

In most environments a simple 'make' will build the program.  The build
system is designed for GNU make, which make be avilable as 'gmake'.

The CC, CFLAGS and LDFLAGS environment variables can be used to configure
the build parameters.

To build _with_ PostgreSQL support set the variable `WITHPOSTGRESQL`.
This can be done in a single command with make like:
$ WITHPOSTGRESQL=1 make

If Postgres is installed in non-system locations, specific their location
using the `CFLAGS` and `LDFLAGS` environment variables:
* CFLAGS='-I/Library/PostgreSQL/14/include'
* LDFLAGS='-L/Library/PostgreSQL/14/lib/'

By default the build system will detect if necessary libraries are available
for URL support and enable this capability.  To build _without_ support for
reading URLs set the variable `WIHOUTURL`.  This can be done in a single
command with make like:
$ WITHOUTURL=1 make

For further installation simply copy the resulting binary and man page
(in the 'doc' directory) to appropriate system directories.

In the Win32 environment the Makefile.win can be used with the nmake
build tool included with Visual Studio.  PostgreSQL support is turned
off by default in the Windows build procedure.

## License

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

[http://www.apache.org/licenses/LICENSE-2.0](http://www.apache.org/licenses/LICENSE-2.0)

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Copyright (C) 2024 Chad Trabant, EarthScope Data Services
