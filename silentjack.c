/*

	silentjack.c
	Silence/dead air detector for JACK
	Copyright (C) 2006  Nicholas J. Humfrey
	
	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

//tb/120922/130529
//osc

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jack/jack.h>
#include <getopt.h>
#include "config.h"
#include "db.h"

#include "lo/lo.h"

#include <signal.h>

#define DEFAULT_CLIENT_NAME		"silentjack"

// *** Globals ***
jack_port_t *input_port = NULL;			// Our single jack input port
float peak = 0.0f;				// Current peak signal level (linear)
int running = 1;				// SilentJack keeps running while true
int quiet = 0;					// If true, don't send messages to stdout
int verbose = 0;				// If true, send more messages to stdout
int verbose_osc = 0;				// If true, send more osc messages

int enable_osc = 1;				//if true, send OSC messages
const char* osc_my_server_port = "7777";	//default port to listen to OSC Messages 
const char* osc_send_to_host = "127.0.0.1";	//default host to send OSC messages 
const char* osc_send_to_port = "7778";		//default port to send OSC messages 

lo_address loa;
lo_server_thread st;

#define STATUS_UNDEFINED	0
#define STATUS_READY		1
#define STATUS_NOT_CONNECTED	2
#define STATUS_CONNECTED	3
#define STATUS_LEVEL_BELOW	4
#define STATUS_LEVEL_ABOVE	5
#define STATUS_SILENCE		6
#define STATUS_RUN_COMMAND	7
#define STATUS_GRACE		8
#define STATUS_QUIT		9

//RUN CMD, QUIT

int status_=STATUS_UNDEFINED;

int status_int_=0;
float status_float_=0;

const char* _client_name = DEFAULT_CLIENT_NAME;

jack_client_t *client = NULL;
//const char* client_name = DEFAULT_CLIENT_NAME;
const char* connect_port = NULL;
float peakdb = 0.0f;			// The current peak signal level (in dB)
float last_peakdb = 0.0f;		// The previous peak signal level (in dB)
int silence_period = 1;			// Required period of silence for trigger
int nodynamic_period = 10;		// Required period of no-dynamic for trigger
int grace_period = 0;			// Period to wait before triggering again
float silence_threshold = -40;		// Level considered silent (in dB)
float nodynamic_threshold = 0;		// Minimum allowed delta between peaks (in dB)
int silence_count = 0;			// Number of seconds of silence detected
int nosilence_count = 0;		// Number of seconds of nosilence detected

int nodynamic_count = 0;		// Number of seconds of no-dynamic detected
int in_grace = 0;			// Number of seconds left in grace

/* Read and reset the recent peak sample */
static
float read_peak()
{
	float peakdb = lin2db(peak);
	peak = 0.0f;

	return peakdb;
}


/* Callback called by JACK when audio is available.
   Stores value of peak sample */
static
int process_peak(jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *in;
	unsigned int i;

	/* just incase the port isn't registered yet */
	if (input_port == NULL) {
		return 0;
	}

	/* get the audio samples, and find the peak sample */
	in = (jack_default_audio_sample_t *) jack_port_get_buffer(input_port, nframes);
	for (i = 0; i < nframes; i++) {
		const float s = fabs(in[i]);
		if (s > peak) {
			peak = s;
		}
	}

	return 0;
}

/* Connect the chosen port to ours */
static
void connect_jack_port( jack_client_t *client, jack_port_t *port, const char* out )
{
	const char* in = jack_port_name( port );
	int err;
		
	if (!quiet) printf("Connecting %s to %s\n", out, in);
	
	if ((err = jack_connect(client, out, in)) != 0) {
		fprintf(stderr, "connect_jack_port(): failed to jack_connect() ports: %d\n",err);
		exit(1);
	}
}

static
void shutdown_callback_jack(void *arg)
{
	status_=STATUS_QUIT;

	if (enable_osc) send_osc_status();

	running = 0;
}

static
jack_client_t* init_jack( const char * client_name, const char* connect_port ) 
{
	jack_status_t status;
	jack_options_t options = JackNoStartServer;
	jack_client_t *client = NULL;

	// Register with Jack
	if ((client = jack_client_open(client_name, options, &status)) == 0) {
		fprintf(stderr, "Failed to start jack client: %d\n", status);
		exit(1);
	}
	if (!quiet) printf("JACK client registered as '%s'.\n", jack_get_client_name( client ) );
	_client_name=jack_get_client_name( client );

	// Create our pair of output ports
	if (!(input_port = jack_port_register(client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
		fprintf(stderr, "Cannot register input port 'in'.\n");
		exit(1);
	}
	
	// Register shutdown callback
	jack_on_shutdown (client, shutdown_callback_jack, NULL );

	// Register the peak audio callback
	jack_set_process_callback(client, process_peak, 0);

	// Activate the client
	if (jack_activate(client)) {
		fprintf(stderr, "Cannot activate client.\n");
		exit(1);
	}
	
	// Connect up our input port ?
	if (connect_port) {
		connect_jack_port( client, input_port, connect_port );
	}
	
	return client;
}

static
void finish_jack( jack_client_t *client )
{
	// Leave the Jack graph
	jack_client_close(client);
}

#define MAX_LEN 256
char *concat_args(int ac, char **av) 
{
	char buffer[MAX_LEN];
	buffer[0] = 0;
	int offset = 0;
	while(av++,--ac) 
	{
		int toWrite = MAX_LEN-offset;
		int written = snprintf(buffer+offset, toWrite, "%s ", *av);
		if(toWrite < written) 
		{
			break;
		}
		offset += written;
	}
	//printf("cmd args: %s\n", buffer);
	return buffer;
}

static
void run_command( int argc, char* argv[] )
{
	pid_t child;
	int status;
	
	// No command to execute
	if (argc<1) return;

	status_=STATUS_RUN_COMMAND;

   	if (verbose) printf("running command:\n%s %s\n", argv[0],concat_args(argc,argv));

	if (enable_osc) send_osc_status_command(argc,argv);

	// Exit successfully if command is called "exit"
	if (argc==1 && strcmp(argv[0], "exit")==0) exit(0);

	// Fork new process
	child = fork();
	if (child==0) {
		// Child process here
		if (execvp( argv[0], argv )) {
			perror("execvp failed");
			exit(-1);
		}
	} else if (child==-1) {
		// Fork failed
		perror("fork failed");
		exit(-1);
	}
	
	// Wait for process to end
	if (waitpid( child, &status, 0)==-1) {
		perror("waitpid failed");
	}
}

/* Display how to use this program */
static
void usage()
{
	printf("%s version %s OSC\n\n", PACKAGE_NAME, PACKAGE_VERSION);
	printf("Usage: silentjack [options] [COMMAND [ARG]...]\n");
	printf("Options:  -c <port>   Connect to this port\n");
	printf("          -n <name>   Name of this client (default 'silentjack')\n");
	printf("          -l <db>     Trigger level (default -40 decibels)\n");
	printf("          -p <secs>   Period of silence required (default 1 second)\n");
	printf("          -d <db>     No-dynamic trigger level (default disabled)\n");
	printf("          -P <secs>   No-dynamic period (default 10 seconds)\n");
	printf("          -g <secs>   Grace period (default 0 seconds)\n");
	printf("          -v          Enable verbose mode\n");
	printf("          -q          Enable quiet mode\n");
	printf("          -o <port>   Set OSC Port for listening (default 7777, ? for random)\n");
	printf("          -H <port>   Set OSC Host to send to (default 127.0.0.1)\n");
	printf("          -O <port>   Set OSC Port to send to (default 7778)\n");
	printf("          -V          Enable OSC verbose mode\n");
	printf("          -X          Disable all OSC\n");
	printf("          -h          Show help\n");

	exit(1);
}

void error(int num, const char *m, const char *path);

int get_status_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);
int get_settings_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);

int set_trigger_level_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);
int set_silence_period_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);
int set_grace_period_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);
int set_verbose_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);

int quit_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data);

void sig_handler(int i)
{
	printf("shutting down...\n");

	status_=STATUS_QUIT;

	if (enable_osc) send_osc_status();

	exit(0);
}

int main(int argc, char *argv[])
{
	//variables moved from here to global/member

	int opt;

	// Make STDOUT unbuffered
	setbuf(stdout, NULL);

	// Parse command line arguments
	while ((opt = getopt(argc, argv, "c:n:o:H:O:l:p:P:d:g:vqXVh")) != -1) {
		switch (opt) {
			case 'c': connect_port = optarg; break;
			case 'n': _client_name = optarg; break;
			case 'o': osc_my_server_port = optarg; break;
			case 'H': osc_send_to_host = optarg; break;
			case 'O': osc_send_to_port = optarg; break;
			case 'l': silence_threshold = atof(optarg); break;
			case 'p': silence_period = fabs(atoi(optarg)); break;
			case 'd': nodynamic_threshold = atof(optarg); break;
			case 'P': nodynamic_period = atof(optarg); break;
			case 'g': grace_period = fabs(atoi(optarg)); break;
			case 'v': verbose = 1; break;
			case 'q': quiet = 1; break;
			case 'X': enable_osc = 0; break;
			case 'V': verbose_osc = 1; break;
			case 'h':
			default:
				/* Show usage information */
				usage();
				break;
		}
	}

	argc -= optind;
	argv += optind;

	signal(SIGINT,sig_handler);
	signal(SIGTERM,sig_handler);
	//signal(SIGKILL,sig_handler);

	// Validate parameters
	if (quiet && verbose) {
		fprintf(stderr, "Can't be quiet and verbose at the same time.\n");
		usage();
	}

	// Initialise Jack
	client = init_jack( _client_name, connect_port );

	if(enable_osc)
	{

		/* start osc server on requested port (?: random) */
		if (strcmp (osc_my_server_port, "?") == 0)
		{
			st = lo_server_thread_new(NULL, error);
		}
		else
		{
			st = lo_server_thread_new(osc_my_server_port, error);
		}

		lo_server_thread_add_method(st, "/silentjack/set_trigger_level", "fs", set_trigger_level_handler, NULL);
		lo_server_thread_add_method(st, "/silentjack/set_silence_period", "is", set_silence_period_handler, NULL);
		lo_server_thread_add_method(st, "/silentjack/set_grace_period", "is", set_grace_period_handler, NULL);
		lo_server_thread_add_method(st, "/silentjack/set_verbose", "is", set_verbose_handler, NULL);

		lo_server_thread_add_method(st, "/silentjack/get_status", "s", get_status_handler, NULL);
		lo_server_thread_add_method(st, "/silentjack/get_settings", "s", get_settings_handler, NULL);
		lo_server_thread_add_method(st, "/silentjack/quit", "", quit_handler, NULL);
		
		lo_server_thread_start(st);

		//read back port (specified, default or random)
		char c[10];
		sprintf(c,"%d",lo_server_thread_get_port(st));
		osc_my_server_port=c;

		loa = lo_address_new(osc_send_to_host, osc_send_to_port);

		status_=STATUS_READY;

		if (verbose) 
		{ 
			fprintf(stderr, "Listening for incoming OSC messages on port %s\n",osc_my_server_port);
			fprintf(stderr, "Sending OSC to %s, port %s\n",osc_send_to_host,osc_send_to_port);
		}

		send_osc_status();

	} //end if enable_osc
		
	//int tell_nc=1;
	int tell_con=1;

	// Main loop
	while (running) {
	
		// Sleep for 1 second
		usleep( 500000 );
		//...
		usleep( 500000 );

		// Are we in grace period ?
		if (in_grace) {
			in_grace--;

			status_=STATUS_GRACE;
			status_int_=in_grace;
			status_float_=0;

			if (verbose) printf("%d seconds left in grace period.\n", in_grace);

			if (verbose_osc && enable_osc) send_osc_status();

			continue;
		}

		// Check we are connected to something
		if (jack_port_connected(input_port)==0) {

			status_=STATUS_NOT_CONNECTED;
			status_int_=0;
			status_float_=0;

			if (verbose)// && tell_nc) 
			{
				printf("Input port isn't connected to anything.\n");
			}

			if (verbose_osc && enable_osc) send_osc_status(); // && tell_nc) 

			//tell_nc=0;
			tell_con=1;

			continue;
		}
		else
		{
			if (tell_con) status_=STATUS_CONNECTED;

			if (verbose && tell_con) printf("Input port connected.\n");

			if (verbose_osc && enable_osc && tell_con) send_osc_status();

			tell_con=0;
			//tell_nc=1;
		}
	
		// Read the recent peak (in decibels)
		last_peakdb = peakdb;
		peakdb = read_peak();
			
		// Do silence detection?
		if (silence_threshold) {
			if (verbose) printf("peak: %2.2fdB", peakdb);

			// Is peak too low?
			if (peakdb < silence_threshold) {
				nosilence_count=0;
				silence_count++;

				status_=STATUS_LEVEL_BELOW;
				status_int_=silence_count;
				status_float_=peakdb;

				if (verbose) printf(" (%d seconds of silence)\n", silence_count);
		
				if (verbose_osc && enable_osc) send_osc_status();

			} else {
				if (verbose) printf(" (not silent)\n");
				nosilence_count++;
				silence_count=0;

				status_=STATUS_LEVEL_ABOVE;
				status_int_=nosilence_count;
				status_float_=peakdb;		

				if (verbose_osc && enable_osc) send_osc_status();
			}

			// Have we had enough seconds of silence?
			if (silence_count >= silence_period) {
				if (!quiet) 
				{
					printf("**SILENCE**\n");
					
					status_=STATUS_SILENCE;
					//status_int_ secs from level_below
					status_float_=peakdb;
				}

				if(enable_osc) send_osc_status();

				run_command( argc, argv );
				silence_count = 0;
				in_grace = grace_period;
			}
			
		}
		
		// Do no-dynamic detection
		if (nodynamic_threshold) {
			
			if (verbose) printf("delta: %2.2fdB", fabs(last_peakdb-peakdb));
			
			// Check the dynamic/delta between peaks
			if (fabs(last_peakdb-peakdb) < nodynamic_threshold) {
				nodynamic_count++;
				if (verbose) printf(" (%d seconds of no dynamic)\n", nodynamic_count);
			} else {
				if (verbose) printf(" (dynamic)\n");
				nodynamic_count=0;
			}
	
			// Have we had enough seconds of no dynamic?
			if (nodynamic_count >= nodynamic_period) {
				if (!quiet) printf("**NO DYNAMIC**\n");
				run_command( argc, argv );
				nodynamic_count = 0;
				in_grace = grace_period;
			}
	
		}
		
	} //end while running

	// Clean up
	finish_jack( client );

	return 0;
}

int quit_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	if (verbose) printf("quiting\n\n");
	fflush(stdout);

	status_=STATUS_QUIT;

	if (enable_osc) send_osc_status();

	running=0;
	return 0;
}

void send_osc_status()
{
	lo_message reply=lo_message_new();

	lo_message_add_string(reply,_client_name);
	lo_message_add_string(reply,osc_my_server_port);

	switch ( status_ ) 
	{
	case STATUS_READY:
		lo_message_add_int32(reply,silence_period);
		lo_message_add_int32(reply,grace_period);
		lo_message_add_float(reply,silence_threshold);
		lo_message_add_int32(reply,verbose_osc);
		lo_send_message(loa, "/silentjack/started", reply);
		break;
	case STATUS_NOT_CONNECTED:
		lo_send_message (loa, "/silentjack/not_connected", reply);
		break;
	case STATUS_CONNECTED:
		lo_send_message (loa, "/silentjack/connected", reply);
		break;
	case STATUS_LEVEL_BELOW:
		lo_message_add_int32(reply,0);
		lo_message_add_int32(reply,status_int_);
		lo_message_add_float(reply,status_float_);
		lo_send_message (loa, "/silentjack/level", reply);
		break;
	case STATUS_LEVEL_ABOVE:
		lo_message_add_int32(reply,1);
		lo_message_add_int32(reply,status_int_);
		lo_message_add_float(reply,status_float_);
		lo_send_message (loa, "/silentjack/level", reply);
		break;
	case STATUS_SILENCE:
		lo_message_add_float(reply,status_float_);
		lo_send_message (loa, "/silentjack/silent", reply);
		break;
//	case STATUS_RUN_COMMAND:
//		break;
	case STATUS_GRACE:
		lo_message_add_int32(reply,status_int_);
		lo_send_message (loa, "/silentjack/grace", reply);
		break;
	case STATUS_QUIT:
		lo_send_message (loa, "/silentjack/quit", reply);
		break;
	default:
		lo_send_message (loa, "/silentjack/unknown_status", reply);
		break;

	lo_message_free (reply);
	} //end switch statement

}//end send_osc_status

void send_osc_status_command(int argc, char* argv[])
{
	lo_message reply=lo_message_new();

	lo_message_add_string(reply,_client_name);
	lo_message_add_string(reply,osc_my_server_port);

	int n = 0;
	for (n = 0; n < argc; ++n) 
	{
		lo_message_add_string (reply, argv[n]);
	}
	lo_send_message (loa, "/silentjack/run_cmd", reply);
	lo_message_free (reply);
}

int get_status_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	send_osc_settings(&argv[0]->s);

	return 0;
}

void send_osc_settings_()
{
	char* unknown="";
	send_osc_settings(unknown);
}

void send_osc_settings(char* remote_id)
{
	lo_message reply=lo_message_new();

	lo_message_add_string(reply,_client_name);
	lo_message_add_string(reply,osc_my_server_port);
	lo_message_add_int32(reply,silence_period);
	lo_message_add_int32(reply,grace_period);
	lo_message_add_float(reply,silence_threshold);
	lo_message_add_int32(reply,verbose_osc);
	lo_message_add_string(reply,remote_id);

	lo_send_message(loa, "/silentjack/settings", reply);
	lo_message_free(reply);
}

int get_settings_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	send_osc_settings(&argv[0]->s);

	return 0;
}

int set_trigger_level_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	float level=argv[0]->f;
	
	if(level<=-1 && level>-100) silence_threshold=level;

	send_osc_settings(&argv[1]->s);

	return 0;
}

int set_silence_period_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	if(argv[0]->i >0) silence_period=argv[0]->i;
	send_osc_settings(&argv[1]->s);

	return 0;
}

int set_grace_period_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	if(argv[0]->i >=0) grace_period=argv[0]->i;
	send_osc_settings(&argv[1]->s);

	return 0;
}

int set_verbose_handler(const char *path, const char *types, lo_arg ** argv, int argc, void *msg, void *user_data)
{
	if(argv[0]->i ==0) verbose_osc=0;
	else if(argv[0]->i ==1) verbose_osc=1;
	//quiet=0;
	send_osc_settings(&argv[1]->s);

	return 0;
}

void error(int num, const char *msg, const char *path)
{
	printf("liblo server error %d in path %s: %s\n", num, path, msg);
	fflush(stdout);
	exit(1);
}
