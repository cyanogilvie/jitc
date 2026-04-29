namespace eval ::jitc {
	namespace export *

	apply [list {} {
		variable includepath	{}
		variable librarypath	{}
		variable prefix
		variable packagedir
		variable re2cpath
		variable packccpath
		variable lemonpath
		variable tccpath

		set dir	[file normalize [file dirname [info script]]]

		set builddir_sentinel	[file join $dir __builddir]
		if {[file readable $builddir_sentinel]} {
			set fd			[open $builddir_sentinel r]
			set packagedir	[try {string trim [read $fd]} finally {close $fd}]
			set re2cpath	[file join $packagedir subprojects/re2c-4.3/re2c]
			set tccpath		[file join $packagedir subprojects/tinycc]
			#lappend librarypath [file join $packagedir subprojects/tinycc]
		} else {
			set packagedir	$dir
			set tccpath		$packagedir
			set re2cpath	[file join $packagedir re2c]
		}

		set srcdir_sentinel	[file join $dir __srcdir]
		if {[file readable $srcdir_sentinel]} {
			set fd			[open $srcdir_sentinel r]
			set srcdir		[try {string trim [read $fd]} finally {close $fd}]
			# These aren't staged in the builddir:
			lappend includepath [file join $srcdir subprojects/tinycc/include]
			lappend includepath	[file join $srcdir tools/chaos-pp]
			lappend includepath	[file join $srcdir tools/order-pp/inc]
		}

		lappend includepath	[file join $packagedir include]
		set packccpath	[file join $packagedir packcc]
		set lemonpath	[file join $packagedir lemon]
		set jitclib		[file join $packagedir jitclib]

		foreach path [list \
			[tcl::pkgconfig get includedir,runtime] \
			[tcl::pkgconfig get includedir,install] \
		] {
			if {$path ni $includepath} {
				lappend includepath $path
			}
		}

		foreach path [list \
			[tcl::pkgconfig get libdir,runtime] \
			[tcl::pkgconfig get libdir,install] \
		] {
			if {$path ni $librarypath} {
				lappend librarypath $path
			}
		}

		set prefix	[file join {*}[lrange [file split [info nameofexecutable]] 0 end-2]]

		set path	[file join $prefix include]
		if {$path ni $includepath} {
			lappend includepath $path
		}

		set path	[file join $prefix lib]
		if {$path ni $librarypath} {
			lappend librarypath $path
		}

		tcl::tm::path add [file join $packagedir tm]
	} [namespace current]]

	proc _build_compile_error {code errorstr args} { #<<<
		package require jitclib::parse_tcc_errors
		switch -exact -- [llength $args] {
			0	{}
			2	{lassign $args previous_errmsg previous_options}
			default	{error "Wrong args"}
		}
		set lines	[split $code \n]
		#puts stderr "errorstr: ($errorstr)"
		#puts stderr "lines\n[join [lmap l $lines {format {%4d: %s} [incr _lno] $l}] \n]"
		if {0 && ![info exists ::jitc::_parsing_errors]} {
			set ::jitc::_parsing_errors	1	;# prevent endless recursion if parse_tcc_errors fails to build
			try {
				set errors	[jitc::capply $::jitclib::parse_tcc_errors parse $errorstr]
			} finally {
				set ::jitc::_parsing_errors 0
			}
		} else {
			set errors	[lmap {- fn line lvl msg} [regexp -all -inline -line {^(.*?):([0-9]+): (error|warning): +(.*?)$} $errorstr] {
				list $lvl $fn $line $msg
			}]
			lappend errors	{*}[lmap {- fn lvl msg} [regexp -all -inline -line {^([^:]*): (error|warning): +(.*?)$} $errorstr] {
				list $lvl $fn {} $msg
			}]
		}
		#puts stderr "errorstr ($errorstr) -> errors ($errors)"
		set error_report	{}
		set sep				{}
		foreach error $errors {
			lassign $error lvl fn line msg
			if {$line ne {}} {
				if {$line == 0} {set line 1}
				append error_report	$sep [format "%s: In \"%s\", line %d: %s:\n%s" [string toupper $lvl] $fn $line $msg [lindex $lines $line-1]]
			} else {
				append error_report	$sep [format "%s: In \"%s\": %s" [string toupper $lvl] $fn $msg]
			}
			set sep	\n
		}
		list [list JITC COMPILE $errors $code] $error_report
	}

	#>>>

	proc packageinclude {} { #<<<
		variable packagedir
		set packagedir
	}

	#>>>
	proc re2c args { #<<<
		variable re2cpath

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		exec echo $source | $re2cpath - --input-encoding utf8 {*}$options
	}

	#>>>
	proc packcc args { #<<<
		variable packccpath
		error "Not implemented yet"

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		in_builddir {
			exec echo $source | $packccpath -l -o  {*}$options
		}
	}

	#>>>
	proc lemon args { #<<<
		variable lemonpath
		error "Not implemented yet"

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		in_builddir {
			exec echo $source | $lemonpath -q {*}$options
		}
	}

	#>>>
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
