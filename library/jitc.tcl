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
		#puts stderr "in build dir: [file exists [file join $dir ../generic/jitc.c]], dir is ($dir)"
		if {[file exists [file join $dir ../generic/jitc.c]]} {
			set packagedir	[file dirname $dir]
			set tccpath		[file join $packagedir local/lib/tcc]
			set re2cpath	[file join $packagedir local/bin/re2c]
			set packccpath	[file join $packagedir local/bin/packcc]
			set lemonpath	[file join $packagedir local/bin/lemon]

			lappend includepath	[file join $packagedir generic]
			lappend includepath	[file join $packagedir local/lib/tcc/include]
			lappend librarypath	[file join $packagedir local/lib/tcc]
			set jitclib $dir
		} else {
			set packagedir	$dir
			set tccpath		$packagedir
			set re2cpath	[file join $packagedir re2c]
			set packccpath	[file join $packagedir packcc]
			set lemonpath	[file join $packagedir lemon]

			lappend includepath	[file join $packagedir include]
			lappend librarypath	$packagedir
			set jitclib $packagedir
		}

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

		load [file join $packagedir libjitc0.4.so] jitc
		tcl::tm::path add $jitclib
	} [namespace current]]

	proc _build_compile_error {code errorstr args} { #<<<
		package require jitclib::parse_tcc_errors
		switch -exact -- [llength $args] {
			0	{}
			2	{lassign $args previous_errmsg previous_options}
			default	{error "Wrong args"}
		}
		set lines	[split $code \n]
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
		variable packagedir
		variable re2cpath

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		exec echo $source | $re2cpath - {*}$options
	}

	#>>>
	proc packcc args { #<<<
		variable packagedir
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
		variable packagedir
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
