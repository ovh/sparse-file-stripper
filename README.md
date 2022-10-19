Sparse file stripper
====================

"Sparse file stripper"'s primary goal is to help in snaphotting/backuping drives but can also be used as a compression booster, suited for sparse files or half-empty drives.


# [Release note](RELEASENOTE)


# Usage

## Build

```
make
```

## Compression

### Basic

```
$> sfsz /dev/nvme0n1 drive.img
```

### Adapted atomic block size

```
$> sfsz -b 33554432 /dev/nvme0n1 drive.img
```

### Combined with any compression tool

```
$> sfsz /dev/nvme0n1 - | pigz --fast -c > anything_named_pipe_or_file
```

## Extraction

### Basic

```
$> sfsuz drive.img /dev/nvme0n1
```

### Combined with any compression tool

```
$> pigz -d -c anything_named_pipe_or_file | sfsuz - /dev/nvme0n1
```


# What for ?

To compress/save data, be it a block device or a regular file on any given filesystem.

The provided backup/restore workflow:

1. Ends with a compressed backup to spare storage volume.
2. Is entirely streamable, so that one does not need any intermediate storage for the whole backup and/or restore to happen.
3. Uses hardware or filesystem capabilities when possible to spare as much physical I/O as possible when restoring the image.
4. Can be used as a compression booster for any sparse file/device: indeed, since it strips trivial patterns (zero ranges, but it could actually be any pattern), this volume of stripped data does not transit through advanced compression tools that sparse-file-stripper can be combined with. Thus, it spares compute and time for both the compression and uncompression. The benchmark shows this boost effect.


# Alternatives

* dd or dd_rescue or cp or cat or partclone or partimage, optionally combined with a compression tool.

```
$> dd (conv=sparse or not, equivalent here because the standart output is not seekable anyway) | gzip
```

With such a solution you achieve goals 1 and 2 above but not 3, nor 4.

Note, however, that if you zero out the drive first (using fallocate command when supported), then combine with

```
gzip -d -c | dd conv=sparse
```

(fallback when instant zeroing not supported: no fallocate + gzip -d | dd)

you get 1, 2 and 3 and you only miss 4 (though when inflating the file you actually still get a boost effect, but due to the spared writes on the target device exclusively,
not the fact that 0s are not handled by the compression tool)


* qemu-img and qcow2 format
```
$> qemu-img convert -c -O qcow2 file.in file.out
```

With such a solution, you achieve 1 and 3, but you do not have 2 (so you cannot have 4).

* qemu-img and raw format combined with nbdkit

```
$> mkfifo file.out
$> nbdkit -U - streaming pipe=file.out --run 'qemu-img convert -f raw -O raw file.in $nbd'
```

With such a solution you achieve 1 and 2, but then you lose 3 and 4 (no sparse file efficiency anymore)

* More advanced ui (like clonezilla): they will generally use one of the previous solutions under the hood, though we may have missed some of them.


# [Benchmark](benchmark/README.md)


# Known limitations

- Only restore data you built yourself or you trust.

- Basic corruption protection mechanisms, that can probably be bypassed by carefully crafted data. Some missing unconsistency checks spotted, some probably missed.

- Data craft and fuzzing to be done more thoroughly (see [fuzzing](tests/fuzzing)).


# The story

Creating drive backups can be frustrating, especially when these drives are half-empty, meaning most of what you backup is actually garbage data.
You end up with additional storage requirement for your backup.
And when restoring, you end up rewriting all this data you actually do not need, losing time with useless network and disk I/O.

So we were interested in saving raw data, and we were disappointed about the useless I/O operations we had to perform.
We wanted to find some way to spare them.

How could we achieve this for a drive, regardless of what it actually contained
(ie, regardless of the partitioning layout, or the underlying filesystems) ?

We were already aware of sparse files, that could efficiently be handled on some filesystems, implementing hole punching and/or whole zero range writes
(ext4, xfs, now tmpfs...) using fallocate calls (https://www.man7.org/linux/man-pages/man2/fallocate.2.html)

But sparse files are handled at the filesystem layer. So we craved reusing the punch_hole/zero range mechanisms at the block level but seems like we
could not, at first sight :(.

And it was not relevant to spare I/O during the restore process, leaving uninitialized garbage data on drive, if the filesystems on top of the blocks
did not expect garbage from the block device
(besides, this raises another issue, how to distinguish discarded blocks from actual allocated space filled with zeros at the filesystem layer,
when saving ?).
We wanted a mechanism to correctly save blocks without any assumptions about the partioning layout or the filesystems capabilities on top of them.

While digging, we once saw something interesting: we are used to using qemu-img to create drive images, incremental snapshots... It's a great tool
(as the whole qemu project, https://www.qemu.org/download/).

While restoring a qcow2 image to an entire physical drive we got amazed at how fast qemu-img was able to deploy the image on the entire drive,
despite the big size of the extracted raw file. All happened as if the restore process was able to spare disk I/O when extracting the file.

So obviously, this tool used some trick to achieve exactly what we wanted to.

How could it possibly spare these I/O at the block level ?

What we did not know is that these falloc punch_hole and zero range calls that make all the sparse files efficiency are actually
implemented for block devices since Linux kernel 4.9 (https://kernelnewbies.org/Linux_4.9#Block_layer) and also efficiently handled by a lot of modern drives.

For example, some SSD's firmwares implement an RZAT function: Return Zero After Trim.

This means that when blocks are discarded, even if the data is not physically erased in the SSD's memory,
its controller will make sure that the blocks will reread 0 (and this is exactly what we needed).

It is not to be confused with an instant secure erase, which consists in changing a cryptographic key stored in controller's non volatile memory,
used to cipher/decipher data on the fly before persisting data physically on drive. Changing the key (and forgetting the old one) makes physical data
undecipherable, though unchanged, as long as the cipher algo is secure and the key did not leak out of the drive.

When discarding blocks, the underlying data can still be recovered with a physical access to drive (or controller's firmware hack),
but otherwise, there is no way to distinguish between a physical zero from a logical one.

With such drives and, for example, an ext4 filesystem mounted with the discard option, the block device can be kept 'clean' during the filesystem life cycle,
when removing files. Thanks to this, we can build a drive backup, regardless of the underlying filesystems, whose volume can actually be representative
of the overall underlying filesystem usages (in other words garbage data become zeros and can be easily spotted and stripped).

So thanks to qemu-img, we discovered we could actually reuse this fallocate syscalls for block devices as well,
provided the hardware supports it.

Then why did not we use qemu-img, instead of reinventing the wheel ?

We tried. But qemu-img suffers from a big drawback: it is not streamable - to be more accurate, qcow2 format is not streamable
(probably because of qcow2 structural constraints, that are necessary to implement the incremental/create on write features,
we do not feel confident enough to talk precisely about it though).
For the raw format, some additional tools have been developped to make qemu-img streamable (nbdkit, https://github.com/libguestfs/nbdkit),
but then the whole process is equivalent to a simple raw copy and you lose the sparse efficiency that interests us. 
Proof here: [nbdkit hands on](doc/nbdkit.md)

When saving data with qemu-img, you need to be able to perform random accesses to your output. And the same, when restoring data, with your input.

Thus, if you intend to store your data somewhere without any random write accesses you need some intermediate storage to convert your data first
before you can start uploading it.
This happens, for example, if you store your backups on an S3 storage (the http range RFC - https://tools.ietf.org/html/rfc7233 - provides ways to
emulate random read accesses but not write ones, because S3 storage solution usually do not implement existing file updates).
So as a consequence with qemu-img, you need to split data formatting and upload.
In other words, the whole backup (resp. restore) workflow takes more time, as the two operations are sequential, not parallel, you convert then you upload
(resp. you download then you convert).

So we developped our own backup/restore tool that is:

- Streamable: no intermediate storage needed, no sequential backup+upload nor download+restore operations, time can thus be saved
- Spares I/O when restoring file (depending on the underlying block device or filesystem hole_punch support)

Time saved, storage volume spared, I/O spared

Finally we realized that all this effort made for drive backups could actually be used for something else: to boost more advanced compression tools.
What our binary does is basically stripping trivial patterns (currently zero ranges only, but it could be any pattern) before piping data
to the next processing unit in the workflow. So sparse data are all trimed and the compression tool does not have to process them.
Actually the boost effect observed during compression is not so great as the benchmark tends to show
(there can even be a small overhead, especially on multithreaded compression tools, probably due to the fact that our code should be optimized).
But the sparse file stripper really boosts data extraction.


# Streamable workflow

What makes sparse-file-stripper interesting is that the whole backup/restore workflow is streamable and keeps its I/O efficiency on sparse files on both the backup and the restore processes.

For a workflow to be streamable, its input/output should be read/written sequentially (no seeks, nor random accesses required). In our case, it seems hard to achieve this for both the backup and the restore processes if we stick at the byte level. We achieve this by changing the stream unit, designing an atomic data block structure, *whose maximum size we can control*. When restoring data, sequential input happens at the atomic data block level, provided we are able to bufferize the whole atomic block in memory at the restore place first (hence the importance of being able to have an upper boundary for the atomic block size during the backup process, to control the memory used by the restore one). Then, inside the atomic blocks, we can use random accesses. Note this is most likely what happens for advanced compression tools too: for example, the compression level of xz is not only conditioning the compression ratio, but also the amount of ram needed where the data is restored.


 # Data structure details
 
 [details](doc/sfs_structure_and_workflow.txt)
