#!/usr/bin/env python
#
# fetchIndexInfo.py: Fetch time series index information.
# Reference: https://github.com/iris-edu/mseedindex/wiki
#
# Read an IRIS time series index table in an SQLite3 database and print
# readable details of the index entries, alternatively a SYNC-style
# listing can be printed.
#
# Data may be selected by network, station, location, channel and time
# range.  A selection may be specified using command line options or by
# specifying a file containing one or more independent selections.
#
# This script requires the existence of an 'all_channel_summary' table
# that is a summary of the index.  This is normally created with a
# a statement like:
#
# CREATE TABLE all_channel_summary AS
#   SELECT network,station,location,channel,
#     min(starttime) AS starttime, max(endtime) AS endtime, datetime('now') as updated
#     FROM tsindex
#     GROUP BY 1,2,3,4;
#
# Modified: 2017.072
# Written by Chad Trabant, IRIS Data Management Center

from __future__ import print_function
from collections import namedtuple
import sys
import os
import getopt
import re
import datetime
import sqlite3

version = '1.0'
verbose = 0
table = 'tsindex'

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
    sync = False

    try:
        opts, args = getopt.gnu_getopt(sys.argv[1:],
                                       "vhl:t:N:S:L:C:s:e:",
                                       ["verbose","help", "listfile=", "table=",
                                        "network=","station=","location=","channel=",
                                        "starttime=", "endtime=", "SYNC"])
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
        if not network:
            network = '*'
        if not station:
            station = '*'
        if not location:
            location = '*'
        if not channel:
            channel = '*'
        if not starttime:
            starttime = '*'
        else:
            starttime = normalize_datetime (starttime)
        if not endtime:
            endtime = '*'
        else:
            endtime = normalize_datetime (endtime)

        request.append([network,station,location,channel,starttime,endtime])

    if len(request) <= 0:
        print ("No selection specified, try -h for usage")
        sys.exit()

    if verbose >= 2:
        print ("Request:")
        for req in request:
            print ("  {0}: {1}".format(len(req), req))

    try:
        index_rows = fetch_index_rows(sqlitefile, request, table)
    except Exception as err:
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

def fetch_index_rows(sqlitefile, request, table):
    '''Fetch index rows matching specified request[]

    `sqlitefile`: SQLite3 database file
    `request`: List of tuples containing (net,sta,loc,chan,start,end)

    Request elements may contain '?' and '*' wildcards.  The start and
    end elements can be a single '*' if not a date-time string.

    Return rows as list of tuples containing:
    (network,station,location,channel,quality,starttime,endtime,samplerate,
     filename,byteoffset,bytes,hash,timeindex,timespans,timerates,
     format,filemodtime,updated,scanned)
    '''

    global verbose
    index_rows = []

    if not os.path.exists(sqlitefile):
        raise ValueError("Database not found: {0}".format(sqlitefile))

    try:
        conn = sqlite3.connect(sqlitefile, 10.0)
    except Exception as err:
        raise ValueError(str(err))

    cur = conn.cursor()

    # Create temporary table and load request
    try:
        if verbose >= 2:
            print ("Creating temporary request table")

        cur.execute("CREATE TEMPORARY TABLE request "
                    "(network TEXT, station TEXT, location TEXT, channel TEXT, "
                    "starttime TEXT, endtime TEXT) ")

        if verbose >= 2:
            print ("Populating temporary request table")

        for req in request:
            # Replace "--" location ID request alias with true empty value
            if req[2] == "--":
                req[2] = "";

            cur.execute("INSERT INTO request (network,station,location,channel,starttime,endtime) "
                        "VALUES (?,?,?,?,?,?) ", req)

        if verbose >= 2:
            print ("Request table populated")
    except Exception as err:
        raise ValueError(str(err))

    # Print request table
    if verbose >= 3:
        cur.execute ("SELECT * FROM request")
        rows = cur.fetchall()
        print ("REQUEST:")
        for row in rows:
            print ("  ", row)

    # Create temporary resolved table as a join between request and all_channel_summary to:
    # a) resolve wildcards, allows use of '=' operator and table index
    # b) reduce index table search to channels that are known included
    try:
        if verbose >= 2:
            print ("Creating temporary resolved table")

        cur.execute("CREATE TEMPORARY TABLE resolved "
                    "(network TEXT, station TEXT, location TEXT, channel TEXT, "
                    "starttime TEXT, endtime TEXT) ")

        if verbose >= 2:
            print ("Populating temporary resolved table")

        cur.execute("INSERT INTO resolved (network,station,location,channel,starttime,endtime) "
                    "SELECT s.network,s.station,s.location,s.channel,r.starttime,r.endtime "
                    "FROM all_channel_summary s, request r "
                    "WHERE "
                    "  (r.starttime='*' OR r.starttime <= s.endtime) "
                    "  AND (r.endtime='*' OR r.endtime >= s.starttime) "
                    "  AND (r.network='*' OR s.network GLOB r.network) "
                    "  AND (r.station='*' OR s.station GLOB r.station) "
                    "  AND (r.location='*' OR s.location GLOB r.location) "
                    "  AND (r.channel='*' OR s.channel GLOB r.channel) ")
    except Exception as err:
        raise ValueError(str(err))

    resolvedrows = cur.execute("SELECT COUNT(*) FROM resolved").fetchone()[0]

    # Print resolved table
    if verbose >= 3:
        cur.execute ("SELECT * FROM resolved")
        rows = cur.fetchall()
        print ("RESOLVED ({0} rows):".format(resolvedrows))
        for row in rows:
            print ("  ", row)

    # Fetch final results by joining resolved and index table
    try:
        if verbose >= 2:
            print ("Fetching index entries")

        cur.execute("SELECT ts.network,ts.station,ts.location,ts.channel,ts.quality, "
                    "ts.starttime,ts.endtime,ts.samplerate, "
                    "ts.filename,ts.byteoffset,ts.bytes,ts.hash, "
                    "ts.timeindex,ts.timespans,ts.timerates, "
                    "ts.format,ts.filemodtime,ts.updated,ts.scanned "
                    "FROM {0} ts, resolved r "
                    "WHERE "
                    "  (r.starttime='*' OR r.starttime <= ts.endtime) "
                    "  AND (r.endtime='*' OR r.endtime >= ts.starttime) "
                    "  AND ts.network = r.network "
                    "  AND ts.station = r.station "
                    "  AND ts.location = r.location "
                    "  AND ts.channel = r.channel "
                    "ORDER BY ts.network,ts.station,ts.location,ts.channel,"
                    "ts.quality,ts.starttime".format(table))
    except Exception as err:
        raise ValueError(str(err))

    index_rows = cur.fetchall()

    # Print time series index rows
    if verbose >= 3:
        print ("TSINDEX")
        for row in index_rows:
            print ("  ", row)

    cur.execute("DROP TABLE request")
    cur.execute("DROP TABLE resolved")
    conn.close()

    return index_rows

def print_info(index_rows):
    '''Print index information given a list of tuples containing:

    (0:network,1:station,2:location,3:channel,4:quality,5:starttime,6:endtime,7:samplerate,
     8:filename,9:byteoffset,10:bytes,11:hash,12:timeindex,13:timespans,14:timerates,
     15:format,16:filemodtime,17:updated,18:scanned)
    '''

    # Map tuple to a named tuple for clear referencing
    NamedRow = namedtuple ('NamedRow',
                           ['network','station','location','channel','quality',
                            'starttime','endtime','samplerate','filename',
                            'byteoffset','bytes','hash','timeindex','timespans',
                            'timerates','format','filemodtime','updated','scanned'])

    for row in index_rows:
        NRow = NamedRow(*row)
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
                time = datetime.datetime.utcfromtimestamp(float(time)).isoformat(' ')

            print ("  {0} => {1}".format(time,offset))

        # If per-span sample rates are present create a list of them
        rates = None
        if NRow.timerates:
            rates = NRow.timerates.split(',')

        # Print time spans either with or without per-span rates
        print ("Time spans:")
        for idx, span in enumerate(NRow.timespans.split(',')):
            (start,end) = span.lstrip('[').rstrip(']').split(':')
            if rates:
                print ("  {0} - {1} ({2})".format(datetime.datetime.utcfromtimestamp(float(start)).isoformat(' '),
                                               datetime.datetime.utcfromtimestamp(float(end)).isoformat(' '),
                                               rates[idx]))
            else:
                print ("  {0} - {1}".format(datetime.datetime.utcfromtimestamp(float(start)).isoformat(' '),
                                      datetime.datetime.utcfromtimestamp(float(end)).isoformat(' ')))

    return

def print_sync(index_rows):
    '''Print a SYNC listing given a list of tuples containing:

    (0:network,1:station,2:location,3:channel,4:quality,5:starttime,6:endtime,7:samplerate,
     8:filename,9:byteoffset,10:bytes,11:hash,12:timeindex,13:timespans,14:timerates,
     15:format,16:filemodtime,17:updated,18:scanned)
    '''

    # Map tuple to a named tuple for clear referencing
    NamedRow = namedtuple ('NamedRow',
                           ['network','station','location','channel','quality',
                            'starttime','endtime','samplerate','filename',
                            'byteoffset','bytes','hash','timeindex','timespans',
                            'timerates','format','filemodtime','updated','scanned'])

    for row in index_rows:
        NRow = NamedRow(*row)

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

if __name__ == "__main__":
    main()
