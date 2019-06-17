/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/


#include <gtk/gtk.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>

#ifdef SOUNDIO
#include <soundio/soundio.h>
#else
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <pulse/simple.h>
#endif

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "adc.h"
#include "dac.h"
#include "discovered.h"
#include "wideband.h"
#include "receiver.h"
#include "transmitter.h"
#include "radio.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "audio.h"


int n_input_devices;
AUDIO_DEVICE input_devices[MAX_AUDIO_DEVICES];
int n_output_devices;
AUDIO_DEVICE output_devices[MAX_AUDIO_DEVICES];

int audio = 0;
int audio_buffer_size = 480; // samples (both left and right)
int mic_buffer_size = 720; // samples (both left and right)

// each buffer contains 63 samples of left and right audio at 16 bits
#define AUDIO_SAMPLES 63
#define AUDIO_SAMPLE_SIZE 2
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFERS 10
#define OUTPUT_BUFFER_SIZE (AUDIO_SAMPLE_SIZE*AUDIO_CHANNELS*audio_buffer_size)

#define MIC_BUFFER_SIZE (AUDIO_SAMPLE_SIZE*AUDIO_CHANNELS*mic_buffer_size)

static unsigned char *mic_buffer=NULL;

static GThread *mic_read_thread_id;

static int running=FALSE;

static void *mic_read_thread(void *arg);

#ifdef SOUNDIO
struct SoundIo *soundio;
#else
static pa_buffer_attr bufattr;
static pa_glib_mainloop *main_loop;
static pa_mainloop_api *main_loop_api;
static pa_operation *op;
static pa_context *pa_ctx;
#endif

static int ready=0;

static int triggered=0;

static int sample_rate=48000;

#ifdef SOUNDIO

static int underflow_count=0;

static void underflow_callback(struct SoundIoOutStream *outstream) {
  underflow_count++;
  //g_print("audio_write: underflow %d\n", underflow_count);
}

static void write_callback(struct SoundIoOutStream *outstream, int frame_count_min, int frame_count_max) {
  RECEIVER *rx=(RECEIVER *)outstream->userdata;
  struct SoundIoChannelArea *areas;
  int frames_left;
  int frame_count;
  int err;

  char *read_ptr = soundio_ring_buffer_read_ptr(rx->ring_buffer);
  int fill_bytes = soundio_ring_buffer_fill_count(rx->ring_buffer);
  int fill_count = fill_bytes / outstream->bytes_per_frame;

  if (frame_count_min > fill_count) {
    //g_print("write_callback: not enough data: frame_count_min=%d fill_count=%d bytes_per_frame=%d\n",frame_count_min,fill_count,outstream->bytes_per_frame);
    // Ring buffer does not have enough data, fill with zeroes.
    frames_left = frame_count_min;
    for (;;) {
      frame_count = frames_left;
      if (frame_count <= 0)
        return;
      if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
        //g_print("write_callback: begin write error: %s\n", soundio_strerror(err));
        return;
      }
      if (frame_count <= 0)
        return;
      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
          memset(areas[ch].ptr, 0, outstream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
        }
      }
      if ((err = soundio_outstream_end_write(outstream)))
        //g_print("write_callback: end write error: %s\n", soundio_strerror(err));
        frames_left -= frame_count;
      }
    }

    int read_count;
    if(frame_count_max<fill_count) read_count=frame_count_max; else read_count=fill_count;
    frames_left = read_count;

    while (frames_left > 0) {
      int frame_count = frames_left;

      if ((err = soundio_outstream_begin_write(outstream, &areas, &frame_count))) {
        g_print("begin write error: %s", soundio_strerror(err));
        return;
      }

      if (frame_count <= 0)
        break;

      for (int frame = 0; frame < frame_count; frame += 1) {
        for (int ch = 0; ch < outstream->layout.channel_count; ch += 1) {
          memcpy(areas[ch].ptr, read_ptr, outstream->bytes_per_sample);
          areas[ch].ptr += areas[ch].step;
          read_ptr += outstream->bytes_per_sample;
        }
      }

      if ((err = soundio_outstream_end_write(outstream))) {
        //g_print("end write error: %s\n", soundio_strerror(err));
        return;
      }

      frames_left -= frame_count;
  }

  soundio_ring_buffer_advance_read_ptr(rx->ring_buffer, read_count * outstream->bytes_per_frame);
}

static void read_callback(struct SoundIoInStream *instream, int frame_count_min, int frame_count_max) {
    RADIO *r=(RADIO *)instream->userdata;
    struct SoundIoChannelArea *areas;
    int err;

    if(frame_count_min!=frame_count_max) {
      g_print("read_callback: frame_counts differ min=%d max=%d\n",frame_count_min,frame_count_max);
      return;
    }

    if(r->local_microphone_buffer!=NULL) {
      if(r->local_microphone_buffer_size!=frame_count_min) {
        free(r->local_microphone_buffer);
        r->local_microphone_buffer=NULL;
      }
    }

    if(r->local_microphone_buffer==NULL) {
      r->local_microphone_buffer_size=frame_count_min;
      r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
g_print("read_callback: create microphone buffer: %p length=%d (%d bytes)\n",r->local_microphone_buffer,r->local_microphone_buffer_size,instream->bytes_per_sample*r->local_microphone_buffer_size);
    }

    int frame_count=frame_count_min;

    if((err = soundio_instream_begin_read(instream, &areas, &frame_count))) {
      g_print("read_callback: begin read error: %s\n", soundio_strerror(err));
      return;
    }

    g_mutex_lock(&r->ring_buffer_mutex);

    char *write_ptr = soundio_ring_buffer_write_ptr(r->ring_buffer);
    int free_bytes = soundio_ring_buffer_free_count(r->ring_buffer);
    int free_count = free_bytes / instream->bytes_per_frame;
    if(frame_count!=0 && free_count>=frame_count) {
      if(areas==NULL) {
        g_print("read_callback: areas is NULL\n");
        memset(write_ptr,0,frame_count*instream->bytes_per_sample);
      } else {
        memcpy(write_ptr,areas[0].ptr,frame_count*instream->bytes_per_sample);
      }

      if((err = soundio_instream_end_read(instream))) {
        g_print("read_callback: end read error: %s", soundio_strerror(err));
      }

      soundio_ring_buffer_advance_write_ptr(r->ring_buffer, frame_count*instream->bytes_per_frame);
      g_cond_signal (&r->ring_buffer_cond);
    } else {
      g_print("read_callback: frame_count is 0 or free_count too small\n");
    }
    g_mutex_unlock (&r->ring_buffer_mutex);

}
#endif

int audio_open_output(RECEIVER *rx) {
  int result=0;
g_print("audio_open_output: %s\n",rx->audio_name);
#ifdef SOUNDIO
  int err;

  g_mutex_lock(&rx->local_audio_mutex);

  // find the device
  rx->output_index=-1;
  for(int i=0;i<n_output_devices;i++) {
    if(strcmp(rx->audio_name,output_devices[i].name)==0) {
      rx->output_index=output_devices[i].index;
      break;
    }
  }

  if(rx->output_index==-1) {
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }
  
  rx->output_device = soundio_get_output_device(soundio, rx->output_index);
  if(!rx->output_device) {
    g_print("audio_open_output: could not get output device: out of memory");
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }

  if(!soundio_device_supports_sample_rate(rx->output_device, sample_rate)) {
    g_print("audio_open_output: device does not support sample rate of %d",sample_rate);
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }

  if(!soundio_device_supports_format(rx->output_device, SoundIoFormatFloat32NE)) {
    g_print("audio_open_output: device does not support SoundIoFormatFloat32NE");
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }

  // guess that 8 output buffers should be enough
  int size=8*rx->output_samples*sizeof(float)*2;
  if(size<32768) size=32768;
  rx->ring_buffer = soundio_ring_buffer_create(soundio, size);
  if(!rx->ring_buffer) {
    g_print("audio_open_output: soundio_ring_buffer_create failed");
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }

  rx->output_stream = soundio_outstream_create(rx->output_device);
  if(!rx->output_stream) {
    g_print("audio_open_output: could not open output device: out of memory");
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }
  rx->output_stream->format = SoundIoFormatFloat32NE;
  rx->output_stream->sample_rate = sample_rate;
  rx->output_stream->write_callback = write_callback;
  rx->output_stream->underflow_callback = underflow_callback;
  rx->output_stream->software_latency = 0.01;
  rx->output_stream->userdata=(void *)rx;

  if((err = soundio_outstream_open(rx->output_stream))) {
    g_print("audio_open_output: unable to open output stream: %s", soundio_strerror(err));
    g_mutex_unlock(&rx->local_audio_mutex);
    return -1;
  }

  g_mutex_unlock(&rx->local_audio_mutex);
#else
  pa_sample_spec sample_spec;
  int err;

  if(rx->audio_name==NULL) {
    result=-1;
  } else {
    g_mutex_lock(&rx->local_audio_mutex);
    sample_spec.rate=48000;
    sample_spec.channels=2;
    sample_spec.format=PA_SAMPLE_FLOAT32NE;

    char stream_id[16];
    sprintf(stream_id,"RX-%d",rx->channel);

    rx->playstream=pa_simple_new(NULL,               // Use the default server.
                    "linHPSDR",           // Our application's name.
                    PA_STREAM_PLAYBACK,
                    rx->audio_name,
                    stream_id,            // Description of our stream.
                    &sample_spec,                // Our sample format.
                    NULL,               // Use default channel map
                    NULL,               // Use default buffering attributes.
                    &err               // error code if returns NULL
                    );
    
    if(rx->playstream!=NULL) {
      rx->local_audio_buffer_offset=0;
      rx->local_audio_buffer=g_new0(float,2*rx->local_audio_buffer_size);
fprintf(stderr,"audio_open_output: allocated local_audio_buffer %p size %ld bytes\n",rx->local_audio_buffer,2*rx->local_audio_buffer_size*sizeof(float));
    } else {
      result=-1;
      fprintf(stderr,"pa-simple_new failed: err=%d\n",err);
    }
    g_mutex_unlock(&rx->local_audio_mutex);
  }

#endif
  return result;
}
	
int audio_open_input(RADIO *r) {
  int result=0;
#ifdef SOUNDIO
  int err;

  
  if(r->microphone_name==NULL) {
g_print("audio_open_input: microphone name is NULL\n");
    return -1;
  }

g_print("audio_open_input: %s\n",r->microphone_name);
// find the device
  int input_index=-1;
  for(int i=0;i<n_input_devices;i++) {
    if(strcmp(r->microphone_name,input_devices[i].name)==0) {
      input_index=input_devices[i].index;
      break;
    }
  }

  if(input_index==-1) {
    g_print("audio_open_input: did not find %s\n",r->microphone_name);
    return -1;
  }


  r->input_device = soundio_get_input_device(soundio, input_index);
  if(!r->input_device) {
    g_print("audio_open_input: could not get input device: out of memory");
    return -1;
  }

  if(!soundio_device_supports_sample_rate(r->input_device, sample_rate)) {
    g_print("audio_open_input: device does not support sample rate of %d",sample_rate);
    return -1;
  }

  if(!soundio_device_supports_format(r->input_device, SoundIoFormatFloat32NE)) {
    g_print("audio_open_input: device does not support SoundIoFormatFloat32NE");
    return -1;
  }

  r->input_stream = soundio_instream_create(r->input_device);
  if(!r->input_stream) {
    g_print("audio_open_input: could not open input device: out of memory");
    return -1;
  }
  r->input_stream->format = SoundIoFormatFloat32NE;
  r->input_stream->sample_rate = sample_rate;
  r->input_stream->read_callback = read_callback;
  r->input_stream->userdata=(void *)r;

  if((err = soundio_instream_open(r->input_stream))) {
    g_print("audio_open_input: unable to open input stream: %s", soundio_strerror(err));
    return -1;
  }

  // guess that 8 input buffers should be enough
  int size=8*512*sizeof(float);
  r->ring_buffer = soundio_ring_buffer_create(soundio, size);
  if(!r->ring_buffer) {
    g_print("audio_open_input: soundio_ring_buffer_create failed");
    return -1;
  }

  if((err = soundio_instream_start(r->input_stream))) {
    g_print("unable to start input device: %s", soundio_strerror(err));
    return -1;
  }
  r->input_started=TRUE;
  running=TRUE;
  mic_read_thread_id = g_thread_new( "mic_thread", mic_read_thread, r);
  if(!mic_read_thread_id ) {
    fprintf(stderr,"g_thread_new failed on mic_read_thread\n");
    soundio_instream_destroy(r->input_stream);
    soundio_device_unref(r->input_device);
    soundio_ring_buffer_destroy(r->ring_buffer);
    if(r->local_microphone_buffer!=NULL) {
      g_free(r->local_microphone_buffer);
      r->local_microphone_buffer=NULL;
    }
    running=FALSE;
    result=-1;
  }
#else
  pa_sample_spec sample_spec;

  if(r->microphone_name==NULL) {
    return -1;
  }

  g_mutex_lock(&r->local_microphone_mutex);
  sample_spec.rate=48000;
  sample_spec.channels=1;
  sample_spec.format=PA_SAMPLE_FLOAT32NE;

  r->microphone_stream=pa_simple_new(NULL,               // Use the default server.
                  "linHPSDR",           // Our application's name.
                  PA_STREAM_RECORD,
                  r->microphone_name,
                  "TX",            // Description of our stream.
                  &sample_spec,                // Our sample format.
                  NULL,               // Use default channel map
                  NULL,               // Use default buffering attributes.
                  NULL               // Ignore error code.
                  );

  if(r->microphone_stream!=NULL) {
    r->local_microphone_buffer_offset=0;
    r->local_microphone_buffer=g_new0(float,r->local_microphone_buffer_size);
    running=TRUE;
    mic_read_thread_id = g_thread_new( "mic_thread", mic_read_thread, r);
    if(!mic_read_thread_id ) {
      fprintf(stderr,"g_thread_new failed on mic_read_thread\n");
      g_free(r->local_microphone_buffer);
      r->local_microphone_buffer=NULL;
      running=FALSE;
      result=-1;
    }
  } else {
    result=-1;
  }
  g_mutex_unlock(&r->local_microphone_mutex);
#endif
  return result;
}

void audio_close_output(RECEIVER *rx) {
 g_print("audio_close_output\n");
#ifdef SOUNDIO
  g_mutex_lock(&rx->local_audio_mutex);
  soundio_outstream_destroy(rx->output_stream);
  soundio_device_unref(rx->output_device);
  soundio_ring_buffer_destroy(rx->ring_buffer);
  rx->output_started=FALSE;
  g_mutex_unlock(&rx->local_audio_mutex);
#else
  g_mutex_lock(&rx->local_audio_mutex);
  pa_simple_free(rx->playstream);
  rx->playstream=NULL;
  g_free(rx->local_audio_buffer);
  rx->local_audio_buffer=NULL;
  g_mutex_unlock(&rx->local_audio_mutex);
#endif
}

void audio_close_input(RADIO *r) {
#ifdef SOUNDIO
  g_mutex_lock(&r->local_microphone_mutex);
  soundio_instream_destroy(r->input_stream);
  soundio_device_unref(r->input_device);
  soundio_ring_buffer_destroy(r->ring_buffer);
  g_mutex_unlock(&r->local_microphone_mutex);
#else
  g_mutex_lock(&r->local_microphone_mutex);
  pa_simple_free(r->microphone_stream);
  r->microphone_stream=NULL;
  g_free(r->local_microphone_buffer);
  r->local_microphone_buffer=NULL;
  g_mutex_unlock(&r->local_microphone_mutex);
#endif
}

int audio_write_buffer(RECEIVER *rx) {
  int rc;
#ifdef SOUNDIO
  g_mutex_lock(&rx->local_audio_mutex);
  char *buf = soundio_ring_buffer_write_ptr(rx->ring_buffer);
  int fill_count = rx->output_samples*sizeof(float)*2;
  memcpy(buf, rx->local_audio_buffer, fill_count);
  soundio_ring_buffer_advance_write_ptr(rx->ring_buffer, fill_count);
  g_mutex_unlock(&rx->local_audio_mutex);
#else
  int err;
  g_mutex_lock(&rx->local_audio_mutex);
  rc=pa_simple_write(rx->playstream,
                         rx->local_audio_buffer,
                         rx->output_samples*sizeof(float)*2,
                         &err);
  if(rc!=0) {
    fprintf(stderr,"audio_write buffer=%p length=%d returned %d err=%d\n",rx->local_audio_buffer,rx->output_samples,rc,err);
  } 
  g_mutex_unlock(&rx->local_audio_mutex);
#endif
  return rc;
}

#ifdef SOUNDIO
void audio_start_output(RECEIVER *rx) {
  int err;
  g_print("audio_start_output\n");
  if(!rx->output_started) {
    underflow_count=0;
    if((err = soundio_outstream_start(rx->output_stream))) {
        g_print("audio_start_output: unable to start output device: %s", soundio_strerror(err));
    } else {
      rx->output_started=TRUE;
    }
  }
}
#endif

int audio_write(RECEIVER *rx,float left_sample,float right_sample) {
  int result=0;
#ifdef SOUNDIO
  g_mutex_lock(&rx->local_audio_mutex);
  float samples[2];
  samples[0]=left_sample;
  samples[1]=right_sample;
  char *buf = soundio_ring_buffer_write_ptr(rx->ring_buffer);
  int fill_count = sizeof(float)*2;
  int free=soundio_ring_buffer_free_count(rx->ring_buffer);
  if(free<fill_count) {
    //g_print("audio_write: ring buffer full: need %d free %d\n",fill_count,free);
  } else {
    memcpy(buf, &samples[0], fill_count);
    soundio_ring_buffer_advance_write_ptr(rx->ring_buffer, fill_count);
  }
  g_mutex_unlock(&rx->local_audio_mutex);
#else
  int rc;
  int err;

  g_mutex_lock(&rx->local_audio_mutex);
  if(rx->local_audio_buffer==NULL) {
    rx->local_audio_buffer_offset=0;
    rx->local_audio_buffer=g_new0(float,2*rx->local_audio_buffer_size);
  }
  rx->local_audio_buffer[rx->local_audio_buffer_offset*2]=left_sample;
  rx->local_audio_buffer[(rx->local_audio_buffer_offset*2)+1]=right_sample;
  rx->local_audio_buffer_offset++;
  if(rx->local_audio_buffer_offset>=rx->local_audio_buffer_size) {
    rc=pa_simple_write(rx->playstream,
                       rx->local_audio_buffer,
                       rx->local_audio_buffer_size*sizeof(float)*2,
                       &err); 
    if(rc!=0) {
      fprintf(stderr,"audio_write failed err=%d\n",err);
    }
    rx->local_audio_buffer_offset=0;
  }
  g_mutex_unlock(&rx->local_audio_mutex);
#endif
  return result;
}

static void *mic_read_thread(gpointer arg) {
  RADIO *r=(RADIO *)arg;
  g_print("mic_read_thread: ENTRY\n");
#ifdef SOUNDIO
  while(running) {
    g_mutex_lock (&r->ring_buffer_mutex);
    while(soundio_ring_buffer_fill_count(r->ring_buffer)==0)
      g_cond_wait (&r->ring_buffer_cond, &r->ring_buffer_mutex);
    char *read_ptr = soundio_ring_buffer_read_ptr(r->ring_buffer);
    int fill_bytes = soundio_ring_buffer_fill_count(r->ring_buffer);
    if(fill_bytes>(r->local_microphone_buffer_size*sizeof(float))) {
      fill_bytes=r->local_microphone_buffer_size*sizeof(float);
    }
    memcpy(r->local_microphone_buffer,read_ptr,fill_bytes);
    soundio_ring_buffer_advance_read_ptr(r->ring_buffer, fill_bytes);
    g_mutex_unlock (&r->ring_buffer_mutex);
    switch(r->discovered->protocol) {
      case PROTOCOL_1:
        protocol1_process_local_mic(r);
        break;
      case PROTOCOL_2:
        protocol2_process_local_mic(r);
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        soapy_protocol_process_local_mic(r);
        break;
#endif
    }
  }
#else
  int rc;
  int err;
  while(running) {
    g_mutex_lock(&r->local_microphone_mutex);
    if(r->local_microphone_buffer==NULL) {
      running=0;
    } else {
      rc=pa_simple_read(r->microphone_stream,
  		r->local_microphone_buffer,
  		r->local_microphone_buffer_size*sizeof(float),
  		&err); 
      if(rc<0) {
        running=0;
        g_print("mic_read_thread: returned %d error=%d (%s)\n",rc,err,pa_strerror(err));
      } else {
        switch(r->discovered->protocol) {
          case PROTOCOL_1:
            protocol1_process_local_mic(r);
            break;
          case PROTOCOL_2:
            protocol2_process_local_mic(r);
            break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        soapy_protocol_process_local_mic(r);
        break;
#endif
        }
      }
    }
    g_mutex_unlock(&r->local_microphone_mutex);
  }
g_print("mic_read_thread: EXIT\n");
#endif
  return NULL;
}

void audio_get_cards() {
}

#ifndef SOUNDIO
static void source_list_cb(pa_context *context,const pa_source_info *s,int eol,void *data) {
  int i;
  if(eol>0) {
    for(i=0;i<n_input_devices;i++) {
      g_print("Input: %d: %s (%s)\n",input_devices[i].index,input_devices[i].name,input_devices[i].description);
    }
  } else if(n_input_devices<MAX_AUDIO_DEVICES) {
    input_devices[n_input_devices].name=g_new0(char,strlen(s->name)+1);
    strncpy(input_devices[n_input_devices].name,s->name,strlen(s->name));
    input_devices[n_input_devices].description=g_new0(char,strlen(s->description)+1);
    strncpy(input_devices[n_input_devices].description,s->description,strlen(s->description));
    input_devices[n_input_devices].index=s->index;
    n_input_devices++;
  }
}

static void sink_list_cb(pa_context *context,const pa_sink_info *s,int eol,void *data) {
  int i;
  if(eol>0) {
    for(i=0;i<n_output_devices;i++) {
      g_print("Output: %d: %s (%s)\n",output_devices[i].index,output_devices[i].name,output_devices[i].description);
    }
    op=pa_context_get_source_info_list(pa_ctx,source_list_cb,NULL);
  } else if(n_output_devices<MAX_AUDIO_DEVICES) {
    output_devices[n_output_devices].name=g_new0(char,strlen(s->name)+1);
    strncpy(output_devices[n_output_devices].name,s->name,strlen(s->name));
    output_devices[n_output_devices].description=g_new0(char,strlen(s->description)+1);
    strncpy(output_devices[n_output_devices].description,s->description,strlen(s->description));
    output_devices[n_output_devices].index=s->index;
    n_output_devices++;
  }
}

static void state_cb(pa_context *c, void *userdata) {
        pa_context_state_t state;
        int *ready = userdata;

        state = pa_context_get_state(c);
        switch  (state) {
                // There are just here for reference
                case PA_CONTEXT_UNCONNECTED:
g_print("audio: state_cb: PA_CONTEXT_UNCONNECTED\n");
                        break;
                case PA_CONTEXT_CONNECTING:
g_print("audio: state_cb: PA_CONTEXT_CONNECTING\n");
                        break;
                case PA_CONTEXT_AUTHORIZING:
g_print("audio: state_cb: PA_CONTEXT_AUTHORIZING\n");
                        break;
                case PA_CONTEXT_SETTING_NAME:
g_print("audio: state_cb: PA_CONTEXT_SETTING_NAME\n");
                        break;
                case PA_CONTEXT_FAILED:
g_print("audio: state_cb: PA_CONTEXT_FAILED\n");
                        *ready = 2;
                        break;
                case PA_CONTEXT_TERMINATED:
g_print("audio: state_cb: PA_CONTEXT_TERMINATED\n");
                        *ready = 2;
                        break;
                case PA_CONTEXT_READY:
g_print("audio: state_cb: PA_CONTEXT_READY\n");
                        *ready = 1;
// get a list of the output devices
                        n_input_devices=0;
                        n_output_devices=0;
                        op = pa_context_get_sink_info_list(pa_ctx,sink_list_cb,NULL);
                        break;
                default:
                        g_print("audio: state_cb: unknown state %d\n",state);
                        break;
        }
}
#endif


void create_audio() {
  int rc;
  int i;
  char text[1024];

g_print("audio: create_audio\n");
#ifdef SOUNDIO
  soundio=soundio_create();
  if(!soundio) {
    g_print("create_audio: soundio_create failed\n");
    return;
  }
  rc=soundio_connect_backend(soundio,SoundIoBackendCoreAudio);
  if(rc) {
    g_print("create_audio: soundio_connect_backend (CoreAudio) failed: %s\n",soundio_strerror(rc));
    rc=soundio_connect_backend(soundio,SoundIoBackendPulseAudio);
    if(rc) {
      g_print("create_audio: soundio_connect_backend failed (PulseAudio): %s\n",soundio_strerror(rc));
      rc=soundio_connect_backend(soundio,SoundIoBackendAlsa);
      if(rc) {
        g_print("create_audio: soundio_connect_backend failed (ALSA): %s\n",soundio_strerror(rc));
        return;
      }
    }
  }

  soundio_flush_events(soundio);

  int output_count=soundio_output_device_count(soundio);
  for(int i=0;i<output_count;i++) {
    if(n_output_devices<MAX_AUDIO_DEVICES) {
      struct SoundIoDevice *device=soundio_get_output_device(soundio,i);

      // ignore devices that do not support the sample rate or format
      if(!soundio_device_supports_sample_rate(device, sample_rate) ) {
        continue;
      }
      if(!soundio_device_supports_format(device, SoundIoFormatFloat32NE) ) {
        continue;
      }

      output_devices[n_output_devices].name=g_new0(char,strlen(device->name)+1);
      strncpy(output_devices[n_output_devices].name,device->name,strlen(device->name));
      output_devices[n_output_devices].description=g_new0(char,strlen(device->name)+1);
      strncpy(output_devices[n_output_devices].description,device->name,strlen(device->name));
      output_devices[n_output_devices].index=i;
      soundio_device_unref(device);
      n_output_devices++;
    }
  }
  int input_count=soundio_input_device_count(soundio);
  for(int i=0;i<input_count;i++) {
    if(n_input_devices<MAX_AUDIO_DEVICES) {
      struct SoundIoDevice *device=soundio_get_input_device(soundio,i);
      input_devices[n_input_devices].name=g_new0(char,strlen(device->name)+1);
      strncpy(input_devices[n_input_devices].name,device->name,strlen(device->name));
      input_devices[n_input_devices].description=g_new0(char,strlen(device->name)+1);
      strncpy(input_devices[n_input_devices].description,device->name,strlen(device->name));
      input_devices[n_input_devices].index=i;
      soundio_device_unref(device);
      n_input_devices++;
    }
  }
  
#else
  main_loop=pa_glib_mainloop_new(NULL);
  main_loop_api=pa_glib_mainloop_get_api(main_loop);
  pa_ctx=pa_context_new(main_loop_api,"linhpsdr");
  pa_context_connect(pa_ctx,NULL,0,NULL);
  pa_context_set_state_callback(pa_ctx, state_cb, &ready);
#endif
}
