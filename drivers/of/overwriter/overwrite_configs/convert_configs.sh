#!/bin/sh
set -eu

out_file=$1
source_root=$2
config_dir="${source_root}/overwrite_configs/common"

{
	echo '/* SPDX-License-Identifier: GPL-2.0 */'
	echo '#include <linux/kernel.h>'
	echo '#include "overwrite_configs.h"'
	echo
	echo 'static const char *const common_values[] = {'
	for config_file in "${config_dir}"/*.conf; do
		[ -f "${config_file}" ] || continue
		while IFS= read -r config_line || [ -n "${config_line}" ]; do
			case "${config_line}" in
			''|'#'*) continue ;;
			esac
			escaped_line=$(printf '%s' "${config_line}" |
				sed 's/\\/\\\\/g; s/"/\\"/g')
			printf '\t"%s",\n' "${escaped_line}"
		done < "${config_file}"
	done
	echo '};'
	echo
	echo 'const struct overwrite_config_group overwrite_config_groups[] = {'
	printf '\t{ .prefix = "common", .values = common_values,\n'
	printf '\t  .count = ARRAY_SIZE(common_values) },\n'
	echo '};'
	echo
	echo 'const unsigned int overwrite_config_group_count ='
	printf '\tARRAY_SIZE(overwrite_config_groups);\n'
} > "${out_file}"
