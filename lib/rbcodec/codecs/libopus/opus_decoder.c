/* Copyright (c) 2010 Xiph.Org Foundation, Skype Limited
   Written by Jean-Marc Valin and Koen Vos */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "opus_config.h"
#endif

#ifndef OPUS_BUILD
#error "OPUS_BUILD _MUST_ be defined to build Opus and you probably want a decent config.h, see README for more details."
#endif

#include <stdarg.h>
#include "celt.h"
#include "opus.h"
#include "entdec.h"
#include "modes.h"
#include "API.h"
#include "stack_alloc.h"
#include "float_cast.h"
#include "opus_private.h"
#include "os_support.h"
#include "structs.h"
#include "define.h"
#include "mathops.h"

struct OpusDecoder {
   int          celt_dec_offset;
   int          silk_dec_offset;
   int          channels;
   opus_int32   Fs;          /** Sampling rate (at the API level) */
   silk_DecControlStruct DecControl;
   int          decode_gain;

   /* Everything beyond this point gets cleared on a reset */
#define OPUS_DECODER_RESET_START stream_channels
   int          stream_channels;

   int          bandwidth;
   int          mode;
   int          prev_mode;
   int          frame_size;
   int          prev_redundancy;

   opus_uint32  rangeFinal;
};

#ifdef FIXED_POINT
static inline opus_int16 SAT16(opus_int32 x) {
   return x > 32767 ? 32767 : x < -32768 ? -32768 : (opus_int16)x;
}
#endif


int opus_decoder_get_size(int channels)
{
   int silkDecSizeBytes, celtDecSizeBytes;
   int ret;
   if (channels<1 || channels > 2)
      return 0;
   ret = silk_Get_Decoder_Size( &silkDecSizeBytes );
   if(ret)
      return 0;
   silkDecSizeBytes = align(silkDecSizeBytes);
   celtDecSizeBytes = celt_decoder_get_size(channels);
   return align(sizeof(OpusDecoder))+silkDecSizeBytes+celtDecSizeBytes;
}

int opus_decoder_init(OpusDecoder *st, opus_int32 Fs, int channels)
{
   void *silk_dec;
   CELTDecoder *celt_dec;
   int ret, silkDecSizeBytes;

   if ((Fs!=48000&&Fs!=24000&&Fs!=16000&&Fs!=12000&&Fs!=8000)
    || (channels!=1&&channels!=2))
      return OPUS_BAD_ARG;

   OPUS_CLEAR((char*)st, opus_decoder_get_size(channels));
   /* Initialize SILK encoder */
   ret = silk_Get_Decoder_Size(&silkDecSizeBytes);
   if (ret)
      return OPUS_INTERNAL_ERROR;

   silkDecSizeBytes = align(silkDecSizeBytes);
   st->silk_dec_offset = align(sizeof(OpusDecoder));
   st->celt_dec_offset = st->silk_dec_offset+silkDecSizeBytes;
   silk_dec = (char*)st+st->silk_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);
   st->stream_channels = st->channels = channels;

   st->Fs = Fs;
   st->DecControl.API_sampleRate = st->Fs;
   st->DecControl.nChannelsAPI      = st->channels;

   /* Reset decoder */
   ret = silk_InitDecoder( silk_dec );
   if(ret)return OPUS_INTERNAL_ERROR;

   /* Initialize CELT decoder */
   ret = celt_decoder_init(celt_dec, Fs, channels);
   if(ret!=OPUS_OK)return OPUS_INTERNAL_ERROR;

   celt_decoder_ctl(celt_dec, CELT_SET_SIGNALLING(0));

   st->prev_mode = 0;
   st->frame_size = Fs/400;
   return OPUS_OK;
}

#define STATIC_DECODER_SIZE 26532 /* 26486 for 32bit, 26532 for 64bit environment */
static char s_dec[STATIC_DECODER_SIZE] IBSS_ATTR MEM_ALIGN_ATTR;

OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error)
{
   int ret;
   OpusDecoder *st = NULL;
   if ((Fs!=48000&&Fs!=24000&&Fs!=16000&&Fs!=12000&&Fs!=8000)
    || (channels!=1&&channels!=2))
   {
      if (error)
         *error = OPUS_BAD_ARG;
      return NULL;
   }

   if (STATIC_DECODER_SIZE >= opus_decoder_get_size(channels))
      st = (OpusDecoder *)s_dec;
   else
      st = (OpusDecoder *)opus_alloc(opus_decoder_get_size(channels));

   if (st == NULL)
   {
      if (error)
         *error = OPUS_ALLOC_FAIL;
      return NULL;
   }
   ret = opus_decoder_init(st, Fs, channels);
   if (error)
      *error = ret;
   if (ret != OPUS_OK)
   {
      opus_free(st);
      st = NULL;
   }
   return st;
}

static void smooth_fade(const opus_val16 *in1, const opus_val16 *in2,
      opus_val16 *out, int overlap, int channels,
      const opus_val16 *window, opus_int32 Fs)
{
   int i, c;
   int inc = 48000/Fs;
   for (c=0;c<channels;c++)
   {
      for (i=0;i<overlap;i++)
      {
         opus_val16 w = MULT16_16_Q15(window[i*inc], window[i*inc]);
         out[i*channels+c] = SHR32(MAC16_16(MULT16_16(w,in2[i*channels+c]),
                                   Q15ONE-w, in1[i*channels+c]), 15);
      }
   }
}

static int opus_packet_get_mode(const unsigned char *data)
{
   int mode;
   if (data[0]&0x80)
   {
      mode = MODE_CELT_ONLY;
   } else if ((data[0]&0x60) == 0x60)
   {
      mode = MODE_HYBRID;
   } else {
      mode = MODE_SILK_ONLY;
   }
   return mode;
}

static int opus_decode_frame(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   void *silk_dec;
   CELTDecoder *celt_dec;
   int i, silk_ret=0, celt_ret=0;
   ec_dec dec;
   opus_int32 silk_frame_size;
   VARDECL(opus_int16, pcm_silk);
   VARDECL(opus_val16, pcm_transition);
   VARDECL(opus_val16, redundant_audio);

   int audiosize;
   int mode;
   int transition=0;
   int start_band;
   int redundancy=0;
   int redundancy_bytes = 0;
   int celt_to_silk=0;
   int c;
   int F2_5, F5, F10, F20;
   const opus_val16 *window;
   opus_uint32 redundant_rng = 0;
   ALLOC_STACK;

   silk_dec = (char*)st+st->silk_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);
   F20 = st->Fs/50;
   F10 = F20>>1;
   F5 = F10>>1;
   F2_5 = F5>>1;
   if (frame_size < F2_5)
   {
      RESTORE_STACK;
      return OPUS_BUFFER_TOO_SMALL;
   }
   /* Limit frame_size to avoid excessive stack allocations. */
   frame_size = IMIN(frame_size, st->Fs/25*3);
   /* Payloads of 1 (2 including ToC) or 0 trigger the PLC/DTX */
   if (len<=1)
   {
      data = NULL;
      /* In that case, don't conceal more than what the ToC says */
      frame_size = IMIN(frame_size, st->frame_size);
   }
   if (data != NULL)
   {
      audiosize = st->frame_size;
      mode = st->mode;
      ec_dec_init(&dec,(unsigned char*)data,len);
   } else {
      audiosize = frame_size;

      if (st->prev_mode == 0)
      {
         /* If we haven't got any packet yet, all we can do is return zeros */
         for (i=0;i<audiosize*st->channels;i++)
            pcm[i] = 0;
         RESTORE_STACK;
         return audiosize;
      } else {
         mode = st->prev_mode;
      }
   }

   /* For CELT/hybrid PLC of more than 20 ms, do multiple calls */
   if (data==NULL && frame_size > F20 && mode != MODE_SILK_ONLY)
   {
      int nb_samples = 0;
      do {
         int ret = opus_decode_frame(st, NULL, 0, pcm, F20, 0);
         if (ret != F20)
         {
            RESTORE_STACK;
            return OPUS_INTERNAL_ERROR;
         }
         pcm += F20*st->channels;
         nb_samples += F20;
      } while (nb_samples < frame_size);
      RESTORE_STACK;
      return frame_size;
   }
   ALLOC(pcm_transition, F5*st->channels, opus_val16);

   if (data!=NULL && st->prev_mode > 0 && (
       (mode == MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY && !st->prev_redundancy)
    || (mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY) )
      )
   {
      transition = 1;
      if (mode == MODE_CELT_ONLY)
         opus_decode_frame(st, NULL, 0, pcm_transition, IMIN(F5, audiosize), 0);
   }
   if (audiosize > frame_size)
   {
      /*fprintf(stderr, "PCM buffer too small: %d vs %d (mode = %d)\n", audiosize, frame_size, mode);*/
      RESTORE_STACK;
      return OPUS_BAD_ARG;
   } else {
      frame_size = audiosize;
   }

   ALLOC(pcm_silk, IMAX(F10, frame_size)*st->channels, opus_int16);
   ALLOC(redundant_audio, F5*st->channels, opus_val16);

   /* SILK processing */
   if (mode != MODE_CELT_ONLY)
   {
      int lost_flag, decoded_samples;
      opus_int16 *pcm_ptr = pcm_silk;

      if (st->prev_mode==MODE_CELT_ONLY)
         silk_InitDecoder( silk_dec );

      /* The SILK PLC cannot produce frames of less than 10 ms */
      st->DecControl.payloadSize_ms = IMAX(10, 1000 * audiosize / st->Fs);

      if (data != NULL)
      {
        st->DecControl.nChannelsInternal = st->stream_channels;
        if( mode == MODE_SILK_ONLY ) {
           if( st->bandwidth == OPUS_BANDWIDTH_NARROWBAND ) {
              st->DecControl.internalSampleRate = 8000;
           } else if( st->bandwidth == OPUS_BANDWIDTH_MEDIUMBAND ) {
              st->DecControl.internalSampleRate = 12000;
           } else if( st->bandwidth == OPUS_BANDWIDTH_WIDEBAND ) {
              st->DecControl.internalSampleRate = 16000;
           } else {
              st->DecControl.internalSampleRate = 16000;
              silk_assert( 0 );
           }
        } else {
           /* Hybrid mode */
           st->DecControl.internalSampleRate = 16000;
        }
     }

     lost_flag = data == NULL ? 1 : 2 * decode_fec;
     decoded_samples = 0;
     do {
        /* Call SILK decoder */
        int first_frame = decoded_samples == 0;
        silk_ret = silk_Decode( silk_dec, &st->DecControl,
                                lost_flag, first_frame, &dec, pcm_ptr, &silk_frame_size );
        if( silk_ret ) {
           if (lost_flag) {
              /* PLC failure should not be fatal */
              silk_frame_size = frame_size;
              for (i=0;i<frame_size*st->channels;i++)
                 pcm_ptr[i] = 0;
           } else {
             RESTORE_STACK;
             return OPUS_INVALID_PACKET;
           }
        }
        pcm_ptr += silk_frame_size * st->channels;
        decoded_samples += silk_frame_size;
      } while( decoded_samples < frame_size );
   }

   start_band = 0;
   if (!decode_fec && mode != MODE_CELT_ONLY && data != NULL
    && ec_tell(&dec)+17+20*(st->mode == MODE_HYBRID) <= 8*len)
   {
      /* Check if we have a redundant 0-8 kHz band */
      if (mode == MODE_HYBRID)
         redundancy = ec_dec_bit_logp(&dec, 12);
      else
         redundancy = 1;
      if (redundancy)
      {
         celt_to_silk = ec_dec_bit_logp(&dec, 1);
         /* redundancy_bytes will be at least two, in the non-hybrid
            case due to the ec_tell() check above */
         redundancy_bytes = mode==MODE_HYBRID ?
               (opus_int32)ec_dec_uint(&dec, 256)+2 :
               len-((ec_tell(&dec)+7)>>3);
         len -= redundancy_bytes;
         /* This is a sanity check. It should never happen for a valid
            packet, so the exact behaviour is not normative. */
         if (len*8 < ec_tell(&dec))
         {
            len = 0;
            redundancy_bytes = 0;
            redundancy = 0;
         }
         /* Shrink decoder because of raw bits */
         dec.storage -= redundancy_bytes;
      }
   }
   if (mode != MODE_CELT_ONLY)
      start_band = 17;

   {
      int endband=21;

      switch(st->bandwidth)
      {
      case OPUS_BANDWIDTH_NARROWBAND:
         endband = 13;
         break;
      case OPUS_BANDWIDTH_MEDIUMBAND:
      case OPUS_BANDWIDTH_WIDEBAND:
         endband = 17;
         break;
      case OPUS_BANDWIDTH_SUPERWIDEBAND:
         endband = 19;
         break;
      case OPUS_BANDWIDTH_FULLBAND:
         endband = 21;
         break;
      }
      celt_decoder_ctl(celt_dec, CELT_SET_END_BAND(endband));
      celt_decoder_ctl(celt_dec, CELT_SET_CHANNELS(st->stream_channels));
   }

   if (redundancy)
      transition = 0;

   if (transition && mode != MODE_CELT_ONLY)
      opus_decode_frame(st, NULL, 0, pcm_transition, IMIN(F5, audiosize), 0);

   /* 5 ms redundant frame for CELT->SILK*/
   if (redundancy && celt_to_silk)
   {
      celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0));
      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes,
                          redundant_audio, F5, NULL);
      celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng));
   }

   /* MUST be after PLC */
   celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(start_band));

   if (mode != MODE_SILK_ONLY)
   {
      int celt_frame_size = IMIN(F20, frame_size);
      /* Make sure to discard any previous CELT state */
      if (mode != st->prev_mode && st->prev_mode > 0 && !st->prev_redundancy)
         celt_decoder_ctl(celt_dec, OPUS_RESET_STATE);
      /* Decode CELT */
      celt_ret = celt_decode_with_ec(celt_dec, decode_fec ? NULL : data,
                                     len, pcm, celt_frame_size, &dec);
   } else {
      unsigned char silence[2] = {0xFF, 0xFF};
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = 0;
      /* For hybrid -> SILK transitions, we let the CELT MDCT
         do a fade-out by decoding a silence frame */
      if (st->prev_mode == MODE_HYBRID && !(redundancy && celt_to_silk && st->prev_redundancy) )
      {
         celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0));
         celt_decode_with_ec(celt_dec, silence, 2, pcm, F2_5, NULL);
      }
   }

   if (mode != MODE_CELT_ONLY)
   {
#ifdef FIXED_POINT
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = SAT16(pcm[i] + pcm_silk[i]);
#else
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = pcm[i] + (opus_val16)((1.f/32768.f)*pcm_silk[i]);
#endif
   }

   {
      const CELTMode *celt_mode;
      celt_decoder_ctl(celt_dec, CELT_GET_MODE(&celt_mode));
      window = celt_mode->window;
   }

   /* 5 ms redundant frame for SILK->CELT */
   if (redundancy && !celt_to_silk)
   {
      celt_decoder_ctl(celt_dec, OPUS_RESET_STATE);
      celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0));

      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes, redundant_audio, F5, NULL);
      celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng));
      smooth_fade(pcm+st->channels*(frame_size-F2_5), redundant_audio+st->channels*F2_5,
                  pcm+st->channels*(frame_size-F2_5), F2_5, st->channels, window, st->Fs);
   }
   if (redundancy && celt_to_silk)
   {
      for (c=0;c<st->channels;c++)
      {
         for (i=0;i<F2_5;i++)
            pcm[st->channels*i+c] = redundant_audio[st->channels*i+c];
      }
      smooth_fade(redundant_audio+st->channels*F2_5, pcm+st->channels*F2_5,
                  pcm+st->channels*F2_5, F2_5, st->channels, window, st->Fs);
   }
   if (transition)
   {
      if (audiosize >= F5)
      {
         for (i=0;i<st->channels*F2_5;i++)
            pcm[i] = pcm_transition[i];
         smooth_fade(pcm_transition+st->channels*F2_5, pcm+st->channels*F2_5,
                     pcm+st->channels*F2_5, F2_5,
                     st->channels, window, st->Fs);
      } else {
         /* Not enough time to do a clean transition, but we do it anyway
            This will not preserve amplitude perfectly and may introduce
            a bit of temporal aliasing, but it shouldn't be too bad and
            that's pretty much the best we can do. In any case, generating this
            transition it pretty silly in the first place */
         smooth_fade(pcm_transition, pcm,
                     pcm, F2_5,
                     st->channels, window, st->Fs);
      }
   }

   if(st->decode_gain)
   {
      opus_val32 gain;
      gain = celt_exp2(MULT16_16_P15(QCONST16(6.48814081e-4f, 25), st->decode_gain));
      for (i=0;i<frame_size*st->channels;i++)
      {
         opus_val32 x;
         x = MULT16_32_P16(pcm[i],gain);
         pcm[i] = SATURATE(x, 32767);
      }
   }

   if (len <= 1)
      st->rangeFinal = 0;
   else
      st->rangeFinal = dec.rng ^ redundant_rng;

   st->prev_mode = mode;
   st->prev_redundancy = redundancy && !celt_to_silk;
   RESTORE_STACK;
   return celt_ret < 0 ? celt_ret : audiosize;

}

static int parse_size(const unsigned char *data, opus_int32 len, short *size)
{
   if (len<1)
   {
      *size = -1;
      return -1;
   } else if (data[0]<252)
   {
      *size = data[0];
      return 1;
   } else if (len<2)
   {
      *size = -1;
      return -1;
   } else {
      *size = 4*data[1] + data[0];
      return 2;
   }
}

static int opus_packet_parse_impl(const unsigned char *data, opus_int32 len,
      int self_delimited, unsigned char *out_toc,
      const unsigned char *frames[48], short size[48], int *payload_offset)
{
   int i, bytes;
   int count;
   int cbr;
   unsigned char ch, toc;
   int framesize;
   int last_size;
   const unsigned char *data0 = data;

   if (size==NULL)
      return OPUS_BAD_ARG;

   framesize = opus_packet_get_samples_per_frame(data, 48000);

   cbr = 0;
   toc = *data++;
   len--;
   last_size = len;
   switch (toc&0x3)
   {
   /* One frame */
   case 0:
      count=1;
      break;
   /* Two CBR frames */
   case 1:
      count=2;
      cbr = 1;
      if (!self_delimited)
      {
         if (len&0x1)
            return OPUS_INVALID_PACKET;
         size[0] = last_size = len/2;
      }
      break;
   /* Two VBR frames */
   case 2:
      count = 2;
      bytes = parse_size(data, len, size);
      len -= bytes;
      if (size[0]<0 || size[0] > len)
         return OPUS_INVALID_PACKET;
      data += bytes;
      last_size = len-size[0];
      break;
   /* Multiple CBR/VBR frames (from 0 to 120 ms) */
   default: /*case 3:*/
      if (len<1)
         return OPUS_INVALID_PACKET;
      /* Number of frames encoded in bits 0 to 5 */
      ch = *data++;
      count = ch&0x3F;
      if (count <= 0 || framesize*count > 5760)
         return OPUS_INVALID_PACKET;
      len--;
      /* Padding flag is bit 6 */
      if (ch&0x40)
      {
         int padding=0;
         int p;
         do {
            if (len<=0)
               return OPUS_INVALID_PACKET;
            p = *data++;
            len--;
            padding += p==255 ? 254: p;
         } while (p==255);
         len -= padding;
      }
      if (len<0)
         return OPUS_INVALID_PACKET;
      /* VBR flag is bit 7 */
      cbr = !(ch&0x80);
      if (!cbr)
      {
         /* VBR case */
         last_size = len;
         for (i=0;i<count-1;i++)
         {
            bytes = parse_size(data, len, size+i);
            len -= bytes;
            if (size[i]<0 || size[i] > len)
               return OPUS_INVALID_PACKET;
            data += bytes;
            last_size -= bytes+size[i];
         }
         if (last_size<0)
            return OPUS_INVALID_PACKET;
      } else if (!self_delimited)
      {
         /* CBR case */
         last_size = len/count;
         if (last_size*count!=len)
            return OPUS_INVALID_PACKET;
         for (i=0;i<count-1;i++)
            size[i] = last_size;
      }
      break;
   }
   /* Self-delimited framing has an extra size for the last frame. */
   if (self_delimited)
   {
      bytes = parse_size(data, len, size+count-1);
      len -= bytes;
      if (size[count-1]<0 || size[count-1] > len)
         return OPUS_INVALID_PACKET;
      data += bytes;
      /* For CBR packets, apply the size to all the frames. */
      if (cbr)
      {
         if (size[count-1]*count > len)
            return OPUS_INVALID_PACKET;
         for (i=0;i<count-1;i++)
            size[i] = size[count-1];
      } else if(size[count-1] > last_size)
         return OPUS_INVALID_PACKET;
   } else
   {
      /* Because it's not encoded explicitly, it's possible the size of the
         last packet (or all the packets, for the CBR case) is larger than
         1275. Reject them here.*/
      if (last_size > 1275)
         return OPUS_INVALID_PACKET;
      size[count-1] = last_size;
   }

   if (frames)
   {
      for (i=0;i<count;i++)
      {
         frames[i] = data;
         data += size[i];
      }
   }

   if (out_toc)
      *out_toc = toc;

   if (payload_offset)
      *payload_offset = data-data0;

   return count;
}

int opus_packet_parse(const unsigned char *data, opus_int32 len,
      unsigned char *out_toc, const unsigned char *frames[48],
      short size[48], int *payload_offset)
{
   return opus_packet_parse_impl(data, len, 0, out_toc,
                                 frames, size, payload_offset);
}

int opus_decode_native(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec,
      int self_delimited, int *packet_offset)
{
   int i, nb_samples;
   int count, offset;
   unsigned char toc;
   int tot_offset;
   /* 48 x 2.5 ms = 120 ms */
   short size[48];
   if (decode_fec<0 || decode_fec>1)
      return OPUS_BAD_ARG;
   if (len==0 || data==NULL)
      return opus_decode_frame(st, NULL, 0, pcm, frame_size, 0);
   else if (len<0)
      return OPUS_BAD_ARG;

   tot_offset = 0;
   st->mode = opus_packet_get_mode(data);
   st->bandwidth = opus_packet_get_bandwidth(data);
   st->frame_size = opus_packet_get_samples_per_frame(data, st->Fs);
   st->stream_channels = opus_packet_get_nb_channels(data);

   count = opus_packet_parse_impl(data, len, self_delimited, &toc, NULL, size, &offset);
   if (count < 0)
      return count;

   data += offset;
   tot_offset += offset;

   if (count*st->frame_size > frame_size)
      return OPUS_BUFFER_TOO_SMALL;
   nb_samples=0;
   for (i=0;i<count;i++)
   {
      int ret;
      ret = opus_decode_frame(st, data, size[i], pcm, frame_size-nb_samples, decode_fec);
      if (ret<0)
         return ret;
      data += size[i];
      tot_offset += size[i];
      pcm += ret*st->channels;
      nb_samples += ret;
   }
   if (packet_offset != NULL)
      *packet_offset = tot_offset;
   return nb_samples;
}

#ifdef FIXED_POINT

int opus_decode(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   return opus_decode_native(st, data, len, pcm, frame_size, decode_fec, 0, NULL);
}

#ifndef DISABLE_FLOAT_API
int opus_decode_float(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, float *pcm, int frame_size, int decode_fec)
{
   VARDECL(opus_int16, out);
   int ret, i;
   ALLOC_STACK;

   ALLOC(out, frame_size*st->channels, opus_int16);

   ret = opus_decode_native(st, data, len, out, frame_size, decode_fec, 0, NULL);
   if (ret > 0)
   {
      for (i=0;i<ret*st->channels;i++)
         pcm[i] = (1.f/32768.f)*(out[i]);
   }
   RESTORE_STACK;
   return ret;
}
#endif


#else
int opus_decode(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec)
{
   VARDECL(float, out);
   int ret, i;
   ALLOC_STACK;

   if(frame_size<0)
   {
      RESTORE_STACK;
      return OPUS_BAD_ARG;
   }

   ALLOC(out, frame_size*st->channels, float);

   ret = opus_decode_native(st, data, len, out, frame_size, decode_fec, 0, NULL);
   if (ret > 0)
   {
      for (i=0;i<ret*st->channels;i++)
         pcm[i] = FLOAT2INT16(out[i]);
   }
   RESTORE_STACK;
   return ret;
}

int opus_decode_float(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   return opus_decode_native(st, data, len, pcm, frame_size, decode_fec, 0, NULL);
}

#endif

int opus_decoder_ctl(OpusDecoder *st, int request, ...)
{
   int ret = OPUS_OK;
   va_list ap;
   void *silk_dec;
   CELTDecoder *celt_dec;

   silk_dec = (char*)st+st->silk_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);


   va_start(ap, request);

   switch (request)
   {
   case OPUS_GET_BANDWIDTH_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      *value = st->bandwidth;
   }
   break;
   case OPUS_GET_FINAL_RANGE_REQUEST:
   {
      opus_uint32 *value = va_arg(ap, opus_uint32*);
      *value = st->rangeFinal;
   }
   break;
   case OPUS_RESET_STATE:
   {
      OPUS_CLEAR((char*)&st->OPUS_DECODER_RESET_START,
            sizeof(OpusDecoder)-
            ((char*)&st->OPUS_DECODER_RESET_START - (char*)st));

      celt_decoder_ctl(celt_dec, OPUS_RESET_STATE);
      silk_InitDecoder( silk_dec );
      st->stream_channels = st->channels;
      st->frame_size = st->Fs/400;
   }
   break;
   case OPUS_GET_SAMPLE_RATE_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (value==NULL)
      {
         ret = OPUS_BAD_ARG;
         break;
      }
      *value = st->Fs;
   }
   break;
   case OPUS_GET_PITCH_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (value==NULL)
      {
         ret = OPUS_BAD_ARG;
         break;
      }
      if (st->prev_mode == MODE_CELT_ONLY)
         celt_decoder_ctl(celt_dec, OPUS_GET_PITCH(value));
      else
         *value = st->DecControl.prevPitchLag;
   }
   break;
   case OPUS_GET_GAIN_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (value==NULL)
      {
         ret = OPUS_BAD_ARG;
         break;
      }
      *value = st->decode_gain;
   }
   break;
   case OPUS_SET_GAIN_REQUEST:
   {
       opus_int32 value = va_arg(ap, opus_int32);
       if (value<-32768 || value>32767)
       {
          ret = OPUS_BAD_ARG;
          break;
       }
       st->decode_gain = value;
   }
   break;
   default:
      /*fprintf(stderr, "unknown opus_decoder_ctl() request: %d", request);*/
      ret = OPUS_UNIMPLEMENTED;
      break;
   }

   va_end(ap);
   return ret;
}

void opus_decoder_destroy(OpusDecoder *st)
{
   opus_free(st);
}


int opus_packet_get_bandwidth(const unsigned char *data)
{
   int bandwidth;
   if (data[0]&0x80)
   {
      bandwidth = OPUS_BANDWIDTH_MEDIUMBAND + ((data[0]>>5)&0x3);
      if (bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
         bandwidth = OPUS_BANDWIDTH_NARROWBAND;
   } else if ((data[0]&0x60) == 0x60)
   {
      bandwidth = (data[0]&0x10) ? OPUS_BANDWIDTH_FULLBAND :
                                   OPUS_BANDWIDTH_SUPERWIDEBAND;
   } else {
      bandwidth = OPUS_BANDWIDTH_NARROWBAND + ((data[0]>>5)&0x3);
   }
   return bandwidth;
}

int opus_packet_get_samples_per_frame(const unsigned char *data,
      opus_int32 Fs)
{
   int audiosize;
   if (data[0]&0x80)
   {
      audiosize = ((data[0]>>3)&0x3);
      audiosize = (Fs<<audiosize)/400;
   } else if ((data[0]&0x60) == 0x60)
   {
      audiosize = (data[0]&0x08) ? Fs/50 : Fs/100;
   } else {
      audiosize = ((data[0]>>3)&0x3);
      if (audiosize == 3)
         audiosize = Fs*60/1000;
      else
         audiosize = (Fs<<audiosize)/100;
   }
   return audiosize;
}

int opus_packet_get_nb_channels(const unsigned char *data)
{
   return (data[0]&0x4) ? 2 : 1;
}

int opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len)
{
   int count;
   if (len<1)
      return OPUS_BAD_ARG;
   count = packet[0]&0x3;
   if (count==0)
      return 1;
   else if (count!=3)
      return 2;
   else if (len<2)
      return OPUS_INVALID_PACKET;
   else
      return packet[1]&0x3F;
}

int opus_decoder_get_nb_samples(const OpusDecoder *dec,
      const unsigned char packet[], opus_int32 len)
{
   int samples;
   int count = opus_packet_get_nb_frames(packet, len);

   if (count<0)
      return count;

   samples = count*opus_packet_get_samples_per_frame(packet, dec->Fs);
   /* Can't have more than 120 ms */
   if (samples*25 > dec->Fs*3)
      return OPUS_INVALID_PACKET;
   else
      return samples;
}
