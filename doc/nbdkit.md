Proof that combining nbdkit with qemu-img, we obtain the streaming capability but lose the possibility to use the qcow2
format and keep the sparse efficiency while transferring data

```
$> mkfifo pipe1
$> qemu-img info image1.raw
image: image1.raw
file format: raw
virtual size: 4.2 GiB (4508876800 bytes)
disk size: 1.28 GiB
$> du -sh image1.raw
1.3G	image1.raw
$> ls -alh image1.raw
-rw-r--r-- 1 root root 4.2G Dec 30 16:04 image1.raw
# Using qcow2 not working, expected behaviour.
$> nbdkit -U - streaming pipe=./pipe1 --run 'qemu-img convert -n image1.raw -O qcow2 $nbd'
qemu-img: Could not open 'nbd:unix:/tmp/nbdkitJTWV0d/socket': Image is not in qcow2 format
# So we're forced to stick with raw
$> nbdkit -U - streaming pipe=./pipe1 --run 'qemu-img convert -n image1.raw -O raw $nbd'
```

Then in another shell (no matter which filesystem/block device you're storing file on, you are dumping the whole file with zeroes)
```
$> cat pipe1 > image2.raw
$> md5sum image*raw
ae03c77c598ec1e4073c5add7fcae6cb  image1.raw
ae03c77c598ec1e4073c5add7fcae6cb  image2.raw
$> du -sh image*.raw
1.3G	image1.raw
4.2G	image2.raw
```

No formatting at all (normal we use it for a trivial raw -> raw copy, like cp/dd/cat), so basically, the next process unit in the workflow will have to handle the zeroes of the raw data.