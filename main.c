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

typedef struct {
   bool exists;
   bool is_bad;
   bool is_dir;
   char id[25];
   int skips;
} parsed_path_t;

#define MONGOFS_DEBUG(...) printf(__VA_ARGS__)

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
      *out = bson_as_json(out_doc, (size_t*)outlen);
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
      return 0;
   }

   cache[i].free = false;
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

   printf("readdir\n");
   //return 0;

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
      if (bson_iter_init_find(&iter, doc, "_id")) {
         const bson_oid_t *oid = bson_iter_oid (&iter);
         bson_t tmp = BSON_INITIALIZER;
         bson_append_oid(&tmp, "", 0, oid);
         // TODO: see if there is a better way to get object id as a string.
         char* asStr = bson_as_json(&tmp, 0);
         asStr[19 + 24] = '\0';
         // TODO: filler may return 1 if buf is full. Instead, change this to use the offset form
         // of readdir so filled buffer won't break.
         int ret = filler(buf, asStr + 19, NULL, 0);
         printf("adding %s\n", asStr + 19);
         //bson_free(asStr);
         if (ret != 0) {
            exit_status = -EIO;
            goto cleanup;
         }
      }
   }

   bson_error_t err;
   if (mongoc_cursor_error(cursor, &err)) {
      exit_status = -EIO;
      goto cleanup;
   }

cleanup:
   mongoc_cursor_destroy(cursor);
   mongoc_collection_destroy(coll);

   return exit_status;
}


static int mongofs_truncate(const char *path, off_t offset) {
   // TODO: ignore
   return 0;
}
static int mongofs_open(const char *path, struct fuse_file_info *fi)
{
   // see `man 2 open` for flags

   parsed_path_t parsed = parse_path(path);

   if (parsed.is_bad || !parsed.exists)
      return -ENOENT;

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

   // update the in memory representation.
   //char* as_str;
   //int length;
   //int exit_status = get_bson_string(parsed.id, &as_str, &length);

   MONGOFS_DEBUG("write called with data %s of size %zu", data, size);

   // replace the text using offset/size and rewrite
//
//   if (exit_status != 0) {
//      return exit_status;
//   }
   return 0;
}

static struct fuse_operations mongofs_oper = {
   .getattr	= mongofs_getattr,
   .readdir	= mongofs_readdir,
   .open		= mongofs_open,
   .read		= mongofs_read,
   .truncate = mongofs_truncate,
   .write = mongofs_write,
};

int main(int argc, char *argv[])
{
   mongoc_init();
   init_cache();
   client = mongoc_client_new("mongodb://localhost:27017");
   printf("testing before fuse_main\n");
   return fuse_main(argc, argv, &mongofs_oper, NULL);

}