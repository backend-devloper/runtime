/*
 * shared.c:  Shared memory handling, and daemon launching
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002-2006 Novell, Inc.
 */


#include <config.h>
#include <glib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/utsname.h>

#include <mono/io-layer/wapi.h>
#include <mono/io-layer/wapi-private.h>
#include <mono/io-layer/shared.h>
#include <mono/io-layer/handles-private.h>

#undef DEBUG

static guchar *_wapi_shm_file (_wapi_shm_t type)
{
	static guchar file[_POSIX_PATH_MAX];
	guchar *name = NULL, *filename, *dir, *wapi_dir;
	gchar machine_name[256];
	const gchar *fake_name;
	struct utsname ubuf;
	int ret;
	int len;
	
	ret = uname (&ubuf);
	if (ret == -1) {
		ubuf.machine[0] = '\0';
		ubuf.sysname[0] = '\0';
	}

	fake_name = g_getenv ("MONO_SHARED_HOSTNAME");
	if (fake_name == NULL) {
		if (gethostname(machine_name, sizeof(machine_name)) != 0)
			machine_name[0] = '\0';
	} else {
		len = MIN (strlen (fake_name), sizeof (machine_name) - 1);
		strncpy (machine_name, fake_name, len);
		machine_name [len] = '\0';
	}
	
	switch (type) {
	case WAPI_SHM_DATA:
		name = g_strdup_printf ("shared_data-%s-%s-%s-%d-%d-%d",
					machine_name, ubuf.sysname,
					ubuf.machine,
					(int) sizeof(struct _WapiHandleShared),
					_WAPI_HANDLE_VERSION, 0);
		break;
		
	case WAPI_SHM_FILESHARE:
		name = g_strdup_printf ("shared_fileshare-%s-%s-%s-%d-%d-%d",
					machine_name, ubuf.sysname,
					ubuf.machine,
					(int) sizeof(struct _WapiFileShare),
					_WAPI_HANDLE_VERSION, 0);
		break;
	}

	/* I don't know how nfs affects mmap.  If mmap() of files on
	 * nfs mounts breaks, then there should be an option to set
	 * the directory.
	 */
	wapi_dir = getenv ("MONO_SHARED_DIR");
	if (wapi_dir == NULL) {
		filename = g_build_filename (g_get_home_dir (), ".wapi", name,
					     NULL);
	} else {
		filename = g_build_filename (wapi_dir, ".wapi", name, NULL);
	}
	g_free (name);

	g_snprintf (file, _POSIX_PATH_MAX, "%s", filename);
	g_free (filename);
		
	/* No need to check if the dir already exists or check
	 * mkdir() errors, because on any error the open() call will
	 * report the problem.
	 */
	dir = g_path_get_dirname (file);
	mkdir (dir, 0755);
	g_free (dir);
	
	return(file);
}

static int _wapi_shm_file_open (const guchar *filename, guint32 wanted_size)
{
	int fd;
	struct stat statbuf;
	int ret;
	gboolean created = FALSE;
	
try_again:
	/* No O_CREAT yet, because we need to initialise the file if
	 * we have to create it.
	 */
	fd = open (filename, O_RDWR, 0600);
	if (fd == -1 && errno == ENOENT) {
		/* OK, its up to us to create it.  O_EXCL to avoid a
		 * race condition where two processes can
		 * simultaneously try and create the file
		 */
		fd = open (filename, O_CREAT|O_EXCL|O_RDWR, 0600);
		if (fd == -1 && errno == EEXIST) {
			/* It's possible that the file was created in
			 * between finding it didn't exist, and trying
			 * to create it.  Just try opening it again
			 */
			goto try_again;
		} else if (fd == -1) {
			g_critical ("%s: shared file [%s] open error: %s",
				    __func__, filename, g_strerror (errno));
			return(-1);
		} else {
			/* We created the file, so we need to expand
			 * the file.
			 *
			 * (wanted_size-1, because we're about to
			 * write the other byte to actually expand the
			 * file.)
			 */
			if (lseek (fd, wanted_size-1, SEEK_SET) == -1) {
				g_critical ("%s: shared file [%s] lseek error: %s", __func__, filename, g_strerror (errno));
				close (fd);
				unlink (filename);
				return(-1);
			}
			
			do {
				ret = write (fd, "", 1);
			} while (ret == -1 && errno == EINTR);
				
			if (ret == -1) {
				g_critical ("%s: shared file [%s] write error: %s", __func__, filename, g_strerror (errno));
				close (fd);
				unlink (filename);
				return(-1);
			}
			
			created = TRUE;

			/* The contents of the file is set to all
			 * zero, because it is opened up with lseek,
			 * so we don't need to do any more
			 * initialisation here
			 */
		}
	} else if (fd == -1) {
		g_critical ("%s: shared file [%s] open error: %s", __func__,
			    filename, g_strerror (errno));
		return(-1);
	}
	
	/* Use stat to find the file size (instead of hard coding it)
	 * because we can expand the file later if needed (for more
	 * handles or scratch space.)
	 */
	if (fstat (fd, &statbuf) == -1) {
		g_critical ("%s: fstat error: %s", __func__,
			    g_strerror (errno));
		if (created == TRUE) {
			unlink (filename);
		}
		close (fd);
		return(-1);
	}

	if (statbuf.st_size < wanted_size) {
		close (fd);
		if (created == TRUE) {
#ifdef HAVE_LARGE_FILE_SUPPORT
			/* Keep gcc quiet... */
			g_critical ("%s: shared file [%s] is not big enough! (found %lld, need %d bytes)", __func__, filename, statbuf.st_size, wanted_size);
#else
			g_critical ("%s: shared file [%s] is not big enough! (found %ld, need %d bytes)", __func__, filename, statbuf.st_size, wanted_size);
#endif
			unlink (filename);
			return(-1);
		} else {
			/* We didn't create it, so just try opening it again */
			goto try_again;
		}
	}
	
	return(fd);
}

/*
 * _wapi_shm_attach:
 * @success: Was it a success
 *
 * Attach to the shared memory file or create it if it did not exist.
 * Returns the memory area the file was mmapped to.
 */
gpointer _wapi_shm_attach (_wapi_shm_t type)
{
	gpointer shm_seg;
	int fd;
	struct stat statbuf;
	guchar *filename=_wapi_shm_file (type);
	guint32 size;
	
	switch(type) {
	case WAPI_SHM_DATA:
		size = sizeof(struct _WapiHandleSharedLayout);
		break;
		
	case WAPI_SHM_FILESHARE:
		size = sizeof(struct _WapiFileShareLayout);
		break;
	}
	
	fd = _wapi_shm_file_open (filename, size);
	if (fd == -1) {
		g_critical ("%s: shared file [%s] open error", __func__,
			    filename);
		return(NULL);
	}
	
	if (fstat (fd, &statbuf)==-1) {
		g_critical ("%s: fstat error: %s", __func__,
			    g_strerror (errno));
		close (fd);
		return(NULL);
	}
	
	shm_seg = mmap (NULL, statbuf.st_size, PROT_READ|PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (shm_seg == MAP_FAILED) {
		shm_seg = mmap (NULL, statbuf.st_size, PROT_READ|PROT_WRITE,
			MAP_PRIVATE, fd, 0);
		if (shm_seg == MAP_FAILED) {
			g_critical ("%s: mmap error: %s", __func__, g_strerror (errno));
			close (fd);
			return(NULL);
		}
	}
		
	close (fd);
	return(shm_seg);
}

void _wapi_shm_semaphores_init ()
{
	key_t key;
	key_t oldkey;
	int thr_ret;
	struct _WapiHandleSharedLayout *tmp_shared;
	
	/* Yet more barmy API - this union is a well-defined parameter
	 * in a syscall, yet I still have to define it here as it
	 * doesn't appear in a header
	 */
	union semun {
		int val;
		struct semid_ds *buf;
		ushort *array;
	} defs;
	ushort def_vals[_WAPI_SHARED_SEM_COUNT];
	int i;
	int retries = 0;
	
	for (i = 0; i < _WAPI_SHARED_SEM_COUNT; i++) {
		def_vals[i] = 1;
	}
#ifdef NEXT_VERSION_INC
	/* Process count must start at '0' - the 1 for all the others
	 * sets the semaphore to "unlocked"
	 */
	def_vals[_WAPI_SHARED_SEM_PROCESS_COUNT] = 0;
#endif
	
	defs.array = def_vals;
	
	/* Temporarily attach the shared data so we can read the
	 * semaphore key.  We release this mapping and attach again
	 * after getting the semaphores to avoid a race condition
	 * where a terminating process can delete the shared files
	 * between a new process attaching the file and getting access
	 * to the semaphores (which increments the process count,
	 * preventing destruction of the shared data...)
	 */
	tmp_shared = _wapi_shm_attach (WAPI_SHM_DATA);
	g_assert (tmp_shared != NULL);
	
	key = ftok (_wapi_shm_file (WAPI_SHM_DATA), 'M');

again:
	retries++;
	oldkey = tmp_shared->sem_key;

	if (oldkey == 0) {
#ifdef DEBUG
		g_message ("%s: Creating with new key (0x%x)", __func__, key);
#endif

		/* The while loop attempts to make some sense of the
		 * bonkers 'think of a random number' method of
		 * picking a key without collision with other
		 * applications
		 */
		while ((_wapi_sem_id = semget (key, _WAPI_SHARED_SEM_COUNT,
					       IPC_CREAT | IPC_EXCL | 0600)) == -1) {
			if (errno == ENOMEM) {
				g_critical ("%s: semget error: %s", __func__,
					    g_strerror (errno));
			} else if (errno == ENOSPC) {
				g_critical ("%s: semget error: %s.  Try deleting some semaphores with ipcs and ipcrm", __func__, g_strerror (errno));
			} else if (errno != EEXIST) {
				if (retries > 3)
					g_warning ("%s: semget error: %s key 0x%x - trying again", __func__,
							g_strerror (errno), key);
			}
			
			key++;
#ifdef DEBUG
			g_message ("%s: Got (%s), trying with new key (0x%x)",
				   __func__, g_strerror (errno), key);
#endif
		}
		/* Got a semaphore array, so initialise it and install
		 * the key into the shared memory
		 */
		
		if (semctl (_wapi_sem_id, 0, SETALL, defs) == -1) {
			if (retries > 3)
				g_warning ("%s: semctl init error: %s - trying again", __func__, g_strerror (errno));

			/* Something went horribly wrong, so try
			 * getting a new set from scratch
			 */
			semctl (_wapi_sem_id, 0, IPC_RMID);
			goto again;
		}

		if (InterlockedCompareExchange (&tmp_shared->sem_key,
						key, 0) != 0) {
			/* Someone else created one and installed the
			 * key while we were working, so delete the
			 * array we created and fall through to the
			 * 'key already known' case.
			 */
			semctl (_wapi_sem_id, 0, IPC_RMID);
			oldkey = tmp_shared->sem_key;
		} else {
			/* We've installed this semaphore set's key into
			 * the shared memory
			 */
			goto done;
		}
	}
	
#ifdef DEBUG
	g_message ("%s: Trying with old key 0x%x", __func__, oldkey);
#endif

	_wapi_sem_id = semget (oldkey, _WAPI_SHARED_SEM_COUNT, 0600);
	if (_wapi_sem_id == -1) {
		if (retries > 3)
			g_warning ("%s: semget error opening old key 0x%x (%s) - trying again",
					__func__, oldkey,g_strerror (errno));

		/* Someone must have deleted the semaphore set, so
		 * blow away the bad key and try again
		 */
		InterlockedCompareExchange (&tmp_shared->sem_key, 0, oldkey);
		
		goto again;
	}

  done:
	/* Increment the usage count of this semaphore set */
	thr_ret = _wapi_shm_sem_lock (_WAPI_SHARED_SEM_PROCESS_COUNT_LOCK);
	g_assert (thr_ret == 0);
	
#ifdef DEBUG
	g_message ("%s: Incrementing the process count", __func__);
#endif

	/* We only ever _unlock_ this semaphore, letting the kernel
	 * restore (ie decrement) this unlock when this process exits.
	 * We lock another semaphore around it so we can serialise
	 * access when we're testing the value of this semaphore when
	 * we exit cleanly, so we can delete the whole semaphore set.
	 */
	_wapi_shm_sem_unlock (_WAPI_SHARED_SEM_PROCESS_COUNT);

#ifdef DEBUG
	g_message ("%s: Process count is now %d", __func__, semctl (_wapi_sem_id, _WAPI_SHARED_SEM_PROCESS_COUNT, GETVAL));
#endif
	
	_wapi_shm_sem_unlock (_WAPI_SHARED_SEM_PROCESS_COUNT_LOCK);

	munmap (tmp_shared, sizeof(struct _WapiHandleSharedLayout));
}

void _wapi_shm_semaphores_remove (void)
{
	int thr_ret;
	int proc_count;
	
#ifdef DEBUG
	g_message ("%s: Checking process count", __func__);
#endif

	thr_ret = _wapi_shm_sem_lock (_WAPI_SHARED_SEM_PROCESS_COUNT_LOCK);
	g_assert (thr_ret == 0);
	
	proc_count = semctl (_wapi_sem_id, _WAPI_SHARED_SEM_PROCESS_COUNT,
			     GETVAL);
#ifdef NEXT_VERSION_INC
	g_assert (proc_count > 0);
	if (proc_count == 1) {
#else
	/* Compatibility - the semaphore was initialised to '1' (which
	 * normally means 'unlocked'.  Instead of fixing that right
	 * now, which would mean a shared file version increment, just
	 * cope with the value starting too high for now.  Fix this
	 * next time I have to change the file version.
	 */
	g_assert (proc_count > 1);
	if (proc_count == 2) {
#endif
		/* Just us, so blow away the semaphores and the shared
		 * files
		 */
#ifdef DEBUG
		g_message ("%s: Removing semaphores!", __func__);
#endif

		semctl (_wapi_sem_id, IPC_RMID, 0);
		unlink (_wapi_shm_file (WAPI_SHM_DATA));
		unlink (_wapi_shm_file (WAPI_SHM_FILESHARE));
	} else {
		/* "else" clause, because there's no point unlocking
		 * the semaphore if we've just blown it away...
		 */
		_wapi_shm_sem_unlock (_WAPI_SHARED_SEM_PROCESS_COUNT_LOCK);
	}
}

int _wapi_shm_sem_lock (int sem)
{
	struct sembuf ops;
	int ret;
	
#ifdef DEBUG
	g_message ("%s: locking sem %d", __func__, sem);
#endif

	ops.sem_num = sem;
	ops.sem_op = -1;
	ops.sem_flg = SEM_UNDO;
	
  retry:
	do {
		ret = semop (_wapi_sem_id, &ops, 1);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		/* EINVAL covers the case when the semaphore was
		 * deleted before we started the semop
		 */
		if (errno == EIDRM || errno == EINVAL) {
			/* Someone blew away this semaphore set, so
			 * get a new one and try again
			 */
#ifdef DEBUG
			g_message ("%s: Reinitialising the semaphores!",
				   __func__);
#endif

			_wapi_shm_semaphores_init ();
			goto retry;
		}
		
		/* Turn this into a pthreads-style return value */
		ret = errno;
	}
	
#ifdef DEBUG
	g_message ("%s: returning %d (%s)", __func__, ret, g_strerror (ret));
#endif
	
	return(ret);
}

int _wapi_shm_sem_trylock (int sem)
{
	struct sembuf ops;
	int ret;
	
#ifdef DEBUG
	g_message ("%s: trying to lock sem %d", __func__, sem);
#endif
	
	ops.sem_num = sem;
	ops.sem_op = -1;
	ops.sem_flg = IPC_NOWAIT | SEM_UNDO;
	
  retry:
	do {
		ret = semop (_wapi_sem_id, &ops, 1);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		/* EINVAL covers the case when the semaphore was
		 * deleted before we started the semop
		 */
		if (errno == EIDRM || errno == EINVAL) {
			/* Someone blew away this semaphore set, so
			 * get a new one and try again
			 */
#ifdef DEBUG
			g_message ("%s: Reinitialising the semaphores!",
				   __func__);
#endif

			_wapi_shm_semaphores_init ();
			goto retry;
		}
		
		/* Turn this into a pthreads-style return value */
		ret = errno;
	}
	
	if (ret == EAGAIN) {
		/* But pthreads uses this code instead */
		ret = EBUSY;
	}
	
#ifdef DEBUG
	g_message ("%s: returning %d (%s)", __func__, ret, g_strerror (ret));
#endif
	
	return(ret);
}

int _wapi_shm_sem_unlock (int sem)
{
	struct sembuf ops;
	int ret;
	
#ifdef DEBUG
	g_message ("%s: unlocking sem %d", __func__, sem);
#endif
	
	ops.sem_num = sem;
	ops.sem_op = 1;
	ops.sem_flg = SEM_UNDO;
	
  retry:
	do {
		ret = semop (_wapi_sem_id, &ops, 1);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		/* EINVAL covers the case when the semaphore was
		 * deleted before we started the semop
		 */
		if (errno == EIDRM || errno == EINVAL) {
			/* Someone blew away this semaphore set, so
			 * get a new one and try again (we can't just
			 * assume that the semaphore is now unlocked)
			 */
#ifdef DEBUG
			g_message ("%s: Reinitialising the semaphores!",
				   __func__);
#endif

			_wapi_shm_semaphores_init ();
			goto retry;
		}
		
		/* Turn this into a pthreads-style return value */
		ret = errno;
	}
	
#ifdef DEBUG
	g_message ("%s: returning %d (%s)", __func__, ret, g_strerror (ret));
#endif

	return(ret);
}

