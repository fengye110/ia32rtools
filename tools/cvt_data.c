#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_assert.h"
#include "my_str.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define IS(w, y) !strcmp(w, y)
#define IS_START(w, y) !strncmp(w, y, strlen(y))

#include "protoparse.h"

static const char *asmfn;
static int asmln;
static FILE *g_fhdr;

enum dx_type {
  DXT_UNSPEC,
  DXT_BYTE,
  DXT_WORD,
  DXT_DWORD,
  DXT_QUAD,
  DXT_TEN,
};

#define aerr(fmt, ...) do { \
	printf("%s:%d: error: " fmt, asmfn, asmln, ##__VA_ARGS__); \
  fcloseall(); \
	exit(1); \
} while (0)

#include "masm_tools.h"

static char *next_word_s(char *w, size_t wsize, char *s)
{
  int quote = 0;
	size_t i;

	s = sskip(s);

	for (i = 0; i < wsize - 1; i++) {
    if (s[i] == '\'')
      quote ^= 1;
		if (s[i] == 0 || (!quote && (my_isblank(s[i]) || s[i] == ',')))
			break;
		w[i] = s[i];
	}
	w[i] = 0;

	if (s[i] != 0 && !my_isblank(s[i]) && s[i] != ',')
		printf("warning: '%s' truncated\n", w);

	return s + i;
}

static void next_section(FILE *fasm, char *name)
{
  char words[2][256];
  char line[256];
  int wordc;
  char *p;

  name[0] = 0;

  while (fgets(line, sizeof(line), fasm))
  {
    wordc = 0;
    asmln++;

    p = sskip(line);
    if (*p == 0)
      continue;

    if (*p == ';') {
      while (strlen(line) == sizeof(line) - 1) {
        // one of those long comment lines..
        if (!fgets(line, sizeof(line), fasm))
          break;
      }
      continue;
    }

    for (wordc = 0; wordc < ARRAY_SIZE(words); wordc++) {
      p = sskip(next_word(words[wordc], sizeof(words[0]), p));
      if (*p == 0 || *p == ';') {
        wordc++;
        break;
      }
    }

    if (wordc < 2)
      continue;

    if (!IS(words[1], "segment"))
      continue;

    strcpy(name, words[0]);
    break;
  }
}

static enum dx_type parse_dx_directive(const char *name)
{
  if (IS(name, "dd"))
    return DXT_DWORD;
  if (IS(name, "dw"))
    return DXT_WORD;
  if (IS(name, "db"))
    return DXT_BYTE;
  if (IS(name, "dq"))
    return DXT_QUAD;
  if (IS(name, "dt"))
    return DXT_TEN;

  return DXT_UNSPEC;
}

static const char *type_name(enum dx_type type)
{
  switch (type) {
  case DXT_BYTE:
    return ".byte";
  case DXT_WORD:
    return ".word";
  case DXT_DWORD:
    return ".long";
  case DXT_QUAD:
    return ".quad";
  case DXT_TEN:
    return ".tfloat";
  case DXT_UNSPEC:
    break;
  }
  return "<bad>";
}

static int type_size(enum dx_type type)
{
  switch (type) {
  case DXT_BYTE:
    return 1;
  case DXT_WORD:
    return 2;
  case DXT_DWORD:
    return 4;
  case DXT_QUAD:
    return 8;
  case DXT_TEN:
    return 10;
  case DXT_UNSPEC:
    break;
  }
  return -1;
}

static char *escape_string(char *s)
{
  char buf[256];
  char *t = buf;

  for (; *s != 0; s++) {
    if (*s == '"') {
      strcpy(t, "\\22");
      t += strlen(t);
      continue;
    }
    if (*s == '\\') {
      strcpy(t, "\\\\");
      t += strlen(t);
      continue;
    }
    *t++ = *s;
  }
  *t = *s;
  return strcpy(s, buf);
}

int main(int argc, char *argv[])
{
  FILE *fout, *fasm;
  char words[20][256];
  //int sep_after[20];
  char word[256];
  char line[256];
  char comment[256];
  unsigned long val;
  unsigned long cnt;
  const char *sym;
  enum dx_type type;
  int is_label;
  int wordc;
  int first;
  int arg_out;
  int arg = 1;
  int len;
  int w;
  char *p;
  char *p2;

  if (argc != 4) {
    printf("usage:\n%s <.s> <.asm> <hdrf>\n",
      argv[0]);
    return 1;
  }

  arg_out = arg++;

  asmfn = argv[arg++];
  fasm = fopen(asmfn, "r");
  my_assert_not(fasm, NULL);

  hdrfn = argv[arg++];
  g_fhdr = fopen(hdrfn, "r");
  my_assert_not(g_fhdr, NULL);

  fout = fopen(argv[arg_out], "w");
  my_assert_not(fout, NULL);

  comment[0] = 0;

  while (!feof(fasm)) {
    next_section(fasm, line);
    if (IS(line + 1, "text"))
      continue;

    if (IS(line + 1, "rdata"))
      fprintf(fout, "\n.section .rodata\n");
    else if (IS(line + 1, "data"))
      fprintf(fout, "\n.data\n");
    else
      aerr("unhandled section: '%s'\n", line);

    fprintf(fout, ".align 4\n");

    while (fgets(line, sizeof(line), fasm))
    {
      sym = NULL;
      asmln++;

      p = sskip(line);
      if (*p == 0 || *p == ';')
        continue;

      for (wordc = 0; wordc < ARRAY_SIZE(words); wordc++) {
        //sep_after[wordc] = 0;
        p = sskip(next_word_s(words[wordc], sizeof(words[0]), p));
        if (*p == 0 || *p == ';') {
          wordc++;
          break;
        }
        if (*p == ',') {
          //sep_after[wordc] = 1;
          p = sskip(p + 1);
        }
      }

      if (wordc == 2 && IS(words[1], "ends"))
        break;
      if (wordc < 2)
        aerr("unhandled: '%s'\n", words[0]);

      // don't cares
      if (IS(words[0], "assume"))
        continue;

      if (IS(words[0], "align")) {
        val = parse_number(words[1]);
        fprintf(fout, "\t\t  .align %ld", val);
        goto fin;
      }

      w = 1;
      type = parse_dx_directive(words[0]);
      if (type == DXT_UNSPEC) {
        type = parse_dx_directive(words[1]);
        sym = words[0];
        w = 2;
      }
      if (type == DXT_UNSPEC)
        aerr("unhandled decl: '%s %s'\n", words[0], words[1]);

      if (sym != NULL) {
        len = strlen(sym);
        fprintf(fout, "_%s:", sym);

        len += 2;
        if (len < 8)
          fprintf(fout, "\t");
        if (len < 16)
          fprintf(fout, "\t");
        if (len <= 16)
          fprintf(fout, "  ");
        else
          fprintf(fout, " ");
      }
      else {
        fprintf(fout, "\t\t  ");
      }

      if (type == DXT_BYTE && words[w][0] == '\'') {
        // string; use asciz for most common case
        if (w == wordc - 2 && IS(words[w + 1], "0")) {
          fprintf(fout, ".asciz \"");
          wordc--;
        }
        else
          fprintf(fout, ".ascii \"");

        for (; w < wordc; w++) {
          if (words[w][0] == '\'') {
            p = words[w] + 1;
            p2 = strchr(p, '\'');
            if (p2 == NULL)
              aerr("unterminated string? '%s'\n", p);
            memcpy(word, p, p2 - p);
            word[p2 - p] = 0;
            fprintf(fout, "%s", escape_string(word));
          }
          else {
            val = parse_number(words[w]);
            if (val & ~0xff)
              aerr("bad string trailing byte?\n");
            fprintf(fout, "\\x%02lx", val);
          }
        }
        fprintf(fout, "\"");
        goto fin;
      }

      if (w == wordc - 2) {
        if (IS_START(words[w + 1], "dup(")) {
          cnt = parse_number(words[w]);
          p = words[w + 1] + 4;
          p2 = strchr(p, ')');
          if (p2 == NULL)
            aerr("bad dup?\n");
          memmove(word, p, p2 - p);
          word[p2 - p] = 0;
          val = parse_number(word);

          fprintf(fout, ".fill 0x%02lx,%d,0x%02lx",
            cnt, type_size(type), val);
          goto fin;
        }
      }

      if (type == DXT_DWORD && words[w][0] == '\''
        && words[w][5] == '\'' && strlen(words[w]) == 6)
      {
        if (w != wordc - 1)
          aerr("TODO\n");

        p = words[w];
        val = (p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4];
        fprintf(fout, ".long 0x%lx", val);
        snprintf(comment, sizeof(comment), "%s", words[w]);
        goto fin;
      }

      if ((type == DXT_QUAD || type == DXT_TEN)
          && strchr(words[w], '.'))
      {
        if (w != wordc - 1)
          aerr("TODO\n");

        fprintf(fout, type == DXT_TEN ? ".tfloat " : ".double ");
        fprintf(fout, "%s", words[w]);
        goto fin;
      }

      first = 1;
      fprintf(fout, "%s ", type_name(type));
      for (; w < wordc; w++)
      {
        if (!first)
          fprintf(fout, ", ");

        is_label = 0;
        if (w >= wordc - 2 && IS(words[w], "offset")) {
          is_label = 1;
          w++;
        }
        else if (type == DXT_DWORD
                 && !('0' <= words[w][0] && words[w][0] <= '9'))
        {
          // assume label
          is_label = 1;
        }

        if (is_label) {
          p = words[w];
          if (IS_START(p, "loc_") || strchr(p, '?') || strchr(p, '@'))
          {
            fprintf(fout, "0");
            snprintf(comment, sizeof(comment), "%s", words[w + 1]);
            goto fin;
          }
          fprintf(fout, "_%s", p);
        }
        else {
          val = parse_number(words[w]);
          if (val < 10)
            fprintf(fout, "%ld", val);
          else
            fprintf(fout, "0x%lx", val);
        }

        first = 0;
      }

fin:
      if (comment[0] != 0) {
        fprintf(fout, "\t\t# %s", comment);
        comment[0] = 0;
      }
      fprintf(fout, "\n");
      (void)proto_parse;
    }
  }

  fclose(fout);
  fclose(fasm);
  fclose(g_fhdr);

  return 0;
}

// vim:ts=2:shiftwidth=2:expandtab
