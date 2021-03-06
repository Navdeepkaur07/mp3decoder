/*
 * Copyright (c) 2011 onwards by WeaveBytes InfoTech Pvt. Ltd.
 * 
 * Please reports bugs at weavebytes@gmail.com
 * 
 * This file may be distributed and/or modified under the terms of the 
 * GNU General Public License version 2 as published by the Free Software 
 * Foundation. (See COPYING.GPL for details.)
 * 
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * 
 */
#include "lame.h"

#include <wchar.h>
//#include <mbstring.h>

#include "app_defs.h"
#include "utils.h"
#include "app.h"
#include "fft.h"

//STATIC DATA ------------------------------------------------------------------------------------
static get_audio_global_data global;

/*average*/
double avg_jitter=0.0, avg_weak_note=0.0, avg_excess_note=0.0;
double avg_phi_rels_cnt=0.0, avg_oct_rels_cnt=0.0, avg_fourth_rels_cnt=0.0, avg_fifth_rels_cnt=0.0;

/* AIFF Definitions */
static int const IFF_ID_FORM = 0x464f524d; /* "FORM" */
static int const IFF_ID_AIFF = 0x41494646; /* "AIFF" */
static int const IFF_ID_AIFC = 0x41494643; /* "AIFC" */
static int const IFF_ID_COMM = 0x434f4d4d; /* "COMM" */
static int const IFF_ID_SSND = 0x53534e44; /* "SSND" */
static int const IFF_ID_MPEG = 0x4d504547; /* "MPEG" */

static int const IFF_ID_NONE = 0x4e4f4e45; /* "NONE" *//* AIFF-C data format */
static int const IFF_ID_2CBE = 0x74776f73; /* "twos" *//* AIFF-C data format */
static int const IFF_ID_2CLE = 0x736f7774; /* "sowt" *//* AIFF-C data format */

static int const WAV_ID_RIFF = 0x52494646; /* "RIFF" */
static int const WAV_ID_WAVE = 0x57415645; /* "WAVE" */
static int const WAV_ID_FMT = 0x666d7420; /* "fmt " */
static int const WAV_ID_DATA = 0x64617461; /* "data" */

#ifndef WAVE_FORMAT_PCM
static short const WAVE_FORMAT_PCM = 0x0001;
#endif
#ifndef WAVE_FORMAT_IEEE_FLOAT
static short const WAVE_FORMAT_IEEE_FLOAT = 0x0003;
#endif
#ifndef WAVE_FORMAT_EXTENSIBLE
static short const WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
#endif

//GLOBAL DATA ------------------------------------------------------------------------------------
ReaderConfig global_reader = { sf_unknown, 0, 0, 0 };
WriterConfig global_writer = { 0 };

UiConfig global_ui_config = {0,0,0,0};

DecoderConfig global_decoder;
RawPCMConfig global_raw_pcm = 
{ /* in_bitwidth */ 16
, /* in_signed   */ -1
, /* in_endian   */ ByteOrderLittleEndian
};

//FUNCTION ---------------------------------------------------------------------------------------
static  size_t min_size_t(size_t a, size_t b) {
    if (a < b) {
        return a;
    }
    return b;
}
/* Replacement for forward fseek(,,SEEK_CUR), because fseek() fails on pipes */
static int fskip(FILE * fp, long offset, int whence) {
#ifndef PIPE_BUF
    char    buffer[4096];
#else
    char    buffer[PIPE_BUF];
#endif

    /* S_ISFIFO macro is defined on newer Linuxes */
#ifndef S_ISFIFO
# ifdef _S_IFIFO
    /* _S_IFIFO is defined on Win32 and Cygwin */
#  define S_ISFIFO(m) (((m)&_S_IFIFO) == _S_IFIFO)
# endif
#endif

#ifdef S_ISFIFO
    /* fseek is known to fail on pipes with several C-Library implementations
workaround: 1) test for pipe
2) for pipes, only relatvie seeking is possible
3)            and only in forward direction!
else fallback to old code
     */
    {
        int const fd = fileno(fp);
        struct stat file_stat;

        if (fstat(fd, &file_stat) == 0) {
            if (S_ISFIFO(file_stat.st_mode)) {
                if (whence != SEEK_CUR || offset < 0) {
                    return -1;
                }
                while (offset > 0) {
                    size_t const bytes_to_skip = min_size_t(sizeof(buffer), offset);
                    size_t const read = fread(buffer, 1, bytes_to_skip, fp);
                    if (read < 1) {
                        return -1;
                    }
                    offset -= read;
                }
                return 0;
            }
        }
    }
#endif
    if (0 == fseek(fp, offset, whence)) { return 0; }

    if (whence != SEEK_CUR || offset < 0) {
        if (global_ui_config.silent < 10) {
            printf ("fskip problem: Mostly the return status of functions is not evaluate so it is more secure to polute <stderr>.\n");
        }
        return -1;
    }

    while (offset > 0) {
        size_t const bytes_to_skip = min_size_t(sizeof(buffer), offset);
        size_t const read = fread(buffer, 1, bytes_to_skip, fp);
        if (read < 1) {
            return -1;
        }
        offset -= read;
    }

    return 0;
}


static double read_ieee_extended_high_low(FILE * fp) {
    unsigned char bytes[10];
    memset(bytes, 0, 10);
    fread(bytes, 1, 10, fp);
    {
        int32_t const s = (bytes[0] & 0x80);
        int32_t const e_h = (bytes[0] & 0x7F);
        int32_t const e_l = bytes[1];
        int32_t e = (e_h << 8) | e_l;
        uint32_t const hm = uint32_high_low(bytes + 2);
        uint32_t const lm = uint32_high_low(bytes + 6);
        double  result = 0;
        if (e != 0 || hm != 0 || lm != 0) {
            if (e == 0x7fff) {
                result = HUGE_VAL;
            }
            else {
                double  mantissa_h = UNSIGNED_TO_FLOAT(hm);
                double  mantissa_l = UNSIGNED_TO_FLOAT(lm);
                e -= 0x3fff;
                e -= 31;
                result = ldexp(mantissa_h, e);
                e -= 32;
                result += ldexp(mantissa_l, e);
            }
        }
        return s ? -result : result;
    }
}

/************************************************************************
* aiff_check2
*
* PURPOSE:	Checks AIFF header information to make sure it is valid.
*	        returns 0 on success, 1 on errors
************************************************************************/
static int aiff_check2(IFF_AIFF * const pcm_aiff_data) {
    if (pcm_aiff_data->sampleType != (unsigned long) IFF_ID_SSND) {
        if (global_ui_config.silent < 10) {
            printf("ERROR: input sound data is not PCM\n");
        }
        return 1;
    }
    switch (pcm_aiff_data->sampleSize) {
    case 32:
    case 24:
    case 16:
    case 8:
        break;
    default:
        if (global_ui_config.silent < 10) {
            printf("ERROR: input sound data is not 8, 16, 24 or 32 bits\n");
        }
        return 1;
    }
    if (pcm_aiff_data->numChannels != 1 && pcm_aiff_data->numChannels != 2) {
        if (global_ui_config.silent < 10) {
            printf("ERROR: input sound data is not mono or stereo\n");
        }
        return 1;
    }
    if (pcm_aiff_data->blkAlgn.blockSize != 0) {
        if (global_ui_config.silent < 10) {
            printf("ERROR: block size of input sound data is not 0 bytes\n");
        }
        return 1;
    }
    /* A bug, since we correctly skip the offset earlier in the code.
       if (pcm_aiff_data->blkAlgn.offset != 0) {
       printf("Block offset is not 0 bytes in '%s'\n", file_name);
       return 1;
       } */

    return 0;
}

/*****************************************************************************
 *
 *	Read Audio Interchange File Format (AIFF) headers.
 *
 *	By the time we get here the first 32 bits of the file have already been
 *	read, and we're pretty sure that we're looking at an AIFF file.
 *
 *****************************************************************************/
static int parse_aiff_header(lame_global_flags * gfp, FILE * sf) {
    long    chunkSize = 0, subSize = 0, typeID = 0, dataType = IFF_ID_NONE;
    IFF_AIFF aiff_info;
    int     seen_comm_chunk = 0, seen_ssnd_chunk = 0;
    long    pcm_data_pos = -1;

    memset(&aiff_info, 0, sizeof(aiff_info));
    chunkSize = read_32_bits_high_low(sf);

    typeID = read_32_bits_high_low(sf);
    if ((typeID != IFF_ID_AIFF) && (typeID != IFF_ID_AIFC))
        return -1;

    while (chunkSize > 0) {
        long    ckSize;
        int     type = read_32_bits_high_low(sf);
        chunkSize -= 4;

        /* don't use a switch here to make it easier to use 'break' for SSND */
        if (type == IFF_ID_COMM) {
            seen_comm_chunk = seen_ssnd_chunk + 1;
            subSize = read_32_bits_high_low(sf);
            ckSize = make_even_number_of_bytes_in_length(subSize);
            chunkSize -= ckSize;

            aiff_info.numChannels = (short) read_16_bits_high_low(sf);
            ckSize -= 2;
            aiff_info.numSampleFrames = read_32_bits_high_low(sf);
            ckSize -= 4;
            aiff_info.sampleSize = (short) read_16_bits_high_low(sf);
            ckSize -= 2;
            aiff_info.sampleRate = read_ieee_extended_high_low(sf);
            ckSize -= 10;
            if (typeID == IFF_ID_AIFC) {
                dataType = read_32_bits_high_low(sf);
                ckSize -= 4;
            }
            if (fskip(sf, ckSize, SEEK_CUR) != 0)
                return -1;
        }
        else if (type == IFF_ID_SSND) {
            seen_ssnd_chunk = 1;
            subSize = read_32_bits_high_low(sf);
            ckSize = make_even_number_of_bytes_in_length(subSize);
            chunkSize -= ckSize;

            aiff_info.blkAlgn.offset = read_32_bits_high_low(sf);
            ckSize -= 4;
            aiff_info.blkAlgn.blockSize = read_32_bits_high_low(sf);
            ckSize -= 4;

            aiff_info.sampleType = IFF_ID_SSND;

            if (seen_comm_chunk > 0) {
                if (fskip(sf, (long) aiff_info.blkAlgn.offset, SEEK_CUR) != 0)
                    return -1;
                /* We've found the audio data. Read no further! */
                break;
            }
            pcm_data_pos = ftell(sf);
            if (pcm_data_pos >= 0) {
                pcm_data_pos += aiff_info.blkAlgn.offset;
            }
            if (fskip(sf, ckSize, SEEK_CUR) != 0)
                return -1;
        }
        else {
            subSize = read_32_bits_high_low(sf);
            ckSize = make_even_number_of_bytes_in_length(subSize);
            chunkSize -= ckSize;

            if (fskip(sf, ckSize, SEEK_CUR) != 0)
                return -1;
        }
    }
    if (dataType == IFF_ID_2CLE) {
        global. pcmswapbytes = global_reader.swapbytes;
    }
    else if (dataType == IFF_ID_2CBE) {
        global. pcmswapbytes = !global_reader.swapbytes;
    }
    else if (dataType == IFF_ID_NONE) {
        global. pcmswapbytes = !global_reader.swapbytes;
    }
    else {
        return -1;
    }

    /* DEBUGF("Parsed AIFF %d\n", is_aiff); */
    if (seen_comm_chunk && (seen_ssnd_chunk > 0 || aiff_info.numSampleFrames == 0)) {
        /* make sure the header is sane */
        if (0 != aiff_check2(&aiff_info))
            return 0;
        if (-1 == lame_set_num_channels(gfp, aiff_info.numChannels)) {
            if (global_ui_config.silent < 10) {
                printf("Unsupported number of channels: %u\n", aiff_info.numChannels);
            }
            return 0;
        }
        if (global_reader.input_samplerate == 0) {
            (void) lame_set_in_samplerate(gfp, (int) aiff_info.sampleRate);
        }
        else {
            (void) lame_set_in_samplerate(gfp, global_reader.input_samplerate);
        }
        (void) lame_set_num_samples(gfp, aiff_info.numSampleFrames);
        global. pcmbitwidth = aiff_info.sampleSize;
        global. pcm_is_unsigned_8bit = 0;
        global. pcm_is_ieee_float = 0; /* FIXME: possible ??? */
        if (pcm_data_pos >= 0) {
            if (fseek(sf, pcm_data_pos, SEEK_SET) != 0) {
                if (global_ui_config.silent < 10) {
                    printf("Can't rewind stream to audio data position\n");
                }
                return 0;
            }
        }
        return 1;
    }
    return -1;
}

/*****************************************************************************
 *
 *	Read Microsoft Wave headers
 *
 *	By the time we get here the first 32-bits of the file have already been
 *	read, and we're pretty sure that we're looking at a WAV file.
 *
 *****************************************************************************/
static int parse_wave_header(lame_global_flags * gfp, FILE * sf) {
    int     format_tag = 0;
    int     channels = 0;
    int     block_align = 0;
    int     bits_per_sample = 0;
    int     samples_per_sec = 0;
    int     avg_bytes_per_sec = 0;
    int     is_wav = 0;
    long    data_length = 0, file_length, subSize = 0;
    int     loop_sanity = 0;

    file_length = read_32_bits_high_low(sf);
    if (read_32_bits_high_low(sf) != WAV_ID_WAVE)
        return -1;

    for (loop_sanity = 0; loop_sanity < 20; ++loop_sanity) {
        int     type = read_32_bits_high_low(sf);

        if (type == WAV_ID_FMT) {
            subSize = read_32_bits_low_high(sf);
            subSize = make_even_number_of_bytes_in_length(subSize);
            if (subSize < 16) {
                return -1;
            }

            format_tag = read_16_bits_low_high(sf);
            subSize -= 2;
            channels = read_16_bits_low_high(sf);
            subSize -= 2;
            samples_per_sec = read_32_bits_low_high(sf);
            subSize -= 4;
            avg_bytes_per_sec = read_32_bits_low_high(sf);
            subSize -= 4;
            block_align = read_16_bits_low_high(sf);
            subSize -= 2;
            bits_per_sample = read_16_bits_low_high(sf);
            subSize -= 2;

            /* WAVE_FORMAT_EXTENSIBLE support */
            if ((subSize > 9) && (format_tag == WAVE_FORMAT_EXTENSIBLE)) {
                read_16_bits_low_high(sf); /* cbSize */
                read_16_bits_low_high(sf); /* ValidBitsPerSample */
                read_32_bits_low_high(sf); /* ChannelMask */
                /* SubType coincident with format_tag for PCM int or float */
                format_tag = read_16_bits_low_high(sf);
                subSize -= 10;
            }

            if (subSize > 0) {
                if (fskip(sf, (long) subSize, SEEK_CUR) != 0)
                    return -1;
            };

        }
        else if (type == WAV_ID_DATA) {
            subSize = read_32_bits_low_high(sf);
            data_length = subSize;
            is_wav = 1;
            /* We've found the audio data. Read no further! */
            break;

        }
        else {
            subSize = read_32_bits_low_high(sf);
            subSize = make_even_number_of_bytes_in_length(subSize);
            if (fskip(sf, (long) subSize, SEEK_CUR) != 0) {
                return -1;
            }
        }
    }
    if (is_wav) {
        if (format_tag != WAVE_FORMAT_PCM && format_tag != WAVE_FORMAT_IEEE_FLOAT) {
            if (global_ui_config.silent < 10) {
                printf("Unsupported data format: 0x%04X\n", format_tag);
            }
            return 0;   /* oh no! non-supported format  */
        }

        /* make sure the header is sane */
        if (-1 == lame_set_num_channels(gfp, channels)) {
            if (global_ui_config.silent < 10) {
                printf("Unsupported number of channels: %u\n", channels);
            }
            return 0;
        }
        if (global_reader.input_samplerate == 0) {
            (void) lame_set_in_samplerate(gfp, samples_per_sec);
        }
        else {
            (void) lame_set_in_samplerate(gfp, global_reader.input_samplerate);
        }
        global. pcmbitwidth = bits_per_sample;
        global. pcm_is_unsigned_8bit = 1;
        global. pcm_is_ieee_float = (format_tag == WAVE_FORMAT_IEEE_FLOAT ? 1 : 0);
        (void) lame_set_num_samples(gfp, data_length / (channels * ((bits_per_sample + 7) / 8)));
        return 1;
    }
    return -1;
}

static void setSkipStartAndEnd(lame_t gfp, int enc_delay, int enc_padding) {
    int     skip_start = 0, skip_end = 0;

    if (global_decoder.mp3_delay_set)
        skip_start = global_decoder.mp3_delay;

    switch (global_reader.input_format) {
        case sf_mp123:
            break;

        case sf_mp3:
            if (skip_start == 0) {
                if (enc_delay > -1 || enc_padding > -1) {
                    if (enc_delay > -1)
                        skip_start = enc_delay + 528 + 1;
                    if (enc_padding > -1)
                        skip_end = enc_padding - (528 + 1);
                }
                else
                    skip_start = lame_get_encoder_delay(gfp) + 528 + 1;
            }
            else {
                /* user specified a value of skip. just add for decoder */
                skip_start += 528 + 1; /* mp3 decoder has a 528 sample delay, plus user supplied "skip" */
            }
            break;
        case sf_mp2:
            skip_start += 240 + 1;
            break;
        case sf_mp1:
            skip_start += 240 + 1;
            break;
        case sf_raw:
            skip_start += 0; /* other formats have no delay *//* is += 0 not better ??? */
            break;
        case sf_wave:
            skip_start += 0; /* other formats have no delay *//* is += 0 not better ??? */
            break;
        case sf_aiff:
            skip_start += 0; /* other formats have no delay *//* is += 0 not better ??? */
            break;
        default:
            skip_start += 0; /* other formats have no delay *//* is += 0 not better ??? */
            break;
    }
    skip_start = skip_start < 0 ? 0 : skip_start;
    skip_end = skip_end < 0 ? 0 : skip_end;
    global. pcm16.skip_start = global.pcm32.skip_start = skip_start;
    global. pcm16.skip_end = global.pcm32.skip_end = skip_end;
}

/************************************************************************
*
* parse_file_header
*
* PURPOSE: Read the header from a bytestream.  Try to determine whether
*		   it's a WAV file or AIFF without rewinding, since rewind
*		   doesn't work on pipes and there's a good chance we're reading
*		   from stdin (otherwise we'd probably be using libsndfile).
*
* When this function returns, the file offset will be positioned at the
* beginning of the sound data.
*
************************************************************************/
static int parse_file_header(lame_global_flags * gfp, FILE * sf) {
    int     type = read_32_bits_high_low(sf);

    global. count_samples_carefully = 0;
    global. pcm_is_unsigned_8bit = global_raw_pcm.in_signed == 1 ? 0 : 1;
    /*global_reader.input_format = sf_raw; commented out, because it is better to fail
       here as to encode some hundreds of input files not supported by LAME
       If you know you have RAW PCM data, use the -r switch
     */

    if (type == WAV_ID_RIFF) {
        /* It's probably a WAV file */
        int const ret = parse_wave_header(gfp, sf);
        if (ret > 0) {
            global. count_samples_carefully = 1;
            return sf_wave;
        }
        if (ret < 0) {
            if (global_ui_config.silent < 10) {
                printf("Warning: corrupt or unsupported WAVE format\n");
            }
        }
    }
    else if (type == IFF_ID_FORM) {
        /* It's probably an AIFF file */
        int const ret = parse_aiff_header(gfp, sf);
        if (ret > 0) {
            global. count_samples_carefully = 1;
            return sf_aiff;
        }
        if (ret < 0) {
            if (global_ui_config.silent < 10) {
                printf("Warning: corrupt or unsupported AIFF format\n");
            }
        }
    }
    else {
        if (global_ui_config.silent < 10) {
            printf("Warning: unsupported audio format\n");
        }
    }
    return sf_unknown;
}

static int lame_set_stream_binary_mode(FILE * const fp) {
#if   defined __EMX__
    _fsetmode(fp, "b");
#elif defined __BORLANDC__
    setmode(_fileno(fp), O_BINARY);
#elif defined __CYGWIN__
    setmode(fileno(fp), _O_BINARY);
#elif defined _WIN32
    _setmode(_fileno(fp), _O_BINARY);
#else
    (void) fp;          /* doing nothing here, silencing the compiler only. */
#endif
    return 0;
}

#if defined( _WIN32 ) && !defined(__MINGW32__)
wchar_t*     utf8ToUnicode(const char *mbstr);
static wchar_t* mbsToUnicode(const char *mbstr, int code_page);

wchar_t *utf8ToUnicode(const char *mbstr) {
  return mbsToUnicode(mbstr, CP_UTF8);
}

static wchar_t *mbsToUnicode(const char *mbstr, int code_page) {
  int n = MultiByteToWideChar(code_page, 0, mbstr, -1, NULL, 0);
  wchar_t* wstr = malloc( n*sizeof(wstr[0]) );
  if ( wstr !=0 ) {
    n = MultiByteToWideChar(code_page, 0, mbstr, -1, wstr, n);
    if ( n==0 ) {
      free( wstr );
      wstr = 0;
    }
  }
  return wstr;
}
FILE* lame_fopen(char const* file, char const* mode) {
    FILE* fh = 0;
    wchar_t* wfile = utf8ToUnicode(file);
    wchar_t* wmode = utf8ToUnicode(mode);
    if (wfile != 0 && wmode != 0) {
        fh = _wfopen(wfile, wmode);
    }
    else {
        fh = fopen(file, mode);
    }
    free(wfile);
    free(wmode);
    return fh;
}
static char* lame_getenv(char const* var) {
    char* str = 0;
    wchar_t* wvar = utf8ToUnicode(var);
    wchar_t* wstr = 0;
    if (wvar != 0) {
        wstr = _wgetenv(wvar);
        str = unicodeToUtf8(wstr);
    }
    free(wvar);
    free(wstr);
    return str;
}

#else
static char* lame_getenv(char const* var) {
    char* str = getenv(var);
    if (str) {
        return strdup(str);
    }
    return 0;
}

FILE* lame_fopen(char const* file, char const* mode) {
    return fopen(file, mode);
}
#endif

static  off_t lame_get_file_size(FILE * fp) {
    struct stat sb;
    int     fd = fileno(fp);

    if (0 == fstat(fd, &sb))
        return sb.st_size;
    return (off_t) - 1;
}


static void initPcmBuffer(PcmBuffer * b, int w) {
    b->ch[0] = 0;
    b->ch[1] = 0;
    b->w = w;
    b->n = 0;
    b->u = 0;
    b->skip_start = 0;
    b->skip_end = 0;
}

static void freePcmBuffer(PcmBuffer * b) {
    if (b != 0) {
        free(b->ch[0]);
        free(b->ch[1]);
        b->ch[0] = 0;
        b->ch[1] = 0;
        b->n = 0;
        b->u = 0;
    }
}
static int close_input_file(FILE * musicin) {
    int     ret = 0;

    if (musicin != stdin && musicin != 0) { ret = fclose(musicin); }
    if (ret != 0) {
        if (global_ui_config.silent < 10) {
            printf("Could not close audio input file\n");
        }
    }
    return ret;
}
static FILE * open_mpeg_file(lame_t gfp, char const *inPath, int *enc_delay, int *enc_padding) {
    FILE   *musicin;

    /* set the defaults from info incase we cannot determine them from file */
    lame_set_num_samples(gfp, MAX_U_32_NUM);

    if (strcmp(inPath, "-") == 0) {
        musicin = stdin;
        lame_set_stream_binary_mode(musicin); /* Read from standard input. */
    }
    else {
        musicin = lame_fopen(inPath, "rb");
        if (musicin == NULL) {
            if (global_ui_config.silent < 10) {
                printf("[ERROR] :: Could not find \"%s\".\n", inPath);
            }
            return 0;
        }
    }
#ifdef AMIGA_MPEGA
    if (-1 == lame_decode_initfile(inPath, &global_decoder.mp3input_data)) {
        if (global_ui_config.silent < 10) {
            printf("Error reading headers in mp3 input file %s.\n", inPath);
        }
        close_input_file(musicin);
        return 0;
    }
#endif
#ifdef HAVE_MPGLIB
    printf("-- HAVE_MPGLIB --\n");

    if (-1 == lame_decode_initfile(musicin, &global_decoder.mp3input_data, enc_delay, enc_padding)) {
        if (global_ui_config.silent < 10) {
            printf("Error reading headers in mp3 input file %s.\n", inPath);
        }
        close_input_file(musicin);
        return 0;
    }
#endif
    if (-1 == lame_set_num_channels(gfp, global_decoder.mp3input_data.stereo)) {
        if (global_ui_config.silent < 10) {
            printf("Unsupported number of channels: %ud\n",
                         global_decoder.mp3input_data.stereo);
        }
        close_input_file(musicin);
        return 0;
    }
    if (global_reader.input_samplerate == 0) {
        (void) lame_set_in_samplerate(gfp, global_decoder.mp3input_data.samplerate);
    }
    else {
        (void) lame_set_in_samplerate(gfp, global_reader.input_samplerate);
    }
    (void) lame_set_num_samples(gfp, global_decoder.mp3input_data.nsamp);

    if (lame_get_num_samples(gfp) == MAX_U_32_NUM && musicin != stdin) {
        double  flen = lame_get_file_size(musicin); /* try to figure out num_samples */
        if (flen >= 0) {
            /* try file size, assume 2 bytes per sample */
            if (global_decoder.mp3input_data.bitrate > 0) {
                double  totalseconds =
                    (flen * 8.0 / (1000.0 * global_decoder.mp3input_data.bitrate));
                unsigned long tmp_num_samples =
                    (unsigned long) (totalseconds * lame_get_in_samplerate(gfp));

                (void) lame_set_num_samples(gfp, tmp_num_samples);
                global_decoder.mp3input_data.nsamp = tmp_num_samples;
            }
        }
    }
    return musicin;
}

static FILE * open_wave_file(lame_t gfp, char const *inPath) {
    FILE   *musicin;

    /* set the defaults from info incase we cannot determine them from file */
    lame_set_num_samples(gfp, MAX_U_32_NUM);

    if (!strcmp(inPath, "-")) {
        
        printf("open_wave_file read from stdin...\n");
        lame_set_stream_binary_mode(musicin = stdin); /* Read from standard input. */
    }
    else {
        if ((musicin = lame_fopen(inPath, "rb")) == NULL) {
            if (global_ui_config.silent < 10) {
                printf("Could not find \"%s\".\n", inPath);
            }
            exit(1);
        }
    }

    if (global_reader.input_format == sf_ogg) {
        if (global_ui_config.silent < 10) {
            printf("sorry, vorbis support in LAME is deprecated.\n");
        }
        exit(1);
    }
    else if (global_reader.input_format == sf_raw) {
        /* assume raw PCM */
        if (global_ui_config.silent < 9) {
            printf("Assuming raw pcm input file");
            if (global_reader.swapbytes)
                printf(" : Forcing byte-swapping\n");
            else
                printf("\n");
        }
        global. pcmswapbytes = global_reader.swapbytes;
    }
    else {
        global_reader.input_format = parse_file_header(gfp, musicin);
    }
    if (global_reader.input_format == sf_unknown) {
        printf("open_wave_file sf_unknown...\n");
        exit(1);
    }

    if (lame_get_num_samples(gfp) == MAX_U_32_NUM && musicin != stdin) {
        double const flen = lame_get_file_size(musicin); /* try to figure out num_samples */
        if (flen >= 0) {
            /* try file size, assume 2 bytes per sample */
            unsigned long fsize = (unsigned long) (flen / (2 * lame_get_num_channels(gfp)));
            (void) lame_set_num_samples(gfp, fsize);
        }
    }
    return musicin;
}
int is_mpeg_file_format(int input_file_format) {

    switch (input_file_format) {
    case sf_mp1:
        return 1;
    case sf_mp2:
        return 2;
    case sf_mp3:
        return 3;
    case sf_mp123:
        return -1;
    default:
        break;
    }
    return 0;
}
int init_infile(lame_t gfp, char const *inPath) {
    int     enc_delay = 0, enc_padding = 0;

    global. count_samples_carefully = 0;
    global. num_samples_read = 0;
    global. pcmbitwidth = global_raw_pcm.in_bitwidth;
    global. pcmswapbytes = global_reader.swapbytes;
    global. pcm_is_unsigned_8bit = global_raw_pcm.in_signed == 1 ? 0 : 1;
    global. pcm_is_ieee_float = 0;
    global. hip = 0;
    global. music_in = 0;
    global. snd_file = 0;
    global. in_id3v2_size = 0;
    global. in_id3v2_tag = 0;

    if (is_mpeg_file_format(global_reader.input_format)) {
        global. music_in = open_mpeg_file(gfp, inPath, &enc_delay, &enc_padding);
    }
    else {
#ifdef LIBSNDFILE
        if (strcmp(inPath, "-") != 0) { /* not for stdin */
            global. snd_file = open_snd_file(gfp, inPath);
        }
#endif
        if (global.snd_file == 0) {
            global. music_in = open_wave_file(gfp, inPath);
        }
    }
    initPcmBuffer(&global.pcm32, sizeof(int));
    initPcmBuffer(&global.pcm16, sizeof(short));
    setSkipStartAndEnd(gfp, enc_delay, enc_padding);
    {
        unsigned long n = lame_get_num_samples(gfp);
        if (n != MAX_U_32_NUM) {
            unsigned long const discard = global.pcm32.skip_start + global.pcm32.skip_end;
            lame_set_num_samples(gfp, n > discard ? n - discard : 0);
        }
    }
    return (global.snd_file != NULL || global.music_in != NULL) ? 1 : -1;
}

FILE* init_outfile(char const *outPath, int decode) {
    FILE   *outf;

    /* open the output file */
    if (0 == strcmp(outPath, "-")) {
        outf = stdout;
        lame_set_stream_binary_mode(outf);
    }
    else {
        outf = lame_fopen(outPath, "w+b");
#ifdef __riscos__
        /* Assign correct file type */
        if (outf != NULL) {
            char   *p, *out_path = strdup(outPath);
            for (p = out_path; *p; p++) { /* ugly, ugly to modify a string */
                switch (*p) {
                case '.':
                    *p = '/';
                    break;
                case '/':
                    *p = '.';
                    break;
                }
            }
            SetFiletype(out_path, decode ? 0xFB1 /*WAV*/ : 0x1AD /*AMPEG*/);
            free(out_path);
        }
#else
        (void) decode;
#endif
    }
    return outf;
}
static FILE* init_files(lame_global_flags * gf, char const *inPath, char const *outPath) {
    FILE   *outf;
    /* Mostly it is not useful to use the same input and output name.
       This test is very easy and buggy and don't recognize different names
       assigning the same file
     */
    if (0 != strcmp("-", outPath) && 0 == strcmp(inPath, outPath)) {
        printf("[ERROR] :: Input file and Output file are the same. Abort.\n");
        return NULL;
    }

    /* open the wav/aiff/raw pcm or mp3 input file.  This call will
     * open the file, try to parse the headers and
     * set gf.samplerate, gf.num_channels, gf.num_samples.
     * if you want to do your own file input, skip this call and set
     * samplerate, num_channels and num_samples yourself.
     */
    //----------------------------------------------//
    if (init_infile(gf, inPath) < 0) {
        printf("[ERROR] :: Can't init infile '%s'\n", inPath);
        return NULL;
    }
    if ((outf = init_outfile(outPath, lame_get_decode_only(gf))) == NULL) {
        printf("[ERROR] :: Can't init outfile '%s'\n", outPath);
        return NULL;
    }

    return outf;
}
static int samples_to_skip_at_start(void) {
    return global.pcm32.skip_start;
}
static int samples_to_skip_at_end(void) {
    return global.pcm32.skip_end;
}

/************************************************************************
*
* read_samples()
*
* PURPOSE:  reads the PCM samples from a file to the buffer
*
*  SEMANTICS:
* Reads #samples_read# number of shorts from #musicin# filepointer
* into #sample_buffer[]#.  Returns the number of samples read.
*
************************************************************************/
static int read_samples_pcm(FILE * musicin, int sample_buffer[2304], int samples_to_read) {
    int     samples_read;
    int     bytes_per_sample = global.pcmbitwidth / 8;
    int     swap_byte_order; /* byte order of input stream */

    switch (global.pcmbitwidth) {
    case 32:
    case 24:
    case 16:
        if (global_raw_pcm.in_signed == 0) {
            if (global_ui_config.silent < 10) {
                printf("Unsigned input only supported with bitwidth 8\n");
            }
            return -1;
        }
        swap_byte_order = (global_raw_pcm.in_endian != ByteOrderLittleEndian) ? 1 : 0;
        if (global.pcmswapbytes) {
            swap_byte_order = !swap_byte_order;
        }
        break;

    case 8:
        swap_byte_order = global.pcm_is_unsigned_8bit;
        break;

    default:
        if (global_ui_config.silent < 10) {
            printf("Only 8, 16, 24 and 32 bit input files supported \n");
        }
        return -1;
    }
    samples_read = unpack_read_samples(samples_to_read, bytes_per_sample, swap_byte_order,
                                       sample_buffer, musicin);
    if (ferror(musicin)) {
        if (global_ui_config.silent < 10) {
            printf("Error reading input file\n");
        }
        return -1;
    }

    return samples_read;
}

static int read_samples_mp3(lame_t gfp, FILE * musicin, short int mpg123pcm[2][1152]) {
    int     out;
#if defined(AMIGA_MPEGA)  ||  defined(HAVE_MPGLIB)
    int     samplerate;
    static const char type_name[] = "MP3 file";

    out = lame_decode_fromfile(musicin, mpg123pcm[0], mpg123pcm[1], &global_decoder.mp3input_data);
    /*
     * out < 0:  error, probably EOF
     * out = 0:  not possible with lame_decode_fromfile() ???
     * out > 0:  number of output samples
     */
    if (out < 0) {
        memset(mpg123pcm, 0, sizeof(**mpg123pcm) * 2 * 1152);
        return 0;
    }

    if (lame_get_num_channels(gfp) != global_decoder.mp3input_data.stereo) {
        if (global_ui_config.silent < 10) {
            printf("Error: number of channels has changed in %s - not supported\n", type_name);
        }
        out = -1;
    }
    samplerate = global_reader.input_samplerate;
    if (samplerate == 0) {
        samplerate = global_decoder.mp3input_data.samplerate;
    }
    if (lame_get_in_samplerate(gfp) != samplerate) {
        if (global_ui_config.silent < 10) {
            printf("Error: sample frequency has changed in %s - not supported\n", type_name);
        }
        out = -1;
    }
#else
    out = -1;
#endif
    return out;
}

static int WriteWaveHeader(FILE * const fp, int pcmbytes, int freq, int channels, int bits) {
    int     bytes = (bits + 7) / 8;

    if(fp == NULL) {
        printf("[ERROR]::WriteWaveHeader Got NULL fp\n");
    }
    /* quick and dirty, but documented */
    fwrite("RIFF", 1, 4, fp); /* label */
    write_32_bits_low_high(fp, pcmbytes + 44 - 8); /* length in bytes without header */
    fwrite("WAVEfmt ", 2, 4, fp); /* 2 labels */
    write_32_bits_low_high(fp, 2 + 2 + 4 + 4 + 2 + 2); /* length of PCM format declaration area */
    write_16_bits_low_high(fp, 1); /* is PCM? */
    write_16_bits_low_high(fp, channels); /* number of channels */
    write_32_bits_low_high(fp, freq); /* sample frequency in [Hz] */
    write_32_bits_low_high(fp, freq * channels * bytes); /* bytes per second */
    write_16_bits_low_high(fp, channels * bytes); /* bytes per sample time */
    write_16_bits_low_high(fp, bits); /* bits per sample */
    fwrite("data", 1, 4, fp); /* label */
    write_32_bits_low_high(fp, pcmbytes); /* length in bytes of raw PCM data */

    return ferror(fp) ? -1 : 0;
}

static unsigned long calcEndPadding(unsigned long samples, int pcm_samples_per_frame) {
    unsigned long end_padding;
    samples += 576;
    end_padding = pcm_samples_per_frame - (samples % pcm_samples_per_frame);
    if (end_padding < 576)
        end_padding += pcm_samples_per_frame;
    return end_padding;
}

static unsigned long calcNumBlocks(unsigned long samples, int pcm_samples_per_frame) {
    unsigned long end_padding;
    samples += 576;
    end_padding = pcm_samples_per_frame - (samples % pcm_samples_per_frame);
    if (end_padding < 576)
        end_padding += pcm_samples_per_frame;
    return (samples + end_padding) / pcm_samples_per_frame;
}

/************************************************************************
*
*  get_audio16 - behave as the original get_audio function, with a limited
*                16 bit per sample output
*
**********************************************************************************/
int get_audio16(lame_t gfp, short buffer[2][1152]) {
    int     used = 0, read = 0;
    do {
        read = get_audio_common(gfp, NULL, buffer);
        used = addPcmBuffer(&global.pcm16, buffer[0], buffer[1], read);
    } while (used <= 0 && read > 0);
    if (read < 0) {
        return read;
    }
    if (global_reader.swap_channel == 0)
        return takePcmBuffer(&global.pcm16, buffer[0], buffer[1], used, 1152);
    else
        return takePcmBuffer(&global.pcm16, buffer[1], buffer[0], used, 1152);
}

/************************************************************************
* get_audio_common - central functionality of get_audio*
*    in: gfp
*        buffer    output to the int buffer or 16-bit buffer
*   out: buffer    int output    (if buffer != NULL)
*        buffer16  16-bit output (if buffer == NULL) 
* returns: samples read
* note: either buffer or buffer16 must be allocated upon call
**********************************************************************************/
static int get_audio_common(lame_t gfp, int buffer[2][1152], short buffer16[2][1152]) {
    int     num_channels = lame_get_num_channels(gfp);
    int     insamp[2 * 1152];
    short   buf_tmp16[2][1152];
    int     samples_read;
    int     framesize;
    int     samples_to_read;
    unsigned int remaining, tmp_num_samples;
    int     i;
    int    *p;

    /* 
     * NOTE: LAME can now handle arbritray size input data packets,
     * so there is no reason to read the input data in chuncks of
     * size "framesize".  EXCEPT:  the LAME graphical frame analyzer 
     * will get out of sync if we read more than framesize worth of data.
     */
    samples_to_read = framesize = lame_get_framesize(gfp);
    assert(framesize <= 1152);

    /* get num_samples */
    if (is_mpeg_file_format(global_reader.input_format)) {
        tmp_num_samples = global_decoder.mp3input_data.nsamp;
    }
    else {
        tmp_num_samples = lame_get_num_samples(gfp);
    }

    /* if this flag has been set, then we are carefull to read
     * exactly num_samples and no more.  This is useful for .wav and .aiff
     * files which have id3 or other tags at the end.  Note that if you
     * are using LIBSNDFILE, this is not necessary 
     */
    if (global.count_samples_carefully) {
        if (global.num_samples_read < tmp_num_samples) {
            remaining = tmp_num_samples - global.num_samples_read;
        }
        else {
            remaining = 0;
        }
        if (remaining < (unsigned int) framesize && 0 != tmp_num_samples)
            /* in case the input is a FIFO (at least it's reproducible with
               a FIFO) tmp_num_samples may be 0 and therefore remaining
               would be 0, but we need to read some samples, so don't
               change samples_to_read to the wrong value in this case */
            samples_to_read = remaining;
    }

    if (is_mpeg_file_format(global_reader.input_format)) {
        if (buffer != NULL)
            samples_read = read_samples_mp3(gfp, global.music_in, buf_tmp16);
        else
            samples_read = read_samples_mp3(gfp, global.music_in, buffer16);
        if (samples_read < 0) {
            return samples_read;
        }
    }
    else {
        if (global.snd_file) {
#ifdef LIBSNDFILE
            samples_read = sf_read_int(global.snd_file, insamp, num_channels * samples_to_read);
#else
            samples_read = 0;
#endif
        }
        else {
            samples_read =
                read_samples_pcm(global.music_in, insamp, num_channels * samples_to_read);
        }
        if (samples_read < 0) {
            return samples_read;
        }
        p = insamp + samples_read;
        samples_read /= num_channels;
        if (buffer != NULL) { /* output to int buffer */
            if (num_channels == 2) {
                for (i = samples_read; --i >= 0;) {
                    buffer[1][i] = *--p;
                    buffer[0][i] = *--p;
                }
            }
            else if (num_channels == 1) {
                memset(buffer[1], 0, samples_read * sizeof(int));
                for (i = samples_read; --i >= 0;) {
                    buffer[0][i] = *--p;
                }
            }
            else
                assert(0);
        }
        else {          /* convert from int; output to 16-bit buffer */
            if (num_channels == 2) {
                for (i = samples_read; --i >= 0;) {
                    buffer16[1][i] = *--p >> (8 * sizeof(int) - 16);
                    buffer16[0][i] = *--p >> (8 * sizeof(int) - 16);
                }
            }
            else if (num_channels == 1) {
                memset(buffer16[1], 0, samples_read * sizeof(short));
                for (i = samples_read; --i >= 0;) {
                    buffer16[0][i] = *--p >> (8 * sizeof(int) - 16);
                }
            }
            else
                assert(0);
        }
    }

    /* LAME mp3 output 16bit -  convert to int, if necessary */
    if (is_mpeg_file_format(global_reader.input_format)) {
        if (buffer != NULL) {
            for (i = samples_read; --i >= 0;)
                buffer[0][i] = buf_tmp16[0][i] << (8 * sizeof(int) - 16);
            if (num_channels == 2) {
                for (i = samples_read; --i >= 0;)
                    buffer[1][i] = buf_tmp16[1][i] << (8 * sizeof(int) - 16);
            }
            else if (num_channels == 1) {
                memset(buffer[1], 0, samples_read * sizeof(int));
            }
            else
                assert(0);
        }
    }


    /* if num_samples = MAX_U_32_NUM, then it is considered infinitely long.
       Don't count the samples */
    if (tmp_num_samples != MAX_U_32_NUM)
        global. num_samples_read += samples_read;

    return samples_read;
}

static int addPcmBuffer(PcmBuffer * b, void *a0, void *a1, int read) {
    int     a_n;

    if (b == 0) {
        return 0;
    }
    if (read < 0) {
        return b->u - b->skip_end;
    }
    if (b->skip_start >= read) {
        b->skip_start -= read;
        return b->u - b->skip_end;
    }
    a_n = read - b->skip_start;

    if (b != 0 && a_n > 0) {
        int const b_free = b->n - b->u;
        int const a_need = b->w * a_n;
        int const b_used = b->w * b->u;
        if (b_free < a_need) {
            b->n += a_n;
            b->ch[0] = realloc(b->ch[0], b->w * b->n);
            b->ch[1] = realloc(b->ch[1], b->w * b->n);
        }
        b->u += a_n;
        if (b->ch[0] != 0 && a0 != 0) {
            char   *src = a0;
            memcpy((char *) b->ch[0] + b_used, src + b->skip_start, a_need);
        }
        if (b->ch[1] != 0 && a1 != 0) {
            char   *src = a1;
            memcpy((char *) b->ch[1] + b_used, src + b->skip_start, a_need);
        }
    }
    b->skip_start = 0;
    return b->u - b->skip_end;
}

static int takePcmBuffer(PcmBuffer * b, void *a0, void *a1, int a_n, int mm) {
    if (a_n > mm) {
        a_n = mm;
    }
    if (b != 0 && a_n > 0) {
        int const a_take = b->w * a_n;
        if (a0 != 0 && b->ch[0] != 0) {
            memcpy(a0, b->ch[0], a_take);
        }
        if (a1 != 0 && b->ch[1] != 0) {
            memcpy(a1, b->ch[1], a_take);
        }
        b->u -= a_n;
        if (b->u < 0) {
            b->u = 0;
            return a_n;
        }
        if (b->ch[0] != 0) {
            memmove(b->ch[0], (char *) b->ch[0] + a_take, b->w * b->u);
        }
        if (b->ch[1] != 0) {
            memmove(b->ch[1], (char *) b->ch[1] + a_take, b->w * b->u);
        }
    }
    return a_n;
}

/*********************************************************************************
*
* Decode Audio
* Get PCM Data
* Write PCM Data Into Output File
*
**********************************************************************************/
static int lame_decoder(lame_t gfp, FILE * outf, char *inPath, char *outPath) {
    short int Buffer[2][1152];
    int     i, iread;
    double  wavsize;
    int     tmp_num_channels = lame_get_num_channels(gfp);
    int     skip_start = samples_to_skip_at_start();
    int     skip_end = samples_to_skip_at_end();

    if (!(tmp_num_channels >= 1 && tmp_num_channels <= 2)) {
        printf("Internal error.  Aborting.");
        exit(-1);
    }

    if (0 == global_decoder.disable_wav_header) {
        WriteWaveHeader(outf, 0x7FFFFFFF, lame_get_in_samplerate(gfp), tmp_num_channels, 16);
    }

    wavsize = 0;
    do {
        iread = get_audio16(gfp, Buffer); /* read in 'iread' samples */
        if (iread >= 0) {
            wavsize += iread;
            put_audio16(outf, Buffer, iread, tmp_num_channels);
        }
    } while (iread > 0);


    i = (16 / 8) * tmp_num_channels;
    assert(i > 0);
    if (wavsize <= 0) {
        if (global_ui_config.silent < 10)
            printf("WAVE file contains 0 PCM samples\n");
        wavsize = 0;
    }
    else if (wavsize > 0xFFFFFFD0 / i) {
        if (global_ui_config.silent < 10)
            printf("Very huge WAVE file, can't set filesize accordingly\n");
        wavsize = 0xFFFFFFD0;
    }
    else {
        wavsize *= i;
    }
    /* if outf is seekable, rewind and adjust length */
    if (!global_decoder.disable_wav_header && strcmp("-", outPath)
        && !fseek(outf, 0l, SEEK_SET))
        WriteWaveHeader(outf, (int) wavsize, lame_get_in_samplerate(gfp), tmp_num_channels, 16);
    fclose(outf);
    close_infile();

    return 0;
}

static void close_infile(void) {
    close_input_file(global.music_in);
#ifdef LIBSNDFILE
    if (global.snd_file) {
        if (sf_close(global.snd_file) != 0) {
            if (global_ui_config.silent < 10) {
                printf("Could not close sound file \n");
            }
        }
        global. snd_file = 0;
    }
#endif
    freePcmBuffer(&global.pcm32);
    freePcmBuffer(&global.pcm16);
    global. music_in = 0;
    free(global.in_id3v2_tag);
    global.in_id3v2_tag = 0;
    global.in_id3v2_size = 0;
}

/*********************************************************************************
* unpack_read_samples - read and unpack signed low-to-high byte or unsigned
*                      single byte input. (used for read_samples function)
*                      Output integers are stored in the native byte order
*                      (little or big endian).  -jd
*  in: samples_to_read
*      bytes_per_sample
*      swap_order    - set for high-to-low byte order input stream
* i/o: pcm_in
* out: sample_buffer  (must be allocated up to samples_to_read upon call)
* returns: number of samples read
**********************************************************************************/
static int
unpack_read_samples(const int samples_to_read, const int bytes_per_sample,
                    const int swap_order, int *sample_buffer, FILE * pcm_in)
{
    size_t  samples_read;
    int     i;
    int    *op;              /* output pointer */
    unsigned char *ip = (unsigned char *) sample_buffer; /* input pointer */
    const int b = sizeof(int) * 8;

#define GA_URS_IFLOOP( ga_urs_bps ) \
    if( bytes_per_sample == ga_urs_bps ) \
      for( i = samples_read * bytes_per_sample; (i -= bytes_per_sample) >=0;)

    samples_read = fread(sample_buffer, bytes_per_sample, samples_to_read, pcm_in);
    op = sample_buffer + samples_read;

    if (swap_order == 0) {
        GA_URS_IFLOOP(1)
            * --op = ip[i] << (b - 8);
        GA_URS_IFLOOP(2)
            * --op = ip[i] << (b - 16) | ip[i + 1] << (b - 8);
        GA_URS_IFLOOP(3)
            * --op = ip[i] << (b - 24) | ip[i + 1] << (b - 16) | ip[i + 2] << (b - 8);
        GA_URS_IFLOOP(4)
            * --op =
            ip[i] << (b - 32) | ip[i + 1] << (b - 24) | ip[i + 2] << (b - 16) | ip[i + 3] << (b -
                                                                                              8);
    }
    else {
        GA_URS_IFLOOP(1)
            * --op = (ip[i] ^ 0x80) << (b - 8) | 0x7f << (b - 16); /* convert from unsigned */
        GA_URS_IFLOOP(2)
            * --op = ip[i] << (b - 8) | ip[i + 1] << (b - 16);
        GA_URS_IFLOOP(3)
            * --op = ip[i] << (b - 8) | ip[i + 1] << (b - 16) | ip[i + 2] << (b - 24);
        GA_URS_IFLOOP(4)
            * --op =
            ip[i] << (b - 8) | ip[i + 1] << (b - 16) | ip[i + 2] << (b - 24) | ip[i + 3] << (b -
                                                                                             32);
    }
#undef GA_URS_IFLOOP
    if (global.pcm_is_ieee_float) {
        ieee754_float32_t const m_max = INT_MAX;
        ieee754_float32_t const m_min = -(ieee754_float32_t) INT_MIN;
        ieee754_float32_t *x = (ieee754_float32_t *) sample_buffer;
        assert(sizeof(ieee754_float32_t) == sizeof(int));
        for (i = 0; i < samples_to_read; ++i) {
            ieee754_float32_t const u = x[i];
            int     v;
            if (u >= 1) {
                v = INT_MAX;
            }
            else if (u <= -1) {
                v = INT_MIN;
            }
            else if (u >= 0) {
                v = (int) (u * m_max + 0.5f);
            }
            else {
                v = (int) (u * m_min - 0.5f);
            }
            sample_buffer[i] = v;
        }
    }
    return (samples_read);
}


/*********************************************************************************
*
* Write PCM Buffers Into Output File
*
**********************************************************************************/
static void put_audio16(FILE * outf, short Buffer[2][1152], int iread, int nch) {
    char    data[2 * 1152 * 2];
    int     i, m = 0;

    if (global_decoder.disable_wav_header && global_reader.swapbytes) {
        if (nch == 1) {
            for (i = 0; i < iread; i++) {
                short   x = Buffer[0][i];
                /* write 16 Bits High Low */
                data[m++] = HIGH_BYTE(x);
                data[m++] = LOW__BYTE(x);
            }
        }
        else {
            for (i = 0; i < iread; i++) {
                short   x = Buffer[0][i], y = Buffer[1][i];
                /* write 16 Bits High Low */
                data[m++] = HIGH_BYTE(x);
                data[m++] = LOW__BYTE(x);
                /* write 16 Bits High Low */
                data[m++] = HIGH_BYTE(y);
                data[m++] = LOW__BYTE(y);
            }
        }
    }
    else {
        if (nch == 1) {
            for (i = 0; i < iread; i++) {
                short   x = Buffer[0][i];
                /* write 16 Bits Low High */
                data[m++] = LOW__BYTE(x);
                data[m++] = HIGH_BYTE(x);
            }
        }
        else {
            for (i = 0; i < iread; i++) {
                short   x = Buffer[0][i], y = Buffer[1][i];
                /* write 16 Bits Low High */
                data[m++] = LOW__BYTE(x);
                data[m++] = HIGH_BYTE(x);
                /* write 16 Bits Low High */
                data[m++] = LOW__BYTE(y);
                data[m++] = HIGH_BYTE(y);
            }
        }
    }
    if (m > 0) {
        /*.... writing PCM data into the file...*/
        fwrite(data, 1, m, outf);
        //processPCM(data);
        computeFft4Buf(data);
    }
    if (global_writer.flush_write == 1) {
        fflush(outf);
    }
}

/*
* Function to process raw PCM data.
*/
void processPCM(char data[2 * 1152 * 2]) {
    static int firstTime = 0;
    
    int i=0;
    int lmax=0, rmax=0;
    int lmin=0, rmin=0;

    if(firstTime > 5) return;
    firstTime++;


    while(i<1152) {
        short pcmLeft  = data[i] << 8 | data[i+1];
        short pcmRight = data[i+2] << 8 | data[i+3];
        printf("PCM L,R %d, %d\n", pcmLeft, pcmRight);
        
        /*calculate lmax, lmin*/
        if(lmax < pcmLeft) lmax = pcmLeft;
        if(lmin > pcmLeft) lmin = pcmLeft;

        /*calculate rmax, rmin*/
        if(rmax < pcmRight) rmax = pcmRight;
        if(rmin > pcmRight) rmin = pcmRight;

        i+=4;
    }
    printf("LMAX=%d, LMIN=%d\n", lmax, lmin);
    printf("RMAX=%d, RMIN=%d\n", rmax, rmin);
}

void computeFft4Buf(char data[2 * 1152 * 2]) {

    #define BUF_SIZE   2 * 1152 * 2

    double jitter=0.0, weak_note=0.0, excess_note=0.0, *min, *max;
    double phi_rels_cnt=0.0, oct_rels_cnt=0.0, fourth_rels_cnt=0.0, fifth_rels_cnt=0.0;
    double Fft_Buffer[2 * 1152 * 2];
    int i;
    /*
       fft fn needs double buffer and m,
       2^m = BUF_SIZE,
       BUF_SIZE = 2 * 1152 * 2
       so, m=12, approxmately, need to fix it later
     */
    int M = 12;//FIXME

    for(i=0; i<BUF_SIZE; i++) { Fft_Buffer[i] = (double)data[i]; }

    fft(Fft_Buffer, M);

    /*show the contents of buffer containing fft*/
    //for(i=0; i<BUF_SIZE; i++) { printf("%d %f\n", data[i], Fft_Buffer[i]); }

    /* FIXME : need to decide whether to use ,
       Formants(double *vfft, double *HzFrm, double *PwrFrm, double *phFrm, int n, int &nform)
       or not ?  */

    /*finding out jitter*/
    jitter = Jitter(Fft_Buffer, M);
    avg_jitter = (avg_jitter + jitter)/2;

    /*finding weak note*/
    weak_note = weakNote(Fft_Buffer, M);
    avg_weak_note = (avg_weak_note + weak_note)/2;

    /*min, max*/
    //MinMax(Fft_Buffer, BUF_SIZE, min, max);

    /*finding excess note*/
    excess_note = excessNote(Fft_Buffer, M);
    avg_excess_note = (avg_excess_note + excess_note)/2;

    /*finding Phi Rels Cnt*/
    phi_rels_cnt = CountPhiRels(Fft_Buffer, M);
    avg_phi_rels_cnt = (avg_phi_rels_cnt + phi_rels_cnt)/2;

    /*finding Oct Rels Cnt*/
    oct_rels_cnt = CountOctRels(Fft_Buffer, M);
    avg_oct_rels_cnt = (avg_oct_rels_cnt + oct_rels_cnt)/2;

    /*finding Fourth Rels Cnt*/
    fourth_rels_cnt = CountFourthRels(Fft_Buffer, M);
    avg_fourth_rels_cnt = (avg_fourth_rels_cnt + fourth_rels_cnt)/2;

    /*finding Fifth Rels Cnt*/
    fifth_rels_cnt = CountFourthRels(Fft_Buffer, M);
    avg_fifth_rels_cnt = (avg_fifth_rels_cnt + fifth_rels_cnt)/2;

    printf("------------------ Sound Analysis ----------------\n");
    printf("Jitter          : %f\n", jitter);
    printf("Weak Note       : %f\n", weak_note);
    printf("Excess Note     : %f\n", excess_note);
    printf("Phi Rels Cnt    : %f\n", phi_rels_cnt);
    printf("Oct Rels Cnt    : %f\n", oct_rels_cnt);
    //printf("Mix, Max        : %f, %f\n", *min, *max);
    printf("Fourth Rels Cnt : %f\n", fourth_rels_cnt);
    printf("Fifth Rels Cnt  : %f\n", fifth_rels_cnt);
    printf("--------------------------------------------------\n");

}

#if defined(HAVE_MPGLIB)
static int check_aid(const unsigned char *header) {
    return 0 == memcmp(header, "AiD\1", 4);
}

/*********************************************************************************
*
* Complete header analysis for a MPEG-1/2/2.5 Layer I, II or III data stream
*
**********************************************************************************/
static int is_syncword_mp123(const void *const headerptr) {
    const unsigned char *const p = headerptr;
    static const char abl2[16] = { 0, 7, 7, 7, 0, 7, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8 };

    if ((p[0] & 0xFF) != 0xFF)
        return 0;       /* first 8 bits must be '1' */
    if ((p[1] & 0xE0) != 0xE0)
        return 0;       /* next 3 bits are also */
    if ((p[1] & 0x18) == 0x08)
        return 0;       /* no MPEG-1, -2 or -2.5 */
    switch (p[1] & 0x06) {
    default:
    case 0x00:         /* illegal Layer */
        return 0;

    case 0x02:         /* Layer3 */
        if (global_reader.input_format != sf_mp3 && global_reader.input_format != sf_mp123) {
            return 0;
        }
        global_reader.input_format = sf_mp3;
        break;

    case 0x04:         /* Layer2 */
        if (global_reader.input_format != sf_mp2 && global_reader.input_format != sf_mp123) {
            return 0;
        }
        global_reader.input_format = sf_mp2;
        break;

    case 0x06:         /* Layer1 */
        if (global_reader.input_format != sf_mp1 && global_reader.input_format != sf_mp123) {
            return 0;
        }
        global_reader.input_format = sf_mp1;
        break;
    }
    if ((p[1] & 0x06) == 0x00)
        return 0;       /* no Layer I, II and III */
    if ((p[2] & 0xF0) == 0xF0)
        return 0;       /* bad bitrate */
    if ((p[2] & 0x0C) == 0x0C)
        return 0;       /* no sample frequency with (32,44.1,48)/(1,2,4)     */
    if ((p[1] & 0x18) == 0x18 && (p[1] & 0x06) == 0x04 && abl2[p[2] >> 4] & (1 << (p[3] >> 6)))
        return 0;
    if ((p[3] & 3) == 2)
        return 0;       /* reserved enphasis mode */
    return 1;
}

static size_t lenOfId3v2Tag(unsigned char const* buf) {
    unsigned int b0 = buf[0] & 127;
    unsigned int b1 = buf[1] & 127;
    unsigned int b2 = buf[2] & 127;
    unsigned int b3 = buf[3] & 127;
    return (((((b0 << 7) + b1) << 7) + b2) << 7) + b3;
}

/*********************************************************************************
*
*
**********************************************************************************/
int lame_decode_initfile(FILE * fd, mp3data_struct * mp3data, int *enc_delay, int *enc_padding) {
    /*  VBRTAGDATA pTagData; */
    /* int xing_header,len2,num_frames; */
    unsigned char    buf[100];
    int              ret;
    size_t           len;
    int              aid_header;
    short int        pcm_l[1152], pcm_r[1152];
    int              freeformat = 0;


    memset(mp3data, 0, sizeof(mp3data_struct));
    if (global.hip) {
        hip_decode_exit(global.hip);
    }
    global. hip = hip_decode_init();

    len = 4;
    if (fread(buf, 1, len, fd) != len)
        return -1;      /* failed */
    while (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        len = 6;
        if (fread(&buf[4], 1, len, fd) != len)
            return -1;  /* failed */
        len = lenOfId3v2Tag(&buf[6]);
        if (global.in_id3v2_size < 1) {
            global.in_id3v2_size = 10 + len;
            global.in_id3v2_tag = malloc(global.in_id3v2_size);
            if (global.in_id3v2_tag) {
                memcpy(global.in_id3v2_tag, buf, 10);
                if (fread(&global.in_id3v2_tag[10], 1, len, fd) != len)
                    return -1;  /* failed */
                len = 0; /* copied, nothing to skip */
            }
            else {
                global.in_id3v2_size = 0;
            }
        }
        fskip(fd, len, SEEK_CUR);
        len = 4;
        if (fread(&buf, 1, len, fd) != len)
            return -1;  /* failed */
    }
    aid_header = check_aid(buf);
    if (aid_header) {
        if (fread(&buf, 1, 2, fd) != 2)
            return -1;  /* failed */
        aid_header = (unsigned char) buf[0] + 256 * (unsigned char) buf[1];
        /* skip rest of AID, except for 6 bytes we have already read */
        fskip(fd, aid_header - 6, SEEK_CUR);

        /* read 4 more bytes to set up buffer for MP3 header check */
        if (fread(&buf, 1, len, fd) != len)
            return -1;  /* failed */
    }
    len = 4;
    while (!is_syncword_mp123(buf)) {
        unsigned int i;
        for (i = 0; i < len - 1; i++)
            buf[i] = buf[i + 1];
        if (fread(buf + len - 1, 1, 1, fd) != 1)
            return -1;  /* failed */
    }

    if ((buf[2] & 0xf0) == 0) {
        freeformat = 1;
    }
    /* now parse the current buffer looking for MP3 headers.    */
    /* (as of 11/00: mpglib modified so that for the first frame where  */
    /* headers are parsed, no data will be decoded.   */
    /* However, for freeformat, we need to decode an entire frame, */
    /* so mp3data->bitrate will be 0 until we have decoded the first */
    /* frame.  Cannot decode first frame here because we are not */
    /* yet prepared to handle the output. */
    ret = hip_decode1_headersB(global.hip, buf, len, pcm_l, pcm_r, mp3data, enc_delay, enc_padding);
    if (-1 == ret)
        return -1;

    /* repeat until we decode a valid mp3 header.  */
    while (!mp3data->header_parsed) {
        len = fread(buf, 1, sizeof(buf), fd);
        if (len != sizeof(buf)) return -1;
        ret = hip_decode1_headersB(global.hip, buf, len, pcm_l, pcm_r, mp3data, enc_delay, enc_padding);
        if (-1 == ret) return -1;
    }

    if (mp3data->bitrate == 0 && !freeformat) {
        if (global_ui_config.silent < 10) {
            printf("[WARNING] :: fail to sync...\n");
        }
        return lame_decode_initfile(fd, mp3data, enc_delay, enc_padding);
    }

    if (mp3data->totalframes > 0) {
        /* mpglib found a Xing VBR header and computed nsamp & totalframes */
    }
    else {
        /* set as unknown.  Later, we will take a guess based on file size
         * ant bitrate */
        mp3data->nsamp = MAX_U_32_NUM;
    }
    return 0;
}

/*********************************************************************************
* For lame_decode_fromfile:  return code
*  -1     error
*   n     number of samples output.  either 576 or 1152 depending on MP3 file.
*
*
* For lame_decode1_headers():  return code
*   -1     error
*    0     ok, but need more data before outputing any samples
*    n     number of samples output.  either 576 or 1152 depending on MP3 file.
**********************************************************************************/
static int lame_decode_fromfile(FILE * fd, short pcm_l[], short pcm_r[], mp3data_struct * mp3data) {
    int           ret = 0;
    size_t        len = 0;
    unsigned char buf[1024];

    /* first see if we still have data buffered in the decoder: */
    ret = hip_decode1_headers(global.hip, buf, len, pcm_l, pcm_r, mp3data);
    if (ret != 0)
        return ret;


    /* read until we get a valid output frame */
    for (;;) {
        len = fread(buf, 1, 1024, fd);
        if (len == 0) {
            /* we are done reading the file, but check for buffered data */
            ret = hip_decode1_headers(global.hip, buf, len, pcm_l, pcm_r, mp3data);
            if (ret <= 0) {
                hip_decode_exit(global.hip); /* release mp3decoder memory */
                global. hip = 0;
                return -1; /* done with file */
            }
            break;
        }

        ret = hip_decode1_headers(global.hip, buf, len, pcm_l, pcm_r, mp3data);
        if (ret == -1) {
            hip_decode_exit(global.hip); /* release mp3decoder memory */
            global. hip = 0;
            return -1;
        }
        if (ret > 0)
            break;
    }
    return ret;
}
#endif /* defined(HAVE_MPGLIB) */

/*********************************************************************************
*
*    MAIN 
*
**********************************************************************************/
int main(int argc, char *argv[]) {
    lame_t  gf;
    int     ret=0;
    char    inPath[PATH_MAX + 1];
    char    outPath[PATH_MAX + 1];
    FILE   *outf;

    gf = lame_init(); /* initialize libmp3lame */
    if (!gf) {
        printf("[ERROR] :: failed to initialize lame\n");
        exit(1);
    }

    if(argc<3) {
        printf("USAGE:-\n%s <input-mp3-file> <out-wav-file>\n", argv[0]);
        exit(1);
    }
    printf("[INFO] :: Initialized Lame...\n");


    /*set input format to mp3*/
    global_reader.input_format = sf_mp3;

    outf = init_files(gf, argv[1], argv[2]);
    if (outf == NULL) { return -1; }

    /* turn off automatic writing of ID3 tag data into mp3 stream 
     * we have to call it before 'lame_init_params', because that
     * function would spit out ID3v2 tag data.
     */
    lame_set_write_id3tag_automatic(gf, 0);

    /* Now that all the options are set, lame needs to analyze them and
     * set some more internal options and check for problems
     */
    ret = lame_init_params(gf);
    if (ret < 0) {
        printf("[ERROR]:: fatal error during initialization\n");
        return ret;
    }

    if (global_ui_config.silent > 0) {
        global_ui_config.brhist = 0; /* turn off VBR histogram */
    }

    /*decode an mp3 to wav*/
    ret = lame_decoder(gf, outf, inPath, outPath);

    /*cleanup*/
    lame_close(gf);
    printf("------------------ Average Sound Analysis ----------------\n");
    printf("Average Jitter          : %f\n", avg_jitter);
    printf("Average Weak Note       : %f\n", avg_weak_note);
    printf("Average Excess Note     : %f\n", avg_excess_note);
    printf("Average Phi Rels Cnt    : %f\n", avg_phi_rels_cnt);
    printf("Average Oct Rels Cnt    : %f\n", avg_oct_rels_cnt);
    printf("Average Fourth Rels Cnt : %f\n", avg_fourth_rels_cnt);
    printf("Average Fifth Rels Cnt  : %f\n", avg_fifth_rels_cnt);
    printf("--------------------------------------------------\n");
    printf("Decoding finished !!!\n\n");
    return ret;
}
