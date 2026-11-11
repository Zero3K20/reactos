# UDFS write benchmark

`udfsbench` measures representative file creation/data writes, file metadata
updates, explicit flushes, and closes on the volume containing a directory
you select. Run it on a mounted UDF volume and, for comparison, on another
filesystem using a similar device.

```text
udfsbench <existing-directory> [files=100] [bytes-per-file=4096] [repeats=3] [--keep]
```

For example, `udfsbench U:\bench 250 8192 5` creates a unique temporary
directory under `U:\bench`, runs both workloads five times, and removes its
files and directories afterward. `--keep` retains the generated files for
inspection. The workload is limited to 256 MiB of total payload data.

The TAR-like workload makes one native `FileBasicInformation` request per file
with creation, access, and write timestamps plus the archive attribute. The
ZIP-like workload makes the same request with only a write timestamp and the
archive attribute. These are controlled synthetic workloads, not a full 7-Zip
extraction or a guarantee about which fields a particular archive contains.

Each run reports file creation, data writing, metadata updates, explicit
per-file `FlushFileBuffers`, and close times separately. The explicit flush is a
deliberately strong persistence boundary and is not the normal 7-Zip extraction
close path. Compare creation, write, and metadata timings separately from flush
times; run multiple times and avoid unrelated I/O for more useful comparisons.
