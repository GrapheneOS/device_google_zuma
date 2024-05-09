#!/vendor/bin/sh

CHECKPOINT_DIR=/data/vendor/copied

BIN_DIR=/vendor/bin

$BIN_DIR/mkdir -p $CHECKPOINT_DIR

function copy_files_to_data()
{
  partition_name=$(basename $1)
  mount_point=$2
  tmpdir=$CHECKPOINT_DIR/$partition_name.img
  build_checkpoint=$CHECKPOINT_DIR/$partition_name
  if [ ! -e $build_checkpoint ]; then
    $BIN_DIR/rm -rf $tmpdir
    $BIN_DIR/rm -rf $build_checkpoint
    cp -rp $mount_point $tmpdir
    if [ $? -ne 0 ]; then
      echo "Failed to cp -rp $mount_point $tmpdir"
      return
    fi
    fsync `find $tmpdir -type fd`
    mv $tmpdir $build_checkpoint
    if [ $? -ne 0 ]; then
      echo "mv $tmpdir $build_checkpoint"
      return
    fi
    fsync `dirname $build_checkpoint`
  fi
  echo "Successfully copied $mount_point to $build_checkpoint"
}

chmod g+rx -R /mnt/vendor/efs
chmod g+rx -R /mnt/vendor/efs_backup
chmod g+rx -R /mnt/vendor/modem_userdata
copy_files_to_data "/dev/block/by-name/efs" "/mnt/vendor/efs"
copy_files_to_data "/dev/block/by-name/efs_backup" "/mnt/vendor/efs_backup"
copy_files_to_data "/dev/block/by-name/modem_userdata" "/mnt/vendor/modem_userdata"

chmod g+rx -R /mnt/vendor/persist
copy_files_to_data "/dev/block/by-name/persist" "/mnt/vendor/persist"
