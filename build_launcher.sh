#!/bin/sh
# Builds only the co-op launcher on Linux (uses the repo's linux preset,
# which links against the system SDL2 like the rest of the project).
set -e

cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --target fallout2coop_launcher

echo
echo "Launcher built: out/build/linux-x64-release/tools/launcher/fallout2coop_launcher"
