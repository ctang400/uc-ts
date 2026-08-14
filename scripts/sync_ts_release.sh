#!/usr/bin/env bash
# 按"发布目录捆绑 pandora so"的方式部署 ucts(替代旧 sync_ts00x.sh 的裸拷贝):
#
#   bash scripts/sync_ts_release.sh <NNN|all> [--stage-only]
#
#   <NNN>         服务器编号, 目标主机为 uc_aws_jp_<NNN> (如 001)
#   all           部署到 MACHINES 列表里的全部机器(新增机器只需加一行)
#   --stage-only  只在本地组装 + 校验发布目录, 不上传不改 prod (预演用)
#
# all 模式下 staging 只组装/校验一次, 再分发到各台 —— 保证所有机器拿到的是
# 字节完全相同的 release(逐台重新组装可能因本地依赖变动产生差异)。
# 某台失败不影响其余机器继续部署, 结尾汇总并以非 0 退出。
#
# 做什么:
#   1. 本地组装 release 目录: ucts + ldd 闭包里所有非系统 .so (pandora 自有库、
#      3rdlib、conan 库) 平铺一层, 附 MANIFEST (ucts/pandora git hash)。
#   2. patchelf 把 ucts 的 RPATH 改为 $ORIGIN (--force-rpath, legacy RPATH 对
#      整个依赖树生效), 保证优先加载同目录 so; pandora 各 so 自带 $ORIGIN
#      RUNPATH, 平铺后其相互依赖同样就地解析, 无需再 patch。
#   3. 本地 ldd 校验: 所有非系统依赖必须解析进 staging 目录, 否则中止。
#   4. rsync 到 <host>:~/ucts_releases/ucts_<yyyymmdd>_<hash>/。
#   5. 远端换 symlink: ~/ts/bin -> 该 release 目录(已存在的真实 bin 目录
#      直接删除, 不备份); 再跑一遍远端 ldd 校验。
#
# 回滚 = 在 prod 上把 ~/ts/bin 重新 ln -sfnT 到上一个 release 目录
# (脚本会在换链前打印当前指向, 便于记录)。
set -euo pipefail
cd "$(dirname "$0")/.."

MACHINES=(001 002 003 004) # 新增机器加在这里
HOST_PREFIX=uc_aws_jp_

NNN=${1:?usage: sync_ts_release.sh <NNN|all> [--stage-only]}
STAGE_ONLY=${2:-}
if [ "$NNN" = "all" ]; then
  TARGETS=("${MACHINES[@]}")
else
  TARGETS=()
  for m in "${MACHINES[@]}"; do [ "$m" = "$NNN" ] && TARGETS=("$m"); done
  if [ ${#TARGETS[@]} -eq 0 ]; then
    echo "ERROR: 未知机器编号 '$NNN' (可用: ${MACHINES[*]} 或 all)"; exit 1
  fi
fi
UCTS=build/ucts
[ -f "$UCTS" ] || { echo "ERROR: $UCTS not found (先 build)"; exit 1; }
command -v patchelf >/dev/null || { echo "ERROR: 本机缺 patchelf"; exit 1; }

# release 名: ucts_<日期>_<ucts短hash>[-dirty]_p<pandora短hash>
# pandora 的 so 一起打包, 版本必须体现在目录名里 —— 同一 ucts hash 配不同
# pandora 会是两个不同的 release, 否则回滚/对账时分不清。
HASH=$(git rev-parse --short HEAD)
git diff --quiet HEAD -- src CMakeLists.txt 2>/dev/null || HASH="${HASH}-dirty"
PAND_HASH=$(git -C ~/pandora-cpp rev-parse --short HEAD 2>/dev/null || echo unknown)
REL="ucts_$(date +%Y%m%d)_${HASH}_p${PAND_HASH}"
STG=$(mktemp -d)
trap 'rm -rf "$STG"' EXIT

# ---- 1. 组装: binary + ldd 闭包的非系统 so, 平铺一层 ------------------------
cp "$UCTS" "$STG/ucts"
# 非系统 = 解析路径不在 /lib、/usr/lib 下 (盖住 pandora、conan、/usr/local/lib);
# vdso/ld-linux 无路径, 自然跳过。
mapfile -t SO_PATHS < <(ldd "$UCTS" | awk '/=>/ && $3 ~ /^\// {print $3}' \
  | grep -vE '^/lib/|^/lib64/|^/usr/lib/')
declare -A SEEN
for p in "${SO_PATHS[@]}"; do
  base=$(basename "$p")
  if [ -n "${SEEN[$base]:-}" ] && ! cmp -s "$p" "$STG/$base"; then
    echo "ERROR: 同名不同内容的 so: $base ($p vs ${SEEN[$base]})"; exit 1
  fi
  cp "$p" "$STG/$base"; SEEN[$base]=$p
done
echo "bundled $(ls "$STG" | grep -c '\.so') shared libs"

# ---- 2. RPATH -> $ORIGIN (legacy RPATH, 对整树生效) -------------------------
patchelf --force-rpath --set-rpath '$ORIGIN' "$STG/ucts"

# ---- 3. 本地校验: 非系统依赖必须全部解析进 staging --------------------------
BAD=$(cd "$STG" && ldd ./ucts | awk '/=>/ && $3 ~ /^\// {print $3}' \
  | grep -vE '^/lib/|^/lib64/|^/usr/lib/' | grep -v "^$STG/" || true)
NOTFOUND=$(cd "$STG" && ldd ./ucts | grep "not found" || true)
if [ -n "$BAD$NOTFOUND" ]; then
  echo "ERROR: staging 校验失败, 以下依赖没有解析进发布目录:"
  echo "$BAD"; echo "$NOTFOUND"; exit 1
fi
echo "staging ldd check OK"

# ---- MANIFEST ---------------------------------------------------------------
{
  echo "release:       $REL"
  echo "built_at:      $(date +%FT%T)"
  echo "ucts_commit:   $HASH"
  echo "pandora_commit:$PAND_HASH"
  echo "targets:       ${TARGETS[*]}"
  echo "libs:"; (cd "$STG" && ls -1 *.so* | sed 's/^/  /')
} > "$STG/MANIFEST"

if [ "$STAGE_ONLY" = "--stage-only" ]; then
  echo "--stage-only: 组装+校验完成, 未部署。staging 内容:"
  ls -la "$STG"; exit 0
fi

# ---- 4/5. 逐台上传 + 换链 + 校验 --------------------------------------------
# 单台部署封装成函数: 每步显式判失败, 不依赖 errexit(函数在 if 条件里被调用时
# errexit 会被抑制)。任一步失败即返回非 0, 该机记入 failed 后继续下一台。
deploy_one() {
  local HOST=$1
  echo "===== $HOST ====="
  echo "deploying $REL -> $HOST"
  ssh "$HOST" "mkdir -p ~/ucts_releases/$REL" || { echo "!! $HOST mkdir 失败"; return 1; }
  rsync -a "$STG/" "$HOST:~/ucts_releases/$REL/" || { echo "!! $HOST rsync 失败"; return 1; }
  ssh "$HOST" REL="$REL" 'bash -s' <<'REMOTE'
set -euo pipefail
mkdir -p ~/ts
# 旧指向打印一下(回滚线索); 真实 bin 目录直接删除, 不备份
if [ -L ~/ts/bin ]; then
  echo "previous release: $(readlink ~/ts/bin)"
elif [ -e ~/ts/bin ]; then
  echo "removing existing ~/ts/bin (no backup)"
  rm -rf ~/ts/bin
fi
ln -sfnT ~/ucts_releases/"$REL" ~/ts/bin
# 远端校验: 无 not found, 且非系统依赖全部来自本 release 目录
cd ~/ts/bin
if ldd ./ucts | grep "not found"; then echo "ERROR: 缺依赖"; exit 1; fi
LEAK=$(ldd ./ucts | awk '/=>/ && $3 ~ /^\// {print $3}' \
  | grep -vE '^/lib/|^/lib64/|^/usr/lib/' \
  | grep -v "^$(readlink -f ~/ucts_releases/"$REL")/" || true)
if [ -n "$LEAK" ]; then
  echo "ERROR: 以下依赖泄漏到发布目录之外:"; echo "$LEAK"; exit 1
fi
echo "remote ldd check OK; ~/ts/bin -> $(readlink ~/ts/bin)"
REMOTE
}

failed=()
for m in "${TARGETS[@]}"; do
  host="${HOST_PREFIX}${m}"
  if deploy_one "$host"; then
    echo "done: $host ~/ts/bin -> ~/ucts_releases/$REL"
  else
    failed+=("$host")
  fi
  echo
done

if [ ${#failed[@]} -ne 0 ]; then
  echo "以下机器部署失败: ${failed[*]}"
  echo "(其余机器已切到 $REL; 失败机器的 ~/ts/bin 保持原指向)"
  exit 1
fi
echo "全部完成 (${#TARGETS[@]} 台): $REL"
echo "注意: 二进制已就位, ucts 未重启。"
