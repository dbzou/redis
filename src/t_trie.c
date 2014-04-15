#include "redis.h"

/* trie set commands
 * set key type to REDIS_TRIE and do not need to try encoding key */
void tsetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	setCommand(c);
}

void tsetnxCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	setnxCommand(c);
}

void tsetexCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	setexCommand(c);
}

void ptsetexCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	psetexCommand(c);
}

void tgetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	getCommand(c);
}

void tgetsetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	getsetCommand(c);
}

void tdelCommand(redisClient *c)
{
	int deleted = 0, j;
	for (j = 1; j < c->argc; j++) {
		c->argv[j]->notused = REDIS_TRIE_FLAG;//indicate db find in trie
		if (dbDelete(c->db,c->argv[j])) {
			signalModifiedKey(c->db,c->argv[j]);
			notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
				"del",c->argv[j],c->db->id);
			server.dirty++;
			deleted++;
		}
	}
	addReplyLongLong(c,deleted);
}

void texistsCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	existsCommand(c);
}

void thsetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hsetCommand(c);
}

void thsetnxCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hsetnxCommand(c);
}

void thmsetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hmsetCommand(c);
}
void thincrbyCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hincrbyCommand(c);
}

void thincrbyfloatCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hincrbyfloatCommand(c);
}

void thgetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hgetCommand(c);
}

void thmgetCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hmgetCommand(c);
}

void thdelCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hdelCommand(c);
}
void thlenCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hlenCommand(c);
}

void thexistsCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hexistsCommand(c);
}

void thkeysCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hkeysCommand(c);
}

void thvalsCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hvalsCommand(c);
}

void thgetallCommand(redisClient *c)
{
	c->argv[1]->notused = REDIS_TRIE_FLAG;
	hgetallCommand(c);
}
