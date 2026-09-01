/*
 * wavchunk — split/join helper for the Stems tool.
 *
 * The SpleeterRT engine loads the whole track and holds a full-length
 * spectrogram, so its peak RSS is linear in input length. Measured on device
 * 2026-09-01 with the shipped binary:
 *
 *     30 s -> 447 MB      60 s -> 592 MB      => ~302 MB + 4.83 MB per second
 *
 * The Move has ~1.25 GB free, so anything past ~145 s of audio is OOM-killed
 * (SIGKILL, which the driver reports as exit 137). v0.3.2 tried to buy headroom
 * with a swapfile, but swapon(2) needs CAP_SYS_ADMIN and the tool runs as
 * uid 1000 — it never once succeeded.
 *
 * Instead we cap the engine's working set: run it over fixed-length chunks that
 * overlap by a crossfade region, then reassemble each stem here. Peak RSS then
 * depends on the chunk length, not the track length.
 *
 * Subcommands:
 *   info  <in.wav>
 *       -> "frames=<n> rate=<r> channels=<c> bits=<b> format=<f>"
 *   split <in.wav> <outdir> <chunk_frames> <xfade_frames>
 *       -> writes <outdir>/chunk_%03d.wav, prints one "<name> <frames>" per line.
 *          Adjacent chunks overlap by exactly xfade_frames, which is the
 *          invariant `join` relies on.
 *   join  <out.wav> <xfade_frames> <part0.wav> <part1.wav> ...
 *       -> linear crossfade across each overlap, streamed (memory is bounded by
 *          the crossfade buffer, not by the output length).
 *
 * `split` copies sample bytes verbatim, so any PCM format survives it intact.
 * `join` has to do arithmetic, so it decodes; it writes back in the format it
 * read (the engine emits 32-bit float stereo).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define FMT_PCM   1
#define FMT_FLOAT 3

typedef struct {
    FILE    *f;
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    uint16_t format;
    uint32_t frames;
    long     data_off;      /* byte offset of the first sample */
    int      frame_bytes;   /* channels * bits/8 */
} wav_t;

static int rd_u32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }
static int rd_u16(FILE *f, uint16_t *v) { return fread(v, 2, 1, f) == 1; }

static int wav_open(const char *path, wav_t *w)
{
    char id[4];
    uint32_t sz;

    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "rb");
    if (!w->f) { fprintf(stderr, "wavchunk: cannot open %s\n", path); return -1; }

    if (fread(id, 1, 4, w->f) != 4 || memcmp(id, "RIFF", 4) != 0) goto bad;
    if (!rd_u32(w->f, &sz)) goto bad;
    if (fread(id, 1, 4, w->f) != 4 || memcmp(id, "WAVE", 4) != 0) goto bad;

    int have_fmt = 0, have_data = 0;
    while (!have_data) {
        if (fread(id, 1, 4, w->f) != 4) break;
        if (!rd_u32(w->f, &sz)) break;
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t block_align; uint32_t byte_rate;
            if (!rd_u16(w->f, &w->format))   goto bad;
            if (!rd_u16(w->f, &w->channels)) goto bad;
            if (!rd_u32(w->f, &w->rate))     goto bad;
            if (!rd_u32(w->f, &byte_rate))   goto bad;
            if (!rd_u16(w->f, &block_align)) goto bad;
            if (!rd_u16(w->f, &w->bits))     goto bad;
            if (sz > 16) fseek(w->f, (long)sz - 16, SEEK_CUR);
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            w->data_off = ftell(w->f);
            /* A streamed writer can leave data size 0 or 0xffffffff; trust the
             * file length in that case rather than producing zero frames. */
            long here = w->data_off;
            fseek(w->f, 0, SEEK_END);
            long end = ftell(w->f);
            uint32_t avail = (uint32_t)(end - here);
            if (sz == 0 || sz == 0xffffffffu || sz > avail) sz = avail;
            w->frame_bytes = w->channels * (w->bits / 8);
            if (w->frame_bytes <= 0) goto bad;
            w->frames = sz / (uint32_t)w->frame_bytes;
            fseek(w->f, here, SEEK_SET);
            have_data = 1;
        } else {
            fseek(w->f, (long)sz + (sz & 1), SEEK_CUR);
        }
    }
    if (!have_fmt || !have_data) goto bad;
    if (w->format != FMT_PCM && w->format != FMT_FLOAT) {
        fprintf(stderr, "wavchunk: unsupported WAV format %u in %s\n", w->format, path);
        goto bad_quiet;
    }
    return 0;
bad:
    fprintf(stderr, "wavchunk: malformed WAV: %s\n", path);
bad_quiet:
    fclose(w->f); w->f = NULL;
    return -1;
}

static void wav_close(wav_t *w) { if (w->f) fclose(w->f); w->f = NULL; }

/* Write a canonical 44-byte header. data_bytes may be patched later. */
static int write_header(FILE *f, const wav_t *src, uint32_t data_bytes)
{
    uint32_t byte_rate = src->rate * (uint32_t)src->frame_bytes;
    uint16_t block_align = (uint16_t)src->frame_bytes;
    uint32_t sz16 = 16, riff = 36 + data_bytes;
    if (fwrite("RIFF", 1, 4, f) != 4) return -1;
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&sz16, 4, 1, f);
    fwrite(&src->format, 2, 1, f);
    fwrite(&src->channels, 2, 1, f);
    fwrite(&src->rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&src->bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    return fwrite(&data_bytes, 4, 1, f) == 1 ? 0 : -1;
}

/* ---------- sample decode / encode ---------- */

static void decode(const unsigned char *in, double *out, int n, const wav_t *w)
{
    int i;
    if (w->format == FMT_FLOAT && w->bits == 32) {
        for (i = 0; i < n; i++) { float v; memcpy(&v, in + i * 4, 4); out[i] = (double)v; }
    } else if (w->bits == 16) {
        for (i = 0; i < n; i++) {
            int16_t v; memcpy(&v, in + i * 2, 2);
            out[i] = v / 32768.0;
        }
    } else if (w->bits == 24) {
        for (i = 0; i < n; i++) {
            const unsigned char *p = in + i * 3;
            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            out[i] = (double)(v >> 8) / 8388608.0;
        }
    } else if (w->bits == 32) {
        for (i = 0; i < n; i++) {
            int32_t v; memcpy(&v, in + i * 4, 4);
            out[i] = (double)v / 2147483648.0;
        }
    } else if (w->bits == 8) {
        for (i = 0; i < n; i++) out[i] = ((double)in[i] - 128.0) / 128.0;
    }
}

static void encode(const double *in, unsigned char *out, int n, const wav_t *w)
{
    int i;
    if (w->format == FMT_FLOAT && w->bits == 32) {
        for (i = 0; i < n; i++) { float v = (float)in[i]; memcpy(out + i * 4, &v, 4); }
        return;
    }
    /* Encode with the same scale decode() used, so that split->join without any
     * processing in between is bit-exact rather than losing a LSB per pass. */
    for (i = 0; i < n; i++) {
        double s = in[i];
        if (w->bits == 16) {
            long v = lrint(s * 32768.0);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            int16_t q = (int16_t)v;
            memcpy(out + i * 2, &q, 2);
        } else if (w->bits == 24) {
            long v = lrint(s * 8388608.0);
            if (v >  8388607) v =  8388607;
            if (v < -8388608) v = -8388608;
            out[i * 3 + 0] = (unsigned char)(v & 0xff);
            out[i * 3 + 1] = (unsigned char)((v >> 8) & 0xff);
            out[i * 3 + 2] = (unsigned char)((v >> 16) & 0xff);
        } else if (w->bits == 32) {
            double v = s * 2147483648.0;
            if (v >  2147483647.0) v =  2147483647.0;
            if (v < -2147483648.0) v = -2147483648.0;
            int32_t q = (int32_t)llrint(v);
            memcpy(out + i * 4, &q, 4);
        } else if (w->bits == 8) {
            long v = lrint(s * 128.0) + 128;
            if (v > 255) v = 255;
            if (v < 0)   v = 0;
            out[i] = (unsigned char)v;
        }
    }
}

/* ---------- info ---------- */

static int cmd_info(const char *path)
{
    wav_t w;
    if (wav_open(path, &w) < 0) return 1;
    printf("frames=%u rate=%u channels=%u bits=%u format=%u\n",
           w.frames, w.rate, (unsigned)w.channels, (unsigned)w.bits, (unsigned)w.format);
    wav_close(&w);
    return 0;
}

/* ---------- split ---------- */

#define COPY_BUF_FRAMES 8192

static int copy_range(wav_t *src, FILE *out, uint32_t start, uint32_t count)
{
    unsigned char *buf = malloc((size_t)COPY_BUF_FRAMES * src->frame_bytes);
    if (!buf) return -1;
    if (fseek(src->f, src->data_off + (long)start * src->frame_bytes, SEEK_SET) != 0) {
        free(buf); return -1;
    }
    while (count > 0) {
        uint32_t n = count > COPY_BUF_FRAMES ? COPY_BUF_FRAMES : count;
        if (fread(buf, src->frame_bytes, n, src->f) != n) { free(buf); return -1; }
        if (fwrite(buf, src->frame_bytes, n, out) != n) { free(buf); return -1; }
        count -= n;
    }
    free(buf);
    return 0;
}

static int cmd_split(const char *in, const char *outdir, uint32_t chunk, uint32_t xfade)
{
    wav_t w;
    if (wav_open(in, &w) < 0) return 1;
    if (chunk < 2 * xfade + 1) {
        fprintf(stderr, "wavchunk: chunk_frames must exceed 2*xfade_frames\n");
        wav_close(&w); return 1;
    }

    uint32_t step = chunk - xfade;   /* advance per chunk; the overlap is xfade */
    uint32_t pos = 0, idx = 0;

    while (pos < w.frames) {
        uint32_t len = w.frames - pos;
        if (len > chunk) len = chunk;

        /* A final sliver shorter than the crossfade cannot be faded into the
         * previous chunk, so absorb it: extend this chunk to the end instead of
         * emitting a runt. Costs at most 2*xfade extra frames of engine input. */
        uint32_t remaining_after = w.frames - (pos + len);
        if (remaining_after > 0 && remaining_after < 2 * xfade) {
            len = w.frames - pos;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/chunk_%03u.wav", outdir, idx);
        FILE *out = fopen(path, "wb");
        if (!out) { fprintf(stderr, "wavchunk: cannot write %s\n", path); wav_close(&w); return 1; }
        if (write_header(out, &w, len * (uint32_t)w.frame_bytes) < 0 ||
            copy_range(&w, out, pos, len) < 0) {
            fprintf(stderr, "wavchunk: write failed for %s\n", path);
            fclose(out); wav_close(&w); return 1;
        }
        fclose(out);
        printf("chunk_%03u.wav %u\n", idx, len);

        if (pos + len >= w.frames) break;
        pos += step;
        idx++;
    }

    wav_close(&w);
    return 0;
}

/* ---------- join ---------- */

static int cmd_join(const char *outpath, uint32_t xfade, char **parts, int nparts)
{
    wav_t first;
    if (wav_open(parts[0], &first) < 0) return 1;

    FILE *out = fopen(outpath, "wb");
    if (!out) { fprintf(stderr, "wavchunk: cannot write %s\n", outpath); wav_close(&first); return 1; }
    if (write_header(out, &first, 0) < 0) { fclose(out); wav_close(&first); return 1; }
    wav_close(&first);

    int fb = 0, ch = 0;
    uint32_t written = 0;
    double *tail = NULL;         /* previous part's last xfade frames, decoded */
    unsigned char *raw = NULL;   /* scratch, xfade frames wide */
    double *cur = NULL;
    unsigned char *cbuf = NULL;
    int rc = 1;

    for (int i = 0; i < nparts; i++) {
        wav_t w;
        if (wav_open(parts[i], &w) < 0) goto done;
        if (i == 0) {
            fb = w.frame_bytes; ch = w.channels;
            tail  = malloc((size_t)xfade * ch * sizeof(double));
            cur   = malloc((size_t)xfade * ch * sizeof(double));
            raw   = malloc((size_t)xfade * fb);
            cbuf  = malloc((size_t)xfade * fb);
            if (!tail || !cur || !raw || !cbuf) {
                fprintf(stderr, "wavchunk: out of memory\n"); wav_close(&w); goto done;
            }
        } else if (w.frame_bytes != fb || w.channels != ch) {
            fprintf(stderr, "wavchunk: part %s has a different format\n", parts[i]);
            wav_close(&w); goto done;
        }

        uint32_t body_start = (i > 0) ? xfade : 0;
        uint32_t body_end   = (i < nparts - 1) ? w.frames - xfade : w.frames;
        if (w.frames < body_start || body_end < body_start) {
            fprintf(stderr, "wavchunk: part %s is shorter than the crossfade\n", parts[i]);
            wav_close(&w); goto done;
        }

        /* Crossfade this part's head against the previous part's tail. */
        if (i > 0) {
            if (fseek(w.f, w.data_off, SEEK_SET) != 0 ||
                fread(raw, fb, xfade, w.f) != xfade) {
                fprintf(stderr, "wavchunk: short read in %s\n", parts[i]);
                wav_close(&w); goto done;
            }
            decode(raw, cur, (int)(xfade * (uint32_t)ch), &w);
            for (uint32_t f = 0; f < xfade; f++) {
                double g = (double)(f + 1) / (double)(xfade + 1);
                for (int c = 0; c < ch; c++) {
                    uint32_t k = f * (uint32_t)ch + (uint32_t)c;
                    /* lerp, not tail*(1-g) + cur*g: (1-g)+g is not exactly 1 in
                     * floating point, so the symmetric form perturbs samples
                     * even where the two parts agree. This form is exact when
                     * they do, which keeps an unprocessed split/join
                     * bit-identical. The buffers are double so that 32-bit PCM
                     * survives too — float has only 24 mantissa bits. */
                    cur[k] = tail[k] + (cur[k] - tail[k]) * g;
                }
            }
            encode(cur, cbuf, (int)(xfade * (uint32_t)ch), &w);
            if (fwrite(cbuf, fb, xfade, out) != xfade) { wav_close(&w); goto done; }
            written += xfade;
        }

        if (copy_range(&w, out, body_start, body_end - body_start) < 0) {
            fprintf(stderr, "wavchunk: copy failed in %s\n", parts[i]);
            wav_close(&w); goto done;
        }
        written += body_end - body_start;

        /* Stash this part's tail for the next crossfade. */
        if (i < nparts - 1) {
            if (fseek(w.f, w.data_off + (long)(w.frames - xfade) * fb, SEEK_SET) != 0 ||
                fread(raw, fb, xfade, w.f) != xfade) {
                fprintf(stderr, "wavchunk: short tail read in %s\n", parts[i]);
                wav_close(&w); goto done;
            }
            decode(raw, tail, (int)(xfade * (uint32_t)ch), &w);
        }
        wav_close(&w);
    }

    /* Patch the two length fields now that the total is known. */
    {
        uint32_t data_bytes = written * (uint32_t)fb;
        uint32_t riff = 36 + data_bytes;
        if (fseek(out, 4, SEEK_SET) != 0) goto done;
        fwrite(&riff, 4, 1, out);
        if (fseek(out, 40, SEEK_SET) != 0) goto done;
        fwrite(&data_bytes, 4, 1, out);
    }
    rc = 0;

done:
    free(tail); free(cur); free(raw); free(cbuf);
    if (fclose(out) != 0) rc = 1;
    if (rc != 0) remove(outpath);
    return rc;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: wavchunk info  <in.wav>\n"
            "       wavchunk split <in.wav> <outdir> <chunk_frames> <xfade_frames>\n"
            "       wavchunk join  <out.wav> <xfade_frames> <part.wav>...\n");
        return 2;
    }
    if (strcmp(argv[1], "info") == 0 && argc == 3)
        return cmd_info(argv[2]);
    if (strcmp(argv[1], "split") == 0 && argc == 6)
        return cmd_split(argv[2], argv[3], (uint32_t)strtoul(argv[4], NULL, 10),
                         (uint32_t)strtoul(argv[5], NULL, 10));
    if (strcmp(argv[1], "join") == 0 && argc >= 5)
        return cmd_join(argv[2], (uint32_t)strtoul(argv[3], NULL, 10), &argv[4], argc - 4);

    fprintf(stderr, "wavchunk: bad arguments\n");
    return 2;
}
