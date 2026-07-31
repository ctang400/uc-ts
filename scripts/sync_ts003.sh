now=$(date +%FT%T)
echo $now
rsync -avu build/ucts uc_aws_jp_003:~/ts/bin/ucts

