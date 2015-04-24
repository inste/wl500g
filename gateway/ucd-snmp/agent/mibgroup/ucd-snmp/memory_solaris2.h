/*
 *  memory quantity mib groups
 *
 */
#ifndef _MIBGROUP_MEMORY_H
#define _MIBGROUP_MEMORY_H

#include "mibdefs.h"

int getswap(int);
extern FindVarMethod var_extensible_mem;

void init_memory_solaris2(void);

/* config file parsing routines */
void memory_parse_config(char *, char *);
void memory_free_config(void);
long getTotalSwap(void);
long getFreeSwap(void);
long getTotalFree(void);

#define MAXSTRSIZE 80

#define MEMTOTALSWAP 3
#define MEMAVAILSWAP 4
#define MEMTOTALREAL 5
#define MEMAVAILREAL 6
#define MEMTOTALSWAPTXT 7
#define MEMUSEDSWAPTXT 8
#define MEMTOTALREALTXT 9
#define MEMUSEDREALTXT 10
#define MEMTOTALFREE 11
#define MEMSWAPMINIMUM 12
#define MEMSHARED 13
#define MEMBUFFER 14
#define MEMCACHED 15
#define MEMSWAPERROR 16

#endif /* _MIBGROUP_MEMORY_H */
