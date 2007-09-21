/* Copyright (C) 2003,2004 Andi Kleen, SuSE Labs.
   Manage shared memory policy for numactl. 
   The actual policy is set in numactl itself, this just sets up and maps
   the shared memory segments and dumps them.

   numactl is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; version
   2.

   numactl is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should find a copy of v2 of the GNU General Public License somewhere
   on your Linux system; if not, write to the Free Software Foundation, 
   Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include "numa.h"
#include "numaif.h"
#include "numaint.h"
#include "util.h"
#include "shm.h"

int shmfd = -1;
long shmid = 0;
char *shmptr;
unsigned long long shmlen;
mode_t shmmode = 0600;
unsigned long long shmoffset;
int shmflags; 
static int shm_pagesize;

long huge_page_size(void)
{ 
	size_t len = 0;
	char *line = NULL;
	FILE *f = fopen("/proc/meminfo", "r"); 
	if (f != NULL) {
		while (getdelim(&line, &len, '\n', f) > 0) { 
			int ps;
			if (sscanf(line, "Hugepagesize: %d kB", &ps) == 1) 
				return ps * 1024;
		}
		free(line);
		fclose(f);
	}
	return getpagesize();
}

static void check_region(void)
{
	if (((unsigned long)shmptr % shm_pagesize) || (shmlen % shm_pagesize)) { 
		fprintf(stderr, "numactl: policy region not page aligned\n");
		exit(1);
	} 
}

static key_t sysvkey(char *name)
{
	int fd;
	key_t key = ftok(name, shmid);
	if (key >= 0)
		return key;

	fprintf(stderr, "numactl: Creating shm key file %s mode %04o\n",
		name, shmmode);
	fd = creat(name, shmmode);
	if (fd < 0)
		nerror("cannot create key for shm %s\n", name);	
	key = ftok(name, shmid);
	if (key < 0) 
		nerror("cannot get key for newly created shm key file %s", 
		       name);
	return key; 
}

/* Attach a sysv style shared memory segment. */
void attach_sysvshm(char *name) 
{ 
	struct shmid_ds s;
	key_t key = sysvkey(name);
	
	shmfd = shmget(key, shmlen, shmflags);
	if (shmfd < 0 && errno == ENOENT) { 
		if (shmlen == 0)
			complain(
                     "need a --length to create a sysv shared memory segment");
		fprintf(stderr, 
         "numactl: Creating shared memory segment %s id %ld mode %04o length %.fMB\n",
			name, shmid, shmmode, ((double)shmlen) / (1024*1024) );
		shmfd = shmget(key, shmlen, IPC_CREAT|shmmode|shmflags);
		if (shmfd < 0) 
			nerror("cannot create shared memory segment");
	}

	if (shmlen == 0) { 
		if (shmctl(shmfd, IPC_STAT, &s) < 0) 
			err("shmctl IPC_STAT");
		shmlen = s.shm_segsz;
	}

	shmptr = shmat(shmfd, NULL, SHM_RDONLY);
	if (shmptr == (void*)-1) 
		err("shmat");
	shmptr += shmoffset;

	shm_pagesize = (shmflags & SHM_HUGETLB) ? huge_page_size() : getpagesize();

	check_region();
}

/* Attach a shared memory file. */
void attach_shared(char *name)
{
	struct stat64 st;

	shmfd = open(name, O_RDONLY); 
	if (shmfd < 0) { 
		errno = 0;
		if (shmlen == 0)
		        complain("need a --length to create a shared file");
		shmfd = open(name, O_RDWR|O_CREAT, shmmode);
		if (shmfd < 0) 
			nerror("cannot create file %s", name);
	} 
	if (fstat64(shmfd, &st) < 0) 
		err("shm stat");
	if (shmlen > st.st_size) {
		if (ftruncate64(shmfd, shmlen) < 0) { 
			/* XXX: we could do it by hand, but it would it 
			   would be impossible to apply policy then. 
			   need to fix that in the kernel. */
			perror("ftruncate");
		}
	}

	shm_pagesize = st.st_blksize;

	check_region();
	
	/* RED-PEN For shmlen > address space may need to map in pieces.
	   Left for some poor 32bit soul. */   
	shmptr = mmap64(NULL, shmlen, PROT_READ, MAP_SHARED, shmfd, shmoffset);
	if (shmptr == (char*)-1)
		err("shm mmap");			

} 

static void 
dumppol(unsigned long long start, unsigned long long end, int pol, nodemask_t mask)
{ 
	if (pol == MPOL_DEFAULT)
		return;
	printf("%016Lx-%016Lx: %s ", 
	       shmoffset+start, 
	       shmoffset+end, 
	       policy_name(pol));
	printmask("", &mask);
} 

/* Dump policies in a shared memory segment. */
void dump_shm(void) 
{ 
	nodemask_t nodes, prevnodes;
	int prevpol = -1, pol; 
	unsigned long long c, start; 

	start = 0;
	if (shmlen == 0) {
		printf("nothing to dump\n");
		return;
	}

	for (c = 0; c < shmlen; c += shm_pagesize) { 
		if (get_mempolicy(&pol, nodes.n, NUMA_NUM_NODES+1, c+shmptr, 
				  MPOL_F_ADDR) < 0) 
			err("get_mempolicy on shm");
		if (pol == prevpol) 
			continue;
		if (prevpol != -1)
			dumppol(start, c, prevpol, prevnodes);
		prevnodes = nodes;
		prevpol = pol;
		start = c;
	} 
	dumppol(start, c, prevpol, prevnodes);
} 

static void vwarn(char *ptr, char *fmt, ...) 
{ 
	va_list ap;
	unsigned long off = (unsigned long)ptr - (unsigned long)shmptr;
	va_start(ap,fmt);
	printf("numactl verify %lx(%lx): ",  (unsigned long)ptr, off);
	vprintf(fmt, ap);
	va_end(ap); 
	exitcode = 1;
} 

static unsigned interleave_next(unsigned cur, nodemask_t *mask)
{
	++cur;
	while (!nodemask_isset(mask, cur)) { 		
		cur = (cur+1) % NUMA_NUM_NODES;
	} 
	return cur;
}

/* Verify policy in a shared memory segment */
void verify_shm(int policy, nodemask_t nodes)
{
	char *p; 
	int ilnode, node;
	int pol2;
	nodemask_t nodes2;
	
	if (policy == MPOL_INTERLEAVE) {
		if (get_mempolicy(&ilnode, NULL, 0, shmptr, MPOL_F_ADDR|MPOL_F_NODE) 
		    < 0) 
			err("get_mempolicy");
	} 
	
	for (p = shmptr; p - (char *)shmptr < shmlen; p += shm_pagesize) { 
		if (get_mempolicy(&pol2, nodes2.n, NUMA_NUM_NODES, p, MPOL_F_ADDR)
		    < 0) 
			err("get_mempolicy");
		if (pol2 != policy) { 
			vwarn(p, "wrong policy %s, expected %s\n", 
			      policy_name(pol2), policy_name(policy));
			return;
		}
		if (memcmp(&nodes2, &nodes, sizeof(nodemask_t))) { 
			vwarn(p, "mismatched node mask\n"); 
			printmask("expected", &nodes);
			printmask("real", &nodes2);
		} 

		if (get_mempolicy(&node, NULL, 0, p, MPOL_F_ADDR|MPOL_F_NODE) < 0) 
			err("get_mempolicy");

		switch (policy) { 
		case MPOL_INTERLEAVE: 
			if (node < 0 || !nodemask_isset(&nodes2, node))
				vwarn(p, "interleave node out of range %d\n", node);
			if (node != ilnode) { 
				vwarn(p, "expected interleave node %d, got %d\n",
				     ilnode,node); 
				return;
			}
			ilnode = interleave_next(ilnode, &nodes2); 
			break;
		case MPOL_PREFERRED:
		case MPOL_BIND:
			if (!nodemask_isset(&nodes2, node)) {
				vwarn(p, "unexpected node %d\n", node);
				printmask("expected", &nodes2);
			}	
			break;

		case MPOL_DEFAULT: 
			break; 
			
		}
	} 
		
}