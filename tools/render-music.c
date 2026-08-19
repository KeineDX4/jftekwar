/*
render-music.c - extract the General MIDI songs from the TekWar SONGS file
into standalone Standard MIDI (SMF) files.

Usage:  render-music <game-data-dir> [out-dir]

Reads <game-data-dir>/SONGS, walks the song index table stored at the END of
the file (1024 little-endian int32s, three per song entry: block offset
(scaled by 4096), byte length, unused) and, for every GM song entry (7
levels x 3 tracks + menu + subway menu = 27 entries), reads the HMI/NDMF
chunk, converts it to SMF in place with transmutehmp() (copied verbatim
from jftekwar/src/teksnd.c, Jonathon Fowler, 2023, public domain) and
writes <out-dir>/music/song_%03d.mid.

The SMF files are then rendered to OGG by tools/render-music.sh (see
doc/music.txt for the full pipeline).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAXBASESONGLENGTH 44136
#define NUMINDEXINTS      1024      /* int32s in the tail index table */

/* Song entries to extract: the GM version of every context (the FM and
   AWE32 variants are skipped).  The game plays BASESONG=0 only, i.e. entry
   9*level+6 for the levels and, in menusong(), the third entry of each
   menu group (index 63/66 then +2): entry 65 for the menu, 68 for the
   subway.  All three GM entries of every group are rendered anyway so the
   naming is complete and robust; entries 69/70 are zero-length and are
   left out. */
static const int targets[] = {
      6,  7,  8,
     15, 16, 17,
     24, 25, 26,
     33, 34, 35,
     42, 43, 44,
     51, 52, 53,
     60, 61, 62,
     63, 64, 65,
     66, 67, 68,
     -1
};

/* Byte-order helpers.  NDMF is little-endian, SMF is big-endian; the B_*
   macros are value-based, exactly like the ones jfbuild's baselayer.h
   provides to teksnd.c (a no-op swap on little-endian hosts). */
#if defined(__GNUC__) || defined(__clang__)
#define B_SWAP16(x) __builtin_bswap16(x)
#define B_SWAP32(x) __builtin_bswap32(x)
#else
static inline uint32_t B_SWAP32(uint32_t x)
{
     return ((x & 0xffU) << 24) | ((x & 0xff00U) << 8) |
            ((x & 0xff0000U) >> 8) | ((x & 0xff000000U) >> 24);
}
static inline uint16_t B_SWAP16(uint16_t x)
{
     return (uint16_t)(((x & 0xffU) << 8) | ((x & 0xff00U) >> 8));
}
#endif
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define B_LITTLE32(x) B_SWAP32((uint32_t)(x))
#define B_BIG32(x)    (uint32_t)(x)
#define B_BIG16(x)    (uint16_t)(x)
#else
#define B_LITTLE32(x) (uint32_t)(x)
#define B_BIG32(x)    B_SWAP32((uint32_t)(x))
#define B_BIG16(x)    B_SWAP16((uint16_t)(x))
#endif

/* --- NDMF/SMF structures + transmutehmp, verbatim from jftekwar/src/teksnd.c --- */

#if defined(__GNUC__) || defined(__clang__)
#define PACKED_STRUCT struct __attribute__ ((packed))
#elif defined(_MSC_VER)
#define PACKED_STRUCT struct
#pragma pack(1)
#else
#define PACKED_STRUCT struct
#endif

PACKED_STRUCT ndmfheader {
     uint8_t ident[32];       /* "HMIMIDIP013195" \0... */
     uint32_t branchofs;      /* File offset to the branch table at the end */
     uint32_t pad[3];
     uint32_t numtracks;
     uint32_t ticksperqunote;
     uint32_t tempo;          /* Game clock dependent */
     uint32_t playtime;       /* Song length in seconds */
     uint32_t channelprio[16];
     uint32_t trackmap[32][5];
     uint8_t  ctrlrestore[128];
     uint32_t pad2[2];
};

PACKED_STRUCT ndmftrackheader {
     uint32_t tracknum;
     uint32_t tracklen;       /* Header length inclusive */
     uint32_t channel;
     uint8_t data[];          /* [tracklen-12] */
};

PACKED_STRUCT smfheader {
     uint8_t ident[4];        /* "MThd" */
     uint32_t headsize;       /* 6 */
     uint16_t format;
     uint16_t numtracks;
     uint16_t ticksperqunote;
};
PACKED_STRUCT smftrackheader {
     uint8_t ident[4];        /* "MTrk" */
     uint32_t tracklen;       /* Header length exclusive */
     uint8_t data[];          /* [tracklen] */
};

/*
Brutal in-place transformation of a SOS HMP file (a.k.a. NDMF according to sosm.h)
into Standard MIDI, mutating global looping controllers into Apogee EMIDI equivalents.

By Jonathon Fowler, 2023
Provided to the public domain given how dubiously licensed the sources of information
going into this were. Realised through a combination of:
  - SOS.H in the Witchaven code dump exposing the file structure
  - http://www.r-t-c-m.com/knowledge-base/downloads-rtcm/tekwar-tools/sos40.zip
    providing the SOS special MIDI controller descriptions
  - A crucial hint about variable length encoding byte order at
    https://github.com/Mindwerks/wildmidi/blob/master/docs/formats/HmpFileFormat.txt#L84-L96

Overall:
    struct ndmfheader header;
    struct ndmftracks tracks[header.numtracks];
    uint8_t branchtable[];

    NDMF is little-endian, MIDI is big-endian.
    NDMF variable-length encoding: 0aaaaaaa 0bbbbbbb 1ccccccc
    MIDI variable-length encoding: 1ccccccc 1bbbbbbb 0aaaaaaa

Transformation can happen in-place because NDMF has a massive header compared to MIDI,
so every write will be happening onto ground already trodden. Strict aliasing be damned.
*/

static int transmutehmp(char *filedata)
{
     const char ndmfident[] = "HMIMIDIP013195";
     const int commandlengths[8] = { 2, 2, 2, 2, 1, 1, 2, -1 };
     const int syscomlengths[16] =  { -1, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1 };

     if (memcmp(ndmfident, filedata, sizeof ndmfident)) return -1;

     // Extract the important values from the NDMF header.
     struct ndmfheader *ndmfhead = (struct ndmfheader *)filedata;
     int numtracks = B_LITTLE32(ndmfhead->numtracks);
     int ticksperqunote = B_LITTLE32(ndmfhead->ticksperqunote);
     int tempo = 120000000 / B_LITTLE32(ndmfhead->tempo);
     ndmfhead = NULL;

     // Construct a new MIDI header.
     struct smfheader *smfhead = (struct smfheader *)filedata;
     memcpy(smfhead->ident, "MThd", 4);
     smfhead->headsize = B_BIG32(6);
     smfhead->format = B_BIG16(1);
     smfhead->numtracks = B_BIG16(numtracks);
     smfhead->ticksperqunote = B_BIG16(ticksperqunote);

     // Transcribe tracks.
     int ndmffileofs = sizeof(struct ndmfheader);
     int smffileofs = sizeof(struct smfheader);
     for (int trk = 0; trk < numtracks; trk++) {
          struct ndmftrackheader *ndmftrack = (struct ndmftrackheader *)&filedata[ndmffileofs];
          struct smftrackheader *smftrack = (struct smftrackheader *)&filedata[smffileofs];

          int ndmfdatalen = B_LITTLE32(ndmftrack->tracklen) - 12;
          int smfdatalen = ndmfdatalen;
          if (trk == 0) {
               // We need to add a tempo event to the first MIDI track.
               smfdatalen += 7;
          }

          memcpy(smftrack->ident, "MTrk", 4);
          smftrack->tracklen = B_BIG32(smfdatalen);

          uint8_t *ndmfdata = (uint8_t *)&ndmftrack->data[0];
          uint8_t *smfdata = (uint8_t *)&smftrack->data[0];

          if (trk == 0) {
               // Insert a tempo event.
               *(smfdata++) = 0;
               *(smfdata++) = 0xff;
               *(smfdata++) = 0x51;
               *(smfdata++) = 3;
               *(smfdata++) = (tempo>>16)&0xff;
               *(smfdata++) = (tempo>>8)&0xff;
               *(smfdata++) = tempo&0xff;
          }

          // Process events.
          uint8_t status = 0;
          for (int i = 0; i < ndmfdatalen; ) {
               uint8_t b;
               int copylen = 0;

               // Re-encode the offset.
               uint8_t vlenbytes[4], vlencnt = 0;
               do {
                    b = ndmfdata[i++];
                    vlenbytes[vlencnt++] = b & 0x7f;
               } while (!(b & 0x80));
               do {
                    b = vlenbytes[--vlencnt];
                    if (vlencnt) b |= 0x80;
                    *(smfdata++) = b;
               } while (vlencnt > 0);

               b = ndmfdata[i];
               if (b&0x80) {
                    // A new status byte.
                    *(smfdata++) = b;
                    i++;

                    status = b;    // Keep for running status.
                    copylen = commandlengths[(status & 0x7f)>>4];

                    if ((b&0xf0) == 0xf0) {
                         switch (b&0x0f) {
                              case 0x0: // Sysex.
                                   do *(smfdata++) = (b = ndmfdata[i++]);
                                   while (!(b&0x80) && b != 0xf7);
                                   break;
                              case 0xf: // Meta.
                                   *(smfdata++) = ndmfdata[i++];  // Type.
                                   copylen = (*(smfdata++) = ndmfdata[i++]);     // Length.
                                   break;
                              default:  // Sys common.
                                   copylen = syscomlengths[b&0x0f];
                                   break;
                         }
                    } else if ((b&0xf0) == 0xb0) {     // Controller change.
                         // SOS/EMIDI custom controller range. For whatever reason SOS
                         // controller values have their high bit set.
                         if (ndmfdata[i] >= 102 && ndmfdata[i] <= 119) {
                              if (trk == 1 && ndmfdata[i] == 110) {        // Global loop start
                                   *(smfdata++) = 118;
                                   *(smfdata++) = (ndmfdata[i+1] & 0x7f);
                              } else if (trk == 1 && ndmfdata[i] == 111) { // Global loop end.
                                   *(smfdata++) = 119;
                                   *(smfdata++) = 127;
                              } else {
                                   *(smfdata++) = 102; // Neuter all other controllers.
                                   *(smfdata++) = 0;
                              }
                              i += 2;
                              copylen = 0;
                         }
                    }
               } else {
                    copylen = commandlengths[(status & 0x7f)>>4];
               }

               for (; copylen>0; copylen--) {     // Copy data bytes.
                    *(smfdata++) = ndmfdata[i++];
               }
          }

          ndmffileofs += ndmfdatalen + 12;
          smffileofs += smfdatalen + 8;
     }

     return smffileofs;
}

static void usage(void)
{
     fprintf(stderr,
          "usage: render-music <game-data-dir> [out-dir]\n"
          "  Reads <game-data-dir>/SONGS and writes the GM songs as\n"
          "  <out-dir>/music/song_XXX.mid (out-dir defaults to game-data-dir).\n");
     exit(1);
}

int main(int argc, char **argv)
{
     const char *datadir, *outdir;
     char path[1024], outpath[1024];
     FILE *f;
     long flen, off, len;
     int32_t index[NUMINDEXINTS];
     uint8_t *filedata;
     int i, e, converted = 0, skipped = 0;

     if (argc < 2) usage();
     datadir = argv[1];
     outdir = (argc >= 3) ? argv[2] : datadir;

     snprintf(path, sizeof path, "%s/SONGS", datadir);
     f = fopen(path, "rb");
     if (!f) {
          fprintf(stderr, "render-music: can't open %s\n", path);
          return 1;
     }
     fseek(f, 0, SEEK_END);
     flen = ftell(f);
     fseek(f, 0, SEEK_SET);
     if (flen < NUMINDEXINTS*4L) {
          fprintf(stderr, "render-music: %s too small (%ld bytes)\n", path, flen);
          fclose(f);
          return 1;
     }
     filedata = malloc(flen);
     if (!filedata) { fclose(f); return 1; }
     if (fread(filedata, 1, flen, f) != (size_t)flen) {
          fprintf(stderr, "render-music: short read on %s\n", path);
          fclose(f);
          free(filedata);
          return 1;
     }
     fclose(f);

     /* Song index table: the last NUMINDEXINTS int32s of the file,
        little-endian. */
     memcpy(index, filedata + (flen - NUMINDEXINTS*4), NUMINDEXINTS*4);
     for (i = 0; i < NUMINDEXINTS; i++)
          index[i] = (int32_t)B_LITTLE32(index[i]);

     snprintf(outpath, sizeof outpath, "%s/music", outdir);
     mkdir(outpath, 0777);

     for (e = 0; targets[e] >= 0; e++) {
          int ent = targets[e];
          char *chunk;
          int midilen;

          off = (long)index[ent*3] * 4096L;
          len = index[ent*3+1];
          if (len <= 0 || len > MAXBASESONGLENGTH) {
               printf("song_%03d: skip (bad length %ld)\n", ent, len);
               skipped++;
               continue;
          }
          if (off < 0 || off + len > flen) {
               printf("song_%03d: skip (offset %ld out of range)\n", ent, off);
               skipped++;
               continue;
          }

          chunk = malloc(len);
          if (!chunk) {
               fprintf(stderr, "render-music: out of memory\n");
               break;
          }
          memcpy(chunk, filedata + off, len);

          midilen = transmutehmp(chunk);
          if (midilen <= 0) {
               printf("song_%03d: transmutehmp failed (not an HMI chunk?)\n", ent);
               free(chunk);
               skipped++;
               continue;
          }

          snprintf(path, sizeof path, "%s/music/song_%03d.mid", outdir, ent);
          f = fopen(path, "wb");
          if (!f) {
               fprintf(stderr, "render-music: can't create %s\n", path);
               free(chunk);
               return 1;
          }
          if (fwrite(chunk, 1, midilen, f) != (size_t)midilen) {
               fprintf(stderr, "render-music: short write on %s\n", path);
               fclose(f);
               free(chunk);
               return 1;
          }
          fclose(f);
          printf("song_%03d: %ld-byte HMI -> %d-byte SMF\n", ent, len, midilen);
          free(chunk);
          converted++;
     }

     printf("render-music: %d songs converted, %d skipped\n", converted, skipped);
     free(filedata);
     return 0;
}
