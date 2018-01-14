# mongofs

## Overview

mongofs is a utility for representing documents in a mongodb collection as files in a directory. The primary use cases are:
- search data within documents with tools like ag or perl
- simple editing of documents with tools like sed or perl and text editors

## Basic Plan
- get all working in one file and add minimal command line interface
- add some unittests + python/js integration tests

## MVP features
- each document is represented as a file having the name of the _id field
- documents can be read and cached in memory
- when docs are closed they are saved (if possible) or report an error
- errors are reported by being appended to a *real* log file
- when files are unlinked the document is deleted
- documents are batched. Each batch is represented as a nested `it` directory
- have single file mode for each collection.


## Desired features
- handle non ObjectID _id
- enable multithreaded access
- cache is fixed and spills over to real filesystem
- cursor(s) follow what docs are being read so sequential file access corresponds to iterating the cursor
- different representations of docs (one-line, yaml, quoteless)
- enable for read-only views (and make the files readonly)
- error handling in file
- operations are batched into bulk operations

## Behavior

## Implementation

### Caching file path vs file descriptor
A program may assume that partially completed operations are still closeable.
One particular case is truncate followed by a open/write/close. The truncate call opens/closes in one go.
So if we flush the cache after every close, this may have undesirable consequences.
OTOH if we don't assume closing means ok to flush, then how do we know when to flush?

Instead, we will not flush invalid bson after release. But they will be the first to be purged from cache.

### Cache Design
- fixed size cache, spills to disk
- responsible for malloc/free
- have embedded LRU DLLs for purgeable data (non-open valid) and (non-open invalid)
- table lookup
