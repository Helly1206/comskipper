INSTALL = /usr/bin/install -c
INSTALL_DATA = ${INSTALL} -m 644
INSTALL_PROGRAM = ${INSTALL}
INSTALL_SCRIPT = ${INSTALL}
etcdir = /etc
confdir = ${etcdir}/comskip
initdir = ${etcdir}/init
servicedir = ${etcdir}/systemd/system
defaultdir = ${etcdir}/default
scriptdir = ./script
admscript = hts_skipper.py
postprocscript = hts_post_proc.py
initscript = hts-skipper.conf
servicescript = hts-skipper.service
defaultscript = hts-skipper
defaultxml = hts_skipper.xml

all: install

install: installscript

uninstall: uninstallscript

uninstallall: uninstallscript uninstallsettings

installscript:
	if [ ! -e $(confdir) ]; then mkdir $(confdir); fi
	$(INSTALL_SCRIPT) "$(scriptdir)/$(admscript)" "$(confdir)"
	$(INSTALL_SCRIPT) "$(scriptdir)/$(postprocscript)" "$(confdir)"
	if [ ! -e $(servicedir) ]; then mkdir $(servicedir); fi
	$(INSTALL_DATA) "$(scriptdir)/$(servicescript)" "$(servicedir)"
	systemctl enable "$(servicescript)"
	if [ ! -e $(defaultdir) ]; then mkdir $(defaultdir); fi
	$(INSTALL_DATA) "$(scriptdir)/$(defaultscript)" "$(defaultdir)"
	if [ ! -e $(confdir)/$(defaultxml) ]; then $(INSTALL_DATA) "$(scriptdir)/$(defaultxml)" "$(confdir)"; fi

uninstallscript:
	-test -z "$(confdir)/$(admscript)" || rm -f "$(confdir)/$(admscript)"
	-test -z "$(confdir)/$(postprocscript)" || rm -f "$(confdir)/$(postprocscript)"
	-test -z "$(initdir)/$(initscript)" || rm -f "$(initdir)/$(initscript)"
	-test -z "$(servicedir)/$(servicescript)" || rm -f "$(servicedir)/$(servicescript)"
	-test -z "$(defaultdir)/$(defaultscript)" || rm -f "$(defaultdir)/$(defaultscript)"

uninstallsettings:
	-test -z "$(confdir)/$(defaultxml)" || rm -f "$(confdir)/$(defaultxml)"

