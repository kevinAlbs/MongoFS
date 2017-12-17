/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall hello.c `pkg-config fuse --cflags --libs` -o hello
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <mongoc.h>

mongoc_client_t* client;
#define FUSE_USE_VERSION 26

static const char *hello_str = "Hello World!\n";
static const char *hello_path = "/huh";

static int hello_getattr(const char *path, struct stat *stbuf)
{
   int res = 0;

   memset(stbuf, 0, sizeof(struct stat));
   if (strcmp(path, "/") == 0) {
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;
   } else {
      stbuf->st_mode = S_IFREG | 0444;
      stbuf->st_nlink = 1;
      stbuf->st_size = strlen(hello_str);
   }
   return res;
}

static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
   (void) offset;
   (void) fi;

   if (strcmp(path, "/") != 0)
      return -ENOENT;

   filler(buf, ".", NULL, 0);
   filler(buf, "..", NULL, 0);

   const bson_t* doc;
   bson_t empty = BSON_INITIALIZER;
   mongoc_collection_t* coll = mongoc_client_get_collection (client, "test", "coll");
   mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, &empty, &empty, NULL);

   while (mongoc_cursor_next(cursor, &doc)) {
      bson_iter_t iter;
      if (bson_iter_init_find(&iter, doc, "_id")) {
         const bson_oid_t *oid = bson_iter_oid (&iter);
         bson_t tmp = BSON_INITIALIZER;
         bson_append_oid(&tmp, "", 0, oid);
         char* asStr = bson_as_json(&tmp, 0);
         asStr[19 + 24] = '\0';
         filler(buf, asStr + 19, NULL, 0);
         bson_free(asStr);
      }
   }


   return 0;
}

static int hello_open(const char *path, struct fuse_file_info *fi)
{
   //if (strcmp(path, hello_path) != 0)
   //   return -ENOENT;

   if ((fi->flags & 3) != O_RDONLY)
      return -EACCES;

   return 0;
}

static int hello_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
   size_t len;
   (void) fi;
   //if(strcmp(path, hello_path) != 0)
    //  return -ENOENT;

   len = strlen(hello_str);
   if (offset < len) {
      if (offset + size > len)
         size = len - offset;
      memcpy(buf, hello_str + offset, size);
   } else
      size = 0;

   return size;
}

static struct fuse_operations hello_oper = {
   .getattr	= hello_getattr,
   .readdir	= hello_readdir,
   .open		= hello_open,
   .read		= hello_read,
};

int main(int argc, char *argv[])
{
   mongoc_init();
   client = mongoc_client_new("mongodb://localhost:27017");

   return fuse_main(argc, argv, &hello_oper, NULL);
}