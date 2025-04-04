//
// Audio interface Routine

// Passes audio samples to/from the sound intemrface

// As this is platform specific it also has the main() routine, which does
// platform specific initialisation before calling ardopmain()

// This is ALSASound.c for Linux
// Windows Version is Waveout.c

#include <alsa/asoundlib.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HANDLE int

#include "common/ardopcommon.h"
#include "common/wav.h"
#include "rockliff/rrs.h"

#define SHARECAPTURE  // if defined capture device is opened and closed for each transission


void gpioSetMode(unsigned gpio, unsigned mode);
void gpioWrite(unsigned gpio, unsigned level);
int WriteLog(char * msg, int Log);
int _memicmp(unsigned char *a, unsigned char *b, int n);
int stricmp(const unsigned char * pStr1, const unsigned char *pStr2);
int gpioInitialise(void);
HANDLE OpenCOMPort(VOID * pPort, int speed, BOOL SetDTR, BOOL SetRTS, BOOL Quiet, int Stopbits);

VOID COMSetDTR(HANDLE fd);
VOID COMClearDTR(HANDLE fd);
VOID COMSetRTS(HANDLE fd);
VOID COMClearRTS(HANDLE fd);
VOID RadioPTT(int PTTState);
VOID SerialHostPoll();
VOID TCPHostPoll();
int CloseSoundCard();
int PackSamplesAndSend(short * input, int nSamples);
BOOL WriteCOMBlock(HANDLE fd, char * Block, int BytesToWrite);
VOID processargs(int argc, char * argv[]);
int wg_send_currentlevel(int cnum, unsigned char level);
int wg_send_pttled(int cnum, bool isOn);
int wg_send_pixels(int cnum, unsigned char *data, size_t datalen);
void WebguiPoll();

int add_noise(short *samples, unsigned int nSamples, short stddev);
short InputNoiseStdDev = 0;

extern BOOL blnDISCRepeating;

extern char * CM108Device;

extern int SampleNo;
extern unsigned int pttOnTime;

snd_pcm_uframes_t fpp;  // Frames per period.
int dir;

BOOL UseLeftRX = TRUE;
BOOL UseRightRX = TRUE;

BOOL UseLeftTX = TRUE;
BOOL UseRightTX = TRUE;

extern BOOL WriteRxWav;
extern BOOL WriteTxWav;
extern BOOL FixTiming;
extern char DecodeWav[5][256];
extern int WavNow;  // Time since start of WAV file being decoded

extern struct sockaddr HamlibAddr;  // Dest for above
extern int useHamLib;


void Sleep(int mS)
{
	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		usleep(mS * 1000);
	return;
}


// Windows and ALSA work with signed samples +- 32767
short buffer[2][1200];  // Two Transfer/DMA buffers of 0.1 Sec
short inbuffer[2][1200];  // Two Transfer/DMA buffers of 0.1 Sec

BOOL Loopback = FALSE;
// BOOL Loopback = TRUE;

char CaptureDevice[80] = "ARDOP";
char PlaybackDevice[80] = "ARDOP";

char * CaptureDevices = CaptureDevice;
char * PlaybackDevices = CaptureDevice;

int InitSound();

int Ticks;

int LastNow;

extern int Number;  // Number waiting to be sent

snd_pcm_sframes_t MaxAvail;

#include <stdarg.h>

struct WavFile *rxwf = NULL;
struct WavFile *txwff = NULL;
// writing unfiltered tx audio to WAV disabled
// struct WavFile *txwfu = NULL;
#define RXWFTAILMS 10000;  // 10 seconds
unsigned int rxwf_EndNow = 0;

void extendRxwf()
{
	rxwf_EndNow = Now + RXWFTAILMS;
}

void StartRxWav()
{
	// Open a new WAV file if not already recording.
	// If already recording, then just extend the time before
	// recording will end.
	//
	// Wav files will use a filename that includes port, UTC date,
	// and UTC time, similar to log files but with added time to
	// the nearest second.  Like Log files, these Wav files will be
	// written to the Log directory if defined, else to the current
	// directory
	//
	// As currently implemented, the wav file written contains only
	// received audio.  Since nothing is written for the time while
	// transmitting, and thus not receiving, this recording is not
	// time continuous.  Thus, the filename indicates the time that
	// the recording was started, but the times of the received
	// transmissions, other than the first one, are not indicated.
	char rxwf_pathname[1024];
	int pnlen;

	if (rxwf != NULL)
	{
		// Already recording, so just extend recording time.
		extendRxwf();
		return;
	}
	struct tm * tm;
	time_t T;

	T = time(NULL);
	tm = gmtime(&T);

	struct timespec tp;
	int ss, hh, mm;
	clock_gettime(CLOCK_REALTIME, &tp);
	ss = tp.tv_sec % 86400;  // Seconds in a day
	hh = ss / 3600;
	mm = (ss - (hh * 3600)) / 60;
	ss = ss % 60;

	if (ardop_log_get_directory()[0])
		pnlen = snprintf(rxwf_pathname, sizeof(rxwf_pathname),
			"%s/ARDOP_rxaudio_%d_%04d%02d%02d_%02d%02d%02d.wav",
			ardop_log_get_directory(), port, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
			hh, mm, ss);
	else
		pnlen = snprintf(rxwf_pathname, sizeof(rxwf_pathname),
			"ARDOP_rxaudio_%d_%04d%02d%02d_%02d:%02d:%02d.wav",
			port, tm->tm_year +1900, tm->tm_mon+1, tm->tm_mday,
			hh, mm, ss);
	if (pnlen == -1 || pnlen > sizeof(rxwf_pathname)) {
		// Logpath too long likely to also prevent writing to log files.
		// So, print this error directly to console instead of using
		// WriteDebugLog.
		printf("Unable to write WAV file, invalid pathname. Logpath may be too long.\n");
		WriteRxWav = FALSE;
		return;
	}
	rxwf = OpenWavW(rxwf_pathname);
	extendRxwf();
}

// writing unfiltered tx audio to WAV disabled.  Only filtered
// tx audio will be written.  However, the code for unfiltered
// audio is left in place but commented out so that it can eaily
// be restored if desired.
void StartTxWav()
{
	// Open two new WAV files for filtered and unfiltered Tx audio.
	//
	// Wav files will use a filename that includes port, UTC date,
	// and UTC time, similar to log files but with added time to
	// the nearest second.  Like Log files, these Wav files will be
	// written to the Log directory if defined, else to the current
	// directory
	char txwff_pathname[1024];
	// char txwfu_pathname[1024];
	int pnflen;
	// int pnulen;

	if (txwff != NULL)  // || txwfu != NULL)
	{
		ZF_LOGW("WARNING: Trying to open Tx WAV file, but already open.");
		return;
	}
	struct tm * tm;
	time_t T;

	T = time(NULL);
	tm = gmtime(&T);

	struct timespec tp;
	int ss, hh, mm;
	clock_gettime(CLOCK_REALTIME, &tp);
	ss = tp.tv_sec % 86400;  // Seconds in a day
	hh = ss / 3600;
	mm = (ss - (hh * 3600)) / 60;
	ss = ss % 60;

	if (ardop_log_get_directory()[0])
	{
		pnflen = snprintf(txwff_pathname, sizeof(txwff_pathname),
			"%s/ARDOP_txfaudio_%d_%04d%02d%02d_%02d%02d%02d.wav",
			ardop_log_get_directory(), port, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
			hh, mm, ss);
	}
	else
	{
		pnflen = snprintf(txwff_pathname, sizeof(txwff_pathname),
			"ARDOP_txfaudio_%d_%04d%02d%02d_%02d:%02d:%02d.wav",
			port, tm->tm_year +1900, tm->tm_mon+1, tm->tm_mday,
			hh, mm, ss);
	}
	if (pnflen == -1 || pnflen > sizeof(txwff_pathname)) {
		// Logpath too long likely to also prevent writing to log files.
		// So, print this error directly to console instead of using
		// WriteDebugLog.
		printf("Unable to write WAV file, invalid pathname. Logpath may be too long.\n");
		WriteTxWav = FALSE;
		return;
	}
	// if (pnulen == -1 || pnulen > sizeof(txwfu_pathname)) {
		// Logpath too long likely to also prevent writing to log files.
		// So, print this error directly to console instead of using
		// WriteDebugLog.
		// printf("Unable to write WAV file, invalid pathname. Logpath may be too long.\n");
		// WriteTxWav = FALSE;
		// return;
	// }
	txwff = OpenWavW(txwff_pathname);
	// txwfu = OpenWavW(txwfu_pathname);
}

void printtick(char * msg)
{
	ZF_LOGI("%s %i", msg, Now - LastNow);
	LastNow = Now;
}

struct timespec time_start;

unsigned int getTicks()
{
	struct timespec tp;

	// When decoding a WAV file, return WavNow, a measure of the offset
	// in ms from the start of the WAV file.
	if (DecodeWav[0][0])
		return WavNow;

	// Otherwise, return a measure of clock time (also measured in ms).
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (tp.tv_sec - time_start.tv_sec) * 1000 + (tp.tv_nsec - time_start.tv_nsec) / 1000000;
}

void PlatformSleep(int mS)
{
	TCPHostPoll();
	WebguiPoll();

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		Sleep(mS);

	if (PKTLEDTimer && Now > PKTLEDTimer)
	{
		PKTLEDTimer = 0;
		SetLED(PKTLED, 0);  // turn off packet rxed led
	}
}

/*
 * Lists common signal abbreviations
 *
 * This method is a portable version of glibc's sigabbrev_np().
 * It only supports a handful of signal names that ARDOP
 * currently catches and/or ignores. Unlike the glibc function,
 * the return value is always guaranteed to be non-NULL.
 */
const char* PlatformSignalAbbreviation(int signal) {
	switch (signal) {
		case SIGABRT:
			return "SIGABRT";
		case SIGINT:
			return "SIGINT";
		case SIGHUP:
			return "SIGHUP";
		case SIGPIPE:
			return "SIGPIPE";
		case SIGTERM:
			return "SIGTERM";
		default:
			return "Unknown";
	}
}

// PTT via GPIO code

#ifdef __ARM_ARCH

#define PI_INPUT 0
#define PI_OUTPUT 1
#define PI_ALT0 4
#define PI_ALT1 5
#define PI_ALT2 6
#define PI_ALT3 7
#define PI_ALT4 3
#define PI_ALT5 2

// Set GPIO pin as output and set low

extern int pttGPIOPin;
extern BOOL pttGPIOInvert;



void SetupGPIOPTT()
{
	if (pttGPIOPin == -1)
	{
		ZF_LOGI("GPIO PTT disabled");
		RadioControl = FALSE;
		useGPIO = FALSE;
	}
	else
	{
		if (pttGPIOPin < 0) {
			pttGPIOInvert = TRUE;
			pttGPIOPin = -pttGPIOPin;
		}

		gpioSetMode(pttGPIOPin, PI_OUTPUT);
		gpioWrite(pttGPIOPin, pttGPIOInvert ? 1 : 0);
		ZF_LOGI("Using GPIO pin %d for PTT", pttGPIOPin);
		RadioControl = TRUE;
		useGPIO = TRUE;
	}
}
#endif


static void signal_handler_trigger_shutdown(int sig)
{
	blnClosing = TRUE;
	closedByPosixSignal = sig;
}

char * PortString = NULL;


int platform_main(int argc, char * argv[])
{
	struct timespec tp;
	struct sigaction act;
	int lnlen;
	// rslen_set[] must list all of the rslen values used.
	int rslen_set[] = {2, 4, 8, 16, 32, 36, 50, 64};
	init_rs(rslen_set, 8);

	char cmdstr[3000] = "";
	for (int i = 0; i < argc; i++) {
		if ((int)(sizeof(cmdstr) - strlen(cmdstr))
			<= snprintf(
				cmdstr + strlen(cmdstr),
				sizeof(cmdstr) - strlen(cmdstr),
				"%s ",
				argv[i])
		) {
			printf("ERROR: cmdstr[%ld] insufficient to hold fill command string for logging.\n", sizeof(cmdstr));
			break;
		}
	}

//	Sleep(1000);  // Give LinBPQ time to complete init if exec'ed by linbpq

	processargs(argc, argv);

	setlinebuf(stdout);  // So we can redirect output to file and tail

	ZF_LOGI("%s Version %s (https://www.github.com/pflarue/ardop)", ProductName, ProductVersion);
	ZF_LOGI("Copyright (c) 2014-2024 Rick Muething, John Wiseman, Peter LaRue");
	ZF_LOGI(
		"See https://github.com/pflarue/ardop/blob/master/LICENSE for licence details including\n"
		"  information about authors of external libraries used and their licenses."
	);
	ZF_LOGD("Command line: %s", cmdstr);

	if (DecodeWav[0][0])
	{
		decode_wav();
		return (0);
	}

	if (HostPort[0])
		port = atoi(HostPort);

	if (CATPort[0])
	{
		char * Baud = strlop(CATPort, ':');
		if (Baud)
			CATBAUD = atoi(Baud);

		hCATDevice = OpenCOMPort(CATPort, CATBAUD, FALSE, FALSE, FALSE, 0);

	}

	if (PTTPort[0])
	{
		int fd;

		if (strstr(PTTPort, "hidraw"))
		{
			// Linux - Param is HID Device, eg /dev/hidraw0

			CM108Device = strdup(PTTPort);
			fd = open (CM108Device, O_WRONLY);

			if (fd == -1)
			{
				ZF_LOGE("Could not open %s for write, errno=%d", CM108Device, errno);
			}
			else
			{
				close (fd);
				PTTMode = PTTCM108;
				RadioControl = TRUE;
				ZF_LOGI("Using %s for PTT", CM108Device);
			}
		}
		else
		{
			char * Baud = strlop(PTTPort, ':');
			char * pin = strlop(PTTPort, '=');

			if (Baud)
				PTTBAUD = atoi(Baud);

			if (strcmp(CATPort, PTTPort) == 0)
			{
				hPTTDevice = hCATDevice;
			}
			else
			{
				if (stricmp(PTTPort, "GPIO") == 0)
				{
					// Initialise GPIO for PTT if available

#ifdef __ARM_ARCH
					if (gpioInitialise() == 0)
					{
						printf("GPIO interface for PTT available\n");
						gotGPIO = TRUE;

						if (pin)
							pttGPIOPin = atoi(pin);
						else
							pttGPIOPin = 17;

						SetupGPIOPTT();
					}
					else
						printf("Couldn't initialise GPIO interface for PTT\n");

#else
					printf("GPIO interface for PTT not available on this platform\n");
#endif

				}
				else  // Not GPIO
				{
					if (Baud)
					{
						// Could be IPADDR:PORT or COMPORT:SPEED. See if first part is valid ip address

						struct sockaddr_in * destaddr = (struct sockaddr_in *)&HamlibAddr;

						destaddr->sin_family = AF_INET;
						destaddr->sin_addr.s_addr = inet_addr(PTTPort);
						destaddr->sin_port = htons(atoi(Baud));

						if (destaddr->sin_addr.s_addr != INADDR_NONE)
						{
							useHamLib = 1;
							ZF_LOGI("Using Hamlib at %s:%s for PTT", PTTPort, Baud);
							RadioControl = TRUE;
							PTTMode = PTTHAMLIB;
						}
						else
							PTTBAUD = atoi(Baud);
					}
					if (useHamLib == 0)
						hPTTDevice = OpenCOMPort(PTTPort, PTTBAUD, FALSE, FALSE, FALSE, 0);
				}
			}
		}
	}

	if (hCATDevice)
	{
		ZF_LOGI("CAT Control on port %s", CATPort);
		COMSetRTS(hPTTDevice);
		COMSetDTR(hPTTDevice);
		if (PTTOffCmdLen)
		{
			ZF_LOGI("PTT using CAT Port: %s", CATPort);
			RadioControl = TRUE;
		}
	}
	else
	{
		// Warn of -u and -k defined but no CAT Port

		if (PTTOffCmdLen)
		{
			ZF_LOGW("Warning PTT Off string defined but no CAT port");
		}
	}

	if (hPTTDevice)
	{
		ZF_LOGI("Using RTS on port %s for PTT", PTTPort);
		COMClearRTS(hPTTDevice);
		COMClearDTR(hPTTDevice);
		RadioControl = TRUE;
	}


	ZF_LOGI("ARDOPC listening on port %d", port);

	// Get Time Reference

	clock_gettime(CLOCK_MONOTONIC, &time_start);
	LastNow = getTicks();

	// Trap signals

	memset (&act, '\0', sizeof(act));

	act.sa_handler = &signal_handler_trigger_shutdown;
	if (sigaction(SIGINT, &act, NULL) < 0)
		perror ("SIGINT");

	act.sa_handler = &signal_handler_trigger_shutdown;
	if (sigaction(SIGTERM, &act, NULL) < 0)
		perror ("SIGTERM");

	act.sa_handler = SIG_IGN;

	if (sigaction(SIGHUP, &act, NULL) < 0)
		perror ("SIGHUP");

	if (sigaction(SIGPIPE, &act, NULL) < 0)
		perror ("SIGPIPE");

	ardopmain();

	return (0);
}

void txSleep(int mS)
{
	// called while waiting for next TX buffer or to delay response.
	// Run background processes

	TCPHostPoll();
	WebguiPoll();

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		Sleep(mS);

	if (PKTLEDTimer && Now > PKTLEDTimer)
	{
		PKTLEDTimer = 0;
		SetLED(PKTLED, 0);  // turn off packet rxed led
	}
}

// ALSA Code

#define true 1
#define false 0

snd_pcm_t *	playhandle = NULL;
snd_pcm_t *	rechandle = NULL;

int m_playchannels = 1;
int m_recchannels = 1;


char SavedCaptureDevice[256];  // Saved so we can reopen
char SavedPlaybackDevice[256];

int Savedplaychannels = 1;

int SavedCaptureRate;
int SavedPlaybackRate;

// This rather convoluted process simplifies marshalling from Managed Code

char ** WriteDevices = NULL;
int WriteDeviceCount = 0;

char ** ReadDevices = NULL;
int ReadDeviceCount = 0;

// Routine to check that library is available

int CheckifLoaded()
{
	// Prevent CTRL/C from closing the TNC
	// (This causes problems if the TNC is started by LinBPQ)

	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	return TRUE;
}

int GetOutputDeviceCollection()
{
	// Get all the suitable devices and put in a list for GetNext to return

	snd_ctl_t *handle= NULL;
	snd_pcm_t *pcm= NULL;
	snd_ctl_card_info_t *info;
	snd_pcm_info_t *pcminfo;
	snd_pcm_hw_params_t *pars;
	snd_pcm_format_mask_t *fmask;
	char NameString[256];

	ZF_LOGI("Playback Devices\n");

	CloseSoundCard();

	// free old struct if called again

//	while (WriteDeviceCount)
//	{
//		WriteDeviceCount--;
//		free(WriteDevices[WriteDeviceCount]);
//	}

//	if (WriteDevices)
//		free(WriteDevices);

	WriteDevices = NULL;
	WriteDeviceCount = 0;

	// Add virtual device ARDOP so ALSA plugins can be used if needed

	WriteDevices = realloc(WriteDevices,(WriteDeviceCount + 1) * sizeof(WriteDevices));
	WriteDevices[WriteDeviceCount++] = strdup("ARDOP");

	// Get Device List  from ALSA

	snd_ctl_card_info_alloca(&info);
	snd_pcm_info_alloca(&pcminfo);
	snd_pcm_hw_params_alloca(&pars);
	snd_pcm_format_mask_alloca(&fmask);

	char hwdev[80];
	unsigned min, max, ratemin, ratemax;
	int card, err, dev, nsubd;
	snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;

	card = -1;

	if (snd_card_next(&card) < 0)
	{
		ZF_LOGI("No Devices");
		return 0;
	}

	if (playhandle)
		snd_pcm_close(playhandle);

	playhandle = NULL;

	while (card >= 0)
	{
		sprintf(hwdev, "hw:%d", card);
		err = snd_ctl_open(&handle, hwdev, 0);
		err = snd_ctl_card_info(handle, info);

		ZF_LOGI("Card %d, ID `%s', name `%s'", card, snd_ctl_card_info_get_id(info),
			snd_ctl_card_info_get_name(info));


		dev = -1;

		if(snd_ctl_pcm_next_device(handle, &dev) < 0)
		{
			// Card has no devices

			snd_ctl_close(handle);
			goto nextcard;
		}

		while (dev >= 0)
		{
			snd_pcm_info_set_device(pcminfo, dev);
			snd_pcm_info_set_subdevice(pcminfo, 0);
			snd_pcm_info_set_stream(pcminfo, stream);

			err = snd_ctl_pcm_info(handle, pcminfo);


			if (err == -ENOENT)
				goto nextdevice;

			nsubd = snd_pcm_info_get_subdevices_count(pcminfo);

			ZF_LOGI("  Device hw:%d,%d ID `%s', name `%s', %d subdevices (%d available)",
				card, dev, snd_pcm_info_get_id(pcminfo), snd_pcm_info_get_name(pcminfo),
				nsubd, snd_pcm_info_get_subdevices_avail(pcminfo));

			sprintf(hwdev, "hw:%d,%d", card, dev);

			err = snd_pcm_open(&pcm, hwdev, stream, SND_PCM_NONBLOCK);

			if (err)
			{
				ZF_LOGW("Error %d opening output device", err);
				goto nextdevice;
			}

			// Get parameters for this device

			err = snd_pcm_hw_params_any(pcm, pars);

			snd_pcm_hw_params_get_channels_min(pars, &min);
			snd_pcm_hw_params_get_channels_max(pars, &max);

			snd_pcm_hw_params_get_rate_min(pars, &ratemin, NULL);
			snd_pcm_hw_params_get_rate_max(pars, &ratemax, NULL);

			if( min == max )
				if( min == 1 )
					ZF_LOGI("    1 channel,  sampling rate %u..%u Hz", ratemin, ratemax);
				else
					ZF_LOGI("    %d channels,  sampling rate %u..%u Hz", min, ratemin, ratemax);
			else
				ZF_LOGI("    %u..%u channels, sampling rate %u..%u Hz", min, max, ratemin, ratemax);

			// Add device to list

			sprintf(NameString, "hw:%d,%d %s(%s)", card, dev,
				snd_pcm_info_get_name(pcminfo), snd_ctl_card_info_get_name(info));

			WriteDevices = realloc(WriteDevices,(WriteDeviceCount + 1) * sizeof(WriteDevices));
			WriteDevices[WriteDeviceCount++] = strdup(NameString);

			snd_pcm_close(pcm);
			pcm= NULL;

nextdevice:
			if (snd_ctl_pcm_next_device(handle, &dev) < 0)
				break;
		}
		snd_ctl_close(handle);

nextcard:

		ZF_LOGI("%s", "");

		if (snd_card_next(&card) < 0)  // No more cards
			break;
	}

	return WriteDeviceCount;
}

int GetNextOutputDevice(char * dest, int max, int n)
{
	if (n >= WriteDeviceCount)
		return 0;

	strcpy(dest, WriteDevices[n]);
	return strlen(dest);
}


int GetInputDeviceCollection()
{
	// Get all the suitable devices and put in a list for GetNext to return

	snd_ctl_t *handle= NULL;
	snd_pcm_t *pcm= NULL;
	snd_ctl_card_info_t *info;
	snd_pcm_info_t *pcminfo;
	snd_pcm_hw_params_t *pars;
	snd_pcm_format_mask_t *fmask;
	char NameString[256];

	ZF_LOGI("Capture Devices\n");

	ReadDevices = NULL;
	ReadDeviceCount = 0;

	// Add virtual device ARDOP so ALSA plugins can be used if needed

	ReadDevices = realloc(ReadDevices,(ReadDeviceCount + 1) * sizeof(ReadDevices));
	ReadDevices[ReadDeviceCount++] = strdup("ARDOP");

	// Get Device List  from ALSA

	snd_ctl_card_info_alloca(&info);
	snd_pcm_info_alloca(&pcminfo);
	snd_pcm_hw_params_alloca(&pars);
	snd_pcm_format_mask_alloca(&fmask);

	char hwdev[80];
	unsigned min, max, ratemin, ratemax;
	int card, err, dev, nsubd;
	snd_pcm_stream_t stream = SND_PCM_STREAM_CAPTURE;

	card = -1;

	if(snd_card_next(&card) < 0)
	{
		ZF_LOGI("No Devices");
		return 0;
	}

	if (rechandle)
		snd_pcm_close(rechandle);

	rechandle = NULL;

	while(card >= 0)
	{
		sprintf(hwdev, "hw:%d", card);
		err = snd_ctl_open(&handle, hwdev, 0);
		err = snd_ctl_card_info(handle, info);

		ZF_LOGI("Card %d, ID `%s', name `%s'", card, snd_ctl_card_info_get_id(info),
			snd_ctl_card_info_get_name(info));

		dev = -1;

		if (snd_ctl_pcm_next_device(handle, &dev) < 0)  // No Devicdes
		{
			snd_ctl_close(handle);
			goto nextcard;
		}

		while(dev >= 0)
		{
			snd_pcm_info_set_device(pcminfo, dev);
			snd_pcm_info_set_subdevice(pcminfo, 0);
			snd_pcm_info_set_stream(pcminfo, stream);
			err= snd_ctl_pcm_info(handle, pcminfo);

			if (err == -ENOENT)
				goto nextdevice;

			nsubd= snd_pcm_info_get_subdevices_count(pcminfo);
			ZF_LOGI("  Device hw:%d,%d ID `%s', name `%s', %d subdevices (%d available)",
				card, dev, snd_pcm_info_get_id(pcminfo), snd_pcm_info_get_name(pcminfo),
				nsubd, snd_pcm_info_get_subdevices_avail(pcminfo));

			sprintf(hwdev, "hw:%d,%d", card, dev);

			err = snd_pcm_open(&pcm, hwdev, stream, SND_PCM_NONBLOCK);

			if (err)
			{
				ZF_LOGW("Error %d opening input device", err);
				goto nextdevice;
			}

			err = snd_pcm_hw_params_any(pcm, pars);

			snd_pcm_hw_params_get_channels_min(pars, &min);
			snd_pcm_hw_params_get_channels_max(pars, &max);
			snd_pcm_hw_params_get_rate_min(pars, &ratemin, NULL);
			snd_pcm_hw_params_get_rate_max(pars, &ratemax, NULL);

			if( min == max )
				if( min == 1 )
					ZF_LOGI("    1 channel,  sampling rate %u..%u Hz", ratemin, ratemax);
				else
					ZF_LOGI("    %d channels,  sampling rate %u..%u Hz", min, ratemin, ratemax);
			else
				ZF_LOGI("    %u..%u channels, sampling rate %u..%u Hz", min, max, ratemin, ratemax);

			sprintf(NameString, "hw:%d,%d %s(%s)", card, dev,
				snd_pcm_info_get_name(pcminfo), snd_ctl_card_info_get_name(info));

//			Debugprintf("%s", NameString);

			ReadDevices = realloc(ReadDevices,(ReadDeviceCount + 1) * sizeof(ReadDevices));
			ReadDevices[ReadDeviceCount++] = strdup(NameString);

			snd_pcm_close(pcm);
			pcm= NULL;

nextdevice:
			if (snd_ctl_pcm_next_device(handle, &dev) < 0)
				break;
		}
		snd_ctl_close(handle);
nextcard:

		ZF_LOGI("%s", "");
		if (snd_card_next(&card) < 0 )
			break;
	}
	return ReadDeviceCount;
}

int GetNextInputDevice(char * dest, int max, int n)
{
	if (n >= ReadDeviceCount)
		return 0;

	strcpy(dest, ReadDevices[n]);
	return strlen(dest);
}

int blnFirstOpenSoundPlayback = True;  // used to only log warning about -A option once.

int OpenSoundPlayback(char * PlaybackDevice, int m_sampleRate, int channels, char * ErrorMsg)
{
	unsigned int intRate;  // reported number of frames per second
	unsigned int intPeriodTime = 0;  // reported duration of one period in microseconds.
	snd_pcm_uframes_t periodSize = 0;  // reported number of frames per period

	int intDir;

	int err = 0;

	char buf1[100];
	char * ptr;

	// Choose the number of frames per period.  This avoids possible ALSA misconfiguration
	// errors that may result in a TX symbol timing error if the default value is accepted.
	// The value chosen is a tradeoff between avoiding excessive CPU load caused by too
	// small of a value and increased latency associated with too large a value.
	snd_pcm_uframes_t setPeriodSize = m_sampleRate / 100;

	if (playhandle)
	{
		snd_pcm_close(playhandle);
		playhandle = NULL;
	}

	strcpy(buf1, PlaybackDevice);

	ptr = strchr(buf1, ' ');
	if (ptr)
		*ptr = 0;  // Get Device part of name

	snd_pcm_hw_params_t *hw_params;

	if ((err = snd_pcm_open(&playhandle, buf1, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot open playback audio device %s (%s)",  buf1, snd_strerror(err));
		else
			ZF_LOGE("cannot open playback audio device %s (%s)",  buf1, snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot allocate hardware parameter structure (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot allocate hardware parameter structure (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_any (playhandle, hw_params)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot initialize hardware parameter structure (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot initialize hardware parameter structure (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_set_access (playhandle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot set playback access type (%s)", snd_strerror (err));
		else
			ZF_LOGE("cannot set playback access type (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_set_format (playhandle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot setplayback  sample format (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot setplayback  sample format (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_set_rate (playhandle, hw_params, m_sampleRate, 0)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot set playback sample rate (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot set playback sample rate (%s)", snd_strerror(err));
		return false;
	}

	// Initial call has channels set to 1. Subequent ones set to what worked last time

	if ((err = snd_pcm_hw_params_set_channels (playhandle, hw_params, channels)) < 0)
	{
		ZF_LOGE("cannot set play channel count to %d (%s)", channels, snd_strerror(err));

		if (channels == 2)
			return false;  // Shouldn't happen as should have worked before

		channels = 2;

		if ((err = snd_pcm_hw_params_set_channels (playhandle, hw_params, 2)) < 0)
		{
			ZF_LOGE("cannot play set channel count to 2 (%s)", snd_strerror(err));
			return false;
		}
	}

	if (FixTiming) {
		if ((err = snd_pcm_hw_params_set_period_size (playhandle, hw_params, setPeriodSize, 0)) < 0) {
			if (ErrorMsg)
				sprintf (ErrorMsg, "cannot set playback period size (%s)", snd_strerror(err));
			else
				ZF_LOGE("cannot set playback period size (%s)", snd_strerror(err));
			return false;
		}
	}

	if ((err = snd_pcm_hw_params (playhandle, hw_params)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot set parameters (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot set parameters (%s)", snd_strerror(err));
		return false;
	}

	// Verify that key values were set as expected
	if ((err = snd_pcm_hw_params_get_rate(hw_params, &intRate, &intDir)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot verify playback rate (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot verify playback rate (%s)", snd_strerror(err));
		return false;
	}
	if (m_sampleRate != intRate) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "Unable to correctly set playback rate.  Got %d instead of %d.",
				intRate, m_sampleRate);
		else
			ZF_LOGE("Unable to correctly set playback rate.  Got %d instead of %d.",
				intRate, m_sampleRate);
		return false;
	}

	if ((err = snd_pcm_hw_params_get_period_size(hw_params, &periodSize, &intDir)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot verify playback period size (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot verify playback period size (%s)", snd_strerror(err));
		return false;
	}
	if (FixTiming && (setPeriodSize != periodSize)) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "Unable to correctly set playback period size.  Got %lu instead of %lu.",
				periodSize, setPeriodSize);
		else
			ZF_LOGE("Unable to correctly set playback period size.  Got %ld instead of %ld.",
				periodSize, setPeriodSize);
		return false;
	}

	if ((err = snd_pcm_hw_params_get_period_time(hw_params, &intPeriodTime, &intDir)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot verify playback period time (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot verify playback period time (%s)", snd_strerror(err));
		return false;
	}

	if (FixTiming && (intPeriodTime * intRate != periodSize * 1000000)) {
		if (ErrorMsg)
			sprintf (ErrorMsg,
				"\n\nERROR: Inconsistent playback settings: %d * %d != %lu * 1000000."
				"  Please report this error to the ardop users group at ardop.groups.io"
				" or by creating an issue at www.github.com/pflarue/ardop."
				"  You may find that ardopcf is usable with your hardware/operating"
				" system by using the -A command line option.\n\n",
				intPeriodTime, intRate, periodSize);
		else
			ZF_LOGE(
				"\n\nERROR: Inconsistent playback settings: %d * %d != %lu * 1000000."
				"  Please report this error with a message to the ardop users group"
				" at ardop.groups.io or by creating an issue at github.com/pflarue/ardop."
				"  You may find that ardopcf is usable with your hardware/operating"
				" system by using the -A command line option.\n\n",
				intPeriodTime, intRate, periodSize);
		return false;
	}
	// ZF_LOGI("snd_pcm_hw_params_get_period_time(hw_params, &intPeriodTime, &intDir) intPeriodTime=%d intDir=%d", intPeriodTime, intDir);

	if (!FixTiming && (intPeriodTime * intRate != periodSize * 1000000) && blnFirstOpenSoundPlayback) {
		ZF_LOGW("WARNING: Inconsistent ALSA playback configuration: %u * %u != %ld * 1000000.",
			intPeriodTime, intRate, periodSize);
		ZF_LOGW("This will result in a playblack sample rate of %f instead of %d.",
			periodSize * 1000000.0 / intPeriodTime, intRate);
		ZF_LOGW(
			"This is an error of about %fppm.  Per the Ardop spec +/-100ppm should work well and +/-1000 ppm"
			" should work with some performance degredation.",
			(intRate - (periodSize * 1000000.0 / intPeriodTime))/intRate * 1000000);
		ZF_LOGW(
			"\n\nWARNING: The -A option was specified.  So, ALSA misconfiguration will be accepted and ignored."
			"  This option is primarily intended for testing/debuging.  However, it may also be useful if"
			" ardopcf will not run without it.  In this case, please report this problem to the ardop users"
			" group at ardop.groups.io or by creating an issue at www.github.com/pflarue/ardop.\n\n");
	}
	blnFirstOpenSoundPlayback = False;

	snd_pcm_hw_params_free(hw_params);

	if ((err = snd_pcm_prepare (playhandle)) < 0) {
		ZF_LOGE("cannot prepare audio interface for use (%s)", snd_strerror(err));
		return false;
	}

	Savedplaychannels = m_playchannels = channels;

	MaxAvail = snd_pcm_avail_update(playhandle);
//	Debugprintf("Playback Buffer Size %d", (int)MaxAvail);

	return true;
}

int OpenSoundCapture(char * CaptureDevice, int m_sampleRate, char * ErrorMsg)
{
	int err = 0;

	char buf1[100];
	char * ptr;
	snd_pcm_hw_params_t *hw_params;

	if (rechandle)
	{
		snd_pcm_close(rechandle);
		rechandle = NULL;
	}

	strcpy(buf1, CaptureDevice);

	ptr = strchr(buf1, ' ');
	if (ptr)
		*ptr = 0;  // Get Device part of name

	if ((err = snd_pcm_open (&rechandle, buf1, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot open capture audio device %s (%s)",  buf1, snd_strerror(err));
		else
			ZF_LOGE("cannot open capture audio device %s (%s)", buf1, snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
		ZF_LOGE("cannot allocate capture hardware parameter structure (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_any (rechandle, hw_params)) < 0) {
		ZF_LOGE("cannot initialize capture hardware parameter structure (%s)", snd_strerror(err));
		return false;
	}

// craiger add frames per period

	fpp = 600;
	dir = 0;

	if ((err = snd_pcm_hw_params_set_period_size_near (rechandle, hw_params, &fpp, &dir)) < 0)
	{
		ZF_LOGE("snd_pcm_hw_params_set_period_size_near failed (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_set_access (rechandle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
	{
		ZF_LOGE("cannot set capture access type (%s)", snd_strerror(err));
		return false;
	}
	if ((err = snd_pcm_hw_params_set_format (rechandle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0)
	{
		ZF_LOGE("cannot set capture sample format (%s)", snd_strerror(err));
		return false;
	}

	if ((err = snd_pcm_hw_params_set_rate (rechandle, hw_params, m_sampleRate, 0)) < 0)
	{
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot set capture sample rate (%s)", snd_strerror(err));
		else
			ZF_LOGE("cannot set capture sample rate (%s)", snd_strerror(err));
		return false;
	}

	m_recchannels = 1;

	if (UseLeftRX == 0 || UseRightRX == 0)
		m_recchannels = 2;  // L/R implies stereo

	if ((err = snd_pcm_hw_params_set_channels (rechandle, hw_params, m_recchannels)) < 0)
	{
		if (ErrorMsg)
			sprintf (ErrorMsg, "cannot set rec channel count to %d (%s)" ,m_recchannels, snd_strerror(err));
		else
			ZF_LOGE("cannot set rec channel count to %d (%s)", m_recchannels, snd_strerror(err));

		if (m_recchannels  == 1)
		{
			m_recchannels = 2;

			if ((err = snd_pcm_hw_params_set_channels (rechandle, hw_params, 2)) < 0)
			{
				ZF_LOGE("cannot set rec channel count to 2 (%s)", snd_strerror(err));
				return false;
			}
			if (ErrorMsg)
				sprintf (ErrorMsg, "Record channel count set to 2 (%s)", snd_strerror(err));
			else
				ZF_LOGI("Record channel count set to 2 (%s)", snd_strerror(err));
		}
	}

	if ((err = snd_pcm_hw_params (rechandle, hw_params)) < 0) {
		fprintf (stderr, "cannot set parameters (%s)", snd_strerror(err));
		return false;
	}

	snd_pcm_hw_params_free(hw_params);

	if ((err = snd_pcm_prepare (rechandle)) < 0) {
		ZF_LOGE("cannot prepare audio interface for use (%s)", snd_strerror(err));
		return FALSE;
	}

	snd_pcm_start(rechandle);  // without this avail stuck at 0

	return TRUE;
}

int OpenSoundCard(char * CaptureDevice, char * PlaybackDevice, int c_sampleRate, int p_sampleRate, char * ErrorMsg)
{
	int Channels = 1;

	if (UseLeftRX == 1 && UseRightRX == 1)
	{
		ZF_LOGI("Using Both Channels of soundcard for RX");
	}
	else
	{
		if (UseLeftRX == 0)
			ZF_LOGI("Using Right Channel of soundcard for RX");
		if (UseRightRX == 0)
			ZF_LOGI("Using Left Channel of soundcard for RX");
	}

	if (UseLeftTX == 1 && UseRightTX == 1)
	{
		ZF_LOGI("Using Both Channels of soundcard for TX");
	}
	else
	{
		if (UseLeftTX == 0)
			ZF_LOGI("Using Right Channel of soundcard for TX");
		if (UseRightTX == 0)
			ZF_LOGI("Using Left Channel of soundcard for TX");
	}

	ZF_LOGI("Opening Playback Device %s Rate %d", PlaybackDevice, p_sampleRate);

	if (UseLeftTX == 0 || UseRightTX == 0)
		Channels = 2;  // L or R implies stereo

	strcpy(SavedPlaybackDevice, PlaybackDevice);  // Saved so we can reopen in error recovery
	SavedPlaybackRate = p_sampleRate;
	if (OpenSoundPlayback(PlaybackDevice, p_sampleRate, Channels, ErrorMsg))
	{
#ifdef SHARECAPTURE

		// Close playback device so it can be shared

		if (playhandle)
		{
			snd_pcm_close(playhandle);
			playhandle = NULL;
		}
#endif
		ZF_LOGI("Opening Capture Device %s Rate %d", CaptureDevice, c_sampleRate);

		strcpy(SavedCaptureDevice, CaptureDevice);  // Saved so we can reopen in error recovery
		SavedCaptureRate = c_sampleRate;
		return OpenSoundCapture(CaptureDevice, c_sampleRate, ErrorMsg);
	}
	else
		return false;
}



int CloseSoundCard()
{
	if (rechandle)
	{
		snd_pcm_close(rechandle);
		rechandle = NULL;
	}

	if (playhandle)
	{
		snd_pcm_close(playhandle);
		playhandle = NULL;
	}
	return 0;
}


int SoundCardWrite(short * input, unsigned int nSamples)
{
	unsigned int i = 0, n;
	int ret, err, res;
	snd_pcm_sframes_t avail, maxavail;
	snd_pcm_status_t *status = NULL;

	if (playhandle == NULL)
		return 0;

	// Stop Capture

	if (rechandle)
	{
		snd_pcm_close(rechandle);
		rechandle = NULL;
	}

	avail = snd_pcm_avail_update(playhandle);
//	Debugprintf("avail before play returned %d", (int)avail);

	if (avail < 0)
	{
		if (avail != -32)
			ZF_LOGD("Playback Avail Recovering from %s ..", snd_strerror((int)avail));
		snd_pcm_recover(playhandle, avail, 1);

		avail = snd_pcm_avail_update(playhandle);

		if (avail < 0)
			ZF_LOGD("avail play after recovery returned %d", (int)avail);
	}

	maxavail = avail;

//	Debugprintf("Samples %d Tosend %d Avail %d", SampleNo, nSamples, (int)avail);

	while (avail < nSamples)
	{
		txSleep(100);
		avail = snd_pcm_avail_update(playhandle);
//		Debugprintf("After Sleep Tosend %d Avail %d", nSamples, (int)avail);
	}

	ret = PackSamplesAndSend(input, nSamples);

	return ret;
}

int PackSamplesAndSend(short * input, int nSamples)
{
	unsigned short samples[256000];
	unsigned short * sampptr = samples;
	unsigned int n;
	int ret;
	snd_pcm_sframes_t avail;

	// Convert byte stream to int16 (watch endianness)

	if (m_playchannels == 1)
	{
		for (n = 0; n < nSamples; n++)
		{
			*(sampptr++) = input[0];
			input ++;
		}
	}
	else
	{
		int i = 0;
		for (n = 0; n < nSamples; n++)
		{
			if (UseLeftRX)
				*(sampptr++) = input[0];
			else
				*(sampptr++) = 0;

			if (UseRightTX)
				*(sampptr++) = input[0];
			else
				*(sampptr++) = 0;

			input ++;
		}
	}

	ret = snd_pcm_writei(playhandle, samples, nSamples);

	if (ret < 0)
	{
//		Debugprintf("Write Recovering from %d ..", ret);
		snd_pcm_recover(playhandle, ret, 1);
		ret = snd_pcm_writei(playhandle, samples, nSamples);
//		Debugprintf("Write after recovery returned %d", ret);
	}

	avail = snd_pcm_avail_update(playhandle);
	return ret;

}
/*
int xSoundCardClearInput()
{
	short samples[65536];
	int n;
	int ret;
	int avail;

	if (rechandle == NULL)
		return 0;

	// Clear queue

	avail = snd_pcm_avail_update(rechandle);

	if (avail < 0)
	{
		Debugprintf("Discard Recovering from %d ..", avail);
		if (rechandle)
		{
			snd_pcm_close(rechandle);
			rechandle = NULL;
		}
		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, NULL);
		avail = snd_pcm_avail_update(rechandle);
	}

	while (avail)
	{
		if (avail > 65536)
			avail = 65536;

			ret = snd_pcm_readi(rechandle, samples, avail);
//			Debugprintf("Discarded %d samples from card", ret);
			avail = snd_pcm_avail_update(rechandle);

//			Debugprintf("Discarding %d samples from card", avail);
	}
	return 0;
}
*/

int SoundCardRead(short * input, unsigned int nSamples)
{
	short samples[65536];
	int n;
	int ret;
	int avail;
	int start;

	if (rechandle == NULL)
		return 0;

	avail = snd_pcm_avail_update(rechandle);

	if (avail < 0)
	{
		ZF_LOGD("avail Recovering from %s ..", snd_strerror((int)avail));
		if (rechandle)
		{
			snd_pcm_close(rechandle);
			rechandle = NULL;
		}

		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, NULL);
//		snd_pcm_recover(rechandle, avail, 0);
		avail = snd_pcm_avail_update(rechandle);
		ZF_LOGD("After avail recovery %d ..", avail);
	}

	if (avail < nSamples)
		return 0;

//	Debugprintf("ALSARead available %d", avail);

	ret = snd_pcm_readi(rechandle, samples, nSamples);

	if (ret < 0)
	{
		ZF_LOGE("RX Error %d", ret);
//		snd_pcm_recover(rechandle, avail, 0);
		if (rechandle)
		{
			snd_pcm_close(rechandle);
			rechandle = NULL;
		}

		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, NULL);
//		snd_pcm_recover(rechandle, avail, 0);
		avail = snd_pcm_avail_update(rechandle);
		ZF_LOGD("After Read recovery Avail %d ..", avail);

		return 0;
	}

	if (m_recchannels == 1)
	{
		for (n = 0; n < ret; n++)
		{
			memcpy(input, samples, nSamples*sizeof(short));
		}
	}
	else
	{
		if (UseLeftRX)
			start = 0;
		else
			start = 1;

		for (n = start; n < (ret * 2); n+=2)  // return alternate
		{
			input[n] = samples[n];
		}
	}
	return ret;
}




int PriorSize = 0;

int Index = 0;  // DMA Buffer being used 0 or 1
int inIndex = 0;  // DMA Buffer being used 0 or 1

BOOL DMARunning = FALSE;  // Used to start DMA on first write

short * SendtoCard(short * buf, int n)
{
	if (Loopback)
	{
		// Loop back   to decode for testing

		ProcessNewSamples(buf, 1200);  // signed
	}

	if (playhandle)
		SoundCardWrite(&buffer[Index][0], n);
	if (txwff != NULL)
		WriteWav(&buffer[Index][0], n, txwff);

//	txSleep(10);  // Run buckground while waiting

	return &buffer[Index][0];
}

short loopbuff[1200];  // Temp for testing - loop sent samples to decoder


// This generates a nice musical pattern for sound interface testing
// for (t = 0; t < sizeof(buffer); ++t)
//  buffer[t] =((((t * (t >> 8 | t >> 9) & 46 & t >> 8)) ^ (t & t >> 13 | t >> 6)) & 0xFF);



int InitSound(BOOL Quiet)
{
	if (strcmp(PlaybackDevice, "NOSOUND") == 0)
		return (1);
	GetInputDeviceCollection();
	GetOutputDeviceCollection();
	return OpenSoundCard(CaptureDevice, PlaybackDevice, 12000, 12000, NULL);
}

int min = 0, max = 0, lastlevelreport = 0, lastlevelGUI = 0;
UCHAR CurrentLevel = 0;  // Peak from current samples


void PollReceivedSamples()
{
	if (strcmp(PlaybackDevice, "NOSOUND") == 0)
		return;

	// Process any captured samples
	// Ideally call at least every 100 mS, more than 200 will loose data

	if (SoundCardRead(&inbuffer[0][0], ReceiveSize))
	{
		// returns ReceiveSize or none

		short * ptr = &inbuffer[0][0];
		int i;

		if (add_noise(&inbuffer[0][0], ReceiveSize, InputNoiseStdDev) > 0) {
			max = 32767;
			min = -32768;
		} else {
			for (i = 0; i < ReceiveSize; i++)
			{
				if (*(ptr) < min)
					min = *ptr;
				else if (*(ptr) > max)
					max = *ptr;
				ptr++;
			}
		}

		CurrentLevel = ((max - min) * 75) /32768;  // Scale to 150 max
		wg_send_currentlevel(0, CurrentLevel);

		if ((Now - lastlevelGUI) > 2000)  // 2 Secs
		{
			if (WaterfallActive == 0 && SpectrumActive == 0)  // Don't need to send as included in Waterfall Line
				SendtoGUI('L', &CurrentLevel, 1);  // Signal Level

			lastlevelGUI = Now;

			if ((Now - lastlevelreport) > 10000)  // 10 Secs
			{
				lastlevelreport = Now;
				// Report input peaks to host and log if CONSOLELOG is ZF_LOG_DEBUG (2) or ZF_LOG_VERBOSE (1) or if close to clipping
				// TODO: Are these good conditions for logging (and sending to host) Input Peaks values?
				// Conditions were changed with the introduction of ZF_LOG, but are now restored.
				if (max >= 32000 || ardop_log_get_level_console() <= ZF_LOG_DEBUG)
				{
					char HostCmd[64] = "";
					snprintf(HostCmd, sizeof(HostCmd), "INPUTPEAKS %d %d", min, max);
					SendCommandToHostQuiet(HostCmd);
					ZF_LOGD("Input peaks = %d, %d", min, max);
					// A user with the default of CONSOLELOG = ZF_LOG_INFO will see this message if they are close to clipping
					if (ardop_log_get_level_console() > ZF_LOG_DEBUG)
					{
						ZF_LOGI(
							"Your input signal is probably clipping. If you see"
							" this message repeated in the next 20-30 seconds,"
							" Turn down your RX input until this message stops"
							" repeating."
						);
					}
				}
			}
			min = max = 0;  // Every 2 secs
		}

		if (rxwf != NULL)
		{
			// There is an open Wav file recording.
			// Either close it or write samples to it.
			if (rxwf_EndNow < Now)
			{
				CloseWav(rxwf);
				rxwf = NULL;
			}
			else
				WriteWav(&inbuffer[0][0], ReceiveSize, rxwf);
		}


		if (Capturing && Loopback == FALSE)
			ProcessNewSamples(&inbuffer[0][0], ReceiveSize);
	}
}

void StopCapture()
{
	Capturing = FALSE;

	if (strcmp(PlaybackDevice, "NOSOUND") == 0)
		return;

#ifdef SHARECAPTURE

	// Stopcapture is only called when we are about to transmit, so use it to open plaback device. We don't keep
	// it open all the time to facilitate sharing.

	OpenSoundPlayback(SavedPlaybackDevice, SavedPlaybackRate, Savedplaychannels, NULL);
#endif
}

void StartCodec(char * strFault)
{
	strFault[0] = 0;
	OpenSoundCard(CaptureDevice, PlaybackDevice, 12000, 12000, strFault);
}

void StopCodec(char * strFault)
{
	strFault[0] = 0;
	CloseSoundCard();
}

void StartCapture()
{
	Capturing = TRUE;
	DiscardOldSamples();
	ClearAllMixedSamples();
	State = SearchingForLeader;

//	Debugprintf("Start Capture");
}
void CloseSound()
{
	CloseSoundCard();
}

VOID WriteSamples(short * buffer, int len)
{

#ifdef WIN32
	fwrite(buffer, 1, len * 2, wavfp1);
#endif
}

unsigned short * SoundInit()
{
	Index = 0;
	return &buffer[0][0];
}

// Called at end of transmission

void SoundFlush()
{
	// Append Trailer then send remaining samples

	snd_pcm_status_t *status = NULL;
	int err, res;
	char strFault[100] = "";
	int count = 100;
	int lastavail = 0;
	int txlenMs = 0;
	snd_pcm_sframes_t avail;

	AddTrailer();  // add the trailer.

	if (Loopback)
		ProcessNewSamples(&buffer[Index][0], Number);

	SendtoCard(&buffer[Index][0], Number);

	// Wait for tx to complete

	// samples sent is is in SampleNo, time started in mS is in pttOnTime.
	// calculate time to stop

	txlenMs = SampleNo / 12 + 20;  // 12000 samples per sec. 20 mS TXTAIL

	ZF_LOGD("Tx Time %d Time till end = %d", txlenMs, (pttOnTime + txlenMs) - Now);

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		while (Now < (pttOnTime + txlenMs))
		{
			usleep(2000);
		}

/*

	while (count-- && playhandle)
	{
		snd_pcm_sframes_t avail = snd_pcm_avail_update(playhandle);

		Debugprintf("Waiting for complete. Avail %d Max %d last %d", avail, MaxAvail, lastavail);

		snd_pcm_status_alloca(&status);  // alloca allocates once per function, does not need a free

		if ((err=snd_pcm_status(playhandle, status))!=0)
		{
			Debugprintf("snd_pcm_status() failed: %s",snd_strerror(err));
			break;
		}

		res = snd_pcm_status_get_state(status);

		Debugprintf("PCM Status = %d", res);

		// Some cards seem to stop sending but not report not running, so also check that avail is decreasing

		if (res != SND_PCM_STATE_RUNNING || lastavail == avail)  // If sound system is not running then it needs data
//		if (MaxAvail - avail < 100)
		{
			// Send complete - Restart Capture
			break;
		}
		lastavail = avail;
		usleep(50000);
	}
*/

	// I think we should turn round the link here. I dont see the point in
	// waiting for MainPoll

#ifdef SHARECAPTURE
	if (strcmp(PlaybackDevice, "NOSOUND") != 0) {
		if (playhandle)
		{
			snd_pcm_close(playhandle);
			playhandle = NULL;
		}
	}
#endif
	SoundIsPlaying = FALSE;

	if (blnEnbARQRpt > 0 || blnDISCRepeating)  // Start Repeat Timer if frame should be repeated
		dttNextPlay = Now + intFrameRepeatInterval + extraDelay;

	KeyPTT(FALSE);  // Unkey the Transmitter
	if (txwff != NULL)
	{
		CloseWav(txwff);
		txwff = NULL;
	}
	// writing unfiltered tx audio to WAV disabled
	// if (txwfu != NULL)
	// {
		// CloseWav(txwfu);
		// txwfu = NULL;
	// }

	if (strcmp(PlaybackDevice, "NOSOUND") != 0)
		OpenSoundCapture(SavedCaptureDevice, SavedCaptureRate, strFault);
	StartCapture();

	if (WriteRxWav)
		// Start recording if not already recording, else to extend the recording time.
		StartRxWav();
	return;
}

VOID RadioPTT(int PTTState)
{
#ifdef __ARM_ARCH
	if (useGPIO)
		gpioWrite(pttGPIOPin, (pttGPIOInvert ? (1-PTTState) : (PTTState)));
	else
#endif
	{
		if (PTTMode & PTTRTS)
			if (PTTState)
				COMSetRTS(hPTTDevice);
			else
				COMClearRTS(hPTTDevice);

		if (PTTMode & PTTDTR)
			if (PTTState)
				COMSetDTR(hPTTDevice);
			else
				COMClearDTR(hPTTDevice);

		if (PTTMode & PTTCI_V)
			if (PTTState)
				WriteCOMBlock(hCATDevice, PTTOnCmd, PTTOnCmdLen);
			else
				WriteCOMBlock(hCATDevice, PTTOffCmd, PTTOffCmdLen);

		if (PTTMode & PTTCM108)
			CM108_set_ptt(PTTState);
	}
}

// Function to send PTT TRUE or PTT FALSE commanad to Host or if local Radio control Keys radio PTT

const char BoolString[2][6] = {"FALSE", "TRUE"};

BOOL KeyPTT(BOOL blnPTT)
{
	// Returns TRUE if successful False otherwise

	if (blnLastPTT &&  !blnPTT)
		dttStartRTMeasure = Now;  // start a measurement on release of PTT.

	if (!RadioControl)
		if (blnPTT)
			SendCommandToHostQuiet("PTT TRUE");
		else
			SendCommandToHostQuiet("PTT FALSE");

	else
		RadioPTT(blnPTT);

	ZF_LOGD("[Main.KeyPTT]  PTT-%s", BoolString[blnPTT]);

	blnLastPTT = blnPTT;
	SetLED(0, blnPTT);
	wg_send_pttled(0, blnPTT);
	return TRUE;
}


static struct speed_struct
{
	int	user_speed;
	speed_t termios_speed;
} speed_table[] = {
	{300, B300},
	{600, B600},
	{1200, B1200},
	{2400, B2400},
	{4800, B4800},
	{9600, B9600},
	{19200, B19200},
	{38400, B38400},
	{57600, B57600},
	{115200, B115200},
	{-1, B0}
};



// GPIO access stuff for PTT on PI

#ifdef __ARM_ARCH

/*
	tiny_gpio.c
	2016-04-30
	Public Domain
*/
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define GPSET0 7
#define GPSET1 8

#define GPCLR0 10
#define GPCLR1 11

#define GPLEV0 13
#define GPLEV1 14

#define GPPUD 37
#define GPPUDCLK0 38
#define GPPUDCLK1 39

unsigned piModel;
unsigned piRev;

static volatile uint32_t  *gpioReg = MAP_FAILED;

#define PI_BANK (gpio>>5)
#define PI_BIT  (1<<(gpio&0x1F))

/* gpio modes. */

void gpioSetMode(unsigned gpio, unsigned mode)
{
	int reg, shift;

	reg   =  gpio/10;
	shift = (gpio%10) * 3;

	gpioReg[reg] = (gpioReg[reg] & ~(7<<shift)) | (mode<<shift);
}

int gpioGetMode(unsigned gpio)
{
	int reg, shift;

	reg   =  gpio/10;
	shift = (gpio%10) * 3;

	return (*(gpioReg + reg) >> shift) & 7;
}

// Values for pull-ups/downs off, pull-down and pull-up.

#define PI_PUD_OFF  0
#define PI_PUD_DOWN 1
#define PI_PUD_UP   2

void gpioSetPullUpDown(unsigned gpio, unsigned pud)
{
	*(gpioReg + GPPUD) = pud;

	usleep(20);

	*(gpioReg + GPPUDCLK0 + PI_BANK) = PI_BIT;

	usleep(20);

	*(gpioReg + GPPUD) = 0;

	*(gpioReg + GPPUDCLK0 + PI_BANK) = 0;
}

int gpioRead(unsigned gpio)
{
	if ((*(gpioReg + GPLEV0 + PI_BANK) & PI_BIT) != 0)
		return 1;
	else
		return 0;
}
void gpioWrite(unsigned gpio, unsigned level)
{
	if (level == 0)
		*(gpioReg + GPCLR0 + PI_BANK) = PI_BIT;
	else
		*(gpioReg + GPSET0 + PI_BANK) = PI_BIT;
}

void gpioTrigger(unsigned gpio, unsigned pulseLen, unsigned level)
{
	if (level == 0)
		*(gpioReg + GPCLR0 + PI_BANK) = PI_BIT;
	else
		*(gpioReg + GPSET0 + PI_BANK) = PI_BIT;

	usleep(pulseLen);

	if (level != 0)
		*(gpioReg + GPCLR0 + PI_BANK) = PI_BIT;
	else
		*(gpioReg + GPSET0 + PI_BANK) = PI_BIT;
}

// Bit (1<<x) will be set if gpio x is high.

uint32_t gpioReadBank1(void) {
	return (*(gpioReg + GPLEV0));
}
uint32_t gpioReadBank2(void) {
	return (*(gpioReg + GPLEV1));
}

// To clear gpio x bit or in (1<<x).

void gpioClearBank1(uint32_t bits) {
	*(gpioReg + GPCLR0) = bits;
}
void gpioClearBank2(uint32_t bits) {
	*(gpioReg + GPCLR1) = bits;
}

// To set gpio x bit or in (1<<x).

void gpioSetBank1(uint32_t bits) {
	*(gpioReg + GPSET0) = bits;
}
void gpioSetBank2(uint32_t bits) {
	*(gpioReg + GPSET1) = bits;
}

unsigned gpioHardwareRevision(void)
{
	static unsigned rev = 0;

	FILE * filp;
	char buf[512];
	char term;
	int chars=4;  // number of chars in revision string

	if (rev)
		return rev;

	piModel = 0;

	filp = fopen ("/proc/cpuinfo", "r");

	if (filp != NULL)
	{
		while (fgets(buf, sizeof(buf), filp) != NULL)
		{
			if (piModel == 0)
			{
				if (!strncasecmp("model name", buf, 10))
				{
					if (strstr (buf, "ARMv6") != NULL)
					{
						piModel = 1;
						chars = 4;
					}
					else if (strstr (buf, "ARMv7") != NULL)
					{
						piModel = 2;
						chars = 6;
					}
					else if (strstr (buf, "ARMv8") != NULL)
					{
						piModel = 2;
						chars = 6;
					}
				}
			}

			if (!strncasecmp("revision", buf, 8))
			{
				if (sscanf(buf+strlen(buf)-(chars+1),
					"%x%c", &rev, &term) == 2)
				{
					if (term != '\n')
						rev = 0;
				}
			}
		}

		fclose(filp);
	}
	return rev;
}

int gpioInitialise(void)
{
	int fd;

	piRev = gpioHardwareRevision();  // sets piModel and piRev

	fd = open("/dev/gpiomem", O_RDWR | O_SYNC) ;

	if (fd < 0)
	{
		fprintf(stderr, "failed to open /dev/gpiomem\n");
		return -1;
	}

	gpioReg = (uint32_t *)mmap(NULL, 0xB4, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

	close(fd);

	if (gpioReg == MAP_FAILED)
	{
		fprintf(stderr, "Bad, mmap failed\n");
		return -1;
	}
	return 0;
}



#endif



int stricmp(const unsigned char * pStr1, const unsigned char *pStr2)
{
	unsigned char c1, c2;
	int  v;

	if (pStr1 == NULL)
	{
		if (pStr2)
			ZF_LOGW("stricmp called with NULL 1st param - 2nd %s ", pStr2);
		else
			ZF_LOGW("stricmp called with two NULL params");

		return 1;
	}


	do {
		c1 = *pStr1++;
		c2 = *pStr2++;
		// The casts are necessary when pStr1 is shorter & char is signed
		v = tolower(c1) - tolower(c2);
	} while ((v == 0) && (c1 != '\0') && (c2 != '\0') );

	return v;
}

char Leds[8]= {0};
unsigned int PKTLEDTimer = 0;

void SetLED(int LED, int State)
{
	// If GUI active send state

	Leds[LED] = State;
	SendtoGUI('D', Leds, 8);
}

void DrawTXMode(const char * Mode)
{
	unsigned char Msg[64];
	strcpy(Msg, Mode);
	SendtoGUI('T', Msg, strlen(Msg) + 1);  // TX Frame

}

void DrawTXFrame(const char * Frame)
{
	unsigned char Msg[64];
	strcpy(Msg, Frame);
	SendtoGUI('T', Msg, strlen(Msg) + 1);  // TX Frame
}

void DrawRXFrame(int State, const char * Frame)
{
	unsigned char Msg[64];

	Msg[0] = State;  // Pending/Good/Bad
	strcpy(&Msg[1], Frame);
	SendtoGUI('R', Msg, strlen(Frame) + 1);  // RX Frame
}
// mySetPixel() uses 3 bytes from Pixels per call.  So it must be 3 times the
// size of the larger of inPhases[0] or intToneMags/4. (intToneMags/4 is larger)
UCHAR Pixels[9108];
UCHAR * pixelPointer = Pixels;


// This data may be copied and pasted from the debug log file into the
// "Host Command" input box in the WebGui in developer mode to reproduce
// the constellation plot.
void LogConstellation() {
	char Msg[10000] = "CPLOT ";
	for (int i = 0; i < pixelPointer - Pixels; i++)
		snprintf(Msg + strlen(Msg), sizeof(Msg) - strlen(Msg), "%02X", Pixels[i]);
	ZF_LOGV("%s", Msg);
}


void mySetPixel(unsigned char x, unsigned char y, unsigned int Colour)
{
	// Used on Windows for constellation. Save points and send to GUI at end

	*(pixelPointer++) = x;
	*(pixelPointer++) = y;
	*(pixelPointer++) = Colour;
}
void clearDisplay()
{
	// Reset pixel pointer

	pixelPointer = Pixels;

}
void updateDisplay()
{
//	 SendtoGUI('C', Pixels, pixelPointer - Pixels);
}
void DrawAxes(int Qual, char * Mode)
{
	UCHAR Msg[80];
	SendtoGUI('C', Pixels, pixelPointer - Pixels);
	wg_send_pixels(0, Pixels, pixelPointer - Pixels);
	LogConstellation();
	pixelPointer = Pixels;

	sprintf(Msg, "%s Quality: %d", Mode, Qual);
	SendtoGUI('Q', Msg, strlen(Msg) + 1);
}
void DrawDecode(char * Decode)
{}
