/*
 *  Copyright (C) 2000, 2001 Const Kaplinsky.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

#ifdef LIBVNCSERVER_HAVE_LIBZ
#ifdef LIBVNCSERVER_HAVE_LIBJPEG

/*
 * tight.c - handle ``tight'' encoding.
 *
 * This file shouldn't be compiled directly. It is included multiple
 * times by rfbproto.c, each time with a different definition of the
 * macro BPP. For each value of BPP, this file defines a function
 * which handles a tight-encoded rectangle with BPP bits per pixel.
 *
 */

#define TIGHT_MIN_TO_COMPRESS 12

#define CARDBPP CONCAT3E(uint,BPP,_t)
#define filterPtrBPP CONCAT2E(filterPtr,BPP)

#define HandleTightBPP CONCAT2E(HandleTight,BPP)
#define InitFilterCopyBPP CONCAT2E(InitFilterCopy,BPP)
#define InitFilterPaletteBPP CONCAT2E(InitFilterPalette,BPP)
#define InitFilterGradientBPP CONCAT2E(InitFilterGradient,BPP)
#define FilterCopyBPP CONCAT2E(FilterCopy,BPP)
#define FilterPaletteBPP CONCAT2E(FilterPalette,BPP)
#define FilterGradientBPP CONCAT2E(FilterGradient,BPP)

#if BPP != 8
#define DecompressJpegRectBPP CONCAT2E(DecompressJpegRect,BPP)
#endif

#ifndef RGB_TO_PIXEL

#define RGB_TO_PIXEL(bpp,r,g,b)						\
  (((CARD##bpp)(r) & client->format.redMax) << client->format.redShift |		\
   ((CARD##bpp)(g) & client->format.greenMax) << client->format.greenShift |	\
   ((CARD##bpp)(b) & client->format.blueMax) << client->format.blueShift)

#define RGB24_TO_PIXEL(bpp,r,g,b)                                       \
   ((((CARD##bpp)(r) & 0xFF) * client->format.redMax + 127) / 255             \
    << client->format.redShift |                                              \
    (((CARD##bpp)(g) & 0xFF) * client->format.greenMax + 127) / 255           \
    << client->format.greenShift |                                            \
    (((CARD##bpp)(b) & 0xFF) * client->format.blueMax + 127) / 255            \
    << client->format.blueShift)

#define RGB24_TO_PIXEL32(r,g,b)						\
  (((uint32_t)(r) & 0xFF) << client->format.redShift |				\
   ((uint32_t)(g) & 0xFF) << client->format.greenShift |			\
   ((uint32_t)(b) & 0xFF) << client->format.blueShift)

#endif

/* Type declarations */

typedef void (*filterPtrBPP)(rfbClient* client, int, CARDBPP *);

/* Prototypes */

static int InitFilterCopyBPP (rfbClient* client, int rw, int rh);
static int InitFilterPaletteBPP (rfbClient* client, int rw, int rh);
static int InitFilterGradientBPP (rfbClient* client, int rw, int rh);
static void FilterCopyBPP (rfbClient* client, int numRows, CARDBPP *destBuffer);
static void FilterPaletteBPP (rfbClient* client, int numRows, CARDBPP *destBuffer);
static void FilterGradientBPP (rfbClient* client, int numRows, CARDBPP *destBuffer);

#if BPP != 8
static rfbBool DecompressJpegRectBPP(rfbClient* client, int x, int y, int w, int h);
#endif

/* Definitions */

static rfbBool
HandleTightBPP (rfbClient* client, int rx, int ry, int rw, int rh)
{
  CARDBPP fill_colour;
  uint8_t comp_ctl;
  uint8_t filter_id;
  filterPtrBPP filterFn;
  z_streamp zs;
  char *buffer2;
  int err, stream_id, compressedLen, bitsPixel;
  int bufferSize, rowSize, numRows, portionLen, rowsProcessed, extraBytes;

  if (!ReadFromRFBServer(client, (char *)&comp_ctl, 1))
    return FALSE;

  /* Flush zlib streams if we are told by the server to do so. */
  for (stream_id = 0; stream_id < 4; stream_id++) {
    if ((comp_ctl & 1) && client->zlibStreamActive[stream_id]) {
      if (inflateEnd (&client->zlibStream[stream_id]) != Z_OK &&
	  client->zlibStream[stream_id].msg != NULL)
	rfbClientLog("inflateEnd: %s\n", client->zlibStream[stream_id].msg);
      client->zlibStreamActive[stream_id] = FALSE;
    }
    comp_ctl >>= 1;
  }

  /* Handle solid rectangles. */
  if (comp_ctl == rfbTightFill) {
#if BPP == 32
    if (client->format.depth == 24 && client->format.redMax == 0xFF &&
	client->format.greenMax == 0xFF && client->format.blueMax == 0xFF) {
      if (!ReadFromRFBServer(client, client->buffer, 3))
	return FALSE;
      fill_colour = RGB24_TO_PIXEL32(client->buffer[0], client->buffer[1], client->buffer[2]);
    } else {
      if (!ReadFromRFBServer(client, (char*)&fill_colour, sizeof(fill_colour)))
	return FALSE;
    }
#else
    if (!ReadFromRFBServer(client, (char*)&fill_colour, sizeof(fill_colour)))
	return FALSE;
#endif

    FillRectangle(client, rx, ry, rw, rh, fill_colour);

    return TRUE;
  }

#if BPP == 8
  if (comp_ctl == rfbTightJpeg) {
    rfbClientLog("Tight encoding: JPEG is not supported in 8 bpp mode.\n");
    return FALSE;
  }
#else
  if (comp_ctl == rfbTightJpeg) {
    return DecompressJpegRectBPP(client, rx, ry, rw, rh);
  }
#endif

  /* Quit on unsupported subencoding value. */
  if (comp_ctl > rfbTightMaxSubencoding) {
    rfbClientLog("Tight encoding: bad subencoding value received.\n");
    return FALSE;
  }

  /*
   * Here primary compression mode handling begins.
   * Data was processed with optional filter + zlib compression.
   */

  /* First, we should identify a filter to use. */
  if ((comp_ctl & rfbTightExplicitFilter) != 0) {
    if (!ReadFromRFBServer(client, (char*)&filter_id, 1))
      return FALSE;

    switch (filter_id) {
    case rfbTightFilterCopy:
      filterFn = FilterCopyBPP;
      bitsPixel = InitFilterCopyBPP(client, rw, rh);
      break;
    case rfbTightFilterPalette:
      filterFn = FilterPaletteBPP;
      bitsPixel = InitFilterPaletteBPP(client, rw, rh);
      break;
    case rfbTightFilterGradient:
      filterFn = FilterGradientBPP;
      bitsPixel = InitFilterGradientBPP(client, rw, rh);
      break;
    default:
      rfbClientLog("Tight encoding: unknown filter code received.\n");
      return FALSE;
    }
  } else {
    filterFn = FilterCopyBPP;
    bitsPixel = InitFilterCopyBPP(client, rw, rh);
  }
  if (bitsPixel == 0) {
    rfbClientLog("Tight encoding: error receiving palette.\n");
    return FALSE;
  }

  /* Determine if the data should be decompressed or just copied. */
  rowSize = (rw * bitsPixel + 7) / 8;
  if (rh * rowSize < TIGHT_MIN_TO_COMPRESS) {
    if (!ReadFromRFBServer(client, (char*)client->buffer, rh * rowSize))
      return FALSE;

    buffer2 = &client->buffer[TIGHT_MIN_TO_COMPRESS * 4];
    filterFn(client, rh, (CARDBPP *)buffer2);

    CopyRectangle(client, (uint8_t *)buffer2, rx, ry, rw, rh);

    return TRUE;
  }

  /* Read the length (1..3 bytes) of compressed data following. */
  compressedLen = (int)ReadCompactLen(client);
  if (compressedLen <= 0) {
    rfbClientLog("Incorrect data received from the server.\n");
    return FALSE;
  }

  /* Now let's initialize compression stream if needed. */
  stream_id = comp_ctl & 0x03;
  zs = &client->zlibStream[stream_id];
  if (!client->zlibStreamActive[stream_id]) {
    zs->zalloc = Z_NULL;
    zs->zfree = Z_NULL;
    zs->opaque = Z_NULL;
    err = inflateInit(zs);
    if (err != Z_OK) {
      if (zs->msg != NULL)
	rfbClientLog("InflateInit error: %s.\n", zs->msg);
      return FALSE;
    }
    client->zlibStreamActive[stream_id] = TRUE;
  }

  /* Read, decode and draw actual pixel data in a loop. */

  bufferSize = RFB_BUFFER_SIZE * bitsPixel / (bitsPixel + BPP) & 0xFFFFFFFC;
  buffer2 = &client->buffer[bufferSize];
  if (rowSize > bufferSize) {
    /* Should be impossible when RFB_BUFFER_SIZE >= 16384 */
    rfbClientLog("Internal error: incorrect buffer size.\n");
    return FALSE;
  }

  rowsProcessed = 0;
  extraBytes = 0;

  while (compressedLen > 0) {
    if (compressedLen > ZLIB_BUFFER_SIZE)
      portionLen = ZLIB_BUFFER_SIZE;
    else
      portionLen = compressedLen;

    if (!ReadFromRFBServer(client, (char*)client->zlib_buffer, portionLen))
      return FALSE;

    compressedLen -= portionLen;

    zs->next_in = (Bytef *)client->zlib_buffer;
    zs->avail_in = portionLen;

    do {
      zs->next_out = (Bytef *)&client->buffer[extraBytes];
      zs->avail_out = bufferSize - extraBytes;

      err = inflate(zs, Z_SYNC_FLUSH);
      if (err == Z_BUF_ERROR)   /* Input exhausted -- no problem. */
	break;
      if (err != Z_OK && err != Z_STREAM_END) {
	if (zs->msg != NULL) {
	  rfbClientLog("Inflate error: %s.\n", zs->msg);
	} else {
	  rfbClientLog("Inflate error: %d.\n", err);
	}
	return FALSE;
      }

      numRows = (bufferSize - zs->avail_out) / rowSize;

      filterFn(client, numRows, (CARDBPP *)buffer2);

      extraBytes = bufferSize - zs->avail_out - numRows * rowSize;
      if (extraBytes > 0)
	memcpy(client->buffer, &client->buffer[numRows * rowSize], extraBytes);

      CopyRectangle(client, (uint8_t *)buffer2, rx, ry+rowsProcessed, rw, numRows);

      rowsProcessed += numRows;
    }
    while (zs->avail_out == 0);
  }

  if (rowsProcessed != rh) {
    rfbClientLog("Incorrect number of scan lines after decompression.\n");
    return FALSE;
  }

  return TRUE;
}

/*----------------------------------------------------------------------------
 *
 * Filter stuff.
 *
 */

static int
InitFilterCopyBPP (rfbClient* client, int rw, int rh)
{
  client->rectWidth = rw;

#if BPP == 32
  if (client->format.depth == 24 && client->format.redMax == 0xFF &&
      client->format.greenMax == 0xFF && client->format.blueMax == 0xFF) {
    client->cutZeros = TRUE;
    return 24;
  } else {
    client->cutZeros = FALSE;
  }
#endif

  return BPP;
}

static void
FilterCopyBPP (rfbClient* client, int numRows, CARDBPP *dst)
{

#if BPP == 32
  int x, y;

  if (client->cutZeros) {
    for (y = 0; y < numRows; y++) {
      for (x = 0; x < client->rectWidth; x++) {
	dst[y*client->rectWidth+x] =
	  RGB24_TO_PIXEL32(client->buffer[(y*client->rectWidth+x)*3],
			   client->buffer[(y*client->rectWidth+x)*3+1],
			   client->buffer[(y*client->rectWidth+x)*3+2]);
      }
    }
    return;
  }
#endif

  memcpy (dst, client->buffer, numRows * client->rectWidth * (BPP / 8));
}

static int
InitFilterGradientBPP (rfbClient* client, int rw, int rh)
{
  int bits;

  bits = InitFilterCopyBPP(client, rw, rh);
  if (client->cutZeros)
    memset(client->tightPrevRow, 0, rw * 3);
  else
    memset(client->tightPrevRow, 0, rw * 3 * sizeof(uint16_t));

  return bits;
}

#if BPP == 32

static void
FilterGradient24 (rfbClient* client, int numRows, uint32_t *dst)
{
  int x, y, c;
  uint8_t thisRow[2048*3];
  uint8_t pix[3];
  int est[3];

  for (y = 0; y < numRows; y++) {

    /* First pixel in a row */
    for (c = 0; c < 3; c++) {
      pix[c] = client->tightPrevRow[c] + client->buffer[y*client->rectWidth*3+c];
      thisRow[c] = pix[c];
    }
    dst[y*client->rectWidth] = RGB24_TO_PIXEL32(pix[0], pix[1], pix[2]);

    /* Remaining pixels of a row */
    for (x = 1; x < client->rectWidth; x++) {
      for (c = 0; c < 3; c++) {
	est[c] = (int)client->tightPrevRow[x*3+c] + (int)pix[c] -
		 (int)client->tightPrevRow[(x-1)*3+c];
	if (est[c] > 0xFF) {
	  est[c] = 0xFF;
	} else if (est[c] < 0x00) {
	  est[c] = 0x00;
	}
	pix[c] = (uint8_t)est[c] + client->buffer[(y*client->rectWidth+x)*3+c];
	thisRow[x*3+c] = pix[c];
      }
      dst[y*client->rectWidth+x] = RGB24_TO_PIXEL32(pix[0], pix[1], pix[2]);
    }

    memcpy(client->tightPrevRow, thisRow, client->rectWidth * 3);
  }
}

#endif

static void
FilterGradientBPP (rfbClient* client, int numRows, CARDBPP *dst)
{
  int x, y, c;
  CARDBPP *src = (CARDBPP *)client->buffer;
  uint16_t *thatRow = (uint16_t *)client->tightPrevRow;
  uint16_t thisRow[2048*3];
  uint16_t pix[3];
  uint16_t max[3];
  int shift[3];
  int est[3];

#if BPP == 32
  if (client->cutZeros) {
    FilterGradient24(client, numRows, dst);
    return;
  }
#endif

  max[0] = client->format.redMax;
  max[1] = client->format.greenMax;
  max[2] = client->format.blueMax;

  shift[0] = client->format.redShift;
  shift[1] = client->format.greenShift;
  shift[2] = client->format.blueShift;

  for (y = 0; y < numRows; y++) {

    /* First pixel in a row */
    for (c = 0; c < 3; c++) {
      pix[c] = (uint16_t)(((src[y*client->rectWidth] >> shift[c]) + thatRow[c]) & max[c]);
      thisRow[c] = pix[c];
    }
    dst[y*client->rectWidth] = RGB_TO_PIXEL(BPP, pix[0], pix[1], pix[2]);

    /* Remaining pixels of a row */
    for (x = 1; x < client->rectWidth; x++) {
      for (c = 0; c < 3; c++) {
	est[c] = (int)thatRow[x*3+c] + (int)pix[c] - (int)thatRow[(x-1)*3+c];
	if (est[c] > (int)max[c]) {
	  est[c] = (int)max[c];
	} else if (est[c] < 0) {
	  est[c] = 0;
	}
	pix[c] = (uint16_t)(((src[y*client->rectWidth+x] >> shift[c]) + est[c]) & max[c]);
	thisRow[x*3+c] = pix[c];
      }
      dst[y*client->rectWidth+x] = RGB_TO_PIXEL(BPP, pix[0], pix[1], pix[2]);
    }
    memcpy(thatRow, thisRow, client->rectWidth * 3 * sizeof(uint16_t));
  }
}

static int
InitFilterPaletteBPP (rfbClient* client, int rw, int rh)
{
  uint8_t numColors;
#if BPP == 32
  int i;
  CARDBPP *palette = (CARDBPP *)client->tightPalette;
#endif

  client->rectWidth = rw;

  if (!ReadFromRFBServer(client, (char*)&numColors, 1))
    return 0;

  client->rectColors = (int)numColors;
  if (++client->rectColors < 2)
    return 0;

#if BPP == 32
  if (client->format.depth == 24 && client->format.redMax == 0xFF &&
      client->format.greenMax == 0xFF && client->format.blueMax == 0xFF) {
    if (!ReadFromRFBServer(client, (char*)&client->tightPalette, client->rectColors * 3))
      return 0;
    for (i = client->rectColors - 1; i >= 0; i--) {
      palette[i] = RGB24_TO_PIXEL32(client->tightPalette[i*3],
				    client->tightPalette[i*3+1],
				    client->tightPalette[i*3+2]);
    }
    return (client->rectColors == 2) ? 1 : 8;
  }
#endif

  if (!ReadFromRFBServer(client, (char*)&client->tightPalette, client->rectColors * (BPP / 8)))
    return 0;

  return (client->rectColors == 2) ? 1 : 8;
}

static void
FilterPaletteBPP (rfbClient* client, int numRows, CARDBPP *dst)
{
  int x, y, b, w;
  uint8_t *src = (uint8_t *)client->buffer;
  CARDBPP *palette = (CARDBPP *)client->tightPalette;

  if (client->rectColors == 2) {
    w = (client->rectWidth + 7) / 8;
    for (y = 0; y < numRows; y++) {
      for (x = 0; x < client->rectWidth / 8; x++) {
	for (b = 7; b >= 0; b--)
	  dst[y*client->rectWidth+x*8+7-b] = palette[src[y*w+x] >> b & 1];
      }
      for (b = 7; b >= 8 - client->rectWidth % 8; b--) {
	dst[y*client->rectWidth+x*8+7-b] = palette[src[y*w+x] >> b & 1];
      }
    }
  } else {
    for (y = 0; y < numRows; y++)
      for (x = 0; x < client->rectWidth; x++)
	dst[y*client->rectWidth+x] = palette[(int)src[y*client->rectWidth+x]];
  }
}

#if BPP != 8

/*----------------------------------------------------------------------------
 *
 * JPEG decompression.
 *
 */

static rfbBool
DecompressJpegRectBPP(rfbClient* client, int x, int y, int w, int h)
{
  tjhandle tjh;
  int tjerr;
  int compressedLen;
  uint8_t *compressedData;
  int pitch;
  unsigned char *pixelPtr;

  Prof_Scope prof( PROF_RFB_ENCODING_TIGHT_JPEG );

  compressedLen = (int)ReadCompactLen(client);
  if (compressedLen <= 0) {
    rfbClientLog("Incorrect data received from the server.\n");
    return FALSE;
  }

  compressedData = (uint8_t*)malloc(compressedLen);
  if (compressedData == NULL) {
    rfbClientLog("Memory allocation error.\n");
    return FALSE;
  }

  if (!ReadFromRFBServer(client, (char*)compressedData, compressedLen)) {
    free(compressedData);
    return FALSE;
  }

  tjh = tjInitDecompress();
  if (!tjh)
  {
    rfbClientLog("TurboJPEG init error: %s\n", tjGetErrorStr());
    free(compressedData);
    return FALSE;
  }

  pitch = client->width * BPP/8;
  pixelPtr = &client->frameBuffer[y * pitch + x * BPP/8];

  // $$$ only 32bit RGBX is actually implemented here.
  
  tjerr = tjDecompress2(tjh, compressedData, compressedLen, pixelPtr, w, pitch, h, TJPF_RGBX, 0);
  if (tjerr < 0)
    rfbClientLog("TurboJPEG decompress error: %s\n", tjGetErrorStr());

  tjDestroy(tjh);
  free(compressedData);

  return tjerr == 0;
}

#else

static long
ReadCompactLen (rfbClient* client)
{
  long len;
  uint8_t b;

  if (!ReadFromRFBServer(client, (char *)&b, 1))
    return -1;
  len = (int)b & 0x7F;
  if (b & 0x80) {
    if (!ReadFromRFBServer(client, (char *)&b, 1))
      return -1;
    len |= ((int)b & 0x7F) << 7;
    if (b & 0x80) {
      if (!ReadFromRFBServer(client, (char *)&b, 1))
	return -1;
      len |= ((int)b & 0xFF) << 14;
    }
  }
  return len;
}

#endif

#undef CARDBPP

/* LIBVNCSERVER_HAVE_LIBZ and LIBVNCSERVER_HAVE_LIBJPEG */
#endif
#endif

