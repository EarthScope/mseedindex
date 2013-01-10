/***************************************************************************
 * mseedsyncdb.c - Synchronize Mini-SEED with database schema
 *
 * Opens user specified file(s), parses the Mini-SEED records and
 * synchronizes time series summary with database schema.
 *
 * The time series are grouped by continuous segments of series
 * composed of contiguous records in a given file.  The result is that
 * each row in the schema represents a gapless segment of time series
 * contained in a single section of a file.
 * 
 * Expected database schema:
 *
 * Field Name   Type
 * ------------ ------------
 * network	character varying(2)
 * station	character varying(5)
 * location	character varying(2)
 * channel	character varying(3)
 * quality	character varying(1)
 * starttime	timestamp(6) without time zone
 * endtime	timestamp(6) without time zone
 * samplerate	numeric(8,3)
 * filename	character varying(256)
 * offset	numeric(19,0)
 * bytes	numeric(19,0)
 * hash		character varying(256)
 * updated	timestamp without time zone
 * scanned	timestamp without time zone
 *
 * In general critical error messages are prefixed with "ERROR:" and
 * the return code will be 1.  On successfull operation the return
 * code will be 0.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 *
 * modified 2013.009
 ***************************************************************************/

// Need man page

// synching to DB with updates for unchanges segments

// need to handle authenticated request filtering

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <regex.h>

#include <libpq-fe.h>

#include <libmseed.h>

#include "md5.h"

#define VERSION "0.1"
#define PACKAGE "mseedsyncdb"

static int     retval       = 0;
static flag    verbose      = 0;
static double  timetol      = -1.0; /* Time tolerance for continuous traces */
static double  sampratetol  = -1.0; /* Sample rate tolerance for continuous traces */
static flag    nosync       = 0;    /* Control synchronization with database, 1 = no database */

static char   *dbconninfo   = "host=postdb dbname=timeseries user=timeseries password=timeseries";
static PGconn *dbconn       = NULL; /* Database connection */

struct segdetails {
  int64_t startoffset;
  int64_t endoffset;
  md5_state_t digeststate;
};

struct filelink {
  char *filename;
  MSTraceGroup *mstg;
  struct filelink *next;
};

struct filelink *filelist = 0;
struct filelink *filelisttail = 0;

static void syncfileseries (struct filelink *flp, time_t scantime);
static int processparam (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static int addfile (char *filename);
static int addlistfile (char *filename);
static void usage (void);

int
main (int argc, char **argv)
{
  struct segdetails *sd = NULL;
  struct filelink *flp = NULL;
  MSRecord *msr = NULL;
  MSTrace *mst = NULL;
  MSTrace *cmst = NULL;
  hptime_t endtime = HPTERROR;
  int retcode = MS_NOERROR;
  time_t scantime;
  
  off_t filepos = 0;
  off_t prevfilepos = 0;
  
  flag whence = 0;
  
  /* Set default error message prefix */
  ms_loginit (NULL, NULL, NULL, "ERROR: ");
  
  /* Process given parameters (command line and parameter file) */
  if ( processparam (argc, argv) < 0 )
    return 1;
  
  if ( dbconninfo && ! nosync )
    {
      dbconn = PQconnectdb(dbconninfo);
      
      if ( ! dbconn )
	{
	  ms_log (2, "PQconnectdb returned NULL, connection failed");
	  exit (1);
	}
      
      if ( PQstatus(dbconn) != CONNECTION_OK )
	{
	  ms_log (2, "Connection to database failed: %s", PQerrorMessage(dbconn));
	  PQfinish (dbconn);
	  exit (1);
	}
      
      if ( verbose )
	{
	  int sver = PQserverVersion(dbconn);
	  int major, minor, less;
	  major = sver/10000;
	  minor = sver/100 - major*100;
	  less = sver - major*10000 - minor*100;
	  
	  ms_log (1, "Connected to database %s on host %s (server %d.%d.%d)\n",
		  PQdb(dbconn), PQhost(dbconn),
		  major, minor, less);
	}
    }
  
  flp = filelist;
  while ( flp )
    {
      if ( verbose >= 1 )
	ms_log (1, "Processing: %s\n", flp->filename);
      
      flp->mstg = mst_initgroup (flp->mstg);
      cmst = NULL;
      
      scantime = time (NULL);
      
      /* Read records from the input file */
      while ( (retcode = ms_readmsr (&msr, flp->filename, -1, &filepos,
				     NULL, 1, 0, verbose)) == MS_NOERROR )
	{
	  mst = NULL;
	  whence = 0;
	  endtime = msr_endtime(msr);
	  
	  if ( cmst )
	    {
	      mst = mst_findadjacent (flp->mstg, &whence, msr->dataquality,
				      msr->network, msr->station, msr->location,
				      msr->channel, msr->samprate, sampratetol,
				      msr->starttime, endtime, timetol);
	    }
	  
	  /* Exception: check for channel matching records with no samples (e.g. detection records) */
	  if ( mst != cmst && msr->samplecnt == 0 )
	    {
	      mst = mst_findmatch (cmst, msr->dataquality, msr->network, msr->station,
				   msr->location, msr->channel);
	      if ( mst == cmst )
		{
		  whence = 1;
		}
	    }
	  
	  if ( mst == cmst && whence == 1 && filepos == (prevfilepos + msr->reclen) )
	    {
	      if ( msr->samplecnt > 0 )
		mst_addmsr (cmst, msr, 1);
	      
	      sd = (struct segdetails *)cmst->prvtptr;
	      sd->endoffset = filepos + msr->reclen - 1;
	      md5_append (&(sd->digeststate), (const md5_byte_t *)msr->record, msr->reclen);
	    }
	  else
	    {
	      /* Create & populate new current MSTrace and add it to the file MSTraceGroup */
	      cmst = mst_init (NULL);
	      mst_addtracetogroup (flp->mstg, cmst);
	      
	      strncpy (cmst->network, msr->network, sizeof(cmst->network));
	      strncpy (cmst->station, msr->station, sizeof(cmst->station));
	      strncpy (cmst->location, msr->location, sizeof(cmst->location));
	      strncpy (cmst->channel, msr->channel, sizeof(cmst->channel));
	      cmst->dataquality = msr->dataquality;
	      cmst->starttime = msr->starttime;
	      cmst->endtime = endtime;
	      cmst->samprate = msr->samprate;
	      cmst->samplecnt = msr->samplecnt;
	      
	      if ( ! (sd = calloc (1, sizeof (struct segdetails))) )
		{
		  ms_log (2, "Cannot allocate segment details\n");
		  return 1;
		}
	      
	      cmst->prvtptr = sd;
	      
	      sd->startoffset = filepos;
	      sd->endoffset = filepos + msr->reclen - 1;
	      
	      memset (&(sd->digeststate), 0, sizeof(md5_state_t));
 	      md5_init (&(sd->digeststate));
	      md5_append (&(sd->digeststate), (const md5_byte_t *)msr->record, msr->reclen);
	    }
	  
	  prevfilepos = filepos;
	}
      
      /* Print error if not EOF and not counting down records */
      if ( retcode != MS_ENDOFFILE )
	{
	  ms_log (2, "Cannot read %s: %s\n", flp->filename, ms_errorstr(retcode));
	  ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);
	  exit (1);
	}
      
      /* Make sure everything is cleaned up */
      ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);
      
      /* Sync time series listing */
      syncfileseries (flp, scantime);
      
      flp = flp->next;
    } /* End of looping over file list */
  
  if ( dbconn )
    {
      if ( verbose >= 2 )
	ms_log (1, "Closing database connection to %s\n", PQhost(dbconn));

      PQfinish (dbconn);
    }
  
  return retval;
}  /* End of main() */


/***************************************************************************
 * syncfileseries():
 *
 * Synchronize the time series list associated with a file entry to
 * the database.
 *
 * Expected database schema:
 *
 * Field Name   Type
 * ------------ ------------
 * network	character varying(2)
 * station	character varying(5)
 * location	character varying(2)
 * channel	character varying(3)
 * quality	character varying(1)
 * starttime	timestamp(6) without time zone
 * endtime	timestamp(6) without time zone
 * samplerate	numeric(8,3)
 * filename	character varying(256)
 * offset	numeric(19,0)
 * bytes	numeric(19,0)
 * hash		character varying(256)
 * updated	timestamp without time zone
 * scanned	timestamp without time zone
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static void
syncfileseries (struct filelink *flp, time_t scantime)
{
  struct segdetails *sd;
  MSTrace *mst = NULL;
  char starttime[30];
  char endtime[30];
  int64_t bytecount;
  
  md5_byte_t digest[16];
  char digeststr[33];
  int idx;

  if ( ! flp )
    return;
  
  // Chad, sync with database, use transactions.
  // Check for nosync and print when verbose
  
  /* Print header line */
  ms_log (0, "%s:\n", flp->filename);
  
  /* Loop through trace list */
  mst = flp->mstg->traces;
  while ( mst )
    {
      sd = (struct segdetails *)mst->prvtptr;
      
      ms_hptime2seedtimestr (mst->starttime, starttime, 1);
      ms_hptime2seedtimestr (mst->endtime, endtime, 1);
      
      /* Calculate MD5 digest and string */
      md5_finish(&(sd->digeststate), digest);
      for (idx=0; idx < 16; idx++)
	sprintf (digeststr+(idx*2), "%02x", digest[idx]);
      
      bytecount = sd->endoffset - sd->startoffset + 1;
      
      /* Print trace line */
      ms_log (0, "%s|%s|%s|%s|%.1s|%s|%s|%.10g|%s|%lld|%lld|%s|%lld\n",
	      mst->network, mst->station, mst->location, mst->channel,
	      (mst->dataquality) ? &(mst->dataquality) : "",
	      starttime, endtime, mst->samprate, flp->filename,
	      sd->startoffset, bytecount, digeststr,
	      (long long int) scantime);
      
      mst = mst->next;
    }
  
  return;
}  /* End of syncfileseries() */


/***************************************************************************
 * parameter_proc():
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
processparam (int argcount, char **argvec)
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
	  usage();
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
      else if (strncmp (argvec[optind], "-C", 2) == 0)
	{
	  dbconninfo = strdup (getoptval(argcount, argvec, optind++));
	}
      else if (strcmp (argvec[optind], "-tt") == 0)
	{
	  timetol = strtod (getoptval(argcount, argvec, optind++), NULL);
	}
      else if (strcmp (argvec[optind], "-rt") == 0)
	{
	  sampratetol = strtod (getoptval(argcount, argvec, optind++), NULL);
	}
      else if (strncmp (argvec[optind], "-", 1) == 0 &&
	       strlen (argvec[optind]) > 1 )
	{
	  ms_log (2, "Unknown option: %s\n", argvec[optind]);
	  exit (1);
	}
      else
	{
	  tptr = argvec[optind];
	  
          /* Check for an input file list */
          if ( tptr[0] == '@' )
            {
              if ( addlistfile (tptr+1) < 0 )
                {
                  ms_log (2, "Error adding list file %s", tptr+1);
                  exit (1);
                }
            }
          /* Otherwise this is an input file */
          else
            {
              /* Add file to global file list */
              if ( addfile (tptr) )
                {
                  ms_log (2, "Error adding file to input list %s", tptr);
                  exit (1);
                }
            }
	}
    }
  
  /* Make sure input files were specified */
  if ( filelist == 0 )
    {
      ms_log (2, "No input files were specified\n\n");
      ms_log (1, "%s version %s\n\n", PACKAGE, VERSION);
      ms_log (1, "Try %s -h for usage\n", PACKAGE);
      exit (1);
    }
  
  /* Report the program version */
  if ( verbose )
    ms_log (1, "%s version: %s\n", PACKAGE, VERSION);
  
  return 0;
}  /* End of parameter_proc() */


/***************************************************************************
 * getoptval:
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
getoptval (int argcount, char **argvec, int argopt)
{
  if ( argvec == NULL || argvec[argopt] == NULL ) {
    ms_log (2, "getoptval(): NULL option requested\n");
    exit (1);
    return 0;
  }
  
  /* Special case of '-o -' usage */
  if ( (argopt+1) < argcount && strcmp (argvec[argopt], "-o") == 0 )
    if ( strcmp (argvec[argopt+1], "-") == 0 )
      return argvec[argopt+1];
    
  if ( (argopt+1) < argcount && *argvec[argopt+1] != '-' )
    return argvec[argopt+1];
  
  ms_log (2, "Option %s requires a value, try -h for usage\n", argvec[argopt]);
  exit (1);
  return 0;
}  /* End of getoptval() */


/***************************************************************************
 * addfile:
 *
 * Add file to end of the global file list (filelist).
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
addfile (char *filename)
{
  struct filelink *newlp;
  
  if ( ! filename )
    {
      ms_log (2, "addfile(): No file name specified\n");
      return -1;
    }
  
  if ( ! (newlp = malloc (sizeof (struct filelink))) )
    {
      ms_log (2, "addfile(): Cannot allocate memory\n");
      return -1;
    }

  if ( ! (newlp->filename = strdup(filename)) )
    {
      ms_log (2, "addfile(): Cannot duplicate filename string\n");
      return -1;
    }
  
  newlp->mstg = NULL;
  newlp->next = NULL;
  
  /* Add new file to the end of the list */
  if ( filelisttail == 0 )
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
}  /* End of addfile() */


/***************************************************************************
 * addlistfile:
 *
 * Add files listed in the specified file to the global input file list.
 *
 * Returns count of files added on success and -1 on error.
 ***************************************************************************/
static int
addlistfile (char *filename) 
{
  FILE *fp;
  char filelistent[1024];
  int filecount = 0;
  
  if ( verbose >= 1 )
    ms_log (1, "Reading list file '%s'\n", filename);
  
  if ( ! (fp = fopen(filename, "rb")) )
    {
      ms_log (2, "Cannot open list file %s: %s\n", filename, strerror(errno));
      return -1;
    }
  
  while ( fgets (filelistent, sizeof(filelistent), fp) )
    {
      char *cp;
      
      /* End string at first newline character */
      if ( (cp = strchr(filelistent, '\n')) )
        *cp = '\0';
      
      /* Skip empty lines */
      if ( ! strlen (filelistent) )
        continue;
      
      /* Skip comment lines */
      if ( *filelistent == '#' )
        continue;
      
      if ( verbose > 1 )
        ms_log (1, "Adding '%s' from list file\n", filelistent);
      
      if ( addfile (filelistent) )
        return -1;
      
      filecount++;
    }
  
  fclose (fp);
  
  return filecount;
}  /* End of addlistfile() */


/***************************************************************************
 * usage():
 * Print the usage message.
 ***************************************************************************/
static void
usage (void)
{
  fprintf (stderr, "%s - Synchronize Mini-SEED to database schema version: %s\n\n", PACKAGE, VERSION);
  fprintf (stderr, "Usage: %s [options] file1 [file2] [file3] ...\n\n", PACKAGE);
  fprintf (stderr,
	   " ## General options ##\n"
	   " -V           Report program version\n"
	   " -h           Show this usage message\n"
	   " -v           Be more verbose, multiple flags can be used\n"
	   " -ns          No sync, perform data parsing but do not connect to database\n"
	   "\n"
	   " -C conninfo  Database connection parameters\n"
	   "                currently: '%s'\n"
	   "\n"
	   " -tt secs     Specify a time tolerance for continuous traces\n"
	   " -rt diff     Specify a sample rate tolerance for continuous traces\n"
	   "\n"
	   " files        File(s) of Mini-SEED records, list files prefixed with '@'\n"
	   "\n", dbconninfo);
}  /* End of usage() */
