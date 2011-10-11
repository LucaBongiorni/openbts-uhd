/*
* Copyright 2008, 2009, 2010 Free Software Foundation, Inc.
*
* This software is distributed under the terms of the GNU Affero Public License.
* See the COPYING file in the main directory for details.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <iostream>
#include <fstream>

#include <TRXManager.h>
#include <GSML1FEC.h>
#include <GSMConfig.h>
#include <GSMSAPMux.h>
#include <GSML3RRMessages.h>
#include <GSMLogicalChannel.h>

#include <SIPInterface.h>
#include <Globals.h>

#include <Logger.h>
#include <CLI.h>
#include <CLIServer.h>
#include <CLIParser.h>
#include <PowerManager.h>
#include <RRLPQueryController.h>
#include <Configuration.h>

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>

using namespace std;
using namespace GSM;
using namespace CommandLine;

static int daemonize(std::string &lockfile, int &lfp);
static int forkLoop();

class DaemonInitializer
{
public:
	DaemonInitializer(bool doDaemonize)
	: mLockFileFD(-1)
	{
		// Start in daemon mode?
		if (doDaemonize)
			if (daemonize(mLockFileName, mLockFileFD) != EXIT_SUCCESS)
				exit(EXIT_FAILURE);
	}

	~DaemonInitializer()
	{
		if (mLockFileFD >= 0) close(mLockFileFD);
		if	(mLockFileName.size() > 0) {
			if (unlink(mLockFileName.data()) == 0) {
				LOG(INFO) << "Deleted lock file " << mLockFileName;
			} else {
				LOG(INFO) << "Error while deleting lock file " << mLockFileName
				          << " code=" << errno << ": " << strerror(errno);
			}
		}
	}

protected:
	std::string mLockFileName;
	int mLockFileFD;
};

class Restarter
{
public:
	Restarter(bool restartOnCrash)
	{
		if (restartOnCrash)
			if (forkLoop() != EXIT_SUCCESS)
				exit(EXIT_FAILURE);
	}
};

/// Load configuration from a file.
ConfigurationTable gConfig("OpenBTS.config");
/// Initialize Logger form the config.
static LogInitializer sgLogInitializer;
/// Fork daemon if needed.
static DaemonInitializer sgDaemonInitializer(gConfig.defines("Server.Daemonize"));
/// Fork a child and restart it if it crash. Kind of failsafe.
static Restarter sgRestarter(gConfig.defines("Server.RestartOnCrash"));


// All of the other globals that rely on the global configuration file need to
// be declared here.

/// The global SIPInterface object.
SIP::SIPInterface gSIPInterface;

/// Configure the BTS object based on the config file.
/// So don't create this until AFTER loading the config file.
GSMConfig gBTS;

/// Our interface to the software-defined radio.
TransceiverManager gTRX(1, gConfig.getStr("TRX.IP"), gConfig.getNum("TRX.Port"));

/// Pointer to the server socket if we run remote CLI.
static ConnectionServerSocket *sgCLIServerSock = NULL;

/// We store Transceiver PID if we start it.
static pid_t sgTransceiverPid = 0;
static int sgTransceiverPidFileFd = -1;
static std::string sgTransceiverPidFile;

/** Function to shutdown the process when something wrong happens. */
void shutdownOpenbts()
{
	kill(SIGTERM, getpid());
}

static int openPidFile(const std::string &lockfile)
{
	int lfp = open(lockfile.data(), O_RDWR|O_CREAT, 0640);
	if (lfp < 0) {
		LOG(ERROR) << "Unable to create PID file " << lockfile << ", code="
		           << errno << " (" << strerror(errno) << ")";
	} else {
		LOG(INFO) << "Created PID file " << lockfile;
	}
	return lfp;
}

static int lockPidFile(const std::string &lockfile, int lfp, bool block=false)
{
	if (lockf(lfp, block?F_LOCK:F_TLOCK,0) < 0) {
		LOG(ERROR) << "Unable to lock PID file " << lockfile << ", code="
		           << errno << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int writePidFile(const std::string &lockfile, int lfp, int pid)
{
	// Clear old file content first
	if (ftruncate(lfp, 0) < 0) {
		LOG(ERROR) << "Unable to clear PID file " << lockfile << ", code="
		           << errno << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}

	// Write PID
	char tempBuf[64];
	snprintf(tempBuf, sizeof(tempBuf), "%d\n", pid);
	ssize_t tempDataLen = strlen(tempBuf);
	lseek(lfp, 0, SEEK_SET);
	if (write(lfp, tempBuf, tempDataLen) != tempDataLen) {
		LOG(ERROR) << "Unable to write PID to file " << lockfile << ", code="
		           << errno << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int readPidFile(const std::string &lockfile, int lfp, int &pid)
{
	char tempBuf[64];
	lseek(lfp, 0, SEEK_SET);
	int bytesRead = read(lfp, tempBuf, sizeof(tempBuf));
	if (bytesRead <= 0) {
		LOG(ERROR) << "Unable to read PID from file " << lockfile << ", code="
		           << errno << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}
	tempBuf[bytesRead<sizeof(tempBuf)?bytesRead:sizeof(tempBuf)-1] = '\0';
	int res = sscanf(tempBuf, " %d", &pid);
	if (res < 1) {
		LOG(ERROR) << "Unable to parse PID from file " << lockfile << ", code="
		           << errno << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int startTransceiver()
{
	// Start the transceiver binary, if the path is defined.
	// If the path is not defined, the transceiver must be started by some other process.
	if (gConfig.defines("TRX.Path")) {

		// Open and lock PID file, taking care of old transceiver instance.
		sgTransceiverPidFile = gConfig.getStr("TRX.WritePID");
		sgTransceiverPidFileFd = openPidFile(sgTransceiverPidFile);
		if (sgTransceiverPidFileFd < 0) return EXIT_SUCCESS;
		int pid;
		if (lockPidFile(sgTransceiverPidFile, sgTransceiverPidFileFd, false) != EXIT_SUCCESS) {
			// Another OpenBTS instance is running and blocking PID file.
			return EXIT_FAILURE;
		}
		if (readPidFile(sgTransceiverPidFile, sgTransceiverPidFileFd, pid) == EXIT_SUCCESS) {
			// There is no harm in this. Transceiver's owner is not
			// running and could safely kill it.
			kill(pid, SIGTERM);
		}

		// Start transceiver
		const char *TRXPath = gConfig.getStr("TRX.Path");
		const char *TRXLogLevel = gConfig.getStr("TRX.LogLevel");
		const char *TRXLogFileName = NULL;
		if (gConfig.defines("TRX.LogFileName")) TRXLogFileName=gConfig.getStr("TRX.LogFileName");
		sgTransceiverPid = vfork();
		LOG_ASSERT(sgTransceiverPid>=0);
		if (sgTransceiverPid==0) {
			// Pid==0 means this is the process that starts the transceiver.
			execl(TRXPath,"transceiver",TRXLogLevel,TRXLogFileName,NULL);
			LOG(ERROR) << "cannot start transceiver";
			_exit(0);
		}
		// Now we can finally write transceiver PID to the file.
		if (writePidFile(sgTransceiverPidFile, sgTransceiverPidFileFd, sgTransceiverPid) != EXIT_SUCCESS) return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static void serverCleanup()
{
	if (sgTransceiverPid) {
		kill(sgTransceiverPid, SIGTERM);
		if (sgTransceiverPidFileFd >= 0) {
			close(sgTransceiverPidFileFd);
		}
		if	(sgTransceiverPidFile.size() > 0) {
			if (unlink(sgTransceiverPidFile.data()) == 0) {
				LOG(INFO) << "Deleted lock file " << sgTransceiverPidFile;
			} else {
				LOG(INFO) << "Error while deleting lock file " << sgTransceiverPidFile
				          << " code=" << errno << ": " << strerror(errno);
			}
		}
	}
}

static void exitCLI()
{
	if (sgCLIServerSock == NULL) {
		serverCleanup();
		_exit(EXIT_SUCCESS);
	} else {
		// Closing server sock
		sgCLIServerSock->close();
		sgCLIServerSock = NULL;
	}

	// Closing server standard input to shutdown local CLI
	// Following functions are not async-signal-safe, but I don't have
	// better idea how to do this.
	cin.setstate(ios::eofbit);
//	cin.putback('\n');
	fclose(stdin);
}

static void daemonChildHandler(int signum)
{
	LOG(INFO) << "Handling signal " << signum;
	switch(signum) {
	 case SIGALRM:
		 // alarm() fired.
		 exit(EXIT_FAILURE);
		 break;
	 case SIGUSR1:
		 //Child sent us a signal. Good sign!
		 exit(EXIT_SUCCESS);
		 break;
	 case SIGCHLD:
		 // Child has died
		 exit(EXIT_FAILURE);
		 break;
	}
}

static int daemonize(std::string &lockfile, int &lfp)
{
	// Already a daemon
	if ( getppid() == 1 ) return EXIT_SUCCESS;

	// Sanity checks
	if (strcasecmp(gConfig.getStr("CLI.Type"),"Local") == 0) {
		LOG(ERROR) << "OpenBTS runs in daemon mode, but CLI is set to Local!";
		return EXIT_FAILURE;
	}
	if (!gConfig.defines("Server.WritePID")) {
		LOG(ERROR) << "OpenBTS runs in daemon mode, but Server.WritePID is not set in config!";
		return EXIT_FAILURE;
	}

	// According to the Filesystem Hierarchy Standard 5.13.2:
	// "The naming convention for PID files is <program-name>.pid."
	// The same standard specifies that PID files should be placed
	// in /var/run, but we make this configurable.
	lockfile = gConfig.getStr("Server.WritePID");

	// Create the PID file as the current user
	if ((lfp=openPidFile(lockfile)) < 0) return EXIT_FAILURE;

	// Drop user if there is one, and we were run as root
/*	if ( getuid() == 0 || geteuid() == 0 ) {
		struct passwd *pw = getpwnam(RUN_AS_USER);
		if ( pw ) {
			syslog( LOG_NOTICE, "setting user to " RUN_AS_USER );
			setuid( pw->pw_uid );
		}
	}
*/

	// Trap signals that we expect to receive
	signal(SIGCHLD, daemonChildHandler);
	signal(SIGUSR1, daemonChildHandler);
	signal(SIGALRM, daemonChildHandler);

	// Fork off the parent process
	pid_t pid = fork();
	if (pid < 0) {
		LOG(ERROR) << "Unable to fork daemon, code=" << errno
		           << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}
	// If we got a good PID, then we can exit the parent process.
	if (pid > 0) {
		// Wait for confirmation from the child via SIGUSR1 or SIGCHLD.
		LOG(INFO) << "Forked child process with PID " << pid;
		// Some recommend to add timeout here too (it will signal SIGALRM),
		// but I don't think it's a good idea if we start on a slow system.
		// Or may be we should make timeout value configurable and set it
		// a big enough value.
//		alarm(2);
		// pause() should not return.
		pause();
		LOG(ERROR) << "Executing code after pause()!";
		return EXIT_FAILURE;
	}

	// Now lock our PID file and write our PID to it
	if (lockPidFile(lockfile, lfp) != EXIT_SUCCESS) return EXIT_FAILURE;
	if (writePidFile(lockfile, lfp, getpid()) != EXIT_SUCCESS) return EXIT_FAILURE;

	// At this point we are executing as the child process
	pid_t parent = getppid();

	// Return signals to default handlers
	signal(SIGCHLD, SIG_DFL);
	signal(SIGUSR1, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	// Change the file mode mask
	// This will restrict file creation mode to 750 (complement of 027).
	umask(gConfig.getNum("Server.umask"));

	// Create a new SID for the child process
	pid_t sid = setsid();
	if (sid < 0) {
		LOG(ERROR) << "Unable to create a new session, code=" << errno
		           << " (" << strerror(errno) << ")";
		return EXIT_FAILURE;
	}

	// Change the current working directory.  This prevents the current
	// directory from being locked; hence not being able to remove it.
	if (gConfig.defines("Server.ChdirToRoot")) {
		if (chdir("/") < 0) {
			LOG(ERROR) << "Unable to change directory to %s, code" << errno
			           << " (" << strerror(errno) << ")";
			return EXIT_FAILURE;
		} else {
			LOG(INFO) << "Changed current directory to \"/\"";
		}
	}

	// Redirect standard files to /dev/null
	if (freopen( "/dev/null", "r", stdin) == NULL)
		LOG(WARN) << "Error redirecting stdin to /dev/null";
	if (freopen( "/dev/null", "w", stdout) == NULL)
		LOG(WARN) << "Error redirecting stdout to /dev/null";
	if (freopen( "/dev/null", "w", stderr) == NULL)
		LOG(WARN) << "Error redirecting stderr to /dev/null";

	// Tell the parent process that we are okay
	kill(parent, SIGUSR1);

	return EXIT_SUCCESS;
}

static int forkLoop()
{
	bool shouldExit = false;
	sigset_t chldSignalSet;
	sigemptyset(&chldSignalSet);
	sigaddset(&chldSignalSet, SIGCHLD);
	sigaddset(&chldSignalSet, SIGTERM);
	sigaddset(&chldSignalSet, SIGINT);
	sigaddset(&chldSignalSet, SIGKILL);

	// Block signals to avoid race condition.
	// It will be delivered to us in sigwait() when we are ready to handle it.
	sigprocmask(SIG_BLOCK, &chldSignalSet, NULL);

	while (1) {
		// Fork off the parent process
		pid_t pid = fork();
		if (pid < 0) {
			// fork() failed.
			LOG(ERROR) << "Unable to fork child, code=" << errno
			           << " (" << strerror(errno) << ")";
			return EXIT_FAILURE;
		} else if (pid > 0) {
			// Parent process
			// Wait for child process to exit (SIGCHLD).
			LOG(INFO) << "Forked child process with PID " << pid;
			int signum = -1;
			while (signum != SIGCHLD) {
				sigwait(&chldSignalSet, &signum);
				switch(signum) {
					case SIGCHLD:
						LOG(ERROR) << "Child with PID " << pid << " died.";
						if (shouldExit) exit(EXIT_SUCCESS);
						break;
					case SIGTERM:
					case SIGINT:
					case SIGKILL:
						// Forward signal to the child.
						kill(pid, signum);
						// We will exit child exits and send us SIGCHLD.
						shouldExit = true;
				}
			}
		} else {
			// Child process
			// Unblock signals we blocked.
			sigprocmask(SIG_UNBLOCK, &chldSignalSet, NULL);
			return EXIT_SUCCESS;
		}
	}

	return EXIT_SUCCESS;
}

static void signalHandler(int sig)
{
	COUT("Handling signal " << sig);
	LOG(INFO) << "Handling signal " << sig;
	switch(sig){
		case SIGHUP:
			// re-read the config
			// TODO::
			break;		
		case SIGTERM:
		case SIGINT:
			// finalize the server
			exitCLI();
			break;
		default:
			break;
	}	
}

int main(int argc, char *argv[])
{
	srandom(time(NULL));

	// Catch signal to re-read config
	if (signal(SIGHUP, signalHandler) == SIG_ERR) {
		cerr << "Error while setting handler for SIGHUP.";
		return EXIT_FAILURE;
	}
	// Catch signal to shutdown gracefully
	if (signal(SIGTERM, signalHandler) == SIG_ERR) {
		cerr << "Error while setting handler for SIGTERM.";
		return EXIT_FAILURE;
	}
	// Catch Ctrl-C signal
	if (signal(SIGINT, signalHandler) == SIG_ERR) {
		cerr << "Error while setting handler for SIGINT.";
		return EXIT_FAILURE;
	}
	// Various TTY signals
	// We don't really care about return values of these.
	signal(SIGTSTP,SIG_IGN);
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);

	cout << endl << endl << gOpenBTSWelcome << endl;

	try {

#if 1
	cout << endl << "Starting the system..." << endl;

	if (gConfig.defines("Control.TMSITable.SavePath")) {
		gTMSITable.load(gConfig.getStr("Control.TMSITable.SavePath"));
	}

	LOG(ALARM) << "OpenBTS starting, ver " << VERSION << " build date " << __DATE__;

	startTransceiver();

	// Start the SIP interface.
	gSIPInterface.start();

	// Start the transceiver interface.
	// Sleep long enough for the USRP to bootload.
	sleep(5);
	gTRX.start();

	// Set up the interface to the radio.
	// Get a handle to the C0 transceiver interface.
	ARFCNManager* radio = gTRX.ARFCN(0);

	// Tuning.
	// Make sure its off for tuning.
	radio->powerOff();
	// Set TSC same as BCC everywhere.
	radio->setTSC(gBTS.BCC());
	// Tune.
	radio->tune(gConfig.getNum("GSM.ARFCN"));

	// Turn on and power up.
	radio->powerOn();
	radio->setPower(gConfig.getNum("GSM.PowerManager.MinAttenDB"));

	// Set maximum expected delay spread.
	radio->setMaxDelay(gConfig.getNum("GSM.MaxExpectedDelaySpread"));

	// Set Receiver Gain
	radio->setRxGain(gConfig.getNum("GSM.RxGain"));

	// C-V on C0T0
	radio->setSlot(0,5);
	// SCH
	SCHL1FEC SCH;
	SCH.downstream(radio);
	SCH.open();
	// FCCH
	FCCHL1FEC FCCH;
	FCCH.downstream(radio);
	FCCH.open();
	// BCCH
	BCCHL1FEC BCCH;
	BCCH.downstream(radio);
	BCCH.open();
	// RACH
	RACHL1FEC RACH(gRACHC5Mapping);
	RACH.downstream(radio);
	RACH.open();
	// CCCHs
	CCCHLogicalChannel CCCH0(gCCCH_0Mapping);
	CCCH0.downstream(radio);
	CCCH0.open();
	CCCHLogicalChannel CCCH1(gCCCH_1Mapping);
	CCCH1.downstream(radio);
	CCCH1.open();
	CCCHLogicalChannel CCCH2(gCCCH_2Mapping);
	CCCH2.downstream(radio);
	CCCH2.open();
	// use CCCHs as AGCHs
	gBTS.addAGCH(&CCCH0);
	gBTS.addAGCH(&CCCH1);
	gBTS.addAGCH(&CCCH2);

	// C-V C0T0 SDCCHs
	SDCCHLogicalChannel C0T0SDCCH[4] = {
		SDCCHLogicalChannel(0,gSDCCH_4_0),
			SDCCHLogicalChannel(0,gSDCCH_4_1),
			SDCCHLogicalChannel(0,gSDCCH_4_2),
			SDCCHLogicalChannel(0,gSDCCH_4_3),
	};
	Thread C0T0SDCCHControlThread[4];
	for (int i=0; i<4; i++) {
		C0T0SDCCH[i].downstream(radio);
		C0T0SDCCHControlThread[i].start((void*(*)(void*))Control::DCCHDispatcher,&C0T0SDCCH[i]);
		C0T0SDCCH[i].open();
		gBTS.addSDCCH(&C0T0SDCCH[i]);
	}

	// Count configured slots.
	unsigned sCount = 1;

	bool halfDuplex = gConfig.defines("GSM.HalfDuplex");
	if (halfDuplex) { LOG(NOTICE) << "Configuring for half-duplex operation." ; }
	else { LOG(NOTICE) << "Configuring for full-duplex operation."; }

	if (halfDuplex) sCount++;

	// Create C-VII slots.
	for (int i=0; i<gConfig.getNum("GSM.NumC7s"); i++) {
		gBTS.createCombinationVII(gTRX,sCount/8,sCount);
		if (halfDuplex) sCount++;
		sCount++;
	}

	// Create C-I slots.
	for (int i=0; i<gConfig.getNum("GSM.NumC1s"); i++) {
		gBTS.createCombinationI(gTRX,sCount/8,sCount);
		if (halfDuplex) sCount++;
		sCount++;
	}


	// Set up idle filling on C0 as needed.
	while (sCount<8) {
		gBTS.createCombination0(gTRX,sCount/8,sCount);
		if (halfDuplex) sCount++;
		sCount++;
	}

	//GPRS
	if (gConfig.getNum("GSM.GPRS")) {
		radio->setSlot(gConfig.getNum("GPRS.TS"),8);
		for (int i=0; i<1; i++) {
			PDTCHLogicalChannel* PDTCH = new PDTCHLogicalChannel(gConfig.getNum("GPRS.TS"),gPDTCH_FPair);
			PDTCH->downstream(radio);
			PDTCH->open();
			gBTS.addPDTCH(PDTCH);
		}
	}

	/*
		Note: The number of different paging subchannels on       
		the CCCH is:                                        
                                                           
		MAX(1,(3 - BS-AG-BLKS-RES)) * BS-PA-MFRMS           
			if CCCH-CONF = "001"                        
		(9 - BS-AG-BLKS-RES) * BS-PA-MFRMS                  
			for other values of CCCH-CONF               
	*/

	// Set up the pager.
	// Set up paging channels.
	// HACK -- For now, use a single paging channel, since paging groups are broken.
	gBTS.addPCH(&CCCH2);

	// Be sure we are not over-reserving.
	LOG_ASSERT(gConfig.getNum("GSM.PagingReservations")<gBTS.numAGCHs());

	// OK, now it is safe to start the BTS.
	gBTS.start();

	LOG(INFO) << "system ready";
#endif

	if (strcasecmp(gConfig.getStr("CLI.Type"),"TCP") == 0) {
		ConnectionServerSocketTCP serverSock(gConfig.getNum("CLI.TCP.Port"),
		                                     gConfig.getStr("CLI.TCP.IP"));
		sgCLIServerSock = &serverSock;
		runCLIServer(&serverSock);
		sgCLIServerSock = NULL;
	} else if (strcasecmp(gConfig.getStr("CLI.Type"),"Unix") == 0) {
		ConnectionServerSocketUnix serverSock(gConfig.getStr("CLI.Unix.Path"));
		sgCLIServerSock = &serverSock;
		runCLIServer(&serverSock);
		sgCLIServerSock = NULL;
	} else {
		runCLI(&gParser);
	}

	} catch(SocketError) {
		// Shutdown without core dump.
		// SocketError is a usual case, e.g. it's fired when transceiver fails.
		LOG(ALARM) << "Uncaught exception. Shutting down.";
	}

	if (!gBTS.hold()) {
		exitBTS(0, cout);
	}

	serverCleanup();

	return EXIT_SUCCESS;
}

// vim: ts=4 sw=4
