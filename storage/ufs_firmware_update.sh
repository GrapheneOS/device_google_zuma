#!/vendor/bin/sh
#
# The script belongs to the feature of UFS FFU via OTA: go/p23-ffu-ota
# Its purpose is to copy the corresponding firmware into partition for UFS FFU.

ufs_dev="/dev/sys/block/bootdevice"
fw_dir="/vendor/firmware"
blk_dev="/dev/block/by-name/ufs_internal"

vendor=$(cat ${ufs_dev}/vendor | tr -d "[:space:]")
model=$(cat ${ufs_dev}/model | tr -d "[:space:]")
rev=$(cat ${ufs_dev}/rev | tr -d "[:space:]")

file=$(find ${fw_dir} -name "*${vendor}${model}${rev}*" | head -n 1)
if [ -n "$file" ]; then
	dd if="$file" of=$blk_dev bs=4k
fi
