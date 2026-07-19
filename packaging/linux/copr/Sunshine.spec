%global build_timestamp %(date +"%Y%m%d")

# use sed to replace these values
%global build_version 0
%global branch 0
%global commit 0
%global sunshine_python_version 3.14

%undefine _hardened_build

# When enabled (default), download the NVIDIA CUDA runfile if no system nvcc is found.
# Build with --without bundled_cuda to skip the runfile (use system nvcc or build without CUDA).
%bcond_without bundled_cuda 1

# Define _metainfodir for OpenSUSE if not already defined
%if 0%{?suse_version}
%if !0%{?_metainfodir:1}
%global _metainfodir %{_datadir}/metainfo
%endif
%endif

Name: Sunshine
Version: %{build_version}
Release: 1%{?dist}
Summary: Self-hosted game stream host for Moonlight
License: GPL-3.0-only
URL: https://github.com/LizardByte/Sunshine
Source0: tarball.tar.gz

# Common BuildRequires
BuildRequires: cmake >= 3.25.0
BuildRequires: desktop-file-utils
BuildRequires: git
BuildRequires: libcap-devel
BuildRequires: libcurl-devel
BuildRequires: libdrm-devel
BuildRequires: libevdev-devel
BuildRequires: libnotify-devel >= 0.8.0
BuildRequires: libva-devel
BuildRequires: libX11-devel
BuildRequires: libxcb-devel
BuildRequires: libXcursor-devel
BuildRequires: libXfixes-devel
BuildRequires: libXi-devel
BuildRequires: libXinerama-devel
BuildRequires: libXrandr-devel
BuildRequires: libXtst-devel
BuildRequires: pipewire-devel
%if 0%{?fedora}
BuildRequires: openssl-devel
%endif
%if 0%{?suse_version}
BuildRequires: libopenssl-3-devel
%endif
BuildRequires: rpm-build
BuildRequires: systemd-rpm-macros
BuildRequires: wget
BuildRequires: which

%if 0%{?fedora}
# Fedora-specific BuildRequires
BuildRequires: appstream
# BuildRequires: boost-devel >= 1.86.0
BuildRequires: glslc
BuildRequires: libappstream-glib
BuildRequires: vulkan-loader-devel
%if 0%{fedora} > 43
# needed for npm from nvm
BuildRequires: libatomic
%endif
BuildRequires: libgudev
BuildRequires: mesa-libGL-devel
BuildRequires: mesa-libgbm-devel
BuildRequires: miniupnpc-devel
%if 0%{?fedora} < 44
BuildRequires: nodejs-npm
%endif
BuildRequires: numactl-devel
BuildRequires: opus-devel
BuildRequires: pulseaudio-libs-devel
BuildRequires: qt6-qtbase-devel
BuildRequires: qt6-qtsvg-devel
BuildRequires: systemd-udev
BuildRequires: uv
%{?sysusers_requires_compat}
# for unit tests
BuildRequires: xorg-x11-server-Xvfb
%endif

%if 0%{?suse_version}
# OpenSUSE-specific BuildRequires
BuildRequires: AppStream
BuildRequires: appstream-glib
BuildRequires: libgudev-1_0-devel
BuildRequires: Mesa-libGL-devel
BuildRequires: libgbm-devel
BuildRequires: libminiupnpc-devel
BuildRequires: libnuma-devel
BuildRequires: libopus-devel
BuildRequires: libpulse-devel
%if 0%{?sle_version}
# OpenSUSE Leap: npm/python package names differ from Tumbleweed
BuildRequires: npm
BuildRequires: python311
BuildRequires: python311-Jinja2
%else
# OpenSUSE Tumbleweed (suse_version may equal Leap; sle_version is unset)
BuildRequires: libxml2-16
BuildRequires: npm-default
BuildRequires: python313
BuildRequires: python313-Jinja2
%endif
%if !0%{?sle_version}
BuildRequires: shaderc
%endif
BuildRequires: udev
%if !0%{?sle_version}
BuildRequires: vulkan-devel
%endif
# for unit tests
BuildRequires: xvfb-run
%endif

# Conditional BuildRequires for cuda-gcc based on distribution version
%if 0%{?fedora}
%if 0%{?fedora} <= 41
BuildRequires: gcc13
BuildRequires: gcc13-c++
%global gcc_version 13
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 42 && 0%{?fedora} <= 43
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 44
BuildRequires: gcc15
BuildRequires: gcc15-c++
%global gcc_version 15
%global cuda_version 13.1.1
%global cuda_build 590.48.01
%endif
%endif

%if 0%{?suse_version}
%if 0%{?sle_version}
# OpenSUSE Leap 15.x (Qt6 not in standard repos, use Qt5)
BuildRequires: gcc14
BuildRequires: gcc14-c++
BuildRequires: libqt5-qtbase-devel
BuildRequires: libqt5-qtsvg-devel
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%else
# OpenSUSE Tumbleweed (suse_version may equal Leap; sle_version is unset)
BuildRequires: gcc15
BuildRequires: gcc15-c++
BuildRequires: qt6-base-devel
BuildRequires: qt6-svg-devel
%global gcc_version 15
%global cuda_version 13.1.1
%global cuda_build 590.48.01
%endif
%endif

%global cuda_dir %{_builddir}/cuda

# Common runtime requirements
Requires: which >= 2.21

%if 0%{?fedora}
Requires: libnotify >= 0.8.0
Requires: miniupnpc >= 2.2.4
%endif

%if 0%{?fedora}
# Fedora runtime requirements
Requires: libcap >= 2.22
Requires: libcurl >= 7.0
Requires: libdrm > 2.4.97
Requires: libevdev >= 1.5.6
Requires: libopusenc >= 0.2.1
Requires: libva >= 2.14.0
Requires: libwayland-client >= 1.20.0
Requires: libX11 >= 1.7.3.1
Requires: numactl-libs >= 2.0.14
Requires: openssl >= 3.0.2
Requires: pulseaudio-libs >= 10.0
Requires: qt6-qtbase
Requires: qt6-qtsvg
Requires: vulkan-loader
%endif

%if 0%{?suse_version}
# Shared library dependencies are added automatically on openSUSE via shlib policy.
%endif

%description
Self-hosted game stream host for Moonlight.

%prep
# extract tarball to current directory
mkdir -p %{_builddir}/Sunshine
tar -xzf %{SOURCE0} -C %{_builddir}/Sunshine

# list directory
ls -a %{_builddir}/Sunshine

%build
# exit on error
set -e

# Detect the architecture and Fedora version
architecture=$(uname -m)

cuda_supported_architectures=("x86_64" "aarch64")

# prepare CMAKE args
cmake_args=(
  "-B=%{_builddir}/Sunshine/build"
  "-G=Unix Makefiles"
  "-S=."
  "-DBUILD_DOCS=OFF"
  "-DBUILD_WERROR=ON"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_PREFIX=%{_prefix}"
  "-DSUNSHINE_ASSETS_DIR=%{_datadir}/sunshine"
  "-DSUNSHINE_EXECUTABLE_PATH=%{_bindir}/sunshine"
  "-DSUNSHINE_ENABLE_DRM=ON"
  "-DSUNSHINE_ENABLE_KWIN=ON"
  "-DSUNSHINE_ENABLE_PORTAL=ON"
  "-DSUNSHINE_ENABLE_WAYLAND=ON"
  "-DSUNSHINE_ENABLE_X11=ON"
  "-DSUNSHINE_PUBLISHER_NAME=LizardByte"
  "-DSUNSHINE_PUBLISHER_WEBSITE=https://app.lizardbyte.dev"
  "-DSUNSHINE_PUBLISHER_ISSUE_URL=https://app.lizardbyte.dev/support"
)

%if 0%{?fedora}
# uv installs Python and glad's Python dependencies into .venv before CMake runs.
cmake_args+=("-DGLAD_SKIP_PIP_INSTALL=ON")
cmake_args+=("-DPython_EXECUTABLE=%{_builddir}/Sunshine/.venv/bin/python")
%endif

%if 0%{?suse_version}
%if 0%{?sle_version}
# Leap: use the Python interpreter that owns python311-Jinja2
cmake_args+=("-DGLAD_SKIP_PIP_INSTALL=ON")
cmake_args+=("-DPython_EXECUTABLE=/usr/bin/python3.11")
%else
# Tumbleweed: python3 is provided by python313
cmake_args+=("-DGLAD_SKIP_PIP_INSTALL=ON")
cmake_args+=("-DPython_EXECUTABLE=/usr/bin/python3.13")
%endif
%endif

export CC=gcc-%{gcc_version}
export CXX=g++-%{gcc_version}

function apply_cuda_patches() {
  local toolkit_root="$1"

  # we need to patch math_functions.h depending on the CUDA major version
  # see https://forums.developer.nvidia.com/t/error-exception-specification-is-incompatible-for-cospi-sinpi-cospif-sinpif-with-glibc-2-41/323591/3
  local cuda_major
  cuda_major=$(echo "%{cuda_version}" | cut -d. -f1)
  local patch_file=""
  if [ "${cuda_major}" -eq 12 ]; then
    # CUDA 12.x: the extern declarations lack noexcept(true); add it to match glibc 2.41.
    patch_file="cuda-12-math_functions.patch"
  elif [ "${cuda_major}" -eq 13 ]; then
    # CUDA 13.x: the extern declarations already have noexcept(true), but the __func__()
    # macro invocations at the bottom still lack it, causing a redeclaration conflict.
    patch_file="cuda-13-math_functions.patch"
  else
    echo "Warning: no math_functions.h patch available for CUDA ${cuda_major}.x, skipping."
  fi

  if [ -n "${patch_file}" ]; then
    echo "Applying CUDA patch: ${patch_file}"
    # -N/--forward ignores an already-applied patch (non-zero exit is OK then).
    patch -p2 \
      -N \
      --forward \
      --backup \
      --directory="${toolkit_root}" \
      --verbose \
      < "%{_builddir}/Sunshine/packaging/linux/patches/${architecture}/${patch_file}" \
      || echo "CUDA patch ${patch_file} already applied or skipped; continuing."
  fi
}

function install_cuda() {
  # check if we need to install cuda
  if [ -f "%{cuda_dir}/bin/nvcc" ]; then
    echo "cuda already installed"
    return
  fi

  local cuda_prefix="https://developer.download.nvidia.com/compute/cuda/"
  local cuda_suffix=""
  if [ "$architecture" == "aarch64" ]; then
    local cuda_suffix="_sbsa"
  fi

  local url="${cuda_prefix}%{cuda_version}/local_installers/cuda_%{cuda_version}_%{cuda_build}_linux${cuda_suffix}.run"
  echo "cuda url: ${url}"
  wget \
    "$url" \
    --progress=bar:force:noscroll \
    --retry-connrefused \
    --tries=3 \
    -q -O "%{_builddir}/cuda.run"
  chmod a+x "%{_builddir}/cuda.run"

  # openSUSE Tumbleweed provides libxml2.so.16; the NVIDIA cuda-installer still requires libxml2.so.2.
  if ! ldconfig -p 2>/dev/null | grep -q 'libxml2\.so\.2 '; then
    for libdir in /usr/lib64 /usr/lib /lib64 /lib; do
      if [ -e "${libdir}/libxml2.so.2" ]; then
        break
      fi
      for candidate in "${libdir}"/libxml2.so.*; do
        if [ -e "${candidate}" ]; then
          echo "Creating CUDA compatibility symlink: ${libdir}/libxml2.so.2 -> ${candidate}"
          ln -sfn "$(basename "${candidate}")" "${libdir}/libxml2.so.2"
          break 2
        fi
      done
    done
  fi

  "%{_builddir}/cuda.run" \
    --no-drm \
    --no-man-page \
    --no-opengl-libs \
    --override \
    --silent \
    --toolkit \
    --toolkitpath="%{cuda_dir}"
  rm "%{_builddir}/cuda.run"
}

function detect_nvcc_path() {
  local nvcc_path=""

  nvcc_path="$(command -v nvcc 2>/dev/null || true)"
  if [ -n "${nvcc_path}" ]; then
    echo "${nvcc_path}"
    return 0
  fi

  for candidate in \
    "/usr/local/cuda/bin/nvcc" \
    "%{cuda_dir}/bin/nvcc"; do
    if [ -x "${candidate}" ]; then
      echo "${candidate}"
      return 0
    fi
  done

  return 1
}

nvcc_path=""
if nvcc_path="$(detect_nvcc_path)"; then
  echo "Using CUDA compiler: ${nvcc_path}"
fi

%if %{with bundled_cuda}
if [ -z "${nvcc_path}" ]; then
  install_cuda
  nvcc_path="%{cuda_dir}/bin/nvcc"
fi
%else
echo "bundled_cuda disabled; skipping NVIDIA CUDA runfile download"
%endif

if [ -n "%{cuda_version}" ] && [[ " ${cuda_supported_architectures[@]} " =~ " ${architecture} " ]]; then
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=ON")
  if [ -n "${nvcc_path}" ] && [ -x "${nvcc_path}" ]; then
    cuda_toolkit_root="$(cd "$(dirname "${nvcc_path}")/.." && pwd)"
    apply_cuda_patches "${cuda_toolkit_root}"
    cmake_args+=("-DCMAKE_CUDA_COMPILER:PATH=${nvcc_path}")
    cmake_args+=("-DCMAKE_CUDA_HOST_COMPILER=gcc-%{gcc_version}")
  else
    echo "No CUDA compiler found; building without NVENC/NvFBC (CUDA_FAIL_ON_MISSING=OFF)"
    cmake_args+=("-DCUDA_FAIL_ON_MISSING=OFF")
  fi
else
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=OFF")
fi

# Install and setup NVM for Fedora 44+
%if 0%{?fedora} > 43
echo "Installing NVM for Fedora 44+..."
export HOME=${HOME:-/builddir}
export NVM_DIR="$HOME/.nvm"

# Install NVM
if [ ! -d "$NVM_DIR" ]; then
  wget -qO- https://raw.githubusercontent.com/nvm-sh/nvm/master/install.sh | bash
fi

# Load NVM
export NVM_DIR="$HOME/.nvm"
[ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"

# Install and use Node.js
nvm install node
nvm use node

echo "Node.js version: $(node --version)"
echo "npm version: $(npm --version)"
echo "npm location: $(which npm)"
echo "node location: $(which node)"

# Add npm and node path to cmake args
NPM_PATH=$(which npm)
NODE_PATH=$(which node)
cmake_args+=("-DNPM=${NPM_PATH}")

# Add node bin directory to PATH for make
export PATH="$(dirname ${NODE_PATH}):${PATH}"
%endif

# setup the version
export BRANCH=%{branch}
export BUILD_VERSION=v%{build_version}
export COMMIT=%{commit}

# Disable Vulkan on openSUSE Leap (shaderc/glslang not in official repos)
%if 0%{?sle_version}
cmake_args+=("-DSUNSHINE_ENABLE_VULKAN=OFF")
%endif

%if 0%{?suse_version}
%if !0%{?sle_version}
# build-deps is excluded from packaging tarballs; use distro Vulkan headers on Tumbleweed
cmake_args+=("-DSUNSHINE_SYSTEM_VULKAN_HEADERS=ON")
%endif
%endif

# cmake
cd %{_builddir}/Sunshine
%if 0%{?fedora}
uv python install %{sunshine_python_version}
uv sync \
  --locked \
  --only-group glad \
  --python %{sunshine_python_version} \
  --no-install-project
%endif
echo "cmake args:"
echo "${cmake_args[@]}"
cmake "${cmake_args[@]}"
make -j$(nproc) -C "%{_builddir}/Sunshine/build"

%check
# validate the metainfo file
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/*.metainfo.xml
appstream-util validate %{buildroot}%{_metainfodir}/*.metainfo.xml
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop

# run tests under Xvfb only; unset Wayland session vars so platf::init() enables X11 capture
cd %{_builddir}/Sunshine/build
env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE xvfb-run -a -s "-screen 0 1024x768x24" ./tests/test_sunshine

%install
# Load NVM for Fedora 44+ so npm is available during make install
%if 0%{?fedora} > 43
export HOME=${HOME:-/builddir}
export NVM_DIR="$HOME/.nvm"
[ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"
nvm use node

# Add node bin directory to PATH for make install
NODE_PATH=$(which node)
export PATH="$(dirname ${NODE_PATH}):${PATH}"

echo "Node.js version: $(node --version)"
echo "npm version: $(npm --version)"
%endif

cd %{_builddir}/Sunshine/build
%make_install

%if 0%{?suse_version}
# Reduce rpmlint noise for local/Tumbleweed builds (symbols are not needed in the package).
strip %{buildroot}%{_bindir}/sunshine
%endif

%post
# Note: this is copied from the postinst script

# Load uhid (DS5 emulation)
echo "Loading uhid kernel module for DS5 emulation."
modprobe uhid

# Check if we're in an rpm-ostree environment
if [ ! -x "$(command -v rpm-ostree)" ]; then
  echo "Not in an rpm-ostree environment, proceeding with post install steps."

  # Trigger udev rule reload for /dev/uinput and /dev/uhid
  path_to_udevadm=$(which udevadm)
  if [ -x "$path_to_udevadm" ]; then
    echo "Reloading udev rules."
    $path_to_udevadm control --reload-rules
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uinput
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uhid
    echo "Udev rules reloaded successfully."
  else
    echo "error: udevadm not found or not executable."
  fi
else
  echo "rpm-ostree environment detected, skipping post install steps. Restart to apply the changes."
fi

%files
# Executables
%caps(cap_sys_admin,cap_sys_nice+p) %{_bindir}/sunshine

# Systemd unit files for user services
%{_userunitdir}/*.service

# Udev rules
%{_udevrulesdir}/*-sunshine.rules

# Modules-load configuration
%{_modulesloaddir}/*-sunshine.conf

# Desktop entries
%{_datadir}/applications/*.desktop

# Icons
%{_datadir}/icons/hicolor/scalable/apps/*.Sunshine.svg

# Metainfo
%{_datadir}/metainfo/*.metainfo.xml

# Assets
%{_datadir}/sunshine/**

%changelog
* Sun Jul 12 2026 LizardByte <https://github.com/LizardByte/Sunshine> - %{version}-%{release}
- Update to %{version}