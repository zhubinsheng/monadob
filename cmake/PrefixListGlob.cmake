# - For each given prefix in a list, glob using the prefix+pattern
#
#
# Original Author:
# 2009-2010 Rylie Pavlik <rylie@ryliepavlik.com>
# https://ryliepavlik.com/
# Iowa State University HCI Graduate Program/VRAC
#
# Copyright 2009-2010, Iowa State University
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
# SPDX-License-Identifier: BSL-1.0

if(__prefix_list_glob)
	return()
endif()
set(__prefix_list_glob YES)

function(prefix_list_glob var pattern)
	set(_out)
	set(_result)
	foreach(prefix ${ARGN})
		file(GLOB _globbed ${prefix}${pattern})
		if(_globbed)
			list(SORT _globbed)
			list(REVERSE _globbed)
			list(APPEND _out ${_globbed})
		endif()
	endforeach()
	foreach(_name ${_out})
		get_filename_component(_name "${_name}" ABSOLUTE)
		list(APPEND _result "${_name}")
	endforeach()

	set(${var} "${_result}" PARENT_SCOPE)
endfunction()
