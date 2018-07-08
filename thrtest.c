//
// Copyright 2018 Staysail Systems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use file except in compliance with the License.
// You may obtain a copy of the license at
// 
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// thrtest -- this test your threading platform (POSIX style threads)
// to determine the context switch and synchronization latency overhead.
// You need to have  a working clock_gettime() function. Note that macOS
// prior to 10.12 * does not have this.
//
// The results are somewhat idealized, as we only contend with two threads
// to minimize inconsistency.  Binding this to a specific CPU core that is
// isolated from other operating system tasks will give more consistent
// results, but the results will only be representative of real program
// overheads if the real program is also bound to a core like this, with
// a small number of threads.
//
// On Linux, you need to compile with -lpthread, e.g.:
//
//  $(CC) thrtest.c -l pthread -o thrtest

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(CLOCK_MONOTONIC)
#define	CLK CLOCK_MONOTONIC
#define CLK_NAME "CLOCK_MONOTONIC"
#elif defined(CLOCK_REALTIME)
#define CLK CLOCK_REALTIME
#define CLK_NAME "CLOCK_REALTIME"
#endif

#define NSEC 1000000000

struct timer {
	struct timespec ts;
	struct timespec te;
};

void
timer_start(struct timer *t)
{
	clock_gettime(CLK, &t->ts);
}

void
timer_end(struct timer *t)
{
	clock_gettime(CLK, &t->te);
}

int64_t
timer_nsec(struct timer *t)
{
	int64_t ts, te;

	ts = t->ts.tv_sec;
	ts *= NSEC;
	ts += t->ts.tv_nsec;

	te = t->te.tv_sec;
	te *= NSEC;
	te += t->te.tv_nsec;

	return (te - ts);
}

void
timer_overhead(void)
{
	struct timer timer;
	int count = 1000000;
	struct timespec res;
	int64_t ns = 0;
	int64_t nslo = NSEC;
	int64_t nshi = 0;

	printf("USING CLOCK: %s\n", CLK_NAME);
	clock_getres(CLK, &res);
	printf("CLOCK RESOLUTION: %lu ns\n", res.tv_nsec);

	for (int i = 1; i < count; i++) {
		int64_t nsdiff;
		int64_t ts, te, delta;

		timer_start(&timer);
		timer_end(&timer);
		delta = timer_nsec(&timer);
		if (delta > nshi) {
			nshi = delta;
		}
		if (delta < nslo) {
			nslo = delta;
		}
		ns += delta;
		if ((delta > res.tv_nsec) && (delta > 1000)) {
			printf("OUTLIER SAMPLE %i: %ld ns\n", i, (long)delta);
		}
	}

	ns /= count;
	printf("AVG CLOCK OVERHEAD: %lld ns\n", (long long)ns);
	printf("MIN CLOCK OVERHEAD: %lld ns\n", (long long)nslo);
	printf("MAX CLOCK OVERHEAD: %lld ns\n", (long long)nshi);
}

void
uncontended_lock(void)
{
	pthread_mutex_t lk;
	pthread_mutex_init(&lk, NULL);
	int count = 1000;
	int64_t ns = 0;
	int64_t delta;
	struct timer timer;
	int64_t nslo = NSEC;
	int64_t nshi = 0;

	for (int i = 0; i < count; i++) {
		int64_t ts, te, delta;

		timer_start(&timer);
		pthread_mutex_lock(&lk);
		pthread_mutex_unlock(&lk);
		timer_end(&timer);
		delta = timer_nsec(&timer);
		if (delta > nshi) {
			nshi = delta;
		}
		if (delta < nslo) {
			nslo = delta;
		}
		ns += delta;
	}
	ns /= count;
	pthread_mutex_destroy(&lk);

	printf("UNCONTENDED LOCK/UNLOCK: AVG %ld ns  MIN %ld ns  MAX %ld ns\n", (long) ns, (long) nslo, (long) nshi);
}


int start = 0;
pthread_mutex_t start_lk = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;

pthread_mutex_t contended = PTHREAD_MUTEX_INITIALIZER;

void
wait_start(void)
{
	pthread_mutex_lock(&start_lk);
	while (!start) {
		pthread_cond_wait(&start_cv, &start_lk);
	}
	pthread_mutex_unlock(&start_lk);
}

void
kick_start(void)
{
	pthread_mutex_lock(&start_lk);
	start = 1;
	pthread_cond_broadcast(&start_cv);
	pthread_mutex_unlock(&start_lk);

}

void
reset_start(void)
{
	pthread_mutex_lock(&start_lk);
	start = 0;
	pthread_mutex_unlock(&start_lk);
}

void *
contended_lock_work(void *arg)
{
	int count = 1000000;
	int64_t ns = 0;
	int64_t delta;
	struct timer timer;
	int64_t nslo = NSEC;
	int64_t nshi = 0;

	wait_start();

	for (int i = 0; i < count; i++) {
		int64_t ts, te, delta;

		timer_start(&timer);
		pthread_mutex_lock(&contended);
		pthread_mutex_unlock(&contended);
		timer_end(&timer);
		delta = timer_nsec(&timer);
		if (delta > nshi) {
			nshi = delta;
		}
		if (delta < nslo) {
			nslo = delta;
		}
		ns += delta;
	}
	ns /= count;

	printf("CONTENDED LOCK/UNLOCK: AVG %ld ns  MIN %ld ns  MAX %ld ns\n",
		(long) ns, (long) nslo, (long) nshi);
	return (NULL);
}

void
contended_lock(void)
{
	pthread_t thrs[2];
	void *junk;

	reset_start();
	pthread_create(&thrs[0], NULL, contended_lock_work, NULL);
	pthread_create(&thrs[1], NULL, contended_lock_work, NULL);

	kick_start();

	pthread_join(thrs[0], &junk);
	pthread_join(thrs[1], &junk);
}

void *
lock_handoff_worker(void *arg)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
	static struct timer timer;
	static int wait = -1;
	static int hold = -1;
	int self = (int)(intptr_t)arg;
	int peer = self ? 0 : 1;
	int i;
	int count = 100000;
	int64_t ns = 0;
	int64_t nslo = NSEC;
	int64_t nshi = 0;
	int cnt = 0;
	bool myturn = self == 0 ? true : false;

	wait_start();

	for (int i = 0; (i < count) || myturn; i++) {

		if (myturn) {

			pthread_mutex_lock(&lock);
			pthread_mutex_lock(&rlock);
			hold = self;
			pthread_cond_broadcast(&ready);
			while ((wait != peer) && (i < count)) {
				pthread_cond_wait(&ready, &rlock);
			}
			pthread_mutex_unlock(&rlock);

			timer_start(&timer);
			pthread_mutex_unlock(&lock);
			myturn = false;
		} else {
			pthread_mutex_lock(&rlock);
			wait = self;
			pthread_cond_broadcast(&ready);
			while (hold != peer) {
				pthread_cond_wait(&ready, &rlock);
			}
			pthread_mutex_unlock(&rlock);

			pthread_mutex_lock(&lock);
			timer_end(&timer);
			int64_t delta = timer_nsec(&timer);
			pthread_mutex_unlock(&lock);

			if (i > 0) {
				ns += delta;
				cnt ++;
				if (delta > nshi) {
					nshi = delta;
				}
				if (delta < nslo) {
					nslo = delta;
				}
			}
			myturn = true;
		}
	}

	if (cnt > 0) {
		ns /= cnt;
	}

	// This is the average time to between switch threads as a result
	// of a mutex hand off thread A starts holding the lock, thread B
	// starts blocked waiting for the lock.  time begins when A unlocks,
	// and ends when B acquires.
	printf("MUTEX HANDOFF (CNT %d): AVG %lld ns  MIN %lld ns  MAX %lld ns)\n",
		cnt, (long long)ns, (long long)nslo, (long long)nshi);
	return (NULL);
}

void
lock_handoff(void)
{
	pthread_t thrs[2];
	void *junk;

	reset_start();
	
	pthread_create(&thrs[0], NULL, lock_handoff_worker, (void *)0);
	pthread_create(&thrs[1], NULL, lock_handoff_worker, (void *)1);
	
	kick_start();

	pthread_join(thrs[0], &junk);
	pthread_join(thrs[1], &junk);
}

void *
cv_handoff_worker(void *arg)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
	static struct timer timer;
	static int wake = -1;
	static int wait = -1;

	int self = (int)(intptr_t)arg;
	int peer = self ? 0 : 1;
	int count = 100000;
	int64_t ns = 0;
	int64_t nslo = NSEC;
	int64_t nshi = 0;
	int64_t delta;
	int cnt = 0;
	bool myturn = self == 0 ? true : false;

	wait_start();

	for (int i = 0; (i < count) || myturn; i++) {

		if (myturn) {
			myturn = false;
			pthread_mutex_lock(&lock);
			while ((wait != peer) && (i < count)) {
				pthread_cond_wait(&ready, &lock);
			}
			wake = peer;
			timer_start(&timer);
			pthread_cond_broadcast(&cond);
			pthread_mutex_unlock(&lock);
		} else {
			myturn = true;
			pthread_mutex_lock(&lock);
			wait = self;
			pthread_cond_signal(&ready);
			while (wake != self) {
				pthread_cond_wait(&cond, &lock);
			}
			timer_end(&timer);
			delta = timer_nsec(&timer);
			pthread_mutex_unlock(&lock);

			ns += delta;
			cnt ++;
			if (delta > nshi) {
				nshi = delta;
			}
			if (delta < nslo) {
				nslo = delta;
			}
		}
	}

	if (cnt > 0) {
		ns /= cnt;
	}

	// This is the average time to between switch threads as a result
	// of a mutex hand off thread A starts holding the lock, thread B
	// starts blocked waiting for the lock.  time begins when A unlocks,
	// and ends when B acquires.
	printf("CV HANDOFF (CNT %d): AVG %lld ns  MIN %lld ns  MAX %lld ns)\n",
		cnt, (long long)ns, (long long)nslo, (long long)nshi);
	return (NULL);
}

void
cv_handoff(void)
{
	pthread_t thrs[2];
	void *junk;

	reset_start();
	
	pthread_create(&thrs[0], NULL, cv_handoff_worker, (void *)0);
	pthread_create(&thrs[1], NULL, cv_handoff_worker, (void *)1);
	
	kick_start();

	pthread_join(thrs[0], &junk);
	pthread_join(thrs[1], &junk);
}

int
main(int argc, char **argv)
{
	timer_overhead();

	uncontended_lock();

	contended_lock();

	lock_handoff();

	cv_handoff();
}
