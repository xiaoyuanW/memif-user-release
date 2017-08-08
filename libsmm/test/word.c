/*
 * Consumer functions for word count / indexing, for testing heterogeneous
 * memory.
 *
 * Hashtable idea from Psearchy.
 *
 * For the memif project. Felix Xiaozhu Lin, 2015.
 */

//#define K2_NO_DEBUG 	1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "measure.h"
#include "log-xzl.h"
#include "misc-xzl.h"


/* psearchy uses 256 for 4 cores */
#define NR_CORES	4
static long long maxmem = 16*1024*1024;
/* max size per table */
#define NBYTES   (maxmem/NR_CORES)  // number of bytes per data structure

/* helper --
 * fill a source buffer with text that will be analyzed.
 *
 * read in a file and fill the buffer. if we reach the end of file,
 * start over from the beginning. */
#define INPUT_FILE "input.txt"

void wc_fill_text(char *buf, int size)
{
	int i;
	char c;
	FILE *input = fopen((const char *)INPUT_FILE, "r");
	assert(input);

	for (i = 0; i < size; i++) {
		c = getc(input);
		if (c == EOF) {
			c = ' ';
			fseek(input, 0, SEEK_SET);
		}
		buf[i] = c;
	}

	fclose(input);
}

/* -------  primes ------------------- */

extern int nprimes, primes[];

static int find_prime_recurse(int l, int u, int n)
{
	int i = (u - l) / 2 + l;

	//  printf("xzl:%s:l:%d u:%d\n", __func__, l, u);
	if (l == u || l == i) {
		I("xzl:%s:return prime %d\n", __func__, primes[i+1]);
		return primes[i+1];
	}

	if (primes[i] <= n && n <= primes[i+1])
		return primes[i+1];

	if (primes[i] > n)
		return find_prime_recurse(l, i, n);
	else
		return find_prime_recurse(i, u, n);
}

static int find_prime(int n)
{
	return find_prime_recurse(0, nprimes-1, n);
}

/* ------- hash table ---------------- */

struct Bucket
{
	char *word;
	int count;
};

struct pass0_state
{
	int maxinfo;
	int maxword;
	struct Bucket *table;
	int wordi;
	int maxhash;
	int infoi;
	int maxblocks;
	int blocki;
	int buckets_used;
	int nstrcmp;	/* the collision times */
};

static struct pass0_state states[NR_CORES];

int wc_init(void)
{
	int i;
	struct pass0_state *state;

	int maxhash = NBYTES / sizeof(struct Bucket);
	maxhash = find_prime(maxhash);
	V("maxhash %d", maxhash);

	for (i = 0; i < NR_CORES; i++) {
		state = states + i;

		state->wordi = 0;
		state->maxhash = maxhash;

		state->table = (struct Bucket *)malloc(sizeof(struct Bucket) * maxhash);
		assert(state->table);
		memset(state->table, 0, sizeof(struct Bucket) * state->maxhash);
	}

	return 0;
}

void wc_cleanup(void)
{
	int i;
	struct pass0_state *state;

	for (i = 0; i < NR_CORES; i++) {
		state = states + i;
		if (state->table)
			free(state->table);
	}
	return;
}

static int
lookup(struct pass0_state *ps, char *word)
{
	int i;
	unsigned int h;
	unsigned int k = 0;
	unsigned int o;

	/* xzl: compute hash */
	for(i = 0; word[i]; i++)
		k = (k * 33) + word[i];
	/* xzl: @h: init addr @o: skip in open addressing */
	h = k % ps->maxhash;
	o = 1 + (k % (ps->maxhash - 1));
	for(i = 0; i < ps->maxhash; i++)
	{
		if(ps->table[h].word == 0)
			return(h);
		if(strcmp(ps->table[h].word, word) == 0)
			return(h);
		h += o;
		ps->nstrcmp++;
		if(h >= (unsigned int)ps->maxhash)
			h = h - ps->maxhash;
	}
	E("pedsort: hash table full\n");
	exit(1);
}

#if 1
/* per worker thread */
static void consumer_wc_one(char *buf, int size, struct pass0_state *state)
{
	/* @p points to the start of a word */
	char *p = buf, c;
	char *end = buf + size;
	int len = 0, skip;
	unsigned h; /* index in hashtable */
	struct Bucket *bu;

	V("%08lx -- %08lx", (unsigned long)buf, (unsigned long)end);

	while (1)
	{
		len = 0;
		skip = 0;

		while (1) {
			c = p[len];
			if (isalnum(c)) {
				len ++;
				if (p + len >= end)
					return;
			} else {
				p[len] = '\0'; /* plant a separator */
				if (len == 0) {
					p ++;
					skip ++;
					if (p >= end)
						return;
					continue;
				} else {
					len ++;
					if (p + len >= end)
						return;
				}

				/* we have a word */
				V("read word: %s", p);
				break;
			}
		}

#if 1
		state->infoi++;

		assert(len > 0);

		V("start lookup %s", p);

		h = lookup(state, p);		/* not expensive? */
		bu = &state->table[h];
		if (bu->word == 0) { /* new word */
			bu->word = p;
			state->buckets_used++;
		}

		bu->count += 1;
#endif
		p += len;
	}
}
#endif

void consumer_wc_smp(char *buf, int size)
{
	int i;
	/* Use omp to split the work among N threads */
	#pragma omp parallel for
	for (i = 0; i < NR_CORES; i++)
		consumer_wc_one(buf + i * size / NR_CORES, size / NR_CORES, states + i);
}

void consumer_wc_up(char *buf, int size)
{
	/* split the work among 1 thread */
	consumer_wc_one(buf, size, states);

	I("nstrncmp %d buckets_used %d infoi %d",
			states[0].nstrcmp, states[0].buckets_used, states[0].infoi);
}

#ifdef UNITTEST
#define BUF_SIZE (2 * SZ_1M)
//#define BUF_SIZE (4 * SZ_1K)

int main(int argc, char **argv)
{
	char *buf = malloc(BUF_SIZE);
	assert(buf);

	wc_init();
	wc_fill_text(buf, BUF_SIZE);
	I("fill text okay");

	k2_measure("start");
//	consumer_wc_up(buf, BUF_SIZE);
	consumer_wc_smp(buf, BUF_SIZE);
	k2_measure("consume-end");
	k2_flush_measure_format();

	free(buf);

	return 0;
}
#endif
