#!/vendor/bin/sh
echo "------ CMA info ------"
for d in $(ls -d /d/cma/*); do
  echo --- $d
  echo --- count; cat $d/count
  echo --- used; cat $d/used
  echo --- bitmap; cat $d/bitmap
done

