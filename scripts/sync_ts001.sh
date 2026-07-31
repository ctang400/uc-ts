now=$(date +%FT%T)
echo $now
rsync -avu build/ucts uc_aws_jp_001:~/ts/bin/ucts

