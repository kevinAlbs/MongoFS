/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall hello.c `pkg-config fuse --cflags --libs` -o hello
*/

#define FUSE_USE_VERSION 26

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <fuse.h>
#include <mongoc.h>

#include <time.h>

typedef struct {
   bool exists;
   bool is_bad;
   bool is_dir;
   char id[25];
   int skips;
} parsed_path_t;

enum log_type_t {LOG_INFO='I', LOG_WARNING='W', LOG_ERROR='E', LOG_DEBUG='D'};

#define MONGOFS_INFO(...) mongofs_log(LOG_INFO, __VA_ARGS__)
#define MONGOFS_WARNING(...) mongofs_log(LOG_WARNING, __VA_ARGS__)
#define MONGOFS_ERROR(...) mongofs_log(LOG_ERROR, __VA_ARGS__)
#define MONGOFS_DEBUG(...) mongofs_log(LOG_DEBUG, __VA_ARGS__)

struct options_t {
   // Set with --tee
   bool tee; // whether or not to output logs to stdout/stderr as well as log file.
   // Set with --log_file=<file path>
   FILE* log_file; // for fprintf
   // Set with --ns=<namespace string>
   char namespace[120]; // See manual/reference/limits. Includes dot.
} opts;

void mongofs_log(enum log_type_t logtype, const char* format, ...) {
   // Prefix with time.
   time_t now_time = time(NULL);
   char* now_str = ctime(&now_time); // 26 chars including trailing \n + \0
   struct tm* time = localtime(&now_time);
   now_str[19] = '\0'; // remove year.
   clock_t now_clock = clock();
   if (CLOCKS_PER_SEC > 1000) {
      now_clock /= (CLOCKS_PER_SEC / 1000); // should give ms.
   }
   now_clock = now_clock % 1000;
   fprintf(opts.log_file, "[%c %s:%04d] ", logtype, now_str, now_clock);

   FILE* stddes = logtype == LOG_ERROR ? stderr : stdout;
   fprintf(stddes, "[%c %s:%04d] ", logtype, now_str, now_clock);

   // Forward rest of args to printf.
   va_list args, copy;
   va_start(args, format);
   va_copy(copy, args);
   vfprintf (opts.log_file, format, args);
   va_end(args);
   va_start(copy, format);
   vfprintf (stddes, format, args);
   va_end(copy);

}
// fuse may run in multi-threaded mode.
// TODO: I may need to (a) enforce single threaded or (b) use client_pool
mongoc_client_t* client;
#define FUSE_USE_VERSION 26

// number of documents to be shown in a directory. -1 for all
static int batch_size = 1000;

parsed_path_t parse_path(const char* path) {
   const char* path_iter = path;
   parsed_path_t parsed = {0};
   while (strstr(path_iter, "/it") != NULL) {
      parsed.skips++;
      path_iter += 3;
   }

   // If this is a directory, the rest should be a slash.
   int remaining = strlen(path_iter);
   if (remaining == 1 && path_iter[0] == '/') {
      parsed.exists = true;
      parsed.is_bad = false;
      parsed.is_dir = true;
   }
   else if (remaining == 24 + 1) {
      // TODO: query to check for existence
      parsed.exists = true;
      parsed.is_bad = false;
      parsed.is_dir = false;
      strncpy(parsed.id, path_iter + 1, 24);
      parsed.id[24] = '\0';
   } else {
      parsed.exists = false;
      parsed.is_bad = true;
      parsed.is_dir = false;
   }
   return parsed;
}

// if this returns 0, then *out must be freed.
int get_bson_string(const char* id, char** out, int* outlen) {
   int exit_status = 0;
   if (!bson_oid_is_valid(id, 24)) {
      printf("malformed oid\n");
      return -ENOENT;
   }

   *out = NULL;

   bson_oid_t oid;
   bson_oid_init_from_string (&oid, id);

   bson_t query;
   bson_init(&query);
   bson_append_oid(&query, "_id", 3, &oid);

   bson_t opts;
   bson_init(&opts);

   mongoc_collection_t* coll = mongoc_client_get_collection (client, "test", "coll");
   mongoc_cursor_t* cursor = mongoc_collection_find_with_opts (coll, &query, &opts, NULL);

   bson_destroy (&query);
   bson_destroy (&opts);

   bson_error_t err;

   const bson_t *out_doc;
   if (mongoc_cursor_next(cursor, &out_doc)) {
      *out = bson_as_canonical_extended_json (out_doc, (size_t*)outlen);
   }
   else if (mongoc_cursor_error(cursor, &err)) {
      exit_status = -EIO;
      goto cleanup;
   }
   else {
      printf("doc with id %s not found\n", id);
      exit_status = -ENOENT;
      goto cleanup;
   }

cleanup:
   if (exit_status != 0 && *out) bson_free(*out);
   mongoc_collection_destroy(coll);
   mongoc_cursor_destroy(cursor);
   return exit_status;
}

typedef struct {
   bool free;
   bool dirty; /* if this file has been modified */
   bool open; /* if this is a file that's been open'ed but not release'ed */
   char* data;
   int size;
   char id[24];
} cached_bson_t;

cached_bson_t cache[100];

void init_cache() {
   for (int i = 0; i < 100; i++) {
      cache[i] = (cached_bson_t){0};
      cache[i].free = true;
   }
}

// TODO: change id to id_str
int get_cached(char* id, bool add_if_not_found, cached_bson_t** out) {
   MONGOFS_DEBUG("get_cached %s\n", id);

   *out = NULL;

   // check if id is in cache
   int i;
   for (i = 0; i < 100; i++) {
      if (!cache[i].free && strncmp(cache[i].id, id, 24) == 0) {
         *out = cache + i;
         return 0;
      }
   }

   // if user just wants cache entry only if it exists, then return NULL
   if (!add_if_not_found) return 0;

   // otherwise, add a new cache entry
   for (i = 0; i < 100; i++) {
      if (cache[i].free) break;
   }

   if (i == 100) {
      MONGOFS_DEBUG("error, no free cache results");
      // Try to find something that we can free from the cache.
      // First free items where open=false and dirty=false
      // Then free items where open=false and dirty=true
      // Otherwise, return error code.
      for (i = 0; i < 100; i++) {
         if (!cache[i].open && !cache[i].dirty) {
            bson_free(cache[i].data);
            cache[i].free = true;
            break;
         }
      }

      if (i == 100) {
         for (i = 0; i < 100; i++) {
            if (!cache[i].open && cache[i].dirty) {
               bson_free(cache[i].data);
               cache[i].free = true;
               break;
            }
         }
      }

      if (i == 100) {
         // return error.
         return -EIO;
      }
   }

   cache[i].free = false;
   cache[i].dirty = false;
   cache[i].open = false;
   int exit_status  = get_bson_string(id, &cache[i].data, &cache[i].size);
   strncpy(cache[i].id, id, 24);
   if (exit_status != 0) return exit_status;
   *out = cache + i;
   return 0;
}

void remove_if_cached(char* id) {
   MONGOFS_DEBUG("remove_cached %s\n", id);
   // check if id is in cache
   int i;
   for (i = 0; i < 100; i++) {
      if (!cache[i].free && strncmp(cache[i].id, id, 24) == 0) {
         bson_free(cache[i].data);
         cache[i].free = true;
         return;
      }
   }
   // no-op.
   MONGOFS_DEBUG(" no-op");
}

static int mongofs_getattr(const char *path, struct stat *stbuf)
{
   MONGOFS_DEBUG("getattr %s \n", path);
   memset(stbuf, 0, sizeof(struct stat));

   parsed_path_t parsed = parse_path(path);

   if (parsed.is_bad || !parsed.exists)
      return -ENOENT;

   // is marking .localized a directory causing a hang?

   if (parsed.is_dir) {
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;
   } else {
      cached_bson_t* cached;
      int exit_status = get_cached(parsed.id, true, &cached);
      if (exit_status != 0) return exit_status;
      stbuf->st_mode = S_IFREG | 0666;
      stbuf->st_nlink = 1;
      stbuf->st_size = cached->size;
   }

   return 0;
}

static int mongofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi)
{
   // TODO: why?
   // (void) offset;
   // (void) fi;

   MONGOFS_INFO("readdir %s\n", path);

   filler(buf, ".", NULL, 0);
   filler(buf, "..", NULL, 0);

   const bson_t* doc;
   bson_t empty = BSON_INITIALIZER;

   int exit_status = 0; // the error code the function returns with

   parsed_path_t parsed = parse_path(path);

   if (parsed.is_bad || !parsed.is_dir)
      return -ENOENT;   

   bson_t* opts = BCON_NEW(
      "skip", BCON_INT64(parsed.skips * batch_size),
      "limit", BCON_INT32(batch_size),
      "projection", "{", "_id", BCON_INT32(1), "}");

   mongoc_collection_t* coll = mongoc_client_get_collection (client, "test", "coll");
   mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &empty, opts, NULL );

   bson_free(opts);
   
   while (mongoc_cursor_next(cursor, &doc)) {
      bson_iter_t iter;
      if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter)) {
         const bson_oid_t *oid = bson_iter_oid (&iter);
         bson_t tmp = BSON_INITIALIZER;
         bson_append_oid(&tmp, "", 0, oid);
         // TODO: is there a better way to get object id as a string.
         char* asStr = bson_as_json(&tmp, 0);
         asStr[19 + 24] = '\0';
         // Use the non-offset form of the directory filler (pass 0 as offset)
         // for simplicity.
         int ret = filler(buf, asStr + 19, NULL, 0);
         bson_free(asStr);
         if (ret != 0) {
            exit_status = -EIO;
            goto cleanup;
         }
      }
   }

   bson_error_t err;
   if (mongoc_cursor_error(cursor, &err)) {
      MONGOFS_ERROR("error %s\n", err.message);
      exit_status = -EIO;
      goto cleanup;
   }

cleanup:
   mongoc_cursor_destroy(cursor);
   mongoc_collection_destroy(coll);

   return exit_status;
}

static int mongofs_truncate(const char *path, off_t offset) {
   MONGOFS_DEBUG("truncate %s", path);
   parsed_path_t parsed = parse_path(path);

   if (parsed.is_bad || !parsed.exists) return -ENOENT;

   cached_bson_t* cached;
   int exit_status = get_cached(parsed.id, true, &cached);
   if (exit_status != 0) return exit_status;

   cached->dirty = true;
   cached->data = (char*)bson_realloc(cached->data, offset);

   // `man 2 truncate`
   // If the file size is smaller than length, the file is extended and filled with zeros to the indicated length
   if (cached->size < offset) {
      MONGOFS_DEBUG("need to increase data size");
      // set everything from [cached->size, offset) to zero
      memset(cached->data + cached->size, 0, offset - cached->size);
   }

   cached->size = offset;
   return 0;
}

static int mongofs_open(const char *path, struct fuse_file_info *fi)
{
   // see `man 2 open` for flags

   parsed_path_t parsed = parse_path(path);

   MONGOFS_DEBUG("opening %s\n", path);

   if (parsed.is_bad || !parsed.exists)
      return -ENOENT;

   cached_bson_t* cached;
   int exit_status = get_cached(parsed.id, true, &cached);
   if (exit_status != 0) return exit_status;

   cached->open = true;

   // Can open file for writing
//   if ((fi->flags & 3) != O_RDONLY)
//      return -EACCES;

   // return a file descriptor.
   // TODO: use fd to keep track of which bson_t's to cache in memory
   fi->fh = 10;
   return 0;
}

// this queries *every* time a read is issued.
// instead, we should cache at least the most recently read document.
static int mongofs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
   MONGOFS_DEBUG("reading %s\n", path);
   (void) fi;
   // path should be of the form /it/it/it/....<id>
   parsed_path_t parsed = parse_path(path);

   if (parsed.is_bad || !parsed.exists) return -ENOENT;

   cached_bson_t* cached;
   int exit_status = get_cached(parsed.id, true, &cached);
   if (exit_status != 0) return exit_status;

   int bytes_to_copy = size;
   if (offset + size > cached->size) {
      // this is the last read.
      bytes_to_copy = cached->size - offset; // might be zero
   }
   memcpy(buf, cached->data + offset, bytes_to_copy);
   return bytes_to_copy;
}

static int mongofs_write(const char *path, const char* data, size_t size, off_t offset, struct fuse_file_info_t* fi) {
   MONGOFS_DEBUG("writing %s\n", path);
   parsed_path_t parsed = parse_path(path);
   if (parsed.is_bad || !parsed.exists) {
      return -ENOENT;
   }

   cached_bson_t* cached;
   int exit_status = get_cached(parsed.id, true, &cached);
   if (exit_status != 0) return exit_status;

   if (offset + size > cached->size) {
      cached->data = bson_realloc(cached->data, offset + size);
      cached->size = offset + size;
   }

   memcpy(cached->data + offset, data, size);

   return size;
}

static int mongofs_release(const char* path, struct fuse_file_info_t* fi) {
   MONGOFS_DEBUG("mongofs_release %s\n", path);
   parsed_path_t parsed = parse_path(path);

   if (parsed.is_bad || !parsed.exists) return -ENOENT;

   cached_bson_t* cached;
   int exit_status = get_cached(parsed.id, true, &cached);
   if (exit_status != 0) return exit_status;

   cached->open = false;

   bool flush_from_cache = true;

   if (cached->dirty) {
      // write to mongoc driver
      MONGOFS_DEBUG("cached doc is dirty, attempting to write\n");
      // attempt to create bson.
      bson_t bson;
      bson_error_t err;
      if (bson_init_from_json(&bson, cached->data, cached->size, &err)) {
         mongoc_collection_t* coll = mongoc_client_get_collection (client, "test", "coll");
         bson_oid_t oid;
         bson_oid_init_from_string (&oid, cached->id);

         bson_t filter;
         bson_init(&filter);
         bson_append_oid(&filter, "_id", 3, &oid);

         bool res = mongoc_collection_remove(coll, MONGOC_REMOVE_SINGLE_REMOVE, &filter, NULL, &err);
         if (res) {
            mongoc_collection_insert (coll, MONGOC_INSERT_NONE, &bson, NULL, &err);
            // TODO: if this fails then we should probably re-insert the original.
         } else {
            flush_from_cache = false;
         }
      } else {
         // Don't flush yet because user may reopen and edit.
         flush_from_cache = false;
      }
   }

   if (flush_from_cache) remove_if_cached(parsed.id);
}

// Mode 1: separate file per doc.
static struct fuse_operations mongofs_oper = {
   .getattr	= mongofs_getattr,
   .readdir	= mongofs_readdir,
   .open		= mongofs_open,
   .read		= mongofs_read,
   .truncate = mongofs_truncate,
   .write = mongofs_write,
   .release = mongofs_release
};

// Mode 2: separate file per collection.
static struct fuse_operations mongofs_oper_single_file = {

};

void help()
{
   printf ("Usage: mongofs --ns=<namespace> [--tee] [--log_file=<path>]\n");
}

int main(int argc, char *argv[])
{
   char* log_path="mongofs.log";
   // Parse options.
   opts.tee = 0;
   opts.namespace[0] = '\0';
   for (int i = 1; i < argc; i++) {
      char* value;
      if (value = strstr(argv[i], "--ns=")) {
         value += 5;
         if (strlen(value) > 120) {
            fprintf(stderr, "namespace must be <= 120 chars\n");
            exit(1);
         }
         strcpy(opts.namespace, value);
      } else if (value = strstr(argv[i], "--log_file=")) {
         value += 11;
         log_path = value;
      } else if (strcmp("--tee", argv[i]) == 0) {
         opts.tee = 1;
      } else if (strcmp("--help", argv[i]) == 0) {
         help();
         exit(0);
      }
   }

   if (strlen(opts.namespace) == 0) {
      help();
      exit(1);
   }

   opts.log_file = fopen(log_path, "a");
   printf("Mounting namespace '%s'\n", opts.namespace);
   printf("Logging output to '%s'", log_path);
   if (opts.tee) printf(" and stdout/stderr");
   printf("\n");

   mongoc_init();
//   MONGOFS_LOG("test\n");
//   init_cache();
//   client = mongoc_client_new("mongodb://localhost:27017");
//   printf("testing before fuse_main\n");
//   return fuse_main(argc, argv, &mongofs_oper, NULL);
   MONGOFS_INFO("this is a test");
}