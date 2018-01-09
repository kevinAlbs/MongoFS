# mongofs

## Overview

mongofs is a utility for representing documents in a mongodb collection as files in a directory. The primary use cases are:
- search data within documents with tools like ag or perl
- simple editing of documents with tools like sed or perl and text editors

## MVP features
- each document is represented as a file having the name of the _id field
- documents can be read and cached in memory
- when docs are closed they are saved (if possible) or report an error
- errors are reported by being appended to a *real* log file
- when files are unlinked the document is deleted
- documents are batched. Each batch is represented as a nested `it` directory


## Desired features
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

Decision: 

### Designing Cache
- should probably be a fixed size cache. Easier to program, more consistent perf.
- should be an LRU cache
- LRU cache: DLL for data + O(1) lookup structure
- Lookup structure:
    - a trie? For object ids this is 24 chars of 16 values each.
    - how about a simple table. How to size it optimally? O(sqrt(n))