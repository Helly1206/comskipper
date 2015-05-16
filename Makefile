CSPATH=../comskip-0.93i
bin_PROGRAMS = comskip$(EXEEXT)
comskip_SOURCES = mpeg2dec.cpp comskip.cpp CsEdges.cpp BlackFrame.cpp ProcessCsv.cpp ProcessArInfo.cpp LoadSettings.cpp BlackFrame.h CcPacket.h CsEdges.h CsExceptions.h CsOptions.h CsTypes.h comskip.h getopt.h gettimeofday.h inttypes.h resource.h
COMSKIPPER = comskipper
comskip_SOURCES := $(COMSKIPPER).cpp $(comskip_SOURCES)
OBJCOPY=objcopy
EXECUTABLE=comskipper
EXEEXT = 
OBJEXT = o
bin_PROGRAMS=$(EXECUTABLE)$(EXEEXT)

EXCLUDEOBJ = comskip-mpeg2dec.$(OBJEXT)
MPEG2DECSRC = mpeg2dec.cpp

CXX = g++
DEFS = -DHAVE_CONFIG_H
DEFAULT_INCLUDES = -I.
CPPFLAGS = 
comskip_CPPFLAGS = -I/home/helly/projects/ComSkipWork/install/include   -I/home/helly/projects/ComSkipWork/install/include   -I/home/helly/projects/ComSkipWork/install/include    
comskip_CPPFLAGS := $(comskip_CPPFLAGS) -I$(CSPATH)
CXXFLAGS = -g -O2 -std=gnu++11
DEPDIR = .deps
DEPDIR = $CSPATH/$DEPDIR
am__mv = mv -f
CXXCOMPILE = $(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS)
comskipper_SOURCEFILES = $(COMSKIPPER).cpp

OBJEXT = o
am_comskip_OBJECTS = comskip-mpeg2dec.$(OBJEXT) comskip-comskip.$(OBJEXT) comskip-CsEdges.$(OBJEXT) comskip-BlackFrame.$(OBJEXT) comskip-ProcessCsv.$(OBJEXT) comskip-ProcessArInfo.$(OBJEXT) comskip-LoadSettings.$(OBJEXT)
comskip_OBJECTS = $(am_comskip_OBJECTS)
comskipper_OBJECTS = $(filter-out $(EXCLUDEOBJ),$(comskip_OBJECTS))
comskipper_OBJECTS := $(COMSKIPPER).$(OBJEXT) $(comskipper_OBJECTS)
comskip_DEPENDENCIES = 
CXXLD = $(CXX)
LDFLAGS = 
CXXLINK = $(CXXLD) $(AM_CXXFLAGS) $(CXXFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@
comskip_LDADD = -pthread -L/home/helly/projects/ComSkipWork/install/lib -lavcodec -lxcb-shm -lxcb -lX11 -lcrystalhd -lz -lswresample -lavutil -lm   -pthread -L/home/helly/projects/ComSkipWork/install/lib -lavformat -lavcodec -lxcb-shm -lxcb -lX11 -lcrystalhd -lz -lswresample -lavutil -lm   -L/home/helly/projects/ComSkipWork/install/lib -lavutil -lm   -largtable2  
LIBS = 

prefix = /usr
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
INSTALL = /usr/bin/install -c
INSTALL_DATA = ${INSTALL} -m 644
INSTALL_PROGRAM = ${INSTALL}
INSTALL_SCRIPT = ${INSTALL}
etcdir = /etc
confdir = ${etcdir}/comskip
initdir = ${etcdir}/init
defaultdir = ${etcdir}/default
scriptdir = ./script
admscript = hts_skipper.py
postprocscript = hts_post_proc.py
initscript = hts-skipper.conf
defaultscript = hts-skipper
defaultxml = hts_skipper.xml

all: clean executable

objects:
	@for object in $(comskip_OBJECTS); do \
		if [ ! -e $(CSPATH)/$$object ]; then echo "File doesnt exist, recompile comskip: $(CSPATH)/$$object"; exit 1; fi; \
		if [ "$$object" = "$(EXCLUDEOBJ)" ]; then \
			echo "Excluded Object file: $(CSPATH)/$$object"; \
		else \
			$(OBJCOPY) $(CSPATH)/$$object $$object; \
			echo "Object file copied: $(CSPATH)/$$object"; \
		fi; \
	done

csmain:
	@if [ ! -e $(CSPATH)/$(MPEG2DECSRC) ]; then echo "File doesnt exist: $(CSPATH)/$(MPEG2DECSRC)"; exit 1; fi
	@cp $(CSPATH)/$(MPEG2DECSRC) $(MPEG2DECSRC)
	@./comment_c.py $(MPEG2DECSRC) "namespace CS {\n\nvoid FramesPerSecond::print(bool final)"
	@./comment_c.py $(MPEG2DECSRC) "int main (int argc, char **argv)\n{"
	@echo "File copied and modified: $(CSPATH)/$(MPEG2DECSRC)" 

source: $(comskipper_SOURCEFILES)
	@for object in $(comskipper_SOURCEFILES); do \
		$(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(comskip_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c $$object; \
		echo "$(CXX) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(comskip_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c $$object "; \
	done

executable: csmain source objects $(comskip_DEPENDENCIES) $(EXTRA_comskip_DEPENDENCIES)
	@rm -f $(EXECUTABLE)$(EXEEXT)
	$(CXXLINK) -o $(EXECUTABLE)$(EXEEXT) $(comskipper_OBJECTS) $(comskip_LDADD) $(LIBS)

clean:
	-test -z "$(MPEG2DECSRC)" || rm -f $(MPEG2DECSRC)
	-test -z "$(bin_PROGRAMS)" || rm -f $(bin_PROGRAMS)
	-test -z "$(comskipper_OBJECTS)" || rm -f $(comskipper_OBJECTS)

install: installprogram installscript

uninstall: uninstallprogram uninstallscript

uninstallall: uninstallprogram uninstallscript uninstallsettings

installprogram:
	if [ ! -e $(bindir) ]; then mkdir $(bindir); fi
	$(INSTALL_PROGRAM) "$(EXECUTABLE)$(EXEEXT)" "$(bindir)"

uninstallprogram:
	-test -z "$(bindir)/$(EXECUTABLE)$(EXEEXT)" || rm -f "$(bindir)/$(EXECUTABLE)$(EXEEXT)"

installscript:
	if [ ! -e $(confdir) ]; then mkdir $(confdir); fi
	$(INSTALL_SCRIPT) "$(scriptdir)/$(admscript)" "$(confdir)"
	$(INSTALL_SCRIPT) "$(scriptdir)/$(postprocscript)" "$(confdir)"
	if [ ! -e $(initdir) ]; then mkdir $(initdir); fi
	$(INSTALL_DATA) "$(scriptdir)/$(initscript)" "$(initdir)"
	if [ ! -e $(defaultdir) ]; then mkdir $(defaultdir); fi
	$(INSTALL_DATA) "$(scriptdir)/$(defaultscript)" "$(defaultdir)"
	if [ ! -e $(confdir)/$(defaultxml) ]; then $(INSTALL_DATA) "$(scriptdir)/$(defaultxml)" "$(confdir)"; fi

uninstallscript:
	-test -z "$(confdir)/$(admscript)" || rm -f "$(confdir)/$(admscript)"
	-test -z "$(confdir)/$(postprocscript)" || rm -f "$(confdir)/$(postprocscript)"
	-test -z "$(initdir)/$(initscript)" || rm -f "$(initdir)/$(initscript)"
	-test -z "$(defaultdir)/$(defaultscript)" || rm -f "$(defaultdir)/$(defaultscript)"

uninstallsettings:
	-test -z "$(confdir)/$(defaultxml)" || rm -f "$(confdir)/$(defaultxml)"

