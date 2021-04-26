/* Generate a core file for the inferior process.

   Copyright (C) 2001-2020 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "elf-bfd.h"
#include "infcall.h"
#include "inferior.h"
#include "gdbcore.h"
#include "objfiles.h"
#include "solib.h"
#include "symfile.h"
#include "arch-utils.h"
#include "completer.h"
#include "gcore.h"
#include "cli/cli-decode.h"
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include "regcache.h"
#include "regset.h"
#include "gdb_bfd.h"
#include "readline/tilde.h"
#include <algorithm>
#include "gdbsupport/gdb_unlinker.h"
#include "gdbsupport/byte-vector.h"
#include "gdbsupport/scope-exit.h"
#include "zlib.h"

/* The largest amount of memory to read from the target at once.  We
   must throttle it to limit the amount of memory used by GDB during
   generate-core-file for programs with large resident data.  */
#define MAX_COPY_BYTES (1024 * 1024)

struct pending_section_desc
{
    // Link to next section
    struct pending_section_desc * next;

    int id;

    // The section
    asection * osec;
    long long offset;
    long long length;
};

struct compressed_core_file;

struct write_op
{
    struct write_op * next;
    long long offset;
    void const * data;
    file_ptr size;
};

struct compressed_core_segment
{
    struct compressed_core_file * parent;

    // Sequence number of current segment, allocated from compressed_core_file::seq
    int seq;

    unsigned long long startSectOffset;
    struct pending_section_desc * startSect;

    // The start offset in virtual file of current segment indicated by fd
    long long startOffset;

    // Real file descriptor of current writing segment
    int fd;

    // CRC32 of current writing segment, the final CRC32 can be computed using crc32_combine().
    unsigned long crc32;

    // The ZLIB compressor
    z_stream compr;

    // Current write offset in the virtual file
    long long offset;

    void const * static_buf;
    struct write_op oFirstWrite;
    struct write_op * pLastWrite;

    unsigned char outBuf[16384];
};



struct compressed_core_file
{
    // Segment file sequence number, the higher numbered segment will overwrite lower ones if they overlap.
    int seq;

    long long length;

    struct bfd * obfd;

    // Current segment receiving source data.
    int curSegId;

    pthread_mutex_t mutex;

    unsigned long long totalPendingBytes;

    // Pending sections to be written.
    struct pending_section_desc * first;
    struct pending_section_desc * last;

    unsigned long long parallelBatchSize;

    compressed_core_segment segs[32];
};

static const char *default_gcore_target(void);
static enum bfd_architecture default_gcore_arch(void);
static unsigned long default_gcore_mach(void);
static int gcore_memory_sections(bfd *, struct compressed_core_file *);

static void *
open_gcore_dir(struct bfd *abfd, void * ctx)
{
    // Allocate stream object
    struct compressed_core_file * pStream = (struct compressed_core_file *)ctx;

    // Initialize
    memset(pStream, 0, sizeof(struct compressed_core_file));
    pStream->seq = -1;
    pStream->obfd = abfd;
    pStream->curSegId = -1;
    pStream->length = 0;
    pStream->totalPendingBytes = 0;
    pStream->first = NULL;
    pStream->last = NULL;
    pStream->mutex = PTHREAD_MUTEX_INITIALIZER;

    // Initialize compressor for segment 0
    if (deflateInit2(&pStream->segs[0].compr, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    {
        return NULL;
    }

    for (int i = 0; i < 32; i++)
    {
        pStream->segs[i].parent = pStream;
        pStream->segs[i].seq = -1;
        pStream->segs[i].fd = -1;
        pStream->segs[i].crc32 = 0;
        pStream->segs[i].offset = -1;
        pStream->segs[i].startOffset = -1;
        pStream->segs[i].startSectOffset = 0;
        pStream->segs[i].startSect = NULL;
        pStream->segs[i].pLastWrite = NULL;
    }


    // Create folder 
    const char * pcszDirPathName = bfd_get_filename(abfd);
    if (mkdir(pcszDirPathName, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0)
    {
        deflateEnd(&pStream->segs[0].compr);
        free(pStream);
        return NULL;
    }

    return pStream;
}

static void
close_gcore_segment(struct bfd * pBbfd, struct compressed_core_segment * pStream)
{
    pStream->compr.next_in = (Bytef *)pStream; // Just anything not NULL
    pStream->compr.avail_in = 0;

    // Flush deflate stream
    for (;;)
    {
        pStream->compr.next_out = (Bytef *)pStream->outBuf;
        pStream->compr.avail_out = sizeof(pStream->outBuf);
        if (deflate(&pStream->compr, Z_FULL_FLUSH) != Z_OK)
        { // Z_STREAM_END only when Z_FINISH, Z_STREAM_ERROR when next_in/next_out is NULL, Z_BUF_ERROR when input avail_in/avail_out is 0, all impossible.
            break;
        }

        // Write compressed data in output buffer to disk file.
        if (pStream->compr.avail_out < sizeof(pStream->outBuf))
        {
            size_t stToWrite = sizeof(pStream->outBuf) - pStream->compr.avail_out;
            ssize_t sstWritten = write(pStream->fd, pStream->outBuf, stToWrite);
            if (sstWritten != stToWrite)
            {
                break;
            }
        }

        // If the output buffer is filled to full, there may be more data to output, try another round.
        if (pStream->compr.avail_out == 0)
        {
            continue;
        }

        // All data outputed, because there is still free space left in output buffer.
        break;
    }

    close(pStream->fd);
    pStream->fd = -1;

    // Construct current (partial) file name
    char * pszSrcFileName = (char *)pStream->outBuf;
    const char * pcszDirPathName = bfd_get_filename(pBbfd);
    int i32Chars = snprintf(pszSrcFileName, sizeof(pStream->outBuf), "%s/%d.%lld", pcszDirPathName, pStream->seq, pStream->startOffset);
    if (i32Chars >= sizeof(pStream->outBuf))
    {
        return;
    }

    // Construct new (full) file name
    char * pszDstFileName = pszSrcFileName + i32Chars + 1;
    i32Chars = snprintf(pszDstFileName, sizeof(pStream->outBuf) - i32Chars - 1, "%s.%lld.%08lX", pszSrcFileName, pStream->offset, pStream->crc32);
    if (i32Chars >= sizeof(pStream->outBuf) - i32Chars - 1)
    {
        return;
    }

    rename(pszSrcFileName, pszDstFileName);
}

static int
fast_gcore_open_segment(struct bfd *nbfd, struct compressed_core_segment * pSeg, const void *buf, file_ptr nbytes, long long offset)
{
    // Compute the file name for the new segment
    char * pcszFileName = (char *)pSeg->outBuf;
    const char * pcszDirPathName = bfd_get_filename(nbfd);
    int i32Chars = snprintf(pcszFileName, sizeof(pSeg->outBuf), "%s/%d.%lld", pcszDirPathName, pSeg->seq, (long long)offset);
    if (i32Chars >= sizeof(pSeg->outBuf))
    {
        warning(_("Failed to construct file name for segment #%zu with seq %d and offset %lld ~ %lld when writing to %lld\n"), pSeg - pSeg->parent->segs, pSeg->seq, pSeg->startOffset, pSeg->offset, offset);
        return 0;
    }

    // Create the new segment file
    pSeg->fd = open(pcszFileName, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IROTH);
    if (pSeg->fd < 0)
    {
        warning(_("Failed to create file %s for segment #%zu with seq %d and offset %lld ~ %lld when writing to %lld\n"), pcszFileName, pSeg - pSeg->parent->segs, pSeg->seq, pSeg->startOffset, pSeg->offset, offset);
        return 0;
    }

    if (info_verbose)
    {
        fprintf_filtered(gdb_stdout, "Opened file %s for segment #%zu with seq %d and offset %lld ~ %lld when writing to %lld\n", pcszFileName, pSeg - pSeg->parent->segs, pSeg->seq, pSeg->startOffset, pSeg->offset, offset);
    }

    pSeg->startOffset = offset;
    pSeg->offset = offset;
    pSeg->crc32 = crc32(0L, Z_NULL, 0);

    return 1;
}

static int
fast_gcore_write_segment(struct bfd *nbfd, struct compressed_core_segment * pSeg, const void *buf, file_ptr nbytes, long long offset)
{
    if (offset != pSeg->offset)
    { // Seeked, we should start a new file
        if (pSeg->fd >= 0)
        {
            if (info_verbose)
            {
                fprintf_filtered(gdb_stdout, "Closing segment #%zu with seq %d and offset %lld ~ %lld when writing to %lld\n", pSeg - pSeg->parent->segs, pSeg->seq, pSeg->startOffset, pSeg->offset, offset);
            }
            close_gcore_segment(nbfd, pSeg);

            deflateReset(&pSeg->compr);

            // NOTE: This should not be reached in parallel mode
            pthread_mutex_lock(&pSeg->parent->mutex);
            pSeg->seq = ++pSeg->parent->seq;
            pthread_mutex_unlock(&pSeg->parent->mutex);
        }

        if (!fast_gcore_open_segment(nbfd, pSeg, buf, nbytes, offset))
        {
            return 0;
        }
    }

    // Publish the input data to compressor
    pSeg->compr.next_in = (Bytef *)buf;
    pSeg->compr.avail_in = (uInt)nbytes;

    // Compress.
    // NOTE: When avail_in == 0, it is possible if we do another round, we can get more output. But we will leave that to next call of this function.
    //       Here the only important thing is to consume the input data, not write output as much as possible.
    while (pSeg->compr.avail_in > 0)
    {
        pSeg->compr.next_out = (Bytef *)pSeg->outBuf;
        pSeg->compr.avail_out = sizeof(pSeg->outBuf);

        if (deflate(&pSeg->compr, Z_NO_FLUSH) != Z_OK)
        { // Z_STREAM_END only when Z_FINISH, Z_STREAM_ERROR when next_in/next_out is NULL, Z_BUF_ERROR when input avail_in/avail_out is 0, all impossible.
            warning(_("Failed to compress data for segment #%zu with seq %d and offset %lld ~ %lld when writing to %lld\n"), pSeg - pSeg->parent->segs, pSeg->seq, pSeg->startOffset, pSeg->offset, offset);
            return 0;
        }

        // Write compressed data in output buffer to disk file.
        if (pSeg->compr.avail_out < sizeof(pSeg->outBuf))
        {
            size_t stToWrite = sizeof(pSeg->outBuf) - pSeg->compr.avail_out;
            ssize_t sstWritten = write(pSeg->fd, pSeg->outBuf, stToWrite);
            if (sstWritten != stToWrite)
            {
                warning(_("Failed to write data for segment #%zu with seq %d and offset %lld ~ %lld when writing to %lld\n"), pSeg - pSeg->parent->segs, pSeg->seq, pSeg->startOffset, pSeg->offset, offset);
                return 0;
            }
        }
    }

    // Update write offset
    pSeg->offset = offset + nbytes;

    // Update file length if necessary.
    if (pSeg->parent->length < pSeg->offset)
    {
        pSeg->parent->length = pSeg->offset;
    }

    // Update CRC32
    pSeg->crc32 = crc32(pSeg->crc32, (const Bytef *)buf, nbytes);

    return 1;
}


static int
fast_gcore_flush_segment(struct bfd *nbfd, struct compressed_core_segment * pSeg)
{
    for (struct write_op * pScan = &pSeg->oFirstWrite; pScan != NULL; pScan = pScan->next)
    {
        if (!fast_gcore_write_segment(nbfd, pSeg, pScan->data, pScan->size, pScan->offset))
        {
            return 0;
        }
    }

    return 1;
}

static file_ptr
write_gcore_segment_file(struct bfd *nbfd, void *stream, const void *buf, file_ptr nbytes, file_ptr offset)
{
    if (nbytes == 0)
    {
        return 0;
    }

    struct compressed_core_file * pStream = (struct compressed_core_file *)stream;

    // Check if we are in parallel mode, if so, just remember the buffer or data, without really compressing and writing anything
    if (pStream->curSegId >= 0)
    { // Parallel mode
        struct compressed_core_segment * pSeg = pStream->segs + pStream->curSegId;

        if (pSeg->pLastWrite == NULL)
        {
            pSeg->pLastWrite = &pSeg->oFirstWrite;
        }
        else
        {
            pSeg->pLastWrite->next = (struct write_op *)malloc(sizeof(struct write_op));
            if (pSeg->pLastWrite->next == NULL)
            {
                return 0;
            }

            pSeg->pLastWrite = pSeg->pLastWrite->next;
        }

        if (pSeg->static_buf == buf)
        {
            pSeg->pLastWrite->data = buf;
        }
        else
        {
            void * newData = malloc(nbytes);
            if (newData == NULL)
            {
                return 0;
            }
            memcpy(newData, buf, nbytes);
            pSeg->pLastWrite->data = newData;
        }

        pSeg->pLastWrite->size = nbytes;
        pSeg->pLastWrite->offset = offset;
        pSeg->pLastWrite->next = NULL;

        return nbytes;
    }
    else
    {
        // Not in parallel mode, assume the [0] segment is always available
        struct compressed_core_segment * pSeg = pStream->segs;
        pSeg->pLastWrite = &pSeg->oFirstWrite;
        pSeg->oFirstWrite.data = buf;
        pSeg->oFirstWrite.size = nbytes;
        pSeg->oFirstWrite.offset = offset;
        pSeg->oFirstWrite.next = NULL;

        return fast_gcore_flush_segment(nbfd, pSeg) ? nbytes : 0;
    }
}

static char
g_szError[] = { "error" };

static void *
fast_gcore_compressor(void* pvCtx)
{
    struct compressed_core_segment * pSeg = (struct compressed_core_segment *)pvCtx;

    gdb::byte_vector memhunk(MAX_COPY_BYTES);

    int bFail = 0;
    unsigned long long acc = 0;
    unsigned long long limit = pSeg->parent->parallelBatchSize;
    file_ptr inSectOffset = pSeg->startSectOffset;

    for (struct pending_section_desc * pScan = pSeg->startSect; pScan != NULL && acc < limit; pScan = pScan->next, inSectOffset = 0)
    {
        bfd_size_type sectSize = bfd_section_size(pScan->osec);

        if (info_verbose)
        {
            fprintf_filtered(gdb_stdout, "------- Segment %d wrting section %d.%p starting from offset %zu\n", pSeg->seq, pScan->id, pScan, inSectOffset);
        }

        while (acc < limit && inSectOffset < sectSize)
        {
            bfd_size_type batchSize = MAX_COPY_BYTES;
            if (batchSize > sectSize - inSectOffset)
            {
                batchSize = sectSize - inSectOffset;
            }
            if (batchSize > limit - acc)
            {
                batchSize = limit - acc;
            }

            // Prepare segment for capture write contents
            pSeg->pLastWrite = NULL;
            pSeg->static_buf = memhunk.data();

            pthread_mutex_lock(&pSeg->parent->mutex);

            pSeg->parent->curSegId = pSeg - pSeg->parent->segs;

            if (target_read_memory(bfd_section_vma(pScan->osec) + inSectOffset, memhunk.data(), batchSize) != 0)
            {
                warning(_("Memory read failed for corefile section, %s bytes at %s."), plongest(batchSize), paddress(target_gdbarch(), bfd_section_vma(pScan->osec)));
                bFail = 1;
            }

            if (!bFail && !bfd_set_section_contents(pSeg->parent->obfd, pScan->osec, memhunk.data(), inSectOffset, batchSize))
            {
                warning(_("Failed to write corefile contents (%s)."), bfd_errmsg(bfd_get_error()));
                bFail = 1;
            }

            pthread_mutex_unlock(&pSeg->parent->mutex);

            if (bFail)
            {
                break;
            }

            inSectOffset += batchSize;
            acc += batchSize;

            // Compress and write
            if (!fast_gcore_flush_segment(pSeg->parent->obfd, pSeg))
            {
                bFail = 1;
            }

            // Cleanup buffers used to capture the write data
            for (struct write_op * pFree = &pSeg->oFirstWrite; pFree != NULL; )
            {
                struct write_op * pNext = pFree->next;

                if (pFree->data != memhunk.data())
                {
                    free((void*)pFree->data);
                }

                if (pFree != &pSeg->oFirstWrite)
                {
                    free(pFree);
                }

                pFree = pNext;
            }

            if (bFail)
            {
                break;
            }
        } // while (inSectOffset < sectSize)

        if (bFail)
        {
            break;
        }
    }

    pSeg->static_buf = NULL;

    return bFail ? g_szError : NULL;
}

static int
fast_gcore_flush_segments(struct bfd *nbfd, struct compressed_core_file * pStream)
{
    if (pStream->segs[0].fd >= 0)
    {
        close_gcore_segment(nbfd, pStream->segs);

        deflateReset(&pStream->segs[0].compr);
    }

    // Split the sections to number of CPU groups
    int cpuCoreCount = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cpuCoreCount < 0)
    {
        cpuCoreCount = 4;
    }
    else if (cpuCoreCount > 32)
    {
        cpuCoreCount = 32;
    }
    struct pending_section_desc * aSegmentStarts[32];
    long long aui64SegmentOffsets[32];

    unsigned long long limit = (pStream->totalPendingBytes + cpuCoreCount - 1) / cpuCoreCount;
    unsigned long long acc = limit;
    bfd_size_type sectSize = bfd_section_size(pStream->first->osec);
    bfd_size_type sectOffset = 0;

    int i32SegId = -1, k = 0;
    for (struct pending_section_desc * pScan = pStream->first; ;)
    {
        pScan->id = k;

        // If about to cross the limit, create new segment
        if (acc + (sectSize - sectOffset) > limit)
        {
            if (info_verbose)
            {
                fprintf_filtered(gdb_stdout, "===section #%d.%p partial %llu/%llu bytes at offset %llu to segment %d previously %llu bytes long\n", k, pScan, limit - acc, (long long)sectSize, (long long)sectOffset, i32SegId, acc);
            }

            // Allocate data in section to the old segment
            sectOffset += limit - acc;

            // Create new segment
            i32SegId++;
            assert(i32SegId < 32);

            // As current section still has remaining > 0, it will be the first section of the new segment
            aSegmentStarts[i32SegId] = pScan;

            // How many bytes assigned to previous segment, is just the in section offset we should start with.
            aui64SegmentOffsets[i32SegId] = sectOffset;

            // Reset accumulator for new segment
            acc = 0;

            if (info_verbose)
            {
                fprintf_filtered(gdb_stdout, "\n>>>>> started new segment %d by section #%d.%p remaining %llu/%llu bytes at offset %llu\n\n", i32SegId, k, pScan, (long long)(sectSize - sectOffset), (long long)sectSize, (long long)sectOffset);
            }
            continue;
        }

        acc += sectSize - sectOffset;

        if (info_verbose)
        {
            fprintf_filtered(gdb_stdout, "===section #%d.%p %llu/%llu all remaining bytes at offset %llu to segment %d makes it %llu bytes long\n", k, pScan, (long long)(sectSize - sectOffset), (long long)sectSize, (long long)sectOffset, i32SegId, acc);
        }
        
        pScan = pScan->next;
        if (pScan == NULL)
        { // No more sections, all remaining data included in last segment without exceeding the limit.
            break;
        }

        k++;
        sectSize = bfd_section_size(pScan->osec);
        sectOffset = 0;
    }


    int bFail = false;

    pStream->parallelBatchSize = limit;

    // Start a thread for each segment
    pthread_t aui32Threads[32];
    int i32ThreadCount = 0;
    for (int i = 0; i <= i32SegId; i++, i32ThreadCount++)
    {
        pStream->segs[i].parent = pStream;
        pStream->segs[i].seq = ++pStream->seq;
        pStream->segs[i].fd = -1;
        pStream->segs[i].crc32 = 0;
        pStream->segs[i].offset = -1;
        pStream->segs[i].startOffset = -1;
        pStream->segs[i].startSectOffset = aui64SegmentOffsets[i];
        pStream->segs[i].startSect = aSegmentStarts[i];
        pStream->segs[i].pLastWrite = NULL;

        if (i > 0)
        {
            if (deflateInit2(&pStream->segs[i].compr, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            {
                bFail = true;
                break;
            }
        }

        if (pthread_create(aui32Threads + i, nullptr, fast_gcore_compressor, pStream->segs + i) != 0)
        {
            bFail = true;
            break;
        }
    }

    // Wait for all threads to finish
    for (int i = 0; i < i32ThreadCount; i++)
    {
        void * pvRet = nullptr;
        pthread_join(aui32Threads[i], &pvRet);
        if (pvRet != nullptr)
        {
            error(_("Thread #%d failed\n"), i);
            bFail = true;
        }
    }

    pStream->curSegId = -1;

    if (bFail)
    {
        return 0;
    }

    return 1;
}

static int
close_gcore_dir(struct bfd *nbfd, void *stream)
{
    struct compressed_core_file * pStream = (struct compressed_core_file *)stream;
    for (int i = 0; i < 32; i++)
    {
        if (pStream->segs[i].fd >= 0)
        {
            close_gcore_segment(nbfd, pStream->segs + i);
        }

        deflateEnd(&pStream->segs[i].compr);
    }

    free(pStream);

    return 0;
}

static int
stat_gcore_dir(struct bfd *abfd, void *stream, struct stat *sb)
{
    struct compressed_core_file * pStream = (struct compressed_core_file *)stream;

    memset(sb, 0, sizeof(*sb));

    sb->st_size = pStream->length;
    sb->st_nlink = 1;
    sb->st_mode;

    return 0;
}

/* create_gcore_bfd -- helper for gcore_command (exported).
   Open a new bfd core file for output, and return the handle.  */

gdb_bfd_ref_ptr
create_gcore_bfd1 (const char *filename, struct compressed_core_file * pStream)
{
  //gdb_bfd_ref_ptr obfd (gdb_bfd_openw (filename, default_gcore_target ()));
  gdb_bfd_ref_ptr obfd (
                            pStream != nullptr
                          ? gdb_bfd_openw_iovec (filename, default_gcore_target (), open_gcore_dir, pStream, write_gcore_segment_file, close_gcore_dir, stat_gcore_dir)
                          : gdb_bfd_openw(filename, default_gcore_target())
                       );

  if (obfd == NULL)
    error (_("Failed to open '%s' for output."), filename);
  bfd_set_format (obfd.get (), bfd_core);
  bfd_set_arch_mach (obfd.get (), default_gcore_arch (), default_gcore_mach ());
  return obfd;
}

gdb_bfd_ref_ptr
create_gcore_bfd (const char *filename)
{
    return create_gcore_bfd1(filename, nullptr);
}

/* write_gcore_file_1 -- do the actual work of write_gcore_file.  */

static void
write_gcore_file_1 (bfd *obfd, struct compressed_core_file * pStream)
{
  gdb::unique_xmalloc_ptr<char> note_data;
  int note_size = 0;
  asection *note_sec = NULL;

  /* An external target method must build the notes section.  */
  /* FIXME: uweigand/2011-10-06: All architectures that support core file
     generation should be converted to gdbarch_make_corefile_notes; at that
     point, the target vector method can be removed.  */
  if (!gdbarch_make_corefile_notes_p (target_gdbarch ()))
    note_data.reset (target_make_corefile_notes (obfd, &note_size));
  else
    note_data.reset (gdbarch_make_corefile_notes (target_gdbarch (), obfd,
						  &note_size));

  if (note_data == NULL || note_size == 0)
    error (_("Target does not support core file generation."));

  /* Create the note section.  */
  note_sec = bfd_make_section_anyway_with_flags (obfd, "note0",
						 SEC_HAS_CONTENTS
						 | SEC_READONLY
						 | SEC_ALLOC);
  if (note_sec == NULL)
    error (_("Failed to create 'note' section for corefile: %s"),
	   bfd_errmsg (bfd_get_error ()));

  bfd_set_section_vma (note_sec, 0);
  bfd_set_section_alignment (note_sec, 0);
  bfd_set_section_size (note_sec, note_size);

  /* Now create the memory/load sections.  */
  if (gcore_memory_sections (obfd, pStream) == 0)
    error (_("gcore: failed to get corefile memory sections from target."));

  /* Write out the contents of the note section.  */
  if (!bfd_set_section_contents (obfd, note_sec, note_data.get (), 0,
				 note_size))
    warning (_("writing note section (%s)"), bfd_errmsg (bfd_get_error ()));
}

/* write_gcore_file -- helper for gcore_command (exported).
   Compose and write the corefile data to the core file.  */

void
write_gcore_file_internal (bfd *obfd, struct compressed_core_file * pStream)
{
  target_prepare_to_generate_core ();
  SCOPE_EXIT { target_done_generating_core (); };
  write_gcore_file_1 (obfd, pStream);
}

void
write_gcore_file(bfd *obfd)
{
    write_gcore_file_internal(obfd, nullptr);
}

/* gcore_command -- implements the 'gcore' command.
   Generate a core file from the inferior process.  */

static void
gcore_command (const char *args, int from_tty)
{
  gdb::unique_xmalloc_ptr<char> corefilename;

  /* No use generating a corefile without a target process.  */
  if (!target_has_execution)
    noprocess ();

  if (args && *args)
    corefilename.reset (tilde_expand (args));
  else
    {
      /* Default corefile name is "core.PID".  */
      corefilename.reset (xstrprintf ("core.%d", inferior_ptid.pid ()));
    }

  if (info_verbose)
    fprintf_filtered (gdb_stdout,
		      "Opening corefile '%s' for output.\n",
		      corefilename.get ());

  struct compressed_core_file * pStream = NULL;
  size_t len = strlen(corefilename.get());
  if (len > 4 && corefilename.get()[len - 1] == 'r' && corefilename.get()[len - 2] == 'i' && corefilename.get()[len - 3] == 'd' && corefilename.get()[len - 4] == '.')
  {
      // Allocate stream object
      pStream = (struct compressed_core_file *)malloc(sizeof(compressed_core_file));
      if (pStream == NULL)
      {
          error(_("failed to allocate internal structure"));
          return;
      }
  }

  /* Open the output file.  */
  gdb_bfd_ref_ptr obfd (create_gcore_bfd1 (corefilename.get (), pStream));

  /* Arrange to unlink the file on failure.  */
  gdb::unlinker unlink_file (corefilename.get ());

  /* Call worker function.  */
  write_gcore_file_internal (obfd.get (), pStream);

  /* Succeeded.  */
  unlink_file.keep ();

  fprintf_filtered (gdb_stdout, "Saved corefile %s\n", corefilename.get ());
}

static unsigned long
default_gcore_mach (void)
{
#if 1	/* See if this even matters...  */
  return 0;
#else

  const struct bfd_arch_info *bfdarch = gdbarch_bfd_arch_info (target_gdbarch ());

  if (bfdarch != NULL)
    return bfdarch->mach;
  if (exec_bfd == NULL)
    error (_("Can't find default bfd machine type (need execfile)."));

  return bfd_get_mach (exec_bfd);
#endif /* 1 */
}

static enum bfd_architecture
default_gcore_arch (void)
{
  const struct bfd_arch_info *bfdarch = gdbarch_bfd_arch_info (target_gdbarch ());

  if (bfdarch != NULL)
    return bfdarch->arch;
  if (exec_bfd == NULL)
    error (_("Can't find bfd architecture for corefile (need execfile)."));

  return bfd_get_arch (exec_bfd);
}

static const char *
default_gcore_target (void)
{
  /* The gdbarch may define a target to use for core files.  */
  if (gdbarch_gcore_bfd_target_p (target_gdbarch ()))
    return gdbarch_gcore_bfd_target (target_gdbarch ());

  /* Otherwise, try to fall back to the exec_bfd target.  This will probably
     not work for non-ELF targets.  */
  if (exec_bfd == NULL)
    return NULL;
  else
    return bfd_get_target (exec_bfd);
}

/* Derive a reasonable stack segment by unwinding the target stack,
   and store its limits in *BOTTOM and *TOP.  Return non-zero if
   successful.  */

static int
derive_stack_segment (bfd_vma *bottom, bfd_vma *top)
{
  struct frame_info *fi, *tmp_fi;

  gdb_assert (bottom);
  gdb_assert (top);

  /* Can't succeed without stack and registers.  */
  if (!target_has_stack || !target_has_registers)
    return 0;

  /* Can't succeed without current frame.  */
  fi = get_current_frame ();
  if (fi == NULL)
    return 0;

  /* Save frame pointer of TOS frame.  */
  *top = get_frame_base (fi);
  /* If current stack pointer is more "inner", use that instead.  */
  if (gdbarch_inner_than (get_frame_arch (fi), get_frame_sp (fi), *top))
    *top = get_frame_sp (fi);

  /* Find prev-most frame.  */
  while ((tmp_fi = get_prev_frame (fi)) != NULL)
    fi = tmp_fi;

  /* Save frame pointer of prev-most frame.  */
  *bottom = get_frame_base (fi);

  /* Now canonicalize their order, so that BOTTOM is a lower address
     (as opposed to a lower stack frame).  */
  if (*bottom > *top)
    {
      bfd_vma tmp_vma;

      tmp_vma = *top;
      *top = *bottom;
      *bottom = tmp_vma;
    }

  return 1;
}

/* call_target_sbrk --
   helper function for derive_heap_segment.  */

static bfd_vma
call_target_sbrk (int sbrk_arg)
{
  struct objfile *sbrk_objf;
  struct gdbarch *gdbarch;
  bfd_vma top_of_heap;
  struct value *target_sbrk_arg;
  struct value *sbrk_fn, *ret;
  bfd_vma tmp;

  if (lookup_minimal_symbol ("sbrk", NULL, NULL).minsym != NULL)
    {
      sbrk_fn = find_function_in_inferior ("sbrk", &sbrk_objf);
      if (sbrk_fn == NULL)
	return (bfd_vma) 0;
    }
  else if (lookup_minimal_symbol ("_sbrk", NULL, NULL).minsym != NULL)
    {
      sbrk_fn = find_function_in_inferior ("_sbrk", &sbrk_objf);
      if (sbrk_fn == NULL)
	return (bfd_vma) 0;
    }
  else
    return (bfd_vma) 0;

  gdbarch = get_objfile_arch (sbrk_objf);
  target_sbrk_arg = value_from_longest (builtin_type (gdbarch)->builtin_int, 
					sbrk_arg);
  gdb_assert (target_sbrk_arg);
  ret = call_function_by_hand (sbrk_fn, NULL, target_sbrk_arg);
  if (ret == NULL)
    return (bfd_vma) 0;

  tmp = value_as_long (ret);
  if ((LONGEST) tmp <= 0 || (LONGEST) tmp == 0xffffffff)
    return (bfd_vma) 0;

  top_of_heap = tmp;
  return top_of_heap;
}

/* Derive a reasonable heap segment for ABFD by looking at sbrk and
   the static data sections.  Store its limits in *BOTTOM and *TOP.
   Return non-zero if successful.  */

static int
derive_heap_segment (bfd *abfd, bfd_vma *bottom, bfd_vma *top)
{
  bfd_vma top_of_data_memory = 0;
  bfd_vma top_of_heap = 0;
  bfd_size_type sec_size;
  bfd_vma sec_vaddr;
  asection *sec;

  gdb_assert (bottom);
  gdb_assert (top);

  /* This function depends on being able to call a function in the
     inferior.  */
  if (!target_has_execution)
    return 0;

  /* The following code assumes that the link map is arranged as
     follows (low to high addresses):

     ---------------------------------
     | text sections                 |
     ---------------------------------
     | data sections (including bss) |
     ---------------------------------
     | heap                          |
     --------------------------------- */

  for (sec = abfd->sections; sec; sec = sec->next)
    {
      if (bfd_section_flags (sec) & SEC_DATA
	  || strcmp (".bss", bfd_section_name (sec)) == 0)
	{
	  sec_vaddr = bfd_section_vma (sec);
	  sec_size = bfd_section_size (sec);
	  if (sec_vaddr + sec_size > top_of_data_memory)
	    top_of_data_memory = sec_vaddr + sec_size;
	}
    }

  top_of_heap = call_target_sbrk (0);
  if (top_of_heap == (bfd_vma) 0)
    return 0;

  /* Return results.  */
  if (top_of_heap > top_of_data_memory)
    {
      *bottom = top_of_data_memory;
      *top = top_of_heap;
      return 1;
    }

  /* No additional heap space needs to be saved.  */
  return 0;
}

static void
make_output_phdrs (bfd *obfd, asection *osec, void *ignored)
{
  int p_flags = 0;
  int p_type = 0;

  /* FIXME: these constants may only be applicable for ELF.  */
  if (startswith (bfd_section_name (osec), "load"))
    p_type = PT_LOAD;
  else if (startswith (bfd_section_name (osec), "note"))
    p_type = PT_NOTE;
  else
    p_type = PT_NULL;

  p_flags |= PF_R;	/* Segment is readable.  */
  if (!(bfd_section_flags (osec) & SEC_READONLY))
    p_flags |= PF_W;	/* Segment is writable.  */
  if (bfd_section_flags (osec) & SEC_CODE)
    p_flags |= PF_X;	/* Segment is executable.  */

  bfd_record_phdr (obfd, p_type, 1, p_flags, 0, 0, 0, 0, 1, &osec);
}

/* find_memory_region_ftype implementation.  DATA is 'bfd *' for the core file
   GDB is creating.  */

static int
gcore_create_callback (CORE_ADDR vaddr, unsigned long size, int read,
		       int write, int exec, int modified, void *data)
{
  bfd *obfd = (bfd *) data;
  asection *osec;
  flagword flags = SEC_ALLOC | SEC_HAS_CONTENTS | SEC_LOAD;

  /* If the memory segment has no permissions set, ignore it, otherwise
     when we later try to access it for read/write, we'll get an error
     or jam the kernel.  */
  if (read == 0 && write == 0 && exec == 0 && modified == 0)
    {
      if (info_verbose)
        {
          fprintf_filtered (gdb_stdout, "Ignore segment, %s bytes at %s\n",
                            plongest (size), paddress (target_gdbarch (), vaddr));
        }

      return 0;
    }

  if (write == 0 && modified == 0 && !solib_keep_data_in_core (vaddr, size))
    {
      /* See if this region of memory lies inside a known file on disk.
	 If so, we can avoid copying its contents by clearing SEC_LOAD.  */
      struct obj_section *objsec;

      for (objfile *objfile : current_program_space->objfiles ())
	ALL_OBJFILE_OSECTIONS (objfile, objsec)
	  {
	    bfd *abfd = objfile->obfd;
	    asection *asec = objsec->the_bfd_section;
	    bfd_vma align = (bfd_vma) 1 << bfd_section_alignment (asec);
	    bfd_vma start = obj_section_addr (objsec) & -align;
	    bfd_vma end = (obj_section_endaddr (objsec) + align - 1) & -align;

	    /* Match if either the entire memory region lies inside the
	       section (i.e. a mapping covering some pages of a large
	       segment) or the entire section lies inside the memory region
	       (i.e. a mapping covering multiple small sections).

	       This BFD was synthesized from reading target memory,
	       we don't want to omit that.  */
	    if (objfile->separate_debug_objfile_backlink == NULL
		&& ((vaddr >= start && vaddr + size <= end)
		    || (start >= vaddr && end <= vaddr + size))
		&& !(bfd_get_file_flags (abfd) & BFD_IN_MEMORY))
	      {
		flags &= ~(SEC_LOAD | SEC_HAS_CONTENTS);
		goto keep;	/* Break out of two nested for loops.  */
	      }
	  }

    keep:;
    }

  if (write == 0)
    flags |= SEC_READONLY;

  if (exec)
    flags |= SEC_CODE;
  else
    flags |= SEC_DATA;

  osec = bfd_make_section_anyway_with_flags (obfd, "load", flags);
  if (osec == NULL)
    {
      warning (_("Couldn't make gcore segment: %s"),
	       bfd_errmsg (bfd_get_error ()));
      return 1;
    }

  if (info_verbose)
    {
      fprintf_filtered (gdb_stdout, "Save segment, %s bytes at %s\n",
			plongest (size), paddress (target_gdbarch (), vaddr));
    }

  bfd_set_section_size (osec, size);
  bfd_set_section_vma (osec, vaddr);
  bfd_set_section_lma (osec, 0);
  return 0;
}

int
objfile_find_memory_regions (struct target_ops *self,
			     find_memory_region_ftype func, void *obfd)
{
  /* Use objfile data to create memory sections.  */
  struct obj_section *objsec;
  bfd_vma temp_bottom, temp_top;

  /* Call callback function for each objfile section.  */
  for (objfile *objfile : current_program_space->objfiles ())
    ALL_OBJFILE_OSECTIONS (objfile, objsec)
      {
	asection *isec = objsec->the_bfd_section;
	flagword flags = bfd_section_flags (isec);

	/* Separate debug info files are irrelevant for gcore.  */
	if (objfile->separate_debug_objfile_backlink != NULL)
	  continue;

	if ((flags & SEC_ALLOC) || (flags & SEC_LOAD))
	  {
	    int size = bfd_section_size (isec);
	    int ret;

	    ret = (*func) (obj_section_addr (objsec), size, 
			   1, /* All sections will be readable.  */
			   (flags & SEC_READONLY) == 0, /* Writable.  */
			   (flags & SEC_CODE) != 0, /* Executable.  */
			   1, /* MODIFIED is unknown, pass it as true.  */
			   obfd);
	    if (ret != 0)
	      return ret;
	  }
      }

  /* Make a stack segment.  */
  if (derive_stack_segment (&temp_bottom, &temp_top))
    (*func) (temp_bottom, temp_top - temp_bottom,
	     1, /* Stack section will be readable.  */
	     1, /* Stack section will be writable.  */
	     0, /* Stack section will not be executable.  */
	     1, /* Stack section will be modified.  */
	     obfd);

  /* Make a heap segment.  */
  if (derive_heap_segment (exec_bfd, &temp_bottom, &temp_top))
    (*func) (temp_bottom, temp_top - temp_bottom,
	     1, /* Heap section will be readable.  */
	     1, /* Heap section will be writable.  */
	     0, /* Heap section will not be executable.  */
	     1, /* Heap section will be modified.  */
	     obfd);

  return 0;
}

static void
gcore_copy_callback (bfd *obfd, asection *osec, void * stream)
{
  bfd_size_type size, total_size = bfd_section_size (osec);
  file_ptr offset = 0;

  /* Read-only sections are marked; we don't have to copy their contents.  */
  if ((bfd_section_flags (osec) & SEC_LOAD) == 0)
    return;

  /* Only interested in "load" sections.  */
  if (!startswith (bfd_section_name (osec), "load"))
    return;

  // Deliver to section queue without actually writing anything if we are writing to our private stream
  if (stream != NULL)
  {
      struct compressed_core_file * pStream = (struct compressed_core_file *)stream;

      struct pending_section_desc * pSec = (struct pending_section_desc *)malloc(sizeof(struct pending_section_desc));
      if (pSec == NULL)
      {
          error(_("gcore: failed to allocate sections memory for descriptor."));
          return;
      }

      pSec->next = NULL;
      pSec->osec = osec;

      if (pStream->last == NULL)
      {
          pStream->last = pStream->first = pSec;
      }
      else
      {
          pStream->last->next = pSec;
          pStream->last = pSec;
      }

      pStream->totalPendingBytes += total_size;

      return;
  }

  size = std::min (total_size, (bfd_size_type) MAX_COPY_BYTES);
  gdb::byte_vector memhunk (size);

  while (total_size > 0)
    {
      if (size > total_size)
	size = total_size;

      if (target_read_memory (bfd_section_vma (osec) + offset,
			      memhunk.data (), size) != 0)
	{
	  warning (_("Memory read failed for corefile "
		     "section, %s bytes at %s."),
		   plongest (size),
		   paddress (target_gdbarch (), bfd_section_vma (osec)));
	  break;
	}
      if (!bfd_set_section_contents (obfd, osec, memhunk.data (),
				     offset, size))
	{
	  warning (_("Failed to write corefile contents (%s)."),
		   bfd_errmsg (bfd_get_error ()));
	  break;
	}

      total_size -= size;
      offset += size;
    }
}

static int
gcore_memory_sections (bfd *obfd, struct compressed_core_file * pStream)
{
  /* Try gdbarch method first, then fall back to target method.  */
  if (!gdbarch_find_memory_regions_p (target_gdbarch ())
      || gdbarch_find_memory_regions (target_gdbarch (),
				      gcore_create_callback, obfd) != 0)
    {
      if (target_find_memory_regions (gcore_create_callback, obfd) != 0)
	return 0;			/* FIXME: error return/msg?  */
    }

  /* Record phdrs for section-to-segment mapping.  */
  bfd_map_over_sections (obfd, make_output_phdrs, pStream);

  /* Copy memory region contents.  */
  bfd_map_over_sections (obfd, gcore_copy_callback, pStream);

  if (pStream != NULL)
  { // Perform the actual writing here
      if (!fast_gcore_flush_segments(obfd, pStream))
      {
          return 0;
      }
  }

  return 1;
}

void
_initialize_gcore (void)
{
  add_com ("generate-core-file", class_files, gcore_command, _("\
Save a core file with the current state of the debugged process.\n\
Usage: generate-core-file [FILENAME]\n\
Argument is optional filename.  Default filename is 'core.PROCESS_ID'."));

  add_com_alias ("gcore", "generate-core-file", class_files, 1);
}
