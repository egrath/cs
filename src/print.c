/* print.c - the Print command: reports to a .PRN file, the PRINTER
   device (written to CALCSTAR.PRN), ">file" comma-delimited export,
   or DISPLAY (console view) */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curses.h>
#include "cs.h"

static void wait_space(void)
{
    int ch;
    for (;;) {
        ch = getch();
        if (cs_was_resize(ch)) continue;
        if (ch == ' ' || ch == 27) break;
    }
}

/* a row is a page break if the leading cell of the spoke is "/page" */
static int is_pagebreak_row(int row, int c1, int c2)
{
    Cell *c = cs_cell(c1, row);
    if (c2 < c1) c2 = c1;
    return c && c->type == CE_TEXT && c->cont &&
           strncmp(c->cont, "/page", 5) == 0 && c->cont[5] == '\0';
}

/* cell print text: values right-justified formatted, text as displayed */
static void print_render(int col, int row, char *out, int width)
{
    Cell *c = cs_cell(col, row);
    out[0] = '\0';
    memset(out, ' ', (size_t) width);
    out[width] = '\0';
    if (!c || c->type == CE_EMPTY || c->type == CE_ALLOC) return;
    if (c->type == CE_TEXT) {
        if (c->just == 'W') {
            int i, j = 0, rl;
            const char *rep = (c->rep && c->rep[0]) ? c->rep : "=";
            rl = (int) strlen(rep);
            for (i = 0; i < width; i++) out[i] = rep[j++ % rl];
            out[width] = '\0';
            return;
        }
        {
            const char *s = c->cont ? c->cont : "";
            int len = (int) strlen(s);
            if (len > width) {
                memcpy(out, s, (size_t) width);
                out[width] = '\0';
            } else if (c->just == 'R') {
                memcpy(out + (width - len), s, (size_t) len);
            } else if (c->just == 'C') {
                memcpy(out + (width - len) / 2, s, (size_t) len);
            } else {
                memcpy(out, s, (size_t) len);
            }
        }
        return;
    }
    if (c->iserr) {
        sprintf(out, "%*s", width, "?n?");
        return;
    }
    if (c->strres) {
        int len = (int) strlen(c->strres);
        if (len < width) memcpy(out + (width - len), c->strres, (size_t) len);
        else memcpy(out, c->strres, (size_t) width);
        return;
    }
    if (cs_is_inputf(c)) {
        sprintf(out, "%*s", width, "In!");
        return;
    }
    {
        char buf[64];
        int prec = (c->prec >= 0) ? c->prec : colp[col];
        cs_fmt_num(&c->val, width, prec, 0, buf, (int) sizeof(buf));
        sprintf(out, "%*s", width, buf);
    }
}

void cs_print_cmd(void)
{
    char dest[64], tl[40], br[40], tmp[40];
    int c1, r1, c2, r2;
    int form, stopp, pw, fix, ntitle = 0;
    char titles[5][62];
    FILE *f = NULL;
    int console, delimited;
    int segStart[64], segEnd[64], nseg, seg;
    int col, row, i;

    if (cs_prompt_str("To which file?", "PRINTER :", dest, (int) sizeof(dest)) < 0)
        return;
    if (!dest[0]) return;
    console = (strcmp(dest, "DISPLAY") == 0);
    delimited = (dest[0] == '>' && dest[1] != '\0');
    if (console) {
        ;
    } else if (delimited) {
        f = fopen(dest + 1, "w");
        if (!f) { cs_msg("can't open"); return; }
    } else if (strcmp(dest, "PRINTER") == 0) {
        f = fopen("CALCSTAR.PRN", "w");
        if (!f) { cs_msg("can't open"); return; }
    } else {
        char base[48], path[64];
        strncpy(base, dest, 47);
        base[47] = '\0';
        for (i = (int) strlen(base) - 1; i >= 0; i--)
            if (base[i] == '.') { base[i] = '\0'; break; }
        sprintf(path, "%s.PRN", base);
        f = fopen(path, "w");
        if (!f) { cs_msg("can't open file"); return; }
    }

    if (cs_prompt_str("top left corner : A1", "", tl, (int) sizeof(tl)) < 0)
        goto fail;
    if (!cs_parse_coord(tl, &c1, &r1)) { cs_msg("bad coord"); goto fail; }
    if (cs_prompt_str("bottom right corner:  Hit RETURN for bottom right of array",
                      "", br, (int) sizeof(br)) < 0)
        goto fail;
    if (!br[0]) {
        c2 = c1; r2 = r1;
        for (r2 = CS_MAXROW - 1; r2 >= r1; r2--) {
            for (col = 0; col < CS_MAXCOL; col++)
                if (cs_cell(col, r2)->type != CE_EMPTY) goto gotrow;
        }
gotrow:
        if (r2 < r1) r2 = r1;
        for (c2 = CS_MAXCOL - 1; c2 >= c1; c2--) {
            for (row = 0; row < CS_MAXROW; row++)
                if (cs_cell(c2, row)->type != CE_EMPTY) goto gotcol;
        }
gotcol:
        if (c2 < c1) c2 = c1;
    } else if (strchr(br, '>')) {
        int a1, b1, a2, b2;
        if (!cs_parse_range(br, &a1, &b1, &a2, &b2)) { cs_msg("bad range coord"); goto fail; }
        c1 = a1; r1 = b1; c2 = a2; r2 = b2;
    } else if (!cs_parse_coord(br, &c2, &r2)) {
        cs_msg("bad coord"); goto fail;
    }
    if (c2 < c1 || r2 < r1 || c2 >= CS_MAXCOL || r2 >= CS_MAXROW) {
        cs_msg("bad coord"); goto fail;
    }

    if (cs_prompt_str("Form length : CONTINUOUS(or lines)", "CONTINUOUS", tmp, (int) sizeof(tmp)) < 0)
        goto fail;
    if (tmp[0] == 'C' || tmp[0] == 'c' || !tmp[0] ||
        strcmp(tmp, "CONTINUOUS") == 0 || strcmp(tmp, "CONTINUOUS ") == 0) form = 66;
    else {
        form = atoi(tmp);
        if (form < 1 || form > 66) { cs_msg("bad form length"); goto fail; }
    }
    stopp = cs_prompt_yn("Stop on each page (y,n)?");
    cs_draw();
    if (stopp < 0) goto fail;
    if (cs_prompt_str("Printer width:", "132", tmp, (int) sizeof(tmp)) < 0)
        goto fail;
    pw = atoi(tmp);
    if (pw < 10) pw = 132;
    fix = cs_prompt_yn("Fix ordinates (y,n)?");
    cs_draw();
    if (fix < 0) goto fail;

    /* titles */
    for (;;) {
        if (ntitle < 5) {
            if (cs_prompt_str("Title:", "", titles[ntitle], 62) < 0) break;
            if (!titles[ntitle][0]) break;
            ntitle++;
            i = cs_prompt_yn("Additional titles (y,n)?");
            cs_draw();
            if (i != 1) break;
        } else break;
    }
    cs_draw();
    if (f != NULL && !delimited && strcmp(dest, "PRINTER") == 0) {
        int chx;
        cs_msg("Report printing...  Make sure printer and paper ready.");
        chx = getch();
        if (chx == 27) { cs_prompt_hide(); cs_draw(); return; }
        cs_prompt_hide();
        cs_draw();
    }

    /* a single column wider than the paper cannot print at all
     * (manual appendix: "data too wide") */
    for (col = c1; col <= c2; col++)
        if (colw[col] + 1 > pw) { cs_msg("data too wide"); goto fail; }

    /* split columns into segments that fit the width */
    nseg = 0;
    segStart[0] = c1;
    {
        int w = 0;
        seg = 0;
        for (col = c1; col <= c2; col++) {
            if (w + colw[col] > pw && segStart[seg] < col) {
                segEnd[seg++] = col - 1;
                if (seg >= 64) { seg--; break; }
                segStart[seg] = col;
                w = 0;
            }
            w += colw[col];
        }
        segEnd[seg] = c2;
        nseg = seg + 1;
    }

    for (seg = 0; seg < nseg; seg++) {
        int lines = 0;
        row = r1;
        while (row <= r2) {
            if (is_pagebreak_row(row, c1, c2)) {
                /* eject before continuing; the /page row itself is not printed */
                if (lines > 0) {
                    if (!console && !delimited) fputc('\f', f);
                    if (stopp == 1) wait_space();
                    lines = 0;
                }
                row++;
                continue;
            }
            if (lines == 0) {
                int t;
                if (delimited) {
                    ;
                } else {
                    for (t = 0; t < ntitle; t++) {
                        if (console) printf("%s\n", titles[t]);
                        else fprintf(f, "%s\n", titles[t]);
                        lines++;
                    }
                    /* ordinates: column headings */
                    if (console || f) {
                        char line[256];
                        int n = 0;
                        if (fix) {
                            /* first header cell carries column A text */
                            ;
                        } else {
                            n = sprintf(line, "%6s", "");
                        }
                        for (col = segStart[seg]; col <= segEnd[seg]; col++) {
                            char rend[CS_MAX_W + 2];
                            if (fix)
                                print_render(col, r1, rend, colw[col] - 1);
                            else {
                                char cn[4];
                                cs_col_name(col, cn);
                                sprintf(rend, "%*s", colw[col] - 1, cn);
                            }
                            memcpy(line + n, rend, (size_t) (colw[col] - 1));
                            n += colw[col] - 1;
                            line[n] = '\0';
                        }
                        if (console) printf("%s\n", line);
                        else fprintf(f, "%s\n", line);
                        lines++;
                    }
                }
            }
            /* one sheet row */
            if (delimited) {
                int cc, firstf = 1;
                for (cc = segStart[seg]; cc <= segEnd[seg]; cc++) {
                    char rend[CS_MAX_W + 2];
                    char *p;
                    print_render(cc, row, rend, colw[cc] - 1);
                    if (!firstf) fputc(',', f);
                    firstf = 0;
                    p = rend;
                    while (*p == ' ') p++;
                    fprintf(f, "%s", p);
                }
                fputc('\n', f);
            } else if (console) {
                char lab[16], rend[CS_MAX_W + 2];
                int cc;
                if (fix) {
                    Cell *h = cs_cell(c1 - 1, row);
                    if (c1 > 0 && h && h->type != CE_EMPTY) {
                        lab[0] = '\0';
                        print_render(c1 - 1, row, lab, 6);
                        lab[6] = '\0';
                    } else sprintf(lab, "      ");
                } else sprintf(lab, " %4d ", row + 1);
                printf("%s", lab);
                for (cc = segStart[seg]; cc <= segEnd[seg]; cc++) {
                    print_render(cc, row, rend, colw[cc] - 1);
                    printf("%*s", colw[cc] - 1, rend);
                }
                printf("\n");
            } else {
                char lab[16], rend[CS_MAX_W + 2];
                int cc;
                if (fix) {
                    Cell *h = cs_cell(c1 - 1, row);
                    if (c1 > 0 && h && h->type != CE_EMPTY) {
                        lab[0] = '\0';
                        print_render(c1 - 1, row, lab, 6);
                        lab[6] = '\0';
                    } else strcpy(lab, "      ");
                } else sprintf(lab, " %4d ", row + 1);
                fprintf(f, "%s", lab);
                for (cc = segStart[seg]; cc <= segEnd[seg]; cc++) {
                    print_render(cc, row, rend, colw[cc] - 1);
                    fprintf(f, "%*s", colw[cc] - 1, rend);
                }
                fputc('\n', f);
            }
            lines++;
            row++;
            if (row <= r2 && lines >= form - 2) {
                if (!console && !delimited) fputc('\f', f);
                if (stopp == 1) wait_space();
                lines = 0;

            }
        }
        if (seg < nseg - 1 && f && !delimited) fputc('\f', f);
    }

    if (!console && !delimited) {
        fprintf(f, "-** End of report\n");
    }
    if (f) fclose(f);
    if (console) {
        cs_msg("-** Hit space to continue");
        cs_draw();
        wait_space();
    }
    cs_msg("");
    cs_draw();
    return;

fail:
    if (f) fclose(f);
    cs_draw();
}
