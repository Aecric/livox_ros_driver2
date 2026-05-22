#!/usr/bin/env bash
# ============================================================================
# 用 Docker buildx 把 livox_ros_driver2 打成 .deb
#
# 矩阵: {humble, jazzy, lyrical} × {amd64, arm64}
#
# 用法:
#   ./build_deb.sh                      # 默认: humble + 本机架构
#   ./build_deb.sh humble arm64         # 指定单个组合
#   ./build_deb.sh jazzy  amd64
#   ./build_deb.sh all    amd64         # humble, jazzy & lyrical, 仅 amd64
#   ./build_deb.sh humble all           # humble, amd64 & arm64
#   ./build_deb.sh all    all           # 全 6 个组合
#
# 环境变量:
#   BUILD_JOBS=4         deb 内部 make 并发数 (QEMU 下保守, 默认 2)
#   LIVOX_SDK2_REF=v1.x  钉 Livox-SDK2 版本 (默认 master)
#   APT_MIRROR=host      可选 Ubuntu apt 镜像, 例如 mirrors.ustc.edu.cn/ubuntu
#   ROSDISTRO_INDEX_URL  可选 rosdep rosdistro index 镜像
#   BUILDER_NAME=name    buildx builder 名 (默认 livox-deb-builder)
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

DOCKERFILE="docker/Dockerfile"
OUTPUT_DIR="./debs"
BUILD_JOBS="${BUILD_JOBS:-2}"
LIVOX_SDK2_REF="${LIVOX_SDK2_REF:-master}"
APT_MIRROR="${APT_MIRROR:-}"
ROSDISTRO_INDEX_URL="${ROSDISTRO_INDEX_URL:-}"
BUILDER_NAME="${BUILDER_NAME:-livox-deb-builder}"

# --- 参数解析 ----------------------------------------------------------------
case "$(uname -m)" in
    x86_64)  HOST_ARCH="amd64" ;;
    aarch64) HOST_ARCH="arm64" ;;
    *)       HOST_ARCH="$(uname -m)" ;;
esac

DISTRO_ARG="${1:-humble}"
ARCH_ARG="${2:-${HOST_ARCH}}"

case "${DISTRO_ARG}" in
    humble) DISTROS=(humble) ;;
    jazzy)  DISTROS=(jazzy) ;;
    lyrical) DISTROS=(lyrical) ;;
    all)    DISTROS=(humble jazzy lyrical) ;;
    *) echo "ERROR: 未知 distro '${DISTRO_ARG}', 应为 humble|jazzy|lyrical|all" >&2; exit 1 ;;
esac

case "${ARCH_ARG}" in
    amd64) ARCHES=(amd64) ;;
    arm64) ARCHES=(arm64) ;;
    all)   ARCHES=(amd64 arm64) ;;
    *) echo "ERROR: 未知 arch '${ARCH_ARG}', 应为 amd64|arm64|all" >&2; exit 1 ;;
esac

# --- 前置检查 ----------------------------------------------------------------
if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: 找不到 docker 命令" >&2
    exit 1
fi
if ! docker buildx version >/dev/null 2>&1; then
    echo "ERROR: docker buildx 不可用; 请安装 docker-buildx-plugin" >&2
    exit 1
fi

NEEDS_QEMU=0
for arch in "${ARCHES[@]}"; do
    [ "${arch}" != "${HOST_ARCH}" ] && NEEDS_QEMU=1
done

# --- 1. QEMU binfmt ---------------------------------------------------------
echo "==> [1/4] QEMU binfmt"
if [ "${NEEDS_QEMU}" -eq 1 ]; then
    if [ "${HOST_ARCH}" = "amd64" ] && [ ! -f /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        echo "    未检测到 qemu-aarch64, 安装中..."
        docker run --privileged --rm tonistiigi/binfmt --install arm64
    elif [ "${HOST_ARCH}" = "arm64" ] && [ ! -f /proc/sys/fs/binfmt_misc/qemu-x86_64 ]; then
        echo "    未检测到 qemu-x86_64, 安装中..."
        docker run --privileged --rm tonistiigi/binfmt --install amd64
    fi
    echo "    QEMU 已就绪"
else
    echo "    全部为本机架构 (${HOST_ARCH}), 跳过"
fi

# --- 2. buildx builder ------------------------------------------------------
echo "==> [2/4] buildx builder (${BUILDER_NAME})"
if ! docker buildx inspect "${BUILDER_NAME}" >/dev/null 2>&1; then
    docker buildx create \
        --name "${BUILDER_NAME}" \
        --driver docker-container \
        --driver-opt network=host \
        --use \
        --bootstrap
else
    docker buildx use "${BUILDER_NAME}"
fi
docker buildx inspect --bootstrap | grep -E "Name|Driver|Platforms" || true

# --- 3. 构建矩阵 ------------------------------------------------------------
echo "==> [3/4] 构建"
echo "    DISTROS=${DISTROS[*]}, ARCHES=${ARCHES[*]}"
echo "    BUILD_JOBS=${BUILD_JOBS}, LIVOX_SDK2_REF=${LIVOX_SDK2_REF}"
if [ -n "${APT_MIRROR}" ]; then
    echo "    APT_MIRROR=${APT_MIRROR}"
fi
if [ -n "${ROSDISTRO_INDEX_URL}" ]; then
    echo "    ROSDISTRO_INDEX_URL=${ROSDISTRO_INDEX_URL}"
fi
echo "    OUTPUT_DIR=${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

START=$(date +%s)
FAILED=()

for distro in "${DISTROS[@]}"; do
    for arch in "${ARCHES[@]}"; do
        TAG="${distro}/${arch}"
        echo ""
        echo "    ----- 构建 ${TAG} -----"
        if docker buildx build \
            --platform "linux/${arch}" \
            --file "${DOCKERFILE}" \
            --target export \
            --build-arg "ROS_DISTRO=${distro}" \
            --build-arg "BUILD_JOBS=${BUILD_JOBS}" \
            --build-arg "LIVOX_SDK2_REF=${LIVOX_SDK2_REF}" \
            --build-arg "APT_MIRROR=${APT_MIRROR}" \
            --build-arg "ROSDISTRO_INDEX_URL=${ROSDISTRO_INDEX_URL}" \
            --output "type=local,dest=${OUTPUT_DIR}" \
            --progress plain \
            . ; then
            echo "    ===== ${TAG} OK ====="
        else
            echo "    ===== ${TAG} FAILED =====" >&2
            FAILED+=("${TAG}")
        fi
    done
done

END=$(date +%s)
ELAPSED=$((END - START))

# --- 4. 汇总 ----------------------------------------------------------------
echo ""
echo "==> [4/4] 汇总"
printf "    总耗时: %02d:%02d:%02d\n" \
    $((ELAPSED / 3600)) $(((ELAPSED % 3600) / 60)) $((ELAPSED % 60))

if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "    失败: ${FAILED[*]}" >&2
fi

if ls "${OUTPUT_DIR}"/*.deb >/dev/null 2>&1; then
    echo "    产物:"
    ls -lh "${OUTPUT_DIR}"/*.deb
    echo ""
    echo "安装:"
    echo "    sudo apt install ${OUTPUT_DIR}/ros-<distro>-livox-ros-driver2_*.deb"
    echo "    source /opt/ros/<distro>/setup.bash"
else
    echo "    (无 .deb 产物)" >&2
fi

[ "${#FAILED[@]}" -eq 0 ] || exit 1
