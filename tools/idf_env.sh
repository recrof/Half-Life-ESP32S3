# Source this to get idf.py and the toolchain: an ESP-IDF environment already set up (its
# export.sh sourced: IDF_PATH set) is used as it is, otherwise the ESP-IDF 5.5.5 installed by
# PlatformIO (framework-espidf)
if [ -z "$IDF_PATH" ] || ! command -v idf.py > /dev/null 2>&1; then
	export IDF_PATH="$(ls -d "$HOME"/.platformio/packages/framework-espidf@src-* 2>/dev/null | head -1)"
	# PlatformIO's ESP-IDF virtualenv lacks esptool: take it from PlatformIO's own virtualenv
	export IDF_PYTHON_ENV_PATH="$(ls -d "$HOME"/.platformio/penv/.espidf-5.5* 2>/dev/null | head -1)"
	export PYTHONPATH="$HOME/.platformio/tools/tool-esptoolpy:$(ls -d "$HOME"/.platformio/penv/lib/python3*/site-packages 2>/dev/null | head -1)"
	export PATH="$HOME/.platformio/tools/toolchain-xtensa-esp-elf/bin:$HOME/.platformio/tools/tool-cmake/bin:$HOME/.platformio/tools/tool-ninja:$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$PATH"
	export ESP_ROM_ELF_DIR="$HOME/.platformio/tools/tool-esp-rom-elfs/"
fi
