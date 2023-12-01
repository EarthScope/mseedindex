# <p >Synchronize miniSEED summary and index with database</p>

1. [Name](#)
1. [Synopsis](#synopsis)
1. [Description](#description)
1. [File Versioning](#file-versioning)
1. [Data Section Update Time](#data-section-update-time)
1. [Options](#options)
1. [Input List File](#input-list-file)
1. [Leap Second List File](#leap-second-list-file)
1. [Author](#author)

## <a id='synopsis'>Synopsis</a>

<pre >
mseedindex [options] file1 [file2 file3 ...]
</pre>

## <a id='description'>Description</a>

<p ><b>mseedindex</b> reads miniSEED, determines contiguous data sections of time series (same network, station, location, channel and version) and optionally synchronizes information about each data section with a database and/or generates a JSON-formatted file of the index information.</p>

<p >The location of a given section is represented in the database as starting at a byte offset in a file and a count of bytes that follow. Each section of data is a row in the schema.</p>

<p >Data files should be scanned or synchronized when they are in-place. By default the absolute path to each input file will be resolved and stored.</p>

<p >If '-' is specified as an input file, records will be read from standard input.  Some other software is then responsible for tracking the location (path) of the data.  This can be used with the <b>-json</b> output option, where the index information is externally processed and the real path can be ignored or inserted.</p>

<p >Any existing rows in the database that match the file being synchronized will be replaced during synchronization, assuming the original and replacement files contain data that is within 1 day of overlapping.  This operation is done as a database transaction containing all deletions and all insertions.  See <b>FILE VERSIONING</b> for a description of how to avoid race conditions while simultaneously updating data files and extracting data.</p>

<p >PostgreSQL (version >= 9.1) and SQLite3 are supported as target databases.  When using Postgres the specified table is expected to exist.  When using SQLite both the database file and table will be created as needed, along with some indexes on common fields.</p>

## <a id='file-versioning'>File Versioning</a>

<p >During synchronization, all existing rows associated with the file being scanned will be deleted and new rows representing the current file will be inserted, effectively replacing all rows for a given file.</p>

<p >This situation leads to a timing issue if data file replacement/synchronization and data extraction are occurring at the same time.  To avoid this race condition <b>mseedindex</b> supports the following:</p>

<p >1) all deletions of existing rows and insertions of rows for a given file are performed in a transaction, meaning that the operation is atomic and no database reader can see a mix of rows from both files.</p>

<p >2) <b>mseedindex</b> recognizes file versions as numbers appended to a file name following a <b>#</b> character, with this pattern:</p>

<pre >
/path/to/data.file#VERSION
</pre>

<p >If a file name contains this version information <b>mseedindex</b> will search for existing rows in the database that match everything but the version, e.g. <b>filename like /path/to/data.file%</b> in SQL. The original and replacement file must contain data that is within 1 day of overlapping.</p>

<p >These features combined mean that a data file can be "replaced" by creating a new version of the file and scanning it without interrupting concurrent data extraction.  After replacement, another process can later remove the older version of the file.</p>

<p >One consequence is that no file names should contain a <b>#</b> character other than to designate a file version.</p>

## <a id='data-section-update-time'>Data Section Update Time</a>

<p >The schema contains fields for storing a hash (MD5) and update time of the data records containing a given data section.  When updating existing rows the <b>updated</b> field will only be changed when the hash does not match the existing row.  This facilitates tracking of time series updates even when data files are replaced but contain the same segments.</p>

<p >The time that the row was last updated is tracked independently in the <b>scanned</b> field.</p>

## <a id='options'>Options</a>

<b>-V</b>

<p style="padding-left: 30px;">Print program version and exit.</p>

<b>-h</b>

<p style="padding-left: 30px;">Print program usage and exit.</p>

<b>-v</b>

<p style="padding-left: 30px;">Be more verbose.  This flag can be used multiple times ("-v -v" or "-vv") for more verbosity.</p>

<b>-snd</b>

<p style="padding-left: 30px;">Skip non-miniSEED data.  Useful if indexing files that contain miniSEED but also otherdata, e.g. full SEED volumes.  Otherwise, the program will exit when un-recognized data are encountered.  If this option is used and data are skipped the calculated SHA-256 will not represent all data in the file.</p>

<b>-ns</b>

<p style="padding-left: 30px;">No synchronization.  Read miniSEED, determine data representation but do not synchronize the time series information with the database. Typically this would be used during diagnostics or testing and in combination the verbose option to print the time series information.</p>

<b>-noup</b>

<p style="padding-left: 30px;">No updates.  Do not search for and delete existing database entries. This is more efficient when indexing data for the first time and it is known that no entries exist for the specified file(s).  Misuse of this option can result in duplicate or inappropriate index entries.</p>

<b>-kp</b>

<p style="padding-left: 30px;">Keep the original file paths as specified.  By default the absolute, canonical path to each file is determined and stored in the database.</p>

<b>-tt </b><i>secs</i>

<p style="padding-left: 30px;">Specify a time tolerance for constructing continuous trace segments. The tolerance is specified in seconds.  The default tolerance is 1/2 of the sample period.</p>

<b>-rt </b><i>diff</i>

<p style="padding-left: 30px;">Specify a sample rate tolerance for constructing continuous trace segments. The tolerance is specified as the difference between two sampling rates.  The default tolerance is tested as: (abs(1-sr1/sr2) < 0.0001).</p>

<b>-si </b><i>secs</i>

<p style="padding-left: 30px;">Specify the sub-indexing interval in seconds, default is 3600 (1 hour). This parameter controls how often a time index is created with an otherwise contiguous data section.</p>

<b>-pghost </b><i>hostname</i>

<p style="padding-left: 30px;">Specify the Postgres database host name.</p>

<b>-sqlite </b><i>file</i>

<p style="padding-left: 30px;">Specify the SQLite3 database file name, e.g. 'timeseries.sqlite'.</p>

<b>-json </b><i>file</i>

<p style="padding-left: 30px;">Specify file to write JSON-formatted index information.</p>

<b>-table </b><i>tablename</i>

<p style="padding-left: 30px;">Specify the database table name, default value is 'tsindex'.</p>

<b>-dbport </b><i>port</i>

<p style="padding-left: 30px;">Specify the database host name, default: 5432.</p>

<b>-dbname </b><i>name</i>

<p style="padding-left: 30px;">Specify database name or full connection info, default: timeseries</p>

<b>-dbuser </b><i>username</i>

<p style="padding-left: 30px;">Specify the database user name, default: timeseries.</p>

<b>-dbpass </b><i>password</i>

<p style="padding-left: 30px;">Specify the database user password.  Database specific authentication mechanisms may be used, such as the Postgres password file.</p>

<b>-TRACE</b>

<p style="padding-left: 30px;">Enable Postgres libpq tracing facility and direct output to stderr.</p>

<b>-sqlitebusyto </b><i>milliseconds</i>

<p style="padding-left: 30px;">Set the SQLite busy timeout value in milliseconds, default is 10 seconds.  This is the amount of time to wait for a database lock and may need to be tuned in special scenarios where the database is particularly busy, such as highly concurrent usage.</p>

## <a id='input-list-file'>Input List File</a>

<p >A list file can be used to specify input files, one file per line. The initial '@' character indicating a list file is not considered part of the file name.  As an example, if the following command line option was used:</p>

<pre >
<b>@files.list</b>
</pre>

<p >The 'files.list' file might look like this:</p>

<pre >
data/day1.mseed
data/day2.mseed
data/day3.mseed
</pre>

## <a id='leap-second-list-file'>Leap Second List File</a>

<p >NOTE: A list of leap seconds is included in the program and no external list should be needed unless a leap second is added after year 2023.</p>

<p >If the environment variable LIBMSEED_LEAPSECOND_FILE is set it is expected to indicate a file containing a list of leap seconds in NTP format.  Some locations where this file can be obtained are indicated in RFC 8633 section 3.7: https://www.rfc-editor.org/rfc/rfc8633.html#section-3.7</p>

<p >If present, the leap seconds listed in this file will be used to adjust the time coverage for records that contain a leap second. Also, leap second indicators in the miniSEED headers will be ignored.</p>

## <a id='author'>Author</a>

<pre >
Chad Trabant
EarthScope Data Services
</pre>


(man page 2023/10/10)
