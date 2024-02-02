#!/usr/bin/env python3
#
# fetchIndexInfo.py: Fetch time series index information.
# Reference: https://github.com/iris-edu/mseedindex/wiki
#
# Read an EarthScope time series index table in an SQLite3 database and print
# readable details of the index entries, alternatively a SYNC-style
# listing can be printed.
#
# Data may be selected by network, station, location, channel and time
# range.  A selection may be specified using command line options or by
# specifying a file containing one or more independent selections.
#
# If present, a '<table>_summary' table that summarizes the time
# extents of each channel in the index is used to optimize the query
# to the index.  This table can be created with the following
# statement (assuming an index table of 'tsindex'):
#
# CREATE TABLE tsindex_summary AS
#   SELECT network,station,location,channel,
#     min(starttime) AS earliest, max(endtime) AS latest, datetime('now') as updt
#     FROM tsindex
#     GROUP BY 1,2,3,4;
#
# The summary table is used to:
#   a) resolve wildcards, allowing the use of '=' operator and thus table index
#   b) reduce index table search to channels that are known to be present
#
# Modified: 2024.032
# Written by Chad Trabant, EarthScope Data Services

from collections import namedtuple
import threading
import time
import sys
import signal
import os
import getopt
import re
import datetime
import sqlite3

version = '1.6'
verbose = 0
table = 'tsindex'
dbconn = None

# Assume no time series index section (row) spans more than this many days
maxsectiondays = 10

def main():
    global verbose
    global table
    requestfile = None
    request = []
    index_rows = []
    network = None
    station = None
    location = None
    channel = None
    starttime = None
    endtime = None
    filename = None
    sync = False

    try:
        opts, args = getopt.gnu_getopt(sys.argv[1:],
                                       "vhl:t:N:S:L:C:s:e:f:",
                                       ["verbose","help", "listfile=", "table=",
                                        "network=","station=","location=","channel=",
                                        "starttime=", "endtime=", "filename=", "SYNC"])
    except Exception as err:
        print (str(err))
        usage()
        sys.exit(2)

    for o, a in opts:
        if o == "-v":
            verbose = verbose + 1
        elif o in ("-h", "--help"):
            usage()
            sys.exit()
        elif o in ("-l", "--listfile"):
            requestfile = a
        elif o in ("-t", "--table"):
            table = a
        elif o in ("-N", "--network"):
            network = a
        elif o in ("-S", "--station"):
            station = a
        elif o in ("-L", "--location"):
            location = a
        elif o in ("-C", "--channel"):
            channel = a
        elif o in ("-s", "--starttime"):
            starttime = a
        elif o in ("-e", "--endtime"):
            endtime = a
        elif o in ("-f", "--filename"):
            filename = a
        elif o == "--SYNC":
            sync = True
        else:
            assert False, "Unhandled option"

    if len(args) == 0:
        print ("No database specified, try -h for usage\n")
        sys.exit()

    sqlitefile = args[0]

    # Read request from specified request file
    if requestfile:
        try:
            request = read_request_file(requestfile)
        except Exception as err:
            print ("Error reading request file:\n  {0}".format(err))
            sys.exit(2)

    # Add request specified as command line options
    if network or station or location or channel or starttime or endtime:
        if network is None:
            network = '*'
        if station is None:
            station = '*'
        if location is None:
            location = '*'
        if channel is None:
            channel = '*'
        if starttime is None:
            starttime = '*'
        else:
            starttime = normalize_datetime (starttime)
        if endtime is None:
            endtime = '*'
        else:
            endtime = normalize_datetime (endtime)

        request.append([network,station,location,channel,starttime,endtime])

    if len(request) <= 0 and filename is None:
        print ("No selection specified, try -h for usage")
        sys.exit()

    if verbose >= 2:
        print ("Request:")
        for req in request:
            print ("  {0}: {1}".format(len(req), req))

    try:
        index_rows = fetch_index_rows(sqlitefile, table, request, filename)
    except Exception as err:
        if str(err) != "interrupted":
            print ("Error fetching index rows from '{0}':\n  {1}".format(sqlitefile, err))
        sys.exit(2)

    if sync:
        print_sync(index_rows)
    else:
        print_info(index_rows)

    if verbose >= 1:
        print ("Fetched {0:d} index rows".format(len(index_rows)))

    return

def read_request_file(requestfile):
    '''Read a specified request file and return it as a list of tuples.

    Expected selection format is:
      Network Station Location Channel StartTime EndTime

    where the fields are space delimited
    and Network, Station, Location and Channel may contain '*' and '?' wildcards
    and StartTime and EndTime are in YYYY-MM-DDThh:mm:ss.ssssss format or are '*'

    Returned tuples have the same fields and ordering as the selection lines.
    '''
    global verbose

    if verbose >= 1:
        print ("Reading select file: {0}".format(requestfile))

    request = []
    linenumber = 1
    linematch = re.compile ('^[\w\?\*]{1,2}\s+[\w\?\*]{1,5}\s+[-\w\?\*]{1,2}\s+[\w\?\*]{1,3}\s+[-:T.*\d]+\s+[-:T.*\d]+$');

    with open(requestfile, mode="r") as fp:
        for line in fp:
            line = line.strip()

            # Add line to request list if it matches validation regex
            if linematch.match(line):
                fields = line.split()

                # Normalize start and end times to "YYYY-MM-DDThh:mm:ss.ffffff" if not wildcards
                if fields[4] != '*':
                    try:
                        fields[4] = normalize_datetime (fields[4])
                    except:
                        raise ValueError("Cannot normalize start time (line {0:d}): {1:s}".format(linenumber, fields[4]))

                if fields[5] != '*':
                    try:
                        fields[5] = normalize_datetime (fields[5])
                    except:
                        raise ValueError("Cannot normalize start time (line {0:d}): {1:s}".format(linenumber, fields[4]))

                request.append(fields)

            # Raise error if line is not empty and does not start with a #
            elif line and not line.startswith("#"):
                raise ValueError("Unrecognized selection line ({0:d}): '{1:s}'".format(linenumber, line))

            linenumber = linenumber + 1

    return request

def fetch_index_rows(sqlitefile, table, request, filename):
    '''Fetch index rows matching specified request[]

    `sqlitefile`: SQLite3 database file
    `table`: Target database table name
    `request`: List of tuples containing (net,sta,loc,chan,start,end)
    `filename`: Limit results to filename (or comma-separated list of)

    Request elements may contain '?' and '*' wildcards.  The start and
    end elements can be a single '*' if not a date-time string.

    Return rows as list of tuples containing:
    (network,station,location,channel,quality,starttime,endtime,samplerate,
     filename,byteoffset,bytes,hash,timeindex,timespans,timerates,
     format,filemodtime,updated,scanned)
    '''

    global verbose
    global dbconn
    index_rows = []

    if not os.path.exists(sqlitefile):
        raise ValueError("Database not found: {0}".format(sqlitefile))

    try:
        dbconn = sqlite3.connect(sqlitefile, 10.0)
    except Exception as err:
        raise ValueError(str(err))

    cursor = dbconn.cursor()

    # Store temporary table(s) in memory
    try:
        cursor.execute("PRAGMA temp_store=MEMORY")
    except Exception as err:
        raise ValueError(str(err))

    if len(request) > 0:
        index_rows = fetch_index_by_request(cursor, table, request, filename)
    elif filename:
        index_rows = fetch_index_by_filename(cursor, table, filename)
    else:
        print ("No criteria (request selections or filename), nothing to do.")

    # Print time series index rows
    if verbose >= 3:
        print ("TSINDEX:")
        for row in index_rows:
            print ("  ", row)

    dbconn.close()
    dbconn = None

    return index_rows

def fetch_index_by_request(cursor, table, request, filename):
    '''Fetch index rows matching specified request[]

    `cursor`: Database cursor
    `table`: Target database table
    `request`: List of tuples containing (net,sta,loc,chan,start,end)
    `filename`: Limit results to filename (or comma-separated list of)

    Request elements may contain '?' and '*' wildcards.  The start and
    end elements can be a single '*' if not a date-time string.

    Return rows as list of tuples containing:
    (network,station,location,channel,quality,starttime,endtime,samplerate,
     filename,byteoffset,bytes,hash,timeindex,timespans,timerates,
     format,filemodtime,updated,scanned)
    '''

    global verbose
    global maxsectiondays

    # Create temporary table and load request
    try:
        if verbose >= 2:
            print ("Creating temporary request table")

        cursor.execute("CREATE TEMPORARY TABLE request "
                       "(network TEXT, station TEXT, location TEXT, channel TEXT, "
                       "starttime TEXT, endtime TEXT) ")

        if verbose >= 2:
            print ("Populating temporary request table")

        for req in request:
            # Replace "--" location ID request alias with true empty value
            if req[2] == "--":
                req[2] = ""

            cursor.execute("INSERT INTO request (network,station,location,channel,starttime,endtime) "
                           "VALUES (?,?,?,?,?,?) ", req)

        if verbose >= 2:
            print ("Request table populated")
    except Exception as err:
        raise ValueError(str(err))

    # Print request table
    if verbose >= 3:
        cursor.execute ("SELECT * FROM request")
        rows = cursor.fetchall()
        print ("REQUEST:")
        for row in rows:
            print ("  ", row)

    # Determine if any wildcards are used
    wildcards = False
    for req in request:
        for field in req:
            if '*' in field or '?' in field:
                wildcards = True
                break

    # Determine if summary table exists
    summary_table = "{0}_summary".format(table)
    cursor.execute("SELECT count(*) FROM sqlite_master WHERE type='table' and name='%s'" % summary_table);
    summary_present = cursor.fetchone()[0]

    if wildcards:
        # Resolve wildcards using summary table if present to:
        # a) resolve wildcards, allows use of '=' operator and table index
        # b) reduce index table search to channels that are known included
        if summary_present:
            resolve_request(cursor, summary_table, "request")
            wildcards = False
        # Replace wildcarded starttime and endtime with extreme date-times
        else:
            cursor.execute("UPDATE request SET starttime='0000-00-00T00:00:00' WHERE starttime='*'")
            cursor.execute("UPDATE request SET endtime='5000-00-00T00:00:00' WHERE endtime='*'")

    # Fetch final results by joining request and index table
    try:
        if verbose >= 2:
            print ("Fetching index entries")

        # The index rows are selected by joining with the request for matching
        # network, station, location and channel entries with intersecting time ranges.
        #
        # The 'maxsectiondays' value is used to further constrain the search.  This is
        # needed because the endtime cannot be used by the index scan as only a single
        # field may be compared with inequalities (i.e. greater-than, less-than).
        # This optimization limits discovery of index section entries (rows) to those
        # not spanning more than 'maxsectiondays'.

        statement = ("SELECT ts.network,ts.station,ts.location,ts.channel,ts.quality, "
                     "  ts.starttime,ts.endtime,ts.samplerate, "
                     "  ts.filename,ts.byteoffset,ts.bytes,ts.hash, "
                     "  ts.timeindex,ts.timespans,ts.timerates, "
                     "  ts.format,ts.filemodtime,ts.updated,ts.scanned "
                     "FROM {0} ts, request r "
                     "WHERE "
                     "  ts.network {1} r.network "
                     "  AND ts.station {1} r.station "
                     "  AND ts.location {1} r.location "
                     "  AND ts.channel {1} r.channel "
                     "  AND ts.starttime <= r.endtime "
                     "  AND (r.starttime < 1 OR ts.starttime >= datetime(r.starttime,'-{2} days')) "
                     "  AND ts.endtime >= r.starttime "
                     .format(table, "GLOB" if wildcards else "=", maxsectiondays))

        if filename:
            statement += ("  AND ts.filename IN ({0}) ".
                          format(','.join("'" + item + "'" for item in filename.split(','))))

        if verbose >= 4:
            print ("STATEMENT:\n{0}".format(statement))

        cursor.execute(statement)

    except Exception as err:
        raise ValueError(str(err))

    # Map tuple to a named tuple for clear referencing
    NamedRow = namedtuple ('NamedRow',
                           ['network','station','location','channel','quality',
                            'starttime','endtime','samplerate','filename',
                            'byteoffset','bytes','hash','timeindex','timespans',
                            'timerates','format','filemodtime','updated','scanned'])

    index_rows = []
    while True:
        row = cursor.fetchone()
        if row is None:
            break
        #index_rows.append(row)
        index_rows.append(NamedRow(*row))

    # Sort results in application (ORDER BY in SQL triggers bad index usage)
    index_rows.sort()

    cursor.execute("DROP TABLE request")

    return index_rows

def resolve_request(cursor, summary_table, request_table):
    '''Resolve request table using summary

    `cursor`: Database cursor
    `summary_table`: summary table to resolve
    `request_table`: request table to resolve

    Resolve any '?' and '*' wildcards in the specified request table.
    The original table is renamed, rebuilt with a join to summary table
    and then original table is then removed.
    '''

    global verbose
    request_table_orig = request_table + "_orig"

    if verbose >= 1:
        print ("Resolving request using summary")

    # Rename request table
    try:
        cursor.execute("ALTER TABLE {0} RENAME TO {1}".format(request_table, request_table_orig))
    except Exception as err:
        raise ValueError(str(err))

    # Create resolved request table by joining with summary
    try:
        cursor.execute("CREATE TEMPORARY TABLE {0} "
                       "(network TEXT, station TEXT, location TEXT, channel TEXT, "
                       "starttime TEXT, endtime TEXT) ".format(request_table))

        if verbose >= 2:
            print ("Populating resolved request table")

        cursor.execute("INSERT INTO {0} (network,station,location,channel,starttime,endtime) "
                       "SELECT s.network,s.station,s.location,s.channel,"
                       "CASE WHEN r.starttime='*' THEN s.earliest ELSE r.starttime END,"
                       "CASE WHEN r.endtime='*' THEN s.latest ELSE r.endtime END "
                       "FROM {1} s, {2} r "
                       "WHERE "
                       "  (r.starttime='*' OR r.starttime <= s.latest) "
                       "  AND (r.endtime='*' OR r.endtime >= s.earliest) "
                       "  AND (r.network='*' OR s.network GLOB r.network) "
                       "  AND (r.station='*' OR s.station GLOB r.station) "
                       "  AND (r.location='*' OR s.location GLOB r.location) "
                       "  AND (r.channel='*' OR s.channel GLOB r.channel) ".
                       format(request_table, summary_table, request_table_orig))

    except Exception as err:
        raise ValueError(str(err))

    resolvedrows = cursor.execute("SELECT COUNT(*) FROM request").fetchone()[0]

    # Print resolved request table
    if verbose >= 3:
        cursor.execute ("SELECT * FROM request")
        rows = cursor.fetchall()
        print ("RESOLVED ({0} rows):".format(resolvedrows))
        for row in rows:
            print ("  ", row)

    cursor.execute("DROP TABLE {0}".format(request_table_orig))

    return

def fetch_index_by_filename(cursor, table, filename):
    '''Fetch index rows matching specified filename(s)

    `cursor`: Database cursor
    `filename`: Limit results to filename (or comma-separated list of)

    Request elements may contain '?' and '*' wildcards.  The start and
    end elements can be a single '*' if not a date-time string.

    Return rows as list of tuples containing:
    (network,station,location,channel,quality,starttime,endtime,samplerate,
     filename,byteoffset,bytes,hash,timeindex,timespans,timerates,
     format,filemodtime,updated,scanned)
    '''

    global verbose
    index_rows = []

    # Fetch final results limiting to specified filename(s)
    try:
        if verbose >= 2:
            print ("Fetching index entries based on filename")

        statement = ("SELECT ts.network,ts.station,ts.location,ts.channel,ts.quality, "
                     "  ts.starttime,ts.endtime,ts.samplerate, "
                     "  ts.filename,ts.byteoffset,ts.bytes,ts.hash, "
                     "  ts.timeindex,ts.timespans,ts.timerates, "
                     "  ts.format,ts.filemodtime,ts.updated,ts.scanned "
                     "FROM {0} ts "
                     "WHERE "
                     "  ts.filename IN ({1}) ".
                     format(table, ','.join("'" + item + "'" for item in filename.split(','))))

        if verbose >= 4:
            print ("STATEMENT:\n{0}".format(statement))

        cursor.execute(statement)

    except Exception as err:
        raise ValueError(str(err))

    # Map tuple to a named tuple for clear referencing
    NamedRow = namedtuple ('NamedRow',
                           ['network','station','location','channel','quality',
                            'starttime','endtime','samplerate','filename',
                            'byteoffset','bytes','hash','timeindex','timespans',
                            'timerates','format','filemodtime','updated','scanned'])

    index_rows = []
    while True:
        row = cursor.fetchone()
        if row is None:
            break
        index_rows.append(NamedRow(*row))

    # Sort results in application (ORDER BY in SQL triggers bad index usage)
    index_rows.sort()

    return index_rows

def print_info(index_rows):
    '''Print index information given a list of named tuples containing:

    (0:network,1:station,2:location,3:channel,4:quality,5:starttime,6:endtime,7:samplerate,
     8:filename,9:byteoffset,10:bytes,11:hash,12:timeindex,13:timespans,14:timerates,
     15:format,16:filemodtime,17:updated,18:scanned)
    '''

    for NRow in index_rows:
        print ("{0}:".format(NRow.filename))
        print ("  {0}.{1}.{2}.{3}.{4}, samplerate: {5}, timerange: {6} - {7}".
               format(NRow.network,NRow.station,NRow.location,NRow.channel,NRow.quality,
                      NRow.samplerate,NRow.starttime,NRow.endtime))
        print ("  byteoffset: {0}, bytes: {1}, endoffset: {2}, hash: {3}".
               format(NRow.byteoffset,NRow.bytes,NRow.byteoffset + NRow.bytes, NRow.hash))
        print ("  filemodtime: {0}, updated: {1}, scanned: {2}".
               format(NRow.filemodtime, NRow.updated, NRow.scanned))

        print ("Time index: (time => byteoffset)")
        for index in NRow.timeindex.split(','):
            (time,offset) = index.split('=>')

            # Convert epoch times to nicer format
            if re.match ("^[+-]?\d+(>?\.\d+)?$", time.strip()):
                time = timestamp_to_isoZ(time)

            print ("  {0} => {1}".format(time,offset))

        if NRow.timespans:
            # If per-span sample rates are present create a list of them
            rates = None
            if NRow.timerates:
                rates = NRow.timerates.split(',')

            # Print time spans either with or without per-span rates
            print ("Time spans:")
            for idx, span in enumerate(NRow.timespans.split(',')):
                (start,end) = span.lstrip('[').rstrip(']').split(':')
                if rates:
                    print ("  {0} - {1} ({2})".format(timestamp_to_isoZ(start),
                                                      timestamp_to_isoZ(end),
                                                      rates[idx]))
                else:
                    print ("  {0} - {1}".format(timestamp_to_isoZ(start),
                                                timestamp_to_isoZ(end)))

    return

def print_sync(index_rows):
    '''Print a SYNC listing given a list of named tuples containing:

    (0:network,1:station,2:location,3:channel,4:quality,5:starttime,6:endtime,7:samplerate,
     8:filename,9:byteoffset,10:bytes,11:hash,12:timeindex,13:timespans,14:timerates,
     15:format,16:filemodtime,17:updated,18:scanned)
    '''

    for NRow in index_rows:
        if NRow.timespans:
            # If per-span sample rates are present create a list of them
            rates = None
            if NRow.timerates:
                rates = NRow.timerates.split(',')

            # Print time spans either with or without per-span rates
            for idx, span in enumerate(NRow.timespans.split(',')):
                (start,end) = span.lstrip('[').rstrip(']').split(':')

                starttime = datetime.datetime.utcfromtimestamp(float(start)).strftime("%Y,%j,%H:%M:%S.%f")
                endtime = datetime.datetime.utcfromtimestamp(float(end)).strftime("%Y,%j,%H:%M:%S.%f")
                updated = datetime.datetime.strptime(NRow.updated, "%Y-%m-%dT%H:%M:%S").strftime("%Y,%j")

                if rates:
                    print ("{0}|{1}|{2}|{3}|{4}|{5}||{6}||||{7}||NC|{8}||".
                           format(NRow.network,NRow.station,NRow.location,NRow.channel,
                                  starttime,endtime,
                                  rates[idx],
                                  NRow.quality,
                                  updated))
                else:
                    print ("{0}|{1}|{2}|{3}|{4}|{5}||{6}||||{7}||NC|{8}||".
                           format(NRow.network,NRow.station,NRow.location,NRow.channel,
                                  starttime,endtime,
                                  NRow.samplerate,
                                  NRow.quality,
                                  updated))

    return

def normalize_datetime(timestring):
    '''Normalize time string to strict YYYY-MM-DDThh:mm:ss.ffffff format
    '''

    # Split Year,Month,Day Hour,Min,Second,Fractional seconds
    timepieces = re.split("[-.:T]+", timestring)
    timepieces = [int(i) for i in timepieces]

    # Rebuild into target format
    return datetime.datetime(*timepieces).strftime("%Y-%m-%dT%H:%M:%S.%f")

def timestamp_to_isoZ(timestamp):
    '''Convert epoch timestamp to ISO8601 format with Z
    '''
    return datetime.datetime.utcfromtimestamp(float(timestamp)).isoformat() + "Z"

def usage():
    '''Print usage message
    '''
    global version

    print ("Fetch time series index information from SQLite database ({0})".format(version))
    print ()
    print ("Usage: {0} [options] sqlitefile".format(os.path.basename(sys.argv[0])))
    print ()
    print ("  -h        Print help message")
    print ("  -l file   Specify file containing list of selections")
    print ("  -t table  Specify time series index table, default: {0}".format(table))
    print ()
    print ("  -N net    Specify network code")
    print ("  -S sta    Specify station code")
    print ("  -L loc    Specify location IDs")
    print ("  -C chan   Specify channel codes")
    print ("  -s start  Specify start time in YYYY-MM-DDThh:mm:ss.ffffff format")
    print ("  -s end    Specify end time in YYYY-MM-DDThh:mm:ss.ffffff format")
    print ("  -f file   Limit results to specified filename")
    print ()
    print ("  --SYNC    Print results as SYNC listing instead of default details")
    print ()
    print ("The file containing a list of selections is expected to contain one more")
    print ("lines with a series of space-separated fields following this pattern:")
    print ()
    print ("Net  Sta  Loc  Chan  StartTime  EndTime")
    print ()
    print ("where the Net, Sta, Loc and Chan may contain '?' and '*' wildcards and")
    print ("StartTime and EndTime are either 'YYYY-MM-DDThh:mm:ss.ffffff' or '*'")
    print ()

    return

def interrupt_handler(signum, frame):
    '''Interruption signal handler.

    When triggered, interrupt database connection
    '''

    global dbconn

    if dbconn:
        print ("Termination requested (signal {0})".format(signum), file=sys.stderr)
        dbconn.interrupt()

if __name__ == "__main__":
    signal.signal(signal.SIGINT, interrupt_handler)
    signal.signal(signal.SIGTERM, interrupt_handler)

    try:
        mainthread = threading.Thread(target=main)
        mainthread.start()
    except Exception as err:
        print (err)

    while mainthread.is_alive():
       time.sleep(0.2)
