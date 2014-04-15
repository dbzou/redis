/* Trie Tables Implementation.
 *
 * This file implements Trie tables with Double-Array Trie, support insert/del/replace/
 * find/prefix query(tkeys *) operations. Tire tables will auto-resize if needed. It
 * supports both rdb and aof. As the max key range size is 255, it would be better 
 * with ASCII keys, enjoy it :)
 *
 * Copyright (c) 2013-2014, Zou Daobing <daobing.zou at gmail dot com>
 * The Double-Array Trie base on libdatrie wrote by
 *     Theppitak Karoonboonyanan <thep@linux.thai.net>
 *     http://linux.thai.net/~thep/datrie/datrie.html
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __TDICT_H
#define __TDICT_H

#include <stdint.h>

#define TDICT_OK 0
#define TDICT_ERR 1

/* Trie character type for key */
typedef unsigned char TrieChar;

/* Trie terminator character   */
#define TRIE_CHAR_TERM    '\0'
#define TRIE_CHAR_MAX     255

/* Error value for alphabet character */
#define ALPHA_CHAR_ERROR   (~(long)0)
#define TRIE_INDEX_ERROR 0
#define TRIE_INDEX_HALFMAX 0x3fffffff
#define TRIE_INDEX_MAX 0x7fffffff

/* key range entry, the total key range size can not be larger than TRIE_CHAR_MAX */
typedef struct keyRange {
	unsigned long begin;
	unsigned long end;
	struct keyRange *next;
} keyRange;

typedef struct trieEntry {
	void *suffix;
	void *key;			/* store real key */
	void *val;			/* store real val */
	long next_free;		/* next free slot index, -1 when in use */
} trieEntry;

typedef struct trieType {  
	TrieChar *(*encodingFunction)(struct trieType *type, const void *key);			/* translate key to TrieChar */
	void *(*decodingFunction)(struct trieType *type, const TrieChar *internalkey);	/* translate TrieChar back to key */
	void *(*keyDup)(void *privdata, const void *key);
	void *(*valDup)(void *privdata, const void *obj);
	int (*keyCompare)(void *privdata, const void *key1, const void *key2);
	void (*keyDestructor)(void *privdata, void *key);
	void (*valDestructor)(void *privdata, void *obj);
	int (*initRange)(struct trieType *type);										/* init key range */
	keyRange *range;
} trieType;

typedef struct trie {
	long *base;
	long *check;
	unsigned long dasize;			/* dual array size */
	unsigned long used;

	trieEntry *tails;
	unsigned long tailsize;			/* tail slots */
	unsigned long first_free;		/* first free tailEntry index */

	int iterators; 					/* number of iterators currently running */
	trieType *type;
	void *privdata;
} trie;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * trieAdd, trieFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only trieNext()
 * should be called while iterating. */
//TODO: safe iterator implement
typedef struct trieIterator {
	trie *t;
	int index, safe;
	struct listNode *cur;
	struct list *result;
	long long fingerprint; /* unsafe iterator fingerprint for misuse detection */
} trieIterator;

/* DA Header:
 * - Cell 0: SIGNATURE, number of cells
 * - Cell 1: free circular-list pointers
 * - Cell 2: root node
 * - Cell 3: DA pool begin */
#define DA_SIGNATURE 0xDAFCDAFC
#define DA_POOL_FREE 1
#define DA_POOL_ROOT 2
#define DA_POOL_BEGIN 3

#define TAIL_SIGNATURE 0xDFFCDFFC
#define TAIL_START_BLOCKNO  2

/* ------------------------------- Macros ------------------------------------*/

#define MIN_VAL(a,b)  ((a)<(b)?(a):(b))
#define MAX_VAL(a,b)  ((a)>(b)?(a):(b))

#define dictFreeVal(d, entry) \
	if ((d)->type->valDestructor) \
		(d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
	if ((d)->type->valDup) \
		entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
	else \
		entry->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
	do { entry->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
	do { entry->v.u64 = _val_; } while(0)

#define dictFreeKey(d, entry) \
	if ((d)->type->keyDestructor) \
		(d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
	if ((d)->type->keyDup) \
		entry->key = (d)->type->keyDup((d)->privdata, _key_); \
	else \
		entry->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
	(((d)->type->keyCompare) ? \
	(d)->type->keyCompare((d)->privdata, key1, key2) : \
	(key1) == (key2))


#define trieGetKey(te) ((te)->key)
#define trieGetVal(te) ((te)->val)
#define trieSlots(t) ((t)->tailsize)
#define trieSize(t) ((t)->used)

#define trieSetBase(t, i, val) do { \
	if ((i) < (t)->dasize) \
		(t)->base[(i)] = (val); \
} while(0)

#define trieSetCheck(t, i, val) do { \
	if ((i) < (t)->dasize) \
	(t)->check[(i)] = (val); \
} while(0)

#define trieGetBase(t, i) \
	(((i) < (t)->dasize) ? \
	(t)->base[(i)] :\
	TRIE_INDEX_ERROR)

#define trieGetCheck(t, i) \
	(((i) < (t)->dasize) ? \
	(t)->check[(i)] :\
	TRIE_INDEX_ERROR)

#define trieBranchEnd(t,i) \
	(((i) < (t)->dasize) ? \
	(t)->base[(i)] < 0 :\
	TRIE_INDEX_ERROR)

#define trieGetTailIndex(t,i) \
	(((i) < (t)->dasize) ? \
	-((t)->base[(i)]) :\
	TRIE_INDEX_ERROR)

#define trieSetTailIndex(t,i,val) do { \
	if ((i) < (t)->dasize) \
	(t)->base[(i)] = -(val); \
} while(0)

#define trieGetTailSuffix(t, i) \
	((((i) - TAIL_START_BLOCKNO) < (t)->tailsize) ? \
	(t)->tails[(i)-TAIL_START_BLOCKNO].suffix :\
	NULL)
#define trieGetTrieEntry(t, i) \
	((((i) - TAIL_START_BLOCKNO) < (t)->tailsize) ? \
	&((t)->tails[(i)-TAIL_START_BLOCKNO]) :\
	NULL)
#define trieSetTailKey(t, i, key) do { \
	if (((i) - TAIL_START_BLOCKNO) < (t)->tailsize) {\
	    if ((t)->type->keyDup) \
			(t)->type->keyDup((t)->privdata,(t)->tails[(i)-TAIL_START_BLOCKNO].key); \
		else \
			(t)->tails[(i)-TAIL_START_BLOCKNO].key = (key); }\
} while(0)

#define trieSetTailVal(t, i, val) do { \
	if (((i) - TAIL_START_BLOCKNO) < (t)->tailsize) {\
		if ((t)->type->valDup) \
			(t)->type->valDup((t)->privdata,(t)->tails[(i)-TAIL_START_BLOCKNO].val); \
		else \
			(t)->tails[(i)-TAIL_START_BLOCKNO].val = (val); }\
} while(0)

/* API */
trie *trieCreate(trieType *type, void *privDataPtr);
int trieExpand(trie *t, long size);//trie use negative index，let size be signed
int trieAdd(trie *t, void *key, void *val);
int trieReplace(trie *t, trieEntry *te, void *val);
int trieDelete(trie *t, const void *key);
int trieDeleteNoFree(trie *t, const void *key);
void trieRelease(trie *t);
trieEntry * trieFind(trie *t, const void *key);
void *trieFetchValue(trie *t, const void *key);
trieIterator * getTrieIterator(trie *t, long state);
trieEntry *trieNext(trieIterator *iter);
void trieReleaseIterator(trieIterator *iter);
trieIterator * triePrefixSearch(trie *t, const void *key);
//trieEntry *trieGetRandomKey(trie *t);
void triePrintStats(trie *t);
void trieEmpty(trie *t,void(callback)(void*));
void trieEnableResize(void);
void trieDisableResize(void);

#endif /* __TDICT_H */
