#!/usr/bin/python

import os
import subprocess
import threading
import urllib2
from xml.dom import minidom
import time
import logging
from logging.handlers import RotatingFileHandler
import signal
import cPickle
try:
    from subprocess import DEVNULL # py3k
except ImportError:
    DEVNULL = open(os.devnull, 'wb')

TVHPORT = ':9981/status.xml'
MKV_EXT = ".mkv"
TS_EXT = ".ts"
#VIDEOEXT  = ".mkv|.ts"
VIDEOEXT2 = [MKV_EXT, TS_EXT] #VIDEOEXT.decode('utf-8').split('|')
EDL_EXT = ".edl"
LOG_EXT = ".log"
LOGO_EXT = ".logo.txt"
TXT_EXT = ".txt"
COMSKIPEXT2 = [EDL_EXT,LOG_EXT,LOGO_EXT,TXT_EXT]
DBFILE    = "/hts_skipper.db"
DEBUGFILE = "./test2.xml"
DEBUG     = False

#Status
IDLE = 0
QUEUED = 1
SKIPPING = 2
FINISHED = 3

RECORDING = 99

############################

class Recording:
    Start = 0 # Real start unix time
    Stop = 0 # Real stop unix time
    Title = ''
    Status = 0

############################

class DataBaseItem:
    FileName = None
    Recording = None
    PID = 0
    Skipper = None
    Status = IDLE

############################

class DiskDataBaseItem:
    FileName = None
    Recording = None
    PID = 0
    Status = IDLE

############################

class logger(threading.Thread):
    def __init__(self, Settings):
        threading.Thread.__init__(self)
        self.daemon = False
        self.fdRead, self.fdWrite = os.pipe()
        self.pipeReader = os.fdopen(self.fdRead)
        self.logger = None
        self.InitLogger(Settings)
        self.daemon = True
        self.start()

    # wrap over original logging.logger attributes
    def __getattr__(self,attr):
        orig_attr = self.logger.__getattribute__(attr)
        if callable(orig_attr):
            def hooked(*args, **kwargs):
                result = orig_attr(*args, **kwargs)
                if result == self.logger:
                    return self
                return result
            return hooked
        else:
            return orig_attr

    def InitLogger(self,Settings):
        self.logger = logging.getLogger("hts_skipper")
        if (DEBUG):
            handler=logging.handlers.RotatingFileHandler("./hts_skipper.log", maxBytes=200000, backupCount=3)
        else:
            handler=logging.handlers.RotatingFileHandler(Settings.GetSetting("logpath"), maxBytes=200000, backupCount=3)
        formatter = logging.Formatter('%(asctime)s %(levelname)s:%(message)s')
        handler.setFormatter(formatter)
        self.logger.addHandler(handler)
        self.logger.setLevel(logging.DEBUG)

    def fileno(self):
        return self.fdWrite

    def run(self):
        for line in iter(self.pipeReader.readline, ''):
            self.logger.info(line.strip('\n'))

        self.pipeReader.close()

    def close(self):
        os.close(self.fdWrite)

############################

class Settings(object):
    def __init__(self):
        self.settings = []
        self.defaults = [[u'server', u'http://localhost'], [u'userpass', u'xbmc:xbmc'], [u'maxattempts', u'10'], [u'sleepbetweenattempts', u'5'], [u'recordingpath', u'/kodi/Recorded TV'], [u'logpath', u'/var/log/comskip'], [u'comskiplocation', u'/usr/local/bin/comskip'],[u'inilocation', u''], [u'simultaneousskippers', u'1'], [u'updateinterval', u'20'], [u'logskipperdata', u'True'], [u'logskippersettings', u'False'], [u'delete_edl', u'True'], [u'delete_log', u'True'], [u'delete_logo', u'True'], [u'delete_txt', u'False'], [u'storedb', u'True'], [u'dblocation', u'/etc/comskip']]
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

    def GetUserPassword(self, userpass):
        return userpass.split(':', 1);

###########################

class ComSkipper(object):
    def __init__(self, Settings, logger):
        self.__settings = Settings
        self.__logger = logger
        self.p = None

    def Start(self, filename, endtime):
        if (self.__settings.GetSetting("logskippersettings").lower()=="true"):
            _stdout=logger
        else:
            _stdout=DEVNULL
        if (self.__settings.GetSetting("logskipperdata").lower()=="true"):
            _stderr=logger
        else:
            _stderr=DEVNULL
        if self.__settings.GetSetting("inilocation") == '':
        	process = [self.__settings.GetSetting("comskiplocation"),filename]
        else:
        	process = [self.__settings.GetSetting("comskiplocation"),"--ini=%s"%(self.__settings.GetSetting("inilocation")),filename]
        self.p = subprocess.Popen(process, stdout=_stdout, stderr=_stderr)

    def Busy(self):
        return (self.p.poll() <= -15)
    
    def GetPID(self):
        return (self.p.pid)

    def Kill(self):
	if (self.Busy()):
            self.p.kill()

###########################

class HTS(object):
    def __init__(self, Settings, logger):
        self.__settings = Settings
        self.__logger = logger
        self.__conn_established = None
        #self.__xml = None
        self.__maxattempts = 10
        self.establishConn()

    def establishConn(self):
        if (DEBUG):
            self.__conn_established = True
            self.__logger.info('Connection to %s established' % 'DEBUG')
        else:
            self.__conn_established = False
            self.__maxattempts = int(Settings.GetSetting('maxattempts'))
            while self.__maxattempts > 0:
                try:
                    pwd_mgr = urllib2.HTTPPasswordMgrWithDefaultRealm()
                    upass=self.__settings.GetUserPassword(self.__settings.GetSetting('userpass'))
                    pwd_mgr.add_password(None, self.__settings.GetSetting('server') + TVHPORT, upass[0], upass[1])
                    handle = urllib2.HTTPBasicAuthHandler(pwd_mgr)
                    opener = urllib2.build_opener(handle)
                    opener.open(self.__settings.GetSetting('server') + TVHPORT)
                    urllib2.install_opener(opener)
                    self.__conn_established = True
                    self.__logger.info('Connection to %s established' % (self.__settings.GetSetting('server')))
                    break
                except Exception, e:
                    print('%s' % (e))
                    self.__maxattempts -= 1
                    self.__logger.warning('Remaining connection attempts to %s: %s' % (self.__settings.GetSetting('server'), self.__maxattempts))
                    time.sleep(5)
                    continue

        if not self.__conn_established:
            self.__logger.error("Error establishing connection")
            time.sleep(6)

    def readXMLbyTag(self, xmlnode):
        nodedata = []
        while self.__conn_established:
            try:
                if (DEBUG):
                    __f = open(DEBUGFILE,"r") #, timeout=mytimeout
                else:
                    __f = urllib2.urlopen(self.__settings.GetSetting('server') + TVHPORT) #, timeout=mytimeout
                __xmlfile = __f.read()
                __xml = minidom.parseString(__xmlfile)
                __f.close()
                nodes = __xml.getElementsByTagName(xmlnode)
                if nodes:
                    for node in nodes:
                        nodedata.append(node.childNodes[0].data)
                    del nodes
                break
            except Exception, e:
                self.__logger.error("Could not read from %s" % (self.__settings.GetSetting('server')))
                time.sleep(5)
                self.establishConn()
        return nodedata

    def readXMLRecordings(self):
        Recordings = []
        while self.__conn_established:
            try:
                if (DEBUG):
                    __f = open(DEBUGFILE,"r") #, timeout=mytimeout
                else:
                    __f = urllib2.urlopen(self.__settings.GetSetting('server') + TVHPORT) #, timeout=mytimeout
                __xmlfile = __f.read()
                __xml = minidom.parseString(__xmlfile)
                __f.close()
                nodes = __xml.getElementsByTagName('recording')
                if nodes:
                    for node in nodes:
                        Rec = Recording()
                        start=node.getElementsByTagName('start')[0]
                        unixtime=start.getElementsByTagName('unixtime')[0]
                        extra=start.getElementsByTagName('extra_start')[0]
                        Rec.Start=int(unixtime.firstChild.data)-(int(extra.firstChild.data)*60)
                        stop=node.getElementsByTagName('stop')[0]
                        unixtime=stop.getElementsByTagName('unixtime')[0]
                        extra=stop.getElementsByTagName('extra_stop')[0]
                        Rec.Stop=int(unixtime.firstChild.data)+(int(extra.firstChild.data)*60)
                        Rec.Title=node.getElementsByTagName('title')[0].firstChild.data
                        Rec.Status=RECORDING if (node.getElementsByTagName('status')[0].firstChild.data=="Recording") else IDLE
                        Recordings.append(Rec)
                    del nodes
                __xml.unlink()
                break
            except Exception, e:
                self.__logger.error("Could not read from %s" % (self.__settings.GetSetting('server')))
                time.sleep(5)
                self.establishConn()
        return Recordings

###########################

class Database(object):
    def __init__(self, Settings, logger):
        self.__settings = Settings
        self.__logger = logger
        self.DataBase = []
        self.InitDBFromDisk()

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
                            self.DiskDB2DB(cPickle.load(input))
            except Exception, e:
                self.__logger.error("Error reading from dbfile: %s" % (path))

    def SaveDBToDisk(self):
        if (self.__settings.GetSetting('storedb').lower()=="true"):
            if (DEBUG):
                path1 = "."
            else:
                path1 = self.__settings.GetSetting('dblocation')
            path = path1 + DBFILE
            try:
                if os.path.isdir(path1):
                    DiskDB = self.DB2DiskDB()
                    with open(path, "wb") as output:
                        cPickle.dump(DiskDB, output, cPickle.HIGHEST_PROTOCOL)
                    del DiskDB
            except Exception, e:
                self.__logger.error("Error writing to dbfile: %s" % (path))

    def DB2DiskDB(self):
        DiskDB = []
        for dbitem in self.DataBase:
            ddbitem=DiskDataBaseItem()
            ddbitem.FileName = dbitem.FileName
            ddbitem.Recording = dbitem.Recording
            ddbitem.PID = dbitem.PID
            ddbitem.Status = dbitem.Status
            DiskDB.append(ddbitem)
        return DiskDB

    def DiskDB2DB(self, DiskDB):
        del self.DataBase
        self.DataBase = []
        for ddbitem in DiskDB:
            dbitem=DataBaseItem()
            dbitem.FileName = ddbitem.FileName
            dbitem.Recording = ddbitem.Recording
            dbitem.PID = ddbitem.PID
            dbitem.Status = ddbitem.Status
            self.DataBase.append(dbitem)

    def InitDBFromDisk(self):
        self.LoadDBFromDisk()
        #self.PrintDB()
        # check for finished items and remove them
        changeditems=0
        CheckDB = True
        while (CheckDB):
            CheckDB = False
            for dbitem in self.DataBase:
                if (dbitem.Status == QUEUED):
                    #Check beyond endtime and no filename
                    if (dbitem.FileName == None):
                        dbitem.FileName = self.GetRecordingName(dbitem.Recording.Title)
                    elif not os.path.isfile(dbitem.FileName):
                        #File deleted
                        dbitem.FileName = None
                    if (dbitem.FileName == None):
                        curtime=int(time.time())
                        if (curtime > dbitem.Recording.Stop):
                            #Recording finished and still no filename, delete from database
                            changeditems+=1
                            self.__logger.info("DB: Init - %s queued, but no file found beyond finish time" % dbitem.Recording.Title)
                            self.__logger.info("DB: Init - %s recording probably failed, so removing it from db" % dbitem.Recording.Title)
                            if dbitem.Skipper != None:
                                del dbitem.Skipper
                            if dbitem.Recording != None:
                                del dbitem.Recording
                            dbitem.Skipper = None
                            dbitem.Recording = None
                            self.DataBase.remove(dbitem)
                            CheckDB = True
                        else:
                            self.__logger.info("DB: Init - %s queued, but no file to skip found (yet)" % dbitem.Recording.Title)
                    else:
                        self.__logger.info("DB: Init - %s queued, re-queue to restart" % dbitem.Recording.Title)
                if (dbitem.Status == FINISHED):
                    changeditems+=1
                    self.__logger.info("DB: Init - %s finished, so removing it from db" % dbitem.Recording.Title)
                    if dbitem.Skipper != None:
                        del dbitem.Skipper
                    if dbitem.Recording != None:
                        del dbitem.Recording
                    dbitem.Skipper = None
                    dbitem.Recording = None
                    self.DataBase.remove(dbitem)
                    CheckDB = True
                    break;
                if (dbitem.Status == SKIPPING):
                    changeditems+=1
                    self.__logger.info("DB: Init - %s was skipping during shutdown, queue to restart" % dbitem.Recording.Title)
                    dbitem.Status = QUEUED;
        if (changeditems>0):
            self.SaveDBToDisk()
        #self.PrintDB()

    def CleanupDeletedRecordings(self):
        files = []
        files = self.GetFiles(self.__settings.GetSetting('recordingpath'), "", files, self.IsComSkipFile)
        prevnumber = 0
        for csfile in files:
            name, ext = os.path.splitext(csfile)
            fexists = False
            for ext in VIDEOEXT2:
                destination = name + ext
                if os.path.isfile(destination):
                    fexists = True
            if not fexists:
                self.__logger.info("DB: Cleanup - no video file found for %s, so removing this file" % csfile)
                os.remove(csfile)

    def Update(self, Recordings):
        # Check database for new entry (s)     
        # Check database for finished entry (s) --> I Think we do not need this one, only add new items, don't do anything if finished
        # Check comskipper finished and delete required files
        # Check start of new comskipper
        newitems = self.CheckForNewItems(Recordings)
        finishedskippers = self.CheckComskipperFinished()
        startskippers = self.CheckStartComskipper()
        if (newitems + finishedskippers + startskippers > 0): #Database has changed, so save it
            self.SaveDBToDisk()
        for rec in Recordings:
            del rec
        del Recordings
        return

    def CheckForNewItems(self, Recordings):
        newitems = 0
        for rec in Recordings:
            newitem = True
            for dbitem in self.DataBase:
                if self.CompareRecording(dbitem.Recording, rec):
                    newitem = False
            if newitem:
		self.__logger.info("DB: Recording - %s started, added to db" % rec.Title)
                self.AddItem(rec, QUEUED)
                newitems += 1
        return newitems

    def CheckForFinishedItems(self, Recordings):
        finisheditems = 0
        for dbitem in self.DataBase:
            finisheditem = True
            for rec in Recordings:
                if self.CompareRecording(dbitem.Recording, rec):
                    finisheditem = False
            if finisheditem:
                self.__logger.info("DB: Recording - %s finished" % rec.Title)
                finisheditems += 1
        return finisheditems

    def CheckComskipperFinished(self):
        readyitems = 0
        for dbitem in self.DataBase:
            if (dbitem.Status == SKIPPING):
                if (dbitem.Skipper == None):
                    self.__logger.error("DB: Lost Skipper information - %s, set to finished" % dbitem.Recording.Title)
                    dbitem.Status = FINISHED
                    readyitems += 1
                elif not dbitem.Skipper.Busy():
                    self.__logger.info("DB: Skipping - %s finished" % dbitem.Recording.Title)
                    dbitem.Status = FINISHED
                    dbitem.PID=0
                    self.DeleteUnnecessaryFiles(dbitem)
                    if dbitem.Skipper != None:
                        del dbitem.Skipper
                    dbitem.Skipper = None
                    readyitems += 1
        return readyitems

    def DeleteUnnecessaryFiles(self, dbitem):
        if (dbitem.FileName != None):
            name, ext = os.path.splitext(dbitem.FileName)
            try:
                if (self.__settings.GetSetting('delete_edl').lower()=="true"):
                    destination = name + EDL_EXT
                    if os.path.isfile(destination):
                        os.remove(destination)
                if (self.__settings.GetSetting('delete_log').lower()=="true"):
                    destination = name + LOG_EXT
                    if os.path.isfile(destination):
                        os.remove(destination)
                if (self.__settings.GetSetting('delete_logo').lower()=="true"):
                    destination = name + LOGO_EXT
                    if os.path.isfile(destination):
                        os.remove(destination)
                if (self.__settings.GetSetting('delete_txt').lower()=="true"):
                    destination = name + TXT_EXT
                    if os.path.isfile(destination):
                        os.remove(destination)
            except IOError, e:
                self.__logger.error("DB: IOError Removing file - %s" % destination)
        return

    def IsVideoFile(self, path, title):
        head, tail = os.path.split(path)
        title2=title.replace(' ','-')
        if (title.lower() in tail.lower()) or (title2.lower() in tail.lower()):
            name, ext = os.path.splitext(tail)
            return ext.lower() in VIDEOEXT2
        return False

    def IsComSkipFile(self, path, title):
        head, tail = os.path.split(path)
        name, ext = os.path.splitext(tail)
        return ext.lower() in COMSKIPEXT2

    def GetFiles(self, folder, title, files, TestFunction):
        if os.path.isdir(folder):
            for item in os.listdir(folder):
                itempath = os.path.join(folder, item)
                if os.path.isfile(itempath):
                    if TestFunction(itempath, title):
                        files.append(itempath)
                elif os.path.isdir(itempath):
                    files = self.GetFiles(itempath, title)
        return files

    def GetRecordingName(self, Title):
        recordingname = None
        files = []
        files = self.GetFiles(self.__settings.GetSetting('recordingpath'), Title, files, self.IsVideoFile)
        prevnumber = 0
        for vfile in files:
            name, ext = os.path.splitext(vfile)
            k = name.rfind("-")
            try:
                number = int(name[k+1:])
            except:
                number = 0
            if (number >= prevnumber):
                recordingname = vfile
            prevnumber = number               
        del files
        return recordingname

    def CheckStartComskipper(self):
        startitems = 0
        nskippers = 0
        maxskippers = int(self.__settings.GetSetting('simultaneousskippers'))
        for dbitem in self.DataBase:
            if (dbitem.Status == SKIPPING):
                nskippers += 1
            elif (dbitem.Status == QUEUED):
                if dbitem.FileName == None:
                    dbitem.FileName = self.GetRecordingName(dbitem.Recording.Title)
        if (nskippers < maxskippers):
            for dbitem in self.DataBase:
                if (dbitem.Status == QUEUED):
                    dbitem.Skipper = ComSkipper(self.__settings, self.__logger)
                    if dbitem.FileName != None:                    
                        dbitem.Skipper.Start(dbitem.FileName, dbitem.Recording.Stop)
                        self.__logger.info("DB: Skipping - %s started" % dbitem.Recording.Title)
                        dbitem.Status = SKIPPING
                        dbitem.PID = dbitem.Skipper.GetPID()
                        nskippers += 1
                        startitems += 1
                    else:
                        curtime=int(time.time())
                        if (curtime > dbitem.Recording.Stop):
                            #Recording finished and still no filename, delete from database
                            startitems += 1
                            self.__logger.info("DB: Recording - %s started, but no file found beyond finish time" % dbitem.Recording.Title)
                            self.__logger.info("DB: Recording - %s recording probably failed, so removing it from db" % dbitem.Recording.Title)
                            if dbitem.Skipper != None:
                                del dbitem.Skipper
                            if dbitem.Recording != None:
                                del dbitem.Recording
                            dbitem.Skipper = None
                            dbitem.Recording = None
                            self.DataBase.remove(dbitem)
                        else:
                            self.__logger.info("DB: Recording - %s started, but no file to skip found (yet)" % dbitem.Recording.Title)
                    if (nskippers >= maxskippers):
                        break
        return startitems

    def CompareRecording(self, rec1, rec2):
         return True if (rec1.Start == rec2.Start) and (rec1.Stop == rec2.Stop) and (rec1.Title == rec2.Title) else False

    def AddItem(self, Recording, Status = IDLE):
        Item = DataBaseItem()
        Item.Recording = Recording
        Item.Status = Status
        self.DataBase.append(Item)
        return len(self.DataBase)-1

    def PrintDB(self):
        item = 0
        for dbitem in self.DataBase:
            self.__logger.info("DB: Item %d" % item)
            self.__logger.info("DB:     Status %d" % dbitem.Status)
            self.__logger.info("DB:     PID %d" % dbitem.PID)
            self.__logger.info("DB:     Skipper %s" % dbitem.Skipper)
            self.__logger.info("DB:     Recording: Start %d" % dbitem.Recording.Start)
            self.__logger.info("DB:     Recording: Stop  %d" % dbitem.Recording.Stop)
            self.__logger.info("DB:     Recording: Title %s" % dbitem.Recording.Title)
            self.__logger.info("DB:     Recording: Status %d" % dbitem.Recording.Status)
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

#init logger
logger=logger(Settings)
logger.info("Started ...")

#init HTS
TVH = HTS(Settings, logger)

#init Database
DB = Database(Settings, logger)

#cleanup deleted recordings 
DB.CleanupDeletedRecordings()

looptime = 1
while (Running):
    time.sleep(1)
    if looptime <= 1:
        # Check recordings
        Recordings = TVH.readXMLRecordings()
        if (DEBUG):
            for rec in Recordings:
                logger.info("start:%d, stop:%d, title:%s, status:%d" % (rec.Start, rec.Stop, rec.Title, rec.Status))        
        DB.Update(Recordings)
        looptime = int(Settings.GetSetting('updateinterval'))
    else:
        looptime -= 1

del DB
del TVH
del Settings

logger.info("Ready ...");

logger.close()
del logger
