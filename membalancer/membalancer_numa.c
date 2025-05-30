/*
 * membalancer_numa.c - Automatic NUMA memory balancer Based on IBS sampler
 *
 * Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <numa.h>
#include <stdbool.h>
#include <sys/param.h>
#include <stdatomic.h>

#include "memory_profiler_arch.h"
#include "memory_profiler_common.h"
#include "thread_pool.h"
#include "membalancer_utils.h"
#include "membalancer_numa.h"

static atomic_int pending_freemem_cal;
static atomic_int error_status;  /* thread error status */

int max_nodes;
struct numa_node_mem numa_table[MAX_NUMA_NODES];
struct numa_node_cpu numa_node_cpu[MAX_NUMA_NODES];
int numa_cpu[MAX_CPU_CORES];

static unsigned long code_overall_samples[MAX_NUMA_NODES];
static unsigned long data_overall_samples[MAX_NUMA_NODES];

#define NUMA_NODE_INFO "/sys/devices/system/node"

static int init_numa_node(void **fpout)
{
	FILE *fp;
	char buffer[1024];
	int i;

	fp = popen("/usr/bin/cat /proc/zoneinfo|/usr/bin/grep start_pfn", "r");
	if (!fp)
		return -errno;
	i = 0;
	while (fgets(buffer, sizeof(buffer) - 1, fp)) {
		if (i++ >= 1)
			break;

	}

	*fpout = fp;

	return 0;
}

static void deinit_numa_node(void *handle)
{
	FILE *fp = handle;

	pclose(fp);
}

static int get_next_numa_distance(char **distance)
{
	int i, val;
	char *dist = *distance;

	i = 0;
	while (dist[i] != 0 && dist[i] != ' ')
		i++;

	if (dist[i] == ' ')
		*distance = &dist[++i];
	else
		*distance = NULL;

	val  = atoi(&dist[0]);

	return  val;
}

static int fill_numa_distances_for_node(int node)
{
	int fd, i, val;
	char path[1024];
	char buffer[1024];
	ssize_t bytes;
	char *bufferp;

	snprintf(path, sizeof(path), "%s/node%d/distance",
			NUMA_NODE_INFO, node);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	bytes = read(fd,  buffer, sizeof(buffer));
	close(fd);

	bufferp = buffer;
	if (bytes <= 0)
		return -1;
	i = 0;
	while (bufferp) {
		val = get_next_numa_distance(&bufferp);
		numa_table[node].distance[i++] = val;
	}

	return 0;
}

static unsigned long numa_node_next(void *handle)
{
	FILE *fp;
	char buffer[1024];
	char *value;

	fp = handle;
	if (!fgets(buffer, 1024, fp))
		return (unsigned long)-1;

	value = strchr(buffer, ':');
	value++;

	while (*value != 0 && *value == ' ')
		value++;

	if (*value == 0)
		return (unsigned long)-1;

	return atol(value);
}

static int fill_numa_distances(void)
{
	int i, err;

	for (i = 0; i < max_nodes; i++) {
		err = fill_numa_distances_for_node(i);
		if (err)
			return err;
	}

	return 0;
}


int fill_numa_table(void)
{
	void *handle;
	int err;
	int i, j, node = 0;
	unsigned long value, last_pfn;

	err = init_numa_node(&handle);
	if (err)
		return err;

	while ((value = numa_node_next(handle)) != -1) {
		numa_table[max_nodes].node = node++;
		numa_table[max_nodes].tierno = -1;
		numa_table[max_nodes].first_pfn = value;
		last_pfn  = value;
		last_pfn += (numa_node_size(max_nodes, NULL) / PAGE_SIZE);
		numa_table[max_nodes++].last_pfn = last_pfn;
		assert(last_pfn > numa_table[max_nodes].first_pfn);

	}

	deinit_numa_node(handle);
	fill_numa_distances();
	for (i = 0; i < max_nodes; i++) {
		printf("numa.%d ", i);
		for (j = 0; j < max_nodes; j++)
			printf("%d ", numa_table[i].distance[j]);

		printf("\n");
	}

	if (verbose > 3) {
		for (i = 0; i < max_nodes; i++)
			printf("NUMA %i 0x%lx-0x%lx 0x%lu\n", i,
					numa_table[i].first_pfn * PAGE_SIZE,
					numa_table[i].last_pfn * PAGE_SIZE,
					PAGE_SIZE * (numa_table[i].last_pfn -
						numa_table[i].first_pfn + 1));

	}

	return 0;
}

int get_current_node(unsigned long physaddr)
{
	int i;

	if (physaddr == (unsigned long)-1)
		return -1;

	physaddr >>= PAGE_SHIFT;

	for (i = max_nodes - 1; i > -1; i--) {
		if ((physaddr >= numa_table[i].first_pfn) &&
				(physaddr <= numa_table[i].last_pfn))
			return i;
	}

	return -1;
}

static int calcuate_weight(int node, unsigned int *counts, int numa_count)
{
	int weight, i;

	weight = 0;
	for (i = 0; i < numa_count; i++)
		weight += numa_table[node].distance[i] * counts[i];

	return weight;
}

int get_target_node_numa(int node, unsigned long count, unsigned int *counts,
		int numa_count, unsigned long total_samples)
{
	int next_node;
	int weight, weight_current, weight_min;
	int i, minfree_pct;
	unsigned long ccount = 0;

	next_node = 0;
	minfree_pct = freemem_threshold();

	weight_current = calcuate_weight(node, counts, numa_count);
	weight_min = weight_current;
	for (i = 0; i < numa_count; i++) {
		ccount += counts[i];
		if (i == node)
			continue;

		weight = calcuate_weight(i, counts, numa_count);
		if ((weight + (weight / 10)) < weight_min) {
			if (node_freemem_get(i) >= minfree_pct) {
				weight_min = weight;
				next_node = i;
			}
		}
	}

	if ((weight_min + (weight_min / 10))<= weight_current)
		return next_node;

	return node;
}

void update_sample_statistics_numa(unsigned long *samples, bool code)
{
	int i;

	if (code) {
		for (i = 0; i < max_nodes; i++)
			code_overall_samples[i] = samples[i];
	} else {
		for (i = 0; i < max_nodes; i++)
			data_overall_samples[i] = samples[i];
	}
}

void get_sample_statistics_numa(bool code, unsigned long **samples, int *count)
{
 	*count   = max_nodes;
	*samples = (code) ? code_overall_samples : data_overall_samples;
}

void process_code_samples_numa(struct bst_node **rootpp,
			       unsigned long total,
			       bool balancer_mode,
			       bool user_space_only)
{
	int i, j, k, node, target_node;
	unsigned long count;
	bool hdr = true;

	if (!total)
		return;

        for (node = 0; node < max_nodes; node++) {
                k = (code_samples_cnt[node] * MIN_PCT) / 100;
                for (i = code_samples_cnt[node] - 1; i > -1; i--) {
                        float pct;

                        if (!code_samples[node][i].count)
                                continue;

                        if (--k < 0)  {
                                code_samples[node][i].count = 0;
                                continue;
                        }

                        count = 0;
                        for (j = 0; j < max_nodes; j++)
                                count += code_samples[node][i].counts[j];

                        pct  = (float)code_samples[node][i].count * 100;
                        pct /= total;

                        if ((!l3miss && (pct < MIN_PCT)) || count < MIN_CNT) {
                                code_samples[node][i].count = 0;
                                continue;
                        }

                        if (user_space_only &&
                                IBS_KERN_SAMPLE(
                                        code_samples[node][i].ip)) {

                                code_samples[node][i].count = 0;
                                continue;
                        }

                        if (balancer_mode) {
                                target_node = get_target_node_numa(node,
                                                code_samples[node][i].count,
                                                code_samples[node][i].counts,
                                                max_nodes, total);

                        } else {
                                target_node = node;
                        }


                        if (node != target_node) {
                                bst_add_page(code_samples[node][i].tgid,
                                        target_node,
                                        code_samples[node][i].vaddr,
					true,
                                        rootpp);
                        }

                        code_samples[node][i].count = 0;

                }

        }
}

void process_data_samples_numa(struct bst_node **rootpp,
			       unsigned long total,
			       bool balancer_mode,
			       bool user_space_only)
{
	int i, j, k, node, target_node;
	unsigned long count;
	bool hdr = true;

	for (node = 0; node < max_nodes; node++) {
		k = (data_samples_cnt[node] * MIN_PCT) / 100;
		for (i = data_samples_cnt[node] - 1; i > -1; i--) {
			float pct;

			if (!data_samples[node][i].count)
				continue;
			if (--k < 0)  {
				data_samples[node][i].count = 0;
				continue;
			}

			count = 0;
			for (j = 0; j < max_nodes; j++)
				count += data_samples[node][i].counts[j];

			pct  = (float)data_samples[node][i].count * 100;
			pct /= total;

			if ((!l3miss && (pct < MIN_PCT)) || count < MIN_CNT) {
				data_samples[node][i].count = 0;
				continue;
			}

			if (!user_space_only &&
					IBS_KERN_SAMPLE(
						data_samples[node][i].ip)) {
				data_samples[node][i].count = 0;
				continue;
			}

			if (balancer_mode) {
				target_node = get_target_node_numa(node,
						data_samples[node][i].count,
						data_samples[node][i].counts,
						max_nodes, total);
			} else {
				target_node = node;
			}


			if (node != target_node) {
				bst_add_page(data_samples[node][i].tgid,
						target_node,
						data_samples[node][i].vaddr,
						true,
						rootpp);
			}

			data_samples[node][i].count = 0;
		}
	}
}

int numa_range_get(int idx, struct numa_range *range)
{
	if (idx >= max_nodes)
		return -1;

	range->first_pfn = numa_table[idx].first_pfn;
	range->last_pfn  = numa_table[idx].last_pfn;
	range->node      = numa_table[idx].node;
	range->tier      = numa_table[idx].tierno;

	return 0;
}

static int get_number(const char *buffer,
		      const char *key,
		      const char *unit,
		      int size,
		      int *offset,
		      unsigned  long *number)
{
	int off = *offset, len;
	char *next, *where = NULL;
	unsigned long num = 0;

	len = strlen(key);
	if (size - off < len)
		return -EINVAL;

	next = strstr(&buffer[off], key);
	if (next == NULL)
		return -EINVAL;

	off += next - &buffer[off];
	off += len;
	next = next + len;

	while(*next == ' ' || *next == ':') {
		if (size - off <= 0)
			return -EINVAL;
		next++;
		off++;
		if (size - off <= 0)
			return -EINVAL;
	}

	while (*next >= '0' && *next <='9') {
		if (size - off <= 0)
			return -EINVAL;
		if (where == NULL)
			where = next;
		next++;
		off++;
	}

	if (!where)
		return -EINVAL;

	*next = 0;
	num = atol(where);

	off++;
	next++;

	len = strlen(unit);
	if (size - off < len)
		return -EINVAL;

	off += len;

	next = strstr(&buffer[off], unit);
	if (next == NULL)
		return -EINVAL;

	*number = num * 1024;
	*offset = off;

	return 0;
}

static int get_freemem_pct(const char *buffer, int size)
{
	char *next;
	unsigned long total_mem, free_mem, pct;
	int offset = 0, err;


	err = get_number(buffer, "MemTotal", "kB", size, &offset, &total_mem);
	if (err)
		return err;

	err = get_number(buffer, "MemFree", "kB", size, &offset, &free_mem);
	if (err)
		return err;

	if (total_mem == 0 || total_mem < free_mem)
		return  -EINVAL;

	pct = (free_mem * 100 / total_mem);

	return (int)pct;
}

static int node_freemem_load(int node)
{
	int fd, bytes;
	char filename[PATH_MAX];
	char buffer[PAGE_SIZE];

	snprintf(filename, PATH_MAX, "/sys/devices/system/node/node%d/meminfo",
		 node);

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -errno;

	bytes = read(fd, buffer, PAGE_SIZE);
	close(fd);

	if (bytes < 0)
		return -EINVAL;

	return get_freemem_pct(buffer, PAGE_SIZE);
}

void  update_per_node_freemem(void *arg)
{
	int node, result;

	atomic_init(&pending_freemem_cal, 1);
	atomic_init(&error_status, 0);

	for (node = 0; node < max_nodes; node++) {
		result =  node_freemem_load(node);
		if (result < 0) {
			numa_table[node].freemem_pct = 0;
			atomic_store(&error_status, 1);
			return;
		}
		assert(result >= 0 && result <= 100);

		numa_table[node].freemem_pct = result;
	}

	atomic_store(&pending_freemem_cal, 0);
}
/*
 * Wait for thread to update node free memory
 * if update is pending, return invalid value
 * on update failure by thread.
 */
int node_freemem_get(int node)
{
	if (node >= max_nodes)
		return -EINVAL;

	if(atomic_load(&pending_freemem_cal) &&
			!atomic_load(&error_status)) {
		usleep(100 * 1000);
		if (atomic_load(&pending_freemem_cal) ||
				atomic_load(&error_status))
			return -EINVAL;
	}
	return numa_table[node].freemem_pct;
}
