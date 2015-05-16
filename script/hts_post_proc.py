#!/usr/bin/python

import sys
import os
import subprocess
from xml.dom import minidom
import time
import signal
import cPickle

DBFILE    = "/hts_skipper.db"
DEBUG     = False

#Status
IDLE = 0
QUEUED = 1
SKIPPING = 2
FINISHED = 3

############################

class Recording:
    Start = 0 # Real start unix time
    Stop = 0 # Real stop unix time
    Title = ''
    Status = 0

############################

class DiskDataBaseItem:
    FileName = None
    Recording = None
    PID = 0
    Status = IDLE

############################

class Settings(object):
    def __init__(self):
        self.settings = []
        self.defaults = [[u'server', u'http://localhost'], [u'userpass', u'xbmc:xbmc'], [u'maxattempts', u'10'], [u'sleepbetweenattempts', u'5'], [u'recordingpath', u'/kodi/Recorded TV'], [u'logpath', u'/var/log/comskip'], [u'comskipperlocation', u'/usr/bin/comskipper'], [u'simultaneousskippers', u'1'], [u'updateinterval', u'20'], [u'logskipperdata', u'True'], [u'logskippersettings', u'False'], [u'delete_edl', u'True'], [u'delete_log', u'True'], [u'delete_logo', u'True'], [u'delete_txt', u'False'], [u'storedb', u'True'], [u'dblocation', u'/etc/comskip']]
        self.GetSettingsHtsSkipperXml()

    def GetSettingsHtsSkipperXml(self):
        path = "./hts_skipper.xml"
        # find file and get settings
        if not os.path.isfile(path):
            path = "~/hts_skipper.xml"
            if not os.path.isfile(path):
                path = "/etc/comskip/hts_skipper.xml"
                if not os.path.isfile(path):
                    print "Settingsfile does not exist: %s" % path
    
        try:
            __xml = minidom.parse(path)
            nodes = __xml.getElementsByTagName("settings")
            if nodes:
                for node in nodes:
                    asettings=node.getElementsByTagName('setting')
                    for a in asettings:
                        self.settings.append([a.getAttribute("id"),a.getAttribute("value")])
                    del asettings
            del nodes
            __xml.unlink()
        except Exception, e:
            print "Error reading from settingsfile: %s" % path

        return

    def GetSetting(self, search):
        for a in self.settings:
            if (a[0].lower() == search.lower()):
                return a[1]

        for a in self.defaults: # if not found, search in defaults
            if (a[0].lower() == search.lower()):
                return a[1]

        return None

###########################

class Database(object):
    def __init__(self, Settings, FileName):
        self.__filename = FileName
        self.__settings = Settings
        self.DataBase = []

    def LoadDBFromDisk(self):
        if (self.__settings.GetSetting('storedb').lower()=="true"):
            if (DEBUG):
                path = "."+DBFILE
            else:
                path = self.__settings.GetSetting('dblocation')+DBFILE
            try:
                if os.path.isfile(path):
                    if (os.path.getsize(path) > 0):
                        with open(path, "rb") as input:
                            self.DataBase = cPickle.load(input)
            except Exception, e:
                print "Error reading from dbfile: %s" % (path)

    def Update(self):
        if self.CheckDaemonRunning():
            self.LoadDBFromDisk()
            #self.PrintDB()
            dbitem = self.FindComSkipper()
            # Check for dbitem containing filename, if not found try by PID
            # if filename == None:
            #    3: Try to find skipper by PID
            #    2: get number of skippers and wait all finished
            # if filename not found, exit
            # if filename found:
            #     Check skipper finished
            #     If finished --> exit
            #     If queued and all skippers in use --> wait
            #     If queued and not all skippers in use --> exit
            return self.CheckComskipperFinished(dbitem)
        else:
            print "Daemon not running (anymore), exit"
            return True

    def CheckDaemonRunning(self):
        _syscmd = subprocess.Popen(['ps aux | grep -v grep | grep %s | awk \'{print $2}\'' % "hts_skipper.py"], shell=True, stdout=subprocess.PIPE)
        PID = _syscmd.stdout.read().strip().split()
        return (len(PID) > 0)

    def FindComSkipper(self):
        dbitem = None
        
        if (self.__filename == None):
            dbitem = self.FindComSkipperByPID()
        else:
            dbitem = self.FindComSkipperByName()
            if (dbitem == None):
                dbitem = self.FindComSkipperByPID()

        return dbitem

    def FindComSkipperByName(self):
        for item in self.DataBase:
            if (item.FileName == self.__filename):
                return item
        return None

    def FindComSkipperByPID(self):
        PIDs = self.GetComSkipperPIDs()
        # try to find the first PID that is in the database
        for item in self.DataBase:
            for PID in PIDs:
                if (int(PID) == item.PID):
                    del PIDs
                    return item
        del PIDs
        return None

    def GetComSkipperPIDs(self):
        _syscmd = subprocess.Popen(['pidof', self.__settings.GetSetting("comskipperlocation")], stdout=subprocess.PIPE)
        return _syscmd.stdout.read().strip().split()

    def CountSkippers(self):
        Skippers = 0

        for item in self.DataBase:
            if item.Status == SKIPPING:
                Skippers += 1

        return Skippers

    def CheckComskipperFinished(self, dbitem):
        ExitProg = False
        if (dbitem == None): 
            if (self.__filename == None): #Wait for all skippers finished
                nPIDs = len(self.GetComSkipperPIDs())
                if (nPIDs > 0):
                    print "No Item found: wait until %d ComSkippers are finished" % (nPIDs)
                    ExitProg = False
                else:
                    print "No Item found, no ComSkippers either, exit"
                    ExitProg = True
            else: #Filename not found, exit
                print "No Item found by filename and PID, exit"
                ExitProg = True    
        else: #we do have an item to check
            if (dbitem.Status == SKIPPING):
                 print "ComSkipper still busy, wait until finished"
                 ExitProg = False # wait till finished
            elif (dbitem.Status == FINISHED):
                 print "ComSkipper finished, exit"
                 ExitProg = True
            else: # (dbitem.Status == QUEUED):
                Skippers = self.CountSkippers()
                if (Skippers >= int(self.__settings.GetSetting("simultaneousskippers"))):
                    print "ComSkipper Queued and all ComSkippers in use, wait until finished"
                    ExitProg = False
                else:
                    print "ComSkipper Queued but not all ComSkippers in use, exit"
                    ExitProg = True
        return ExitProg

    def PrintDB(self):
        item = 0
        for dbitem in self.DataBase:
            print "DB: Item %d" % item
            print "DB:     Status %d" % dbitem.Status
            print "DB:     PID %d" % dbitem.PID
            print "DB:     FileName %s" % dbitem.FileName
            print "DB:     Recording: Start %d" % dbitem.Recording.Start
            print "DB:     Recording: Stop  %d" % dbitem.Recording.Stop
            print "DB:     Recording: Title %s" % dbitem.Recording.Title
            print "DB:     Recording: Status %d" % dbitem.Recording.Status
            item += 1

#########################################################
# Main                                                  #
#########################################################
### 

Running = True

def sigterm_handler(signum, frame):
    global Running
    Running = False

### MAIN ###

signal.signal(signal.SIGTERM, sigterm_handler)
signal.signal(signal.SIGINT, sigterm_handler)

#Read settings file, no logger location yet, refer to defaults if fails
Settings = Settings()

print "Comskipper HTS Post Processor Started ..."

if len(sys.argv) > 1:
    FileName = sys.argv[1]
else:
    FileName = None

#init Database
DB = Database(Settings, FileName)

looptime = 1
Stop = False
while (Running and not Stop):
    time.sleep(1)
    if looptime <= 1:  
        Stop = DB.Update()
        looptime = int(Settings.GetSetting('updateinterval'))
    else:
        looptime -= 1

del DB
del Settings

print "Comskipper HTS Post Processor Ready ..."

