/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/jasper/src/uds/bufferedWriter.c#4 $
 */

#include "bufferedWriter.h"

#include "compiler.h"
#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"

struct bufferedWriter {
  IORegion *bw_region;             // region to write to
  off_t     bw_pos;                // offset of start of buffer
  size_t    bw_size;               // size of buffer
  char     *bw_buf;                // start of buffer
  char     *bw_ptr;                // end of written data
  int       bw_err;                // error code
  bool      bw_used;               // have writes been done?
};

/*****************************************************************************/
int makeBufferedWriter(IORegion *region, BufferedWriter **writerPtr)
{
  size_t bufSize;
  int result = getRegionBestBufferSize(region, &bufSize);
  if (result != UDS_SUCCESS) {
    return result;
  }

  BufferedWriter *writer;
  result = ALLOCATE(1, BufferedWriter, "buffered writer", &writer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  *writer = (BufferedWriter) {
    .bw_region = region,
    .bw_size   = bufSize,
    .bw_pos    = 0,
    .bw_err    = UDS_SUCCESS,
    .bw_used   = false,
  };

  result = ALLOCATE_IO_ALIGNED(bufSize, char, "buffer writer buffer",
                               &writer->bw_buf);
  if (result != UDS_SUCCESS) {
    FREE(writer);
    return result;
  }

  getIORegion(region);
  writer->bw_ptr = writer->bw_buf;
  *writerPtr = writer;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void freeBufferedWriter(BufferedWriter *bw)
{
  if (bw) {
    int result = syncRegionContents(bw->bw_region);
    if (result != UDS_SUCCESS) {
      logWarningWithStringError(result, "%s cannot sync region", __func__);
    }
    putIORegion(bw->bw_region);
    FREE(bw->bw_buf);
    FREE(bw);
  }
}

/*****************************************************************************/
static INLINE size_t spaceUsedInBuffer(BufferedWriter *bw)
{
  return bw->bw_ptr - bw->bw_buf;
}

/*****************************************************************************/
size_t spaceRemainingInWriteBuffer(BufferedWriter *bw)
{
  return bw->bw_size - spaceUsedInBuffer(bw);
}

/*****************************************************************************/
int writeToBufferedWriter(BufferedWriter *bw, const void *data, size_t len)
{
  bool alwaysCopy = false;
  if (bw->bw_err != UDS_SUCCESS) {
    return bw->bw_err;
  }

  const byte *dp = data;
  int result = UDS_SUCCESS;
  while ((len > 0) && (result == UDS_SUCCESS)) {
    if ((len >= bw->bw_size) && !alwaysCopy && (spaceUsedInBuffer(bw) == 0)) {
      size_t n = len / bw->bw_size * bw->bw_size;
      int result = writeToRegion(bw->bw_region, bw->bw_pos, dp, n, n);
      if (result == UDS_INCORRECT_ALIGNMENT) {
        // Usually the buffer is not correctly aligned, so switch to
        // copying from the buffer
        result = UDS_SUCCESS;
        alwaysCopy = true;
        continue;
      } else if (result != UDS_SUCCESS) {
        logWarningWithStringError(result, "failed in writeToBufferedWriter");
        bw->bw_err = result;
        break;
      }
      bw->bw_pos += n;
      dp         += n;
      len        -= n;
      continue;
    }

    size_t avail = spaceRemainingInWriteBuffer(bw);
    size_t chunk = minSizeT(len, avail);
    memcpy(bw->bw_ptr, dp, chunk);
    len        -= chunk;
    dp         += chunk;
    bw->bw_ptr += chunk;

    if (spaceRemainingInWriteBuffer(bw) == 0) {
      result = flushBufferedWriter(bw);
    }
  }

  bw->bw_used = true;
  return result;
}

/*****************************************************************************/
int writeZerosToBufferedWriter(BufferedWriter *bw, size_t len)
{
  if (bw->bw_err != UDS_SUCCESS) {
    return bw->bw_err;
  }

  int result = UDS_SUCCESS;
  while ((len > 0) && (result == UDS_SUCCESS)) {
    size_t avail = spaceRemainingInWriteBuffer(bw);
    size_t chunk = minSizeT(len, avail);
    memset(bw->bw_ptr, 0, chunk);
    len        -= chunk;
    bw->bw_ptr += chunk;

    if (spaceRemainingInWriteBuffer(bw) == 0) {
      result = flushBufferedWriter(bw);
    }
  }

  bw->bw_used = true;
  return result;
}

/*****************************************************************************/
int flushBufferedWriter(BufferedWriter *bw)
{
  if (bw->bw_err != UDS_SUCCESS) {
    return bw->bw_err;
  }

  size_t n = spaceUsedInBuffer(bw);
  if (n > 0) {
    int result = writeToRegion(bw->bw_region, bw->bw_pos, bw->bw_buf,
                               bw->bw_size, n);
    if (result != UDS_SUCCESS) {
      return bw->bw_err = result;
    } else {
      bw->bw_ptr =  bw->bw_buf;
      bw->bw_pos += bw->bw_size;
    }
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
bool wasBufferedWriterUsed(const BufferedWriter *bw)
{
  return bw->bw_used;
}

/*****************************************************************************/
void noteBufferedWriterUsed(BufferedWriter *bw)
{
  bw->bw_used = true;
}
