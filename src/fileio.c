/* fileio.c - Save / Load / Merge using the custom .CSD format */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cs.h"

/*
 * Custom CalcStar sheet archive ("CSD5" - the format VERSION rides in
 * the magic: CSD1..CSD4 are older layouts and are refused with
 * READ ERROR; a future layout change bumps the digit so a stale file
 * refuses cleanly instead of desyncing - the 2026-11-16 full-precision
 * change left the magic stale and CSD4 files desynced on the variable
 * digit count):
 *   char  magic[4] = "CSD5"
 *   unsigned char  origin col  (0-based; the saved range's upper-left
 *                  corner - manual 14-9: the load position anchors the
 *                  FILE's upper left corner, not its first data cell,
 *                  so an All-mode sheet loads identically at A1)
 *   unsigned short origin row (0-based LE)
 *   per column (127): unsigned char width, unsigned char precision
 *   unsigned char pwdlength, pwd chars        (0 = no password)
 *   unsigned short cellCount (little endian)
 *   cells: unsigned char col, unsigned short row(0-based LE),
 *          unsigned char type (2=text,3=num), unsigned char just,
 *          unsigned char prec+1 (0=none, bits 0-4) | notation<<5
 *            (bits 5-6: 0=auto,1=sci,2=dec - flagged ext 2026-11),
 *          unsigned char formf,
 *          unsigned char volatf (the volatile-entry flag, batch G
 *            2026-11-15),
 *          unsigned short contlen + bytes (u16 LE),
 *          [type==3] BCD number of value:
 *               unsigned char sign (0/1),
 *               signed short exp10 (LE, two's complement),
 *               unsigned char ndig (0..CSN_DIGS; the variable digit
 *               count keeps the record honest for the full 40-digit
 *               mantissa),
 *               ndig digit bytes (0..9, most significant first - the
 *               full 40-digit mantissa is stored),
 *          unsigned short strreslen + bytes, unsigned short replen + bytes
 *  (CSD1/2/3/4 files are refused with READ ERROR - the CSD4 layout's
 *   fixed 22-digit padding is unparseable beside the variable count.)
 */
static void wr16(FILE *f, int v)
{
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}

static int rd16(FILE *f)
{
    int a = fgetc(f), b = fgetc(f);
    if (a == EOF || b == EOF) return -1;
    return a | (b << 8);
}

static void wr_num(FILE *f, const Csn *v)
{
    int i, nd;
    fputc(v->sign, f);
    fputc(v->exp10 & 255, f);
    fputc((v->exp10 >> 8) & 255, f);
    /* the FULL mantissa (the user's report, 2026-11-16): the old
     * fixed 22-digit record dropped the 40-digit engine's tail -
     * the record writes exactly ndig digit bytes (the loader reads
     * the variable count, so old 22-digit files load unchanged) */
    nd = (v->ndig > CSN_DIGS) ? CSN_DIGS : v->ndig;
    fputc(nd, f);
    for (i = 0; i < nd; i++)
        fputc(v->nd[i], f);
}

static int rd_num(FILE *f, Csn *v)
{
    int i, sign, e0, e1, nd, d;
    sign = fgetc(f);
    e0 = fgetc(f);
    e1 = fgetc(f);
    nd = fgetc(f);
    if (sign == EOF || e0 == EOF || e1 == EOF || nd == EOF) return 0;
    v->sign = (sign != 0) ? 1 : 0;
    v->exp10 = e0 | (e1 << 8);
    if (v->exp10 >= 32768) v->exp10 -= 65536;
    /* the VARIABLE digit count: the old fixed-22 records (nd <= 22)
     * and the new full-precision records (nd <= CSN_DIGS) both load */
    v->ndig = (nd > CSN_DIGS) ? CSN_DIGS : nd;
    if (v->ndig < 0) v->ndig = 0;
    for (i = 0; i < v->ndig; i++) {
        d = fgetc(f);
        if (d == EOF) return 0;
        v->nd[i] = (d >= 0 && d <= 9) ? (unsigned char) d : 0;
    }
    /* the record's remaining digits (a shorter stored count): the
     * mantissa's stored length is authoritative */
    for (; i < CSN_DIGS; i++) v->nd[i] = 0;
    if (v->exp10 < -CSN_EMAX || v->exp10 > CSN_EMAX) v->exp10 = 0;
    if (v->ndig == 0) { v->exp10 = 0; v->sign = 0; }
    return 1;
}

/* user-facing file-name rule: a typed ".Csd"/".csd"/".CSD" tail is kept
 * exactly as entered (never doubled on save), anything else gets the
 * lowercase ".csd" appended.  Used by save, load, merge and the
 * password probe so every path spells the same file. */
static void cs_savename(const char *name, char *out, int outn)
{
    size_t n = strlen(name);
    if (n >= 4) {
        const char *e = name + n - 4;
        if (e[0] == '.' &&
            toupper((unsigned char) e[1]) == 'C' &&
            toupper((unsigned char) e[2]) == 'S' &&
            toupper((unsigned char) e[3]) == 'D') {
            /* the typed tail is kept, bounded by the output size */
            int k = (int) n;
            if (k > outn - 1) k = outn - 1;
            memcpy(out, name, (size_t) k);
            out[k] = '\0';
            return;
        }
    }
    /* the lowercase ".csd" appended, the name capped so the result
     * always fits (the buffers are CS_PATHMAX + 4 since the 2026-11-22
     * long-path batch) */
    {
        int k = (int) n;
        if (k > outn - 5) k = outn - 5;
        memcpy(out, name, (size_t) k);
        memcpy(out + k, ".csd", 5);
    }
}

/* ------------------- the CSV path (user spec, 2026-11-17) ------------
 * A filename with a .csv extension (any case) takes the CSV path in
 * the save/load dialogs; everything else stays CSD.  Plain 7-bit
 * ASCII both ways (RFC 4180): the numerics as 17 significant digits
 * (cs_csv_num - the IEEE-754 precision other spreadsheets hold),
 * text as typed with doubled quotes and quoted commas, embedded
 * CR/LF flattened to a space on BOTH sides (the grid cannot hold a
 * newline; flattening both ways keeps the round trip consistent -
 * flagged: tools that preserve the embedded break round-trip it
 * losslessly, we do not).  On import every byte > 127 is dropped,
 * a leading UTF-8 BOM is skipped, and a field is a NUMBER only when
 * the whole field parses as a pure numeric literal - everything
 * else commits as text with NO formula evaluation (CSV is static).
 * No recalculation on load. */

/* the .csv extension test (any case) - cross-file: the CLI load
 * dispatches on it too */
int cs_csv_ext(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return 0;
    return name[n - 4] == '.' &&
           toupper((unsigned char) name[n - 3]) == 'C' &&
           toupper((unsigned char) name[n - 2]) == 'S' &&
           toupper((unsigned char) name[n - 1]) == 'V';
}

/* one text field: 7-bit, CR/LF flattened to a space, quoted (with
 * the internal quotes doubled) when it carries a comma or a quote */
static void csv_put_field(FILE *f, const char *s)
{
    int quote = 0, k;
    for (k = 0; s[k]; k++) {
        unsigned char ch = (unsigned char) s[k];
        if (ch > 127) continue;             /* plain 7-bit ascii */
        if (ch == ',' || ch == '"') { quote = 1; break; }
    }
    if (quote) fputc('"', f);
    for (k = 0; s[k]; k++) {
        unsigned char ch = (unsigned char) s[k];
        if (ch > 127) continue;
        if (ch == '\r' || ch == '\n') { fputc(' ', f); continue; }
        if (ch == '"') fputc('"', f);       /* doubled inside quotes */
        fputc((int) ch, f);
    }
    if (quote) fputc('"', f);
}

int cs_save_csv(const char *name, int c1, int r1, int c2, int r2)
{
    FILE *f;
    int r, c, err;
    /* the trailing empty fields and rows carry nothing: trim them so
     * an All-mode export emits the USED range (the spreadsheet
     * convention) instead of a 255x127 comma desert; leading and
     * embedded empties stay (they are positions) */
    {
        int lastrow = -1, lastcol = -1;
        for (r = r1; r <= r2; r++)
            for (c = c1; c <= c2; c++) {
                Cell *cl = cs_getcell(c, r);
                if (cl && cl->type != CE_EMPTY && cl->type != CE_ALLOC) {
                    if (r > lastrow) lastrow = r;
                    if (c > lastcol) lastcol = c;
                }
            }
        if (lastrow < r1) {          /* a wholly blank range */
            f = fopen(name, "wb");   /* still write an empty file */
            if (!f) { cs_msg("can't open"); return 0; }
            if (fclose(f) != 0) { cs_msg("WRITE ERROR"); return 0; }
            return 1;
        }
        r2 = lastrow;
        c2 = lastcol;
    }
    f = fopen(name, "wb");                  /* the name AS TYPED */
    if (!f) { cs_msg("can't open"); return 0; }
    for (r = r1; r <= r2; r++) {
        for (c = c1; c <= c2; c++) {
            Cell *cl = cs_getcell(c, r);
            char nbuf[64];
            if (c > c1) fputc(',', f);
            if (!cl || cl->type == CE_EMPTY || cl->type == CE_ALLOC)
                continue;                   /* an empty field */
            if (cl->type == CE_NUM) {
                if (cl->iserr) { fputs("?n?", f); continue; }
                if (cl->strres) {           /* the derived string type */
                    csv_put_field(f, cl->strres);
                    continue;
                }
                cs_csv_num(&cl->val, nbuf, (int) sizeof(nbuf));
                fputs(nbuf, f);
                continue;
            }
            csv_put_field(f, cl->cont ? cl->cont : "");
        }
        if (r < r2) { fputc('\r', f); fputc('\n', f); }
    }
    err = ferror(f);
    if (fclose(f) != 0) err = 1;
    if (err) { cs_msg("WRITE ERROR"); return 0; }
    return 1;
}

/* open a CSV positioned after a leading UTF-8 BOM (a plain rewind
 * when the first bytes are not the BOM - the file is a disk file) */
static FILE *csv_open_bom(const char *name)
{
    FILE *f = fopen(name, "rb");
    int b1, b2, b3;
    if (!f) return NULL;
    b1 = fgetc(f); b2 = fgetc(f); b3 = fgetc(f);
    if (b1 == EOF) { fclose(f); return NULL; }
    if (!(b1 == 0xEF && b2 == 0xBB && b3 == 0xBF)) rewind(f);
    return f;
}

/* the field-parsing state machine (shared by both passes): pulls one
 * field into buf (bounded, truncated at 255 - flagged), returns:
 *   1 = a field was read,  2 = the record ended after the field,
 *   0 = the file ended. */
static int csv_get_field(FILE *f, char *buf, int bufn)
{
    int fl = 0, inq = 0, done = 0, rec = 0;
    buf[0] = '\0';
    while (!done) {
        int c = fgetc(f);
        if (c == EOF) {
            done = 1;
            if (fl == 0 && !rec) return 0;   /* nothing left */
            break;
        }
        if (inq) {
            if (c == '"') {
                int n2 = fgetc(f);
                if (n2 == '"') {
                    if (fl < bufn - 1) buf[fl++] = '"';
                } else {
                    inq = 0;
                    if (n2 != EOF) ungetc(n2, f);
                }
            } else if (c == '\r' || c == '\n') {
                if (fl < bufn - 1) buf[fl++] = ' ';   /* flattened */
            } else if (c < 128) {
                if (fl < bufn - 1) buf[fl++] = (char) c;
            }                                       /* >127 dropped */
        } else if (c == '"') {
            inq = 1;
        } else if (c == ',') {
            done = 1;
        } else if (c == '\n') {
            done = 1; rec = 1;
        } else if (c == '\r') {
            int n2 = fgetc(f);
            if (n2 != '\n' && n2 != EOF) ungetc(n2, f);
            done = 1; rec = 1;
        } else if (c < 128) {
            if (fl < bufn - 1) buf[fl++] = (char) c;
        }                                       /* >127 dropped */
    }
    buf[fl] = '\0';
    return rec ? 2 : 1;
}

int cs_load_csv(const char *name, int c0, int r0, int merge)
{
    FILE *f;
    int row, col, ok = 1;
    /* ---- pass 1: the dimensions and the cell count, so an oversized
     * file is refused with the sheet untouched (the CSD loader's
     * two-pass discipline) - the clear happens after pass 1 ---- */
    f = csv_open_bom(name);
    if (!f) { cs_msg("read error %s not found", name); return 0; }
    {
        char buf[256];
        int r2 = r0, c2 = c0, nonblank = 0;
        while (ok) {
            int st = csv_get_field(f, buf, (int) sizeof(buf));
            if (st == 0) break;
            if (buf[0] != '\0') {
                nonblank++;
                if (r2 >= CS_MAXROW || c2 >= CS_MAXCOL) ok = 0;
            }
            if (st == 2) { r2++; c2 = c0; }
            else c2++;
        }
        fclose(f);
        if (!ok) { cs_msg("READ ERROR"); return 0; }
        if (nonblank > CS_BUDGET) { cs_msg("MEMORY IS TOO LOW"); return 0; }
    }
    if (!merge) cs_clear_all();             /* ;L replaces, ;M overlays */
    else cs_reg_reset();                    /* merged data: fresh sums */
    /* ---- pass 2: place the fields ---- */
    f = csv_open_bom(name);
    if (!f) { cs_msg("read error %s not found", name); return 0; }
    row = r0; col = c0;
    for (;;) {
        char buf[256];
        int st = csv_get_field(f, buf, (int) sizeof(buf));
        if (st == 0) break;
        if (buf[0] != '\0') {
            Csn pv;                        /* parsed BEFORE the store
                                              touches the cell: the
                                              parse target must not
                                              live inside it (the
                                              setters free first) */
            Cell *cl = cs_alloc_cell(col, row);
            if (!cl) { cs_msg("MEMORY IS TOO LOW"); ok = 0; break; }
            if (csn_parse(buf, &pv) == 0) {
                if (!cs_set_num(cl, buf, &pv, 0, NULL))
                    { cs_msg("MEMORY IS TOO LOW"); ok = 0; break; }
            } else if (!cs_set_text(cl, buf, 'L')) {
                cs_msg("MEMORY IS TOO LOW"); ok = 0; break;
            }
        }
        if (st == 2) { row++; col = c0; }
        else col++;
    }
    fclose(f);
    if (ok) cs_recount_entries();
    return ok;
}

int cs_save(const char *name, int c1, int r1, int c2, int r2, const char *pw)
{
    char path[CS_PATHMAX + 4];   /* name (<= CS_PATHMAX) + ".csd" */
    FILE *f;
    int r, c, count = 0;
    Cell *cl;

    cs_savename(name, path, (int) sizeof(path));
    for (r = r1; r <= r2; r++)
        for (c = c1; c <= c2; c++)
            if ((cl = cs_getcell(c, r)) != NULL && cl->type != CE_EMPTY)
                count++;
    f = fopen(path, "wb");
    if (!f) { cs_msg("can't open file"); return 0; }
    fwrite("CSD5", 1, 4, f);
    /* the saved range's upper-left corner: the load position anchors
     * THIS, so All-mode sheets (origin 0,0) load identically at A1 */
    fputc(c1, f);
    wr16(f, r1);
    for (c = 0; c < CS_MAXCOL; c++) {
        fputc(colw[c], f);
        fputc(colp[c], f);
    }
    fputc(pw ? (int) strlen(pw) : 0, f);
    if (pw && pw[0]) fwrite(pw, 1, strlen(pw), f);
    wr16(f, count);
    /* the populated cells in coordinate order (the ordered scan) */
    c = c1; r = r1;
    while (r <= r2) {
        cl = cs_next_cell(&c, &r);
        if (!cl || r > r2) break;
        if (c > c2) { c = c1; r++; continue; }
        if (cl->type != CE_EMPTY) {
            char just;
            int len;
            fputc(c, f);
            wr16(f, r);
            fputc((int) cl->type, f);
            just = cl->just;
            fputc((int) (unsigned char) just, f);
            /* prec+1 occupies 0..25, so bits 5-6 carry the notation
             * (0=auto,1=sci,2=dec) and bit 7 the factorial-entry
             * flag - the layout is stable here */
            fputc(((cl->prec + 1) & 31) | ((cl->notation & 3) << 5) |
                  ((cl->factf & 1) << 7), f);
            fputc(cl->formf, f);
            fputc(cl->volatf, f);
            len = cl->cont ? (int) strlen(cl->cont) : 0;
            wr16(f, len);
            if (len) fwrite(cl->cont, 1, (size_t) len, f);
            if (cl->type == CE_NUM) {
                wr_num(f, &cl->val);
                len = cl->strres ? (int) strlen(cl->strres) : 0;
                wr16(f, len);
                if (len) fwrite(cl->strres, 1, (size_t) len, f);
            }
            len = (cl->rep && just == 'W') ? (int) strlen(cl->rep) : 0;
            wr16(f, len);
            if (len) fwrite(cl->rep, 1, (size_t) len, f);
        }
        c = cl->col + 1;
        if (c > c2) { c = c1; r++; }
    }
    if (ferror(f)) { fclose(f); cs_msg("WRITE ERROR"); return 0; }
    if (fclose(f) == EOF) { cs_msg("WRITE CLOSE ERROR"); return 0; }
    return 1;
}

/* the pre-scan's record walk (external review item 7, 2026-11-15):
 * validates ONE cell record without touching any sheet state -
 * every read must succeed, every length must be non-negative, and
 * the type must be one the loader can consume in sync.  Returns
 * 1 ok, 0 malformed/truncated. */
static int rd_check_record(FILE *f)
{
    int cc, rr, ty, len;
    Csn v;
    cc = fgetc(f);
    rr = rd16(f);
    ty = fgetc(f);
    if (cc == EOF || rr < 0 || ty == EOF) return 0;
    if (fgetc(f) == EOF) return 0;              /* just */
    if (fgetc(f) == EOF) return 0;              /* prec/notation/factf */
    if (fgetc(f) == EOF) return 0;              /* formf */
    if (fgetc(f) == EOF) return 0;              /* volatf */
    /* an unknown type would desync the stream (the loader would not
     * know which payload fields follow); 1 = the ALLOC-EMPTY record
     * (the form-mode cell - the same payload shape as 2) */
    if (ty != 1 && ty != 2 && ty != 3) return 0;
    len = rd16(f);                              /* cont */
    if (len < 0) return 0;
    for (; len > 0; len--)
        if (fgetc(f) == EOF) return 0;
    if (ty == 3) {
        if (!rd_num(f, &v)) return 0;
        len = rd16(f);                          /* strres */
        if (len < 0) return 0;
        for (; len > 0; len--)
            if (fgetc(f) == EOF) return 0;
    }
    len = rd16(f);                              /* rep (both types) */
    if (len < 0) return 0;
    for (; len > 0; len--)
        if (fgetc(f) == EOF) return 0;
    return 1;
}

int cs_load(const char *name, int c0, int r0, int merge)
{
    char path[CS_PATHMAX + 4];   /* name (<= CS_PATHMAX) + ".csd" */
    FILE *f;
    int c, count, pwlen, i;
    int bascc = 0, basrr = 0;
    char magic[4];

    cs_savename(name, path, (int) sizeof(path));
    f = fopen(path, "rb");
    if (!f) { cs_msg("read error %s not found", path); return 0; }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CSD5", 4) != 0) {
        fclose(f);
        cs_msg("READ ERROR");
        return 0;
    }
    {
        int oc = fgetc(f), or1 = rd16(f);
        if (oc == EOF || or1 < 0) { fclose(f); cs_msg("READ ERROR"); return 0; }
        bascc = oc; basrr = or1;
    }
    /* ---- pass 1: validate the WHOLE file before the sheet is
     * touched (external review item 7, 2026-11-15) - a malformed,
     * truncated or over-budget file is refused with the user's
     * current sheet intact.  Two passes over a file cost nothing on
     * an XT-class disk. ---- */
    for (c = 0; c < CS_MAXCOL; c++) {
        int a = fgetc(f), b = fgetc(f);
        if (a == EOF || b == EOF) { fclose(f); cs_msg("READ ERROR"); return 0; }
    }
    pwlen = fgetc(f);
    if (pwlen == EOF) { fclose(f); cs_msg("READ ERROR"); return 0; }
    for (i = 0; i < pwlen; i++)
        if (fgetc(f) == EOF) { fclose(f); cs_msg("READ ERROR"); return 0; }
    count = rd16(f);
    /* the count is 16-bit but the sheet holds at most CS_BUDGET
     * entries - an over-budget file is refused here (flagged: the
     * manual's READ ERROR wording covers every malformed-file case) */
    if (count < 0 || count > CS_BUDGET) {
        fclose(f);
        cs_msg("READ ERROR");
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!rd_check_record(f)) {
            fclose(f);
            cs_msg("READ ERROR");
            return 0;
        }
    }
    /* ---- pass 2: the validated load (clear happens only now) ---- */
    rewind(f);
    if (fread(magic, 1, 4, f) != 4) { fclose(f); cs_msg("READ ERROR"); return 0; }
    {
        int oc = fgetc(f), or1 = rd16(f);
        if (oc == EOF || or1 < 0) { fclose(f); cs_msg("READ ERROR"); return 0; }
        bascc = oc; basrr = or1;
    }
    if (!merge) {
        cs_clear_all();
        fname[0] = '\0';
        lockCols = lockRows = 0;
        winCol = winRow = 0;
    } else {
        cs_reg_reset();     /* merged data under an old snapshot */
    }
    for (c = 0; c < CS_MAXCOL; c++) {
        int a = fgetc(f), b = fgetc(f);
        if (a == EOF || b == EOF) { fclose(f); cs_msg("READ ERROR"); return 0; }
        /* the RANGES are enforced (the user's memory-safety report,
         * 2026-11-16): a corrupt width (0..255 raw) fed every
         * downstream render buffer - the manual's Format-command
         * range 3..63 clamps, the precision caps at CS_MAX_P */
        colw[c] = (a < 3) ? 3 : ((a > CS_MAX_W) ? CS_MAX_W : a);
        colp[c] = (b < 0) ? 0 : ((b > CS_MAX_P) ? CS_MAX_P : b);
    }
    pwlen = fgetc(f);
    if (pwlen == EOF) { fclose(f); cs_msg("READ ERROR"); return 0; }
    for (i = 0; i < pwlen; i++) fgetc(f);   /* password ignored on load */
    count = rd16(f);
    if (count < 0) { fclose(f); cs_msg("READ ERROR"); return 0; }
    for (i = 0; i < count; i++) {
        int cc, rr, ty, just, prc, frm, nta, fct, len, vol;
        int dr, dc, drop;
        Cell *cl, scratch;
        cc = fgetc(f);
        rr = rd16(f);
        ty = fgetc(f);
        just = (char) fgetc(f);
        nta = fgetc(f);
        prc = (nta & 31) - 1;           /* bits 0-4: prec+1 (0=none) */
        fct = (nta >> 7) & 1;           /* bit 7: factorial entry */
        nta = (nta >> 5) & 3;           /* bits 5-6: notation */
        frm = fgetc(f);
        vol = fgetc(f);                 /* the volatile-entry flag */
        if (cc == EOF || rr < 0) break; /* pass 1 made this impossible */
        /* Load position (manual 14-9: "If the upper left corner is to
         * be positioned at another location, enter the coordinates of
         * that location"): the stored range's upper-left corner goes at
         * the requested position, so an All-mode sheet (origin 0,0)
         * loads identically at A1.  Cells running off the sheet edge
         * are parsed and DROPPED so the file stream stays in sync. */
        dc = c0 + cc - bascc;
        dr = r0 + rr - basrr;
        drop = (dc >= CS_MAXCOL || dr >= CS_MAXROW || dc < 0 || dr < 0);
        memset(&scratch, 0, sizeof(scratch));
        cl = drop ? &scratch : cs_alloc_cell(dc, dr);
        if (!drop && !cl) {
            /* the entry pool is exhausted: report and stop cleanly */
            fclose(f);
            cs_msg("MEMORY IS TOO LOW");
            return 0;
        }
        /* merge overlays: release the destination cell's old content
         * first (bookkeeping stays exact) */
        if (!drop && cl->type != CE_EMPTY)
            cs_free_cell(cl);
        if (ty == 1 || ty == 2) {
            len = rd16(f);
            cl->cont = (char *) malloc((size_t) len + 1);
            if (!cl->cont) { fclose(f); cs_msg("MEMORY IS TOO LOW"); return 0; }
            if (fread(cl->cont, 1, (size_t) len, f) != (size_t) len) {
                free(cl->cont); cl->cont = NULL;
                fclose(f); cs_msg("READ ERROR"); return 0;
            }
            cl->cont[len] = '\0';
            cl->type = (ty == 1) ? CE_ALLOC : CE_TEXT;
            cl->just = just;
            cl->formf = frm;
            cl->volatf = (vol != 0);
            cl->prec = prc;
            cl->notation = nta;
            cl->factf = fct;
            csn_zero(&cl->val);
            cl->iserr = 0;
            cl->strres = NULL;
            cl->rep = NULL;
            if (ty == 1) {
                /* the allocated-empty cell has NO content (the
                 * in-memory form-mode state) */
                free(cl->cont);
                cl->cont = NULL;
            }
            if (!drop) entriesUsed++;
            len = rd16(f);
            if (len > 0) {
                cl->rep = (char *) malloc((size_t) len + 1);
                if (!cl->rep) { fclose(f); cs_msg("MEMORY IS TOO LOW"); return 0; }
                if (fread(cl->rep, 1, (size_t) len, f) != (size_t) len) {
                    free(cl->rep); cl->rep = NULL;
                    fclose(f); cs_msg("READ ERROR"); return 0;
                }
                cl->rep[len] = '\0';
            }
        } else if (ty == 3) {
            Csn v;
            len = rd16(f);
            cl->cont = (char *) malloc((size_t) len + 1);
            if (!cl->cont) { fclose(f); cs_msg("MEMORY IS TOO LOW"); return 0; }
            if (fread(cl->cont, 1, (size_t) len, f) != (size_t) len) {
                free(cl->cont); cl->cont = NULL;
                fclose(f); cs_msg("READ ERROR"); return 0;
            }
            cl->cont[len] = '\0';
            if (!rd_num(f, &v)) { cl->type = CE_EMPTY; break; }
            if (cs_is_inputf(cl)) csn_zero(&v);   /* re-evaluated after load */
            /* the LOAD-AT-ORIGIN reference adjust (the user's report,
             * 2026-11-16): a formula loaded into a different location
             * behaves like a copy there - the relative references
             * shift by the file's offset (the frozen '!' refs stay);
             * the value holders hold no references */
            if (!cs_is_inputf(cl) && cl->cont &&
                (bascc != c0 || basrr != r0)) {
                char *adj = cs_adjust_refs(cl->cont,
                                           c0 - bascc, r0 - basrr);
                if (adj) { free(cl->cont); cl->cont = adj; }
            }
            cl->type = CE_NUM;
            cl->just = just;
            cl->formf = frm;
            cl->volatf = (vol != 0);
            cl->prec = prc;
            cl->notation = nta;
            cl->factf = fct;
            csn_copy(&cl->val, &v);
            cl->iserr = 0;
            cl->strres = NULL;
            cl->rep = NULL;
            len = rd16(f);
            if (len > 0) {
                cl->strres = (char *) malloc((size_t) len + 1);
                if (!cl->strres) { fclose(f); cs_msg("MEMORY IS TOO LOW"); return 0; }
                if (fread(cl->strres, 1, (size_t) len, f) != (size_t) len) {
                    free(cl->strres); cl->strres = NULL;
                    fclose(f); cs_msg("READ ERROR"); return 0;
                }
                cl->strres[len] = '\0';
            }
            len = rd16(f);
            if (len > 0) {
                cl->rep = (char *) malloc((size_t) len + 1);
                if (!cl->rep) { fclose(f); cs_msg("MEMORY IS TOO LOW"); return 0; }
                if (fread(cl->rep, 1, (size_t) len, f) != (size_t) len) {
                    free(cl->rep); cl->rep = NULL;
                    fclose(f); cs_msg("READ ERROR"); return 0;
                }
                cl->rep[len] = '\0';
            }
            if (!drop) entriesUsed++;
        }
        if (drop) {
            /* beyond the edge: release the parsed strings */
            free(scratch.cont);
            free(scratch.strres);
            free(scratch.rep);
        }
    }
    fclose(f);
    curCol = c0; curRow = r0;
    winCol = 0; winRow = 0;
    strcpy(fname, name);
    return 1;
}

/* read the stored password of a .CSD file */
int cs_file_password(const char *name, char *pw, int max)
{
    char path[CS_PATHMAX + 4];   /* name (<= CS_PATHMAX) + ".csd" */
    FILE *f;
    char magic[4];
    int c, pwlen, i;
    cs_savename(name, path, (int) sizeof(path));
    f = fopen(path, "rb");
    if (!f) { pw[0] = '\0'; return 0; }
    pw[0] = '\0';
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CSD5", 4)) { fclose(f); return 0; }
    /* the CSD5 header carries the ORIGIN (1 col + 2 row bytes) between
     * the magic and the column table - the old CSD1 probe walked
     * straight into the column table and read the precision bytes as
     * a password, so EVERY overwrite asked for verification and even
     * the correct password failed (the user's persistence bug,
     * 2026-11-16) */
    if (fgetc(f) == EOF || fgetc(f) == EOF || fgetc(f) == EOF) {
        fclose(f);
        return 0;
    }
    for (c = 0; c < CS_MAXCOL * 2; c++) fgetc(f);
    pwlen = fgetc(f);
    if (pwlen > 0 && pwlen < max) {
        for (i = 0; i < pwlen; i++) pw[i] = (char) fgetc(f);
        pw[pwlen] = '\0';
    }
    fclose(f);
    return 1;
}

/* ------------------------- dialogs ------------------------------- */
int cs_prompt_hidden(const char *pr, char *out, int max)
{
    char buf[64];
    char dots[64];
    int n = 0, ch, dn = 0;
    buf[0] = '\0';
    dots[0] = '\0';
    for (;;) {
        char line[80];
        sprintf(line, "%s ", pr);
        cs_draw_prompt_text(line, dots);
        ch = getch();
        if (cs_was_resize(ch)) continue;
        if (ch == 27) { cs_prompt_hide(); return -1; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
        if (ch == KEY_BACKSPACE || ch == 8 || ch == 127) {
            if (n > 0) buf[--n] = '\0';
            if (dn > 0) dots[--dn] = '\0';
        } else if (ch >= 32 && ch < 127 && n < max - 1) {
            buf[n++] = (char) ch;
            buf[n] = '\0';
            if (dn < 60) { dots[dn++] = '*'; dots[dn] = '\0'; }
        }
    }
    strncpy(out, buf, max - 1);
    out[max - 1] = '\0';
    return n;
}

int cs_save_dialog(void)
{
    char name[CS_PATHMAX], pwbuf[40], pw2[40];
    char dflt[CS_PATHMAX];
    int filex;
    FILE *probe;
    char promptbuf[80];

    strncpy(dflt, fname, (int) sizeof(dflt) - 1);
    dflt[sizeof(dflt) - 1] = '\0';
    if (cs_prompt_str("File name :", fname[0] ? dflt : "", name, (int) sizeof(name)) < 0)
        return 0;
    if (!name[0]) { cs_prompt_hide(); return 0; }

    /* the CSV path (user spec 2026-11-17): a .csv extension takes the
     * CSV route - no password prompts, no CSD layout; the name is
     * used AS TYPED (cs_savename would append ".csd") */
    if (cs_csv_ext(name)) {
        char npath[CS_PATHMAX];
        FILE *probe;
        strncpy(npath, name, (int) sizeof(npath) - 1);
        npath[sizeof(npath) - 1] = '\0';
        probe = fopen(npath, "rb");
        if (probe) {
            fclose(probe);
            if (cs_prompt_yn("File exists. Destroy old contents (y,n)?") != 1)
                return 0;
        }
        for (;;) {
            int pc = cs_prompt_char("P)artial or A)all");
            cs_draw();
            if (pc == 27) return 0;
            if (pc == 'P' || pc == 'p') {
                char b[24];
                int c1, r1, c2, r2, t1, t2;
                if (cs_prompt_str("top left corner:", "A1", b, (int) sizeof(b)) < 0) return 0;
                if (!cs_coord_text(b, &c1, &r1, &t1, &t2)) { cs_msg("write error bad coord"); return 0; }
                sprintf(b, "%c%d", 'A' + curCol, curRow + 1);
                if (cs_prompt_str("bottom right corner:", "", b, (int) sizeof(b)) < 0) return 0;
                if (!cs_coord_text(b, &c2, &r2, &t1, &t2)) { cs_msg("write error bad coord"); return 0; }
                if (c2 < c1 || r2 < r1) { cs_msg("write error end>start"); return 0; }
                if (c2 >= CS_MAXCOL || r2 >= CS_MAXROW) { cs_msg("write error bad coord"); return 0; }
                if (!cs_save_csv(npath, c1, r1, c2, r2)) return 0;
                break;
            } else if (pc == 'A' || pc == 'a') {
                if (!cs_save_csv(npath, 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1)) return 0;
                break;
            }
        }
        cs_msg("%s", "SAVED");
        return 1;
    }

    filex = 0;
    {
        char probebuf[CS_PATHMAX + 4];   /* name (<= CS_PATHMAX) + ".csd" */
        cs_savename(name, probebuf, (int) sizeof(probebuf));
        probe = fopen(probebuf, "rb");
    }
    if (probe) { fclose(probe); filex = 1; }

    if (filex) {
        int yn = cs_prompt_yn("File exists. Destroy old contents (y,n)?");
        cs_draw();
        if (yn != 1) return 0;
        {
            char oldpw[40];
            cs_file_password(name, oldpw, (int) sizeof(oldpw));
            if (oldpw[0]) {
                char got[40];
                if (cs_prompt_hidden("Verify the password to remove:", got, 40) < 0)
                    return 0;
                if (strcmp(got, oldpw) != 0) { cs_msg("not ok"); return 0; }
            }
        }
    }
    if (cs_prompt_hidden("Password (<CR>=none):", pwbuf, 40) < 0) return 0;
    if (pwbuf[0]) {
        if (cs_prompt_hidden("Again:", pw2, 40) < 0) return 0;
        if (strcmp(pwbuf, pw2) != 0) { cs_msg("write error, not the same"); return 0; }
    }
    /* the question must be answered (original CalcStar): anything but
     * P/A re-prompts; ESC cancels the dialog.  The loop EXITS after a
     * successful save (a broken loop would re-ask forever). */
    for (;;) {
        int pc = cs_prompt_char("P)artial or A)all");
        cs_draw();
        if (pc == 27) return 0;
        if (pc == 'P' || pc == 'p') {
            char b[24];
            int c1, r1, c2, r2, t1, t2;
            if (cs_prompt_str("top left corner:", "A1", b, (int) sizeof(b)) < 0) return 0;
            if (!cs_coord_text(b, &c1, &r1, &t1, &t2)) { cs_msg("write error bad coord"); return 0; }
            sprintf(b, "%c%d", 'A' + curCol, curRow + 1);
            if (cs_prompt_str("bottom right corner:", "", b, (int) sizeof(b)) < 0) return 0;
            if (!cs_coord_text(b, &c2, &r2, &t1, &t2)) { cs_msg("write error bad coord"); return 0; }
            if (c2 < c1 || r2 < r1) {
                /* manual: "write error end>start - in saving a partial
                 * file, the top left coordinate must come before the
                 * bottom right coordinate" */
                cs_msg("write error end>start");
                return 0;
            }
            if (c2 >= CS_MAXCOL || r2 >= CS_MAXROW) {
                cs_msg("write error bad coord"); return 0;
            }
            if (!cs_save(name, c1, r1, c2, r2, pwbuf)) return 0;
            break;
        } else if (pc == 'A' || pc == 'a') {
            if (!cs_save(name, 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1, pwbuf)) return 0;
            break;
        }
        /* any other answer (e.g. bare RETURN) loops back to the prompt:
         * the question must be answered; it must never silently drop
         * the save */
    }
    strcpy(fname, name);
    sprintf(promptbuf, "SAVED");
    cs_msg("%s", promptbuf);
    return 1;
}

int cs_load_dialog(int merge)
{
    char name[CS_PATHMAX], b[24];
    int c0, r0, ok;
    if (cs_prompt_str(merge ? "File name :" :
                      "File name (make sure current file is saved) :",
                      "", name, (int) sizeof(name)) < 0) return 0;
    if (!name[0]) { cs_prompt_hide(); return 0; }
    if (cs_prompt_coord(merge ? "Load position:" : "Load position:", "A1", b, (int) sizeof(b), 1) < 0)
        return 0;
    {
        int t1, t2;
        if (!cs_coord_text(b, &c0, &r0, &t1, &t2)) { cs_msg("bad coord"); return 0; }
    }
    cs_msg("...");
    if (cs_csv_ext(name))
        ok = cs_load_csv(name, c0, r0, merge);   /* the CSV path */
    else
        ok = cs_load(name, c0, r0, merge);
    cs_draw();
    if (!ok) {
        /* the loader painted its own reason (read error ... not
         * found / READ ERROR / MEMORY IS TOO LOW) BEFORE this
         * repaint - paint_status wipes the message row, so the
         * reason is re-painted from lastmsg AFTER the draw (the old
         * fallback probe painted a SECOND, reworded message here) */
        cs_msg_repaint();
    }
    return ok;
}
