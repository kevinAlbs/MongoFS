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

// fuse may run in multi-threaded mode.
// TODO: I may need to (a) enforce single threaded or (b) use client_pool
mongoc_client_t* client;
#define FUSE_USE_VERSION 26

static const char *hello_str = "Hello World!\n";
static const char *hello_path = "/huh";

// number of documents to be shown in a directory. -1 for all
static int batch_size = 1000;

static int mongofs_getattr(const char *path, struct stat *stbuf)
{
   int res = 0;

   memset(stbuf, 0, sizeof(struct stat));

   bool is_dir = false;
   const char* path_iter = path;
   while (strstr(path_iter, "/it") != NULL) {
      path_iter += 3;
   }

   if (strlen(path_iter) == 0 || path_iter[0] != '/')
      return -ENOENT;

   ++path_iter; // skip slash
   is_dir = strlen(path_iter) == 0;

   if (is_dir == 0) {
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;
   } else {
      stbuf->st_mode = S_IFREG | 0444;
      stbuf->st_nlink = 1;
      // stbuf->st_size = strlen(hello_str); not sure if this matters for reading
      stbuf->st_size = 0;
   }
   return res;
}

static int mongofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi)
{
   // TODO: why?
   // (void) offset;
   // (void) fi;

   filler(buf, ".", NULL, 0);
   filler(buf, "..", NULL, 0);

   const bson_t* doc;
   bson_t empty = BSON_INITIALIZER;

   int exit_status = 0; // the error code the function returns with
   int skip = 0;

   // parse path to see how many "it" directories down we are.
   const char* path_iter = path;
   while (strstr(path_iter, "/it") != NULL) {
      path_iter += 3;
      skip++;
   }

   // TODO: confirm, but I believe we should always have a trailing slash.
   if (strlen(path_iter) == 0 || path_iter[0] != '/')
      return -ENOENT;   

   bson_t* opts = BCON_NEW(
      "skip", BCON_INT64(skip * batch_size),
      "limit", BCON_INT32(batch_size),
      "projection", "{", "_id", BCON_INT32(1), "}");

   mongoc_collection_t* coll = mongoc_client_get_collection (client, "test", "coll");
   mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &empty, opts, NULL /* read pref */);

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
         bson_free(asStr);
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

static int mongofs_open(const char *path, struct fuse_file_info *fi)
{
   // see `man 2 open` for flags
   if ((fi->flags & 3) != O_RDONLY)
      return -EACCES;

   // return a file descriptor.
   // TODO: use fd to keep track of which bson_t's to cache in memory
   return 0;
}

// this queries *every* time a read is issued.
// instead, we should cache at least the most recently read document.
static int mongofs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
   (void) fi;
   int exit_status;
   // path should be of the form /it/it/it/....<id>
   const char* path_iter = path;
   while (strstr(path_iter, "/it") != NULL) {
      path_iter += 3;
   }

   if (strlen(path_iter) != 1 + 24) {
      printf("malformed read, expecting file to be /<24 chars>\n");
      return -ENOENT;
   }

   ++path_iter; // skip slash

   if (!bson_oid_is_valid(path_iter, 24)) {
      printf("malformed oid\n");
      return -ENOENT;
   }

   bson_oid_t oid;
   bson_oid_init_from_string (&oid, path_iter);

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

   const bson_t *out;
   if (mongoc_cursor_next(cursor, &out)) {
      // instead of copying, I could hack/write my own visitor to write in place.
      int length;
      char* doc_data = bson_as_json(out, &length);
      int bytes_to_copy = size;
      if (offset + size > length) {
         // this is the last read.
         bytes_to_copy = length - offset; // might be zero
      }
      memcpy(buf, doc_data + offset, bytes_to_copy);
      return bytes_to_copy;
   }
   else if (mongoc_cursor_error(cursor, &err)) {
      exit_status = -EIO;
      goto cleanup;
   }
   else {
      printf("doc with id %s not found\n", path_iter);
      exit_status = -ENOENT;
      goto cleanup;
   }

cleanup:
   mongoc_collection_destroy(coll);
   mongoc_cursor_destroy(cursor);
   return exit_status;
}

static struct fuse_operations mongofs_oper = {
   .getattr	= mongofs_getattr,
   .readdir	= mongofs_readdir,
   .open		= mongofs_open,
   .read		= mongofs_read,
};

int main(int argc, char *argv[])
{
   mongoc_init();
   client = mongoc_client_new("mongodb://localhost:27017");

   return fuse_main(argc, argv, &mongofs_oper, NULL);

}