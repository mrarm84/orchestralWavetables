/* Raw deflate (RFC 1951) decoder, small and slow, after Mark Adler's
 * "puff" (zlib license). No static state: everything is on the stack,
 * so it runs before any .data/.bss setup. */

#include "inflate.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

struct state {
  uint8_t *out;
  uint32_t outlen;
  uint32_t outcnt;
  const uint8_t *in;
  uint32_t inlen;
  uint32_t incnt;
  uint32_t bitbuf;
  int bitcnt;
  int error;
};

struct huffman {
  short count[MAXBITS + 1];
  short symbol[FIXLCODES];
};

static int bits(struct state *s, int need) {
  uint32_t val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->incnt == s->inlen) { s->error = 1; return 0; }
    val |= (uint32_t)(s->in[s->incnt++]) << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = val >> need;
  s->bitcnt -= need;
  return (int)(val & ((1UL << need) - 1));
}

static int stored(struct state *s) {
  uint32_t len;
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->incnt + 4 > s->inlen) return 2;
  len = s->in[s->incnt] | ((uint32_t)s->in[s->incnt + 1] << 8);
  if (s->in[s->incnt + 2] != (~len & 0xff) ||
      s->in[s->incnt + 3] != ((~len >> 8) & 0xff))
    return -2;
  s->incnt += 4;
  if (s->incnt + len > s->inlen) return 2;
  if (s->outcnt + len > s->outlen) return 1;
  while (len--) s->out[s->outcnt++] = s->in[s->incnt++];
  return 0;
}

static int decode(struct state *s, const struct huffman *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= MAXBITS; len++) {
    code |= bits(s, 1);
    if (s->error) return -10;
    int count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -10;
}

static int construct(struct huffman *h, const short *length, int n) {
  short offs[MAXBITS + 1];
  int len, symbol, left;
  for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
  for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
  if (h->count[0] == n) return 0;
  left = 1;
  for (len = 1; len <= MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return left;
  }
  offs[1] = 0;
  for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
  for (symbol = 0; symbol < n; symbol++)
    if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = symbol;
  return left;
}

static const short lbase[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short lext[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short dbase[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
  8193, 12289, 16385, 24577};
static const short dext[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct state *s, const struct huffman *lencode,
                 const struct huffman *distcode) {
  int symbol;
  do {
    symbol = decode(s, lencode);
    if (symbol < 0) return symbol;
    if (symbol < 256) {
      if (s->outcnt == s->outlen) return 1;
      s->out[s->outcnt++] = (uint8_t)symbol;
    } else if (symbol > 256) {
      symbol -= 257;
      if (symbol >= 29) return -10;
      uint32_t len = lbase[symbol] + bits(s, lext[symbol]);
      symbol = decode(s, distcode);
      if (symbol < 0 || symbol >= 30) return -10;
      uint32_t dist = dbase[symbol] + bits(s, dext[symbol]);
      if (s->error) return -10;
      if (dist > s->outcnt) return -11;
      if (s->outcnt + len > s->outlen) return 1;
      while (len--) {
        s->out[s->outcnt] = s->out[s->outcnt - dist];
        s->outcnt++;
      }
    }
  } while (symbol != 256);
  return 0;
}

static int fixed(struct state *s) {
  struct huffman lencode, distcode;
  short lengths[FIXLCODES];
  int symbol;
  for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
  for (; symbol < 256; symbol++) lengths[symbol] = 9;
  for (; symbol < 280; symbol++) lengths[symbol] = 7;
  for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
  construct(&lencode, lengths, FIXLCODES);
  for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
  construct(&distcode, lengths, MAXDCODES);
  return codes(s, &lencode, &distcode);
}

static int dynamic(struct state *s) {
  int nlen, ndist, ncode, index, err;
  short lengths[MAXCODES];
  struct huffman lencode, distcode;
  static const short order[19] =
    {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  nlen = bits(s, 5) + 257;
  ndist = bits(s, 5) + 1;
  ncode = bits(s, 4) + 4;
  if (s->error) return -10;
  if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

  for (index = 0; index < ncode; index++) lengths[order[index]] = bits(s, 3);
  for (; index < 19; index++) lengths[order[index]] = 0;
  err = construct(&lencode, lengths, 19);
  if (err != 0) return -4;

  index = 0;
  while (index < nlen + ndist) {
    int symbol, len;
    symbol = decode(s, &lencode);
    if (symbol < 0) return symbol;
    if (symbol < 16) {
      lengths[index++] = symbol;
    } else {
      len = 0;
      if (symbol == 16) {
        if (index == 0) return -5;
        len = lengths[index - 1];
        symbol = 3 + bits(s, 2);
      } else if (symbol == 17) {
        symbol = 3 + bits(s, 3);
      } else {
        symbol = 11 + bits(s, 7);
      }
      if (index + symbol > nlen + ndist) return -6;
      while (symbol--) lengths[index++] = len;
    }
  }
  if (lengths[256] == 0) return -9;

  err = construct(&lencode, lengths, nlen);
  if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
  err = construct(&distcode, lengths + nlen, ndist);
  if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;

  return codes(s, &lencode, &distcode);
}

int inflate_raw(uint8_t *dest, uint32_t destlen,
                const uint8_t *source, uint32_t sourcelen) {
  struct state s;
  int last, type, err;

  s.out = dest;
  s.outlen = destlen;
  s.outcnt = 0;
  s.in = source;
  s.inlen = sourcelen;
  s.incnt = 0;
  s.bitbuf = 0;
  s.bitcnt = 0;
  s.error = 0;

  do {
    last = bits(&s, 1);
    type = bits(&s, 2);
    if (s.error) return -10;
    err = type == 0 ? stored(&s) :
      (type == 1 ? fixed(&s) :
       (type == 2 ? dynamic(&s) : -1));
    if (err != 0) return err < 0 ? err : -20;
  } while (!last);

  return (int)s.outcnt;
}
