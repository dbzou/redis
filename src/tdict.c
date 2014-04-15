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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <sys/time.h>
#include <ctype.h>

#include "adlist.h"
#include "tdict.h"
#include "sds.h"
#include "zmalloc.h"

/*----------------------------------*
 *    INTERNAL TYPES DECLARATIONS   *
 *----------------------------------*/
typedef struct _Symbols {
	short       num_symbols;
	TrieChar    symbols[256];
} Symbols;

static Symbols* symbols_new();
static void symbols_free(Symbols *syms);
static void symbols_add(Symbols *syms, TrieChar c);

#define symbols_num(s)          ((s)->num_symbols)
#define symbols_get(s,i)        ((s)->symbols[i])
#define symbols_add_fast(s,c)   ((s)->symbols[(s)->num_symbols++] = c)

typedef struct stackNode {
	int key;
	struct stackNode *next;
} stackNode;

typedef struct stack {
	stackNode * head;
} stack;
/*------------------------------------*
 *   INTERNAL TYPES IMPLEMENTATIONS   *
 *------------------------------------*/
Symbols * symbols_new()
{
	Symbols *syms = (Symbols *)malloc(sizeof(Symbols));
	if(!syms)
		return NULL;

	syms->num_symbols = 0;
	return syms;
}

void symbols_free(Symbols *syms)
{
	free(syms);
}

void symbols_add(Symbols *syms, TrieChar c)
{
	short lower, upper;

	lower = 0;
	upper = syms->num_symbols;
	while(lower < upper) {
		short middle;
		middle = (lower + upper)/2;
		if(c > syms->symbols[middle])
			lower = middle + 1;
		else if(c < syms->symbols[middle])
			upper = middle;
		else
			return;
	}
	if(lower < syms->num_symbols) {
		memmove(syms->symbols + lower + 1, syms->symbols + lower,
			syms->num_symbols - lower);
	}
	syms->symbols[lower] = c;
	syms->num_symbols++;
}

void stackinit(stack* s)
{
	s->head = NULL;
}

void push(stack* s, int v)
{
	stackNode *t = (stackNode*)zmalloc(sizeof(*t));
	t->key = v;
	t->next = s->head;
	s->head = t;
}

/* check empty before call pop */
int pop(stack* s)
{
	stackNode *t;
	int x;
	if(s->head == NULL)
		return -1;
	t = s->head;
	s->head = t->next;
	x = t->key;
	zfree(t);
	return x;
}

int stackEmpty(stack* s)
{
	return s->head == NULL;
}

/*------------------------------------*
 *   PRIVATE IMPLEMENTATIONS          *
 *------------------------------------*/
void _trieReset(trie *t)
{
	t->base = NULL;
	t->check = NULL;
	t->dasize = 0;
	t->used = 0;
	t->tailsize = 0;
	t->first_free = 0;
	t->tails = NULL;
	t->iterators = 0;
}

int _trieSetup(trie *t)
{
	t->dasize = DA_POOL_BEGIN;
	t->base = (long*)zmalloc(t->dasize*sizeof(long));
	t->check = (long*)zmalloc(t->dasize*sizeof(long));
	if(t->base == NULL || t->check == NULL)
		return TDICT_ERR;
	t->base[0] = DA_SIGNATURE;
	t->check[0] = t->dasize;
	t->base[1] = -1;
	t->check[1] = -1;
	t->base[2] = DA_POOL_BEGIN;
	t->check[2] = 0;
	return TDICT_OK;
}

int _trieInit(trie *t, trieType *type, void *privDataPtr)
{
	_trieReset(t);
	t->type = type;
	t->privdata = privDataPtr;
	t->type->initRange(type);

	return _trieSetup(t);
}

long _trieNextPower(long size)
{
	long i = DA_POOL_BEGIN;

	if(size >= TRIE_INDEX_HALFMAX) return TRIE_INDEX_MAX;
	while(1) {
		if(i > size)
			return i;
		i = i << 1;
	}
}

void _daAssignCell(trie *t, long s)
{
	long   prev, next;

	prev = -trieGetBase(t, s);
	next = -trieGetCheck(t, s);

	/* remove the cell from free list */
	trieSetCheck(t, prev, -next);
	trieSetBase(t, next, -prev);
}

void _daFreeCell(trie *t, long s)
{
	long   i, prev;

	/* find insertion point */
	i = -trieGetCheck(t, DA_POOL_FREE);
	while(i != DA_POOL_FREE && i < s)
		i = -trieGetCheck(t, i);

	prev = -trieGetBase(t, i);

	/* insert cell before i */
	trieSetCheck(t, s, -i);
	trieSetBase(t, s, -prev);
	trieSetCheck(t, prev, -s);
	trieSetBase(t, i, -s);
}

int _daPrepareSpace(trie *t, long s)
{
	if(!trieExpand(t, s) && trieGetCheck(t, s) < 0)
		return 1;
	return 0;
}

int _daHasChildren(trie *t, long s)
{
	long base;
	long c, max_c;

	base = trieGetBase(t, s);
	if(TRIE_INDEX_ERROR == base || base < 0)
		return 0;

	max_c = MIN_VAL(TRIE_CHAR_MAX, TRIE_INDEX_MAX - base);
	max_c = MIN_VAL(t->dasize,max_c);
	for(c = 0; c < max_c; c++) {
		if(trieGetCheck(t, base + c) == s)
			return 1;
	}

	return 0;
}

/**
* @brief Prune the single branch up to given parent
*
* @param d : the double-array structure
* @param p : the parent up to which to be pruned
* @param s : the dangling state to prune off
*
* Prune off a non-separate path up from the final state @a s to the
* given parent @a p. The prunning stop when either the parent @a p
* is met, or a first non-separate node is found.
*/
void _daPrune(trie *t, long p, long s)
{
	while(p != s && _daHasChildren(t, s) == 0) {
		long   parent;

		parent = trieGetCheck(t, s);
		_daFreeCell(t, s);
		s = parent;
	}
}

/* get all children */
Symbols * _daFillSymbols(trie *t, long s)
{
	Symbols *syms;
	long base;
	long c, max_c;

	syms = symbols_new();

	base = trieGetBase(t, s);
	max_c = MIN_VAL(TRIE_CHAR_MAX, TRIE_INDEX_MAX - base);
	max_c = MIN_VAL(max_c, t->dasize);
	for(c = 0; c < max_c; c++) {
		if(trieGetCheck(t, base + c) == s)
			symbols_add_fast(syms, (TrieChar)c);
	}

	return syms;
}

int _daFitSymbols(trie *t, long base, const Symbols  *symbols)
{
	int i;
	for(i = 0; i < symbols_num(symbols); i++) {
		TrieChar sym = symbols_get(symbols, i);

		/* if(base + sym) > TRIE_INDEX_MAX which means it's overflow,
		* or cell [base + sym] is not free, the symbol is not fit.
		*/
		if(base > TRIE_INDEX_MAX - sym || !_daPrepareSpace(t, base + sym))
			return TDICT_ERR;
	}
	return TDICT_OK;
}

long _daFindFreeBase(trie *t, const Symbols *symbols)
{
	TrieChar first_sym;
	long s;

	/* find first free cell that is beyond the first symbol */
	first_sym = symbols_get(symbols, 0);
	s = -trieGetCheck(t, DA_POOL_FREE);
	while(s != DA_POOL_FREE
		&& s < (long) first_sym + DA_POOL_BEGIN)
	{
		s = -trieGetCheck(t, s);
	}
	if(s == DA_POOL_FREE) {
		for(s = first_sym + DA_POOL_BEGIN; ; ++s) {
			if(trieExpand(t, s))
				return TRIE_INDEX_ERROR;
			if(trieGetCheck(t, s) < 0)
				break;
		}
	}

	/* search for next free cell that fits the symbols set */
	while(_daFitSymbols(t, s - first_sym, symbols)) {
		/* extend pool before getting exhausted */
		if(-trieGetCheck(t, s) == DA_POOL_FREE) {
			if(trieExpand(t, t->dasize+1))
				return TRIE_INDEX_ERROR;
		}

		s = -trieGetCheck(t, s);
	}

	return s - first_sym;
}

long _tailAllocCell(trie *t)
{
	long block,newsize,i;

	if(0 != t->first_free) {
		block = t->first_free;
		t->first_free = t->tails[block].next_free;
	} else {
		block = t->tailsize;
		newsize = _trieNextPower(block);
		if(newsize == block)
			return -1;
		trieEntry * newtail = (trieEntry *) zrealloc(t->tails,
			newsize * sizeof(trieEntry));
		if(newtail){
			t->tails = newtail;
			t->tailsize = newsize;

			for(i=block+1; i < newsize-1; i++)
			{
				t->tails[i].next_free = i+1;
			}
			t->tails[newsize-1].next_free = 0;
			t->first_free = block+1;
		}
		else {
			return -1;
		}
	}
	t->tails[block].next_free = -1;
	t->tails[block].suffix = NULL;
	t->tails[block].key = NULL;
	t->tails[block].val = NULL;
	t->used++;
	return block + TAIL_START_BLOCKNO;
}

void _tailFreeCell(trie *t, long block)
{
	//printf("free tail:%ld\n",block);
	long i, j;

	block -= TAIL_START_BLOCKNO;

	if(block >= t->tailsize)
		return;

	if(NULL != t->tails[block].suffix) {
		zfree((TrieChar*)t->tails[block].suffix);
		t->tails[block].suffix = NULL;
	}
	if(t->type->keyDestructor)
		t->type->keyDestructor(t->privdata,t->tails[block].key);
	if(t->type->valDestructor)
		t->type->valDestructor(t->privdata,t->tails[block].val);
	/* find insertion point */
	j = 0;
	for(i = t->first_free; i != 0 && i < block; i = t->tails[i].next_free)
		j = i;

	/* insert free block between j and i */
	t->tails[block].next_free = i;
	if(0 != j)
		t->tails[j].next_free = block;
	else
		t->first_free = block;
	t->used--;
}

//relocate index to fit new inserted node
int _trieReIndex(trie *t, long s, long new_base)
{
	long old_base;
	Symbols *symbols;
	int i;

	old_base = trieGetBase(t, s);
	symbols = _daFillSymbols(t, s);

	for(i = 0; i < symbols_num(symbols); i++) {
		long old_next, new_next, old_next_base;

		old_next = old_base + symbols_get(symbols, i);
		new_next = new_base + symbols_get(symbols, i);
		old_next_base = trieGetBase(t, old_next);

		/* allocate new next node and copy BASE value */
		_daAssignCell(t, new_next);
		trieSetCheck(t, new_next, s);
		trieSetBase(t, new_next, old_next_base);

		/* old_next node is now moved to new_next
		* so, all cells belonging to old_next
		* must be given to new_next
		*/
		/* preventing the case of TAIL pointer */
		if(old_next_base > 0) {
			long c, max_c;

			max_c = MIN_VAL(TRIE_CHAR_MAX, TRIE_INDEX_MAX - old_next_base);
			for(c = 0; c < max_c; c++) {
				if(trieGetCheck(t, old_next_base + c) == old_next)
					trieSetCheck(t, old_next_base + c, new_next);
			}
		}

		/* free old_next node */
		_daFreeCell(t, old_next);
	}

	symbols_free(symbols);

	/* finally, make BASE[s] point to new_base */
	trieSetBase(t, s, new_base);
	return TDICT_OK;
}



/**
* @brief Walk in tail with a character
*
* @param t          : the tail data
* @param s          : the tail data index
* @param suffix_idx : pointer to current character index in suffix
* @param c          : the character to use in walking
*
* @return boolean indicating success
*
* Walk in the tail data @a t at entry @a s, from given character position
* @a *suffix_idx, using given character @a c. If the walk is successful,
* it returns TRUE, and @a *suffix_idx is updated to the next character.
* Otherwise, it returns FALSE, and @a *suffix_idx is left unchanged.
*/
int _trieWalkTail(trie *t, long s, short *suffix_idx, const TrieChar c)
{
	TrieChar *suffix;
	TrieChar        suffix_char;

	suffix = (TrieChar *)trieGetTailSuffix(t, s);

	if(!suffix)
		return TDICT_ERR;

	suffix_char = suffix[*suffix_idx];
	//printf("suffix_char=%c,c=%c\n",suffix_char,c);
	if(suffix_char == c) {
		if(0 != suffix_char)
			++*suffix_idx;
		return TDICT_OK;
	}
	return TDICT_ERR;
}


/**
* @brief Insert a branch from trie node
*
* @param d : the double-array structure
* @param s : the state to add branch to
* @param c : the character for the branch label
*
* @return the index of the new node
*
* Insert a new arc labelled with character @a c from the trie node 
* represented by index @a s in double-array structure @a d.
* Note that it assumes that no such arc exists before inserting.
*/
long _daInsert(trie *t, long s, const TrieChar c)
{
	long   base, next;

	base = trieGetBase(t, s);

	if(base > 0) {
		next = base + c;

		/* if already there, do not actually insert */
		if(trieGetCheck(t, next) == s)
			return next;

		/* if(base + c) > TRIE_INDEX_MAX which means 'next' is overflow,
		* or cell [next] is not free, relocate to a free slot
		*/
		if(base > TRIE_INDEX_MAX - c || !_daPrepareSpace(t, next)) {
			Symbols    *symbols;
			long   new_base;

			/* relocate BASE[s] */
			symbols = _daFillSymbols(t, s);
			symbols_add(symbols, c);
			new_base = _daFindFreeBase(t, symbols);
			symbols_free(symbols);

			if(TRIE_INDEX_ERROR == new_base)
				return TRIE_INDEX_ERROR;

			_trieReIndex(t, s, new_base);
			next = new_base + c;
		}
	} else {
		Symbols    *symbols;
		long   new_base;

		symbols = symbols_new();
		symbols_add(symbols, c);
		new_base = _daFindFreeBase(t, symbols);
		symbols_free(symbols);

		if(TRIE_INDEX_ERROR == new_base)
			return TRIE_INDEX_ERROR;

		trieSetBase(t, s, new_base);
		next = new_base + c;
	}
	_daAssignCell(t, next);
	trieSetCheck(t, next, s);

	return next;
}

int _trieSetTailSuffix(trie *t, long index, const TrieChar *suffix)
{
	index -= TAIL_START_BLOCKNO;
	if(index >=0 && index < t->tailsize) {
		/* suffix and t->tails[index].suffix may overlap;
		* so, dup it before it's overwritten
		*/
		TrieChar *tmp = NULL;
		if(suffix) {
			tmp =(TrieChar*)zstrdup((const char *)suffix);
		}
		if(t->tails[index].suffix)
			zfree((TrieChar*)t->tails[index].suffix);
		t->tails[index].suffix = tmp;

		return TDICT_OK;
	}
	return TDICT_ERR;
}

/**
* @brief Add a new suffix
*
* @param t      : the tail data
* @param suffix : the new suffix
*
* @return the index of the newly added suffix.
*
* Add a new suffix entry to tail.
*/
long _trieAddTailSuffix(trie *t, const TrieChar *suffix)
{
	long   new_block;

	new_block = _tailAllocCell(t);
	_trieSetTailSuffix(t, new_block, suffix);

	return new_block;
}

long _trieInsertInBranch(trie *t, long sep_node, const TrieChar *suffix)
{
	long new_da, new_tail;

	new_da = _daInsert(t, sep_node, *suffix);
	if(TRIE_INDEX_ERROR == new_da)
		return TDICT_ERR;

	if('\0' != *suffix)
		++suffix;

	new_tail = _trieAddTailSuffix(t, suffix);
	//tail_set_data(trie->tail, new_tail, data);
	trieSetTailIndex(t, new_da, new_tail);
	return new_tail;
}

long _trieInsertInTail(trie *t, long sep_node, const TrieChar *suffix)
{
	long       old_tail, old_da, s;
	const TrieChar *old_suffix, *p;

	/* adjust separate point in old path */
	old_tail = trieGetTailIndex(t, sep_node);
	old_suffix = (TrieChar *)trieGetTailSuffix(t, old_tail);
	if(!old_suffix)
		return TDICT_ERR;

	for(p = old_suffix, s = sep_node; *p == *suffix; p++, suffix++) {
		long tt = _daInsert(t, s, *p);
		if(TRIE_INDEX_ERROR == tt)
			goto fail;
		s = tt;
	}

	old_da = _daInsert(t, s, *p);
	if(TRIE_INDEX_ERROR == old_da)
		goto fail;  

	if('\0' != *p) {
		++p;
	}

	_trieSetTailSuffix(t, old_tail, p);
	trieSetTailIndex(t, old_da, old_tail);

	/* insert the new branch at the new separate point */
	return _trieInsertInBranch(t, s, suffix);

fail:
	/* failed, undo previous insertions and return error */
	_daPrune(t, sep_node, s);
	trieSetTailIndex(t, sep_node, old_tail);
	return TDICT_ERR;
}

int _trieWalk(trie *t, long *s, const TrieChar c)
{
	long   next;

	next = trieGetBase(t, *s) + c;
	if(trieGetCheck(t, next) == *s) {
		*s = next;
		return TDICT_OK;
	}
	return TDICT_ERR;
}

int _trieAddKey(trie *t, const void *key)
{
	long        s, index;
	short            suffix_idx;
	TrieChar *p,*sep;

	if(t->base == NULL)
		_trieSetup(t);

	TrieChar * internalKey = t->type->encodingFunction(t->type,key);
	//printf("internalKey=%s\n",(char *)internalKey);
	/* walk through branches */
	s = DA_POOL_ROOT;
	for(p = internalKey; !trieBranchEnd(t, s); p++) {
		if(_trieWalk(t, &s, *p))
		{
			int      res;
			res = _trieInsertInBranch(t, s, p);
			zfree(internalKey);
			return res;
		}
		if(0 == *p)
			break;
	}

	/* walk through tail */
	sep = p;
	index = trieGetTailIndex(t, s);
	suffix_idx = 0;
	for( ; ; p++) {
		if(_trieWalkTail(t, index, &suffix_idx, *p))
		{
			int      res;

			res = _trieInsertInTail(t, s, sep);
			zfree(internalKey);
			return res;
		}
		if(0 == *p)
			break;
	}
	zfree(internalKey);
	return index;
}


/*------------------------------------*
*       API IMPLEMENTATIONS          *
*------------------------------------*/
trie *trieCreate(trieType *type, void *privDataPtr)
{
	trie *t =(trie *)zmalloc(sizeof(*t));
	_trieInit(t,type,privDataPtr);
	return t;
}

int trieExpand(trie *t, long size)
{
	if(size <= 0 || TRIE_INDEX_MAX <= size)
		return TDICT_ERR;
	if(t->dasize >= size)
		return TDICT_OK;//do nothing

	long realsize = _trieNextPower(size);
	t->base = (long *)zrealloc(t->base, realsize*sizeof(long));
	t->check = (long *)zrealloc(t->check, realsize*sizeof(long));

	long new_begin = t->dasize;
	t->dasize = realsize;

	long i,free_tail;
	/* initialize new free list */
	for(i = new_begin; i < realsize-1; i++) {
		trieSetCheck(t, i, -(i + 1));
		trieSetBase(t, i + 1, -i);
	}

	/* merge the new circular list to the old */
	free_tail = -trieGetBase(t, DA_POOL_FREE);
	trieSetCheck(t, free_tail, -new_begin);
	trieSetBase(t, new_begin, -free_tail);
	trieSetCheck(t, realsize-1, -DA_POOL_FREE);
	trieSetBase(t, DA_POOL_FREE, -(realsize-1));

	/* update header cell */
	t->check[0] = t->dasize;
	return TDICT_OK;
}

int trieResize(trie *t)
{
	int minimal = t->used;

	if(minimal < DA_POOL_BEGIN)
		minimal = DA_POOL_BEGIN;

	return trieExpand(t, minimal);
}

int trieAdd(trie *t, void *key, void *val)
{
	long index = _trieAddKey(t,key);//return trieEntry index where set key and value
	//printf("_trieAddKey ret=%d\n",index);

	if(index == TRIE_INDEX_ERROR || index == TDICT_ERR || index == -1)//TODO:整理返回值
		return TDICT_ERR;//add failed

	//set key and value
	trieSetTailKey(t,index,key);//redundance, get entire key fast
	trieSetTailVal(t,index,val);
	//triePrintStats(t);
	return TDICT_OK;
}

trieEntry * trieFind(trie *t, const void *key)
{
	long s;
	short suffix_idx;
	const TrieChar *p;
	TrieChar * internalKey = t->type->encodingFunction(t->type,key);
	//printf("internalKey=%s\n",(char *)internalKey);
	/* walk through branches */
	s = DA_POOL_ROOT;
	for(p = internalKey; !trieBranchEnd(t, s); p++) {
		if(_trieWalk(t,&s,*p))
		{
			zfree(internalKey);
			return NULL;
		}

		if(0 == *p)
			break;
	}

	/* walk through tail */
	s = trieGetTailIndex(t, s);
	suffix_idx = 0;
	for( ; ; p++) {
		if(_trieWalkTail(t,s,&suffix_idx,*p))
		{
			zfree(internalKey);
			return NULL;
		}
		if(0 == *p)
			break;
	}
	zfree(internalKey);
	/* found, get entry and return */
	return trieGetTrieEntry(t,s);
}

trieIterator * getTrieIterator(trie *t, long state)
{
	long   base,s;
	list * result = listCreate();
	stack *nodeStack = (stack *)zmalloc(sizeof(*nodeStack));
	trieIterator *iter = (trieIterator *)zmalloc(sizeof(*iter));
	stackinit(nodeStack);
	//triePrintStats(t);
	push(nodeStack,state);

	while(!stackEmpty(nodeStack))
	{
		s = pop(nodeStack);
		//printf("traverse:%d\n",s);
		base = trieGetBase(t, s);
		if(base < 0)
		{
			listAddNodeTail(result,&t->base[s]);
		}
		else
		{
			Symbols *symbols;
			int      i;
			symbols = _daFillSymbols(t, s);
			for(i = symbols_num(symbols)-1; i >= 0; i--) {
				push(nodeStack, base + symbols_get(symbols, i));
			}
			symbols_free(symbols);
		}
	}

	iter->t = t;
	iter->index = -1;
	iter->cur = NULL;
	iter->safe = 0;
	iter->result = result;
	zfree(nodeStack);//nodeStack already empty
	return iter;
}

trieIterator * triePrefixSearch(trie *t, const void *key)
{
	long        s,ss;
	short            suffix_idx;
	const TrieChar *p;
	int hasWildcard = (strchr((sds)key,'*') != NULL)? 1: 0;
	TrieChar * internalKey = t->type->encodingFunction(t->type,key);
	trieIterator *iter = (trieIterator *)zmalloc(sizeof(*iter));
	iter->cur = NULL;
	iter->t = t;
	iter->safe = 0;
	iter->index = -1;
	iter->result = NULL;

	/* walk through branches */
	s = DA_POOL_ROOT;
	for(p = internalKey; !trieBranchEnd(t, s); p++) {

		if('\0' == *p && hasWildcard == 1)//find node in branches, begin enumerate
		{
			zfree(internalKey);
			zfree(iter);
			return getTrieIterator(t,s);
		}

		if(_trieWalk(t,&s,*p))
		{
			zfree(internalKey);
			return iter;
		}
		if(0 == *p)
			break;
	}

	/* walk through tail */
	ss = trieGetTailIndex(t, s);
	suffix_idx = 0;
	for( ; ; p++) {
		if('\0' == *p && hasWildcard == 1)
		{
			zfree(internalKey);
			zfree(iter);
			return getTrieIterator(t,s);
		}

		if(_trieWalkTail(t,s,&suffix_idx,*p))
		{
			zfree(internalKey);
			return iter;
		}
		if(0 == *p)
			break;
	}

	zfree(internalKey);
	zfree(iter);
	return getTrieIterator(t,s);
}

trieEntry *trieNext(trieIterator *iter)
{
	if(iter->result == NULL)
		return NULL;
	if(iter->index < 0) {
		iter->cur = iter->result->head;
		iter->index = -*(int *)(iter->cur->value);
		return trieGetTrieEntry(iter->t,iter->index);
	}

	if(iter->cur->next != NULL) {
		iter->cur = iter->cur->next;
		iter->index = -*(int *)(iter->cur->value);
		return trieGetTrieEntry(iter->t,iter->index);
	}
	else {
		return NULL;
	}
}

void trieReleaseIterator(trieIterator *iter)
{
	if(iter->result != NULL)
		listRelease(iter->result);
	zfree(iter);
}

int trieDelete(trie *t, const void *key)
{
	//triePrintStats(t);
	long        s,tt;
	short            suffix_idx;
	const TrieChar *p;
	TrieChar * internalKey = t->type->encodingFunction(t->type,key);
	//printf("internalKey=%s\n",(char *)internalKey);
	/* walk through branches */
	s = DA_POOL_ROOT;
	for(p = internalKey; !trieBranchEnd(t, s); p++) {
		if(_trieWalk(t,&s,*p))
		{
			zfree(internalKey);
			return TDICT_ERR;
		}
		if(0 == *p)
			break;
	}

	/* walk through tail */
	tt = trieGetTailIndex(t, s);
	suffix_idx = 0;
	for( ; ; p++) {
		if(_trieWalkTail(t,tt,&suffix_idx,*p))
		{
			zfree(internalKey);
			return TDICT_ERR;
		}
		if(0 == *p)
			break;
	}

	zfree(internalKey);
	/* found, delete */
	_tailFreeCell(t, tt);
	trieSetBase(t, s, TRIE_INDEX_ERROR);
	_daPrune(t, DA_POOL_ROOT, s);
	//triePrintStats(t);
	return TDICT_OK;
}

void trieEmpty(trie *t,void(callback)(void *))
{
	unsigned long i;
	for(i = 0; i < t->tailsize; i++)
	{
		trieEntry *te;
		if(callback &&(i & 65535) == 0) callback(t->privdata);

		te = t->tails+i;
		if(te->suffix != NULL) {
			zfree((TrieChar*)te->suffix);
			if(t->type->keyDestructor)
				t->type->keyDestructor(t->privdata,te->key);
			if(t->type->valDestructor)
				t->type->valDestructor(t->privdata,te->val);
		}
	}

	zfree(t->tails);
	zfree(t->base);
	zfree(t->check);
	_trieReset(t);
}

int trieReplace(trie *t, trieEntry *te, void *val)
{
	if(te == NULL)
		return TDICT_ERR;

	/* Set the new value and free the old one. Note that it is important
	* to do that in this order, as the value may just be exactly the same
	* as the previous one. In this context, think to reference counting,
	* you want to increment(set), and then decrement(free), and not the
	* reverse. */
	void * oldval = te->val;
	te->val = val;
	if(t->type->valDestructor)
		t->type->valDestructor(t->privdata,oldval);

	return TDICT_OK;
}

void triePrintStats(trie *t)
{
	trieType   *tap = t->type;
	keyRange *range;
	printf("AlphaMap:\n");
	for(range = tap->range; range; range = range->next) {
		printf("\tRange begin-end:[%d,%d]\n",(int)range->begin,(int)range->end);
	}
	printf("DArray:\n");
	printf("\tnumbers:%lu\n",t->dasize);
	int i;
	for(i = 0; i < t->dasize; i++) {
		printf("\tbase[%d]=%d,check[%d]=%d\n",i,(int)t->base[i],i,(int)t->check[i]);
	}
	printf("Tail:\n");
	printf("\tused:%lu\n",t->used);
	printf("\tnum_tails:%lu\n",t->tailsize);
	printf("\tfirst_free:%lu\n",t->first_free);
	for(i = 0; i < t->tailsize; i++) {
		int realindex = i+TAIL_START_BLOCKNO;
		TrieChar * suffix = (TrieChar *)trieGetTailSuffix(t,realindex);
		if(suffix) {
			char * orginalsuffix = (char*)t->type->decodingFunction(t->type,suffix);
			printf("\tdata[%d]=%s,key=%s,val=%p\n",i,orginalsuffix,(char*)trieGetTrieEntry(t,realindex)->key,trieGetTrieEntry(t,realindex)->val);
			zfree(orginalsuffix);
		}
		else {
			printf("\tdata[%d]=null\n",i);
		}
	}	
}
