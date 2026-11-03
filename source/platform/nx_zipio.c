/* nx_zipio.c -- how the engine reads pk3 files from the SD card.
 *
 * Opening a pk3 walks its central directory, twice, seeking before each of its
 * thousands of entries, and a seek through stdio throws its buffer away. On a
 * card, that is a read per entry and many seconds before the title screen. This
 * read-ahead keeps its window across seeks, so the walk takes a handful of reads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zlib-1.2.11/ioapi.h"

#define WINDOW_SIZE	(64 * 1024)

typedef struct
{
	FILE		*file;
	unsigned char	*window;	/* allocated on the first read */
	ZPOS64_T	window_start;	/* where the window is in the file */
	size_t		window_length;
	ZPOS64_T	file_pos;	/* where the FILE is */
	ZPOS64_T	pos;		/* where the reader is */
	ZPOS64_T	size;
	int		error;
} nxZipFile_t;

static voidpf ZCALLBACK ZipOpen (voidpf opaque, const void *filename, int mode)
{
	nxZipFile_t	*z;
	FILE		*file;

	(void) opaque;
	if (!filename || (mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) != ZLIB_FILEFUNC_MODE_READ)
		return NULL;

	file = fopen((const char *) filename, "rb");
	if (!file)
		return NULL;

	z = calloc(1, sizeof(*z));
	if (!z)
	{
		fclose(file);
		return NULL;
	}
	z->file = file;
	setvbuf(file, NULL, _IONBF, 0);

	fseeko(file, 0, SEEK_END);
	z->size = ftello(file);
	fseeko(file, 0, SEEK_SET);
	return z;
}

/* Reads from where the FILE is asked to be, moving it only if it is elsewhere. */
static size_t ReadFile (nxZipFile_t *z, ZPOS64_T at, void *buf, size_t size)
{
	size_t	got;

	if (z->file_pos != at)
	{
		if (fseeko(z->file, at, SEEK_SET) != 0)
		{
			z->error = 1;
			return 0;
		}
		z->file_pos = at;
	}
	got = fread(buf, 1, size, z->file);
	z->file_pos += got;
	if (got < size && ferror(z->file))
		z->error = 1;
	return got;
}

static uLong ZCALLBACK ZipRead (voidpf opaque, voidpf stream, void *buf, uLong size)
{
	nxZipFile_t	*z = stream;
	unsigned char	*out = buf;
	uLong		left = size;

	(void) opaque;
	while (left > 0)
	{
		if (z->window && z->pos >= z->window_start && z->pos < z->window_start + z->window_length)
		{
			const size_t	offset = (size_t) (z->pos - z->window_start);
			const size_t	avail = z->window_length - offset;
			const size_t	take = left < avail ? left : avail;

			memcpy(out, z->window + offset, take);
			out += take;
			z->pos += take;
			left -= take;
			continue;
		}

		/* what is larger than the window goes straight to the reader */
		if (left >= WINDOW_SIZE)
		{
			const size_t	got = ReadFile(z, z->pos, out, left);

			out += got;
			z->pos += got;
			left -= got;
			break;
		}

		if (!z->window)
		{
			z->window = malloc(WINDOW_SIZE);
			if (!z->window)
			{
				z->error = 1;
				break;
			}
		}
		z->window_start = z->pos;
		z->window_length = ReadFile(z, z->pos, z->window, WINDOW_SIZE);
		if (z->window_length == 0)
			break;
	}
	return size - left;
}

static uLong ZCALLBACK ZipWrite (voidpf opaque, voidpf stream, const void *buf, uLong size)
{
	(void) opaque;
	(void) stream;
	(void) buf;
	(void) size;
	return 0;
}

static ZPOS64_T ZCALLBACK ZipTell (voidpf opaque, voidpf stream)
{
	(void) opaque;
	return ((nxZipFile_t *) stream)->pos;
}

static long ZCALLBACK ZipSeek (voidpf opaque, voidpf stream, ZPOS64_T offset, int origin)
{
	nxZipFile_t	*z = stream;

	(void) opaque;
	switch (origin)
	{
	case ZLIB_FILEFUNC_SEEK_SET:	z->pos = offset; break;
	case ZLIB_FILEFUNC_SEEK_CUR:	z->pos += offset; break;
	case ZLIB_FILEFUNC_SEEK_END:	z->pos = z->size + offset; break;
	default:			return -1;
	}
	return 0;
}

static int ZCALLBACK ZipClose (voidpf opaque, voidpf stream)
{
	nxZipFile_t	*z = stream;
	const int	ret = fclose(z->file);

	(void) opaque;
	free(z->window);
	free(z);
	return ret;
}

static int ZCALLBACK ZipError (voidpf opaque, voidpf stream)
{
	(void) opaque;
	return ((nxZipFile_t *) stream)->error;
}

void NX_FillZipFileFuncs (zlib_filefunc64_def *def)
{
	def->zopen64_file = ZipOpen;
	def->zread_file = ZipRead;
	def->zwrite_file = ZipWrite;
	def->ztell64_file = ZipTell;
	def->zseek64_file = ZipSeek;
	def->zclose_file = ZipClose;
	def->zerror_file = ZipError;
	def->opaque = NULL;
}
