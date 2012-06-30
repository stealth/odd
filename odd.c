/* (C) 2011-2012 Sebastian Krahmer all rights reserved.
 *
 * Optimized dd, to speed up backups etc.
 *
 * For non-commercial use, you can use it under the GPL.
 *
 * Any use of this code or GPL-derived code for commercial purposes must ask
 * for prior written permission. This includes, but is not limited to use
 * by forensic labs or companies, use during computer administration or backups
 * at labs/instituts/government or companies (e.g. anything that is your 'job' or
 * is done to make money).
 *
 */
#define _GNU_SOURCE

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <asm/unistd.h>
#include <sys/sendfile.h>


#ifdef ANDROID

#ifndef __NR_splice
#define __NR_splice (__NR_SYSCALL_BASE+340)
#endif
#ifndef SPLICE_F_MORE
#define SPLICE_F_MORE 0x04
#endif

#ifndef POSIX_FADV_SEQUENTIAL
#define POSIX_FADV_SEQUENTIAL 2
#endif
#ifndef POSIX_FADV_DONTNEED
#define POSIX_FADV_DONTNEED 4
#endif
#ifndef POSIX_FADV_NOREUSE
#define POSIX_FADV_NOREUSE 5
#endif


static inline int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
	return syscall(__NR_arm_fadvise64_64, fd, offset, len, advice);
}


static inline int splice(int fd_in, loff_t *off_in, int fd_out,
                         loff_t *off_out, size_t len, unsigned int fl)
{
	return syscall(__NR_splice, fd_in, off_in, fd_out, off_out, len, fl);
}

#endif


struct dd_config {
	const char *in, *out;
	uint64_t skip, seek, count, b_in, b_out, rec_in, rec_out, cores, mmap, sf;
	off_t fsize;
	blksize_t bs;
	char quiet, nosync, direct;
	int saved_errno;
	time_t t_start, t_end;
};

int sigint = 0;


size_t free_mem()
{
	uint64_t n = 0;
	char buf[1024], found = 0;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return 1024*1024;
	memset(buf, 0, sizeof(buf));

	for (;!feof(f);) {
		fgets(buf, sizeof(buf), f);
		if (strstr(buf, "MemFree:")) {
			found = 1;
			break;
		}
	}
	fclose(f);
	if (!found)
		return 1024*1024;
	n = strtoul(buf + 9, NULL, 10);

	if (!n)
		return 1024*1024;

	/* kB? */
	if (strchr(buf + 9, 'k'))
		n <<= 10;
	else if (strchr(buf + 9, 'M'))
		n <<= 20;
	return n/2;
}


int prepare_copy(struct dd_config *ddc, int *ifd, int *ofd)
{
	struct stat st;
	int fli = O_RDONLY|O_LARGEFILE|O_NOCTTY, flo = O_WRONLY|O_LARGEFILE|O_NOATIME|O_NOCTTY;
	uid_t euid = 0;

	if (ddc->mmap)
		flo = O_RDWR|O_LARGEFILE|O_NOATIME|O_NOCTTY;
	if (ddc->direct) {
		fli |= O_DIRECT;
		flo |= O_DIRECT;
	}

	if (stat(ddc->in, &st) < 0) {
		ddc->saved_errno = errno;
		return -1;
	}

	euid = geteuid();

	if (!euid || st.st_uid == euid)
		fli |= O_NOATIME;

	if ((*ifd = open(ddc->in, fli)) < 0) {
		ddc->saved_errno = errno;
		return -1;
	}

	ddc->fsize = st.st_size;

	/* If "bsize" is not given, use optimum of both FS' */
	if (!ddc->bs) {
		struct statfs fst;
		memset(&fst, 0, sizeof(fst));
		if (statfs(ddc->out, &fst) < 0 || fst.f_bsize == 0)
			fst.f_bsize = 0x1000;
		if (fst.f_bsize > st.st_blksize)
			ddc->bs = fst.f_bsize;
		else
			ddc->bs = st.st_blksize;
		if (ddc->bs == 0)
			ddc->bs = 0x1000;
	}


	/* check if device or regular file */
	if (!S_ISREG(st.st_mode)) {
		if (S_ISBLK(st.st_mode)) {
			if (ioctl(*ifd, BLKGETSIZE64, &ddc->fsize) < 0) {
				ddc->saved_errno = errno;
				close(*ifd);
				return -1;
			}
		} else {
			ddc->fsize = (off_t)-1;
			if (ddc->count)
				ddc->fsize = ddc->count*ddc->bs;
		}
	}

	/* skip and seek are in block items */
	ddc->skip *= ddc->bs;
	ddc->seek *= ddc->bs;

	/* skip more bytes than are inside source file? */
	if (ddc->fsize != (off_t)-1 && ddc->skip >= ddc->fsize) {
		ddc->saved_errno = EINVAL;
		close(*ifd);
		return -1;
	}


	if (!ddc->seek)
		flo |= O_CREAT|O_TRUNC;

	if ((*ofd = open(ddc->out, flo, st.st_mode)) < 0) {
		ddc->saved_errno = errno;
		close(*ifd);
		return -1;
	}

	lseek(*ifd, ddc->skip, SEEK_SET);
	lseek(*ofd, ddc->seek, SEEK_SET);
	posix_fadvise(*ifd, ddc->skip, 0, POSIX_FADV_SEQUENTIAL);
	posix_fadvise(*ofd, 0, 0, POSIX_FADV_DONTNEED);

	/* count is in block items too */
	ddc->count *= ddc->bs;

	/* If no count is given, its the filesize minus skip offset */
	if (ddc->count == 0)
		ddc->count = ddc->fsize - ddc->skip;

	return 0;
}


#ifdef ANDROID

int copy_splice(struct dd_config *);

int copy_splice_cores(struct dd_config *ddc)
{
	return copy_splice(ddc);
}


#else
int copy_splice_cores(struct dd_config *ddc)
{
	int ifd, ofd, p[2] = {-1, -1};
	ssize_t r = 0, cpu_size = 0;
	size_t n = 0, min_bs = 4096;
	cpu_set_t *cpu_set = NULL;

	if (prepare_copy(ddc, &ifd, &ofd) < 0)
		return -1;

	if ((cpu_set = CPU_ALLOC(2)) == NULL) {
		close(ifd); close(ofd);
		return -1;
	}
	cpu_size = CPU_ALLOC_SIZE(2);
	CPU_ZERO_S(cpu_size, cpu_set);

	if (pipe(p) < 0) {
		ddc->saved_errno = errno;
		close(ifd); close(ofd);
		close(p[0]); close(p[1]);
		return -1;
	}

#ifdef F_SETPIPE_SZ
	for (n = 29; n >= 20; --n) {
		if (fcntl(p[0], F_SETPIPE_SZ, 1<<n) != -1)
			break;
	}
#endif

	n = ddc->bs;
	if (fork() == 0) {
		/* bind to CPU#0 */
		CPU_SET_S(ddc->cores - 1, cpu_size, cpu_set);
		sched_setaffinity(0, cpu_size, cpu_set);

		close(p[0]);
		for (;ddc->b_in != ddc->count && !sigint;) {
			if (n > ddc->count - ddc->b_in)
				n = ddc->count - ddc->b_in;
			r = splice(ifd, NULL, p[1], NULL, n, SPLICE_F_MORE|SPLICE_F_NONBLOCK);
			if (r == 0)
				break;
			if (r < 0) {
				if (errno != EAGAIN)
					break;
				/* If running out of pipe buffer, decrease bs */
				r = 0;
				n = min_bs;
			}
			ddc->b_in += r;
		}

		exit(0);
	}

	/* bind to CPU#1 */
	CPU_SET_S(ddc->cores - 2, cpu_size, cpu_set);
	sched_setaffinity(0, cpu_size, cpu_set);


	for (;ddc->b_out != ddc->count;) {
		r = splice(p[0], NULL, ofd, NULL, n, SPLICE_F_MORE);
		if (r <= 0) {
			ddc->saved_errno = errno;
			break;
		}
		ddc->b_out += r;
		++ddc->rec_out;
	}
	ddc->rec_in = ddc->rec_out;
	close(ifd);
	close(ofd);
	close(p[0]);
	close(p[1]);

	wait(NULL);
	if (r < 0)
		return -1;
	return 0;
}
#endif


int copy_splice(struct dd_config *ddc)
{
	int ifd, ofd, p[2] = {-1, -1};
	ssize_t r = 0;
	size_t n = 0;

	if (prepare_copy(ddc, &ifd, &ofd) < 0)
		return -1;

	if (pipe(p) < 0) {
		ddc->saved_errno = errno;
		close(ifd); close(ofd);
		close(p[0]); close(p[1]);
		return -1;
	}

#ifdef F_SETPIPE_SZ
	for (n = 29; n >= 20; --n) {
		if (fcntl(p[0], F_SETPIPE_SZ, 1<<n) != -1)
			break;
	}
#endif

	n = ddc->bs;
	for (;ddc->b_out != ddc->count && !sigint;) {
		if (n > ddc->count - ddc->b_out)
			n = ddc->count - ddc->b_out;
		r = splice(ifd, NULL, p[1], NULL, n, SPLICE_F_MORE);
		if (r <= 0) {
			ddc->saved_errno = errno;
			break;
		}
		++ddc->rec_in;
		r = splice(p[0], NULL, ofd, NULL, r, SPLICE_F_MORE);
		if (r <= 0) {
			ddc->saved_errno = errno;
			break;
		}
		ddc->b_out += r;
		++ddc->rec_out;
	}
	close(ifd);
	close(ofd);
	close(p[0]);
	close(p[1]);

	if (r < 0)
		return -1;
	return 0;
}


int copy_mmap(struct dd_config *ddc)
{
	int ifd, ofd;
	ssize_t r = 0;
	uint64_t n = 0, bs = 0, i = 0;
	char *addr = NULL;

	if (prepare_copy(ddc, &ifd, &ofd) < 0)
		return -1;

	/* create a file hole of source-file size, if the len is known */
	if (ddc->fsize != (off_t)-1) {
		if (ftruncate(ofd, ddc->fsize) < 0) {
			ddc->saved_errno = errno;
			close(ifd);
			close(ofd);
			return -1;
		}
	}


	for (;ddc->b_out != ddc->count && !sigint;) {
		n = ddc->mmap;
		bs = ddc->bs;

		if (n > ddc->count - ddc->b_out)
			n = ddc->count - ddc->b_out;
		if (bs > n)
			bs = n;

		if (ddc->fsize == (off_t)-1) {
			if (ftruncate(ofd, ddc->b_out + n) < 0) {
				ddc->saved_errno = errno;
				break;
			}
		}

		addr = mmap(NULL, n, PROT_WRITE, MAP_SHARED, ofd, ddc->b_out + ddc->skip);
		if (addr == MAP_FAILED) {
			ddc->saved_errno = errno;
			break;
		}

		for (i = 0; i < n; i += r) {
			if (i + bs > n)
				bs = n - i;
			r = read(ifd, addr + i, bs);
			if (r <= 0) {
				ddc->saved_errno = errno;
				munmap(addr, n);
				break;
			}
			ddc->b_out += r;
			++ddc->rec_in;
		}

		/* pass along the potential above break */
		if (r <= 0)
			break;
		++ddc->rec_out;
		munmap(addr, n);
	}

	/* remove file holes, in case SIGINT appeared */
	if (ddc->fsize != ddc->b_out)
		ftruncate(ofd, ddc->b_out);

	close(ifd);
	close(ofd);

	if (r < 0 || addr == MAP_FAILED)
		return -1;
	return 0;
}


int copy_sendfile(struct dd_config *ddc)
{
	int ifd, ofd;
	ssize_t r = 0;
	off_t off = 0;
	int ret = 0;
	size_t n = 0;

	if (prepare_copy(ddc, &ifd, &ofd) < 0)
		return -1;

	off = ddc->skip;
	n = ddc->bs;
	for (;ddc->b_out < ddc->count && !sigint;) {
		if (n > ddc->count - ddc->b_out)
			n = ddc->count - ddc->b_out;
		r = sendfile(ofd, ifd, &off, n);
		if (r < 0) {
			ddc->saved_errno = errno;
			ret = -1;
			break;
		}
		++ddc->rec_in; ++ddc->rec_out;
		ddc->b_in += r;
		ddc->b_out += r;
	}

	close(ifd);
	close(ofd);
	return ret;
}


int copy(struct dd_config *ddc)
{
	int r = 0;

	ddc->t_start = time(NULL);

	if (ddc->cores)
		r = copy_splice_cores(ddc);
	else if (ddc->mmap)
		r = copy_mmap(ddc);
	else if (ddc->sf)
		r = copy_sendfile(ddc);
	else
		r = copy_splice(ddc);
	ddc->t_end = time(NULL);

	/* avoid div by zero */
	if (ddc->t_start == ddc->t_end)
		++ddc->t_end;
	return r;
}


void usage(const char *msg)
{
	fprintf(stderr, "Usage: odd [if=F1] [of=F2] [send|mmap|cores=N] [bsize] [skip=N] [count=N] [quiet] [nosync]\n");
	exit(0);
}


void print_stat(const struct dd_config *ddc)
{
	if (ddc->quiet)
		return;

#ifdef ANDROID
	fprintf(stderr, "%llu records in\n%llu records out\n%llu bytes (%llu MB) copied, %lu s, %f MB/s [%f mB/s]\n",
	        ddc->rec_in, ddc->rec_out, ddc->b_out, ddc->b_out/(1<<20),
	        ddc->t_end - ddc->t_start,
	        ((double)(ddc->b_out/(1<<20)))/(ddc->t_end - ddc->t_start),
		((double)(ddc->b_out/(1000*1000)))/(ddc->t_end - ddc->t_start));
#else
	fprintf(stderr, "%lu records in\n%lu records out\n%lu bytes (%lu MB) copied, %lu s, %f MB/s [%f mB/s]\n",
	        ddc->rec_in, ddc->rec_out, ddc->b_out, ddc->b_out/(1<<20),
	        ddc->t_end - ddc->t_start,
	        ((double)(ddc->b_out/(1<<20)))/(ddc->t_end - ddc->t_start),
		((double)(ddc->b_out/(1000*1000)))/(ddc->t_end - ddc->t_start));
#endif

}


void sig_int(int x)
{
	fprintf(stderr, "SIGINT! Aborting ...\n");
	sigint = 1;
	return;
}


int main(int argc, char **argv)
{
	int i = 0;
	char buf[1024];
	struct dd_config config;

	memset(&config, 0, sizeof(config));
	config.bs = 1<<16;
	config.in = "/dev/stdin";
	config.out = "/dev/stdout";

	/* emulate 'dd' argument parsing */
	for (i = 1; i < argc; ++i) {
		memset(buf, 0, sizeof(buf));
		if (sscanf(argv[i], "if=%1023c", buf) == 1)
			config.in = strdup(buf);
		else if (sscanf(argv[i], "of=%1023c", buf) == 1)
			config.out = strdup(buf);
		else if (sscanf(argv[i], "skip=%1023c", buf) == 1)
			config.skip = strtoul(buf, NULL, 10);
		else if (sscanf(argv[i], "seek=%1023c", buf) == 1)
			config.seek = strtoul(buf, NULL, 10);
		else if (sscanf(argv[i], "count=%1023c", buf) == 1)
			config.count = strtoul(buf, NULL, 10);
		else if (sscanf(argv[i], "mmap=%1023c", buf) == 1) {
			if (!config.cores) {
				/* Size in MB */
				config.mmap = strtoul(buf, NULL, 10);
				config.mmap <<= 20;
			}
		} else if (sscanf(argv[i], "cores=%1023c", buf) == 1) {
			config.cores = strtoul(buf, NULL, 10);
			if (config.cores < 2)
				config.cores = 2;
			config.mmap = 0;
		} else if (strcmp(argv[i], "send") == 0) {
			config.sf = 1;
		} else if (strcmp(argv[i], "direct") == 0) {
			config.direct = 1;
		} else if (sscanf(argv[i], "bs=%1023c", buf) == 1) {
			config.bs = strtoul(buf, NULL, 10);
		} else if (strcmp(argv[i], "bs") == 0) {
			config.bs = 0;
		} else if (strcmp(argv[i], "quiet") == 0) {
			config.quiet = 1;
		} else if (strcmp(argv[i], "nosync") == 0) {
			config.nosync = 1;
		}
	}

	if (!config.in || !config.out)
		usage(argv[0]);


	if (config.cores && (config.mmap || config.sf)) {
		fprintf(stderr, "Error: 'cores' only works in splice mode, not 'mmap' or 'sf'\n");
		exit(1);
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, sig_int);

	if (copy(&config) < 0)
		fprintf(stderr, "Error: %s\n", strerror(config.saved_errno));
	print_stat(&config);

	if (config.nosync == 0) {
		sync(); sync();
	}
	return 0;
}

