#!/bin/zsh
set -eu

project_dir=${0:A:h:h}
build_dir=${1:-"$project_dir/build/release"}
publish_dir=${2:-"$project_dir/build/publish"}
stage=$(mktemp -d -t md30-pkg)
trap 'rmdir "$stage" 2>/dev/null || true' EXIT

cmake --install "$build_dir" --prefix "$publish_dir"
mkdir -p "$stage/root/Library/Audio/Plug-Ins/HAL" "$stage/root/Library/LaunchAgents" "$stage/root/Applications"
ditto --norsrc --noextattr "$publish_dir/MD30.driver" "$stage/root/Library/Audio/Plug-Ins/HAL/MD30.driver"
ditto --norsrc --noextattr "$publish_dir/MD30 Panel.app" "$stage/root/Applications/MD30 Panel.app"
/usr/bin/install -m 0644 "$publish_dir/launchd/local.md30.autolaunch.plist" "$stage/root/Library/LaunchAgents/"
version=$(plutil -extract CFBundleVersion raw "$publish_dir/MD30.driver/Contents/Info.plist")
COPYFILE_DISABLE=1 pkgbuild --root "$stage/root" --install-location / --ownership recommended \
    --scripts "$project_dir/scripts/installer" \
    --identifier local.md30.installer --version "$version" "$publish_dir/MD30-macOS.pkg"
python3 - "$stage/root" <<'PY'
import shutil
import sys
shutil.rmtree(sys.argv[1])
PY
