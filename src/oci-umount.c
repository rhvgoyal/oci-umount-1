#define _GNU_SOURCE
#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mount.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <selinux/selinux.h>
#include <yajl/yajl_tree.h>
#include <ctype.h>

#include "config.h"

#define _cleanup_(x) __attribute__((cleanup(x)))

#define MOUNTCONF "/etc/oci-umount.conf"
#define MOUNTINFO_PATH "/proc/self/mountinfo"
#define MAX_UMOUNTS	128	/* Maximum number of unmounts */
#define MAX_MAPS	128	/* Maximum number of source to dest mappings */

/* Basic mount info. For now we need only destination */
struct mount_info {
	char *destination;
	unsigned mntid;
	unsigned parent_mntid;
};

/* Basic config mount info */
struct config_mount_info {
	char *source;
	char *destination;
};

struct host_mount_info {
	char *path;
	bool submounts_only;
};

static inline void freep(void *p) {
	free(*(void**) p);
}

static inline void closep(int *fd) {
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static inline void fclosep(FILE **fp) {
	if (*fp)
		fclose(*fp);
	*fp = NULL;
}

static inline void free_mnt_info(struct mount_info **p) {
	unsigned i;
	struct mount_info *mi = *p;

	if (mi == NULL)
		return;

	for (i = 0; mi[i].destination; i++) {
		free(mi[i].destination);
	}
	free(mi);
}

static inline void free_host_mounts(struct host_mount_info **p) {
	unsigned i;
	struct host_mount_info *hmi = *p;

	if (hmi == NULL)
		return;

	for (i = 0; hmi[i].path; i++) {
		free(hmi[i].path);
	}
	free(hmi);
}

/* Free an array of char pointers */
static inline void free_cptr_array(char ***p) {
	unsigned i;
	char **ptr = *p;

	if (ptr == NULL)
		return;

	for (i = 0; ptr[i]; i++) {
		free(ptr[i]);
	}
	free(ptr);
}

static inline void free_config_mounts(struct config_mount_info **p) {
	unsigned i;
	struct config_mount_info *cm = *p;

	if (cm == NULL)
		return;

	for (i = 0; cm[i].destination || cm[i].source; i++) {
		if (cm[i].destination)
			free(cm[i].destination);
		if (cm[i].source)
			free(cm[i].source);
	}
	free(cm);
}

#define _cleanup_free_ _cleanup_(freep)
#define _cleanup_close_ _cleanup_(closep)
#define _cleanup_fclose_ _cleanup_(fclosep)
#define _cleanup_mnt_info_ _cleanup_(free_mnt_info)
#define _cleanup_host_mounts_ _cleanup_(free_host_mounts)
#define _cleanup_cptr_array_ _cleanup_(free_cptr_array)
#define _cleanup_config_mounts_ _cleanup_(free_config_mounts)

#define DEFINE_CLEANUP_FUNC(type, func)                         \
	static inline void func##p(type *p) {                   \
		if (*p)                                         \
			func(*p);                               \
	}                                                       \

DEFINE_CLEANUP_FUNC(yajl_val, yajl_tree_free)

#define pr_perror(fmt, ...) syslog(LOG_ERR, "umounthook <error>: " fmt ": %m\n", ##__VA_ARGS__)
#define pr_pinfo(fmt, ...) syslog(LOG_INFO, "umounthook <info>: " fmt "\n", ##__VA_ARGS__)
#define pr_pwarning(fmt, ...) syslog(LOG_INFO, "umounthook <warning>: " fmt "\n", ##__VA_ARGS__)
#define pr_pdebug(fmt, ...) syslog(LOG_DEBUG, "umounthook <debug>: " fmt "\n", ##__VA_ARGS__)

#define BUFLEN 1024
#define CHUNKSIZE 4096

char *shortid(const char *id) {
	return strndup(id, 12);
}

int iscomment(const char *line) {
	int len = strlen(line);

	for (int i = 0; i < len; i++) {
		if (isspace(line[i]))
			continue;

		switch (line[i]) {
		case '#':
			return 1;
		default:
			return 0;
		}
	}

	// treat blank lines as comments
	return 1;
}

static void *grow_mountinfo_table(void *curr_table, size_t curr_sz, size_t new_sz) {
	void *table;

	table = realloc(curr_table, new_sz);
	if (!table)
		return NULL;

	/* Zero newly allocated area */
	memset(table + curr_sz, 0, (new_sz - curr_sz));
	return table;
}

static int parse_mountinfo(const char *id, struct mount_info **info, size_t *sz)
{
	_cleanup_fclose_ FILE *fp;
	_cleanup_mnt_info_ struct mount_info *mnt_table = NULL;
	struct mount_info *mnt_table_temp;
	int nr_elem = 64;
	int elem_sz = sizeof(struct mount_info);
	/*
	 * table size bytes also keeps track of last zero element while
	 * nr_elem does not
	 */
	size_t table_sz_bytes = (nr_elem + 1) * elem_sz;
	_cleanup_free_ char *line = NULL;
	size_t len = 0;
	int table_idx = 0;

	fp = fopen(MOUNTINFO_PATH, "r");
	if (!fp) {
		pr_perror("%s: Failed to open %s %m", id, MOUNTINFO_PATH);
		return -1;
	}

	/*
	 * Alaways allocate one member extra at the end and keep it zero so
	 * that cleanup function can find the end of array.
	 */
	mnt_table = (struct mount_info *)realloc(NULL, table_sz_bytes);
	if (!mnt_table) {
		pr_perror("%s: Failed to allocate memory for mount tabel", id);
		return -1;
	}

	memset(mnt_table, 0, table_sz_bytes);

	while ((getline(&line, &len, fp)) != -1) {
		char *token, *str = line, *dest;
		int token_idx = 0;
		unsigned mntid, parent_mntid;

		mntid = parent_mntid = 0;
		while ((token = strtok(str, " ")) != NULL) {
			str = NULL;
			token_idx++;
			if (token_idx == 1)
				mntid = atoi(token);
			if (token_idx == 2)
				parent_mntid = atoi(token);
			if (token_idx != 5)
			       continue;

			dest = strdup(token);
			if (!dest) {
				pr_perror("%s: strdup(%s) failed", id, token);
				return -1;
			}

			mnt_table[table_idx].destination = dest;
			mnt_table[table_idx].mntid = mntid;
			mnt_table[table_idx].parent_mntid = parent_mntid;
			table_idx++;
			if (table_idx == nr_elem) {
				int new_sz_bytes = table_sz_bytes + elem_sz * 64;
				mnt_table_temp = grow_mountinfo_table(mnt_table, table_sz_bytes, new_sz_bytes);
				if (!mnt_table_temp) {
					pr_perror("%s: Failed to realloc mountinfo table", id);
					return -1;
				}
				mnt_table = mnt_table_temp;
				table_sz_bytes = new_sz_bytes;
				nr_elem += 64;
			}
		}
	}

	*info = mnt_table;
	*sz = table_idx;
	/* Make sure cleanup function does not free up this table now */
	mnt_table = NULL;
	return 0;
}

static bool is_mounted(char *path, const struct mount_info *mnt_table, size_t table_sz) {
	size_t i;

	for (i = 0; i < table_sz; i++) {
		if (!strcmp(mnt_table[i].destination, path))
			return true;
	}
	return false;
}

/* return <0 on failure otherwise 0.  */
static int map_one_entry(const char *id, const struct config_mount_info *config_mounts, unsigned config_mounts_len, char *host_mnt, char **cont_mnt, unsigned max_mapped, char *suffix, unsigned *nr_mapped) {
	char *str, *dest;
	unsigned i, suffix_len = 0;
	char path[PATH_MAX];

	if (suffix)
		suffix_len = strlen(suffix);

	for (i = 0; i < config_mounts_len; i++) {
		if (strcmp(host_mnt, config_mounts[i].source))
			continue;

		dest = config_mounts[i].destination;
		if ((strlen(dest) + suffix_len + 1 > PATH_MAX)) {
			pr_perror("%s: Mapped destination=%s and suffix=%s together are longer than PATH_MAX", id, dest, suffix);
		}

		strcpy(path, config_mounts[i].destination);
		if (suffix)
			strcat(path, suffix);

		str = strdup(path);
		if (!str) {
			pr_perror("%s: strdup(%s) failed.", id, path);
			return -1;
		}

		if (*nr_mapped >= max_mapped) {
			pr_perror("%s: Mapping array is full (size=%d). Can't add another entry.", id, *nr_mapped);
			return -1;
		}

		cont_mnt[*nr_mapped] = str;
		*nr_mapped += 1;
	}
	return 0;
}

/* Frees up entries of char ptr array entries. Assumes last entry to be null*/
static void free_char_ptr_array_entries(char **array, unsigned int nr_entries) {
	for (unsigned i = 0; i < nr_entries; i++) {
		free(array[i]);
		array[i] = NULL;
	}
}


/* Returns <0 on error otherwise number of mappings found  */
static int map_mount_host_to_container(const char *id, const struct config_mount_info *config_mounts, unsigned config_mounts_len, char *host_mnt, char **cont_mnt, unsigned max_mapped)
{
	char *str;
	_cleanup_free_ char *host_mnt_dup = NULL;
	char *suffix = NULL;
	int ret;
	unsigned nr_mapped = 0;

	host_mnt_dup = strdup(host_mnt);
	if (!host_mnt_dup) {
		pr_perror("%s: strdup(%s) failed.", id, host_mnt);
		return -1;
	}

	str = host_mnt_dup;
	do {
		ret = map_one_entry(id, config_mounts, config_mounts_len, str, cont_mnt, max_mapped, suffix, &nr_mapped);
		if (ret < 0) {
			free_char_ptr_array_entries(cont_mnt, nr_mapped);
			return ret;
		}

		if (!strcmp(str, "/"))
			break;

		str = dirname(str);

		if (!strcmp(str, "/"))
			suffix = host_mnt;
		else
			suffix = host_mnt + strlen(str);
	} while(1);

	for (unsigned i = 0; i < nr_mapped; i++) {
		pr_pinfo("%s: mapped host_mnt=%s to cont_mnt=%s", id, host_mnt, cont_mnt[i]);
	}

	return nr_mapped;
}

/*
 * Given a mount path, gets its mount id from mountinfo table. If a mount is
 * found, mount id is returned, otherwise -1 is returned
 */
static int find_mntid(char *path, const struct mount_info *mnt_table, size_t table_sz)
{
	unsigned i;

	for (i = 0; i < table_sz; i++) {
		if (!strcmp(path, mnt_table[i].destination)) {
			return mnt_table[i].mntid;
		}
	}

	return -1;
}

/*
 * Find mount id of parent mount of a path. If path itself is a mount point,
 * then mount id of that mount is returned. Otherwise we travel up the path
 * and see try to find which part of it is mounted
 */
static int parent_mntid(const char *id, char *path, const struct mount_info *mnt_table, size_t table_sz)
{
	_cleanup_free_ char *path_copy = NULL;
	char *dname;
	int mntid;

	path_copy = strdup(path);
	if (!path_copy) {
		pr_perror("%s: strdup(%s) failed: %s", id, path, strerror(errno));
		return -1;
	}

	dname = path_copy;

	while(1) {
		mntid = find_mntid(dname, mnt_table, table_sz);
		if (mntid >= 0) {
			return mntid;
		}

		if (!strcmp(dname, "/"))
			break;

		/* Path is not a mount point. Go one level up */
		dname = dirname(dname);
		if (!strcmp(dname, "."))
			break;
	}

	return -1;
}

/* Returns 0 on success, negative error otherwise */
static int unmount(const char *id, char *umount_path, bool submounts_only, const struct mount_info *mnt_table, size_t table_sz)
{
	int ret, i;
	int mntid = 0;

	if (!submounts_only) {
		if (!is_mounted((char *)umount_path, mnt_table, table_sz)) {
			pr_pinfo("[%s] is not a mountpoint. Skipping.", umount_path);
			return 0;
		}

		ret = umount2(umount_path, MNT_DETACH);
		if (!ret)
			pr_pinfo("%s: Unmounted: [%s]", id, umount_path);
		else
			pr_perror("%s: Failed to unmount: [%s]", id, umount_path);
		return ret;
	}

	/* Unmount submounts only */
	mntid = parent_mntid(id, umount_path, mnt_table, table_sz);
	if (mntid < 0) {
		pr_perror("%s: Could not determine mount id of path: [%s]", id, umount_path);
		return -1;
	}

	/*
	 * lazy unmount all direct submounts. Traverse in reverse order so that
	 * if two child have same parent but one child masks other child, we
	 * get to unmount top level child first
	 *
	 * For Example. Try following.
	 * mount -t tmpfs none foo1/foo2
	 * mount -t tmpfs none foo1
	 *
	 * Here both foo1 and foo2 are child of same parent. But we want
	 * to unmount foo1 first and foo2 later. /proc/self/mountinfo seems
	 * to be time ordered and we are relying on that. If not, this logic
	 * will be broken.
	 */
	for (i = table_sz - 1; i >= 0; i--) {
		if (mnt_table[i].parent_mntid != (unsigned)mntid)
			continue;

		/* This mount has to be submount of path specified */
		if (strncmp(umount_path, mnt_table[i].destination, strlen(umount_path))) {
			continue;
		}

		ret = umount2(mnt_table[i].destination, MNT_DETACH);
		if (!ret)
			pr_pinfo("%s: Unmounted submount: [%s]", id, mnt_table[i].destination);
		else
			pr_perror("%s: Failed to unmount submount: [%s]. Skipping.", id, mnt_table[i].destination);
	}
	return 0;
}

static int prestart(
	const char *id,
	const char *rootfs,
	int pid,
	const struct config_mount_info *config_mounts,
	unsigned config_mounts_len)
{
	pr_pinfo("prestart container_id:%s rootfs:%s", id, rootfs);
	_cleanup_close_  int fd = -1;
	_cleanup_free_   char *options = NULL;

	size_t mnt_table_sz;
	_cleanup_mnt_info_ struct mount_info *mnt_table = NULL;

	char process_mnt_ns_fd[PATH_MAX];
	char umount_path[PATH_MAX];
	_cleanup_fclose_ FILE *fp = NULL;
	_cleanup_host_mounts_ struct host_mount_info *mounts_on_host = NULL;
	_cleanup_cptr_array_ char **mapped_paths = NULL;
	int nr_umounts = 0;
	_cleanup_free_ char *line = NULL;
	char *real_path;
	size_t len = 0;
	ssize_t read;
	int i, ret, nr_mapped;

	/* Allocate one extra element and keep it zero for cleanup function */
	mounts_on_host = malloc((MAX_UMOUNTS + 1) * sizeof(struct host_mount_info));
	if (!mounts_on_host) {
		pr_perror("%s: Failed to malloc memory for mounts_on_host table", id);
		return EXIT_FAILURE;
	}
	memset((void *)mounts_on_host, 0, (MAX_UMOUNTS + 1) * sizeof(struct host_mount_info));

	/* Allocate one extra element and keep it zero for cleanup function */
	mapped_paths = malloc((MAX_MAPS + 1) * sizeof(char *));
	if (!mapped_paths) {
		pr_perror("%s: Failed to malloc memory for mapped_paths array", id);
		return EXIT_FAILURE;
	}
	memset((void *)mapped_paths, 0, (MAX_MAPS + 1) * sizeof(char *));

	/* Parse oci-umounts.conf file, canonicalize path names and skip
	 * paths which are not a mountpoint on host */
	fp = fopen(MOUNTCONF, "r");
	if (fp == NULL) {
		if (errno == ENOENT) {
			pr_pwarning("%s: Config file not found: %s", id, MOUNTCONF);
			return 0;
		}
		pr_perror("%s: Failed to open config file: %s", id, MOUNTCONF);
		return EXIT_FAILURE;
	}

	while ((read = getline(&line, &len, fp)) != -1) {
		bool submounts_only = false;

		/* Get rid of newline character at the end */
		line[read - 1] ='\0';
		read--;

		if (iscomment(line))
			continue;

		if (nr_umounts == MAX_UMOUNTS) {
			pr_perror("%s: Exceeded maximum number of supported unmounts is %d", id, MAX_UMOUNTS);
			return EXIT_FAILURE;
		}

		// If there is a "/*" at the end, only unmount submounts
		if (read >= 2) {
			if (line[read - 1] == '*' && line[read - 2] == '/') {
				submounts_only = true;
				line[read - 1] = '\0';
			}
		}

		real_path = realpath(line, NULL);
		if (!real_path) {
			pr_pinfo("%s: Failed to canonicalize path [%s]: %m. Skipping.", id, line);
			continue;
		}

		mounts_on_host[nr_umounts].path = real_path;
		mounts_on_host[nr_umounts].submounts_only = submounts_only;
		nr_umounts++;
	}

	if (!nr_umounts)
		return 0;

	snprintf(process_mnt_ns_fd, PATH_MAX, "/proc/%d/ns/mnt", pid);

	fd = open(process_mnt_ns_fd, O_RDONLY);
	if (fd < 0) {
		pr_perror("%s: Failed to open mnt namespace fd %s", id, process_mnt_ns_fd);
		return EXIT_FAILURE;
	}

	/* Join the mount namespace of the target process */
	if (setns(fd, 0) == -1) {
		pr_perror("%s: Failed to setns to %s", id, process_mnt_ns_fd);
		return EXIT_FAILURE;
	}

	/* Switch to the root directory */
	if (chdir("/") == -1) {
		pr_perror("%s: Failed to chdir", id);
		return EXIT_FAILURE;
	}

	/* Parse mount table */
	ret = parse_mountinfo(id, &mnt_table, &mnt_table_sz);
	if (ret < 0) {
		pr_perror("%s: Failed to parse mountinfo table", id);
		return EXIT_FAILURE;
	}

	for (i = 0; i < nr_umounts; i++) {
		nr_mapped = map_mount_host_to_container(id, config_mounts, config_mounts_len, mounts_on_host[i].path, mapped_paths, MAX_MAPS);
		if (nr_mapped < 0) {
			pr_perror("%s: Error while trying to map mount [%s] from host to conatiner. Skipping.", id, mounts_on_host[i].path);
			continue;
		}

		if (!nr_mapped) {
			pr_pinfo("%s: Could not find mapping for mount [%s] from host to conatiner. Skipping.", id, mounts_on_host[i].path);
			continue;
		}

		for (int j = 0; j < nr_mapped; j++) {
			snprintf(umount_path, PATH_MAX, "%s%s", rootfs, mapped_paths[j]);
			ret = unmount(id, umount_path, mounts_on_host[i].submounts_only, mnt_table, mnt_table_sz);
			if (ret < 0) {
				pr_perror("%s: Skipping unmount path: [%s]", id, umount_path);
				continue;
			}
		}
		free_char_ptr_array_entries(mapped_paths, nr_mapped);
	}
	return 0;
}

/*
 * Read the entire content of stream pointed to by 'from' into a buffer in memory.
 * Return a pointer to the resulting NULL-terminated string.
 */
char *getJSONstring(FILE *from, size_t chunksize, char *msg)
{
	struct stat stat_buf;
	char *err = NULL, *JSONstring = NULL;
	size_t nbytes, bufsize;

	if (fstat(fileno(from), &stat_buf) == -1) {
		err = "fstat failed";
		goto fail;
	}

	if (S_ISREG(stat_buf.st_mode)) {
		/*
		 * If 'from' is a regular file, allocate a buffer based
		 * on the file size and read the entire content with a
		 * single fread() call.
		 */
		if (stat_buf.st_size == 0) {
			err = "is empty";
			goto fail;
		}

		bufsize = (size_t)stat_buf.st_size;

		JSONstring = (char *)malloc(bufsize + 1);
		if (JSONstring == NULL) {
			err = "failed to allocate buffer";
			goto fail;
		}

		nbytes = fread((void *)JSONstring, 1, (size_t)bufsize, from);
		if (nbytes != (size_t)bufsize) {
			err = "error encountered on read";
			goto fail;
		}
	} else {
		/*
		 * If 'from' is not a regular file, call fread() iteratively
		 * to read sections of 'chunksize' bytes until EOF is reached.
		 * Call realloc() during each iteration to expand the buffer
		 * as needed.
		 */
		bufsize = 0;

		for (;;) {
			JSONstring = (char *)realloc((void *)JSONstring, bufsize + chunksize);
			if (JSONstring == NULL) {
				err = "failed to allocate buffer";
				goto fail;
			}

			nbytes = fread((void *)&JSONstring[bufsize], 1, (size_t)chunksize, from);
			bufsize += nbytes;

			if (nbytes != (size_t)chunksize) {
				if (ferror(from)) {
					err = "error encountered on read";
					goto fail;
				}
				if (feof(from))
					break;
			}
		}

		if (bufsize == 0) {
			err = "is empty";
			goto fail;
		}

		JSONstring = (char *)realloc((void *)JSONstring, bufsize + 1);
		if (JSONstring == NULL) {
			err = "failed to allocate buffer";
			goto fail;
		}
	}

	/* make sure the string is NULL-terminated */
	JSONstring[bufsize] = 0;
	return JSONstring;
fail:
	free(JSONstring);
	pr_perror("%s: %s", msg, err);
	return NULL;
}

static int parseBundle(const char *id, yajl_val *node_ptr, char **rootfs, struct config_mount_info **mounts, size_t *mounts_len)
{
	yajl_val node = *node_ptr;
	char config_file_name[PATH_MAX];
	char errbuf[BUFLEN];
	char *configData;
	_cleanup_(yajl_tree_freep) yajl_val config_node = NULL;
	_cleanup_config_mounts_ struct config_mount_info *config_mounts = NULL;
	unsigned config_mounts_len = 0;
	_cleanup_fclose_ FILE *fp = NULL;

	/* 'bundle' must be specified for the OCI hooks, and from there we read the configuration file */
	const char *bundle_path[] = { "bundle", (const char *)0 };
	yajl_val v_bundle_path = yajl_tree_get(node, bundle_path, yajl_t_string);
	if (!v_bundle_path) {
		const char *bundle_path[] = { "bundlePath", (const char *)0 };
		v_bundle_path = yajl_tree_get(node, bundle_path, yajl_t_string);
	}

	if (v_bundle_path) {
		snprintf(config_file_name, PATH_MAX, "%s/config.json", YAJL_GET_STRING(v_bundle_path));
		fp = fopen(config_file_name, "r");
	} else {
		char msg[] = "bundle not found in state";
		snprintf(config_file_name, PATH_MAX, "%s", msg);
	}

	if (fp == NULL) {
		pr_perror("%s: Failed to open config file: %s", id, config_file_name);
		return EXIT_FAILURE;
	}

	/* Read the entire config file */
	snprintf(errbuf, BUFLEN, "failed to read config data from %s", config_file_name);
	configData = getJSONstring(fp, (size_t)CHUNKSIZE, errbuf);
	if (configData == NULL)
		return EXIT_FAILURE;

	/* Parse the config file */
	memset(errbuf, 0, BUFLEN);
	config_node = yajl_tree_parse((const char *)configData, errbuf, sizeof(errbuf));
	if (config_node == NULL) {
		if (strlen(errbuf)) {
			pr_perror("parse error: %s: %s: %s", id, config_file_name, errbuf);
		} else {
			pr_perror("parse error: %s: %s: unknown error", id, config_file_name);
		}
		return EXIT_FAILURE;
	}

	/* Extract root path from the bundle */
	const char *root_path[] = { "root", "path", (const char *)0 };
	yajl_val v_root = yajl_tree_get(config_node, root_path, yajl_t_string);
	if (!v_root) {
		pr_perror("%s: root not found in %s", id, config_file_name);
		return EXIT_FAILURE;
	}
	char *lrootfs = YAJL_GET_STRING(v_root);

	/* Prepend bundle path if the rootfs string is relative */
	if (lrootfs[0] == '/') {
		*rootfs = strdup(lrootfs);
		if (!*rootfs) {
			pr_perror("%s: failed to alloc rootfs", id);
			return EXIT_FAILURE;
		}
	} else {
		char *new_rootfs;

		asprintf(&new_rootfs, "%s/%s", YAJL_GET_STRING(v_bundle_path), lrootfs);
		if (!new_rootfs) {
			pr_perror("%s: failed to alloc rootfs", id);
			return EXIT_FAILURE;
		}
		*rootfs = new_rootfs;
	}

	/* Extract values from the config json */
	const char *mount_points_path[] = {"mounts", (const char *)0 };
	yajl_val v_mounts = yajl_tree_get(config_node, mount_points_path, yajl_t_array);
	if (!v_mounts) {
		pr_perror("%s: mounts not found in %s", id, config_file_name);
		return EXIT_FAILURE;
	}

	config_mounts_len = YAJL_GET_ARRAY(v_mounts)->len;
	/* Allocate one extra element which will be set to 0 and be used as
	 * end of array in free function */
	config_mounts = malloc(sizeof(struct config_mount_info) * (config_mounts_len + 1));
	if (!config_mounts) {
		pr_perror("%s: error malloc'ing", id);
		return EXIT_FAILURE;
	}

	memset(config_mounts, 0, sizeof(struct config_mount_info) * (config_mounts_len + 1));

	for (unsigned int i = 0; i < config_mounts_len; i++) {
		yajl_val v_mounts_values = YAJL_GET_ARRAY(v_mounts)->values[i];

		const char *destination_path[] = {"destination", (const char *)0 };
		const char *source_path[] = {"source", (const char *)0 };

		yajl_val v_destination = yajl_tree_get(v_mounts_values, destination_path, yajl_t_string);
		if (!v_destination) {
			pr_perror("%s: cannot find mount destination in %s", id, config_file_name);
			return EXIT_FAILURE;
		}
		config_mounts[i].destination = strdup(YAJL_GET_STRING(v_destination));
		if (!config_mounts[i].destination) {

			pr_perror("%s: strdup(%s) failed.", id, YAJL_GET_STRING(v_destination));
			return EXIT_FAILURE;
		}

		yajl_val v_source = yajl_tree_get(v_mounts_values, source_path, yajl_t_string);
		if (!v_source) {
			pr_perror("%s: Cannot find mount source in %s", id, config_file_name);
			return EXIT_FAILURE;
		}
		config_mounts[i].source = strdup(YAJL_GET_STRING(v_source));
		if (!config_mounts[i].source) {
			pr_perror("%s: strdup(%s) failed.", id, YAJL_GET_STRING(v_source));
			return EXIT_FAILURE;
		}
	}

	*mounts = config_mounts;
	*mounts_len = config_mounts_len;
	/* set it NULL so that gcc cleanup function does not try to free this */
	config_mounts = NULL;

	return 0;
}

int main(int argc, char *argv[])
{
	_cleanup_(yajl_tree_freep) yajl_val node = NULL;
	_cleanup_(yajl_tree_freep) yajl_val config_node = NULL;
	char errbuf[BUFLEN];
	char *stateData;
	_cleanup_fclose_ FILE *fp = NULL;
	_cleanup_free_ char *id = NULL;
	int ret;
	_cleanup_config_mounts_ struct config_mount_info *config_mounts = NULL;
	size_t config_mounts_len = 0;

	/* Read the entire state from stdin */
	snprintf(errbuf, BUFLEN, "failed to read state data from standard input");
	stateData = getJSONstring(stdin, (size_t)CHUNKSIZE, errbuf);
	if (stateData == NULL)
		return EXIT_FAILURE;

	/* Parse the state */
	memset(errbuf, 0, BUFLEN);
	node = yajl_tree_parse((const char *)stateData, errbuf, sizeof(errbuf));
	if (node == NULL) {
		if (strlen(errbuf)) {
			pr_perror("parse_error: %s", errbuf);
		} else {
			pr_perror("parse_error: unknown error");
		}
		return EXIT_FAILURE;
	}

	const char *id_path[] = { "id", (const char *) 0 };
	yajl_val v_id = yajl_tree_get(node, id_path, yajl_t_string);
	if (!v_id) {
		pr_perror("id not found in state");
		return EXIT_FAILURE;
	}
	const char *container_id = YAJL_GET_STRING(v_id);
	id = shortid(container_id);
	if (!id) {
		pr_perror("%s: failed to create shortid", container_id);
		return EXIT_FAILURE;
	}

	const char *pid_path[] = { "pid", (const char *) 0 };
	yajl_val v_pid = yajl_tree_get(node, pid_path, yajl_t_number);
	if (!v_pid) {
		pr_perror("%s: pid not found in state", id);
		return EXIT_FAILURE;
	}
	int target_pid = YAJL_GET_INTEGER(v_pid);

	/* OCI hooks set target_pid to 0 on poststop, as the container process
	   already exited.  If target_pid is bigger than 0 then it is a start
	   hook.
	   In most cases the calling program should pass in a argv[1] option,
	   like prestart, poststart or poststop.  In certain cases we also
	   support passing of no argv[1], and then default to prestart if the
	   target_pid != 0, poststop if target_pid == 0.
	*/
	if ((argc >= 2 && !strcmp("prestart", argv[1])) ||
	    (argc == 1 && target_pid)) {
		_cleanup_free_ char *rootfs=NULL;
		ret = parseBundle(id, &node, &rootfs, &config_mounts, &config_mounts_len);
		if (ret < 0)
			return EXIT_FAILURE;

		if (prestart(id, rootfs, target_pid, config_mounts, config_mounts_len) != 0) {
			return EXIT_FAILURE;
		}
	} else {
		if (argc >= 2) {
			pr_pdebug("%s: %s ignored", id, argv[1]);
		} else {
			pr_pdebug("%s: No args ignoring", id);
		}
	}

	return EXIT_SUCCESS;
}
