#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h> /* open|read|close dir */
#include <time.h> /* time() */
#define _GNU_SOURCE
#include <string.h> /* strnlen */

#include "ipc.h"
#include "walrus.h"
#include "euca_auth.h"
#include <data.h>
#include <misc.h>
#include <storage.h>
#include <vnetwork.h>

#define BUFSIZE 512 /* random buffer size used all over the place */

/* default paths(may be overriden from config file) */
static char add_key_command_path [BUFSIZE] = "";
static int default_swap_size = 512; /* in MB */
static int default_ephemeral_size = 0; /* in MB, none by default */

static char *sc_instance_path = "";
static char disk_convert_command_path [BUFSIZE] = "";
static int scConfigInit=0;
static sem * sc_sem;

int scInitConfig (void)
{
    struct stat mystat;
    char config [BUFSIZE];
    char * s;

    if (scConfigInit) {
      return 0;
    }
    
    if ((sc_sem = sem_alloc (1, "eucalyptus-storage-semaphore")) == NULL) { /* TODO: use this semaphore to fix the race */
        logprintfl (EUCAERROR, "failed to create and initialize a semaphore\n");
        return 1;
    }
    /* read in configuration */
    char * home = getenv (EUCALYPTUS_ENV_VAR_NAME);
    if (!home) {
        home = strdup(""); /* root by default */
    }
    
    snprintf(config, BUFSIZE, EUCALYPTUS_CONF_LOCATION, home);
    if (stat(config, &mystat)==0) {
        logprintfl (EUCAINFO, "SC is looking for configuration in %s\n", config);
        
        if (get_conf_var(config, INSTANCE_PATH, &s)>0){ 
            sc_instance_path = strdup (s); 
            free (s); 
        }
    }
    snprintf(add_key_command_path, BUFSIZE, EUCALYPTUS_ADD_KEY, home, home, home);
    
    /* we need to have valid path */
    if (check_directory(sc_instance_path)) {
	    logprintfl (EUCAERROR, "ERROR: INSTANCE_PATH (%s) does not exist!\n", sc_instance_path);
	    return(1);
    }

    if (euca_init_cert ()) {
        logprintfl (EUCAFATAL, "failed to find cryptographic certificates\n");
        return 1;
    }

    snprintf (disk_convert_command_path, BUFSIZE, EUCALYPTUS_DISK_CONVERT, home, home);

    scConfigInit=1;
    return(0);
}

void scSaveInstanceInfo (const ncInstance * instance)
{
    char file_path [BUFSIZE];
    int fd;

    if (instance==NULL) return;
    snprintf(file_path, BUFSIZE, "%s/%s/%s/instance-checkpoint", sc_instance_path, instance->userId, instance->instanceId);
    if ((fd=open(file_path, O_CREAT | O_WRONLY, 0600))<0) return;
    write (fd, (char *)instance, sizeof(struct ncInstance_t));
    close (fd);
}

ncInstance * scRecoverInstanceInfo (const char *instanceId)
{
    const int file_size = sizeof(struct ncInstance_t);
    ncInstance * instance = malloc(file_size);
    char file_path [BUFSIZE];
    struct dirent * dir_entry;
    DIR * insts_dir;
    char * userId = NULL;
    int fd;

    if (instance==NULL) return NULL;

    /* we don't know userId, so we'll look for instanceId in every user's
     * directory (we're assuming that instanceIds are unique in the system) */
    if ((insts_dir=opendir(sc_instance_path))==NULL) return NULL;
    while ((dir_entry=readdir(insts_dir))!=NULL) {
        char tmp_path [BUFSIZE];
        struct stat mystat;

        snprintf(tmp_path, BUFSIZE, "%s/%s/%s", sc_instance_path, dir_entry->d_name, instanceId);
        if (stat(tmp_path, &mystat)==0) {
            userId = strdup (dir_entry->d_name);
            break; /* we got it! */
        }
    }
    if (userId==NULL) return NULL;

    snprintf(file_path, BUFSIZE, "%s/%s/%s/instance-checkpoint", sc_instance_path, userId, instanceId);
	free(userId);
    if ((fd=open(file_path, O_RDONLY))<0 ||
        read(fd, instance, file_size)<file_size) {
        perror(file_path);
        free (instance);
        return NULL;
    }
    close (fd);
    instance->stateCode = NO_STATE;
    return instance;
}

/* perform integrity check on instances directory, including the cache:
 * remove any files from non-running Eucalyptus instances, delete files
 * from cache that are not complete, return the amount of bytes used up by
 * everything
 */
long long scFSCK (bunchOfInstances ** instances)
{
    long long total_size = 0;
    struct stat mystat;

    if (instances==NULL) return -1;
    
    logprintfl (EUCAINFO, "checking the integrity of instances directory (%s)\n", sc_instance_path);

    /* let us not 'rm -rf /' accidentally */
    if (strlen(sc_instance_path)<2 || 
        sc_instance_path[0]!='/' ) {
        logprintfl (EUCAFATAL, "error: instances directory cannot be /, sorry\n");
        return -1; 
    }

    if (stat (sc_instance_path, &mystat) < 0) {
        logprintfl (EUCAFATAL, "error: could not stat %s\n", sc_instance_path);
        return -1;
    }
    total_size += mystat.st_size;

    DIR * insts_dir;
    if ((insts_dir=opendir(sc_instance_path))==NULL) {
        logprintfl (EUCAFATAL, "error: could not open instances directory %s\n", sc_instance_path);
        return -1;
    }

    /*** run through all users ***/

    char * cache_path = NULL;
    struct dirent * inst_dir_entry;
    while ((inst_dir_entry=readdir(insts_dir))!=NULL) {
        char * uname = inst_dir_entry->d_name;
        char user_path [BUFSIZE];
        struct dirent * user_dir_entry;
        DIR * user_dir;

        if (!strcmp(".", uname) || 
            !strcmp("..", uname))
            continue;

        snprintf (user_path, BUFSIZE, "%s/%s", sc_instance_path, uname);
        if ((user_dir=opendir(user_path))==NULL) {
            logprintfl (EUCAWARN, "warning: unopeneable directory %s\n", user_path);
            continue;
        }

        /*** run through all instances of a user ***/
        while ((user_dir_entry=readdir(user_dir))!=NULL) {
            char * iname = user_dir_entry->d_name;
            
            if (!strcmp(".", iname) ||
                !strcmp("..", iname))
                continue;
            
            char instance_path [BUFSIZE];
            snprintf (instance_path, BUFSIZE, "%s/%s", user_path, iname);

            if (!strcmp("cache", iname) &&
                !strcmp(EUCALYPTUS_ADMIN, uname)) { /* cache is in admin's dir */
                cache_path = strdup (instance_path);
                continue;
            }

            /* spare directories of running instances, but count their usage */
            if (find_instance (instances, iname)) {
                int bytes = dir_size (instance_path);
                if (bytes>0) {
                    logprintfl (EUCAINFO, "- running instance %s directory, size=%d\n", iname, bytes);
                    total_size += bytes;
                } else if (bytes==0) {
                    logprintfl (EUCAWARN, "warning: empty instance directory %s\n", instance_path);
                } else {
                    logprintfl (EUCAWARN, "warning: non-standard instance directory %s\n", instance_path);
                } 
                continue;
            }

            /* looks good - destroy it */
            if (vrun ("rm -rf %s", instance_path)) {
                logprintfl (EUCAWARN, "warning: failed to remove %s\n", instance_path);
            }
        }
        closedir (user_dir);
    }
    closedir (insts_dir);

    /*** scan the cache ***/

    logprintfl (EUCAINFO, "checking the integrity of the cache directory (%s)\n", cache_path);

    if (cache_path==NULL) {
        logprintfl (EUCAINFO, "no cache directory yet\n");
        return total_size;
    }

    if (stat (cache_path, &mystat) < 0) {
        logprintfl (EUCAFATAL, "error: could not stat %s\n", cache_path);
        free (cache_path);
        return -1;
    }
    total_size += mystat.st_size;
   
    DIR * cache_dir;
    if ((cache_dir=opendir(cache_path))==NULL) {
        logprintfl (EUCAFATAL, "errror: could not open cache directory %s\n", cache_path);
        free (cache_path);
        return -1;
    }

    struct dirent * cache_dir_entry;
    while ((cache_dir_entry=readdir(cache_dir))!=NULL) {
        char * image_name = cache_dir_entry->d_name;
        char image_path [BUFSIZE];
        int image_size = 0;
        int image_files = 0;

        if (!strcmp(".", image_name) || 
            !strcmp("..", image_name))
            continue;
        
        DIR * image_dir;
        snprintf (image_path, BUFSIZE, "%s/%s", cache_path, image_name);
        if ((image_dir=opendir(image_path))==NULL) {
            logprintfl (EUCAWARN, "warning: unopeneable directory %s\n", image_path);
            continue;
        }

        if (stat (image_path, &mystat) < 0) {
            logprintfl (EUCAWARN, "warning: could not stat %s\n", image_path);
            continue;
        }
        image_size += mystat.st_size;
        
        /* make sure that image directory contains only two files: one
         * named X and another X-digest, also add up their sizes */
        char X        [BUFSIZE] = "";
        char X_digest [BUFSIZE] = "";
        struct dirent * image_dir_entry;
        while ((image_dir_entry=readdir(image_dir))!=NULL) {
            char name [BUFSIZE];
            strncpy (name, image_dir_entry->d_name, BUFSIZE);
            
            if (!strcmp(".", name) ||
                !strcmp("..", name))
                continue;
            image_files++;
            
            char filepath [BUFSIZE];
            snprintf (filepath, BUFSIZE, "%s/%s", image_path, name);
            if (stat (filepath, &mystat) < 0 ) {
                logprintfl (EUCAERROR, "error: could not stat file %s\n", filepath);
                break;
            }
            if (mystat.st_size < 1) {
                logprintfl (EUCAERROR, "error: empty file among cached images in %s\n", filepath);
                break;
            }
            image_size += mystat.st_size;
            
            char * suffix;
            if ((suffix=strstr (name, "-digest"))==NULL) {
                if (strlen (X)) 
                    break; /* already saw X => fail */
                strncpy (X, name, BUFSIZE);
            } else {
                if (strlen (X_digest))
                    break; /* already saw X-digest => fail */
                * suffix = '\0';
                strncpy (X_digest, name, BUFSIZE);
            }
        }
        
        if (image_files != 2 || strncmp (X, X_digest, BUFSIZE) != 0 ) {
            logprintfl (EUCAERROR, "error: inconsistent state of cached image %s, deleting it\n", image_name);
            if (vrun ("rm -rf %s", image_path)) {            
                logprintfl (EUCAWARN, "warning: failed to remove %s\n", image_path);
            }
        } else {
            if (image_size>0) {
                logprintfl (EUCAINFO, "- cached image %s directory, size=%d\n", image_name, image_size);
                total_size += image_size;
            } else {
                logprintfl (EUCAWARN, "warning: empty cached image directory %s\n", image_path);
            }
        }
    }
    closedir (cache_dir);
    free (cache_path);
    
    return total_size;
}

const char * scGetInstancePath(void)
{
    return sc_instance_path;
}

int scSetInstancePath(char *path) {
  sc_instance_path = strdup(path);
  return(0);
}

int scCleanupInstanceImage (char *user, char *instId) {
    return vrun ("rm -rf %s/%s/%s/", sc_instance_path, user, instId);
}

/* if path=A/B/C but only A exists, this will try to create B and C */
int ensure_path_exists (const char * path)
{
    mode_t mode = 0777;
    int len = strlen(path);
    char * path_copy = strdup(path);
    int i;

    if (path_copy==NULL) return errno;

    for (i=0; i<len; i++) {
        struct stat buf;
        char try_it = 0;

        if (path[i]=='/' && i>0) {
            path_copy[i] = '\0';
            try_it = 1;
        } else if (path[i]!='/' && i+1==len) { /* last one */
            try_it = 1;
        }
        
        if ( try_it ) {
            if ( stat (path_copy, &buf) == -1 ) {
                printf ("trying to create path %s\n", path_copy);
                if ( mkdir (path_copy, mode) == -1) {
                    printf ("error: failed to create path %s\n", path_copy);
                    return errno;
                }
            }
            path_copy[i] = '/'; /* restore the slash */
        }
    }

	free (path_copy);
    return 0;
}

/* if path=A/B/C/D but only A exists, this will try to create B and C, but not D */
int ensure_subdirectory_exists (const char * path)
{
    int len = strlen(path);
    char * path_copy = strdup(path);
    int i;

    if (path_copy==NULL) return errno;

    for (i=len-1; i>0; i--) {
		if (path[i]=='/') {
			path_copy[i] = '\0';
			ensure_path_exists (path_copy);
			break;
		}
	}
	
	free (path_copy);
	return 0;	
}

/* wait for file 'appear' to appear or for file 'disappear' to disappear */
static int wait_for_file (const char * appear, const char * disappear, const int iterations, const char * name)
{
    int done, i;
    if (!appear && !disappear) return 1;

    for ( i=0, done=0; i<iterations && !done; i++ ) {
		struct stat mystat;
        sem_p (sc_sem);
        int check = ( (appear    && (stat (appear,    &mystat)==0)) ||
                      (disappear && (stat (disappear, &mystat)!=0)) );
        sem_v (sc_sem);
        if (check) {
            done++;
        } else {
	  		if (i==0) {
	    		logprintfl (EUCAINFO, "waiting for %s to become ready...\n", name);
	  		}
            sleep (10);
        }
    }

    if (!done) {
        logprintfl (EUCAERROR, "ERROR: timed out waiting for %s to become ready\n", name);
        return 1;
    }
    return 0;
}

static int get_cached_file (const char * user_id, const char * url, const char * file_id, const char * instance_id, const char * file_name, char * file_path) 
{
	char cached_dir   [BUFSIZE];
	char cached_path  [BUFSIZE];
	char staging_path [BUFSIZE];
	char digest_path  [BUFSIZE];
	
	snprintf (file_path,    BUFSIZE, "%s/%s/%s/%s",    sc_instance_path, user_id, instance_id, file_name);
	snprintf (cached_dir,   BUFSIZE, "%s/%s/cache/%s", sc_instance_path, EUCALYPTUS_ADMIN, file_id); /* cache is in admin's directory */
	snprintf (cached_path,  BUFSIZE, "%s/%s",          cached_dir, file_name);
	snprintf (staging_path, BUFSIZE, "%s-staging",     cached_path);
	snprintf (digest_path,  BUFSIZE, "%s-digest",      cached_path);

retry:

    /* under a lock, figure out the state of the file */
    sem_p (sc_sem); /***** acquire lock *****/
    ensure_path_exists (cached_dir); /* creates missing directories */

	struct stat mystat;
    int cached_exists  = ! stat (cached_path, &mystat);
    int staging_exists = ! stat (staging_path, &mystat);

    int action;
    enum { ABORT, VERIFY, WAIT, STAGE };
    if ( staging_exists ) {
        action = WAIT;
    } else {
        if ( cached_exists ) {
            action = VERIFY;
        } else {
            action = STAGE;
        }
    }
    
    /* while still under lock... */
    if (action==STAGE) { 
        if ( touch (staging_path) ) /* indicate that we'll be downloading it */
            action = ABORT;
    }
    sem_v (sc_sem); /***** release lock *****/
    
	int e = ERROR;
    switch (action) {
    case STAGE:
        /* TODO: purge expired items from the cache */
        logprintfl (EUCAINFO, "bringing file %s into the cache...\n", cached_path);		
        e = walrus_image_by_manifest_url (url, cached_path); /* get the file */
        if (!e) e=walrus_object_by_url (url, digest_path); /* get the digest */

        sem_p (sc_sem);
        unlink (staging_path);
        if ( e ) {
            logprintfl (EUCAERROR, "error: failed to download the file from Walrus\n");
            unlink (cached_path);
            unlink (digest_path);
            if ( rmdir (cached_dir) ) {
                logprintfl (EUCAWARN, "warning: failed to remove cache directory %s\n", cached_dir);
            }
        }
        sem_v (sc_sem);
        if (e) return e;
        /* yes, it is OK to fall through */
        
    case WAIT:
        logprintfl (EUCAINFO, "waiting for disapperance of %s...\n", staging_path);
        /* wait for staging_path to disappear, which means both that either the
         * download succeeded or it failed */
        if ( (e=wait_for_file (NULL, staging_path, 180, "cached image")) ) 
            return e;        
        /* yes, yes, it is OK to fall through */
        
    case VERIFY:
        logprintfl (EUCAINFO, "verifying cached file in %s...\n", cached_path);
        sem_p (sc_sem); /***** acquire lock *****/
        e = ERROR;
        if ( stat (cached_path, &mystat ) != 0 ) {
            logprintfl (EUCAERROR, "error: file %s not found\n", cached_path);
        } else if (mystat.st_size < 1) {
            logprintfl (EUCAERROR, "error: file %s has the size of 0\n", cached_path);
        } else if ((e=walrus_verify_digest (url, digest_path))<0) {
            /* negative status => digest changed */
            unlink (cached_path);
            unlink (staging_path); /* TODO: needed? */
            unlink (digest_path);
            if ( rmdir (cached_dir) ) {
                logprintfl (EUCAWARN, "warning: failed to remove cache directory %s\n", cached_dir);
            } else {
                logprintfl (EUCAINFO, "due to failure, removed cache directory %s\n", cached_dir);
            }
        }
        sem_v (sc_sem); /***** release lock *****/

        if (e<0) { /* digest changed */
            if (action==VERIFY) { /* i.e. we did not download/waited for this file */
                /* try downloading anew */
                goto retry;
            } else {
                logprintfl (EUCAERROR, "error: digest mismatch, giving up\n");
                return ERROR;
            }
        } else if (e>0) { /* problem with file or digest */
            return ERROR;
            
        } else { /* all good - copy it, finally */
            ensure_subdirectory_exists (file_path); /* creates missing directories */
            if ( (e=run ("cp", "-a", cached_path, file_path, 0)) != 0) {
                logprintfl (EUCAERROR, "failed to copy cached file %s into run file %s\n", cached_path, file_path);
                return e;
            }
        }
        break;

    case ABORT:
        logprintfl (EUCAERROR, "get_cached_file() failed (errno=%d)\n", e);
    }
    return e;
}


int scMakeInstanceImage (char *userId, char *imageId, char *imageURL, char *kernelId, char *kernelURL, char *ramdiskId, char *ramdiskURL, char *instanceId, char *keyName, char **instance_path, sem * s, int convert_to_disk) 
{
  char rundir_path  [BUFSIZE];
  char image_path   [BUFSIZE];
  char kernel_path  [BUFSIZE];
  char ramdisk_path [BUFSIZE];
  char config_path  [BUFSIZE];
  int e;
  
  logprintfl (EUCAINFO, "retrieving images for instance %s...\n", instanceId);
  
  /* get the necessary files from Walrus, caching them if necessary */
  if ((e=get_cached_file (userId, imageURL, imageId, instanceId, "root", image_path))!=0) return e;
  if ((e=get_cached_file (userId, kernelURL, kernelId, instanceId, "kernel", kernel_path))!=0) return e;
  if (ramdiskId && strnlen (ramdiskId, CHAR_BUFFER_SIZE) ) {
    if ((e=get_cached_file (userId, ramdiskURL, ramdiskId, instanceId, "ramdisk", ramdisk_path))!=0) return e;
  }
  snprintf (rundir_path, BUFSIZE, "%s/%s/%s", sc_instance_path, userId, instanceId);
  logprintfl (EUCAINFO, "preparing images for instance %s...\n", instanceId);

  /* embed the key, which is contained in keyName */
  if (keyName && strlen(keyName)) {
    int key_len = strlen(keyName);
    char *key_template = NULL;
    int fd = -1;
    int ret;
    
    key_template = strdup("/tmp/sckey.XXXXXX");
    
    if (((fd = mkstemp(key_template)) < 0)) {
      logprintfl (EUCAERROR, "failed to create a temporary key file\n"); 
    } else if ((ret = write (fd, keyName, key_len))<key_len) {
      logprintfl (EUCAERROR, "failed to write to key file %s write()=%d\n", key_template, ret);
    } else {
      close (fd);
      logprintfl (EUCAINFO, "adding key %s to the root file system at %s using (%s)\n", key_template, image_path, add_key_command_path);
      sem_p (s);
      if ((e=run(add_key_command_path, image_path, key_template, 0))!=0) {
	logprintfl (EUCAERROR, "ERROR: key injection command failed\n");
      }
      sem_v (s);
      
      /* let's remove the temporary key file */
      if (unlink(key_template) != 0) {
	logprintfl (EUCAWARN, "WARNING: failed to remove temporary key file %s\n", key_template);
      }
    }
    
    /* TODO: handle errors! */
    if (key_template) free(key_template);
    if (e) return e;
    
  } else {
    sem_p (s);
    if ((e=run(add_key_command_path, image_path, 0))!=0) { /* without key, add_key just does tune2fs */
      logprintfl (EUCAWARN, "WARNING: failed to prepare the superblock of the root disk image\n");
    }
    sem_v (s);
    /* printf ("no user public key to embed, skipping\n"); */
  }

  if (!convert_to_disk) {
    /* create swap */
    int swap_size_mb = default_swap_size; /* TODO: set this dynamically */
    if (swap_size_mb) { 
      if ((e=vrun ("dd bs=1M count=%d if=/dev/zero of=%s/swap 2>/dev/null", swap_size_mb, rundir_path)) != 0) { 
	logprintfl (EUCAINFO, "creation of swap (dd) at %s/swap failed\n", rundir_path);
	return e;
      }
      if ((e=vrun ("mkswap %s/swap >/dev/null", rundir_path)) != 0) {
	logprintfl (EUCAINFO, "initialization of swap (mkswap) at %s/swap failed\n", rundir_path);
	return e;		
      }
    }
  } else {
  }

  if (!convert_to_disk) {
    /* create ephemeral disk */
    int ephemeral_size_mb = default_ephemeral_size; /* TODO: set this dynamically */
    if (ephemeral_size_mb) {
      if ((e=vrun ("dd bs=1M seek=%d if=/dev/zero of=%s/ephemeral 2>/dev/null", ephemeral_size_mb, rundir_path )) != 0) {
	logprintfl (EUCAINFO, "creation of ephemeral disk (dd) at %s/ephemeral failed\n", rundir_path);
	return e;
      }
      if ((e=vrun ("mkfs.ext3 -F %s/ephemeral >/dev/null 2>&1", rundir_path)) != 0) {
	logprintfl (EUCAINFO, "initialization of ephemeral disk (mkfs.ext3) at %s/ephemeral failed\n", rundir_path);
	return e;		
      }
    }
  } else {
  }

  if (!convert_to_disk) {
  } else {
    // need to convert partition to disk image with partition table (for KVM)
    sem_p (s);
    if ((e=run(disk_convert_command_path, image_path))!=0) {
      logprintfl (EUCAERROR, "ERROR: partition to disk image conversion command failed\n");
    }
    sem_v (s);
    if (e) return(e);
  }  
  
  * instance_path = strdup (rundir_path);
  if (*instance_path==NULL) return errno;
  return 0;
}

int scStoreStringToInstanceFile (const char *userId, const char *instanceId, const char * file, const char * data)
{
    FILE * fp;
	char path [BUFSIZE];
	snprintf (path, BUFSIZE, "%s/%s/%s/%s", sc_instance_path, userId, instanceId, file);
    if ( (fp = fopen (path, "w")) != NULL ) {
        if ( fputs (data, fp) == EOF ) return ERROR;
        fclose (fp);
        return OK;
    }
    return ERROR;
}
