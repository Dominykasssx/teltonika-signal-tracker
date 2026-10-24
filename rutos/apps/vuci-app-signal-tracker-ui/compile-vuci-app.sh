#!/bin/bash

SRC_DIR="./src"
DEST_DIR="./dest"
TEMP_DIR=".plugin/temp"
MENU_D_PATHS="./files/usr/share/vuci/menu.d/ ./files/usr/local/share/vuci/menu.d/"
VITE_CONFIG="vite.config.js"

die() { echo "ERROR: $*" >&2; exit 1; }

find_vue_file() {
	local app_name="$1"
	find "$SRC_DIR" -wholename "*src/views*${app_name}.vue" -print -quit
}

get_app_type() {
	local file_path="$1"
	local type_name="${file_path##*/src/views/}"
	echo "${type_name%/*}"
}

compute_md5_prefix() {
	md5sum "${TEMP_DIR}/plugin."* | cut -d ' ' -f1 | head -c 6
}

copy_built_assets() {
	local app_target="$1"

	if [[ -f "${TEMP_DIR}/plugin.js.gz" ]]; then
		cp "${TEMP_DIR}/plugin.js.gz" "${app_target}.js.gz"
	elif [[ -f "${TEMP_DIR}/plugin.mjs.gz" ]]; then
		cp "${TEMP_DIR}/plugin.mjs.gz" "${app_target}.js.gz"
	elif [[ -f "${TEMP_DIR}/plugin.js" ]]; then
		cp "${TEMP_DIR}/plugin.js" "${app_target}.js"
	else
		die "No JS build artifact found in ${TEMP_DIR}"
	fi

	if [[ -f "${TEMP_DIR}/style.css.gz" ]]; then
		cp "${TEMP_DIR}/style.css.gz" "${app_target}.css.gz"
	elif [[ -f "${TEMP_DIR}/style.css" ]]; then
		cp "${TEMP_DIR}/style.css" "${app_target}.css"
	fi
}

patch_menu_d() {
	local app_type="$1"
	local app_name="$2"
	local md5_sub="$3"
	for MENU_D_PATH in $MENU_D_PATHS; do
		local menu_file="${MENU_D_PATH}$(echo "$app_name" | tr '[:upper:]' '[:lower:]').json"
		if [[ -f "$menu_file" ]]; then
			local replacement="${app_type}/${app_name}_${md5_sub}"
			sed -i -E "s|(\"view\":\s*\")[^\"]*\"|\1${replacement}\"|" "$menu_file"
		fi
	done
}

build_plugin() {
	local app_name="$1"

	local file_path=$(find_vue_file "$app_name") || die "Vue file not found for: ${app_name}"
	app_name="${app_name##*/}"

	local app_type=$(get_app_type "$file_path")

	local env_file=".env.plugin${app_name}"
	{
		echo "VITE_PLUGIN_ENTRY=${file_path}"
		echo "VITE_PLUGIN_FILENAME=plugin"
	} > "$env_file"

	trap 'rm -f "$env_file"; rm -rf .plugin' EXIT

	vite build \
		--outDir "$TEMP_DIR" \
		--config "$VITE_CONFIG" \
		--mode "plugin${app_name}" \
		|| { cat package.json; die "vite build failed for ${app_name}"; }

	rm -f "$env_file"
	trap - EXIT

	local md5_sub=$(compute_md5_prefix)

	local app_target="${DEST_DIR}/${app_type}/${app_name}_${md5_sub}"
	mkdir -p "${DEST_DIR}/${app_type}"

	copy_built_assets "$app_target"
	patch_menu_d "$app_type" "$app_name" "$md5_sub"

	rm -rf .plugin
}

main() {
	mkdir -p "$DEST_DIR"
	rm -rf "${DEST_DIR:?}/"*

	local index_file=$(find "$SRC_DIR" -name "Index" -print -quit)

	if [[ -n "$index_file" ]]; then
		local index_dir="${index_file%/Index}"
		local rel_path="${index_dir##*/src/}"

		while IFS= read -r line || [[ -n "$line" ]]; do
			[[ -z "$line" ]] && continue
			build_plugin "$line"
		done < "${SRC_DIR}/${rel_path}/Index"
	else
		local vue_file=$(find "${SRC_DIR}/" -name "*.vue" -print -quit) \
			|| die "No .vue file found under ${SRC_DIR}"
		local name="${vue_file%.*}"
		build_plugin "${name##*/}"
	fi
}

main "$@"
