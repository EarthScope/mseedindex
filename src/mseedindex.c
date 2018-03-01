/***************************************************************************
 * mseedindex.c - Synchronize Mini-SEED with database schema
 *
 * Opens user specified file(s), parses the Mini-SEED records and
 * synchronizes a time series summary with a database.
 *
 * PostgreSQL and SQLite3 are supported as target databases.  When
 * using Postgres the target table is expected to exist.  When using
 * SQLite both the database file and table will be created as needed,
 * along with some indexes on common fields.
 *
 * The time series are grouped by NSLCQ records that are conterminous
 * in a given file, referred to as a section.  Each section will be
 * represented as a single row in the database and includes the
 * earliest and latest time, an index of time->byteoffset and a list
 * of time spans covered by the data and other details.
 *
 * The time index is created such that increasing time markers are
 * always at increasing byte offsets.  This is designed to allow easy
 * determination of a file read range for a certain time range.  For
 * the index to represent all data in a conterminous section, the
 * earliest data time must be first in the section.  If the earliest
 * data is not first (out-of-order data where the earliest is not
 * first), the index will set to NULL indicating that the entire
 * section must be read to cover the entire time range.  As long as
 * the earliest data is first the index is still appropriate for
 * out-of-order data.
 *
 * Expected Postgres database table schema:
 *
 * Field Name   Type
 * ------------ ------------
 * id           bigserial       -- Auto-incrementing
 * network      character text
 * station      character text
 * location     character text
 * channel      character text
 * quality      character text
 * version      smallint,
 * starttime    timestamp with time zone -- Earliest sample time
 * endtime      timestamp with time zone -- Latest sample time
 * samplerate   numeric
 * filename     character text
 * byteoffset   bigint
 * bytes        bigint
 * hash         character text
 * timeindex    hstore          -- List of time=>offset pairs using epoch times
 * timespans    numrange[]      -- Array of numrange values containing epoch times
 * timerates    numeric[]       -- Array of sample rates corresponding to timespans
 * format       character text  -- NULL means miniSEED
 * filemodtime  timestamp with time zone
 * updated      timestamp with time zone
 * scanned      timestamp with time zone
 *
 * Expected SQLite database table schema, created if not existing:
 *
 * Field Name   Type
 * ------------ ------------
 * network      TEXT
 * station      TEXT
 * location     TEXT
 * channel      TEXT
 * quality      TEXT
 * version      INTEGER
 * starttime    TEXT   -- Date-time in format YYYY-MM-DDTHH:MM:SS.ssssss
 * endtime      TEXT   -- Date-time in format YYYY-MM-DDTHH:MM:SS.ssssss
 * samplerate   REAL
 * filename     TEXT
 * byteoffset   INTEGER
 * bytes        INTEGER
 * hash         TEXT
 * timeindex    TEXT   -- List of time=>offset pairs using epoch times
 * timespans    TEXT   -- List of intervals using epoch time values
 * timerates    TEXT   -- List of sample rates corresponding to timespans
 * format       TEXT   -- NULL means miniSEED
 * filemodtime  TEXT   -- Date-time in format YYYY-MM-DDTHH:MM:SS
 * updated      TEXT   -- Date-time in format YYYY-MM-DDTHH:MM:SS
 * scanned      TEXT   -- Date-time in format YYYY-MM-DDTHH:MM:SS
 *
 * In general critical error messages are prefixed with "ERROR:" and
 * the return code will be 1.  On successfull operation the return
 * code will be 0.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 *
 * modified 2018.059
 ***************************************************************************/

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef WITHOUTPOSTGRESQL
#include <libpq-fe.h>
#endif

#include <sqlite3.h>

#include <libmseed.h>

#include "md5.h"

#define VERSION "2.7"
#define PACKAGE "mseedindex"

static flag verbose = 0;
static double timetol = -1.0;     /* Time tolerance for continuous traces */
static double sampratetol = -1.0; /* Sample rate tolerance for continuous traces */
static char keeppath = 0;         /* Use originally specified path, do not resolve absolute */
static flag nosync = 0;           /* Control synchronization with database, 1 = no database */
static flag noupdate = 0;         /* Control replacement of rows in database, 1 = no updating */

static char *table = "tsindex";
static char *pghost = 0;
static char *sqlitefile = 0;
static unsigned long int sqlitebusyto = 10000;

static char *dbport = "5432";
static char *dbname = "timeseries";
static char *dbuser = "timeseries";
static char *dbpass = 0;

static flag dbconntrace = 0; /* Trace database interactions, for debugging */

struct timeindex
{
  hptime_t time;
  int64_t byteoffset;
  struct timeindex *next;
};

struct sectiondetails
{
  int64_t startoffset;
  int64_t endoffset;
  hptime_t earliest;
  hptime_t latest;
  time_t updated;
  md5_state_t digeststate;
  char digeststr[33];
  int timeorderrecords;
  struct timeindex *tindex;
  MSTraceList *spans;
};

struct filelink
{
  char *filename;
  time_t filemodtime;
  time_t scantime;
  MSTraceGroup *mstg;
  struct filelink *next;
};

struct filelink *filelist = 0;
struct filelink *filelisttail = 0;

struct timeindex *AddTimeIndex (struct timeindex **tindex, hptime_t time, int64_t byteoffset);
#ifndef WITHOUTPOSTGRESQL
static int SyncPostgres (void);
static int SyncPostgresFileSeries (PGconn *dbconn, struct filelink *flp);
static PGresult *PQuery (PGconn *pgdb, const char *format, ...);
#endif
static int SyncSQLite (void);
static int SyncSQLiteFileSeries (sqlite3 *dbconn, struct filelink *flp);
static int SQLiteExec (sqlite3 *dbconn, int (*callback) (void *, int, char **, char **),
                       void *callbackdata, char **errmsg, const char *format, ...);
static int SQLitePrepare (sqlite3 *dbconn, sqlite3_stmt **statement, const char *format, ...);
void Local_mst_printtracelist (MSTraceGroup *mstg, flag timeformat);
static int ProcessParam (int argcount, char **argvec);
static char *GetOptValue (int argcount, char **argvec, int argopt);
static int AddFile (char *filename);
static int AddListFile (char *filename);
static int ResolveFilePaths (void);
int AddToString (char **string, char *add, char *delim, int where, int maxlen);
static void Usage (void);

int
main (int argc, char **argv)
{
  struct sectiondetails *sd = NULL;
  struct filelink *flp = NULL;
  MSRecord *msr = NULL;
  MSTrace *mst = NULL;
  MSTrace *cmst = NULL;
  hptime_t endtime = HPTERROR;
  hptime_t nextindex = HPTERROR;
  hptime_t prevstarttime = HPTERROR;
  char *leapsecondfile = NULL;
  int retcode = MS_NOERROR;
  struct stat st;

  off_t filepos = 0;
  off_t prevfilepos = 0;

  /* Set default error message prefix */
  ms_loginit (NULL, NULL, NULL, "ERROR: ");

  /* Process given parameters (command line and parameter file) */
  if (ProcessParam (argc, argv) < 0)
    return 1;

  /* Resolve absolute file paths if not keeping original paths */
  if (!keeppath)
    if (ResolveFilePaths ())
      return 1;

  /* Read leap second list file if env. var. LIBMSEED_LEAPSECOND_FILE is set */
  if ((leapsecondfile = getenv ("LIBMSEED_LEAPSECOND_FILE")))
  {
    if (strcmp (leapsecondfile, "NONE"))
      ms_readleapsecondfile (leapsecondfile);
  }
  else
  {
    ms_log (1, "Warning: No leap second file specified with LIBMSEED_LEAPSECOND_FILE\n");
    ms_log (1, "  This is highly recommended, see man page for details.\n");
  }

  /* Read files and accumulate indexing details */
  flp = filelist;
  while (flp)
  {
    if (verbose >= 1)
      ms_log (1, "Processing: %s\n", flp->filename);

    if ((flp->mstg = mst_initgroup (flp->mstg)) == NULL)
    {
      ms_log (2, "Could not allocate MSTraceGroup, out of memory?\n");
      exit (1);
    }

    if (stat (flp->filename, &st))
    {
      ms_log (2, "Could not stat %s: %s\n", flp->filename, strerror (errno));
      exit (1);
    }

    flp->filemodtime = st.st_mtime;
    flp->scantime = time (NULL);
    cmst = NULL;
    prevfilepos = 0;
    prevstarttime = HPTERROR;

    /* Read records from the input file */
    while ((retcode = ms_readmsr (&msr, flp->filename, -1, &filepos,
                                  NULL, 1, 0, verbose - 2)) == MS_NOERROR)
    {
      mst = NULL;
      endtime = msr_endtime (msr);

      /* Test if this record matches the current MSTrace */
      if (cmst)
      {
        mst = mst_findmatch (cmst, msr->dataquality, msr->network, msr->station,
                             msr->location, msr->channel);
      }

      /* Update details of current MSTrace if current record matches and is next in the file */
      if (mst == cmst && filepos == (prevfilepos + msr->reclen))
      {
        if (msr->samplecnt > 0)
          mst_addmsr (cmst, msr, 1);

        sd = (struct sectiondetails *)cmst->prvtptr;
        sd->endoffset = filepos + msr->reclen - 1;

        /* Maintain earliest and latest time stamps */
        if (msr->starttime < sd->earliest)
          sd->earliest = msr->starttime;
        if (endtime > sd->latest)
          sd->latest = endtime;

        /* Unset time order record indicator if not in time order */
        if (msr->starttime <= prevstarttime)
          sd->timeorderrecords = 0;

        /* Add time index if record crosses over the next index time and set next index for 1 hour later.
         * The time index will always be increasing in both time and offset. */
        if (endtime > nextindex)
        {
          if (AddTimeIndex (&sd->tindex, msr->starttime, filepos) == NULL)
          {
            exit (1);
          }

          while (nextindex < endtime)
            nextindex += MS_EPOCH2HPTIME (3600);
        }

        /* Add coverage to span list if sample rate is non-zero */
        if (msr->samprate)
        {
          if (!mstl_addmsr (sd->spans, msr, 1, 1, timetol, sampratetol))
          {
            ms_log (2, "Could not add MSRecord to span list, out of memory?\n");
            exit (1);
          }
        }

        md5_append (&(sd->digeststate), (const md5_byte_t *)msr->record, msr->reclen);
      }
      /* Otherwise create a new MSTrace */
      else
      {
        /* Create & populate new current MSTrace and add it to the file MSTraceGroup */
        cmst = mst_init (NULL);
        mst_addtracetogroup (flp->mstg, cmst);

        strncpy (cmst->network, msr->network, sizeof (cmst->network));
        strncpy (cmst->station, msr->station, sizeof (cmst->station));
        strncpy (cmst->location, msr->location, sizeof (cmst->location));
        strncpy (cmst->channel, msr->channel, sizeof (cmst->channel));
        cmst->dataquality = msr->dataquality;
        cmst->starttime = msr->starttime;
        cmst->endtime = endtime;
        cmst->samprate = msr->samprate;
        cmst->samplecnt = msr->samplecnt;

        if (!(sd = calloc (1, sizeof (struct sectiondetails))))
        {
          ms_log (2, "Cannot allocate section details\n");
          return 1;
        }

        cmst->prvtptr = sd;

        sd->startoffset = filepos;
        sd->endoffset = filepos + msr->reclen - 1;
        sd->earliest = msr->starttime;
        sd->latest = endtime;
        sd->updated = flp->filemodtime;
        sd->timeorderrecords = 1; /* By default records are assumed to be in order */

        /* Initialize time index with first entry and set next index for 1 hour later */
        if (AddTimeIndex (&sd->tindex, msr->starttime, filepos) == NULL)
        {
          ms_log (2, "Could not add first time index entry with AddTimeIndex, out of memory?\n");
          exit (1);
        }

        nextindex = cmst->starttime + MS_EPOCH2HPTIME (3600);
        while (nextindex < endtime)
          nextindex += MS_EPOCH2HPTIME (3600);

        /* Initialize MSTraceList time span list and populate */
        if ((sd->spans = mstl_init (NULL)) == NULL)
        {
          ms_log (2, "Could not allocate MSTraceList, out of memory?\n");
          exit (1);
        }

        /* Add coverage to span list if sample rate is non-zero */
        if (msr->samprate)
        {
          if (!mstl_addmsr (sd->spans, msr, 1, 1, timetol, sampratetol))
          {
            ms_log (2, "Could not add MSRecord to span list, out of memory?\n");
            exit (1);
          }
        }

        /* Initialize MD5 calculation state */
        memset (&(sd->digeststate), 0, sizeof (md5_state_t));
        md5_init (&(sd->digeststate));
        md5_append (&(sd->digeststate), (const md5_byte_t *)msr->record, msr->reclen);
      }

      prevfilepos = filepos;
      prevstarttime = msr->starttime;
    } /* Done reading records */

    /* Print error if not EOF */
    if (retcode != MS_ENDOFFILE)
    {
      ms_log (2, "Cannot read %s: %s\n", flp->filename, ms_errorstr (retcode));
      ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);
      exit (1);
    }

    /* Make sure everything is cleaned up */
    ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);

    /* Print sections for verbose output */
    if (verbose >= 2)
    {
      ms_log (1, "Section list to synchronize for %s\n", flp->filename);
      Local_mst_printtracelist (flp->mstg, 1);
    }

    flp = flp->next;
  } /* End of looping over file list for reading */

  /* Synchronize details with database */
  if (!nosync)
  {
#ifndef WITHOUTPOSTGRESQL
    if (pghost && SyncPostgres ())
    {
      ms_log (2, "Error synchronizing with Postgres\n");
      exit (1);
    }
#endif

    if (sqlitefile && SyncSQLite ())
    {
      ms_log (2, "Error synchronizing with SQLite\n");
      exit (1);
    }
  }

  return 0;
} /* End of main() */

/***************************************************************************
 * AddTimeIndex():
 *
 * Add the specified time and byte offset to the time index.
 *
 * Returns a pointer to the new timeindex on success and NULL on error.
 ***************************************************************************/
struct timeindex *
AddTimeIndex (struct timeindex **tindex, hptime_t time, int64_t byteoffset)
{
  struct timeindex *findex;
  struct timeindex *nindex;

  if (!tindex)
    return NULL;

  /* Allocate new index entry and populate */
  if (!(nindex = malloc (sizeof (struct timeindex))))
  {
    ms_log (2, "Cannot allocate time index entry, out of memory?\n");
    return NULL;
  }

  nindex->time = time;
  nindex->byteoffset = byteoffset;
  nindex->next = 0;

  /* If the time index is empty set the root pointer */
  if (!*tindex)
  {
    *tindex = nindex;
  }
  /* Otherwise add new index entry to end of chain */
  else
  {
    findex = *tindex;
    while (findex->next)
    {
      findex = findex->next;
    }

    findex->next = nindex;
  }

  return nindex;
} /* End of AddTimeIndex */

#ifndef WITHOUTPOSTGRESQL
/***************************************************************************
 * SyncPostgres():
 *
 * Synchronize with all file list entries with PostgresSQL.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
SyncPostgres (void)
{
  PGconn *dbconn = NULL; /* Database connection */
  PGresult *result = NULL;
  struct filelink *flp = NULL;
  const char *keywords[7];
  const char *values[7];

  /* Set up database connection parameters */
  keywords[0] = "host";
  values[0] = pghost;
  keywords[1] = "port";
  values[1] = dbport;
  keywords[2] = "dbname";
  values[2] = dbname;
  keywords[3] = "fallback_application_name";
  values[3] = PACKAGE;
  keywords[4] = "user";
  values[4] = dbuser;
  if (dbpass)
    keywords[5] = "password";
  else
    keywords[5] = NULL;
  values[5] = dbpass;
  keywords[6] = NULL;
  values[6] = NULL;

  dbconn = PQconnectdbParams (keywords, values, 1);

  if (!dbconn)
  {
    ms_log (2, "PQconnectdb returned NULL, connection failed");
    exit (1);
  }

  if (dbconntrace)
  {
    PQtrace (dbconn, stderr);
  }

  if (PQstatus (dbconn) != CONNECTION_OK)
  {
    ms_log (2, "Connection to database failed: %s", PQerrorMessage (dbconn));
    PQfinish (dbconn);
    exit (1);
  }

  if (verbose)
  {
    int sver = PQserverVersion (dbconn);
    int major, minor, less;
    major = sver / 10000;
    minor = sver / 100 - major * 100;
    less = sver - major * 10000 - minor * 100;

    ms_log (1, "Connected to database %s on host %s (server %d.%d.%d)\n",
            PQdb (dbconn), PQhost (dbconn),
            major, minor, less);
  }

  /* Set session timezone to 'UTC' */
  result = PQexec (dbconn, "SET SESSION timezone TO 'UTC'");
  if (PQresultStatus (result) != PGRES_COMMAND_OK)
  {
    ms_log (2, "Pg SET SESSION timezone failed: %s", PQerrorMessage (dbconn));
    PQclear (result);
    exit (1);
  }
  PQclear (result);

  if (verbose)
    ms_log (1, "Set database session timezone to UTC\n");

  /* Synchronize indexing details with database */
  flp = filelist;
  while (flp)
  {
    /* Sync time series listing */
    if (SyncPostgresFileSeries (dbconn, flp))
    {
      ms_log (2, "Error synchronizing time series for %s with Postgres\n", flp->filename);
      PQfinish (dbconn);
      return -1;
    }

    flp = flp->next;
  } /* End of looping over file list for synchronization */

  if (verbose >= 2)
    ms_log (1, "Closing database connection to %s\n", PQhost (dbconn));

  PQfinish (dbconn);

  return 0;
} /* End of SyncPostgres */

/***************************************************************************
 * SyncPostgresFileSeries():
 *
 * Synchronize the time series list associated with a file entry to
 * the database.
 *
 * Expected database schema is documented in the initial comment block.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
SyncPostgresFileSeries (PGconn *dbconn, struct filelink *flp)
{
  PGresult *result = NULL;
  PGresult *matchresult = NULL;
  int matchcount = 0;
  struct sectiondetails *sd;
  MSTrace *mst = NULL;
  int64_t bytecount;
  char earliest[64];
  char latest[64];
  char *filewhere = NULL;
  int baselength = 0;

  struct timeindex *tindex;
  char tmpstring[100];
  char *timeindexstr = NULL;
  char *timespansstr = NULL;
  char *timeratesstr = NULL;

  int rv;
  int idx;
  char *vp;
  char *ep = NULL;
  double version = -1.0;
  md5_byte_t digest[16];

  hptime_t fileearliest = HPTERROR;
  hptime_t filelatest = HPTERROR;

  if (!flp)
    return -1;

  if (verbose)
    ms_log (0, "Synchronizing sections for %s\n", flp->filename);

  /* Check and parse version from file */
  if ((vp = strrchr (flp->filename, '#')))
  {
    baselength = vp - flp->filename;

    version = strtod (++vp, &ep);

    if (!version && vp == ep)
    {
      ms_log (2, "Error parsing version from %s\n", flp->filename);
      return -1;
    }

    if (verbose >= 2)
      ms_log (1, "Parsed version %g from %s\n", version, flp->filename);
  }

  /* Loop through trace list, create MD5 digest strings and track file extents */
  mst = flp->mstg->traces;
  while (mst)
  {
    if ((sd = (struct sectiondetails *)mst->prvtptr))
    {
      /* Calculate MD5 digest and create string representation */
      md5_finish (&(sd->digeststate), digest);
      for (idx = 0; idx < 16; idx++)
        sprintf (sd->digeststr + (idx * 2), "%02x", digest[idx]);

      /* Determine earliest and latest times for the file */
      if (fileearliest == HPTERROR || fileearliest > sd->earliest)
        fileearliest = sd->earliest;
      if (filelatest == HPTERROR || filelatest < sd->latest)
        filelatest = sd->latest;
    }

    mst = mst->next;
  }

  if (fileearliest == HPTERROR || filelatest == HPTERROR)
  {
    ms_log (2, "No time extents found for %s\n", flp->filename);
    return -1;
  }

  /* Search database for rows matching the filename or a version of the filename */
  if (dbconn)
  {
    if (!noupdate)
    {
      /* Search for existing file entries, using a LIKE clause to search when matching versioned files.
         Include criteria to match an overlapping time range (+- 1 day) of extents, which can be used
         by the database to optimize the search, for example, by selecting only certain partitions. */
      if (baselength > 0)
        rv = asprintf (&filewhere,
                      "filename LIKE '%.*s%%'"
                      " AND starttime <= to_timestamp(%.6f) + interval '1 day'"
                      " AND endtime >= to_timestamp(%.6f) - interval '1 day'",
                       baselength, flp->filename,
                       (double)MS_HPTIME2EPOCH (filelatest),
                       (double)MS_HPTIME2EPOCH (fileearliest));
      else
        rv = asprintf (&filewhere,
                       "filename='%s'"
                       " AND starttime <= to_timestamp(%.6f) + interval '1 day'"
                       " AND endtime >= to_timestamp(%.6f) - interval '1 day'",
                       flp->filename,
                       (double)MS_HPTIME2EPOCH (filelatest),
                       (double)MS_HPTIME2EPOCH (fileearliest));

      if (rv <= 0 || !filewhere)
      {
        ms_log (2, "Cannot allocate memory for WHERE filename clause\n", flp->filename);
        return -1;
      }

      if (verbose >= 2)
        ms_log (1, "Searching for rows matching '%s'\n", flp->filename);

      matchresult = PQuery (dbconn,
                            "SELECT network,station,location,channel,quality,hash,extract (epoch from updated) "
                            "FROM %s "
                            "WHERE %s",
                            table, filewhere);

      if (PQresultStatus (matchresult) != PGRES_TUPLES_OK)
      {
        ms_log (2, "Pg SELECT failed: %s\n", PQresultErrorMessage (matchresult));
        PQclear (matchresult);
        if (filewhere)
          free (filewhere);
        return -1;
      }

      matchcount = PQntuples (matchresult);

      if (verbose >= 2)
        ms_log (1, "Found %d matching rows\n", matchcount);

      /* Retain previous updated value if hash is the same by searching
         for matching values (hash,NSLCQ) and storing previous update time. */
      if (matchcount > 0)
      {
        /* Fields: 0=network,1=station,2=location,3=channel,4=quality,5=hash,6=updated */
        for (idx = 0; idx < matchcount; idx++)
        {
          mst = flp->mstg->traces;
          while (mst)
          {
            if ((sd = (struct sectiondetails *)mst->prvtptr))
            {
              char *qp = PQgetvalue (matchresult, idx, 4);

              if (!strcmp (sd->digeststr, PQgetvalue (matchresult, idx, 5)))
                if (mst->dataquality == *qp)
                  if (!strcmp (mst->channel, PQgetvalue (matchresult, idx, 3)))
                    if (!strcmp (mst->location, PQgetvalue (matchresult, idx, 2)))
                      if (!strcmp (mst->station, PQgetvalue (matchresult, idx, 1)))
                        if (!strcmp (mst->network, PQgetvalue (matchresult, idx, 0)))
                        {
                          sd->updated = strtoll (PQgetvalue (matchresult, idx, 6), NULL, 10);
                        }
            }

            mst = mst->next;
          }
        }
      }

      PQclear (matchresult);
      matchresult = NULL;
    } /* if (noupdate) */

    /* Start a transaction block */
    result = PQexec (dbconn, "BEGIN TRANSACTION");
    if (PQresultStatus (result) != PGRES_COMMAND_OK)
    {
      ms_log (2, "Pg BEGIN TRANSACTION failed: %s", PQerrorMessage (dbconn));
      PQclear (result);
      if (filewhere)
        free (filewhere);
      return -1;
    }
    PQclear (result);

    /* Delete existing rows for filename or previous version of filename */
    if (matchcount > 0)
    {
      result = PQuery (dbconn, "DELETE FROM %s WHERE %s", table, filewhere);
      if (PQresultStatus (result) != PGRES_COMMAND_OK)
      {
        ms_log (2, "Pg DELETE failed: %s", PQerrorMessage (dbconn));
        PQclear (result);
        if (filewhere)
          free (filewhere);
        return -1;
      }
      PQclear (result);
    }

    free (filewhere);
  }

  /* Loop through trace list, synchronizing with database */
  mst = flp->mstg->traces;
  while (mst)
  {
    sd = (struct sectiondetails *)mst->prvtptr;

    bytecount = sd->endoffset - sd->startoffset + 1;

    /* Create earliest and latest epoch time strings, rounding to microseconds */
    snprintf (earliest, sizeof (earliest), "%.6f", (double)MS_HPTIME2EPOCH (sd->earliest));
    snprintf (latest, sizeof (latest), "%.6f", (double)MS_HPTIME2EPOCH (sd->latest));

    /* If time index includes the earliest data first create the time index key-value hstore:
     * 'time1=>offset1,time2=>offset2,time3=>offset3,...,latest=>[0|1]'
     * Otherwise set the index to NULL as it will not represent the entire time range. */
    tindex = sd->tindex;
    if (tindex && tindex->time == sd->earliest)
    {
      char *indexstr = NULL;

      while (tindex)
      {
        snprintf (tmpstring, sizeof (tmpstring), "\"%.6f\"=>\"%lld\"",
                  (double)MS_HPTIME2EPOCH (tindex->time),
                  (long long int)tindex->byteoffset);

        if (AddToString (&indexstr, tmpstring, ",", 0, 8388608))
        {
          fprintf (stderr, "ERROR: Time index has grown too large: %s\n", indexstr);
          return -1;
        }

        tindex = tindex->next;
      }

      /* Add 'latest' indicator to time index.  If the data section contains data
       * records only in progressing time order, then the index also identifies
       * offsets to the latest data. */
      snprintf (tmpstring, sizeof (tmpstring), "\"latest\"=>\"%d\"", sd->timeorderrecords);

      if (AddToString (&indexstr, tmpstring, ",", 0, 8388608))
      {
        fprintf (stderr, "ERROR: Time index has grown too large: %s\n", indexstr);
        return -1;
      }

      /* Add single quotes to make a string for the database */
      if (indexstr)
      {
        rv = asprintf (&timeindexstr, "'%s'", indexstr);

        if (rv <= 0 || !timeindexstr)
        {
          ms_log (2, "Cannot allocate memory for final time index string\n", flp->filename);
          return -1;
        }

        free (indexstr);
      }
    }

    /* Create the time spans and rates arrays for spans */
    if (sd->spans)
    {
      MSTraceID *id;
      MSTraceSeg *seg;
      char *spansstr = NULL;
      char *ratesstr = NULL;
      int ratemismatches = 0;

      if (sd->spans->traces && sd->spans->numtraces != 1)
      {
        ms_log (2, "Span list contains more than 1 trace ID for %s.%s.%s.%s, something is seriously wrong!\n",
                mst->network, mst->station, mst->location, mst->channel);
        return -1;
      }

      id = sd->spans->traces;
      while (id)
      {
        /* Create the time spans array:
           'numrange(start1,end1,'[]'),numrange(start2,end2,'[]'),numrange(start3,end3,'[]'),...' */
        seg = id->first;
        while (seg)
        {
          /* Create number range value entry in array, rounding epoch times to microseconds */
          snprintf (tmpstring, sizeof (tmpstring), "numrange(%.6f,%.6f,'[]')",
                    (double)MS_HPTIME2EPOCH (seg->starttime),
                    (double)MS_HPTIME2EPOCH (seg->endtime));

          if (AddToString (&spansstr, tmpstring, ",", 0, 8388608))
          {
            ms_log (2, "Time span list has grown too large: %s\n", spansstr);
            return -1;
          }

          /* Track if segment rate is in tolerance with trace/row rate */
          if (!MS_ISRATETOLERABLE (seg->samprate, mst->samprate))
          {
            ratemismatches += 1;
          }

          seg = seg->next;
        }

        /* Create the time rates array if there are rate mismatches:
           'rate1,rate2,rate3,...' */
        if (ratemismatches > 0)
        {
          seg = id->first;
          while (seg)
          {
            snprintf (tmpstring, sizeof (tmpstring), "%.6g", seg->samprate);

            if (AddToString (&ratesstr, tmpstring, ",", 0, 8388608))
            {
              ms_log (2, "Time rate list has grown too large: %s\n", ratesstr);
              return -1;
            }

            seg = seg->next;
          }
        }

        id = id->next;
      }

      /* Add Array declaration to timespans for the database */
      if (spansstr)
      {
        rv = asprintf (&timespansstr, "ARRAY[%s]", spansstr);

        if (rv <= 0 || !timespansstr)
        {
          ms_log (2, "Cannot allocate memory for final time spans string\n", flp->filename);
          return -1;
        }

        free (spansstr);
      }

      /* Add Array declaration to timerates for the database */
      if (ratesstr)
      {
        rv = asprintf (&timeratesstr, "ARRAY[%s]", ratesstr);

        if (rv <= 0 || !timeratesstr)
        {
          ms_log (2, "Cannot allocate memory for final time rates string\n", flp->filename);
          return -1;
        }

        free (ratesstr);
      }
    } /* End if (sd->spans) */

    if (dbconn)
    {
      /* Insert new row */
      result = PQuery (dbconn,
                       "INSERT INTO %s "
                       "(network,station,location,channel,quality,starttime,endtime,samplerate,"
                       "filename,byteoffset,bytes,hash,"
                       "timeindex,timespans,timerates,format,"
                       "filemodtime,updated,scanned) "
                       "VALUES "
                       "('%s','%s','%s','%s','%c',to_timestamp(%s),to_timestamp(%s),%.6g,"
                       "'%s',%lld,%lld,'%s',"
                       "%s,%s,%s,%s,"
                       "to_timestamp(%lld),to_timestamp(%lld),to_timestamp(%lld))",
                       table,
                       mst->network,
                       mst->station,
                       mst->location,
                       mst->channel,
                       mst->dataquality,
                       earliest, latest,
                       mst->samprate, flp->filename, sd->startoffset, bytecount, sd->digeststr,
                       (timeindexstr) ? timeindexstr : "NULL",
                       (timespansstr) ? timespansstr : "NULL",
                       (timeratesstr) ? timeratesstr : "NULL",
                       "NULL",
                       (long long int)flp->filemodtime, (long long int)sd->updated, (long long int)flp->scantime);

      if (PQresultStatus (result) != PGRES_COMMAND_OK)
      {
        ms_log (2, "Pg INSERT failed: %s\n", PQresultErrorMessage (result));
        PQclear (result);
        return -1;
      }
      PQclear (result);
    }

    /* Print trace line when verbose >=2 or when verbose and not sync'ing */
    if (verbose >= 2 || (verbose && nosync))
    {
      ms_log (0, "%s|%s|%s|%s|%c|%s|%s|%.10g|%s|%lld|%lld|%s|%lld|%lld\n",
              mst->network, mst->station, mst->location, mst->channel, mst->dataquality,
              earliest, latest, mst->samprate, flp->filename,
              sd->startoffset, bytecount, sd->digeststr,
              (long long int)sd->updated, (long long int)flp->scantime);
      printf (" TINDEX: '%s'\n", (timeindexstr)?timeindexstr:"");
      printf (" TSPANS: '%s'\n", (timespansstr)?timespansstr:"");
      printf (" TRATES: '%s'\n", (timeratesstr)?timeratesstr:"");
    }

    if (timeindexstr)
    {
      free (timeindexstr);
      timeindexstr = NULL;
    }
    if (timespansstr)
    {
      free (timespansstr);
      timespansstr = NULL;
    }
    if (timeratesstr)
    {
      free (timeratesstr);
      timeratesstr = NULL;
    }

    mst = mst->next;
  }

  /* End the transaction */
  if (dbconn)
  {
    result = PQexec (dbconn, "COMMIT");
    PQclear (result);
  }

  return 0;
} /* End of SyncPostgresFileSeries() */

/***************************************************************************
 * PQuery():
 *
 * Execute a query to the Postgres DB connection at 'pgdb'.
 *
 * Returns PGresult on success and NULL on failure
 ***************************************************************************/
static PGresult *
PQuery (PGconn *pgdb, const char *format, ...)
{
  va_list argv;
  char *query = NULL;
  PGresult *result = NULL;
  int length;

  va_start (argv, format);
  length = vasprintf (&query, format, argv);
  va_end (argv);

  if (!query || length <= 0)
  {
    ms_log (2, "Cannot create query string\n");
    return NULL;
  }

  if (verbose >= 2)
    fprintf (stderr, "QUERY(%d): '%s'\n", length, query);

  result = PQexec (pgdb, query);

  if (query)
    free (query);

  return result;
} /* End of PQuery() */
#endif

/***************************************************************************
 * SyncSQLite():
 *
 * Synchronize with all file list entries with SQLite.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
SyncSQLite (void)
{
  sqlite3 *dbconn = NULL;
  char *errmsg = NULL;
  struct filelink *flp = NULL;
  int rv;

  /* Open SQLite database, creating file if not existing */
  if (sqlite3_open (sqlitefile, &dbconn))
  {
    ms_log (2, "Cannot open SQLite database: %s\n", sqlite3_errmsg (dbconn));
    sqlite3_close (dbconn);
    return -1;
  }

  if (verbose)
  {
    ms_log (1, "Opened SQLite database file %s\n", sqlitefile);
  }

  /* Set timeout in milliseconds to wait for access to the database */
  if (sqlitebusyto)
  {
    if (sqlite3_busy_timeout(dbconn, sqlitebusyto))
    {
      ms_log (2, "Cannot set busy timeout on SQLite database: %s\n", sqlite3_errmsg (dbconn));
      sqlite3_close (dbconn);
      return -1;
    }

    if (verbose >= 2)
    {
      ms_log (1, "SQLite database busy timeout set to %lu\n", sqlitebusyto);
    }
  }

  /* Set LIKE operator to be case-sensitive as it should be with file names.
   * More importantly, this allows the index on the filename column to be used with our LIKEs. */
  rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                   "PRAGMA case_sensitive_like = ON",
                   table, table);
  if (rv != SQLITE_OK)
  {
    ms_log (2, "SQLite PRAGMA case_sensitive_like = ON failed: %s\n", (errmsg) ? errmsg : "");
    sqlite3_free (errmsg);
    return -1;
  }

  /* Create table if it does not exist */
  rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                   "CREATE TABLE IF NOT EXISTS %s "
                   "(network TEXT,"
                   "station TEXT,"
                   "location TEXT,"
                   "channel TEXT,"
                   "quality TEXT,"
                   "version INTEGER,"
                   "starttime TEXT,"
                   "endtime TEXT,"
                   "samplerate REAL,"
                   "filename TEXT,"
                   "byteoffset INTEGER,"
                   "bytes INTEGER,"
                   "hash TEXT,"
                   "timeindex TEXT,"
                   "timespans TEXT,"
                   "timerates TEXT,"
                   "format TEXT,"
                   "filemodtime TEXT,"
                   "updated TEXT,"
                   "scanned TEXT)",
                   table);
  if (rv != SQLITE_OK)
  {
    ms_log (2, "SQLite CREATE TABLE failed: %s\n", (errmsg) ? errmsg : "");
    sqlite3_free (errmsg);
    return -1;
  }

  /* Create index for (network,station,location,channel,starttime,endtime) */
  rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                   "CREATE INDEX IF NOT EXISTS %s_nslcse_idx ON %s "
                   "(network,station,location,channel,starttime,endtime)",
                   table, table);
  if (rv != SQLITE_OK)
  {
    ms_log (2, "SQLite CREATE INDEX failed: %s\n", (errmsg) ? errmsg : "");
    sqlite3_free (errmsg);
    return -1;
  }

  /* Create index for (filename) */
  rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                   "CREATE INDEX IF NOT EXISTS %s_filename_idx ON %s (filename)",
                   table, table);
  if (rv != SQLITE_OK)
  {
    ms_log (2, "SQLite CREATE INDEX failed: %s\n", (errmsg) ? errmsg : "");
    sqlite3_free (errmsg);
    return -1;
  }

  /* Create index for (updated) */
  rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                   "CREATE INDEX IF NOT EXISTS %s_updated_idx ON %s (updated)",
                   table, table);
  if (rv != SQLITE_OK)
  {
    ms_log (2, "SQLite CREATE INDEX failed: %s\n", (errmsg) ? errmsg : "");
    sqlite3_free (errmsg);
    return -1;
  }

  /* Synchronize indexing details with database */
  flp = filelist;
  while (flp)
  {
    /* Sync time series listing */
    if (SyncSQLiteFileSeries (dbconn, flp))
    {
      ms_log (2, "Error synchronizing time series for %s with SQLite\n", flp->filename);
      sqlite3_close (dbconn);
      return -1;
    }

    flp = flp->next;
  } /* End of looping over file list for synchronization */

  if (verbose >= 2)
    ms_log (1, "Closing SQLite database %s\n", sqlitefile);

  rv = sqlite3_close (dbconn);
  if (rv != SQLITE_OK)
  {
    ms_log (1, "Warning: closing SQLite database was not clean: %s\n", sqlite3_errstr (rv));
  }

  return 0;
} /* End of SyncSQLite */

/***************************************************************************
 * SyncSQLiteFileSeries():
 *
 * Synchronize the time series list associated with a file entry to
 * the database.
 *
 * Expected database schema is documented in the initial comment block.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
SyncSQLiteFileSeries (sqlite3 *dbconn, struct filelink *flp)
{
  sqlite3_stmt *statement = NULL;
  struct sectiondetails *sd;
  MSTrace *mst = NULL;
  int64_t bytecount;
  char earliest[64];
  char latest[64];
  char *filewhere = NULL;
  char *errmsg = NULL;
  int baselength = 0;
  int matchcount = 0;

  struct timeindex *tindex;
  char tmpstring[100];
  char *timeindexstr = NULL;
  char *timespansstr = NULL;
  char *timeratesstr = NULL;

  int rv;
  int idx;
  char *vp;
  char *ep = NULL;
  double version = -1.0;
  md5_byte_t digest[16];

  hptime_t fileearliest = HPTERROR;
  hptime_t filelatest = HPTERROR;

  if (!flp)
    return -1;

  if (verbose)
    ms_log (0, "Synchronizing sections for %s\n", flp->filename);

  /* Check and parse version from file */
  if ((vp = strrchr (flp->filename, '#')))
  {
    baselength = vp - flp->filename;

    version = strtod (++vp, &ep);

    if (!version && vp == ep)
    {
      ms_log (2, "Error parsing version from %s\n", flp->filename);
      return -1;
    }

    if (verbose >= 2)
      ms_log (1, "Parsed version %g from %s\n", version, flp->filename);
  }

  /* Loop through trace list, create MD5 digest strings and track file extents */
  mst = flp->mstg->traces;
  while (mst)
  {
    if ((sd = (struct sectiondetails *)mst->prvtptr))
    {
      /* Calculate MD5 digest and create string representation */
      md5_finish (&(sd->digeststate), digest);
      for (idx = 0; idx < 16; idx++)
        sprintf (sd->digeststr + (idx * 2), "%02x", digest[idx]);

      /* Determine earliest and latest times for the file */
      if (fileearliest == HPTERROR || fileearliest > sd->earliest)
        fileearliest = sd->earliest;
      if (filelatest == HPTERROR || filelatest < sd->latest)
        filelatest = sd->latest;
    }

    mst = mst->next;
  }

  if (fileearliest == HPTERROR || filelatest == HPTERROR)
  {
    ms_log (2, "No time extents found for %s\n", flp->filename);
    return -1;
  }

  /* Search database for rows matching the filename or a version of the filename */
  if (dbconn)
  {
    /* Create time strings for earliest and latest times for the file */
    if (!ms_hptime2isotimestr (fileearliest, earliest, 1) ||
        !ms_hptime2isotimestr (filelatest, latest, 1))
    {
      ms_log (2, "Cannot create earliest/latest time strings for %s\n", flp->filename);
      return -1;
    }

    if (!noupdate)
    {
      /* Search for existing file entries, using a LIKE clause to search when matching versioned files.
         Include criteria to match an overlapping time range (+- 1 day) of extents, which can be used
         by the database to optimize the search, for example, by selecting only certain partitions. */
      if (baselength > 0)
        rv = asprintf (&filewhere,
                       "filename LIKE '%.*s%%'"
                       " AND starttime <= datetime('%s', '+1 day')"
                       " AND endtime >= datetime('%s', '-1 day')",
                       baselength, flp->filename, latest, earliest);
      else
        rv = asprintf (&filewhere,
                       "filename='%s'"
                       " AND starttime <= datetime('%s', '+1 day')"
                       " AND endtime >= datetime('%s', '-1 day')",
                       flp->filename, latest, earliest);

      if (rv <= 0 || !filewhere)
      {
        ms_log (2, "Cannot allocate memory for WHERE filename clause\n", flp->filename);
        return -1;
      }

      if (verbose >= 2)
        ms_log (1, "Searching for rows matching '%s'\n", flp->filename);

      rv = SQLitePrepare (dbconn, &statement,
                          "SELECT network,station,location,channel,quality,hash,updated "
                          "FROM %s "
                          "WHERE %s",
                          table, filewhere);
      if (rv != SQLITE_OK)
      {
        ms_log (2, "SQLite SELECT preparation failed: %s\n", sqlite3_errstr (rv));
        if (filewhere)
          free (filewhere);
        return -1;
      }

      /* Retain previous updated value if hash is the same by searching
         for matching values (hash,NSLCQ) and storing previous update time. */
      matchcount = 0;
      while ((rv = sqlite3_step (statement)) == SQLITE_ROW)
      {
        matchcount++;

        /* Fields: 0=network,1=station,2=location,3=channel,4=quality,5=hash,6=updated */
        for (idx = 0; idx < matchcount; idx++)
        {
          mst = flp->mstg->traces;
          while (mst)
          {
            if ((sd = (struct sectiondetails *)mst->prvtptr))
            {
              const unsigned char *qp = sqlite3_column_text (statement, 4);
              hptime_t hpupdated;

              if (!strcmp (sd->digeststr, (char *)sqlite3_column_text (statement, 5)))
                if (mst->dataquality == *qp)
                  if (!strcmp (mst->channel, (char *)sqlite3_column_text (statement, 3)))
                    if (!strcmp (mst->location, (char *)sqlite3_column_text (statement, 2)))
                      if (!strcmp (mst->station, (char *)sqlite3_column_text (statement, 1)))
                        if (!strcmp (mst->network, (char *)sqlite3_column_text (statement, 0)))
                        {
                          hpupdated = ms_timestr2hptime ((char *)sqlite3_column_text (statement, 6));

                          if (hpupdated == HPTERROR)
                          {
                            ms_log (1, "Warning: could not convert 'updated' time value: '%s'\n",
                                    sqlite3_column_text (statement, 6));
                          }

                          /* Convert to time_t with simple rounding */
                          sd->updated = (double)MS_HPTIME2EPOCH (hpupdated) + 0.5;
                        }
            }

            mst = mst->next;
          }
        }
      }

      if (rv != SQLITE_DONE)
      {
        ms_log (2, "Cannot step through SQLite results: %s\n", sqlite3_errstr (rv));
        sqlite3_finalize (statement);
        if (filewhere)
          free (filewhere);
        return -1;
      }

      sqlite3_finalize (statement);

      if (verbose >= 2)
        ms_log (1, "Found %d matching rows\n", matchcount);
    } /* if (noupdate) */

    /* Start a transaction block */
    rv = SQLiteExec (dbconn, NULL, NULL, &errmsg, "BEGIN TRANSACTION");
    if (rv != SQLITE_OK)
    {
      ms_log (2, "SQLite BEGIN TRANSACTION failed: %s\n", (errmsg) ? errmsg : "");
      sqlite3_free (errmsg);
      if (filewhere)
        free (filewhere);
      return -1;
    }

    /* Delete existing rows for filename or previous version of filename */
    if (matchcount > 0)
    {
      rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                       "DELETE FROM %s WHERE %s", table, filewhere);
      if (rv != SQLITE_OK)
      {
        ms_log (2, "SQLite DELETE failed: %s\n", (errmsg) ? errmsg : "");
        sqlite3_free (errmsg);
        if (filewhere)
          free (filewhere);
        return -1;
      }
    }

    free (filewhere);
  }

  /* Loop through trace list, synchronizing with database */
  mst = flp->mstg->traces;
  while (mst)
  {
    sd = (struct sectiondetails *)mst->prvtptr;

    bytecount = sd->endoffset - sd->startoffset + 1;

    /* If time index includes the earliest data first create the time index key-value list:
     * 'time1=>offset1,time2=>offset2,time3=>offset3,...,latest=>[0|1]'
     * Otherwise set the index to NULL as it will not represent the entire time range. */
    tindex = sd->tindex;
    if (tindex && tindex->time == sd->earliest)
    {
      char *indexstr = NULL;

      while (tindex)
      {
        snprintf (tmpstring, sizeof (tmpstring), "%.6f=>%lld",
                  (double)MS_HPTIME2EPOCH (tindex->time),
                  (long long int)tindex->byteoffset);

        if (AddToString (&indexstr, tmpstring, ",", 0, 8388608))
        {
          fprintf (stderr, "ERROR: Time index has grown too large: %s\n", indexstr);
          return -1;
        }

        tindex = tindex->next;
      }

      /* Add 'latest' indicator to time index.  If the data section contains data
       * records only in progressing time order, then the index also identifies
       * offsets to the latest data. */
      snprintf (tmpstring, sizeof (tmpstring), "latest=>%d", sd->timeorderrecords);

      if (AddToString (&indexstr, tmpstring, ",", 0, 8388608))
      {
        fprintf (stderr, "ERROR: Time index has grown too large: %s\n", indexstr);
        return -1;
      }

      /* Add single quotes to make a string for the database */
      if (indexstr)
      {
        rv = asprintf (&timeindexstr, "'%s'", indexstr);

        if (rv <= 0 || !timeindexstr)
        {
          ms_log (2, "Cannot allocate memory for final time index string\n", flp->filename);
          return -1;
        }

        free (indexstr);
      }
    }

    /* Create the time spans and rates arrays for spans */
    if (sd->spans)
    {
      MSTraceID *id;
      MSTraceSeg *seg;
      char *spansstr = NULL;
      char *ratesstr = NULL;
      int ratemismatches = 0;

      if (sd->spans->traces && sd->spans->numtraces != 1)
      {
        ms_log (2, "Span list contains more than 1 trace ID for %s.%s.%s.%s, something is seriously wrong!\n",
                mst->network, mst->station, mst->location, mst->channel);
        return -1;
      }

      id = sd->spans->traces;
      while (id)
      {
        /* Create the time spans array:
         * '[start1:end1],[start2:end2],[start3:end3],...' */
        seg = id->first;
        while (seg)
        {
          /* Create number range value (in interval notation), rounding epoch times to microseconds */
          snprintf (tmpstring, sizeof (tmpstring), "[%.6f:%.6f]",
                    (double)MS_HPTIME2EPOCH (seg->starttime),
                    (double)MS_HPTIME2EPOCH (seg->endtime));

          if (AddToString (&spansstr, tmpstring, ",", 0, 8388608))
          {
            ms_log (2, "Time span list has grown too large: %s\n", spansstr);
            return -1;
          }

          /* Track if segment rate is in tolerance with trace/row rate */
          if (!MS_ISRATETOLERABLE (seg->samprate, mst->samprate))
          {
            ratemismatches += 1;
          }

          seg = seg->next;
        }

        /* Create the time rates array if there are rate mismatches:
           'rate1,rate2,rate3,...' */
        if (ratemismatches > 0)
        {
          seg = id->first;
          while (seg)
          {
            snprintf (tmpstring, sizeof (tmpstring), "%.6g", seg->samprate);

            if (AddToString (&ratesstr, tmpstring, ",", 0, 8388608))
            {
              ms_log (2, "Time rate list has grown too large: %s\n", ratesstr);
              return -1;
            }

            seg = seg->next;
          }
        }

        id = id->next;
      }

      /* Add single quotes to timespans to make a string for the database */
      if (spansstr)
      {
        rv = asprintf (&timespansstr, "'%s'", spansstr);

        if (rv <= 0 || !timespansstr)
        {
          ms_log (2, "Cannot allocate memory for final time spans string\n", flp->filename);
          return -1;
        }

        free (spansstr);
      }

      /* Add single quotes to timerates to make a string for the database */
      if (ratesstr)
      {
        rv = asprintf (&timeratesstr, "'%s'", ratesstr);

        if (rv <= 0 || !timeratesstr)
        {
          ms_log (2, "Cannot allocate memory for final time rates string\n", flp->filename);
          return -1;
        }

        free (ratesstr);
      }
    } /* End if (sd->spans) */

    if (dbconn)
    {
      char starttimestr[50];
      char endtimestr[50];
      char filemodtimestr[50];
      char updatedstr[50];
      char scannedstr[50];

      /* Create time strings for SQLite time fields */
      ms_hptime2isotimestr (sd->earliest, starttimestr, 1);
      ms_hptime2isotimestr (sd->latest, endtimestr, 1);
      ms_hptime2isotimestr (MS_EPOCH2HPTIME (flp->filemodtime), filemodtimestr, 0);
      ms_hptime2isotimestr (MS_EPOCH2HPTIME (sd->updated), updatedstr, 0);
      ms_hptime2isotimestr (MS_EPOCH2HPTIME (flp->scantime), scannedstr, 0);

      /* Insert new row */
      rv = SQLiteExec (dbconn, NULL, NULL, &errmsg,
                       "INSERT INTO %s "
                       "(network,station,location,channel,quality,starttime,endtime,samplerate,"
                       "filename,byteoffset,bytes,hash,"
                       "timeindex,timespans,timerates,format,"
                       "filemodtime,updated,scanned) "
                       "VALUES "
                       "('%s','%s','%s','%s','%c','%s','%s',%.6g,"
                       "'%s',%lld,%lld,'%s',"
                       "%s,%s,%s,%s,"
                       "'%s','%s','%s')",
                       table,
                       mst->network,
                       mst->station,
                       mst->location,
                       mst->channel,
                       mst->dataquality,
                       starttimestr, endtimestr,
                       mst->samprate, flp->filename, sd->startoffset, bytecount, sd->digeststr,
                       (timeindexstr) ? timeindexstr : "NULL",
                       (timespansstr) ? timespansstr : "NULL",
                       (timeratesstr) ? timeratesstr : "NULL",
                       "NULL",
                       filemodtimestr, updatedstr, scannedstr);
      if (rv != SQLITE_OK)
      {
        ms_log (2, "SQLite INSERT failed: %s\n", (errmsg) ? errmsg : "");
        sqlite3_free (errmsg);
        return -1;
      }
    }

    /* Print trace line when verbose >=2 or when verbose and not sync'ing */
    if (verbose >= 2 || (verbose && nosync))
    {
      ms_log (0, "%s|%s|%s|%s|%c|%lld|%lld|%.10g|%s|%lld|%lld|%s|%lld|%lld\n",
              mst->network, mst->station, mst->location, mst->channel, mst->dataquality,
              (long long int)sd->earliest, (long long int)sd->latest,
              mst->samprate, flp->filename,
              sd->startoffset, bytecount, sd->digeststr,
              (long long int)sd->updated, (long long int)flp->scantime);
      printf (" TINDEX: '%s'\n", (timeindexstr)?timeindexstr:"");
      printf (" TSPANS: '%s'\n", (timespansstr)?timespansstr:"");
      printf (" TRATES: '%s'\n", (timeratesstr)?timeratesstr:"");
    }

    if (timeindexstr)
    {
      free (timeindexstr);
      timeindexstr = NULL;
    }
    if (timespansstr)
    {
      free (timespansstr);
      timespansstr = NULL;
    }
    if (timeratesstr)
    {
      free (timeratesstr);
      timeratesstr = NULL;
    }

    mst = mst->next;
  }

  /* End the transaction */
  if (dbconn)
  {
    rv = SQLiteExec (dbconn, NULL, NULL, &errmsg, "COMMIT");
    if (rv != SQLITE_OK)
    {
      ms_log (2, "SQLite COMMIT failed: %s\n", (errmsg) ? errmsg : "");
      sqlite3_free (errmsg);
      return -1;
    }
  }

  return 0;
} /* End of SyncSQLiteFileSeries() */

/***************************************************************************
 * SQLiteExec():
 *
 * Generate an SQL statement composed of printf()-like parameters and
 * execute it on the specified SQLite database with sqlite3_exec().
 *
 * Returns value returned by sqlite3_exec() or SQLITE_ERROR if SQL
 * string cannot be generated.
 ***************************************************************************/
static int
SQLiteExec (sqlite3 *dbconn, int (*callback) (void *, int, char **, char **),
            void *callbackdata, char **errmsg, const char *format, ...)
{
  va_list argv;
  char *query = NULL;
  int length;
  int rv;

  va_start (argv, format);
  length = vasprintf (&query, format, argv);
  va_end (argv);

  if (!query || length <= 0)
  {
    ms_log (2, "Cannot create query string\n");
    return SQLITE_ERROR;
  }

  if (verbose >= 2)
    fprintf (stderr, "QUERY(%d): '%s'\n", length, query);

  rv = sqlite3_exec (dbconn, query, callback, callbackdata, errmsg);

  if (query)
    free (query);

  return rv;
} /* End of SQLiteExec() */

/***************************************************************************
 * SQLitePrepare():
 *
 * Generate an SQL statement composed of printf()-like parameters and
 * prepare it for the specified SQLite database.
 *
 * Returns value returned by sqlite3_prepare_v2() or SQLITE_ERROR if
 * SQL string cannot be generated.
 ***************************************************************************/
static int
SQLitePrepare (sqlite3 *dbconn, sqlite3_stmt **statement, const char *format, ...)
{
  va_list argv;
  char *query = NULL;
  int length;
  int rv;

  va_start (argv, format);
  length = vasprintf (&query, format, argv);
  va_end (argv);

  if (!query || length <= 0)
  {
    ms_log (2, "Cannot create query string\n");
    return SQLITE_ERROR;
  }

  if (verbose >= 2)
    fprintf (stderr, "QUERY(%d): '%s'\n", length, query);

  rv = sqlite3_prepare_v2 (dbconn, query, length + 1, statement, NULL);

  if (query)
    free (query);

  return rv;
} /* End of SQLitePrepare() */

/***************************************************************************
 * Local_mst_printtracelist:
 *
 * Print trace list summary information for the specified MSTraceGroup.
 *
 * The timeformat flag can either be:
 * 0 : SEED time format (year, day-of-year, hour, min, sec)
 * 1 : ISO time format (year, month, day, hour, min, sec)
 * 2 : Epoch time, seconds since the epoch
 ***************************************************************************/
void
Local_mst_printtracelist (MSTraceGroup *mstg, flag timeformat)
{
  struct sectiondetails *sd;
  MSTrace *mst = 0;
  char srcname[50];
  char stime[30];
  char etime[30];

  if (!mstg)
  {
    return;
  }

  mst = mstg->traces;

  /* Print out header */
  ms_log (0, "   Source                Earliest sample          Latest sample          Hz\n");

  while (mst)
  {
    mst_srcname (mst, srcname, 1);
    sd = (struct sectiondetails *)mst->prvtptr;

    /* Create formatted time strings */
    if (timeformat == 2)
    {
      snprintf (stime, sizeof (stime), "%.6f", (double)MS_HPTIME2EPOCH (sd->earliest));
      snprintf (etime, sizeof (etime), "%.6f", (double)MS_HPTIME2EPOCH (sd->latest));
    }
    else if (timeformat == 1)
    {
      if (ms_hptime2isotimestr (sd->earliest, stime, 1) == NULL)
        ms_log (2, "Cannot convert earliest time for %s\n", srcname);

      if (ms_hptime2isotimestr (sd->latest, etime, 1) == NULL)
        ms_log (2, "Cannot convert latest time for %s\n", srcname);
    }
    else
    {
      if (ms_hptime2seedtimestr (sd->earliest, stime, 1) == NULL)
        ms_log (2, "Cannot convert earliest time for %s\n", srcname);

      if (ms_hptime2seedtimestr (sd->latest, etime, 1) == NULL)
        ms_log (2, "Cannot convert latest time for %s\n", srcname);
    }

    /* Print trace info */
    ms_log (0, "%-17s %-24s %-24s  %-3.3g\n",
            srcname, stime, etime, mst->samprate);

    if (sd->tindex && verbose >= 3)
    {
      struct timeindex *tindex = sd->tindex;

      ms_log (0, "Time index:\n");
      while (tindex)
      {
        snprintf (stime, sizeof (stime), "%.6f", (double)MS_HPTIME2EPOCH (tindex->time));

        if (ms_hptime2isotimestr (tindex->time, etime, 1) == NULL)
          ms_log (2, "Cannot convert index time for %s\n", srcname);

        ms_log (0, "  %s (%s) - %lld\n", stime, etime, (long long int)tindex->byteoffset);

        tindex = tindex->next;
      }
    }

    if (sd->spans && verbose >= 3)
    {
      MSTraceID *id = 0;
      MSTraceSeg *seg = 0;

      if (sd->spans->traces && sd->spans->numtraces != 1)
        ms_log (2, "Span list contains more than 1 trace ID, something is seriously wrong!\n");

      id = sd->spans->traces;
      while (id)
      {
        ms_log (0, "Span list:\n");

        seg = id->first;
        while (seg)
        {
          if (ms_hptime2isotimestr (seg->starttime, stime, 1) == NULL)
            ms_log (2, "Cannot convert span start time for %s\n", id->srcname);

          if (ms_hptime2isotimestr (seg->endtime, etime, 1) == NULL)
            ms_log (2, "Cannot convert span end time for %s\n", id->srcname);

          ms_log (0, "  %s - %s\n", stime, etime);

          seg = seg->next;
        }
        id = id->next;
      }
    }

    mst = mst->next;
  }
} /* End of Local_mst_printtracelist() */

/***************************************************************************
 * ProcessParam():
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
ProcessParam (int argcount, char **argvec)
{
  int optind;
  char *tptr;

  /* Process all command line arguments */
  for (optind = 1; optind < argcount; optind++)
  {
    if (strcmp (argvec[optind], "-V") == 0)
    {
      ms_log (1, "%s version: %s\n", PACKAGE, VERSION);
      exit (0);
    }
    else if (strcmp (argvec[optind], "-h") == 0)
    {
      Usage ();
      exit (0);
    }
    else if (strncmp (argvec[optind], "-v", 2) == 0)
    {
      verbose += strspn (&argvec[optind][1], "v");
    }
    else if (strncmp (argvec[optind], "-ns", 3) == 0)
    {
      nosync = 1;
    }
    else if (strncmp (argvec[optind], "-noup", 5) == 0)
    {
      noupdate = 1;
    }
    else if (strncmp (argvec[optind], "-kp", 3) == 0)
    {
      keeppath = 1;
    }
    else if (strcmp (argvec[optind], "-tt") == 0)
    {
      timetol = strtod (GetOptValue (argcount, argvec, optind++), NULL);
    }
    else if (strcmp (argvec[optind], "-rt") == 0)
    {
      sampratetol = strtod (GetOptValue (argcount, argvec, optind++), NULL);
    }
    else if (strncmp (argvec[optind], "-table", 6) == 0)
    {
      table = strdup (GetOptValue (argcount, argvec, optind++));
    }
    else if (strncmp (argvec[optind], "-pghost", 7) == 0)
    {
#ifndef WITHOUTPOSTGRESQL
      pghost = strdup (GetOptValue (argcount, argvec, optind++));
#else
      ms_log(2, "%s was not compiled with Postgres support\n", PACKAGE);
#endif
    }
    else if (strcmp (argvec[optind], "-sqlite") == 0)
    {
      sqlitefile = strdup (GetOptValue (argcount, argvec, optind++));
    }
    else if (strncmp (argvec[optind], "-dbport", 7) == 0)
    {
      dbport = strdup (GetOptValue (argcount, argvec, optind++));
    }
    else if (strncmp (argvec[optind], "-dbname", 7) == 0)
    {
      dbname = strdup (GetOptValue (argcount, argvec, optind++));
    }
    else if (strncmp (argvec[optind], "-dbuser", 7) == 0)
    {
      dbuser = strdup (GetOptValue (argcount, argvec, optind++));
    }
    else if (strncmp (argvec[optind], "-dbpass", 7) == 0)
    {
      dbpass = strdup (GetOptValue (argcount, argvec, optind++));
    }
    else if (strncmp (argvec[optind], "-TRACE", 6) == 0)
    {
      dbconntrace = 1;
    }
    else if (strncmp (argvec[optind], "-sqlitebusyto", 7) == 0)
    {
      sqlitebusyto = strtoul (GetOptValue (argcount, argvec, optind++), NULL, 10);
    }
    else if (strncmp (argvec[optind], "-", 1) == 0 &&
             strlen (argvec[optind]) > 1)
    {
      ms_log (2, "Unknown option: %s\n", argvec[optind]);
      exit (1);
    }
    else
    {
      tptr = argvec[optind];

      /* Check for an input file list */
      if (tptr[0] == '@')
      {
        if (AddListFile (tptr + 1) < 0)
        {
          ms_log (2, "Error adding list file %s", tptr + 1);
          exit (1);
        }
      }
      /* Otherwise this is an input file */
      else
      {
        /* Add file to global file list */
        if (AddFile (tptr))
        {
          ms_log (2, "Error adding file to input list %s", tptr);
          exit (1);
        }
      }
    }
  }

  /* Make sure input files were specified */
  if (filelist == 0)
  {
    ms_log (2, "No input files were specified\n\n");
    ms_log (1, "%s version %s\n\n", PACKAGE, VERSION);
    ms_log (1, "Try %s -h for usage\n", PACKAGE);
    exit (1);
  }

  /* Make sure table was specified if database was specified */
  if (!nosync && !(pghost || sqlitefile))
  {
    ms_log (2, "No database was specified\n\n");
    ms_log (1, "%s version %s\n\n", PACKAGE, VERSION);
    ms_log (1, "Try %s -h for usage\n", PACKAGE);
    exit (1);
  }

  /* Report the program version */
  if (verbose)
    ms_log (1, "%s version: %s\n", PACKAGE, VERSION);

  return 0;
} /* End of ProcessParam() */

/***************************************************************************
 * GetOptValue:
 * Return the value to a command line option; checking that the value is
 * itself not an option (starting with '-') and is not past the end of
 * the argument list.
 *
 * argcount: total arguments in argvec
 * argvec: argument list
 * argopt: index of option to process, value is expected to be at argopt+1
 *
 * Returns value on success and exits with error message on failure
 ***************************************************************************/
static char *
GetOptValue (int argcount, char **argvec, int argopt)
{
  if (argvec == NULL || argvec[argopt] == NULL)
  {
    ms_log (2, "GetOptValue(): NULL option requested\n");
    exit (1);
    return 0;
  }

  /* Special case of '-o -' usage */
  if ((argopt + 1) < argcount && strcmp (argvec[argopt], "-o") == 0)
    if (strcmp (argvec[argopt + 1], "-") == 0)
      return argvec[argopt + 1];

  if ((argopt + 1) < argcount && *argvec[argopt + 1] != '-')
    return argvec[argopt + 1];

  ms_log (2, "Option %s requires a value, try -h for usage\n", argvec[argopt]);
  exit (1);
  return 0;
} /* End of GetOptValue() */

/***************************************************************************
 * AddFile:
 *
 * Add file to end of the global file list (filelist).
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
AddFile (char *filename)
{
  struct filelink *newlp;

  if (!filename)
  {
    ms_log (2, "AddFile(): No file name specified\n");
    return -1;
  }

  if (!(newlp = calloc (1, sizeof (struct filelink))))
  {
    ms_log (2, "AddFile(): Cannot allocate memory\n");
    return -1;
  }

  if (!(newlp->filename = strdup (filename)))
  {
    ms_log (2, "AddFile(): Cannot duplicate filename string\n");
    return -1;
  }

  newlp->mstg = NULL;
  newlp->next = NULL;

  /* Add new file to the end of the list */
  if (filelisttail == 0)
  {
    filelist = newlp;
    filelisttail = newlp;
  }
  else
  {
    filelisttail->next = newlp;
    filelisttail = newlp;
  }

  return 0;
} /* End of AddFile() */

/***************************************************************************
 * AddListFile:
 *
 * Add files listed in the specified file to the global input file list.
 *
 * Returns count of files added on success and -1 on error.
 ***************************************************************************/
static int
AddListFile (char *filename)
{
  FILE *fp;
  char filelistent[1024];
  int filecount = 0;

  if (verbose >= 1)
    ms_log (1, "Reading list file '%s'\n", filename);

  if (!(fp = fopen (filename, "rb")))
  {
    ms_log (2, "Cannot open list file %s: %s\n", filename, strerror (errno));
    return -1;
  }

  while (fgets (filelistent, sizeof (filelistent), fp))
  {
    char *cp;

    /* End string at first newline character */
    if ((cp = strchr (filelistent, '\n')))
      *cp = '\0';

    /* Skip empty lines */
    if (!strlen (filelistent))
      continue;

    /* Skip comment lines */
    if (*filelistent == '#')
      continue;

    if (verbose > 1)
      ms_log (1, "Adding '%s' from list file\n", filelistent);

    if (AddFile (filelistent))
      return -1;

    filecount++;
  }

  fclose (fp);

  return filecount;
} /* End of AddListFile() */

/***************************************************************************
 * ResolveFilePaths:
 *
 * Iterate through the global file list (filelist) and resolve full paths.
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
ResolveFilePaths (void)
{
  struct filelink *filelp;
  char abspath[PATH_MAX];

  filelp = filelist;
  while (filelp)
  {
    if (realpath (filelp->filename, abspath))
    {
      free (filelp->filename);

      if (!(filelp->filename = strdup (abspath)))
      {
        ms_log (2, "ResolveFilePaths(): Cannot duplicate filename string\n");
        return -1;
      }
    }
    else
    {
      ms_log (2, "ResolveFilePaths(): Error realpath(): %s\n",
              strerror (errno));
      return -1;
    }

    filelp = filelp->next;
  }

  return 0;
} /* End of ResolveFilePaths() */

/***************************************************************************
 * AddToString:
 *
 * Concatinate one string to another with a delimiter in-between
 * growing the target string as needed up to a maximum length.  The
 * new addition can be added to either the beggining or end of the
 * string using the where flag:
 *
 * where == 0 means add new addition to end of string
 * where != 0 means add new addition to beginning of string
 *
 * Return 0 on success, -1 on memory allocation error and -2 when
 * string would grow beyond maximum length.
 ***************************************************************************/
int
AddToString (char **string, char *add, char *delim, int where, int maxlen)
{
  int length;
  char *ptr;

  if (!string || !add)
    return -1;

  /* If string is empty, allocate space and copy the addition */
  if (!*string)
  {
    length = strlen (add) + 1;

    if (length > maxlen)
      return -2;

    if ((*string = (char *)malloc (length)) == NULL)
      return -1;

    strcpy (*string, add);
  }
  /* Otherwise add the addition with a delimiter */
  else
  {
    length = strlen (*string) + strlen (delim) + strlen (add) + 1;

    if (length > maxlen)
      return -2;

    if ((ptr = (char *)malloc (length)) == NULL)
      return -1;

    /* Put addition at beginning of the string */
    if (where)
    {
      snprintf (ptr, length, "%s%s%s",
                (add) ? add : "",
                (delim) ? delim : "",
                *string);
    }
    /* Put addition at end of the string */
    else
    {
      snprintf (ptr, length, "%s%s%s",
                *string,
                (delim) ? delim : "",
                (add) ? add : "");
    }

    /* Free previous string and set pointer to newly allocated space */
    free (*string);
    *string = ptr;
  }

  return 0;
} /* End of AddToString() */

/***************************************************************************
 * Usage():
 * Print the usage message.
 ***************************************************************************/
static void
Usage (void)
{
  fprintf (stderr, "%s - Synchronize Mini-SEED to database schema version: %s\n\n", PACKAGE, VERSION);
  fprintf (stderr, "Usage: %s [options] file1 [file2] [file3] ...\n\n", PACKAGE);
  fprintf (stderr,
           " ## General options ##\n"
           " -V             Report program version\n"
           " -h             Show this usage message\n"
           " -v             Be more verbose, multiple flags can be used\n"
           " -ns            No sync, perform data parsing but do not connect to database\n"
           "\n"
           " -noup          No updates, do not search for and replace index rows\n"
           " -kp            Keep specified paths, by default absolute paths are stored\n"
           " -tt secs       Specify a time tolerance for continuous traces\n"
           " -rt diff       Specify a sample rate tolerance for continuous traces\n"
           "\n"
#ifndef WITHOUTPOSTGRESQL
           "Either the -pghost or -sqlite argument is required\n"
           " -pghost  host  Specify Postgres database host, e.g. timeseriesdb\n"
#else
           "The -sqlite argument is required\n"
#endif
           " -sqlite  file  Specify SQLite database file, e.g. timeseries.sqlite\n"
           "\n"
           " -table   table Specify database table name, currently: %s\n"
           " -dbport  port  Specify database port, currently: %s\n"
           " -dbname  name  Specify database name or full connection info, currently: %s\n"
           " -dbuser  user  Specify database user name, currently: %s\n"
           " -dbpass  pass  Specify database user password\n"
           "\n"
           " -TRACE         Enable Postgres libpq tracing facility and direct output to stderr\n"
           " -sqlitebusyto msec   Set the SQLite busy timeout in milliseconds, currently: %lu\n"
           "\n"
           " files          File(s) of Mini-SEED records, list files prefixed with '@'\n"
           "\n",
           table, dbport, dbname, dbuser, sqlitebusyto);
} /* End of Usage() */
