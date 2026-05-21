# syntax=docker/dockerfile:1.6
# ============================================================================
# livox_ros_driver2 ROS 2 .deb 打包 Dockerfile (Multi-arch: amd64 / arm64)
#
# 特性:
#   - 同一份 Dockerfile, 通过 buildx --platform 切换 amd64 / arm64
#   - Livox-SDK2 在镜像内从源码编译, 整体打入 .deb
#     (.so + 头文件落到 /opt/ros/${ROS_DISTRO}/{lib,include}, 装上即用)
#   - 默认产物: ros-${ROS_DISTRO}-livox-ros-driver2_*.deb
#
# 用法 (在 livox_ros_driver2 仓库根目录执行):
#
#   # 默认 humble + amd64
#   docker buildx build --platform linux/amd64 \
#       -f docker/Dockerfile --target export \
#       --output type=local,dest=./debs .
#
#   # humble + arm64 (x86_64 宿主 + QEMU 用户态模拟)
#   docker run --privileged --rm tonistiigi/binfmt --install arm64
#   docker buildx build --platform linux/arm64 \
#       -f docker/Dockerfile --target export \
#       --build-arg BUILD_JOBS=2 \
#       --output type=local,dest=./debs .
#
#   # jazzy (Ubuntu 24.04) — 通过 --build-arg ROS_DISTRO=jazzy 切换
#   docker buildx build --platform linux/amd64 \
#       -f docker/Dockerfile --target export \
#       --build-arg ROS_DISTRO=jazzy \
#       --output type=local,dest=./debs .
#
# 安装:
#   sudo apt install ./ros-${ROS_DISTRO}-livox-ros-driver2_*.deb
#   source /opt/ros/${ROS_DISTRO}/setup.bash
#   ros2 launch livox_ros_driver2 msg_MID360_launch.py
# ============================================================================

# 支持: humble (Ubuntu 22.04 / jammy), jazzy (Ubuntu 24.04 / noble), lyrical (Ubuntu 26.04 / resolute)
ARG ROS_DISTRO=humble
FROM ros:${ROS_DISTRO}-perception AS builder

ARG TARGETARCH
# QEMU 模拟下并行度要保守; 宿主内存充足可 --build-arg BUILD_JOBS=4 调高
ARG BUILD_JOBS=2
# Livox-SDK2 版本; --build-arg LIVOX_SDK2_REF=v1.x.x 钉版本
ARG LIVOX_SDK2_REF=master
# 可选: rosdep 的 distribution index 镜像 (绕开 raw.githubusercontent 网络问题)
#   常用清华镜像: https://mirrors.tuna.tsinghua.edu.cn/rosdistro/index-v4.yaml
ARG ROSDISTRO_INDEX_URL="https://mirrors.tuna.tsinghua.edu.cn/rosdistro/index-v4.yaml"

ENV ROS_WS=/root/ros_ws \
    LIVOX_SDK2_STAGE=/opt/livox_sdk2_stage \
    DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

# ---------------------------------------------------------------------------
# 1. apt 镜像源 (USTC; arm64 走 ubuntu-ports)
#    - 从 /etc/os-release 取 codename, 自动适配 jammy(22.04) / noble(24.04)
#    - Noble 默认走 deb822 (ubuntu.sources), 改写成 legacy sources.list 之前
#      先把它禁用, 否则双源会重复抓包
# ---------------------------------------------------------------------------
RUN . /etc/os-release && \
    if [ "$TARGETARCH" = "arm64" ]; then \
        REPO="mirrors.ustc.edu.cn/ubuntu-ports"; \
    else \
        REPO="mirrors.ustc.edu.cn/ubuntu"; \
    fi && \
    if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
        mv /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources.disabled; \
    fi && \
    printf "deb https://$REPO/ $VERSION_CODENAME main restricted universe multiverse\n\
deb-src https://$REPO/ $VERSION_CODENAME main restricted universe multiverse\n\
deb https://$REPO/ $VERSION_CODENAME-updates main restricted universe multiverse\n\
deb-src https://$REPO/ $VERSION_CODENAME-updates main restricted universe multiverse\n\
deb https://$REPO/ $VERSION_CODENAME-security main restricted universe multiverse\n\
deb-src https://$REPO/ $VERSION_CODENAME-security main restricted universe multiverse\n" > /etc/apt/sources.list

# ---------------------------------------------------------------------------
# 2. 构建 + 打包依赖
# ---------------------------------------------------------------------------
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked,id=apt-cache-${TARGETARCH} \
    --mount=type=cache,target=/var/lib/apt,sharing=locked,id=apt-lib-${TARGETARCH} \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        cmake make g++ gcc git ca-certificates \
        libboost-all-dev libpcl-dev libapr1-dev \
        python3-bloom python3-rosdep \
        fakeroot debhelper dpkg-dev devscripts equivs lsb-release \
        ros-${ROS_DISTRO}-pcl-conversions ros-${ROS_DISTRO}-pcl-ros

# ---------------------------------------------------------------------------
# 3. rosdep 初始化 (尽早做, 利用层缓存)
#    - 网络不稳时 raw.githubusercontent 容易 SSL EOF, 加重试 + 镜像支持
# ---------------------------------------------------------------------------
RUN rosdep init 2>/dev/null || true && \
    if [ -n "${ROSDISTRO_INDEX_URL}" ]; then \
        export ROSDISTRO_INDEX_URL="${ROSDISTRO_INDEX_URL}"; \
        echo "Using rosdistro index: ${ROSDISTRO_INDEX_URL}"; \
    fi && \
    n=0; \
    until [ "$n" -ge 5 ]; do \
        rosdep update && break; \
        n=$((n+1)); \
        echo "==> rosdep update failed (attempt $n/5), retrying in $((n*5))s..."; \
        sleep $((n*5)); \
    done; \
    [ "$n" -lt 5 ] || { echo "ERROR: rosdep update failed after 5 attempts" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 4. 编译 + 安装 Livox-SDK2 到 staging 目录
#    - staging 目录 (${LIVOX_SDK2_STAGE}) 用于打包到 deb 暂存树
#    - 同时把 .so / 头文件拷到 /usr/local 让 driver 的 find_library 命中
#
#    分多个 RUN 便于失败定位; "ls staging" 步骤会打印 install 后的实际产物
# ---------------------------------------------------------------------------
RUN git clone --depth=1 --branch ${LIVOX_SDK2_REF} \
        https://github.com/Livox-SDK/Livox-SDK2.git /tmp/Livox-SDK2

# Livox-SDK2 在新编译器(>=gcc-13 / Noble)下有 transitive-include 退化问题:
#   - std::uint8_t / uint16_t 找不到 (新版 libstdc++ 不再隐式引入 <cstdint>)
#   - vendored spdlog/fmt 触发 -Wc++20-compat 等 noise
# 用 -include cstdint 全局补上头文件; -w/-Wno-error 把 vendor 代码先编过去.
RUN cmake -S /tmp/Livox-SDK2 -B /tmp/Livox-SDK2/build \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_CXX_EXTENSIONS=OFF \
        -DCMAKE_CXX_FLAGS="-include cstdint -include cstring -include cstdlib -w -Wno-error" \
        -DCMAKE_C_FLAGS="-include stdint.h -w -Wno-error" \
        -DCMAKE_INSTALL_PREFIX=${LIVOX_SDK2_STAGE} && \
    cmake --build /tmp/Livox-SDK2/build -j${BUILD_JOBS} && \
    cmake --install /tmp/Livox-SDK2/build

RUN echo "==> Livox-SDK2 staging contents:" && \
    find ${LIVOX_SDK2_STAGE} \( -type f -o -type l \) | sort

# 用 shopt -s nullglob 避免 *.so 没匹配到时 cp 失败时不知所措
RUN /bin/bash -c 'set -eo pipefail; \
    shopt -s nullglob; \
    sdk_libs=( ${LIVOX_SDK2_STAGE}/lib/liblivox_lidar_sdk_shared.so* ); \
    if [ ${#sdk_libs[@]} -eq 0 ]; then \
        echo "ERROR: no liblivox_lidar_sdk_shared.so* found under ${LIVOX_SDK2_STAGE}/lib"; \
        ls -la ${LIVOX_SDK2_STAGE}/lib || true; \
        exit 1; \
    fi; \
    cp -P "${sdk_libs[@]}" /usr/local/lib/ && \
    cp -r ${LIVOX_SDK2_STAGE}/include/. /usr/local/include/ && \
    ldconfig && \
    rm -rf /tmp/Livox-SDK2'

# ---------------------------------------------------------------------------
# 5. 拷入源码 + 重现 build.sh ROS2 路径的两个动作:
#      - package_ROS2.xml -> package.xml
#      - launch_ROS2/     -> launch/
#    (依赖根目录 .dockerignore 过滤 build/ install/ log/ .git 等)
# ---------------------------------------------------------------------------
WORKDIR ${ROS_WS}/src/livox_ros_driver2
COPY . .
RUN cp -f package_ROS2.xml package.xml && \
    cp -rf launch_ROS2/ launch/

# ---------------------------------------------------------------------------
# 6. bloom-generate, 注入 rules, 把 Livox-SDK2 搭进 .deb
#
#    QEMU 注意:
#      - 并发用 ARG BUILD_JOBS, 不要 $(nproc) (QEMU 下返回宿主核数, 易 OOM)
#
#    Rules 改动:
#      - dh_auto_configure: 透传 -DROS_EDITION=ROS2 -DDISTRO_ROS=<distro>
#      - dh_auto_install:   把 staging 的 SDK .so + 头文件复制到 deb 暂存树
#      - dh_shlibdeps:      新增 -l 路径 + 忽略缺 shlibs 的私有 .so
#      - dh_usrlocal:       跳过 /usr/local 检查
# ---------------------------------------------------------------------------
RUN <<'BASH'
#!/usr/bin/env bash
set -eo pipefail
source /opt/ros/${ROS_DISTRO}/setup.bash
set -u

# 从 base 镜像取 ubuntu codename (humble->jammy, jazzy->noble)
. /etc/os-release
OS_VERSION="${VERSION_CODENAME}"
echo "==> os-version=${OS_VERSION}, ros-distro=${ROS_DISTRO}"

bloom-generate rosdebian \
    --os-name ubuntu \
    --os-version "${OS_VERSION}" \
    --ros-distro "${ROS_DISTRO}"

PKG=$(dpkg-parsechangelog -ldebian/changelog -SSource)
echo "==> bloom binary package name: ${PKG}"

# 6.0 把 *-dev 从 runtime Depends 中剔除 (留在 Build-Depends).
#     bloom 把 <depend>apr</depend> 翻译为 libapr1-dev 并塞进 Depends, 但运行时
#     不需要头文件; dh_shlibdeps 会通过 ${shlibs:Depends} 自动补上真正的 .so 包
#     (libapr1 / libapr1t64 / libaprutil1 / libaprutil1t64, 依发行版而定).
python3 - <<'PY'
import re
path = "debian/control"
with open(path) as f:
    text = f.read()

# 把多行的 Depends: 字段折行后处理, 写回单行 (debian/control 允许)
def fix(match):
    body = match.group(1)
    body = re.sub(r"\n ", " ", body)
    items = [x.strip() for x in body.split(",") if x.strip()]
    drop = ("libapr1-dev", "libaprutil1-dev")
    items = [x for x in items if not any(x == d or x.startswith(d + " ") for d in drop)]
    return "Depends: " + ", ".join(items) + "\n"

text = re.sub(
    r"^Depends:[ \t]*((?:[^\n]*(?:\n [^\n]+)*))\n",
    fix, text, flags=re.MULTILINE,
)
with open(path, "w") as f:
    f.write(text)
PY

echo '===== debian/control (Depends after fix) ====='
grep -E '^(Depends|Build-Depends):' debian/control || true
echo '=============================================='

# 6.1 透传 cmake 参数 (build.sh 的 colcon --cmake-args 等价物)
sed -i "s|dh_auto_configure --|dh_auto_configure -- -DROS_EDITION=ROS2 -DDISTRO_ROS=${ROS_DISTRO}|" debian/rules

# 6.2 追加 override; 用占位符避开 heredoc 的 $ 展开, 后面 sed 替换
cat >> debian/rules <<'RULES'

override_dh_usrlocal:
	true

override_dh_auto_install:
	dh_auto_install
	install -d debian/__PKG__/opt/ros/__DISTRO__/lib
	cp -P __SDK_STAGE__/lib/liblivox_lidar_sdk_shared.so* debian/__PKG__/opt/ros/__DISTRO__/lib/
	install -d debian/__PKG__/opt/ros/__DISTRO__/include
	cp -r __SDK_STAGE__/include/. debian/__PKG__/opt/ros/__DISTRO__/include/

override_dh_shlibdeps:
	dh_shlibdeps -l$(CURDIR)/debian/__PKG__/opt/ros/__DISTRO__/lib -- --ignore-missing-info
RULES

sed -i \
    -e "s|__PKG__|${PKG}|g" \
    -e "s|__DISTRO__|${ROS_DISTRO}|g" \
    -e "s|__SDK_STAGE__|${LIVOX_SDK2_STAGE}|g" \
    debian/rules

echo '===== debian/rules ====='
cat debian/rules
echo '========================'

export DEB_BUILD_OPTIONS=parallel=${BUILD_JOBS}
echo "Building with DEB_BUILD_OPTIONS=${DEB_BUILD_OPTIONS}"
fakeroot debian/rules binary
BASH

# ---------------------------------------------------------------------------
# 7. 收集 .deb 到 /output, 方便 export 阶段导出
# ---------------------------------------------------------------------------
RUN mkdir -p /output && \
    cp ${ROS_WS}/src/*.deb /output/ && \
    ls -lh /output/

# ---------------------------------------------------------------------------
# Stage 2: export — 仅含 .deb, 配 buildx --output type=local 直接落盘
# ---------------------------------------------------------------------------
FROM scratch AS export
COPY --from=builder /output/*.deb /
