/*							       	    HTCache.c
**	CACHE WRITER
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**
**	This modules manages the cache
**
**      History:
**         HFN: spawned from HTFwrite
**         HWL: converted the caching scheme to be hierachical by taking
**              AL code from Deamon
**
*/

/* Library include files */
#include "tcp.h"
#include "HTUtils.h"
#include "HTString.h"
#include "HTFormat.h"
#include "HTFWrite.h"
#include "HTBind.h"
#include "HTList.h"
#include "HTParse.h"
#include "HTCache.h"					 /* Implemented here */

/*
** The cache limit is the number of files which are kept. Yes, I know,
** the amount of disk space would be more relevant. So this may change.
** Currently it is preset to 100 but may be changed by the application by
** writing into this variable.
*/
#define CACHE_LIMIT	5				  /* Number of files */

#define CACHE_INFO	".cache_info"
#define INDEX_FILE	".cache_dirindex"
#define WELCOME_FILE	".cache_welcome"
#define TMP_SUFFIX	".cache_tmp"
#define LOCK_SUFFIX	".cache_lock"

typedef struct _HTCacheItem {
    HTFormat		format;		/* May have many formats per anchor */
    char *		filename;
    time_t		start_time;
    time_t		load_delay;
    int			reference_count;
} HTCacheItem;

struct _HTStream {
    CONST HTStreamClass *	isa;
    FILE *			fp;
    HTCacheItem *		cache;
    HTRequest *			request;
};

PRIVATE BOOL		HTCacheEnable = NO;
PRIVATE char *		HTCacheRoot = NULL;  	    /* Destination for cache */
PRIVATE HTList *	HTCache = NULL;		  /* List of cached elements */
PRIVATE int		HTCacheLimit = CACHE_LIMIT;

/* ------------------------------------------------------------------------- */
/*  				 CACHE MANAGER				     */
/* ------------------------------------------------------------------------- */

/*
**  Removes cache item from disk and corresponding object from list in memory
*/
PRIVATE void HTCache_remove ARGS1(HTCacheItem *, item)
{
    if (HTCache && item) {
	if (CACHE_TRACE)
	    fprintf(TDEST, "Cache....... Removing %s\n", item->filename);
	HTList_removeObject(HTCache, item);
	REMOVE(item->filename);
	
	/* HWL 22/9/94: Clean up hierachical file structure */
	{
	    char * p;
	    while ((p = strrchr(item->filename,'/')) && (p != NULL)){
		item->filename[p-item->filename] = 0;
		if (strcmp(item->filename, HTCacheRoot) != 0) {
		    if (CACHE_TRACE) 
			fprintf(TDEST, "rmdir....... %s\n", item->filename);
		    RMDIR(item->filename); /* fails if directory isn't empty */
		}
	    }
	}
	free(item->filename);
	free(item);
    }
}


/*
**  Remove a file from the cache to prevent too many files from being cached
*/
PRIVATE void limit_cache ARGS1(HTList * , list)
{
    HTList * cur = list;
    HTCacheItem * item;
    time_t best_delay = 0;   /* time_t in principle can be any arith type */
    HTCacheItem* best_item = NULL;

    if (HTList_count(list) < HTCacheLimit) return;   /* Limit not reached */

    while (NULL != (item = (HTCacheItem*)HTList_nextObject(cur))) {
        if (best_delay == 0  ||  item->load_delay < best_delay) {
            best_delay = item->load_delay;
            best_item = item;
        }
    }
    if (best_item) HTCache_remove(best_item);
}

/*
**	Check that the name we're about to generate doesn't
**	clash with anything used by the caching system.
*/
PRIVATE BOOL reserved_name ARGS1(char *, url)
{
    char * name = strrchr(url, '/');
    char * suff = NULL;

    if (name) name++;
    else name = url;

    if (!strcmp(name, CACHE_INFO) ||
	!strcmp(name, INDEX_FILE) ||
	!strcmp(name, WELCOME_FILE))
	return YES;

    suff = strrchr(name, TMP_SUFFIX[0]);
    if (suff && !strcmp(suff, TMP_SUFFIX))
	return YES;

    suff = strrchr(name, LOCK_SUFFIX[0]);
    if (suff && !strcmp(suff, LOCK_SUFFIX))
	return YES;

    return NO;
}

/*
**	Map url to cache file name.
*/
PRIVATE char * cache_file_name ARGS1(char *, url)
{
    char * access = NULL;
    char * host = NULL;
    char * path = NULL;
    char * cfn = NULL;
    BOOL welcome = NO;
    BOOL res = NO;

    if (!url ||  strchr(url, '?')  ||  (res = reserved_name(url))  ||
	!(access = HTParse(url, "", PARSE_ACCESS)) ||
	(0 != strcmp(access, "http") &&
	 0 != strcmp(access, "ftp")  &&
	 0 != strcmp(access, "gopher"))) {

	if (access) free(access);

	if (res && CACHE_TRACE)
	    fprintf(TDEST,
		    "Cache....... Clash with reserved name (\"%s\")\n",url);

	return NULL;
    }

    host = HTParse(url, "", PARSE_HOST);
    path = HTParse(url, "", PARSE_PATH | PARSE_PUNCTUATION);
    if (path && path[strlen(path)-1] == '/')
	welcome = YES;

    cfn = (char*)malloc(strlen(HTCacheRoot) +
			strlen(access) +
			(host ? strlen(host) : 0) +
			(path ? strlen(path) : 0) +
			(welcome ? strlen(WELCOME_FILE) : 0) + 3);
    if (!cfn) outofmem(__FILE__, "cache_file_name");

    /* Removed extra slash - HF May2,95 */
    sprintf(cfn, "%s%s/%s%s%s", HTCacheRoot, access, host, path,
	    (welcome ? WELCOME_FILE : ""));

    FREE(access); FREE(host); FREE(path);

    /*
    ** This checks that the last component is not too long.
    ** It could check all the components, but the last one
    ** is most important because it could later blow up the
    ** whole gc when reading cache info files.
    ** Operating system handles other cases.
    ** 64 = 42 + 22  and  22 = 42 - 20  :-)
    ** In other words I just picked some number, it doesn't
    ** really matter that much.
    */
    {
	char * last = strrchr(cfn, '/');
	if (!last) last = cfn;
	if ((int)strlen(last) > 64) {
	    if (CACHE_TRACE)
		fprintf(TDEST, "Too long.... cache file name \"%s\"\n", cfn);
	    free(cfn);
	    cfn = NULL;
	}
    }
    return cfn;
}


/*
**	Create directory path for cache file
**
** On exit:
**	return YES
**		if directories created -- after that caller
**		can rely on fopen(cfn,"w") succeeding.
**
*/
PRIVATE BOOL create_cache_place ARGS1(char *, cfn)
{
    struct stat stat_info;
    char * cur = NULL;
    BOOL create = NO;

    if (!cfn  ||  (int)strlen(cfn) <= (int)strlen(HTCacheRoot) + 1)
	return NO;

    cur = cfn + strlen(HTCacheRoot) + 1;

    while ((cur = strchr(cur, '/'))) {
	*cur = 0;
	if (create || HTStat(cfn, &stat_info) == -1) {
	    create = YES;	/* To avoid doing stat()s in vain */
	    if (CACHE_TRACE)
		fprintf(TDEST,"Cache....... creating cache dir \"%s\"\n",cfn);
	    if (MKDIR(cfn, 0777) < 0) {
		if (CACHE_TRACE)
		    fprintf(TDEST,"Cache....... can't create dir `%s\'\n",cfn);
		return NO;
	    }
	} else {
	    if (S_ISREG(stat_info.st_mode)) {
		int len = strlen(cfn);
		char * tmp1 = (char*)malloc(len + strlen(TMP_SUFFIX) + 1);
		char * tmp2 = (char*)malloc(len + strlen(INDEX_FILE) + 2);
		/* time_t t1,t2,t3,t4,t5; */


		sprintf(tmp1, "%s%s", cfn, TMP_SUFFIX);
		sprintf(tmp2, "%s/%s", cfn, INDEX_FILE);

		if (CACHE_TRACE) {
		    fprintf(TDEST,"Cache....... moving \"%s\" to \"%s\"\n",
			    cfn,tmp1);
		    fprintf(TDEST,"and......... creating dir \"%s\"\n",
			    cfn);
		    fprintf(TDEST,"and......... moving \"%s\" to \"%s\"\n",
			    tmp1,tmp2);
		}
		rename(cfn,tmp1);
		(void) MKDIR(cfn, 0777);
		rename(tmp1,tmp2);
		free(tmp1);
		free(tmp2);
	    }
	    else {
		if (CACHE_TRACE)
		    fprintf(TDEST,"Cache....... dir \"%s\" already exists\n",
			    cfn);
	    }
	}
	*cur = '/';
	cur++;
    }
    return YES;
}


/*	Create a cache path
**	-------------------
**	Find a full path name for the cache file and create the path if it
**	does not already exist. Returns name or NULL
**	HWL 22/9/94
**	HWL added support for hierachical structure
*/
PRIVATE char *HTCache_getName ARGS1(char *, url)
{
    char *filename = cache_file_name(url);
    if (!filename)
	return NULL;
    if (create_cache_place(filename))
	return(filename);
    return NULL;
}

/*
**  Make a WWW name from a cache name and returns it if OK, else NULL.
**  The string returned must be freed by the caller.
**  We keep this function private as we might change the naming scheme for
**  cache files. Right now it follows the file hierarchi.
*/
PRIVATE char *HTCache_wwwName ARGS1 (char *, name)
{
    char * result = NULL;
    if (name && *name) {
	StrAllocCopy(result, "file:");	     /* We get an absolute file name */
#ifdef VMS 
	/* convert directory name to Unix-style syntax */
	char * disk = strchr (name, ':');
	char * dir = strchr (name, '[');
	if (disk) {
	    *disk = '\0';
	    StrAllocCat(result, "/");			  /* needs delimiter */
	    StrAllocCat(result, name);
	}
	if (dir) {
	    char *p;
	    *dir = '/'; 	 		      /* Convert leading '[' */
	    for (p = dir ; *p != ']'; ++p)
		if (*p == '.') *p = '/';
	    *p = '\0';  				 /* Cut on final ']' */
	    StrAllocCat(result, dir);
	}
#else  /* not VMS */
#ifdef WIN32
	char * p = name;					  /* a colon */
	StrAllocCat(result, "/");
	while( *p != 0 ) { 
	    if (*p == '\\')		         /* change to one true slash */
		*p = '/' ;
	    p++;
	}
	StrAllocCat(result, name);
#else /* not WIN32 */
	StrAllocCat (result, name);
#endif /* not WIN32 */
#endif /* not VMS */
    }
    return result;
}


/*
**
**  Verifies if a cache object exists for this URL and if so returns a URL
**  for the cached object. It does not verify whether the object is valid or
**  not, for example it might have been expired.
**
**  Returns: file name	If OK (must be freed by caller)
**	     NULL	If no cache object found
*/
PUBLIC char * HTCache_getObject ARGS1(char *, url)
{
    if (url && HTCache_isEnabled()) {
	char *fnam = cache_file_name(url);
	if (fnam) {
	    FILE *fp = fopen(fnam, "r");
	    if (fp) {
		char *url = HTCache_wwwName(fnam);
		fclose(fp);
		if (CACHE_TRACE)
		    fprintf(TDEST, "Cache....... Object found `%s\'\n", url);
		free(fnam);
		return url;
	    } else
		free(fnam);
	}
    }
    return NULL;
}


/*
**  Removes all cache entries in memory
*/
PUBLIC void HTCache_clearMem NOARGS
{
    HTList *cur=HTCache;
    HTCacheItem *pres;
    if (cur) {
	while ((pres = (HTCacheItem *) HTList_nextObject(cur))) {
	    FREE(pres->filename);
	    free(pres);
	}
	HTList_delete(HTCache);
	HTCache = NULL;
    }
}

/*
**  Removes all cache entries in memory and on disk
*/
PUBLIC void HTCache_deleteAll NOARGS
{
    HTList *cur=HTCache;
    HTCacheItem * pres;
    if (cur) {
	while ((pres = (HTCacheItem *) HTList_lastObject(cur)))
	    HTCache_remove(pres);
	HTList_delete(HTCache);
	HTCache = NULL;
    }
}

/*	Enable Cache
**	------------
**	If `cache_root' is NULL then reuse old value or use HT_CACHE_ROOT.
**	An empty string will make '/' as cache root
*/
PUBLIC BOOL HTCache_enable ARGS1(CONST char *, cache_root)
{
    if (cache_root)
	HTCache_setRoot(cache_root);
    HTCacheEnable = YES;
    return YES;
}


/*	Disable Cache
**	------------
**	Turns off the cache. Note that the cache can be disabled and enabled
**	at any time. The cache root is kept and can be reused during the
**	execution.
*/
PUBLIC BOOL HTCache_disable NOARGS
{
    HTCacheEnable = NO;
    return YES;
}

/*	Is Cache Enabled
**	----------------
**	Returns YES or NO. Also makes sure that we have a root value
**	(even though it might be invalid)
*/
PUBLIC BOOL HTCache_isEnabled NOARGS
{
    if (!HTSecure && HTCacheEnable) {
	if (!HTCacheRoot)
	    HTCache_setRoot(NULL);
	return YES;
    }
    return NO;
}


/*	Set Cache Root
**	--------------
**	If `cache_root' is NULL then the current value (might be a define)
**	Should we check if the cache_root is actually OK? I think not!
*/
PUBLIC BOOL HTCache_setRoot ARGS1(CONST char *, cache_root)
{
    StrAllocCopy(HTCacheRoot, cache_root ? cache_root : HT_CACHE_ROOT);
    if (*(HTCacheRoot+strlen(HTCacheRoot)-1) != '/')
	StrAllocCat(HTCacheRoot, "/");
    if (CACHE_TRACE)
	fprintf(TDEST, "Cache Root.. Root set to `%s\'\n", HTCacheRoot);
    return YES;
}


/*	Get Cache Root
**	--------------
*/
PUBLIC CONST char * HTCache_getRoot NOARGS
{
    return HTCacheRoot;
}


/*	Free Cache Root
**	--------------
**	For clean up memory
*/
PUBLIC void HTCache_freeRoot NOARGS
{
    FREE(HTCacheRoot);
}

/* ------------------------------------------------------------------------- */
/*  			     CACHE WRITER STREAM			     */
/* ------------------------------------------------------------------------- */

PRIVATE int HTCache_flush ARGS1(HTStream *, me)
{
    return (fflush(me->fp) == EOF) ? HT_ERROR : HT_OK;
}

PRIVATE int HTCache_putBlock ARGS3(HTStream *, me, CONST char*, s, int, l)
{
    int status = (fwrite(s, 1, l, me->fp) != l) ? HT_ERROR : HT_OK;
    if (l > 1 && status == HT_OK)
	(void) HTCache_flush(me);
    return status;
}

PRIVATE int HTCache_putChar ARGS2(HTStream *, me, char, c)
{
    return HTCache_putBlock(me, &c, 1);
}

PRIVATE int HTCache_putString ARGS2(HTStream *, me, CONST char*, s)
{
    return HTCache_putBlock(me, s, (int) strlen(s));
}

PRIVATE int HTCache_free ARGS1(HTStream *, me)
{
    me->cache->load_delay = time(NULL)-me->cache->start_time;
    fclose(me->fp);
    free(me);
    return HT_OK;
}

PRIVATE int HTCache_abort ARGS2(HTStream *, me, HTError, e)
{
    if (CACHE_TRACE)
	fprintf(TDEST, "Cache....... ABORTING\n");
    if (me->fp)
	fclose(me->fp);
    if (me->cache)
	HTCache_remove(me->cache);
    free(me);
    return HT_ERROR;
}

PRIVATE CONST HTStreamClass HTCacheClass =
{		
    "Cache",
    HTCache_flush,
    HTCache_free,
    HTCache_abort,
    HTCache_putChar,
    HTCache_putString,
    HTCache_putBlock
};


/*	Cache Writer
**	------------------
**
*/
PUBLIC HTStream* HTCacheWriter ARGS5(
	HTRequest *,		request,
	void *,			param,
	HTFormat,		input_format,
	HTFormat,		output_format,
	HTStream *,		output_stream)

{
    char *fnam;
    HTStream *me;
    if (HTSecure) {
	if (CACHE_TRACE)
	    fprintf(TDEST, "Cache....... No caching in secure mode.\n");
	return HTBlackHole();
    }

    /* Get a file name and open file */
    if ((fnam = HTCache_getName(HTAnchor_physical(request->anchor))) == NULL)
	return HTBlackHole();

    /* Set up the stream */
    if ((me = (HTStream *) calloc(sizeof(*me), 1)) == NULL)
	outofmem(__FILE__, "Cache");
    me->isa = &HTCacheClass;
    me->request = request;
    if ((me->fp = fopen(fnam, "w")) == NULL) {
	if (CACHE_TRACE)
	    fprintf(TDEST, "Cache....... Can't open %s for writing\n", fnam);
	free(fnam);
	return HTBlackHole();
    } else
	if (CACHE_TRACE)
	    fprintf(TDEST, "Cache....... Creating file %s\n", fnam);


    /* Set up a cache record */
    if ((me->cache = (HTCacheItem *) calloc(sizeof(*me->cache), 1)) == NULL)
	outofmem(__FILE__, "Cache");
    me->cache->filename = fnam;
    me->cache->start_time = time(NULL);
    me->cache->format = input_format;

    /* Keep a global list of all cache items */
    if (!HTCache) HTCache = HTList_new();
    HTList_addObject(HTCache, me->cache);
    limit_cache(HTCache);		 /* Limit number (not size) of files */
    return me;
}
