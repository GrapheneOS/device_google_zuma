#!/vendor/bin/sh

CHECKPOINT_DIR=/data/vendor/copied

BIN_DIR=/vendor/bin

$BIN_DIR/mkdir -p $CHECKPOINT_DIR

function copy_files_to_data()
{
  block_device=$1
  partition_name=$(basename $1)
  mount_point=$2
  tmpdir=$CHECKPOINT_DIR/$partition_name.img
  build_checkpoint=$CHECKPOINT_DIR/$partition_name
  if [ ! -e $build_checkpoint ]; then
    $BIN_DIR/rm -rf $tmpdir
    $BIN_DIR/mkdir -p $tmpdir
    $BIN_DIR/dump.f2fs -rfPo $tmpdir $block_device
    if [ $? -ne 0 ]; then
      echo "Failed to $BIN_DIR/dump.f2fs -rfPo $tmpdir $block_device"
      return
    fi
    mv $tmpdir $build_checkpoint
    if [ $? -ne 0 ]; then
      echo "mv $tmpdir $build_checkpoint"
      return
    fi
    fsync `dirname $build_checkpoint`
  fi
  echo "Successfully copied $mount_point to $build_checkpoint"
}

copy_files_to_data "/dev/block/by-name/efs" "/mnt/vendor/efs"
copy_files_to_data "/dev/block/by-name/efs_backup" "/mnt/vendor/efs_backup"
copy_files_to_data "/dev/block/by-name/modem_userdata" "/mnt/vendor/modem_userdata"

copy_files_to_data "/dev/block/by-name/persist" "/mnt/vendor/persist"
