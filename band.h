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

#ifndef _BAND_H
#define _BAND_H

#include <gtk/gtk.h>
#include "bandstack.h"

enum {
   band2200=0,
   band630,
   band160,
   band80,
   band60,
   band40,
   band30,
   band20,
   band17,
   band15,
   band12,
   band10,
   band6,
   bandGen,
   bandWWV,
   BANDS
};

#define XVTRS 8

/* --------------------------------------------------------------------------*/
/**
* @brief Band definition
*/
typedef struct _BAND {
    char title[16];
    BANDSTACK *bandstack;
    unsigned char OCrx;
    unsigned char OCtx;
    int preamp;
    int alexRxAntenna;
    int alexTxAntenna;
    int alexAttenuation;
    double pa_calibration;
    long long frequencyMin;
    long long frequencyMax;
    long long frequencyLO;
    long long errorLO;
    int disablePA;
    gint panadapter_low;
    gint panadapter_high;
    gint panadapter_step;
    gint waterfall_low;
    gint waterfall_high;
    gboolean waterfall_automatic;
} BAND;

typedef struct _CHANNEL {
    long long frequency;
    long long width;
} CHANNEL;

extern int band;
//extern gboolean displayHF;

#define UK_CHANNEL_ENTRIES 11
#define OTHER_CHANNEL_ENTRIES 5

extern int channel_entries;
extern CHANNEL *band_channels_60m;

extern CHANNEL band_channels_60m_UK[UK_CHANNEL_ENTRIES];
extern CHANNEL band_channels_60m_OTHER[OTHER_CHANNEL_ENTRIES];

extern BANDSTACK bandstack60;
extern BANDSTACK_ENTRY bandstack_entries60_OTHER[];
extern BANDSTACK_ENTRY bandstack_entries60_UK[];

extern int band_get_current();
extern BAND *band_get_current_band();
extern BAND *band_get_band(int b);
extern BAND *band_set_current(int b);
extern int get_band_from_frequency(gint64 f);

extern BANDSTACK *bandstack_get_bandstack(int band);
extern BANDSTACK_ENTRY *bandstack_get_bandstack_entry(int band,int entry);

extern BANDSTACK_ENTRY *bandstack_entry_next();
extern BANDSTACK_ENTRY *bandstack_entry_previous();
extern BANDSTACK_ENTRY *bandstack_entry_get_current();

extern void bandSaveState();
extern void bandRestoreState();

extern int next_band(int current_band);
extern int previous_band(int current_band);

#endif
