# MongoFS

## Overview

mongofs is a utility for representing documents in a mongodb collection as files in a directory. The primary use cases are:
- search data within documents with tools like ag or perl
- simple editing of documents with tools like sed or perl and text editors

## Basic Usage
```
./mongofs --uri=mongodb://localhost:27017 --ns=mongofs.test --log_file=mylog.log -f -s mount_dir
```

## Features
- each document is represented as a file having the name of the _id field
- documents can be read and cached in memory
- info and errors are reported by being appended to a real log file
- documents are batched. Each batch is represented as a nested `it` directory

## Desired features
- handle non ObjectID _id
- enable multithreaded access
- cache is fixed and spills over to real filesystem
- cursor(s) follow what docs are being read so sequential file access corresponds to iterating the cursor
- different representations of docs (one-line, yaml, quoteless)
- enable for read-only views (and make the files readonly)
- error handling in file
- operations are batched into bulk operations
- have single file mode for each collection?
