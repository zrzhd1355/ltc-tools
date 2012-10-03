/* jack linear time code to MIDI time code translator
 * Copyright (C) 2006, 20120, 2012 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define LTC_QUEUE_LEN (30) // should be >> ( max(jack period size) / (duration of LTC-frame) )
#define JACK_MIDI_QUEUE_SIZE (LTC_QUEUE_LEN)

#define _GNU_SOURCE

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <sys/mman.h>
#include <ltc.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

static jack_port_t *ltc_input_port = NULL;
static jack_port_t *mtc_output_port = NULL;

static jack_client_t *j_client = NULL;
static jack_nframes_t jltc_latency = 0;
static jack_nframes_t jmtc_latency = 0;
static jack_nframes_t j_bufsize = 1024;
static uint32_t j_samplerate = 48000;

static LTCDecoder *decoder = NULL;
static volatile long long int monotonic_fcnt = 0;

static int detect_framerate = 0;
static int fps_num = 25;
static int fps_den = 1;
static int detected_fps;
static char *ltcportname = NULL;
static char *mtcportname = NULL;

/* a simple state machine for this client */
static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;

typedef struct my_midi_event {
  long long int monotonic_align;
  jack_nframes_t time;
  size_t size;
  jack_midi_data_t buffer[16];
} my_midi_event_t;

my_midi_event_t event_queue[JACK_MIDI_QUEUE_SIZE];
int queued_events_start = 0;
int queued_events_end = 0;
int queued_cycle_id = 0;

/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client=NULL;
  }
  ltc_decoder_free(decoder);
  fprintf(stderr, "bye.\n");
}

#if 0 // TODO
static int next_quarter_frame_to_send = 0;

static int queue_mtc_quarterframe(SMPTETimecode *stime, int mtc_tc, long long int posinfo) {
  unsigned char mtc_msg=0;
  switch(next_quarter_frame_to_send) {
    case 0: mtc_msg =  0x00 |  (stime->frame&0xf); break;
    case 1: mtc_msg =  0x10 | ((stime->frame&0xf0)>>4); break;
    case 2: mtc_msg =  0x20 |  (stime->secs&0xf); break;
    case 3: mtc_msg =  0x30 | ((stime->secs&0xf0)>>4); break;
    case 4: mtc_msg =  0x40 |  (stime->mins&0xf); break;
    case 5: mtc_msg =  0x50 | ((stime->mins&0xf0)>>4); break;
    case 6: mtc_msg =  0x60 |  ((mtc_tc|stime->hours)&0xf); break;
    case 7: mtc_msg =  0x70 | (((mtc_tc|stime->hours)&0xf0)>>4); break;
  }

  jack_midi_data_t *sysex = event_queue[queued_events_start].buffer;
  sysex[0] = (char) 0xf1;
  sysex[1] = (char) mtc_msg;

  event_queue[queued_events_start].time = 0;
  event_queue[queued_events_start].size = 2;
  queued_events_start = (queued_events_start + 1)%JACK_MIDI_QUEUE_SIZE;

  next_quarter_frame_to_send++;
  if (next_quarter_frame_to_send >= 8) {
    next_quarter_frame_to_send = 0;
    return 1;
  }
  return 0;
}
#endif

static void queue_mtc_sysex(SMPTETimecode *stime, int mtc_tc, long long int posinfo) {
  jack_midi_data_t *sysex = event_queue[queued_events_start].buffer;
#if 1
  sysex[0]  = (unsigned char) 0xf0; // fixed
  sysex[1]  = (unsigned char) 0x7f; // fixed
  sysex[2]  = (unsigned char) 0x7f; // sysex channel
  sysex[3]  = (unsigned char) 0x01; // fixed
  sysex[4]  = (unsigned char) 0x01; // fixed
  sysex[5]  = (unsigned char) 0x00; // hour
  sysex[6]  = (unsigned char) 0x00; // minute
  sysex[7]  = (unsigned char) 0x00; // seconds
  sysex[8]  = (unsigned char) 0x00; // frame
  sysex[9]  = (unsigned char) 0xf7; // fixed

  sysex[5] |= (unsigned char) (mtc_tc&0x60);
  sysex[5] |= (unsigned char) (stime->hours&0x1f);
  sysex[6] |= (unsigned char) (stime->mins&0x7f);
  sysex[7] |= (unsigned char) (stime->secs&0x7f);
  sysex[8] |= (unsigned char) (stime->frame&0x7f);

  event_queue[queued_events_start].size = 10;

#else

  sysex[0]   = (char) 0xf0;
  sysex[1]   = (char) 0x7f;
  sysex[2]   = (char) 0x7f;
  sysex[3]   = (char) 0x06;
  sysex[4]   = (char) 0x44;
  sysex[5]   = (char) 0x06;
  sysex[6]   = (char) 0x01;
  sysex[7]   = (char) 0x00;
  sysex[8]   = (char) 0x00;
  sysex[9]   = (char) 0x00;
  sysex[10]  = (char) 0x00;
  sysex[11]  = (char) 0x00;
  sysex[12]  = (char) 0xf7;

  sysex[7]  |= (char) 0x20; // 25fps
  sysex[7]  |= (char) (stime->hours&0x1f);
  sysex[8]  |= (char) (stime->mins&0x7f);
  sysex[9]  |= (char) (stime->secs&0x7f);
  sysex[10] |= (char) (stime->frame&0x7f);

  int checksum = (sysex[7] + sysex[8] + sysex[9] + sysex[10] + 0x3f)&0x7f ;
  sysex[11]  = (char) (127-checksum); //checksum
  event_queue[queued_events_start].size = 13;
#endif

  event_queue[queued_events_start].monotonic_align = posinfo;
  event_queue[queued_events_start].time = 0;
  queued_events_start = (queued_events_start + 1)%JACK_MIDI_QUEUE_SIZE;
}


static void detect_fps(SMPTETimecode *stime) {
#if 1 // detect fps: TODO use the maximum
      // frameno found in stream during the last N seconds
      // _not_ all time maximum
  if (detect_framerate) {
    static int ff_cnt = 0;
    static int ff_max = 0;
    if (stime->frame > ff_max) ff_max = stime->frame;
    ff_cnt++;
    if (ff_cnt > 60 && ff_cnt > ff_max) {
      detected_fps = ff_max + 1;
      ff_cnt= 61; //XXX prevent overflow..
    }
  }
#endif
}

/**
 *
 */
static void generate_mtc(LTCDecoder *d) {
  LTCFrameExt frame;

  while (ltc_decoder_read(d,&frame)) {
    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &frame.ltc, 0);
    detect_fps(&stime);

#if 0
    static LTCFrameExt prev_time;
    static int frames_in_sequence = 0;
    int discontinuity_detected = 0;
    /* detect discontinuities */
    ltc_frame_increment(&prev_time.ltc, detected_fps , 0);
    if (memcmp(&prev_time.ltc, &frame.ltc, sizeof(LTCFrame))) {
      discontinuity_detected = 1;
    }
    memcpy(&prev_time, &frame, sizeof(LTCFrameExt));

    /* notfify about discontinuities */
    if (frames_in_sequence > 0 && discontinuity_detected) {
	fprintf(stdout, "#DISCONTINUITY\n");
    }
    frames_in_sequence++;
#endif

    /*set MTC fps */
    static int fps_warn = 0;
    int mtc_tc = 0x20;
    switch (detected_fps) {
      case 24:
	mtc_tc = 0x00;
	fps_warn = 0;
	break;
      case 25:
	mtc_tc = 0x20;
	fps_warn = 0;
	break;
      case 29:
	mtc_tc = 0x40;
	fps_warn = 0;
	break;
      case 30:
	mtc_tc = 0x60;
	fps_warn = 0;
	break;
      default:
	if (!fps_warn) {
	  fps_warn = 1;
	  fprintf(stderr, "WARNING: invalid video framerate %d (using 25fps instead)\n", detected_fps);
	}
	break;
    }

#if 1 // DEBUG
    fprintf(stdout, "%02d:%02d:%02d%c%02d | %8lld %8lld%s\n",
	stime.hours,
	stime.mins,
	stime.secs,
	(frame.ltc.dfbit) ? '.' : ':',
	stime.frame,
	frame.off_start,
	frame.off_end,
	frame.reverse ? " R" : "  "
	);
#endif

    /* when a full LTC frame is decoded, the timecode the LTC frame
     * is referring has just passed.
     * So we send the _next_ timecode which
     * is expected to start at the end of the current frame
     */
    if (frame.reverse) {
      queue_mtc_sysex(&stime, mtc_tc, frame.off_end + 1);
    } else {
      ltc_frame_increment(&frame.ltc, detected_fps , 0);
      ltc_frame_to_time(&stime, &frame.ltc, 0);
      queue_mtc_sysex(&stime, mtc_tc, frame.off_end + 1);
    }
  }
}

static int parse_ltc(jack_nframes_t nframes, jack_default_audio_sample_t *in, long long int posinfo) {
  jack_nframes_t i;
  unsigned char sound[8192];
  if (nframes > 8192) return 1;

  for (i = 0; i < nframes; i++) {
    const int snd=(int)rint((127.0*in[i])+128.0);
    sound[i] = (unsigned char) (snd&0xff);
  }

  ltc_decoder_write(decoder, sound, nframes, posinfo);
  return 0;
}

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  void *out;
  jack_default_audio_sample_t *in;

  in = jack_port_get_buffer (ltc_input_port, nframes);
  out = jack_port_get_buffer(mtc_output_port, nframes);

  parse_ltc(nframes, in, monotonic_fcnt - jltc_latency);

  generate_mtc(decoder);

  jack_midi_clear_buffer(out);
  while (queued_events_end != queued_events_start) {
    long long int mt = event_queue[queued_events_end].monotonic_align - jmtc_latency;
    if (mt >= monotonic_fcnt + j_bufsize) {
      fprintf(stderr, "DEBUG: MTC timestamp is for next jack cycle.\n"); // XXX
      break;
    }
    if (mt < monotonic_fcnt) {
      fprintf(stderr, "WARNING: MTC was for previous jack cycle (too large MTC port latency?)\n"); // XXX
      fprintf(stderr, " %lld < %lld)\n", mt, monotonic_fcnt); // XXX
    } else {
      event_queue[queued_events_end].time = mt - monotonic_fcnt;
      jack_midi_event_write(out,
	  event_queue[queued_events_end].time,
	  event_queue[queued_events_end].buffer,
	  event_queue[queued_events_end].size
	  );
    }
    queued_events_end = (queued_events_end + 1)%JACK_MIDI_QUEUE_SIZE;
  }

  monotonic_fcnt += nframes;

  return 0;
}

int jack_latency_cb(void *arg) {
  jack_latency_range_t jlty;
  if (ltc_input_port) {
    jack_port_get_latency_range(ltc_input_port, JackCaptureLatency, &jlty);
    jltc_latency = jlty.max;
    //fprintf(stderr, "JACK port latency: %d\n", jltc_latency); // DEBUG
  }
  if (mtc_output_port) {
    jack_port_get_latency_range(mtc_output_port, JackPlaybackLatency, &jlty);
    jmtc_latency = jlty.max;
    //fprintf(stderr, "MTC port latency: %d\n", jmtc_latency); // DEBUG
  }
  return 0;
}

int jack_bufsiz_cb(jack_nframes_t nframes, void *arg) {
  j_bufsize = nframes;
  return 0;
}

void jack_shutdown (void *arg) {
  fprintf(stderr,"recv. shutdown request from jackd.\n");
  client_state=Exit;
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
  jack_status_t status;
  j_client = jack_client_open (client_name, JackNullOption, &status);
  if (j_client == NULL) {
    fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    return (-1);
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(j_client);
    fprintf (stderr, "jack-client name: `%s'\n", client_name);
  }
  jack_set_process_callback (j_client, process, 0);

  jack_set_graph_order_callback (j_client, jack_latency_cb, NULL);

  jack_set_buffer_size_callback (j_client, jack_bufsiz_cb, NULL);

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  j_samplerate=jack_get_sample_rate (j_client);
  j_bufsize=jack_get_buffer_size (j_client);

  if (j_bufsize > 256) {
    fprintf(stderr,"consider running at a lower jack period-size.\n");
  }

  return (0);
}

static int jack_portsetup(void) {
  decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);

  if ((ltc_input_port = jack_port_register (j_client, "ltc_in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
    fprintf (stderr, "cannot register ltc input port !\n");
    return (-1);
  }
  if ((mtc_output_port = jack_port_register(j_client, "mtc_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register mtc ouput port !\n");
    return (-1);
  }
  return (0);
}

static void port_connect(char *ltc_port, char *mtc_port) {
  if (ltc_port && jack_connect(j_client, ltc_port, jack_port_name(ltc_input_port))) {
    fprintf(stderr, "cannot connect port %s to %s\n", ltc_port, jack_port_name(ltc_input_port));
  }
  if (mtc_port && jack_connect(j_client, jack_port_name(mtc_output_port), mtc_port)) {
    fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(mtc_output_port), mtc_port);
  }
}

void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state=Exit;
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"fps", required_argument, 0, 'f'},
  {"detectfps", no_argument, 0, 'F'},
  {"ltcport", required_argument, 0, 'l'},
  {"mtcport", required_argument, 0, 'm'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jltc2mtc - JACK app to translate linear time code to midi time code.\n\n");
  printf ("Usage: jltc2mtc [ OPTIONS ]\n\n");
  printf ("Options:\n\
  -f, --fps <num>[/den]      set expected [initial] framerate (default 25/1)\n\
  -F, --detectfps            autodetect framerate from LTC\n\
  -l, --ltcport <portname>   autoconnect LTC input port\n\
  -m, --mtcport <portname>   autoconnect MTC output port\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
...\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/ltc-tools>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "h"	/* help */
			   "F"	/* detect framerate */
			   "f:"	/* fps */
			   "l:"	/* ltcport */
			   "m:"	/* mtcport */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c)
	{

	case 'f':
	{
	  fps_num = atoi(optarg);
	  char *tmp = strchr(optarg, '/');
	  if (tmp) fps_den=atoi(++tmp);
	}
	break;

	case 'F':
	  detect_framerate = 1;
	  break;

	case 'l':
	  free(ltcportname);
	  ltcportname = strdup(optarg);
	  break;

	case 'm':
	  free(mtcportname);
	  mtcportname = strdup(optarg);
	  break;

	case 'V':
	  printf ("jltc2mtc version %s\n\n", VERSION);
	  printf ("Copyright (C) GPL 2006,2012 Robin Gareus <robin@gareus.org>\n");
	  exit (0);

	case 'h':
	  usage (0);

	default:
	  usage (EXIT_FAILURE);
	}
    }

  return optind;
}

int main (int argc, char **argv) {

  if (decode_switches (argc, argv) != argc) {
    usage(EXIT_FAILURE);
  }

  // -=-=-= INITIALIZE =-=-=-

  if (init_jack("jltc2mtc"))
    goto out;
  if (jack_portsetup())
    goto out;

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  detected_fps = ceil((double)fps_num/fps_den);

  // -=-=-= RUN =-=-=-

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  port_connect(ltcportname, mtcportname);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  // -=-=-= JACK DOES ALL THE WORK =-=-=-

  while (client_state != Exit) {
    sleep(1);
  }

  // -=-=-= CLEANUP =-=-=-

out:
  cleanup(0);
  return(0);
}
/* vi:set ts=8 sts=2 sw=2: */
