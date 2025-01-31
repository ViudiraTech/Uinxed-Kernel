# -------------------------------------------------
#
#
#		Kconfig.py
# 		预编译选项配置器
#
#		2025/01/31 By Rainy101112
#		基于 GPL-3.0 开源协议
#		Copyright © 2020 ViudiraTech，保留最终解释权。
#
#
# -------------------------------------------------

import os
import re
import sys
import textwrap
from datetime import datetime
from kconfiglib import Kconfig, split_expr, expr_value, expr_str, BOOL, \
                       TRISTATE, TRI_TO_STR, AND, OR

def generate_config(kconfig_file, config_in, header_out):
	kconf = Kconfig(kconfig_file, warn_to_stderr = False, suppress_traceback = True)

	kconf.load_config(config_in)
	kconf.write_autoconf(header_out)

	with open(header_out, 'r+') as header_file:
		content = header_file.read()
		header_file.truncate(0)
		header_file.seek(0)

		content = content.replace("#define CONFIG_", "#define ")

		current_date = datetime.now()
		year = current_date.year
		month = current_date.month
		day = current_date.day
		formatted_date = f"{year}/{month}/{day}"

		header_file.write("/*\n")
		header_file.write(" *\n")
		header_file.write(" *		autoconfig.h\n")
		header_file.write(" *		预编译选项\n")
		header_file.write(" *\n")
		header_file.write(f" *		{formatted_date} By Kconfig.py\n")
		header_file.write(" *		基于 GPL-3.0 开源协议\n")
		header_file.write(" *		Copyright © 2020 ViudiraTech，保留最终解释权。\n")
		header_file.write(" *\n")
		header_file.write(" */\n\n")

		header_file.write("#ifndef INCLUDE_AUTOCONFIG_H_\n")
		header_file.write("#define INCLUDE_AUTOCONFIG_H_\n\n")

		header_file.write(content)

		header_file.write("\n")
		header_file.write("#endif // INCLUDE_AUTOCONFIG_H_\n")

def main():
	kconfig_file = 'Kconfig'
	config_in = '.config'
	header_out = 'include/autoconfig.h'

	if not os.path.isfile(kconfig_file):
		sys.exit()
	if not os.path.isfile(config_in):
		sys.exit()

	generate_config(kconfig_file, config_in, header_out)

if __name__ == "__main__":
	main()
