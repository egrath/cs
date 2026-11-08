/* fileio.c - Save / Load / Merge using the custom .CSD format */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <curses.h>
#include "cs.h"

/*
 * Custom CalcStar sheet archive ("CSD3"):
 *   char  magic[4] = "CSD3"
 *   per column (127): unsigned char width, unsigned char precision
 *   unsigned char pwdlength, pwd chars        (0 = no password)
 *   unsigned short cellCount (little endian)
 *   cells: unsigned char col, unsigned short row(0-based LE),
 *          unsigned char type (2=text,3=num), unsigned char just,
 *          unsigned char prec+1 (0=none), unsigned char formf,
 *          unsigned short contlen + bytes (u16 LE),
 *          [type==3] BCD number of value:
 *               unsigned char sign (0/1),
 *               signed short exp10 (LE, two's complement),
 *               unsigned char ndig (0..18),
 *               CSN_DIGS (=22) digit bytes (0..9, most significant first),
 *          unsigned short strreslen + bytes, unsigned short replen + bytes
 *  (Older "CSD1"/"CSD2" files are refused with READ ERROR.)
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
    int i;
    fputc(v->sign, f);
    fputc(v->exp10 & 255, f);
    fputc((v->exp10 >> 8) & 255, f);
    fputc(v->ndig, f);
    for (i = 0; i < CSN_DIGS; i++) fputc(v->nd[i], f);
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
    v->ndig = (nd > CSN_DIGS) ? CSN_DIGS : nd;
    if (v->ndig < 0) v->ndig = 0;
    for (i = 0; i < CSN_DIGS; i++) {
        d = fgetc(f);
        if (d == EOF) return 0;
        v->nd[i] = (i < v->ndig && d >= 0 && d <= 9) ? (unsigned char) d : 0;
    }
    if (v->exp10 < -CSN_EMAX || v->exp10 > CSN_EMAX) v->exp10 = 0;
    if (v->ndig == 0) { v->exp10 = 0; v->sign = 0; }
    return 1;
}

int cs_save(const char *name, int c1, int r1, int c2, int r2, const char *pw)
{
    char path[128];
    FILE *f;
    int r, c, count = 0;

    sprintf(path, "%s.CSD", name);
    for (r = r1; r <= r2; r++)
        for (c = c1; c <= c2; c++)
            if (cells[r][c].type != CE_EMPTY) count++;
    f = fopen(path, "wb");
    if (!f) { cs_msg("can't open file"); return 0; }
    fwrite("CSD3", 1, 4, f);
    for (c = 0; c < CS_MAXCOL; c++) {
        fputc(colw[c], f);
        fputc(colp[c], f);
    }
    fputc(pw ? (int) strlen(pw) : 0, f);
    if (pw && pw[0]) fwrite(pw, 1, strlen(pw), f);
    wr16(f, count);
    for (r = r1; r <= r2; r++)
        for (c = c1; c <= c2; c++) {
            Cell *cl = &cells[r][c];
            char just;
            int len;
            if (cl->type == CE_EMPTY) continue;
            fputc(c, f);
            wr16(f, r);
            fputc((int) cl->type, f);
            just = cl->just;
            fputc((int) (unsigned char) just, f);
            fputc(cl->prec + 1, f);
            fputc(cl->formf, f);
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
    if (ferror(f)) { fclose(f); cs_msg("WRITE ERROR"); return 0; }
    if (fclose(f) == EOF) { cs_msg("WRITE CLOSE ERROR"); return 0; }
    return 1;
}

int cs_load(const char *name, int c0, int r0, int merge)
{
    char path[128];
    FILE *f;
    int c, count, pwlen, i;
    char magic[4];

    sprintf(path, "%s.CSD", name);
    f = fopen(path, "rb");
    if (!f) { cs_msg("write error %s not found", path); return 0; }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CSD3", 4) != 0) {
        fclose(f);
        cs_msg("READ ERROR");
        return 0;
    }
    if (!merge) {
        cs_clear_all();
        fname[0] = '\0';
        lockCols = lockRows = 0;
        winCol = winRow = 0;
    }
    for (c = 0; c < CS_MAXCOL; c++) {
        int a = fgetc(f), b = fgetc(f);
        if (a == EOF || b == EOF) { fclose(f); cs_msg("READ ERROR"); return 0; }
        colw[c] = a; colp[c] = b;
    }
    pwlen = fgetc(f);
    for (i = 0; i < pwlen; i++) fgetc(f);   /* password ignored on load */
    count = rd16(f);
    if (count < 0) { fclose(f); cs_msg("READ ERROR"); return 0; }
    for (i = 0; i < count; i++) {
        int cc, rr, ty, just, prc, frm, len;
        Cell *cl;
        cc = fgetc(f);
        rr = rd16(f);
        ty = fgetc(f);
        just = (char) fgetc(f);
        prc = fgetc(f) - 1;
        frm = fgetc(f);
        if (cc == EOF || rr < 0 || cc >= CS_MAXCOL || rr >= CS_MAXROW) break;
        cl = &cells[rr][cc];
        if (ty == 2) {
            len = rd16(f);
            cl->cont = (char *) malloc((size_t) len + 1);
            if (fread(cl->cont, 1, (size_t) len, f) != (size_t) len) { len = 0; }
            cl->cont[len] = '\0';
            cl->type = CE_TEXT;
            cl->just = just;
            cl->formf = frm;
            cl->prec = prc;
            csn_zero(&cl->val);
            cl->iserr = 0;
            cl->strres = NULL;
            cl->rep = NULL;
            entriesUsed++;
            len = rd16(f);
            if (len > 0) {
                cl->rep = (char *) malloc((size_t) len + 1);
                if (fread(cl->rep, 1, (size_t) len, f) != (size_t) len) len = 0;
                cl->rep[len] = '\0';
            }
        } else if (ty == 3) {
            Csn v;
            len = rd16(f);
            cl->cont = (char *) malloc((size_t) len + 1);
            if (fread(cl->cont, 1, (size_t) len, f) != (size_t) len) len = 0;
            cl->cont[len] = '\0';
            if (!rd_num(f, &v)) { cl->type = CE_EMPTY; break; }
            if (cs_is_inputf(cl)) csn_zero(&v);   /* re-evaluated after load */
            cl->type = CE_NUM;
            cl->just = just;
            cl->formf = frm;
            cl->prec = prc;
            csn_copy(&cl->val, &v);
            cl->iserr = 0;
            cl->strres = NULL;
            cl->rep = NULL;
            len = rd16(f);
            if (len > 0) {
                cl->strres = (char *) malloc((size_t) len + 1);
                if (fread(cl->strres, 1, (size_t) len, f) != (size_t) len) len = 0;
                cl->strres[len] = '\0';
            }
            len = rd16(f);
            if (len > 0) {
                cl->rep = (char *) malloc((size_t) len + 1);
                if (fread(cl->rep, 1, (size_t) len, f) != (size_t) len) len = 0;
                cl->rep[len] = '\0';
            }
            entriesUsed++;
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
    char path[128];
    FILE *f;
    char magic[4];
    int c, pwlen, i;
    sprintf(path, "%s.CSD", name);
    f = fopen(path, "rb");
    if (!f) { pw[0] = '\0'; return 0; }
    pw[0] = '\0';
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CSD1", 4)) { fclose(f); return 0; }
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
        if (ch == 27) return -1;
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
    char name[40], pwbuf[40], pw2[40];
    char dflt[40];
    int filex;
    FILE *probe;
    char promptbuf[80];

    strncpy(dflt, fname, 38);
    dflt[38] = '\0';
    if (cs_prompt_str("File name :", fname[0] ? dflt : "", name, (int) sizeof(name)) < 0)
        return 0;
    if (!name[0]) return 0;

    filex = 0;
    sprintf(pwbuf, "%s.CSD", name);
    probe = fopen(pwbuf, "rb");
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
    {
        int pc = cs_prompt_char("P(artial or A(ll");
        cs_draw();
        if (pc == 27) return 0;
        if (pc == 'P' || pc == 'p') {
            char b[24];
            int c1, r1, c2, r2;
            if (cs_prompt_str("top left corner:", "A1", b, (int) sizeof(b)) < 0) return 0;
            if (!cs_parse_coord(b, &c1, &r1)) { cs_msg("write error bad coord"); return 0; }
            sprintf(b, "%c%d", 'A' + curCol, curRow + 1);
            if (cs_prompt_str("bottom right corner:", "", b, (int) sizeof(b)) < 0) return 0;
            if (!cs_parse_coord(b, &c2, &r2)) { cs_msg("write error bad coord"); return 0; }
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
            cs_save(name, c1, r1, c2, r2, pwbuf);
        } else if (pc == 'A' || pc == 'a') {
            if (!cs_save(name, 0, 0, CS_MAXCOL - 1, CS_MAXROW - 1, pwbuf)) return 0;
        } else return 0;
    }
    strcpy(fname, name);
    sprintf(promptbuf, "SAVED");
    cs_msg("%s", promptbuf);
    return 1;
}

int cs_load_dialog(int merge)
{
    char name[40], b[24];
    int c0, r0, ok;
    if (cs_prompt_str(merge ? "File name :" : "File name : (make sure file is saved)",
                      "", name, (int) sizeof(name)) < 0) return 0;
    if (!name[0]) return 0;
    if (cs_prompt_str(merge ? "Load position : A1" : "Load position : A1", "A1", b, (int) sizeof(b)) < 0)
        return 0;
    if (!cs_parse_coord(b, &c0, &r0)) { cs_msg("bad coord"); return 0; }
    cs_msg("...");
    ok = cs_load(name, c0, r0, merge);
    cs_draw();
    if (!ok) cs_msg("write error %s not found", name);
    return ok;
}
