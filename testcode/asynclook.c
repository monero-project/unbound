/*
 * testcode/asynclook.c - debug program perform async libunbound queries.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This program shows the results from several background lookups,
 * while printing time in the foreground.
 */

#include "config.h"
#include "libunbound/unbound.h"
#include "util/locks.h"
#include "util/log.h"

/**
 * result list for the lookups
 */
struct lookinfo {
	/** name to look up */
	char* name;
	/** tracking number that can be used to cancel the query */
	int async_id;
	/** error code from libunbound */
	int err;
	/** result from lookup */
	struct ub_val_result* result;
};

/** global variable to see how many queries we have left */
static int num_wait = 0;

/** usage information for asynclook */
void usage(char* argv[])
{
	printf("usage: %s [options] name ...\n", argv[0]);
	printf("names are looked up at the same time, asynchronously.\n");
	printf("	-b : use blocking requests\n");
	printf("	-c : cancel the requests\n");
	printf("	-d : enable debug output\n");
	printf("	-f addr : use addr, forward to that server\n");
	printf("	-h : this help message\n");
	printf("	-r fname : read resolv.conf from fname\n");
	printf("	-t : use a resolver thread instead of forking a process\n");
	printf("	-x : perform extended threaded test\n");
	exit(1);
}

/** print result from lookup nicely */
static void
print_result(struct lookinfo* info)
{
	char buf[100];
	if(info->err) /* error (from libunbound) */
		printf("%s: error %s\n", info->name,
			ub_val_strerror(info->err));
	else if(!info->result)
		printf("%s: cancelled\n", info->name);
	else if(info->result->havedata)
		printf("%s: %s\n", info->name,
			inet_ntop(AF_INET, info->result->data[0],
			buf, (socklen_t)sizeof(buf)));
	else {
		/* there is no data, why that? */
		if(info->result->rcode == 0 /*noerror*/ ||
			info->result->nxdomain)
			printf("%s: no data %s\n", info->name,
			info->result->nxdomain?"(no such host)":
			"(no IP4 address)");
		else	/* some error (from the server) */
			printf("%s: DNS error %d\n", info->name,
				info->result->rcode);
	}
}

/** this is a function of type ub_val_callback_t */
static void 
lookup_is_done(void* mydata, int err, struct ub_val_result* result)
{
	/* cast mydata back to the correct type */
	struct lookinfo* info = (struct lookinfo*)mydata;
	fprintf(stderr, "name %s resolved\n", info->name);
	info->err = err;
	info->result = result;
	/* one less to wait for */
	num_wait--;
}

/** check error, if bad, exit with error message */
static void 
checkerr(const char* desc, int err)
{
	if(err != 0) {
		printf("%s error: %s\n", desc, ub_val_strerror(err));
		exit(1);
	}
}

/** number of threads to make in extended test */
#define NUMTHR 10

/** struct for extended thread info */
struct ext_thr_info {
	/** thread num for debug */
	int thread_num;
	/** thread id */
	ub_thread_t tid;
	/** context */
	struct ub_val_ctx* ctx;
	/** size of array to query */
	int argc;
	/** array of names to query */
	char** argv;
	/** number of queries to do */
	int numq;
};

/** extended bg result callback, this function is ub_val_callback_t */
static void 
ext_callback(void* mydata, int err, struct ub_val_result* result)
{
	int* my_id = (int*)mydata;
	int doprint = 0;
	if(my_id) {
		/* I have an id, make sure we are not cancelled */
		if(*my_id == 0) {
			printf("error: query returned, but was cancelled\n");
			exit(1);
		}
		if(doprint) 
			printf("cb %d: ", *my_id);
	}
	checkerr("ext_callback", err);
	log_assert(result);
	if(doprint) {
		struct lookinfo pi;
		pi.name = result?result->qname:"noname";
		pi.result = result;
		pi.err = 0;
		print_result(&pi);
	}
	ub_val_result_free(result);
}

/** extended thread worker */
static void*
ext_thread(void* arg)
{
	struct ext_thr_info* inf = (struct ext_thr_info*)arg;
	int i, r;
	struct ub_val_result* result;
	int* async_ids = NULL;
	log_thread_set(&inf->thread_num);
	if(inf->thread_num > NUMTHR*2/3) {
		async_ids = (int*)calloc((size_t)inf->numq, sizeof(int));
		if(!async_ids) {
			printf("out of memory\n");
			exit(1);
		}
	}
	for(i=0; i<inf->numq; i++) {
		if(async_ids) {
			r = ub_val_resolve_async(inf->ctx, 
				inf->argv[i%inf->argc], LDNS_RR_TYPE_A, 
				LDNS_RR_CLASS_IN, &async_ids[i], ext_callback, 
				&async_ids[i]);
			checkerr("ub_val_resolve_async", r);
			if(i > 100) {
				r = ub_val_cancel(inf->ctx, async_ids[i-100]);
				checkerr("ub_val_cancel", r);
				async_ids[i-100]=0;
			}
		} else if(inf->thread_num > NUMTHR/2) {
			/* async */
			r = ub_val_resolve_async(inf->ctx, 
				inf->argv[i%inf->argc], LDNS_RR_TYPE_A, 
				LDNS_RR_CLASS_IN, NULL, ext_callback, NULL);
			checkerr("ub_val_resolve_async", r);
		} else  {
			/* blocking */
			r = ub_val_resolve(inf->ctx, inf->argv[i%inf->argc], 
				LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, &result);
			checkerr("ub_val_resolve", r);
		}
	}
	if(inf->thread_num > NUMTHR/2) {
		r = ub_val_wait(inf->ctx);
		checkerr("ub_val_ctx_wait", r);
	}
	free(async_ids);
	
	return NULL;
}

/** perform extended threaded test */
static int
ext_test(struct ub_val_ctx* ctx, int argc, char** argv)
{
	struct ext_thr_info inf[NUMTHR];
	int i;
	printf("extended test start (%d threads)\n", NUMTHR);
	for(i=0; i<NUMTHR; i++) {
		/* 0 = this, 1 = library bg worker */
		inf[i].thread_num = i+2;
		inf[i].ctx = ctx;
		inf[i].argc = argc;
		inf[i].argv = argv;
		inf[i].numq = 1000;
		ub_thread_create(&inf[i].tid, ext_thread, &inf[i]);
	}
	/* the work happens here */
	for(i=0; i<NUMTHR; i++) {
		ub_thread_join(inf[i].tid);
	}
	printf("extended test end\n");
	ub_val_ctx_delete(ctx);
	checklock_stop();
	return 0;
}

/** getopt global, in case header files fail to declare it. */
extern int optind;
/** getopt global, in case header files fail to declare it. */
extern char* optarg;

/** main program for asynclook */
int main(int argc, char** argv) 
{
	int c;
	struct ub_val_ctx* ctx;
	struct lookinfo* lookups;
	int i, r, cancel=0, blocking=0, ext=0;

	/* lock debug start (if any) */
	checklock_start();

	/* create context */
	ctx = ub_val_ctx_create();
	if(!ctx) {
		printf("could not create context, %s\n", strerror(errno));
		return 1;
	}

	/* command line options */
	if(argc == 1) {
		usage(argv);
	}
	while( (c=getopt(argc, argv, "bcdf:hr:tx")) != -1) {
		switch(c) {
			case 'd':
				r = ub_val_ctx_debuglevel(ctx, 3);
				checkerr("ub_val_ctx_debuglevel", r);
				break;
			case 't':
				r = ub_val_ctx_async(ctx, 1);
				checkerr("ub_val_ctx_async", r);
				break;
			case 'c':
				cancel = 1;
				break;
			case 'b':
				blocking = 1;
				break;
			case 'r':
				r = ub_val_ctx_resolvconf(ctx, optarg);
				if(r != 0) {
					printf("ub_val_ctx_resolvconf "
						"error: %s : %s\n",
						ub_val_strerror(r), 
						strerror(errno));
					return 1;
				}
				break;
			case 'f':
				r = ub_val_ctx_set_fwd(ctx, optarg);
				checkerr("ub_val_ctx_set_fwd", r);
				break;
			case 'x':
				ext = 1;
				break;
			case 'h':
			case '?':
			default:
				usage(argv);
		}
	}
	argc -= optind;
	argv += optind;

	if(ext)
		return ext_test(ctx, argc, argv);

	/* allocate array for results. */
	lookups = (struct lookinfo*)calloc((size_t)argc, 
		sizeof(struct lookinfo));
	if(!lookups) {
		printf("out of memory\n");
		return 1;
	}

	/* perform asyncronous calls */
	num_wait = argc;
	for(i=0; i<argc; i++) {
		lookups[i].name = argv[i];
		if(blocking) {
			fprintf(stderr, "lookup %s\n", argv[i]);
			r = ub_val_resolve(ctx, argv[i], LDNS_RR_TYPE_A,
				LDNS_RR_CLASS_IN, &lookups[i].result);
			checkerr("ub_val_resolve", r);
		} else {
			fprintf(stderr, "start async lookup %s\n", argv[i]);
			r = ub_val_resolve_async(ctx, argv[i], LDNS_RR_TYPE_A,
				LDNS_RR_CLASS_IN, &lookups[i], &lookup_is_done, 
				&lookups[i].async_id);
			checkerr("ub_val_resolve_async", r);
		}
	}
	if(blocking)
		num_wait = 0;
	else if(cancel) {
		for(i=0; i<argc; i++) {
			fprintf(stderr, "cancel %s\n", argv[i]);
			r = ub_val_cancel(ctx, lookups[i].async_id);
			checkerr("ub_val_cancel", r);
		}
		num_wait = 0;
	}

	/* wait while the hostnames are looked up. Do something useful here */
	if(num_wait > 0)
	    for(i=0; i<1000; i++) {
		usleep(100000);
		fprintf(stderr, "%g seconds passed\n", 0.1*(double)i);
		r = ub_val_process(ctx);
		checkerr("ub_val_process", r);
		if(num_wait == 0)
			break;
	}
	if(i>=999) {
		printf("timed out\n");
		return 0;
	}
	printf("lookup complete\n");

	/* print lookup results */
	for(i=0; i<argc; i++) {
		print_result(&lookups[i]);
		ub_val_result_free(lookups[i].result);
	}

	ub_val_ctx_delete(ctx);
	free(lookups);
	checklock_stop();
	return 0;
}
