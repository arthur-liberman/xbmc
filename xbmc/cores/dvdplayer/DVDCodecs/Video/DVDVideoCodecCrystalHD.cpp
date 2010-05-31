/*
 *      Copyright (C) 2005-2009 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#include "libavcodec/avcodec.h"
#endif

#if defined(HAVE_LIBCRYSTALHD)
#include "GUISettings.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "DVDVideoCodecCrystalHD.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"

#define __MODULE_NAME__ "DVDVideoCodecCrystalHD"

CDVDVideoCodecCrystalHD::CDVDVideoCodecCrystalHD() :
  m_Device(NULL),
  m_DecodeStarted(false),
  m_DropPictures(false),
  m_Duration(0.0),
  m_pFormatName(""),
  m_convert_bitstream(false)
{
}

CDVDVideoCodecCrystalHD::~CDVDVideoCodecCrystalHD()
{
  Dispose();
}

bool CDVDVideoCodecCrystalHD::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (g_guiSettings.GetBool("videoplayer.usechd") && !hints.software)
  {
    m_convert_bitstream = false;

    switch (hints.codec)
    {
      case CODEC_ID_MPEG2VIDEO:
        m_codec_type = CRYSTALHD_CODEC_ID_MPEG2;
        m_pFormatName = "chd-mpeg2";
      break;
      case CODEC_ID_H264:
        if (hints.extrasize < 7 || hints.extradata == NULL)
        {
          CLog::Log(LOGNOTICE, "%s - avcC atom too data small or missing", __FUNCTION__);
          return false;
        }
        // valid avcC data (bitstream) always starts with the value 1 (version)
        if ( *(char*)hints.extradata == 1 )
          m_convert_bitstream = bitstream_convert_init(hints.extradata, hints.extrasize);

        m_codec_type = CRYSTALHD_CODEC_ID_H264;
        m_pFormatName = "chd-h264";
      break;
      case CODEC_ID_VC1:
        m_codec_type = CRYSTALHD_CODEC_ID_VC1;
        m_pFormatName = "chd-vc1";
      break;
      case CODEC_ID_WMV3:
        m_codec_type = CRYSTALHD_CODEC_ID_WMV3;
        m_pFormatName = "chd-wmv3";
      break;
      default:
        return false;
      break;
    }

    m_Device = CCrystalHD::GetInstance();
    if (!m_Device)
    {
      CLog::Log(LOGERROR, "%s: Failed to open Broadcom Crystal HD Codec", __MODULE_NAME__);
      return false;
    }

    if (m_Device && !m_Device->OpenDecoder(m_codec_type, hints.extrasize, hints.extradata))
    {
      CLog::Log(LOGERROR, "%s: Failed to open Broadcom Crystal HD Codec", __MODULE_NAME__);
      return false;
    }

    // default duration to 23.976 fps, have to guess something.
    m_Duration = (DVD_TIME_BASE / (24.0 * 1000.0/1001.0));
    m_DropPictures = false;
    m_DecodeStarted = false;

    CLog::Log(LOGINFO, "%s: Opened Broadcom Crystal HD Codec", __MODULE_NAME__);
    return true;
  }

  return false;
}

void CDVDVideoCodecCrystalHD::Dispose(void)
{
  if (m_Device)
  {
    m_Device->CloseDecoder();
    m_Device = NULL;
  }
  if (m_convert_bitstream)
  {
    if (m_sps_pps_context.sps_pps_data)
    {
      free(m_sps_pps_context.sps_pps_data);
      m_sps_pps_context.sps_pps_data = NULL;
    }
  }
}

int CDVDVideoCodecCrystalHD::Decode(BYTE *pData, int iSize, double dts, double pts)
{
  int ret = 0;

  // We are running a picture queue, picture frames are allocated
  // in CrystalHD class if needed, then passed up. Need to return
  // them back to CrystalHD class for re-queuing. This way we keep
  // the memory alloc/free to a minimum and don't churn memory for
  // each picture frame.
  m_Device->BusyListFlush();

  // If NULL is passed, DVDPlayer wants to process any queued picture frames.
  if (!pData)
  {
    // Always return VC_PICTURE if we have one ready.
    if (m_Device->GetReadyCount() > 0)
      ret |= VC_PICTURE;

    // Returning VC_BUFFER only breaks out of DVDPlayerVideo's picture process loop
    // which it then grabs another demuxer packet and calls this routine again.
    // We only do this if the ready picture count drops to below 4 or
    // risk draining vqueue which then will cause DVDPlayer thrashing. 
    if ((m_Device->GetInputCount() < 2) && (m_Device->GetReadyCount() < 6))
      ret |= VC_BUFFER;

    // if no picture ready and input is full, we must wait
    if(!(ret & VC_PICTURE) && !(ret & VC_BUFFER))
      m_Device->WaitInput(100);
      
    return ret;
  }

  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  bool bitstream_convered  = false;

  if (m_convert_bitstream)
  {
    // convert demuxer packet from bitstream to bytestream (AnnexB)
    int bytestream_size = 0;
    uint8_t *bytestream_buff = NULL;

    bitstream_convert(demuxer_content, demuxer_bytes, &bytestream_buff, &bytestream_size);
    if (bytestream_buff && (bytestream_size > 0))
    {
      bitstream_convered = true;
      demuxer_bytes = bytestream_size;
      demuxer_content = bytestream_buff;
    }
  }
  // Handle Input, add demuxer packet to input queue, we must accept it or
  // it will be discarded as DVDPlayerVideo has no concept of "try again".
  if ( !m_Device->AddInput(demuxer_content, demuxer_bytes, dts, pts) )
  {
    // Deep crap error, this should never happen unless we run away pulling demuxer pkts.
    CLog::Log(LOGDEBUG, "%s: m_pInputThread->AddInput full.", __MODULE_NAME__);
    Sleep(10);
  }

  if (bitstream_convered)
    free(demuxer_content);

#if 1
  // Fake a decoding delay of 1/2 the frame duration, this helps keep DVDPlayerVideo from
  // draining the demuxer queue. DVDPlayerVideo expects one picture frame for each demuxer
  // packet so we sleep a little here to give the decoder a chance to output a frame.
  if (!m_DropPictures)
  {
    if (m_Duration > 0.0)
      Sleep((DWORD)(m_Duration/2000.0));
    else
      Sleep(20);
  }
#endif

  // Handle Output, we delay passing back VC_PICTURE on startup until we have a few
  // decoded picture frames as DVDPlayerVideo might discard picture frames when it
  // tries to sync with audio.
  if (m_DecodeStarted)
  {
    if (m_Device->GetReadyCount() > 0)
      ret |= VC_PICTURE;
    if (m_Device->GetInputCount() < 2 && (m_Device->GetReadyCount() < 4))
      ret |= VC_BUFFER;
  }
  else
  {
    if (m_Device->GetReadyCount() > 4)
    {
      m_DecodeStarted = true;
      ret |= VC_PICTURE;
    }
    if (m_Device->GetInputCount() < 2)
      ret |= VC_BUFFER;
  }

  return ret;
}

void CDVDVideoCodecCrystalHD::Reset(void)
{
  // Decoder flush, reset started flag and dump all input and output.
  m_DecodeStarted = false;
  m_Device->Reset();
}

bool CDVDVideoCodecCrystalHD::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  bool  ret;
  
  ret = m_Device->GetPicture(pDvdVideoPicture);
  m_Duration = pDvdVideoPicture->iDuration;
  pDvdVideoPicture->iDuration = 0;
  return ret;
}

void CDVDVideoCodecCrystalHD::SetDropState(bool bDrop)
{
  m_DropPictures = bDrop;
  m_Device->SetDropState(m_DropPictures);
}

////////////////////////////////////////////////////////////////////////////////////////////
bool CDVDVideoCodecCrystalHD::bitstream_convert_init(void *in_extradata, int in_extrasize)
{
  // based on h264_mp4toannexb_bsf.c (ffmpeg)
  // which is Copyright (c) 2007 Benoit Fouet <benoit.fouet@free.fr>
  // and Licensed GPL 2.1 or greater

  m_sps_pps_size = 0;
  m_sps_pps_context.sps_pps_data = NULL;
  
  // nothing to filter
  if (!in_extradata || in_extrasize < 6)
    return false;

  uint16_t unit_size;
  uint32_t total_size = 0;
  uint8_t *out = NULL, unit_nb, sps_done = 0;
  const uint8_t *extradata = (uint8_t*)in_extradata + 4;
  static const uint8_t nalu_header[4] = {0, 0, 0, 1};

  // retrieve length coded size
  m_sps_pps_context.length_size = (*extradata++ & 0x3) + 1;
  if (m_sps_pps_context.length_size == 3)
    return false;

  // retrieve sps and pps unit(s)
  unit_nb = *extradata++ & 0x1f;  // number of sps unit(s)
  if (!unit_nb)
  {
    unit_nb = *extradata++;       // number of pps unit(s)
    sps_done++;
  }
  while (unit_nb--)
  {
    unit_size = extradata[0] << 8 | extradata[1];
    total_size += unit_size + 4;
    if ( (extradata + 2 + unit_size) > ((uint8_t*)in_extradata + in_extrasize) )
    {
      free(out);
      return false;
    }
    out = (uint8_t*)realloc(out, total_size);
    if (!out)
      return false;

    memcpy(out + total_size - unit_size - 4, nalu_header, 4);
    memcpy(out + total_size - unit_size, extradata + 2, unit_size);
    extradata += 2 + unit_size;

    if (!unit_nb && !sps_done++)
      unit_nb = *extradata++;     // number of pps unit(s)
  }

  m_sps_pps_context.sps_pps_data = out;
  m_sps_pps_context.size = total_size;
  m_sps_pps_context.first_idr = 1;

  return true;
}

bool CDVDVideoCodecCrystalHD::bitstream_convert(BYTE* pData, int iSize, uint8_t **poutbuf, int *poutbuf_size)
{
  // based on h264_mp4toannexb_bsf.c (ffmpeg)
  // which is Copyright (c) 2007 Benoit Fouet <benoit.fouet@free.fr>
  // and Licensed GPL 2.1 or greater

  uint8_t *buf = pData;
  uint32_t buf_size = iSize;
  uint8_t  unit_type;
  int32_t  nal_size;
  uint32_t cumul_size = 0;
  const uint8_t *buf_end = buf + buf_size;

  do
  {
    if (buf + m_sps_pps_context.length_size > buf_end)
      goto fail;

    if (m_sps_pps_context.length_size == 1)
      nal_size = buf[0];
    else if (m_sps_pps_context.length_size == 2)
      nal_size = buf[0] << 8 | buf[1];
    else
      nal_size = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];

    buf += m_sps_pps_context.length_size;
    unit_type = *buf & 0x1f;

    if (buf + nal_size > buf_end || nal_size < 0)
      goto fail;

    // prepend only to the first type 5 NAL unit of an IDR picture
    if (m_sps_pps_context.first_idr && unit_type == 5)
    {
      bitstream_alloc_and_copy(poutbuf, poutbuf_size,
        m_sps_pps_context.sps_pps_data, m_sps_pps_context.size, buf, nal_size);
      m_sps_pps_context.first_idr = 0;
    }
    else
    {
      bitstream_alloc_and_copy(poutbuf, poutbuf_size, NULL, 0, buf, nal_size);
      if (!m_sps_pps_context.first_idr && unit_type == 1)
          m_sps_pps_context.first_idr = 1;
    }

    buf += nal_size;
    cumul_size += nal_size + m_sps_pps_context.length_size;
  } while (cumul_size < buf_size);

  return true;

fail:
  free(*poutbuf);
  *poutbuf = NULL;
  *poutbuf_size = 0;
  return false;
}

void CDVDVideoCodecCrystalHD::bitstream_alloc_and_copy(
  uint8_t **poutbuf,      int *poutbuf_size,
  const uint8_t *sps_pps, uint32_t sps_pps_size,
  const uint8_t *in,      uint32_t in_size)
{
  // based on h264_mp4toannexb_bsf.c (ffmpeg)
  // which is Copyright (c) 2007 Benoit Fouet <benoit.fouet@free.fr>
  // and Licensed GPL 2.1 or greater

  #define CHD_WB32(p, d) { \
    ((uint8_t*)(p))[3] = (d); \
    ((uint8_t*)(p))[2] = (d) >> 8; \
    ((uint8_t*)(p))[1] = (d) >> 16; \
    ((uint8_t*)(p))[0] = (d) >> 24; }

  uint32_t offset = *poutbuf_size;
  uint8_t nal_header_size = offset ? 3 : 4;

  *poutbuf_size += sps_pps_size + in_size + nal_header_size;
  *poutbuf = (uint8_t*)realloc(*poutbuf, *poutbuf_size);
  if (sps_pps)
    memcpy(*poutbuf + offset, sps_pps, sps_pps_size);

  memcpy(*poutbuf + sps_pps_size + nal_header_size + offset, in, in_size);
  if (!offset)
  {
    CHD_WB32(*poutbuf + sps_pps_size, 1);
  }
  else
  {
    (*poutbuf + offset + sps_pps_size)[0] = 0;
    (*poutbuf + offset + sps_pps_size)[1] = 0;
    (*poutbuf + offset + sps_pps_size)[2] = 1;
  }
}

#endif
