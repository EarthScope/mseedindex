# <p markdown="1" >Synchronize Mini-SEED summary and index with database</p>

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

<p markdown="1" ><b>mseedindex</b> reads Mini-SEED, determines contigous data sections of time series (same network, station, location, channel and quality) and synchronizes information about each data section with a database.</p>

<p markdown="1" >The location of a given section is represented in the database as starting at a byte offset in a file and a count of bytes that follow. Each section of data is a row in the schema.</p>

<p markdown="1" >Data files should be scanned or synchornized when they are in-place. By default the absolute path to each input file will be resolved and stored.</p>

<p markdown="1" >Any existing rows in the database that match the file being synchronized will be replaced during synchronization.  This operation is done as a database transaction containing all deletions and all insertions.  See <b>FILE VERSIONING</b> for a description of how to avoid race conditions while simultaneously updating data files and extracting data.</p>

<p markdown="1" >PostgreSQL (version >= 9.1) and SQLite3 are supported as target databases.  When using Postgres the specified table is expected to exist.  When using SQLite both the database file and table will be created as needed, along with some indexes on common fields.</p>

<p markdown="1" >When an input file is full SEED including both SEED headers and data records all of the headers will be skipped.</p>

## <a id='file-versioning'>File Versioning</a>

<p markdown="1" >During synchronization, all existing rows associated with the file being scanned will be deleted and new rows representing the current file will be inserted, effectively replacing all rows for a given file.</p>

<p markdown="1" >This situation leads to a timing issue if data file replacement/synchronization and data extraction are occurring at the same time.  To avoid this race condition <b>mseedindex</b> supports the following:</p>

<p markdown="1" >1) all deletions of existing rows and insertions of rows for a given file are performed in a transaction, meaning that the operation is atomic and no database reader can see a mix of rows from both files.</p>

<p markdown="1" >2) <b>mseedindex</b> recognizes file versions as numbers appended to a file name following a <b>#</b> character, with this pattern:</p>

<pre >
/path/to/data.file#VERSION
</pre>

<p markdown="1" >If a file name contains this version information <b>mseedindex</b> will search for existing rows in the database that match everything but the version, e.g. <b>filename like /path/to/data.file%</b> in SQL.</p>

<p markdown="1" >These features combined mean that a data file can be "replaced" by creating a new version of the file and scanning it without ever interrupting concurrent data extraction.  After replacement, another process can later remove the older version of the file.</p>

<p markdown="1" >Once consequence is that no file names should contain a <b>#</b> character other than to designate a file version.</p>

## <a id='data-section-update-time'>Data Section Update Time</a>

<p markdown="1" >The schema contains fields for storing a hash (MD5) and update time of the data records containing a given data section.  When updating existing rows the <b>updated</b> field will only be changed when the hash does not match the existing row.  This facilitates tracking of time series updates even when data files are replaced but contain the same segments.</p>

<p markdown="1" >The time that the row was last updated is tracked independently in the <b>scanned</b> field.</p>

## <a id='options'>Options</a>

<b>-V</b>

<p markdown="1" style="padding-left: 30px;">Print program version and exit.</p>

<b>-h</b>

<p markdown="1" style="padding-left: 30px;">Print program usage and exit.</p>

<b>-v</b>

<p markdown="1" style="padding-left: 30px;">Be more verbose.  This flag can be used multiple times ("-v -v" or "-vv") for more verbosity.</p>

<b>-ns</b>

<p markdown="1" style="padding-left: 30px;">No synchronization.  Read miniSEED, determine data representation but do not synchronize the time series information with the database. Typically this would be used during diagnostics or testing and in combination the verbose option to print the time series information.</p>

<b>-table </b><i>tablename</i>

<p markdown="1" style="padding-left: 30px;">Specify the database table name, .e.g. 'tsindex'.  This option is required along with either <b>pghost</b> or <b>sqlite</b>.</p>

<b>-pghost </b><i>hostname</i>

<p markdown="1" style="padding-left: 30px;">Specify the Postgres database host name.  Either this option or <b>sqlite</b> must be specified.</p>

<b>-sqlite </b><i>file</i>

<p markdown="1" style="padding-left: 30px;">Specify the SQLite3 database file name, e.g. 'timeseries.sqlite'. Either this option or <b>pghost</b> must be specified.</p>

<b>-dbport </b><i>port</i>

<p markdown="1" style="padding-left: 30px;">Specify the database host name, default: 5432.</p>

<b>-dbuser </b><i>username</i>

<p markdown="1" style="padding-left: 30px;">Specify the database user name, default: timeseries.</p>

<b>-dbpass </b><i>password</i>

<p markdown="1" style="padding-left: 30px;">Specify the database user password.  Database specific authentication mechanisms may be used, such as the Postgres password file.</p>

<b>-tt </b><i>secs</i>

<p markdown="1" style="padding-left: 30px;">Specify a time tolerance for constructing continous trace segments. The tolerance is specified in seconds.  The default tolerance is 1/2 of the sample period.</p>

<b>-rt </b><i>diff</i>

<p markdown="1" style="padding-left: 30px;">Specify a sample rate tolerance for constructing continous trace segments. The tolerance is specified as the difference between two sampling rates.  The default tolerance is tested as: (abs(1-sr1/sr2) < 0.0001).</p>

<b>-kp</b>

<p markdown="1" style="padding-left: 30px;">Keep the original file paths as specified.  By default the absolute, canonical path to each file is determiend and stored in the database.</p>

## <a id='input-list-file'>Input List File</a>

<p markdown="1" >A list file can be used to specify input files, one file per line. The initial '@' character indicating a list file is not considered part of the file name.  As an example, if the following command line option was used:</p>

<pre >
<b>@files.list</b>
</pre>

<p markdown="1" >The 'files.list' file might look like this:</p>

<pre >
data/day1.mseed
data/day2.mseed
data/day3.mseed
</pre>

## <a id='leap-second-list-file'>Leap Second List File</a>

<p markdown="1" >If the environment variable LIBMSEED\_LEAPSECOND\_FILE is set it is expected to indicate a file containing a list of leap seconds as published by NIST and IETF, usually available here: http://www.ietf.org/timezones/data/leap-seconds.list</p>

<p markdown="1" >Specifying this file is highly recommended.</p>

<p markdown="1" >If present, the leap seconds listed in this file will be used to adjust the time coverage for records that contain a leap second. Also, leap second indicators in the miniSEED headers will be ignored.</p>

<p markdown="1" >To suppress the warning printed by <b>mseedindex</b> without specifying a leap second file, set LIBMSEED\_LEAPSECOND\_FILE=NONE.</p>

## <a id='author'>Author</a>

<pre >
Chad Trabant
IRIS Data Management Center
</pre>


(man page 2016/09/09)
